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

// 1 MB chunks: at our 3-node cluster size finer granularity (e.g., 64 KB)
// gives no real load-balancing benefit and runs into macOS TCP behavior
// (receiver-side delayed-ACK, no TCP_QUICKACK) that destabilizes the
// pipelined chain at high chunk counts. 1 MB strikes the right balance —
// 100 MB upload finishes in ~2 seconds across the live replicas.
const size_t CHUNK_SIZE = 1024*1024;

// How many nodes each chunk is stored on. 1 = no redundancy, 2 = survive
// 1 node failure, 3 = survive 2 failures, etc. Auto-capped at NUM_NODES if
// you set it higher. The coordinator picks the N least-loaded live nodes
// from the min-heap per upload and pipelines the chunk through them.
static const int NUM_REDUNDANCIES = 2;