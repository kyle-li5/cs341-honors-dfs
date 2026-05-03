#pragma once

// node_server listens on NODE_BASE_PORT + node_id (9001, 9002, 9003)
static const int NODE_BASE_PORT = 9001;

// Number of storage nodes
static const int NUM_NODES = 3;

// 1 MB chunks: finer granularity (e.g. 64 KB) runs into macOS receiver-side
// delayed-ACK behaviour that destabilises uploads at scale. 1 MB keeps chunk
// count manageable and upload time around 2 s for 100 MB.
const size_t CHUNK_SIZE = 1024 * 1024;

// How many nodes each chunk is stored on. 1 = no redundancy, 2 = survive
// 1 node failure, 3 = survive 2 failures. Auto-capped at live node count.
static const int NUM_REDUNDANCIES = 2;