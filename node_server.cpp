#include "node_server.hpp"
#include "tcp_helpers.hpp"
#include "constants.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <iostream>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>

NodeServer::NodeServer(int node_id)
    : node_id(node_id), listen_fd(-1), local_storage(node_id) {}

NodeServer::~NodeServer() {
    if (listen_fd >= 0) {
        close(listen_fd);
    }
}

// Spawns a background thread that runs the blocking accept loop.
void NodeServer::start() {
    background_thread = std::thread(&NodeServer::run, this);
    background_thread.detach();
}

// Sets up the TCP listener on NODE_BASE_PORT + node_id, then accepts and
// handles one connection at a time in a loop.
void NodeServer::run() {
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("node_server socket");
        return;
    }

    // Allow the port to be reused immediately after the server restarts
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in node_address;
    memset(&node_address, 0, sizeof(node_address));
    node_address.sin_family      = AF_INET;
    node_address.sin_addr.s_addr = INADDR_ANY;
    node_address.sin_port        = htons(NODE_BASE_PORT + node_id);

    if (bind(listen_fd, (struct sockaddr *)&node_address, sizeof(node_address)) < 0) {
        perror("node_server bind");
        close(listen_fd);
        return;
    }

    if (listen(listen_fd, 10) < 0) {
        perror("node_server listen");
        close(listen_fd);
        return;
    }

    std::cout << "[node " << node_id << "] listening on port "
              << (NODE_BASE_PORT + node_id) << "\n";
    std::cout.flush();

    while (true) {
        int connection_fd = accept(listen_fd, nullptr, nullptr);
        if (connection_fd < 0) {
            perror("node_server accept");
            break;
        }
        handle_connection(connection_fd);
        close(connection_fd);
    }
}

// Reads the command line from the connection and dispatches to the right handler.
// Commands follow the format: "NODE_STORE foo.txt 1024", "NODE_RETRIEVE foo.txt", etc.
void NodeServer::handle_connection(int connection_fd) {
    std::string command_line;
    if (recv_line(connection_fd, command_line) != 0) {
        return;
    }

    std::istringstream command_stream(command_line);
    std::string command;
    command_stream >> command;

    if (command == "NODE_STORE") {
        std::string filename;
        size_t filesize;
        if (!(command_stream >> filename >> filesize)) {
            std::string error_response = "ERROR invalid NODE_STORE syntax\n";
            send_all(connection_fd, error_response.c_str(), error_response.size());
            return;
        }
        handle_store(connection_fd, filename, filesize);

    } else if (command == "NODE_RETRIEVE") {
        std::string filename;
        if (!(command_stream >> filename)) {
            std::string error_response = "ERROR invalid NODE_RETRIEVE syntax\n";
            send_all(connection_fd, error_response.c_str(), error_response.size());
            return;
        }
        handle_retrieve(connection_fd, filename);

    } else if (command == "NODE_DELETE") {
        std::string filename;
        if (!(command_stream >> filename)) {
            std::string error_response = "ERROR invalid NODE_DELETE syntax\n";
            send_all(connection_fd, error_response.c_str(), error_response.size());
            return;
        }
        handle_delete(connection_fd, filename);

    } else if (command == "NODE_LIST") {
        handle_list(connection_fd);

    } else if (command == "NODE_STATUS") {
        handle_status(connection_fd);

    } else {
        std::string error_response = "ERROR unknown command\n";
        send_all(connection_fd, error_response.c_str(), error_response.size());
    }
}

// Receives the file bytes from the connection, writes them into a pipe, and
// passes the read end of the pipe to NodeInternal which reads from it to store
// the file. A pipe is needed because NodeInternal expects a file descriptor,
// not a buffer. We use a thread to write into the pipe while NodeInternal
// reads from it simultaneously to avoid a deadlock.
void NodeServer::handle_store(int connection_fd, const std::string &filename, size_t filesize) {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        perror("node_server pipe");
        std::string error_response = "ERROR failed to create internal pipe\n";
        send_all(connection_fd, error_response.c_str(), error_response.size());
        return;
    }

    // Spawn a thread to stream bytes from the connection into the write end
    // of the pipe while NodeInternal reads from the read end simultaneously.
    std::thread pipe_writer([connection_fd, filesize, write_end_fd = pipe_fds[1]]() {
        char chunk[BUFFER_SIZE];
        size_t bytes_remaining = filesize;

        while (bytes_remaining > 0) {
            size_t bytes_to_read;
            if (bytes_remaining < sizeof(chunk)) {
                bytes_to_read = bytes_remaining;
            } else {
                bytes_to_read = sizeof(chunk);
            }

            ssize_t bytes_received = read(connection_fd, chunk, bytes_to_read);
            if (bytes_received <= 0) {
                break;
            }

            size_t bytes_written = 0;
            while (bytes_written < (size_t)bytes_received) {
                ssize_t result = write(write_end_fd, chunk + bytes_written,
                                       (size_t)bytes_received - bytes_written);
                if (result <= 0) {
                    goto done;
                }
                bytes_written += (size_t)result;
            }
            bytes_remaining -= (size_t)bytes_received;
        }
        done:
        close(write_end_fd);
    });
    pipe_writer.detach();

    int store_result;
    {
        std::lock_guard<std::mutex> lock(storage_mutex);
        if (local_storage.contains_file(filename.c_str()) == 1) {
            store_result = local_storage.replace_file(filename.c_str(), pipe_fds[0]);
        } else {
            store_result = local_storage.create_file(filename.c_str(), pipe_fds[0]);
        }
    }
    close(pipe_fds[0]);

    if (store_result != 0) {
        std::string error_response = "ERROR failed to store file\n";
        send_all(connection_fd, error_response.c_str(), error_response.size());
        return;
    }

    // NodeInternal sets file permissions to ----r-x--- (owner has no read bit).
    // Fix that so we can open the file ourselves on retrieval.
    char stored_file_path[512];
    snprintf(stored_file_path, sizeof(stored_file_path),
             "./node-data/%d/storage/%s", node_id, filename.c_str());
    chmod(stored_file_path, 0644);

    std::string ok_response = "OK\n";
    send_all(connection_fd, ok_response.c_str(), ok_response.size());
}

