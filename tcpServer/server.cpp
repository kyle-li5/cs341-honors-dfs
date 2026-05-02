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
#include <map>
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

// define constants for chunking
const size_t CHUNK_SIZE = 1024*1024; // 1 MB
const int NUM_REDUNDANCIES = 3;

struct ChunkMetadata {
    int chunk_index;
    size_t size;
    std::vector<int> redundancy_ids;
};

struct FileMetadata {
    size_t total_size;
    std::vector<ChunkMetadata> chunks;
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

    // uses an intermediate map to map filenames to ordered map of chunks, which automatically sorts by chunk_index
    std::unordered_map<std::string, std::map<int, ChunkMetadata>> temp_map;

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
                size_t chunk_pos = filename.rfind("_chunk");
                if (chunk_pos != std::string::npos) {
                    std::string original_filename = filename.substr(0, chunk_pos);
                    int chunk_index = std::stoi(filename.substr(chunk_pos + 6));

                    temp_map[original_filename][chunk_index].chunk_index = chunk_index;
                    temp_map[original_filename][chunk_index].size = filesize;
                    temp_map[original_filename][chunk_index].redundancy_ids.push_back(node_id);
                }

                node_bytes[node_id] += (off_t)filesize;
            }
        }
        close(node_fd);
    }

    for (auto const& [filename, chunks_map] : temp_map) {
        FileMetadata file_metadata;
        file_metadata.total_size = 0;

        for (auto const& [index, chunk_metadata] : chunks_map) {
            file_metadata.chunks.push_back(chunk_metadata);
            file_metadata.total_size += chunk_metadata.size;
        }

        file_metadata_map[filename] = file_metadata;
        std::cout << "[startup] recovered " << filename
                    << " (" << file_metadata.total_size << "bytes)\n";
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
                                + std::to_string(entry.second.total_size) + "\n";
        send_all(client_fd, file_line.c_str(), file_line.size());
    }
}

// Receives the file from the client in CHUNK_SIZE pieces and forwards each chunk
// to NUM_REDUNDANCIES nodes picked from the min-heap. Different chunks of the
// same file can land on different node sets, so a single file's data spreads
// across the cluster instead of piling onto two nodes.

