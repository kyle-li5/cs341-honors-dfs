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
#include <arpa/inet.h>
#include <netinet/tcp.h>

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
    // Check if the node crashed mid-operation last time and clean up before
    // accepting any connections, so NODE_LIST never returns a corrupt file.
    int error = local_storage.check_error();
    if (error == 2 || error == 5) {
        char *bad_file = local_storage.get_error_info(2);
        if (bad_file != nullptr) {
            local_storage.delete_file(bad_file);
            free(bad_file);
        }
    } else if (error == 4) {
        local_storage.clear_existing_data();
    }

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

    {
        std::lock_guard<std::mutex> lock(print_mutex);
        std::cout << "[node " << node_id << "] listening on port "
                  << (NODE_BASE_PORT + node_id) << "\n";
        std::cout.flush();
    }

    while (true) {
        int connection_fd = accept(listen_fd, nullptr, nullptr);
        if (connection_fd < 0) {
            perror("node_server accept");
            break;
        }
        // Disable Nagle so small per-chunk OK replies aren't held by the
        // sender. macOS reports the option back as the internal TF_NODELAY
        // flag bit (0x4), not 1 — that's still "set".
        int nodelay = 1;
        setsockopt(connection_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        handle_connection(connection_fd);
        close(connection_fd);
    }
}

