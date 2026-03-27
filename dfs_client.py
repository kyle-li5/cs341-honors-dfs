#!/usr/bin/env python3
"""
Simple DFS client for uploading/downloading files
Usage:
    python3 dfs_client.py upload <filename>
    python3 dfs_client.py download <filename>
    python3 dfs_client.py list
    python3 dfs_client.py delete <filename>
"""

import socket
import os
import sys

HOST = 'localhost'
PORT = 9000

def connect():
    """Connect to the server"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((HOST, PORT))
    # Read welcome message
    welcome = sock.recv(4096).decode().strip()
    print(f"Server: {welcome}")
    return sock

def send_command(sock, command):
    """Send a command and receive response"""
    sock.sendall((command + '\n').encode())
    response = sock.recv(4096).decode().strip()
    return response

def list_files():
    """List all files on server"""
    sock = connect()
    response = send_command(sock, 'LIST')
    print(response)
    send_command(sock, 'QUIT')
    sock.close()

def upload_file(filename):
    """Upload a file to the server"""
    if not os.path.exists(filename):
        print(f"Error: File '{filename}' not found")
        return

    sock = connect()

    size = os.path.getsize(filename)
    basename = os.path.basename(filename)

    print(f"Uploading {basename} ({size} bytes)...")

    # Send UPLOAD command
    command = f"UPLOAD {basename} {size}"
    sock.sendall((command + '\n').encode())

    # Send file data
    with open(filename, 'rb') as f:
        data = f.read()
        sock.sendall(data)

    response = sock.recv(4096).decode().strip()
    print(f"Server: {response}")

    send_command(sock, 'QUIT')
    sock.close()

def download_file(filename):
    """Download a file from the server"""
    sock = connect()

    print(f"Downloading {filename}...")

    command = f"DOWNLOAD {filename}"
    sock.sendall((command + '\n').encode())

    # Read response (should be "OK <size>")
    response = sock.recv(4096).decode().strip()

    if response.startswith("ERROR"):
        print(f"Server: {response}")
    elif response.startswith("OK"):
        # Parse file size
        parts = response.split()
        if len(parts) >= 2:
            size = int(parts[1])
            print(f"Receiving {size} bytes...")

            # Read file data
            data = b''
            while len(data) < size:
                chunk = sock.recv(min(4096, size - len(data)))
                if not chunk:
                    break
                data += chunk

            # Save to file
            with open(filename, 'wb') as f:
                f.write(data)
            print(f"Saved to {filename} ({len(data)} bytes)")

    send_command(sock, 'QUIT')
    sock.close()

def delete_file(filename):
    """Delete a file from the server"""
    sock = connect()
    response = send_command(sock, f'DELETE {filename}')
    print(f"Server: {response}")
    send_command(sock, 'QUIT')
    sock.close()

def main():
    if len(sys.argv) < 2:
        print("Usage:")
        print("  python3 dfs_client.py list")
        print("  python3 dfs_client.py upload <filename>")
        print("  python3 dfs_client.py download <filename>")
        print("  python3 dfs_client.py delete <filename>")
        sys.exit(1)

    command = sys.argv[1].lower()

    try:
        if command == 'list':
            list_files()
        elif command == 'upload':
            if len(sys.argv) < 3:
                print("Error: Please specify a filename to upload")
                sys.exit(1)
            upload_file(sys.argv[2])
        elif command == 'download':
            if len(sys.argv) < 3:
                print("Error: Please specify a filename to download")
                sys.exit(1)
            download_file(sys.argv[2])
        elif command == 'delete':
            if len(sys.argv) < 3:
                print("Error: Please specify a filename to delete")
                sys.exit(1)
            delete_file(sys.argv[2])
        else:
            print(f"Unknown command: {command}")
            sys.exit(1)
    except ConnectionRefusedError:
        print("Error: Cannot connect to server. Is it running?")
        print("Start the server with: ./server")
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)

if __name__ == '__main__':
    main()
