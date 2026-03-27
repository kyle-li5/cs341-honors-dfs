CXX      = g++
# -I. so that tcpServer/server.cpp can find headers at the repo root
CXXFLAGS = -std=c++17 -Wall -pthread -I.

# server binary - runs on the Pi, starts the server coordinator and all storage nodes
SERVER_SOURCES = tcpServer/server.cpp node_server.cpp tcp_helpers.cpp node/nodeinternal.cpp

# client binary - runs on your laptop, connects to the server
CLIENT_SOURCES = client.cpp tcp_helpers.cpp

all: server client node-test

server: $(SERVER_SOURCES)
	$(CXX) $(CXXFLAGS) -o server $(SERVER_SOURCES)

client: $(CLIENT_SOURCES)
	$(CXX) $(CXXFLAGS) -o client $(CLIENT_SOURCES)

node-test:
	clang++ -Wall ./node/nodeinternal.cpp ./node/nodeinternal.hpp ./node/test.cpp -std=c++20

clean:
	rm -f server client a.out

.PHONY: all clean node-test