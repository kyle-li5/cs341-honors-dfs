#include <iostream>
#include <cstring>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <fstream>
#include <arpa/inet.h>
#include <netdb.h>
#include <vector>
#include <sstream>
#include <filesystem>

#include "tcp_helpers.hpp"
#include "constants.hpp"

// ---------------------------------------------------------------------------
// Original chunk-based upload logic (kept from original client.cpp).
// The round-robin chunk-to-node routing that was here has been moved into
// the coordinator on the server side, so the client now talks only to the
// coordinator on port 9000. The CHUNK_SIZE constant and data_info struct
// are kept here for reference and for future sharding work.
// ---------------------------------------------------------------------------

struct data_info {
    int chunk_id;
    int size;
};

[[maybe_unused]] const int CHUNK_SIZE = 64 * 1024; // 64 KB chunks, kept for future file sharding

// ---------------------------------------------------------------------------
// TCP helpers used by all commands.
// These mirror the helpers in tcp_helpers.cpp but operate on the
// coordinator connection fd rather than node connections.
// ---------------------------------------------------------------------------

// Reads one newline-terminated line from the coordinator.
// Returns the line without the trailing newline, or empty on error.
static std::string read_coordinator_line(int coordinator_fd) {
    std::string line;
    char ch;
    while (true) {
        ssize_t bytes_received = recv(coordinator_fd, &ch, 1, 0);
        if (bytes_received <= 0) {
            return "";
        }
        if (ch == '\n') {
            break;
        }
        if (ch != '\r') {
            line += ch;
        }
    }
    return line;
}

// ---------------------------------------------------------------------------
// Command implementations
// ---------------------------------------------------------------------------

// Sends LIST to the coordinator and prints the file listing.
// Server responds with "OK <count>\n" then one "<filename> <size>\n" per file.
static void cmd_list(int coordinator_fd) {
    std::string request = "LIST\n";
    send_all(coordinator_fd, request.c_str(), request.size());

    std::string response = read_coordinator_line(coordinator_fd);
    if (response.substr(0, 2) != "OK") {
        std::cerr << "Error: " << response << "\n";
        return;
    }

    // Parse the count out of "OK <count>"
    std::istringstream response_stream(response);
    std::string tag;
    int file_count = 0;
    response_stream >> tag >> file_count;

    if (file_count == 0) {
        std::cout << "No files stored.\n";
        return;
    }

    std::cout << file_count << " file(s):\n";
    for (int i = 0; i < file_count; i++) {
        std::string file_line = read_coordinator_line(coordinator_fd);
        // Each line: "<filename> <size>"
        std::istringstream line_stream(file_line);
        std::string filename;
        long long file_size = 0;
        line_stream >> filename >> file_size;
        std::cout << "  " << filename << "  (" << file_size << " bytes)\n";
    }
}

// Reads a local file and uploads it to the coordinator.
// Sends "UPLOAD <filename> <size>\n" then the raw file bytes.
//
// Uses POSIX open()/fstat()/read() instead of std::ifstream so we get real
// errno reporting and can retry on EINTR. The previous ifstream-based code
// silently swallowed the underlying open() error code, which made transient
// failures look "nondeterministic" with no way to diagnose them.
static void cmd_upload(int coordinator_fd, const std::string& filepath) {
    int file_fd;
    while (true) {
        file_fd = open(filepath.c_str(), O_RDONLY);
        if (file_fd >= 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        std::cerr << "Error: Cannot open file: " << filepath
                  << " (" << std::strerror(errno) << ")\n";
        return;
    }

    struct stat file_stat;
    if (fstat(file_fd, &file_stat) != 0) {
        std::cerr << "Error: Cannot stat file: " << filepath
                  << " (" << std::strerror(errno) << ")\n";
        close(file_fd);
        return;
    }
    off_t file_size = file_stat.st_size;

    // Read the entire file into memory
    std::vector<char> file_buffer(file_size);
    off_t total_read = 0;
    while (total_read < file_size) {
        ssize_t bytes_read = read(file_fd,
                                  file_buffer.data() + total_read,
                                  file_size - total_read);
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "Error: Failed to read file: " << filepath
                      << " (" << std::strerror(errno) << ")\n";
            close(file_fd);
            return;
        }
        if (bytes_read == 0) {
            break; // EOF earlier than fstat reported
        }
        total_read += bytes_read;
    }
    close(file_fd);

    // Use just the filename, not the full path, as the stored name
    std::string filename = std::filesystem::path(filepath).filename().string();

    std::string upload_command = "UPLOAD " + filename + " "
                                 + std::to_string(total_read) + "\n";
    send_all(coordinator_fd, upload_command.c_str(), upload_command.size());
    send_all(coordinator_fd, file_buffer.data(), total_read);

    std::string response = read_coordinator_line(coordinator_fd);
    if (response.substr(0, 2) == "OK") {
        std::cout << "Uploaded " << filename << " (" << total_read << " bytes)\n";
    } else {
        std::cerr << "Error: " << response << "\n";
    }
}