// Looks up the file in local storage and streams its bytes back over the connection.
// NodeInternal's read_file has a known bug where it opens the wrong path, so we
// build the full storage path manually and open the file ourselves.
void NodeServer::handle_retrieve(int connection_fd, const std::string &filename) {
    std::lock_guard<std::mutex> lock(storage_mutex);

    if (local_storage.contains_file(filename.c_str()) != 1) {
        std::string error_response = "ERROR file not found\n";
        send_all(connection_fd, error_response.c_str(), error_response.size());
        return;
    }

    off_t filesize = -1;
    try {
        filesize = local_storage.get_file_size(filename.c_str());
    } catch (...) {
        std::string error_response = "ERROR could not get file size\n";
        send_all(connection_fd, error_response.c_str(), error_response.size());
        return;
    }

    // Build the full path manually because NodeInternal::read_file has a bug
    // where it opens the filename directly instead of the full storage path.
    char full_storage_path[512];
    snprintf(full_storage_path, sizeof(full_storage_path),
             "./node-data/%d/storage/%s", node_id, filename.c_str());

    int file_fd = open(full_storage_path, O_RDONLY);
    if (file_fd < 0) {
        std::string error_response = "ERROR could not open file\n";
        send_all(connection_fd, error_response.c_str(), error_response.size());
        return;
    }

    send_file_data(connection_fd, filename.c_str(), filesize, file_fd);
    close(file_fd);
}

// Deletes the file from local storage and reports success or failure.
void NodeServer::handle_delete(int connection_fd, const std::string &filename) {
    int delete_result;
    {
        std::lock_guard<std::mutex> lock(storage_mutex);
        delete_result = local_storage.delete_file(filename.c_str());
    }

    if (delete_result != 0) {
        std::string error_response = "ERROR failed to delete file\n";
        send_all(connection_fd, error_response.c_str(), error_response.size());
        return;
    }

    std::string ok_response = "OK\n";
    send_all(connection_fd, ok_response.c_str(), ok_response.size());
}

// Asks NodeInternal for all stored filenames and their sizes, then sends the
// list back. Frees the char** array that NodeInternal allocates.
void NodeServer::handle_list(int connection_fd) {
    std::lock_guard<std::mutex> lock(storage_mutex);

    char **file_list = local_storage.list_files();

    int file_count = 0;
    std::string file_lines;

    if (file_list != nullptr) {
        while (file_list[file_count] != nullptr) {
            off_t file_size = 0;
            try {
                file_size = local_storage.get_file_size(file_list[file_count]);
            } catch (...) {
                file_size = 0;
            }

            file_lines += file_list[file_count];
            file_lines += " ";
            file_lines += std::to_string((long long)file_size);
            file_lines += "\n";

            free(file_list[file_count]);
            file_count++;
        }
        free(file_list);
    }

    std::string response = "LIST " + std::to_string(file_count) + "\n" + file_lines;
    send_all(connection_fd, response.c_str(), response.size());
}

// Sends back the total number of files and total bytes stored on this node.
void NodeServer::handle_status(int connection_fd) {
    std::lock_guard<std::mutex> lock(storage_mutex);

    char **file_list = local_storage.list_files();
    int file_count = 0;
    if (file_list != nullptr) {
        while (file_list[file_count] != nullptr) {
            free(file_list[file_count]);
            file_count++;
        }
        free(file_list);
    }

    off_t total_bytes_stored = local_storage.get_node_size();
    if (total_bytes_stored < 0) {
        total_bytes_stored = 0;
    }

    std::string response = "STATUS " + std::to_string(file_count) + " "
                         + std::to_string((long long)total_bytes_stored) + "\n";
    send_all(connection_fd, response.c_str(), response.size());
}
