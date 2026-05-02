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
#include <set>
#include <unordered_map>
#include <unordered_set>
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

// Tracks which node(s) a file lives on and how large it is.
// node_id is the primary; replica_node_id is -1 if no replica exists (degraded).
struct FileMetadata {
    int    node_id;
    int    replica_node_id;
    size_t filesize;
};

static std::unordered_map<std::string, FileMetadata> file_metadata_map;
static std::unordered_set<std::string> pending_uploads;
static std::mutex metadata_mutex;

// Per-node byte totals, indexed by node_id. Protected by metadata_mutex.
// Kept alongside node_min_heap so we can find the old size in O(1) when
// updating a node's heap entry.
static off_t node_bytes[NUM_NODES] = {};

// Min-heap over (bytes_stored, node_id). std::set keeps the smallest entry
// at begin(), so picking the least-loaded node is O(1). Updates are O(log N).
// Protected by metadata_mutex.
static std::set<std::pair<off_t, int>> node_min_heap;

// Nodes that failed a recent operation. Excluded from upload routing until the
// health-check loop confirms they are back. Protected by metadata_mutex.
static std::unordered_set<int> dead_nodes;

// Adjusts node_id's stored-byte count by delta and repositions it in the heap.
// Must be called while holding metadata_mutex.
static void update_node_size(int node_id, off_t delta) {
    node_min_heap.erase({node_bytes[node_id], node_id});
    node_bytes[node_id] += delta;
    node_min_heap.insert({node_bytes[node_id], node_id});
}

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
// 5-second recv/send timeouts are set so a dead node's stalled socket doesn't
// block the coordinator forever (Hayden's RANDOM_FAILURES pthread_exit leaves
// the listen socket open but stops accepting, causing connects to succeed but
// responses to never arrive).
static int connect_to_node(int node_id) {
    int connection_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connection_fd < 0) {
        return -1;
    }

    struct timeval timeout;
    timeout.tv_sec  = 5;
    timeout.tv_usec = 0;
    setsockopt(connection_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(connection_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

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

// On startup, queries every node via NODE_LIST and rebuilds file_metadata_map
// so files uploaded in previous sessions are immediately accessible after restart.
static void rebuild_metadata_from_nodes() {
    std::lock_guard<std::mutex> lock(metadata_mutex);
    file_metadata_map.clear();
    std::fill(node_bytes, node_bytes + NUM_NODES, 0);
    node_min_heap.clear();

    for (int node_id = 0; node_id < NUM_NODES; node_id++) {
        int node_fd = connect_to_node(node_id);
        if (node_fd < 0) {
            std::cerr << "[startup] node " << node_id << " unreachable, skipping\n";
            continue;
        }

        std::string list_cmd = "NODE_LIST\n";
        send_all(node_fd, list_cmd.c_str(), list_cmd.size());

        std::string header;
        if (recv_line(node_fd, header) != 0) {
            close(node_fd);
            continue;
        }

        std::istringstream header_stream(header);
        std::string tag;
        int file_count = 0;
        header_stream >> tag >> file_count;

        for (int i = 0; i < file_count; i++) {
            std::string file_line;
            if (recv_line(node_fd, file_line) != 0) { break; }

            std::istringstream line_stream(file_line);
            std::string filename;
            size_t filesize = 0;
            line_stream >> filename >> filesize;

            if (!filename.empty()) {
                auto existing = file_metadata_map.find(filename);
                if (existing == file_metadata_map.end()) {
                    file_metadata_map[filename] = {node_id, -1, filesize};
                } else {
                    existing->second.replica_node_id = node_id;
                }
                node_bytes[node_id] += (off_t)filesize;
                std::cout << "[startup] recovered " << filename
                          << " on node " << node_id << "\n";
            }
        }
        close(node_fd);
    }

    // Build the heap from the recovered per-node totals.
    for (int i = 0; i < NUM_NODES; i++) {
        node_min_heap.insert({node_bytes[i], i});
    }

    std::cout << "[startup] " << file_metadata_map.size()
              << " file(s) recovered from nodes\n\n";
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

// Sends the file to one node via NODE_STORE and returns whether it succeeded.
static bool store_on_node(int node_id, const std::string& filename,
                          const std::vector<char>& data) {
    int node_fd = connect_to_node(node_id);
    if (node_fd < 0) {
        return false;
    }
    std::string cmd = "NODE_STORE " + filename + " "
                      + std::to_string(data.size()) + "\n";
    send_all(node_fd, cmd.c_str(), cmd.size());
    send_all(node_fd, data.data(), data.size());
    std::string resp;
    bool ok = (recv_line(node_fd, resp) == 0 && resp.substr(0, 2) == "OK");
    close(node_fd);
    return ok;
}

// Sends NODE_DELETE to a node. Best-effort: logs on failure but does not abort.
static void delete_from_node(int node_id, const std::string& filename) {
    int node_fd = connect_to_node(node_id);
    if (node_fd < 0) {
        std::cerr << "[warn] could not reach node " << node_id
                  << " to delete orphan " << filename << "\n";
        return;
    }
    std::string cmd = "NODE_DELETE " + filename + "\n";
    send_all(node_fd, cmd.c_str(), cmd.size());
    std::string resp;
    if (recv_line(node_fd, resp) != 0 || resp.substr(0, 2) != "OK") {
        std::cerr << "[warn] node " << node_id << " failed to delete " << filename << "\n";
    }
    close(node_fd);
}

// Receives a file from the client, writes it to REPLICATION_FACTOR nodes chosen
// from the min-heap (primary + replica), and updates metadata.
// If the replica upload fails the file is still stored on the primary (degraded mode).
// Overwrites always pick fresh nodes from the heap — a file that grew substantially
// can migrate off an over-loaded node; displaced old copies are cleaned up after commit.
void handle_upload(int client_fd, int client_id, const std::string& filename, size_t filesize) {
    std::cout << "[Client " << client_id << "] UPLOAD " << filename
              << " (" << filesize << " bytes)\n";

    if (filename.find("..") != std::string::npos || filename.find("/") != std::string::npos) {
        send_response(client_fd, "ERROR Invalid filename");
        return;
    }

    std::vector<char> file_buffer(filesize);
    if (!read_exact(client_fd, file_buffer.data(), filesize)) {
        send_response(client_fd, "ERROR Connection lost during upload");
        return;
    }

    // Select nodes and reserve bytes under the lock so concurrent uploads
    // see updated load and route to different nodes.
    int    primary_node_id = -1;
    int    replica_node_id = -1;
    bool   is_overwrite    = false;
    int    old_primary     = -1;
    int    old_replica     = -1;
    size_t old_size        = 0;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        if (pending_uploads.count(filename)) {
            send_response(client_fd, "ERROR Upload already in progress for this file");
            return;
        }
        auto existing = file_metadata_map.find(filename);
        if (existing != file_metadata_map.end()) {
            is_overwrite = true;
            old_primary  = existing->second.node_id;
            old_replica  = existing->second.replica_node_id;
            old_size     = existing->second.filesize;
        }

        // Pick the REPLICATION_FACTOR least-loaded *live* nodes, skipping any
        // flagged dead by a prior failed operation.
        // NOTE: update_node_size erases and reinserts, invalidating iterators,
        // so we re-iterate from begin() after each update.
        for (const auto& entry : node_min_heap) {
            if (!dead_nodes.count(entry.second)) {
                primary_node_id = entry.second;
                break;
            }
        }
        if (primary_node_id < 0) {
            send_response(client_fd, "ERROR No storage nodes available");
            return;
        }
        update_node_size(primary_node_id, (off_t)filesize);

        if (REPLICATION_FACTOR >= 2) {
            for (const auto& entry : node_min_heap) {
                if (entry.second != primary_node_id && !dead_nodes.count(entry.second)) {
                    replica_node_id = entry.second;
                    update_node_size(replica_node_id, (off_t)filesize);
                    break;
                }
            }
        }
        pending_uploads.insert(filename);
    }

    // Upload to primary — if this fails nothing was committed, undo and bail.
    if (!store_on_node(primary_node_id, filename, file_buffer)) {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        dead_nodes.insert(primary_node_id);
        update_node_size(primary_node_id, -(off_t)filesize);
        if (replica_node_id >= 0) {
            update_node_size(replica_node_id, -(off_t)filesize);
        }
        pending_uploads.erase(filename);
        send_response(client_fd, "ERROR Storage node failed to store file");
        return;
    }

    // Upload to replica (best-effort) — failure means degraded mode, not abort.
    if (replica_node_id >= 0 && !store_on_node(replica_node_id, filename, file_buffer)) {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        dead_nodes.insert(replica_node_id);
        update_node_size(replica_node_id, -(off_t)filesize);
        replica_node_id = -1;
        std::cerr << "[warn] replica upload failed for " << filename
                  << " — storing primary only (degraded)\n";
    }

    // Commit: debit old nodes' sizes, update metadata, collect displaced old
    // nodes that need their copy deleted (those not reused in the new placement).
    std::vector<int> orphaned_nodes;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        if (is_overwrite) {
            for (int old_node : {old_primary, old_replica}) {
                if (old_node < 0) { continue; }
                update_node_size(old_node, -(off_t)old_size);
                if (old_node != primary_node_id && old_node != replica_node_id) {
                    orphaned_nodes.push_back(old_node);
                }
            }
        }
        file_metadata_map[filename] = {primary_node_id, replica_node_id, filesize};
        pending_uploads.erase(filename);
    }

    // Best-effort cleanup of displaced copies from old nodes.
    for (int orphan : orphaned_nodes) {
        delete_from_node(orphan, filename);
    }

    send_response(client_fd, is_overwrite ? "OK File updated successfully"
                                          : "OK File uploaded successfully");

    std::cout << "[Client " << client_id << "] Upload complete: " << filename
              << " -> node " << primary_node_id;
    if (replica_node_id >= 0) {
        std::cout << " + replica on node " << replica_node_id;
    } else {
        std::cout << " (degraded — no replica)";
    }
    std::cout << "\n";
}

// Tries to retrieve a file from one node and stream it to the client.
// Returns true on success. On failure the connection is closed and nothing
// has been sent to client_fd yet.
static bool retrieve_from_node(int client_fd, int node_id, const std::string& filename) {
    int node_fd = connect_to_node(node_id);
    if (node_fd < 0) {
        return false;
    }

    std::string cmd = "NODE_RETRIEVE " + filename + "\n";
    send_all(node_fd, cmd.c_str(), cmd.size());

    std::string node_response;
    if (recv_line(node_fd, node_response) != 0 || node_response.substr(0, 4) != "FILE") {
        close(node_fd);
        return false;
    }

    std::istringstream ss(node_response);
    std::string tag, recv_filename;
    size_t recv_filesize = 0;
    ss >> tag >> recv_filename >> recv_filesize;

    std::vector<char> file_buffer(recv_filesize);
    if (recv_file_data(node_fd, recv_filesize, file_buffer.data()) != 0) {
        close(node_fd);
        return false;
    }
    close(node_fd);

    send_response(client_fd, "OK " + std::to_string(recv_filesize));
    send(client_fd, file_buffer.data(), recv_filesize, 0);
    return true;
}

// Looks up the file, tries the primary node, and falls back to the replica
// if the primary is unreachable or returns an error (node may have crashed).
void handle_download(int client_fd, int client_id, const std::string& filename) {
    std::cout << "[Client " << client_id << "] DOWNLOAD " << filename << "\n";

    int primary_node_id, replica_node_id;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        auto entry = file_metadata_map.find(filename);
        if (entry == file_metadata_map.end()) {
            send_response(client_fd, "ERROR File not found");
            return;
        }
        primary_node_id = entry->second.node_id;
        replica_node_id = entry->second.replica_node_id;
    }

    if (retrieve_from_node(client_fd, primary_node_id, filename)) {
        std::cout << "[Client " << client_id << "] Download complete: " << filename
                  << " from node " << primary_node_id << "\n";
        return;
    }

    if (replica_node_id >= 0) {
        std::cout << "[Client " << client_id << "] Primary node " << primary_node_id
                  << " unreachable, trying replica node " << replica_node_id << "\n";
        if (retrieve_from_node(client_fd, replica_node_id, filename)) {
            std::cout << "[Client " << client_id << "] Download complete: " << filename
                      << " from replica node " << replica_node_id << "\n";
            return;
        }
    }

    send_response(client_fd, "ERROR File unavailable — all replicas unreachable");
}

