#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <vector>
#include <atomic>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>

// Added to support node routing and distributed storage
#include <mutex>
#include <unordered_map>
#include <chrono>
#include <csignal>
#include <arpa/inet.h>

#include "node_server.hpp"
#include "tcp_helpers.hpp"
#include "constants.hpp"

// g++ -std=c++17 server.cpp -o server -pthread
// ./server

//  How to Use:

//   Start server:
//   g++ -std=c++17 server.cpp -o server -pthread
//   ./server

//   Test with netcat:
//   nc localhost 9000
//   LIST
//   QUIT

//   Test with Python client:
//   python3 test_client.py

// Protocol:
// Client sends: COMMAND [args]\n
// Server responds: OK [data]\n or ERROR [message]\n
//
// Commands:
//   LIST                      - List all files
//   UPLOAD <filename> <size>  - Upload a file (followed by <size> bytes)
//   DOWNLOAD <filename>       - Download a file
//   DELETE <filename>         - Delete a file
//   STATUS                    - Show storage info across all nodes
//   QUIT                      - Close connection

namespace fs = std::filesystem;

std::atomic<int> client_count(0);

// Shared mutex so concurrent threads don't interleave their console output
std::mutex print_mutex;

// Tracks which node a file lives on and how large it is.
// The server uses this to route DOWNLOAD and DELETE to the right node.
struct FileMetadata {
    int    node_id;
    size_t filesize;
};

static std::unordered_map<std::string, FileMetadata> file_metadata_map;
static std::mutex metadata_mutex;

// Cycles 0 -> 1 -> 2 -> 0 to spread uploads evenly across nodes
static int next_node_index = 0;

// Global listen fd so the signal handler can close it on Ctrl+C
static int server_listen_fd = -1;

static void handle_shutdown_signal(int signal_number) {
    std::cout << "\n[server] shutting down (signal " << signal_number << ")\n";
    if (server_listen_fd >= 0) {
        close(server_listen_fd);
    }
    _exit(0);
}

// Opens a TCP connection to a node running on localhost at NODE_BASE_PORT + node_id.
// Returns the connection fd, or -1 if the connection failed.
static int connect_to_node(int node_id) {
    int connection_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connection_fd < 0) {
        return -1;
    }

    struct sockaddr_in node_address;
    memset(&node_address, 0, sizeof(node_address));
    node_address.sin_family = AF_INET;
    node_address.sin_port   = htons(NODE_BASE_PORT + node_id);
    inet_pton(AF_INET, "127.0.0.1", &node_address.sin_addr);

    if (connect(connection_fd, (struct sockaddr *)&node_address, sizeof(node_address)) < 0) {
        close(connection_fd);
        return -1;
    }

    return connection_fd;
}

// Send a response to the client
void send_response(int client_fd, const std::string& response) {
    std::string msg = response + "\n";
    send(client_fd, msg.c_str(), msg.length(), 0);
}

// Read a line from the socket (until \n)
std::string read_line(int client_fd) {
    std::string line;
    char ch;
    while (true) {
        ssize_t n = recv(client_fd, &ch, 1, 0);
        if (n <= 0) {
            return "";  // Connection closed or error
        }
        if (ch == '\n') {
            break;
        }
        if (ch != '\r') {  // Ignore \r
            line += ch;
        }
    }
    return line;
}

// Read exact number of bytes from socket
bool read_exact(int client_fd, char* buffer, size_t size) {
    size_t total_read = 0;
    while (total_read < size) {
        ssize_t n = recv(client_fd, buffer + total_read, size - total_read, 0);
        if (n <= 0) {
            return false;  // Connection closed or error
        }
        total_read += n;
    }
    return true;
}

