#include "tcp_helpers.hpp"

#include <unistd.h>
#include <string>

// TCP may only send part of the buffer in one write() call, so we loop,
// advancing through buf each time until everything has been sent.
int send_all(int connection_fd, const char *buf, size_t num_bytes) {
    size_t total_bytes_sent = 0;

    while (total_bytes_sent < num_bytes) {
        ssize_t bytes_sent = write(connection_fd,
                                   buf + total_bytes_sent,
                                   num_bytes - total_bytes_sent);
        if (bytes_sent <= 0) {
            return -1;
        }
        total_bytes_sent += (size_t)bytes_sent;
    }

    return 0;
}

// Read one character at a time until we hit a newline. This lets us receive
// variable-length command strings without knowing their length upfront.
int recv_line(int connection_fd, std::string &out_line) {
    out_line.clear();
    char current_char;

    while (true) {
        ssize_t bytes_read = read(connection_fd, &current_char, 1);

        if (bytes_read <= 0) {
            return -1;
        }

        if (current_char == '\n') {
            return 0;
        }

        out_line += current_char;
    }
}

// Send the header first so the receiver knows the filename and how many bytes
// to expect, then stream the file contents in chunks to avoid loading the
// entire file into memory at once.
int send_file_data(int connection_fd, const char *filename, off_t filesize, int source_file_fd) {
    std::string header = "FILE ";
    header += filename;
    header += " ";
    header += std::to_string((long long)filesize);
    header += "\n";

    if (send_all(connection_fd, header.c_str(), header.size()) != 0) {
        return -1;
    }

    // Stream file contents in 64 KB blocks. Smaller buffers (e.g. 4 KB)
    // turn each NODE_RETRIEVE response into hundreds of small TCP sends,
    // which interact poorly with macOS receiver-side delayed-ACK and slow
    // downloads to ~5 MB/s. 64 KB lets us push at line rate without
    // bloating per-call memory.
    char chunk[64 * 1024];
    off_t bytes_remaining = filesize;

    while (bytes_remaining > 0) {
        ssize_t bytes_to_read;

        if (bytes_remaining < (off_t)sizeof(chunk)) {
            bytes_to_read = (ssize_t)bytes_remaining;
        } else {
            bytes_to_read = (ssize_t)sizeof(chunk);
        }

        ssize_t bytes_read = read(source_file_fd, chunk, (size_t)bytes_to_read);

        if (bytes_read <= 0) {
            return -1;
        }

        if (send_all(connection_fd, chunk, (size_t)bytes_read) != 0) {
            return -1;
        }

        bytes_remaining -= bytes_read;
    }

    return 0;
}

// TCP may deliver data in multiple smaller pieces, so we loop advancing
// through destination_buf until all num_bytes have arrived.
int recv_file_data(int connection_fd, size_t num_bytes, char *destination_buf) {
    size_t total_bytes_received = 0;

    while (total_bytes_received < num_bytes) {
        ssize_t bytes_read = read(connection_fd,
                                  destination_buf + total_bytes_received,
                                  num_bytes - total_bytes_received);
        if (bytes_read <= 0) {
            return -1;
        }

        total_bytes_received += (size_t)bytes_read;
    }

    return 0;
}
