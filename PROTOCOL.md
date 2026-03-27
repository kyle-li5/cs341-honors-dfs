# DFS Server Protocol Documentation

## Overview
Text-based protocol for file operations over TCP.

## Connection
- **Host:** localhost
- **Port:** 9000
- **Type:** Persistent TCP connection (multiple commands per connection)

## Protocol Format

### Client → Server
```
COMMAND [arguments]\n
```

### Server → Client
```
OK [data]\n
ERROR [message]\n
```

## Commands

### LIST
List all files stored on the server.

**Request:**
```
LIST
```

**Response:**
```
OK <count> files:
<filename1> <size1> bytes
<filename2> <size2> bytes
...
```

Or if no files:
```
OK No files stored
```

### UPLOAD
Upload a file to the server.

**Request:**
```
UPLOAD <filename> <size>
<binary data of <size> bytes>
```

**Response:**
```
OK File uploaded successfully
```

Or on error:
```
ERROR <error message>
```

**Example:**
```
UPLOAD test.txt 23
This is the file data.
```

### DOWNLOAD
Download a file from the server.

**Request:**
```
DOWNLOAD <filename>
```

**Response:**
```
OK <size>
<binary data of <size> bytes>
```

Or on error:
```
ERROR File not found
```

### DELETE
Delete a file from the server.

**Request:**
```
DELETE <filename>
```

**Response:**
```
OK File deleted
```

Or on error:
```
ERROR File not found
```

### QUIT
Close the connection gracefully.

**Request:**
```
QUIT
```

**Response:**
```
OK Goodbye
```

## Testing with netcat

```bash
# Connect to server
nc localhost 9000

# You'll see welcome message, then you can type commands:
LIST
UPLOAD test.txt 11
Hello World
LIST
DOWNLOAD test.txt
DELETE test.txt
QUIT
```

## Testing with Python client

```bash
python3 test_client.py
```

## Security Notes

- Filenames are validated to prevent directory traversal attacks
- Files are stored in `./storage/` directory
- No authentication implemented yet (planned for later)
- No encryption (can add TLS later)
