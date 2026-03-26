#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <vector>


// g++ server.cpp -o server
// ./server

// tested with "nc localhost 9000"

// listens for request from client and directs how to distribute chunks of file

struct data_info {
    int chunk_id;
    int size;
};

void handle_request(std::vector<int> data_ports, int node_index, int client_fd) {
    std::cout << "Received manager info." << std::endl;
    data_info info;
    if (recv(client_fd, &info, sizeof(data_info), 0) > 0) {
        int assigned_port = data_ports[node_index];
        node_index = (node_index+1) % data_ports.size();
        std::cout << "Assigning chunk " << info.chunk_id << "to node " << assigned_port << std::endl;
        send(client_fd, &assigned_port, sizeof(assigned_port), 0);
    }
    close(client_fd);
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    std::vector<int> data_ports = {9000, 9001, 9002};
    int node_index = 0;

    // Allow port reuse (avoids "address already in use" errors)
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;  // Accept from any IP
    address.sin_port = htons(8080);        // Port 8080

    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 5);

    std::cout << "Manager initialized. Server listening on port 8080...\n";

    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd >= 0) {
            handle_request(data_ports, node_index, client_fd);
        }
    }
    close(server_fd);
    return 0;
}