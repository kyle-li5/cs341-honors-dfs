#pragma once

#include <string>
#include <sys/types.h>

// TCP does not guarantee all bytes arrive in one shot, it may split a single
// send into multiple partial deliveries. This function loops until all
// num_bytes from buf have been fully sent over connection_fd.
// Returns 0 on success, -1 on error.
int send_all(int connection_fd, const char *buf, size_t num_bytes);

// Commands are sent as text lines (e.g. "UPLOAD foo.txt 1024\n"). This
// function reads one character at a time from connection_fd until it hits
// a newline, storing the full line (without the newline) in out_line.
// Returns 0 on success, -1 on error or disconnect.
int recv_line(int connection_fd, std::string &out_line);

// Sends a file to the receiver in two parts: first a header line so the
// receiver knows what is coming ("FILE <filename> <filesize>\n"), then
// streams all filesize bytes from source_file_fd over connection_fd.
// Returns 0 on success, -1 on error.
int send_file_data(int connection_fd, const char *filename, off_t filesize, int source_file_fd);

// TCP may deliver data in multiple smaller pieces rather than all at once.
// This function loops until exactly num_bytes have been received from
// connection_fd and stored in destination_buf.
// destination_buf must be pre-allocated with at least num_bytes of space.
// Returns 0 on success, -1 on error.
int recv_file_data(int connection_fd, size_t num_bytes, char *destination_buf);
