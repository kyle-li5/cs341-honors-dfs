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
//   QUIT                      - Close connection

namespace fs = std::filesystem;

std::atomic<int> client_count(0);
const std::string STORAGE_DIR = "./storage/";

// Initialize storage directory
void init_storage() {
    if (!fs::exists(STORAGE_DIR)) {
        fs::create_directory(STORAGE_DIR);
        std::cout << "Created storage directory: " << STORAGE_DIR << "\n";
    }
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

// Handle LIST command
void handle_list(int client_fd, int client_id) {
    std::cout << "[Client " << client_id << "] LIST\n";

    std::stringstream ss;
    int count = 0;

    for (const auto& entry : fs::directory_iterator(STORAGE_DIR)) {
        if (entry.is_regular_file()) {
            ss << entry.path().filename().string() << " ";
            ss << entry.file_size() << " bytes\n";
            count++;
        }
    }

    if (count == 0) {
        send_response(client_fd, "OK No files stored");
    } else {
        send_response(client_fd, "OK " + std::to_string(count) + " files:\n" + ss.str());
    }
}

// Handle UPLOAD command
void handle_upload(int client_fd, int client_id, const std::string& filename, size_t size) {
    std::cout << "[Client " << client_id << "] UPLOAD " << filename << " (" << size << " bytes)\n";

    // Validate filename (prevent directory traversal)
    if (filename.find("..") != std::string::npos || filename.find("/") != std::string::npos) {
        send_response(client_fd, "ERROR Invalid filename");
        return;
    }

    std::string filepath = STORAGE_DIR + filename;
    std::ofstream file(filepath, std::ios::binary);

    if (!file.is_open()) {
        send_response(client_fd, "ERROR Failed to create file");
        return;
    }

    // Read file data
    const size_t CHUNK_SIZE = 4096;
    char buffer[CHUNK_SIZE];
    size_t remaining = size;

    while (remaining > 0) {
        size_t to_read = std::min(remaining, CHUNK_SIZE);
        if (!read_exact(client_fd, buffer, to_read)) {
            send_response(client_fd, "ERROR Connection lost during upload");
            file.close();
            fs::remove(filepath);  // Clean up partial file
            return;
        }
        file.write(buffer, to_read);
        remaining -= to_read;
    }

    file.close();
    send_response(client_fd, "OK File uploaded successfully");
    std::cout << "[Client " << client_id << "] Upload complete: " << filename << "\n";
}

// Handle DOWNLOAD command
void handle_download(int client_fd, int client_id, const std::string& filename) {
    std::cout << "[Client " << client_id << "] DOWNLOAD " << filename << "\n";

    std::string filepath = STORAGE_DIR + filename;

    if (!fs::exists(filepath)) {
        send_response(client_fd, "ERROR File not found");
        return;
    }

    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        send_response(client_fd, "ERROR Failed to open file");
        return;
    }

    size_t filesize = file.tellg();
    file.seekg(0);

    // Send OK response with file size
    send_response(client_fd, "OK " + std::to_string(filesize));

    // Send file data
    const size_t CHUNK_SIZE = 4096;
    char buffer[CHUNK_SIZE];

    while (file.read(buffer, CHUNK_SIZE) || file.gcount() > 0) {
        send(client_fd, buffer, file.gcount(), 0);
    }

    file.close();
    std::cout << "[Client " << client_id << "] Download complete: " << filename << "\n";
}

// Handle DELETE command
void handle_delete(int client_fd, int client_id, const std::string& filename) {
    std::cout << "[Client " << client_id << "] DELETE " << filename << "\n";

    std::string filepath = STORAGE_DIR + filename;

    if (!fs::exists(filepath)) {
        send_response(client_fd, "ERROR File not found");
        return;
    }

    if (fs::remove(filepath)) {
        send_response(client_fd, "OK File deleted");
    } else {
        send_response(client_fd, "ERROR Failed to delete file");
    }
}

// Handle each client in a separate thread
void handle_client(int client_fd, int client_id) {
    std::cout << "[Client " << client_id << "] Connected\n";

    send_response(client_fd, "OK Welcome to DFS Server. Commands: LIST, UPLOAD, DOWNLOAD, DELETE, QUIT");

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

        // Convert to uppercase
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);

        if (cmd == "QUIT") {
            send_response(client_fd, "OK Goodbye");
            break;
        }
        else if (cmd == "LIST") {
            handle_list(client_fd, client_id);
        }
        else if (cmd == "UPLOAD") {
            std::string filename;
            size_t size;
            iss >> filename >> size;

            if (filename.empty() || size == 0) {
                send_response(client_fd, "ERROR Usage: UPLOAD <filename> <size>");
            } else {
                handle_upload(client_fd, client_id, filename, size);
            }
        }
        else if (cmd == "DOWNLOAD") {
            std::string filename;
            iss >> filename;

            if (filename.empty()) {
                send_response(client_fd, "ERROR Usage: DOWNLOAD <filename>");
            } else {
                handle_download(client_fd, client_id, filename);
            }
        }
        else if (cmd == "DELETE") {
            std::string filename;
            iss >> filename;

            if (filename.empty()) {
                send_response(client_fd, "ERROR Usage: DELETE <filename>");
            } else {
                handle_delete(client_fd, client_id, filename);
            }
        }
        else {
            send_response(client_fd, "ERROR Unknown command. Available: LIST, UPLOAD, DOWNLOAD, DELETE, QUIT");
        }
    }

    close(client_fd);
    std::cout << "[Client " << client_id << "] Disconnected\n";
    client_count--;
}

int main() {
    // Initialize storage directory
    init_storage();

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