// Deletes from all nodes holding the file (primary + replica).
// Skips nodes already flagged dead to avoid the 5-second timeout per dead node.
// Marks newly unresponsive nodes dead. Succeeds if at least one node confirms.
void handle_delete(int client_fd, int client_id, const std::string& filename) {
    std::cout << "[Client " << client_id << "] DELETE " << filename << "\n";

    int    primary_node_id, replica_node_id;
    size_t filesize;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        auto entry = file_metadata_map.find(filename);
        if (entry == file_metadata_map.end()) {
            send_response(client_fd, "ERROR File not found");
            return;
        }
        primary_node_id = entry->second.node_id;
        replica_node_id = entry->second.replica_node_id;
        filesize        = entry->second.filesize;
    }

    // Returns true if the node confirmed deletion.
    // Skips (returns false) if already dead; marks dead on new failure.
    auto try_delete_node = [&](int node_id) -> bool {
        {
            std::lock_guard<std::mutex> lock(metadata_mutex);
            if (dead_nodes.count(node_id)) { return false; }
        }
        int node_fd = connect_to_node(node_id);
        if (node_fd < 0) {
            std::lock_guard<std::mutex> lock(metadata_mutex);
            dead_nodes.insert(node_id);
            return false;
        }
        std::string cmd = "NODE_DELETE " + filename + "\n";
        send_all(node_fd, cmd.c_str(), cmd.size());
        std::string resp;
        bool ok = (recv_line(node_fd, resp) == 0 && resp.substr(0, 2) == "OK");
        close(node_fd);
        if (!ok) {
            std::lock_guard<std::mutex> lock(metadata_mutex);
            dead_nodes.insert(node_id);
        }
        return ok;
    };

    bool primary_deleted = try_delete_node(primary_node_id);
    bool replica_deleted = (replica_node_id >= 0) && try_delete_node(replica_node_id);

    if (!primary_deleted && !replica_deleted) {
        send_response(client_fd, "ERROR Failed to delete file from any node");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        if (primary_deleted) { update_node_size(primary_node_id, -(off_t)filesize); }
        if (replica_deleted)  { update_node_size(replica_node_id, -(off_t)filesize); }
        file_metadata_map.erase(filename);
    }

    send_response(client_fd, "OK File deleted");
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

