#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fstream>
#include <arpa/inet.h>
#include <vector>

struct data_info {
    int chunk_id;
    int size;
};

const int CHUNK_SIZE = 64*1024;

int request_manager(int chunk_id) {
    int manager_fd = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_port = htons(8080); // Manager always on 8080
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

    if (connect(manager_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Error: Cannot connect to manager." << std::endl;
        close(manager_fd);
        return -1;
    }

    data_info info;
    info.chunk_id = chunk_id;
    info.size = 0;

    send(manager_fd, &info, sizeof(info), 0);
    int assigned_port = -1;
    recv(manager_fd, &assigned_port, sizeof(assigned_port), 0);

    close(manager_fd);
    return assigned_port;
}

void upload_file(std::ifstream& file) {
    std::cout << "Connecting to node." << std::endl;

    std::vector<char> buffer(CHUNK_SIZE);
    int chunk_id = 0;
    
    file.read(buffer.data(), CHUNK_SIZE);
    while (file.gcount() > 0) {
        int assigned_port = request_manager(chunk_id);
        if (assigned_port == -1) {
            std::cerr << "Error: Cannot reach manager. Operation aborted" << std::endl;
            break;
        }
        
        int node_fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_port = htons(assigned_port);        // Port 9000
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

        if (connect(node_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            std::cerr << "Error: Cannot connect to node on port " << assigned_port << std::endl;
            close(node_fd);
            return;
        }
        
        std::cout << "Connected to node. Starting upload." << std::endl;

        chunk_id++;
        close(node_fd);
        file.read(buffer.data(), CHUNK_SIZE);
    }
    std::cout << "Upload complete. Total chunks sent: " << chunk_id << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cout << "Usage:  " << argv[0] << " <filepath>" << std::endl;
        return 1;
    }
    std::ifstream file(argv[1], std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error: could not open file" << argv[1] << std::endl;
        return 1;
    }
    upload_file(file);
    file.close();
    return 0;
}