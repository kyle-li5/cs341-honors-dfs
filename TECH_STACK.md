# Tech Stack & Tools

This document outlines the technologies, libraries, and tools used in this Distributed File System (DFS) project.

---

## Languages

| Language | Standard | Purpose |
|----------|----------|---------|
| C++ | C++17 / C++20 | Core server and node implementation |
| Python 3 | - | Client utilities and integration testing |

---

## C++ Libraries

### Standard Library (STL)
- `<iostream>`, `<string>`, `<sstream>` — I/O and string handling
- `<fstream>` — File stream operations
- `<vector>` — Dynamic arrays (thread pool, file path collection)
- `<queue>` — Queue data structure
- `<filesystem>` (C++17) — Cross-platform file/directory operations (`create_directory`, `exists`, `remove`, `file_size`, `recursive_directory_iterator`)
- `<algorithm>` — Utilities like `std::transform`
- `<thread>` — Multi-threading support
- `<atomic>` — Thread-safe counters (`std::atomic<int>`)

### C Standard Library
- `<stdlib.h>`, `<stdio.h>`, `<string.h>` — General C utilities
- `<limits.h>` — `PATH_MAX` for buffer sizing
- `<assert.h>` — Assertion macros for unit tests

### POSIX / Unix System Headers
- `<sys/socket.h>`, `<netinet/in.h>` — TCP socket operations
- `<sys/stat.h>` — File status information
- `<unistd.h>` — POSIX API (`read`, `write`, `close`)
- `<fcntl.h>` — File control (`O_RDONLY`, `O_WRONLY`, `O_CREAT`)

---

## Python Libraries

All from the Python standard library — no third-party packages required:
- `socket` — TCP client connections
- `os` — File and path operations
- `sys` — Command-line arguments

---

## Networking

- **Protocol:** TCP/IP over IPv4 (`AF_INET`, `SOCK_STREAM`)
- **Port:** 9000 (default)
- **Socket API:** `socket()`, `bind()`, `listen()`, `accept()`, `send()`, `recv()`, `setsockopt()`, `close()`
- **Custom application protocol:** Text-based commands — `LIST`, `UPLOAD`, `DOWNLOAD`, `DELETE`, `QUIT` (documented in `PROTOCOL.md`)
- **`SO_REUSEADDR`:** Allows port reuse on server restart

---

## Concurrency

- `std::thread` — One thread per client connection
- `std::atomic<int>` — Thread-safe active client counter
- Thread detach — Automatic cleanup after client disconnects
- `-pthread` compiler flag — Links POSIX threads

---

## Build Tools

| Tool | Role |
|------|------|
| **Makefile** | Build automation |
| **clang++** | Primary C++ compiler |
| **g++** | Alternative compiler (referenced in comments) |
| `-std=c++20` | Standard used for node/test compilation |
| `-Wall` | All compiler warnings enabled |

### Makefile targets
- `node-test` — Compiles `nodeinternal.cpp` + `test.cpp`
- `clean` — Removes `a.out`

---

## Testing

| Tool | Type | File |
|------|------|------|
| `assert()` (C stdlib) | Unit tests | `node/test.cpp` |
| Python test client | Integration tests | `testing/test_client.py` |
| `netcat (nc)` | Manual protocol testing | Referenced in `PROTOCOL.md` |

---

## Security Measures

- **Directory traversal prevention:** Filenames containing `..` or `/` are rejected before any file operation
- **Input validation:** Command parsing validates argument counts before processing
- **POSIX file permissions:** Standard permission flags on file creation

---

## Runtime Environment

- **OS:** Any POSIX-compliant system (Linux, macOS)
- **Network:** IPv4 localhost (configurable)
- **Storage layout:** `./node-data/<node_id>/storage/` (node component), `./storage/` (TCP server)
