#pragma once

// Port server.cpp listens on for client connections
static const int COORDINATOR_PORT = 9000;

// node_server listens on NODE_BASE_PORT + node_id (9001, 9002, 9003)
static const int NODE_BASE_PORT = 9001;

// Number of storage nodes (can be any # just chose 3 to start)
static const int NUM_NODES = 3;

// Read/write buffer size for TCP transfers
static const int BUFFER_SIZE = 4096;

// Maximum length of a filename
static const int MAX_FILENAME = 256;
