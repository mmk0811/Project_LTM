#include <libgen.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utils.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "color.h"
#include "command.h"
#include "common.h"
#include "connect.h"
#include "log.h"
#include "message.h"
#include "validate.h"

int handle_download(int data_sock, int sock_control, char *arg) {
    Message msg;
    recv_message(sock_control, &msg);
    int is_file = msg.type == MSG_TYPE_DOWNLOAD_FILE ? 1 : 0;
    std::string str_log_message = is_file ? "Downloading file " : "Downloading folder ";
    std::string str_arg(arg);
    str_log_message += str_arg;
    log_message('i', str_log_message.c_str());

    recv_message(sock_control, &msg);
    if (msg.type == MSG_TYPE_ERROR) {
        printf(ANSI_COLOR_RED "%s\n" ANSI_RESET, msg.payload);
        std::string str_payload(msg.payload);
        log_message('e', str_payload.c_str());
        return -1;
    }

    if (!is_file)
        strcat(arg, ".zip");

    char *home = getenv("HOME");
    char *path = (char *) malloc(SIZE);
    strcpy(path, home);
    strcat(path, "/Downloads/");

    char *last_past = basename(arg);
    strcat(path, last_past);
    std::string str_path(path);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        perror("ERROR opening file: ");
        str_log_message = "ERROR opening file: ";
        str_log_message += str_path;
        log_message('e', str_log_message.c_str());
        return -1;
    }
    bool download_success = true;
    while (1) {
        recv_message(data_sock, &msg);
        if (msg.type == MSG_TYPE_DOWNLOAD) {
            fwrite(msg.payload, 1, msg.length, fp);
        } else if (msg.type == MSG_TYPE_ERROR) {
            printf(ANSI_COLOR_YELLOW "%s\n" ANSI_RESET, msg.payload);
            log_message('e', msg.payload);
            download_success = false;
            break;
        } else if (msg.type == MSG_TYPE_OK) {
            break;
        }
    }
    if (!download_success) {
        remove(path);
        log_message('e', "Download file or folder fail!");
        printf(ANSI_COLOR_RED "Download file or folder fail!\n" ANSI_RESET);
    } else {
        printf("File downloaded to %s\n", path);
        str_log_message = "File downloaded to ";
        str_log_message += str_path;
        log_message('i', str_log_message.c_str());
        free(path);
        fclose(fp);
    }
    return 0;
}

int handle_pipe_download(int sockfd, std::string files) {
    std::vector<std::string> tokens = split(files, '\n');

    printf("Downloading %d files...\n", (int) tokens.size());
    std::string str_log_message = "Downloading ";
    str_log_message += std::to_string(tokens.size());
    str_log_message += " files";
    log_message('i', str_log_message.c_str());
    for (const auto &l : tokens) {
        handle_download(sockfd, sockfd, (char *) l.c_str());
    }
    return 0;
}

void server_download(int sock_control, int sock_data, char *dir) {
    char compress_folder[SIZE];
    char *share_folder_path = (char *) malloc(SIZE);
    int share_mode = is_current_share_folder(dir, share_folder_path);

    if (share_mode == -1) {
        char error_msg[] = "You should not download files from a share folder";
        server_log('w', "Attempt to download from share folder without permission");
        send_message(sock_control, create_message(MSG_TYPE_ERROR, error_msg));
        free(share_folder_path);
        return;
    } else if (share_mode == 0) {
        char error_msg[] = "You don't have permission to access this folder!";
        server_log('e', "Attempt to download from folder without permission");
        send_message(sock_control, create_message(MSG_TYPE_ERROR, error_msg));
        free(share_folder_path);
        return;
    } else if (share_mode == 2) {
        send_message(sock_control, create_status_message(MSG_TYPE_OK, NO));
    } else {
        send_message(sock_control, create_status_message(MSG_TYPE_OK, NO));
    }

    if (is_folder(dir)) {
        // Need to compress folder before sending
        strcpy(compress_folder, dir);
        strcat(compress_folder, ".zip");
        zip(dir, compress_folder);
        send_message(sock_control, create_status_message(MSG_TYPE_DOWNLOAD_FOLDER, NO));
        strcpy(dir, compress_folder);
    } else {
        send_message(sock_control, create_status_message(MSG_TYPE_DOWNLOAD_FILE, NO));
    }

    std::string str_dir(dir);
    FILE *fp = fopen(dir, "r");

    if (!fp) {
        send_message(sock_control, create_status_message(MSG_TYPE_ERROR, FILE_NOT_FOUND));
        std::string str_log_message = str_dir + " not found";
        server_log('e', str_log_message.c_str());
    } else {
        send_message(sock_control, create_status_message(MSG_TYPE_OK, NO));
        std::string str_log_message = "Sending file " + str_dir;
        server_log('i', str_log_message.c_str());
        size_t byte_read;

        Message data;
        bool download_success = true;
        do {
            byte_read = fread(data.payload, 1, PAYLOAD_SIZE, fp);
            data.type = MSG_TYPE_DOWNLOAD;
            data.length = byte_read;
            if (send_message(sock_data, data) < 0) {
                server_log('e', "ERROR sending file to client, payload message too large, maybe caused by encrypt!");
                send_message(sock_data, create_status_message(MSG_TYPE_ERROR, STATUS_MESSAGE_TOO_LARGE));
                download_success = false;
                break;
            }
            memset(data.payload, 0, SIZE);
        } while (byte_read > 0);

        send_message(sock_data, create_status_message(MSG_TYPE_OK, NO));
        fclose(fp);

        str_log_message = download_success ? "File " + str_dir + " sent" : "File " + str_dir + " not sent";
        if (download_success)
            server_log('i', str_log_message.c_str());
        else
            server_log('e', str_log_message.c_str());
        if (is_folder(dir)) {
            remove(compress_folder);
        }
    }
    free(share_folder_path);
}
void server_pipe_download(int sockfd, char *output) {
    std::vector<std::string> tokens = split(output, '\n');

    std::string str_log_message = "Downloading ";
    str_log_message += std::to_string(tokens.size());
    str_log_message += " files";
    server_log('i', str_log_message.c_str());
    // Print the lines or process them as needed
    for (const auto &l : tokens) {
        server_download(sockfd, sockfd, (char *) l.c_str());
    }
    str_log_message = "Downloaded ";
    str_log_message += std::to_string(tokens.size());
    str_log_message += " files";
    server_log('i', str_log_message.c_str());
}