// Reads commands from the connection in a loop and dispatches to the right
// handler. Multiple commands may share one connection so the coordinator can
// stream many NODE_STORE chunks down a single TCP socket without paying a
// connect/close round trip per chunk. The loop exits when recv_line fails
// (peer disconnected, RST, or 5s read timeout).
//
// forward_fd / forward_target_node persist across NODE_STORE commands within
// this connection so the head→mid (and mid→tail) hop also stays open across
// chunks instead of re-connecting per chunk.
void NodeServer::handle_connection(int connection_fd) {
    int forward_fd          = -1;
    int forward_target_node = -1;

    while (true) {
        std::string command_line;
        if (recv_line(connection_fd, command_line) != 0) {
            break;
        }

        std::istringstream command_stream(command_line);
        std::string command;
        command_stream >> command;

        if (command == "NODE_STORE") {
            std::string filename;
            size_t filesize;
            int num_redundancies = 0;
            std::vector<int> redundant_nodes;

            if (!(command_stream >> filename >> filesize)) {
                std::string error_response = "ERROR invalid NODE_STORE syntax\n";
                send_all(connection_fd, error_response.c_str(), error_response.size());
                continue;
            }
            if (command_stream >> num_redundancies) {
                for (int i = 0; i < num_redundancies; ++i) {
                    int next_node_id;
                    if (command_stream >> next_node_id) {
                        redundant_nodes.push_back(next_node_id);
                    }
                }
            }
            handle_store(connection_fd, filename, filesize, redundant_nodes,
                         forward_fd, forward_target_node);

        } else if (command == "NODE_RETRIEVE") {
            std::string filename;
            if (!(command_stream >> filename)) {
                std::string error_response = "ERROR invalid NODE_RETRIEVE syntax\n";
                send_all(connection_fd, error_response.c_str(), error_response.size());
                continue;
            }
            handle_retrieve(connection_fd, filename);

        } else if (command == "NODE_DELETE") {
            std::string filename;
            if (!(command_stream >> filename)) {
                std::string error_response = "ERROR invalid NODE_DELETE syntax\n";
                send_all(connection_fd, error_response.c_str(), error_response.size());
                continue;
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

    // Tear down the persistent forward socket when our upstream goes away —
    // the downstream peer's recv_line will then return failure and its loop
    // will fall through here too, so the chain unwinds cleanly.
    if (forward_fd >= 0) {
        close(forward_fd);
    }
}

// Receives the file bytes from the connection, writes them into a pipe, and
// passes the read end of the pipe to NodeInternal which reads from it to store
// the file. A pipe is needed because NodeInternal expects a file descriptor,
// not a buffer. We use a thread to write into the pipe while NodeInternal
// reads from it simultaneously to avoid a deadlock.
//
// forward_fd is owned by handle_connection and persists across NODE_STORE
// commands on this connection — we open it lazily on the first chunk that
// has redundancies, reuse it for every later chunk, and only close + reopen
// if the redundancy chain's first hop changes (rare; only on dead-node
// failover at the coordinator). pipe_writer no longer closes node_fd for the
// same reason.
void NodeServer::handle_store(int connection_fd, const std::string &filename, size_t filesize,
                              std::vector<int>& redundant_nodes,
                              int &forward_fd, int &forward_target_node) {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        perror("node_server pipe");
        std::string error_response = "ERROR failed to create internal pipe\n";
        send_all(connection_fd, error_response.c_str(), error_response.size());
        return;
    }

    // Make sure the persistent forward socket points at the right next-hop.
    // If the redundancy list changed between chunks (coordinator failed over
    // to a different chain) tear down the old forward and open a new one.
    if (!redundant_nodes.empty()) {
        int next_node = redundant_nodes[0];

        if (forward_fd >= 0 && forward_target_node != next_node) {
            close(forward_fd);
            forward_fd          = -1;
            forward_target_node = -1;
        }

        if (forward_fd < 0) {
            int new_fd = socket(AF_INET, SOCK_STREAM, 0);

            // Match the coordinator's 5s recv/send timeouts so a downstream node
            // dying mid-pipeline doesn't hang this node forever on the ack.
            struct timeval timeout;
            timeout.tv_sec  = 5;
            timeout.tv_usec = 0;
            setsockopt(new_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(new_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

            int nodelay = 1;
            setsockopt(new_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

            struct sockaddr_in sockaddr;
            memset(&sockaddr, 0, sizeof(sockaddr));
            sockaddr.sin_family = AF_INET;
            sockaddr.sin_port   = htons(NODE_BASE_PORT + next_node);
            inet_pton(AF_INET, "127.0.0.1", &sockaddr.sin_addr);

            if (connect(new_fd, (struct sockaddr*)&sockaddr, sizeof(sockaddr)) == 0) {
                forward_fd          = new_fd;
                forward_target_node = next_node;
            } else {
                std::cerr << "[node " << node_id << "] warning: failed to connect to node "
                          << next_node << "\n";
                close(new_fd);
            }
        }
    }

    // Send the per-chunk forward command (header only) over the persistent
    // forward socket. Bytes follow inside the pipe_writer loop below.
    if (forward_fd >= 0) {
        std::string forward_command = "NODE_STORE " + filename + " " + std::to_string(filesize);
        int remaining_nodes = (int)redundant_nodes.size() - 1;
        if (remaining_nodes > 0) {
            forward_command += " " + std::to_string(remaining_nodes);
            for (size_t i = 1; i < redundant_nodes.size(); ++i) {
                forward_command += " " + std::to_string(redundant_nodes[i]);
            }
        }
        forward_command += "\n";
        send_all(forward_fd, forward_command.c_str(), forward_command.size());
    }

    // Spawn a thread to stream bytes from the connection into the write end
    // of the pipe while NodeInternal reads from the read end simultaneously.
    // The thread is joined (not detached) before we ACK the upstream caller —
    // its tail does recv_line(forward_fd, ack), so joining ensures the downstream
    // chain has finished writing before we tell our caller "OK". Without this
    // a STATUS query right after upload could see the tail node missing the
    // last chunk because the head ACKed before the chain caught up.
    //
    // forward_ok records whether we got a clean OK from the next hop. If not,
    // handle_connection's persistent forward_fd is unsafe to reuse for the
    // next chunk — we close it below and let the next chunk reopen.
    int  node_fd     = forward_fd;
    bool forward_ok  = (node_fd < 0); // no forward expected → trivially "ok"
    std::thread pipe_writer([connection_fd, filesize, node_fd, write_end_fd = pipe_fds[1], &forward_ok]() {
        // 64 KB local buffer: BUFFER_SIZE = 4 KB fragments each NODE_STORE
        // payload into 16 small TCP sends per chunk, and on macOS those
        // small sends collide with the receiver's delayed-ACK timer at
        // scale (cwnd/quickack heuristics stop firing). One read/write
        // per chunk avoids that and is the difference between ~50 s and
        // ~3 s for a 100 MB upload.
        char chunk[64 * 1024];
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

            // forward to next-hop node over the persistent pipeline socket
            if (node_fd >= 0) {
                send_all(node_fd, chunk, bytes_received);
            }
            bytes_remaining -= (size_t)bytes_received;
        }
        done:
        close(write_end_fd);

        // Consume the per-chunk ACK from the next hop so the next chunk's
        // exchange starts with a clean read state. We do NOT close node_fd —
        // handle_connection owns that lifecycle now.
        if (node_fd >= 0) {
            std::string ack;
            if (recv_line(node_fd, ack) == 0 && ack.substr(0, 2) == "OK") {
                forward_ok = true;
            }
        }
    });

    int store_result;
    {
        std::lock_guard<std::mutex> lock(storage_mutex);
        if (local_storage.contains_file(filename.c_str()) == 1) {
            store_result = local_storage.replace_file(filename.c_str(), pipe_fds[0]);
        } else {
            store_result = local_storage.create_file(filename.c_str(), pipe_fds[0]);
        }
        // A failed op can leave NodeInternal's cached byte total drifted —
        // recompute it so subsequent NODE_STATUS queries stay accurate.
        if (store_result != 0) {
            local_storage.compute_node_size();
        }
    }
    close(pipe_fds[0]);

    // Wait for the pipe_writer (which is also waiting on the downstream ACK)
    // before responding, so our OK happens-after the entire chain's writes.
    pipe_writer.join();

    // If the downstream chain went silent (timeout, RST, garbage response),
    // tear down the persistent forward socket so the next chunk on this
    // connection re-opens it. We still ACK upstream because our LOCAL store
    // succeeded — the chunk is safe on this node, the rest of the chain is
    // degraded but recoverable.
    if (forward_fd >= 0 && !forward_ok) {
        std::cerr << "[node " << node_id << "] forward chain to node "
                  << forward_target_node << " went silent, recycling socket\n";
        close(forward_fd);
        forward_fd          = -1;
        forward_target_node = -1;
    }

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
        if (delete_result != 0) {
            local_storage.compute_node_size();
        }
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