// Downloads a file from the coordinator and saves it to the current directory.
// Server responds with "OK <size>\n" then the raw file bytes.
static void cmd_download(int coordinator_fd, const std::string& filename) {
    std::string download_command = "DOWNLOAD " + filename + "\n";
    send_all(coordinator_fd, download_command.c_str(), download_command.size());

    std::string response = read_coordinator_line(coordinator_fd);
    if (response.substr(0, 2) != "OK") {
        std::cerr << "Error: " << response << "\n";
        return;
    }

    // Parse the file size out of "OK <size>"
    std::istringstream response_stream(response);
    std::string tag;
    size_t file_size = 0;
    response_stream >> tag >> file_size;

    std::vector<char> file_buffer(file_size);
    if (recv_file_data(coordinator_fd, file_size, file_buffer.data()) != 0) {
        std::cerr << "Error: Connection lost while receiving file data\n";
        return;
    }

    // Save to current working directory with the original filename
    std::ofstream output_file(filename, std::ios::binary);
    if (!output_file.is_open()) {
        std::cerr << "Error: Cannot write file: " << filename << "\n";
        return;
    }
    output_file.write(file_buffer.data(), file_size);
    output_file.close();

    std::cout << "Downloaded " << filename << " (" << file_size << " bytes)\n";
}

// Sends DELETE to the coordinator for the given filename.
static void cmd_delete(int coordinator_fd, const std::string& filename) {
    std::string delete_command = "DELETE " + filename + "\n";
    send_all(coordinator_fd, delete_command.c_str(), delete_command.size());

    std::string response = read_coordinator_line(coordinator_fd);
    if (response.substr(0, 2) == "OK") {
        std::cout << "Deleted " << filename << "\n";
    } else {
        std::cerr << "Error: " << response << "\n";
    }
}

// Requests STATUS from the coordinator and prints per-node storage info.
// Server responds with "OK <node_count>\n" then one "NODE <id> <files> <bytes>\n" per node.
static void cmd_status(int coordinator_fd) {
    std::string request = "STATUS\n";
    send_all(coordinator_fd, request.c_str(), request.size());

    std::string response = read_coordinator_line(coordinator_fd);
    if (response.substr(0, 2) != "OK") {
        std::cerr << "Error: " << response << "\n";
        return;
    }

    std::istringstream response_stream(response);
    std::string tag;
    int node_count = 0;
    response_stream >> tag >> node_count;

    std::cout << node_count << " storage node(s):\n";
    for (int i = 0; i < node_count; i++) {
        std::string node_line = read_coordinator_line(coordinator_fd);

        // Each line is either "NODE <id> <files> <bytes>" or "NODE <id> unreachable"
        std::istringstream line_stream(node_line);
        std::string node_tag;
        int node_id = 0;
        std::string field_a, field_b;
        line_stream >> node_tag >> node_id >> field_a >> field_b;

        if (field_a == "unreachable") {
            std::cout << "  Node " << node_id << ": unreachable\n";
        } else {
            long long file_count  = std::stoll(field_a);
            long long total_bytes = std::stoll(field_b);
            std::cout << "  Node " << node_id << ": "
                      << file_count << " file(s), "
                      << total_bytes << " bytes\n";
        }
    }
}

// parse status for specific file
static void cmd_status_file(int coordinator_fd, const std::string& filename) {
    std::string request = "STATUS " + filename + "\n";
    send_all(coordinator_fd, request.c_str(), request.size());

    std::string response = read_coordinator_line(coordinator_fd);
    if (response.substr(0, 2) != "OK") {
        std::cerr << "Error: " << response << "\n";
        return;
    }

    std::istringstream response_stream(response);
    std::string tag;
    int chunk_count = 0;
    long long total_size = 0;
    response_stream >> tag >> chunk_count >> total_size;

    std::cout << "File status: " << filename << "\n";
    std::cout << "Total size: " << total_size << " bytes\n";
    std::cout << "Total chunks: " << chunk_count << "\n";

    for (int i = 0; i < chunk_count; i++) {
        std::string node_line = read_coordinator_line(coordinator_fd);

        // Each line is "CHUNK <index> <size> <node_count> <node1> <node2> ..." 
        std::istringstream line_stream(node_line);
        std::string chunk_tag;
        int chunk_index;
        long long chunk_size = 0;
        int node_count = 0;
        line_stream >> chunk_tag >> chunk_index >> chunk_size >> node_count;

        std::cout << "\tChunk " << chunk_index << " (" << chunk_size << " bytes) -> Nodes: ";

        for (int j = 0; j < node_count; ++j) {
            int node_id;
            line_stream >> node_id;
            std::cout << "[" << node_id << "] ";
        }
        std::cout << "\n";
    }
}