// Queries each node for its file list and sends a combined listing to the client.
// Response format: "OK <count>\n" followed by one "<filename> <size>\n" per file.
// Sending the count first lets the client know exactly how many lines to read.
void handle_list(int client_fd, int client_id) {
    std::cout << "[Client " << client_id << "] LIST\n";

    std::lock_guard<std::mutex> lock(metadata_mutex);

    std::string header = "OK " + std::to_string(file_metadata_map.size()) + "\n";
    send_all(client_fd, header.c_str(), header.size());

    for (auto &entry : file_metadata_map) {
        std::string file_line = entry.first + " "
                                + std::to_string(entry.second.filesize) + "\n";
        send_all(client_fd, file_line.c_str(), file_line.size());
    }
}

// Receives the file from the client then forwards it to a storage node.
// Picks the next node round-robin, or reuses the same node if the file already exists.
void handle_upload(int client_fd, int client_id, const std::string& filename, size_t filesize) {
    std::cout << "[Client " << client_id << "] UPLOAD " << filename
              << " (" << filesize << " bytes)\n";

    // Validate filename to prevent directory traversal attacks
    if (filename.find("..") != std::string::npos || filename.find("/") != std::string::npos) {
        send_response(client_fd, "ERROR Invalid filename");
        return;
    }

    // Receive file bytes from the client into a buffer
    std::vector<char> file_buffer(filesize);
    if (!read_exact(client_fd, file_buffer.data(), filesize)) {
        send_response(client_fd, "ERROR Connection lost during upload");
        return;
    }

    // If the file already exists, send to the same node. Otherwise pick next round-robin.
    int target_node_id;
    bool is_overwrite = false;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        auto existing_entry = file_metadata_map.find(filename);
        if (existing_entry != file_metadata_map.end()) {
            target_node_id = existing_entry->second.node_id;
            is_overwrite   = true;
        } else {
            target_node_id  = next_node_index;
            next_node_index = (next_node_index + 1) % NUM_NODES;
        }
    }

    // Forward the file to the target node
    int node_connection_fd = connect_to_node(target_node_id);
    if (node_connection_fd < 0) {
        send_response(client_fd, "ERROR Could not reach storage node");
        return;
    }

    std::string store_command = "NODE_STORE " + filename + " "
                               + std::to_string(filesize) + "\n";
    send_all(node_connection_fd, store_command.c_str(), store_command.size());
    send_all(node_connection_fd, file_buffer.data(), filesize);

    std::string node_response;
    if (recv_line(node_connection_fd, node_response) != 0
            || node_response.substr(0, 2) != "OK") {
        close(node_connection_fd);
        send_response(client_fd, "ERROR Storage node failed to store file");
        return;
    }
    close(node_connection_fd);

    // Record the file location in the metadata map
    {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        file_metadata_map[filename] = {target_node_id, filesize};
    }

    if (is_overwrite) {
        send_response(client_fd, "OK File updated successfully");
    } else {
        send_response(client_fd, "OK File uploaded successfully");
    }

    std::cout << "[Client " << client_id << "] Upload complete: " << filename
              << " -> node " << target_node_id << "\n";
}

// Looks up which node has the file, retrieves it, and streams it to the client.
void handle_download(int client_fd, int client_id, const std::string& filename) {
    std::cout << "[Client " << client_id << "] DOWNLOAD " << filename << "\n";

    int target_node_id;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        auto entry = file_metadata_map.find(filename);
        if (entry == file_metadata_map.end()) {
            send_response(client_fd, "ERROR File not found");
            return;
        }
        target_node_id = entry->second.node_id;
    }

    int node_connection_fd = connect_to_node(target_node_id);
    if (node_connection_fd < 0) {
        send_response(client_fd, "ERROR Could not reach storage node");
        return;
    }

    std::string retrieve_command = "NODE_RETRIEVE " + filename + "\n";
    send_all(node_connection_fd, retrieve_command.c_str(), retrieve_command.size());

    // Node responds with "FILE <filename> <filesize>\n<bytes>"
    std::string node_response;
    if (recv_line(node_connection_fd, node_response) != 0) {
        close(node_connection_fd);
        send_response(client_fd, "ERROR Node disconnected");
        return;
    }

    if (node_response.substr(0, 4) != "FILE") {
        close(node_connection_fd);
        send_response(client_fd, "ERROR " + node_response);
        return;
    }

    // Parse "FILE <filename> <filesize>"
    std::istringstream response_stream(node_response);
    std::string tag, received_filename;
    size_t received_filesize;
    response_stream >> tag >> received_filename >> received_filesize;

    std::vector<char> file_buffer(received_filesize);
    if (recv_file_data(node_connection_fd, received_filesize, file_buffer.data()) != 0) {
        close(node_connection_fd);
        send_response(client_fd, "ERROR Failed to read file from node");
        return;
    }
    close(node_connection_fd);

    // Send OK with filesize so client knows how many bytes to read
    send_response(client_fd, "OK " + std::to_string(received_filesize));
    send(client_fd, file_buffer.data(), received_filesize, 0);

    std::cout << "[Client " << client_id << "] Download complete: " << filename << "\n";
}