// Background thread: every 10 seconds ping each dead node via NODE_STATUS.
// If a node responds, revive it: remove from dead_nodes and sync its byte
// count back into the heap so future uploads can route to it again.
static void health_check_loop() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(10));

        std::vector<int> to_check;
        {
            std::lock_guard<std::mutex> lock(metadata_mutex);
            to_check.assign(dead_nodes.begin(), dead_nodes.end());
        }

        for (int node_id : to_check) {
            int node_fd = connect_to_node(node_id);
            if (node_fd < 0) { continue; }

            std::string cmd = "NODE_STATUS\n";
            send_all(node_fd, cmd.c_str(), cmd.size());
            std::string resp;
            if (recv_line(node_fd, resp) != 0) {
                close(node_fd);
                continue;
            }
            close(node_fd);

            std::istringstream ss(resp);
            std::string tag;
            int file_count = 0;
            long long total_bytes = 0;
            ss >> tag >> file_count >> total_bytes;
            if (tag != "STATUS") { continue; }

            std::lock_guard<std::mutex> lock(metadata_mutex);
            dead_nodes.erase(node_id);
            node_min_heap.erase({node_bytes[node_id], node_id});
            node_bytes[node_id] = (off_t)total_bytes;
            node_min_heap.insert({node_bytes[node_id], node_id});
            std::cout << "[health] node " << node_id << " revived ("
                      << total_bytes << " bytes stored)\n";
        }
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

    // Rebuild the routing map from whatever files are already on the nodes,
    // so files from previous sessions are immediately accessible after restart.
    rebuild_metadata_from_nodes();

    // Start background thread that pings dead nodes every 10s and revives them
    // when they respond again (e.g. after a transient fault clears).
    std::thread(health_check_loop).detach();

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