// ---------------------------------------------------------------------------
// Main -- interactive dfs> prompt
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <host> <port>\n";
        std::cerr << "Example: " << argv[0] << " localhost 9000\n";
        return 1;
    }

    const char* host = argv[1];
    const char* port_str = argv[2];

    // getaddrinfo resolves both hostnames (e.g. "raspberrypi.local") and
    // numeric IPs (e.g. "192.168.1.10"), so the client works when connecting
    // to the Pi by hostname as well as when testing on localhost.
    struct addrinfo hints;
    struct addrinfo* address_results;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int resolve_result = getaddrinfo(host, port_str, &hints, &address_results);
    if (resolve_result != 0) {
        std::cerr << "Error: Cannot resolve host " << host
                  << ": " << gai_strerror(resolve_result) << "\n";
        return 1;
    }

    // Open a persistent TCP connection to the coordinator.
    // All commands go over this single connection (no reconnect per command).
    int coordinator_fd = socket(address_results->ai_family,
                                address_results->ai_socktype,
                                address_results->ai_protocol);
    if (coordinator_fd < 0) {
        std::cerr << "Error: Failed to create socket\n";
        freeaddrinfo(address_results);
        return 1;
    }

    if (connect(coordinator_fd, address_results->ai_addr,
                address_results->ai_addrlen) < 0) {
        std::cerr << "Error: Cannot connect to " << host << ":" << port_str << "\n";
        freeaddrinfo(address_results);
        close(coordinator_fd);
        return 1;
    }
    freeaddrinfo(address_results);

    // Read and display the server's welcome message
    std::string welcome = read_coordinator_line(coordinator_fd);
    std::cout << welcome << "\n\n";

    std::cout << "Commands: list, upload <filepath>, download <filename>, "
              << "delete <filename>, status, quit\n\n";

    // Interactive command loop
    while (true) {
        std::cout << "dfs> ";
        std::cout.flush();

        std::string input_line;
        if (!std::getline(std::cin, input_line)) {
            // EOF (Ctrl+D) -- exit cleanly
            break;
        }

        if (input_line.empty()) {
            continue;
        }

        std::istringstream input_stream(input_line);
        std::string command;
        input_stream >> command;

        if (command == "quit" || command == "exit") {
            send_all(coordinator_fd, "QUIT\n", 5);
            read_coordinator_line(coordinator_fd); // consume "OK Goodbye"
            break;
        } else if (command == "list") {
            cmd_list(coordinator_fd);
        } else if (command == "upload") {
            // Use getline on the remainder so filepaths with spaces work
            std::string filepath;
            std::getline(input_stream >> std::ws, filepath);
            if (filepath.empty()) {
                std::cerr << "Usage: upload <filepath>\n";
            } else {
                cmd_upload(coordinator_fd, filepath);
            }
        } else if (command == "download") {
            std::string filename;
            std::getline(input_stream >> std::ws, filename);
            if (filename.empty()) {
                std::cerr << "Usage: download <filename>\n";
            } else {
                cmd_download(coordinator_fd, filename);
            }
        } else if (command == "delete") {
            std::string filename;
            std::getline(input_stream >> std::ws, filename);
            if (filename.empty()) {
                std::cerr << "Usage: delete <filename>\n";
            } else {
                cmd_delete(coordinator_fd, filename);
            }
        } else if (command == "status") {
            std::string filename;
            std::getline(input_stream >> std::ws, filename);
            if (filename.empty()) {
                cmd_status(coordinator_fd);
            } else {
                cmd_status_file(coordinator_fd, filename);
            }
        } else {
            std::cerr << "Unknown command: " << command << "\n";
            std::cerr << "Commands: list, upload, download, delete, status, status <filename> quit\n";
        }
    }

    close(coordinator_fd);
    std::cout << "\nGoodbye.\n";
    return 0;
}