// Tells the node holding the file to delete it, then removes it from the metadata map.
void handle_delete(int client_fd, int client_id, const std::string& filename) {
    std::cout << "[Client " << client_id << "] DELETE " << filename << "\n";

    int target_node_id;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        auto entry = file_metadata_map.find(filename);
        if (entry == file_metadata_map.end()) {
            send_response(client_fd, "ERROR File not found");
            return;
        }
        target_node_id = entry->second.node_id;
    }

    int node_connection_fd = connect_to_node(target_node_id);
    if (node_connection_fd < 0) {
        send_response(client_fd, "ERROR Could not reach storage node");
        return;
    }

    std::string delete_command = "NODE_DELETE " + filename + "\n";
    send_all(node_connection_fd, delete_command.c_str(), delete_command.size());

    std::string node_response;
    recv_line(node_connection_fd, node_response);
    close(node_connection_fd);

    if (node_response.substr(0, 2) == "OK") {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        file_metadata_map.erase(filename);
        send_response(client_fd, "OK File deleted");
    } else {
        send_response(client_fd, "ERROR Failed to delete file");
    }
}

// Queries each node for its file count and total storage used, then sends a summary.
// Response format: "OK <node_count>\n" followed by one "NODE <id> <files> <bytes>\n" per node.
void handle_status(int client_fd, int client_id) {
    std::cout << "[Client " << client_id << "] STATUS\n";

    std::string header = "OK " + std::to_string(NUM_NODES) + "\n";
    send_all(client_fd, header.c_str(), header.size());

    for (int node_id = 0; node_id < NUM_NODES; node_id++) {
        int node_connection_fd = connect_to_node(node_id);
        if (node_connection_fd < 0) {
            std::string unreachable_line = "NODE " + std::to_string(node_id) + " unreachable\n";
            send_all(client_fd, unreachable_line.c_str(), unreachable_line.size());
            continue;
        }

        std::string status_command = "NODE_STATUS\n";
        send_all(node_connection_fd, status_command.c_str(), status_command.size());

        std::string node_response;
        recv_line(node_connection_fd, node_response);
        close(node_connection_fd);

        // Node responds with "STATUS <file_count> <total_bytes>"
        std::istringstream response_stream(node_response);
        std::string tag;
        int file_count = 0;
        long long total_bytes = 0;
        response_stream >> tag >> file_count >> total_bytes;

        std::string node_line = "NODE " + std::to_string(node_id) + " "
                                + std::to_string(file_count) + " "
                                + std::to_string(total_bytes) + "\n";
        send_all(client_fd, node_line.c_str(), node_line.size());
    }
}

