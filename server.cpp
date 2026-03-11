#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>


// g++ server.cpp -o server
// ./server

// tested with "nc localhost 9000"

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    // Allow port reuse (avoids "address already in use" errors)
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;  // Accept from any IP
    address.sin_port = htons(9000);        // Port 9000

    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 5);

    std::cout << "Server listening on port 9000...\n";

    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);

        char buffer[1024] = {0};
        recv(client_fd, buffer, sizeof(buffer), 0);
        std::cout << "Received: " << buffer << "\n";

        // Echo back
        send(client_fd, buffer, strlen(buffer), 0);
        close(client_fd);
    }
}