void handle_upload(int client_fd, int client_id, const std::string& filename, size_t filesize) {
    std::cout << "[Client " << client_id << "] UPLOAD " << filename
              << " (" << filesize << " bytes)\n";

    // Validate filename to prevent directory traversal attacks
    if (filename.find("..") != std::string::npos || filename.find("/") != std::string::npos) {
        send_response(client_fd, "ERROR Invalid filename");
        return;
    }

    // Pick the target node and *reserve* its bytes immediately so concurrent
    // uploads see the updated load and don't all stampede onto the same node.
    // Also block concurrent uploads of the same filename to prevent orphaned
    // node copies.
    bool is_overwrite = false;
    FileMetadata old_file_metadata;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        if (pending_uploads.count(filename)) {
            send_response(client_fd, "ERROR Upload already in progress for this file");
            return;
        }
        auto existing_entry = file_metadata_map.find(filename);
        if (existing_entry != file_metadata_map.end()) {
            is_overwrite = true;
            old_file_metadata = existing_entry->second;
        }
        pending_uploads.insert(filename);
    }

    FileMetadata file_metadata;
    file_metadata.total_size = filesize;
    size_t bytes_read = 0;
    int chunk_index = 0;

    while (bytes_read < filesize) {
        size_t curr_chunk_size = std::min(CHUNK_SIZE, filesize - bytes_read);

        // Receive file bytes from the client into a buffer
        std::vector<char> chunk_buff(curr_chunk_size);
        if (!read_exact(client_fd, chunk_buff.data(), curr_chunk_size)) {
            std::lock_guard<std::mutex> lock(metadata_mutex);
            pending_uploads.erase(filename);
            send_response(client_fd, "ERROR Connection lost during upload");
            return;
        }

        // Pick the NUM_REDUNDANCIES least-loaded *live* nodes for this chunk,
        // skipping anything in dead_nodes so we don't waste 5s on a known-dead
        // socket. Bytes are reserved up front so concurrent uploads see the
        // updated load.
        std::vector<int> chosen_nodes;
        {
            std::lock_guard<std::mutex> lock(metadata_mutex);
            for (auto it = node_min_heap.begin();
                 it != node_min_heap.end() && (int)chosen_nodes.size() < NUM_REDUNDANCIES;
                 ++it) {
                if (dead_nodes.count(it->second)) { continue; }
                chosen_nodes.push_back(it->second);
            }
            for (int node_id : chosen_nodes) {
                update_node_size(node_id, (off_t)curr_chunk_size);
            }
        }

        if (chosen_nodes.empty()) {
            std::lock_guard<std::mutex> lock(metadata_mutex);
            pending_uploads.erase(filename);
            send_response(client_fd, "ERROR No live storage nodes available");
            return;
        }

        // check if there are any successful connections to nodes, first successful connection is head node for redundancies
        int node_connection_fd = -1;
        size_t head_index = 0;

        for (; head_index < chosen_nodes.size(); ++head_index) {
            node_connection_fd = connect_to_node(chosen_nodes[head_index]);

            if (node_connection_fd >= 0) {
                break;
            }
            std::cerr << "ERROR Failed to connect to node " + std::to_string(chosen_nodes[head_index]) + ", attempting next node...\n";
            {
                std::lock_guard<std::mutex> lock(metadata_mutex);
                update_node_size(chosen_nodes[head_index], -(off_t)curr_chunk_size); // undo reservation
                dead_nodes.insert(chosen_nodes[head_index]);
            }
        }

        if (node_connection_fd < 0) {
            std::lock_guard<std::mutex> lock(metadata_mutex);
            pending_uploads.erase(filename);
            send_response(client_fd, "ERROR Cannot upload file, all chosen nodes are offline");
            return;
        }

        std::string chunk_filename = filename + "_chunk" + std::to_string(chunk_index);
        std::string store_command = "NODE_STORE " + chunk_filename + " " + std::to_string(curr_chunk_size);

        // logic for pipelining redundancy logic so it pushes to all chosen nodes at the same time
        int remaining_redundancy_nodes = chosen_nodes.size() - 1 - head_index;
        if (remaining_redundancy_nodes > 0) {
            store_command += " " + std::to_string(remaining_redundancy_nodes);
            for (size_t i = head_index + 1; i < chosen_nodes.size(); ++i) {
                store_command += " " + std::to_string(chosen_nodes[i]);
            }
        }
        store_command += "\n";

        // send to head node
        send_all(node_connection_fd, store_command.c_str(), store_command.size());
        send_all(node_connection_fd, chunk_buff.data(), curr_chunk_size);

        std::string node_response;
        recv_line(node_connection_fd, node_response);
        close(node_connection_fd);

        // only record nodes that were successfully connected to
        std::vector<int> successful_redundacies(chosen_nodes.begin() + head_index, chosen_nodes.end());

        ChunkMetadata chunk_metadata;
        chunk_metadata.chunk_index = chunk_index;
        chunk_metadata.size = curr_chunk_size;
        chunk_metadata.redundancy_ids = successful_redundacies;
        file_metadata.chunks.push_back(chunk_metadata);

        bytes_read += curr_chunk_size;
        chunk_index++;
    }

    // If this was an overwrite, release any old chunk copies that are now
    // orphaned. A copy is "orphaned" only if the new placement does NOT also
    // hold (chunk_index, node_id) — otherwise we'd delete the freshly-written
    // new chunk on a node that happened to be reused (very common when
    // NUM_REDUNDANCIES == NUM_NODES, since every chunk lands everywhere).
    if (is_overwrite) {
        std::set<std::pair<int,int>> new_placements;
        for (const auto& new_chunk : file_metadata.chunks) {
            for (int n : new_chunk.redundancy_ids) {
                new_placements.insert({new_chunk.chunk_index, n});
            }
        }

        for (const auto& old_chunk : old_file_metadata.chunks) {
            std::string old_chunk_name = filename + "_chunk" + std::to_string(old_chunk.chunk_index);

            for (int old_node_id : old_chunk.redundancy_ids) {
                if (new_placements.count({old_chunk.chunk_index, old_node_id})) {
                    continue; // new chunk already overwrote this copy in place
                }

                bool orphan_cleaned = false;
                int  old_node_fd = connect_to_node(old_node_id);

                if (old_node_fd >= 0) {
                    std::string delete_cmd = "NODE_DELETE " + old_chunk_name + "\n";
                    send_all(old_node_fd, delete_cmd.c_str(), delete_cmd.size());

                    std::string old_response;
                    if (recv_line(old_node_fd, old_response) == 0
                            && old_response.substr(0, 2) == "OK") {
                        orphan_cleaned = true;
                    }
                    close(old_node_fd);
                }

                if (!orphan_cleaned) {
                    std::cerr << "[warn] orphan copy of " << old_chunk_name
                            << " left on node " << old_node_id << "\n";
                }
            }
        }
    }

    // commit file metadata to map
    {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        if (is_overwrite) {
            // Release the old file's bytes from whichever node held it.
            // If old_node == target, this nets against part of our reservation.
            for (const auto& old_chunk : old_file_metadata.chunks) {
                for (int old_node_id : old_chunk.redundancy_ids) {
                    update_node_size(old_node_id, -(off_t)old_chunk.size); // undo reservation
                }
            }
        }
        file_metadata_map[filename] = file_metadata;
        pending_uploads.erase(filename);
    }

    if (is_overwrite) {
        send_response(client_fd, "OK File updated successfully");
    } else {
        send_response(client_fd, "OK File uploaded successfully");
    }

    std::cout << "[Client " << client_id << "] Upload complete: " << filename << "\n";

}

