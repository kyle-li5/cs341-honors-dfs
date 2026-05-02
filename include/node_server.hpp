#pragma once

#include <thread>
#include <mutex>
#include <vector>
#include "nodeinternal.hpp"

// Shared mutex for serializing console output across threads
extern std::mutex print_mutex;

// NodeInternal (node/nodeinternal.hpp) handles local file storage but has no
// networking - it can only be used directly by code running on the same machine.
// NodeServer wraps NodeInternal with a TCP listener so server.cpp can send
// commands to each storage node over the network. Each NodeServer runs in its
// own thread and listens on its own port (NODE_BASE_PORT + node_id).
class NodeServer {
public:
    explicit NodeServer(int node_id);
    ~NodeServer();

    // Spawns a background thread and starts listening for incoming connections.
    void start();

    // Blocking accept loop that handles one connection at a time.
    // Called internally by start() on the background thread.
    void run();

private:
    // Reads one command from the connection and calls the right handler.
    void handle_connection(int connection_fd);

    // Receives file bytes from the connection and stores them via NodeInternal.
    // forward_fd / forward_target_node carry the persistent pipeline socket
    // across chunks within the same upstream connection; handle_store reuses
    // the existing socket when the redundancy chain hasn't changed and resets
    // both to -1 on connect failure or downstream ack timeout. handle_connection
    // owns the fd lifecycle and closes it when the upstream peer disconnects.
    void handle_store(int connection_fd, const std::string &filename, size_t filesize,
                      std::vector<int>& redundant_nodes,
                      int &forward_fd, int &forward_target_node);

    // Reads a stored file via NodeInternal and sends its bytes back over the connection.
    void handle_retrieve(int connection_fd, const std::string &filename);

    // Deletes a stored file via NodeInternal and sends back OK or ERROR.
    void handle_delete(int connection_fd, const std::string &filename);

    // Lists all files stored on this node via NodeInternal and sends them back.
    void handle_list(int connection_fd);

    // Sends back the number of files and total storage used on this node.
    void handle_status(int connection_fd);

    int node_id;
    int listen_fd;

    // The NodeInternal instance that handles actual file storage for this node.
    NodeInternal local_storage;

    // Prevents concurrent access to local_storage since multiple connections
    // could arrive at the same time.
    std::mutex storage_mutex;

    std::thread background_thread;
};