// Handle each client in a separate thread
void handle_client(int client_fd, int client_id) {
    std::cout << "[Client " << client_id << "] Connected\n";

    send_response(client_fd, "OK Welcome to DFS Server. Commands: LIST, UPLOAD, DOWNLOAD, DELETE, STATUS, QUIT");

    // Persistent connection loop
    while (true) {
        std::string command = read_line(client_fd);

        if (command.empty()) {
            std::cout << "[Client " << client_id << "] Connection closed\n";
            break;
        }

        std::istringstream iss(command);
        std::string cmd;
        iss >> cmd;

        // Convert to uppercase so commands work regardless of how client types them
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);

        if (cmd == "QUIT") {
            send_response(client_fd, "OK Goodbye");
            break;
        } else if (cmd == "LIST") {
            handle_list(client_fd, client_id);
        } else if (cmd == "UPLOAD") {
            std::string filename;
            size_t filesize;
            iss >> filename >> filesize;

            if (filename.empty() || filesize == 0) {
                send_response(client_fd, "ERROR Usage: UPLOAD <filename> <size>");
            } else {
                handle_upload(client_fd, client_id, filename, filesize);
            }
        } else if (cmd == "DOWNLOAD") {
            std::string filename;
            iss >> filename;

            if (filename.empty()) {
                send_response(client_fd, "ERROR Usage: DOWNLOAD <filename>");
            } else {
                handle_download(client_fd, client_id, filename);
            }
        } else if (cmd == "DELETE") {
            std::string filename;
            iss >> filename;

            if (filename.empty()) {
                send_response(client_fd, "ERROR Usage: DELETE <filename>");
            } else {
                handle_delete(client_fd, client_id, filename);
            }
        } else if (cmd == "STATUS") {
            handle_status(client_fd, client_id);
        } else {
            send_response(client_fd, "ERROR Unknown command. Available: LIST, UPLOAD, DOWNLOAD, DELETE, STATUS, QUIT");
        }
    }

    close(client_fd);
    std::cout << "[Client " << client_id << "] Disconnected\n";
    client_count--;
}

int main() {
    signal(SIGINT,  handle_shutdown_signal);
    signal(SIGTERM, handle_shutdown_signal);
    signal(SIGPIPE, SIG_IGN);  // Ignore broken-pipe so writes to dead sockets return -1 instead of killing the process

    // Start all storage nodes before accepting client connections.
    // Each node runs in its own background thread on its own port.
    std::vector<NodeServer *> storage_nodes;
    for (int node_id = 0; node_id < NUM_NODES; node_id++) {
        NodeServer *node = new NodeServer(node_id);
        storage_nodes.push_back(node);
        node->start();
    }

    // Give nodes a moment to bind their ports before we start accepting clients
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Create socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Error: Failed to create socket\n";
        return 1;
    }

    // Allow port reuse (avoids "address already in use" errors)
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "Error: Failed to set socket options\n";
        close(server_fd);
        return 1;
    }

    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;  // Accept from any IP
    address.sin_port = htons(9000);        // Port 9000

    // Bind socket to address
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Error: Failed to bind socket to port 9000\n";
        close(server_fd);
        return 1;
    }

    // Listen for connections
    if (listen(server_fd, 5) < 0) {
        std::cerr << "Error: Failed to listen on socket\n";
        close(server_fd);
        return 1;
    }

    server_listen_fd = server_fd;

    std::cout << "=== Distributed File System Server ===\n";
    std::cout << "Storage nodes on ports "
              << NODE_BASE_PORT << " to " << (NODE_BASE_PORT + NUM_NODES - 1) << "\n";
    std::cout << "Server listening on port 9000...\n";
    std::cout << "Press Ctrl+C to stop\n\n";

    std::vector<std::thread> threads;
    int next_client_id = 1;

    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);

        if (client_fd < 0) {
            std::cerr << "Error: Failed to accept client connection\n";
            continue;
        }

        client_count++;
        std::cout << "Active clients: " << client_count << "\n";

        // Create a new thread to handle this client
        threads.emplace_back(handle_client, client_fd, next_client_id++);
        threads.back().detach();  // Detach so thread cleans up automatically
    }

    close(server_fd);
    return 0;
}