// Looks up which node has the file, retrieves it, and streams it to the client.
void handle_download(int client_fd, int client_id, const std::string& filename) {
    std::cout << "[Client " << client_id << "] DOWNLOAD " << filename << "\n";

    FileMetadata file_metadata;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        auto entry = file_metadata_map.find(filename);
        if (entry == file_metadata_map.end()) {
            send_response(client_fd, "ERROR File not found");
            return;
        }
        file_metadata = entry->second;
    }

    // handle if missing any chunk of file -> return error immediately
    // loop through file_metadata chunks, they were put in order by the temp map used earlier
    int expected_index = 0;
    for (const auto& chunk : file_metadata.chunks) {
        if (chunk.chunk_index != expected_index) {
            send_response(client_fd, "ERROR File corrupted: missing chunk " + std::to_string(expected_index));
            return;
        }
        if (chunk.redundancy_ids.empty()) {
            send_response(client_fd, "ERROR No nodes found for chunk " + std::to_string(expected_index));
            return;
        }
        expected_index++;
    }

    // send client total size
    send_response(client_fd, "OK " + std::to_string(file_metadata.total_size));

    for (const auto& chunk : file_metadata.chunks) {
        std::string chunk_filename = filename + "_chunk" + std::to_string(chunk.chunk_index);
        bool chunk_retrieved = false;

        for (int target_node_id : chunk.redundancy_ids) {
            {
                std::lock_guard<std::mutex> lock(metadata_mutex);
                if (dead_nodes.count(target_node_id)) { continue; }
            }
            int node_connection_fd = connect_to_node(target_node_id);
            if (node_connection_fd < 0) {
                std::lock_guard<std::mutex> lock(metadata_mutex);
                dead_nodes.insert(target_node_id);
                continue; // node offline, try next node
            }

            std::string retrieve_command = "NODE_RETRIEVE " + chunk_filename + "\n";
            send_all(node_connection_fd, retrieve_command.c_str(), retrieve_command.size());

            // Node responds with "FILE <filename> <filesize>\n<bytes>"
            std::string node_response;
            if (recv_line(node_connection_fd, node_response) != 0) {
                close(node_connection_fd);
                std::lock_guard<std::mutex> lock(metadata_mutex);
                dead_nodes.insert(target_node_id);
                continue; // connection failed during read, try next node
            }

            if (node_response.substr(0, 4) != "FILE") {
                close(node_connection_fd);
                continue; // node error, try next node
            }

            // Parse "FILE <filename> <filesize>"
            std::istringstream response_stream(node_response);
            std::string tag, received_filename;
            size_t received_filesize;
            response_stream >> tag >> received_filename >> received_filesize;

            std::vector<char> file_buffer(received_filesize);
            if (recv_file_data(node_connection_fd, received_filesize, file_buffer.data()) != 0) {
                close(node_connection_fd);
                continue; // cannot read file from node, try next node
            }
            close(node_connection_fd);

            // Send OK with filesize so client knows how many bytes to read
            send(client_fd, file_buffer.data(), received_filesize, 0);

            chunk_retrieved = true;
            break; // successfully retrieved current chunk
        }

        // Chunk irrecoverable. We've already sent "OK total_size" so we can't
        // emit an ERROR line into the byte stream — that would corrupt the
        // file the client is reading. Just log; the client will see a short
        // read and surface that itself.
        if (!chunk_retrieved) {
            std::cerr << "ERROR Failed to retrieve chunk " << chunk.chunk_index << " for file " << filename << ", all nodes are down\n";
        }
    }

    std::cout << "[Client " << client_id << "] Download complete: " << filename << "\n";
}

// Tells the node holding the file to delete it, then removes it from the metadata map.
void handle_delete(int client_fd, int client_id, const std::string& filename) {
    std::cout << "[Client " << client_id << "] DELETE " << filename << "\n";

    FileMetadata file_metadata;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        auto entry = file_metadata_map.find(filename);
        if (entry == file_metadata_map.end()) {
            send_response(client_fd, "ERROR File not found");
            return;
        }
        file_metadata = entry->second;
    }

    // track if everything has been deleted
    bool all_deleted = true;
    for (const auto& chunk : file_metadata.chunks) {
        std::string chunk_filename = filename + "_chunk" + std::to_string(chunk.chunk_index);

        for (int target_node_id : chunk.redundancy_ids) {
            {
                std::lock_guard<std::mutex> lock(metadata_mutex);
                if (dead_nodes.count(target_node_id)) {
                    all_deleted = false;
                    continue;
                }
            }

            int node_connection_fd = connect_to_node(target_node_id);

            if (node_connection_fd < 0) {
                std::lock_guard<std::mutex> lock(metadata_mutex);
                dead_nodes.insert(target_node_id);
                all_deleted = false;
                // send_response(client_fd, "ERROR Could not reach storage node");
                continue;
            }

            std::string delete_command = "NODE_DELETE " + chunk_filename + "\n";
            send_all(node_connection_fd, delete_command.c_str(), delete_command.size());

            std::string node_response;
            recv_line(node_connection_fd, node_response);
            close(node_connection_fd);

            if (node_response.substr(0, 2) == "OK") {
                std::lock_guard<std::mutex> lock(metadata_mutex);
                update_node_size(target_node_id, -(off_t)chunk.size);
            } else {
                std::lock_guard<std::mutex> lock(metadata_mutex);
                dead_nodes.insert(target_node_id);
                all_deleted = false;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        file_metadata_map.erase(filename);
    }

    if (all_deleted) {
        send_response(client_fd, "OK File deleted");
    } else {
        send_response(client_fd, "ERROR Failed to completely delete file. Some fragments may still remain");
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
