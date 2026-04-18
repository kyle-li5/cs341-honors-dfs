#!/usr/bin/env python3
"""
Simple test client for the DFS server
Usage: python3 test_client.py
"""

import socket
import os

HOST = 'localhost'
PORT = 9000

def send_command(sock, command):
    """Send a command and receive response"""
    sock.sendall((command + '\n').encode())
    response = sock.recv(4096).decode().strip()
    return response

def upload_file(sock, filename):
    """Upload a file to the server"""
    if not os.path.exists(filename):
        print(f"Error: File {filename} not found")
        return

    size = os.path.getsize(filename)
    command = f"UPLOAD {filename} {size}"
    sock.sendall((command + '\n').encode())

    # Send file data
    with open(filename, 'rb') as f:
        data = f.read()
        sock.sendall(data)

    response = sock.recv(4096).decode().strip()
    print(f"Server: {response}")

def download_file(sock, filename):
    """Download a file from the server"""
    command = f"DOWNLOAD {filename}"
    sock.sendall((command + '\n').encode())

    # Read response (should be "OK <size>")
    response = sock.recv(4096).decode().strip()
    print(f"Server: {response}")

    if response.startswith("OK"):
        # Parse file size
        parts = response.split()
        if len(parts) >= 2:
            size = int(parts[1])

            # Read file data
            data = b''
            while len(data) < size:
                chunk = sock.recv(min(4096, size - len(data)))
                if not chunk:
                    break
                data += chunk

            # Save to file
            output_filename = f"downloaded_{filename}"
            with open(output_filename, 'wb') as f:
                f.write(data)
            print(f"Saved to {output_filename} ({len(data)} bytes)")

def main():
    # Connect to server
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((HOST, PORT))

    # Read welcome message
    welcome = sock.recv(4096).decode().strip()
    print(f"Server: {welcome}\n")

    # Example usage
    print("=== Testing LIST ===")
    print(f"Server: {send_command(sock, 'LIST')}\n")

    # Create a test file
    test_file = "test.txt"
    with open(test_file, 'w') as f:
        f.write("Hello from DFS client!\nThis is a test file.\n")

    print("=== Testing UPLOAD ===")
    upload_file(sock, test_file)
    print()

    print("=== Testing LIST (after upload) ===")
    print(f"Server: {send_command(sock, 'LIST')}\n")

    print("=== Testing DOWNLOAD ===")
    download_file(sock, test_file)
    print()

    print("=== Testing DELETE ===")
    print(f"Server: {send_command(sock, f'DELETE {test_file}')}\n")

    print("=== Testing LIST (after delete) ===")
    print(f"Server: {send_command(sock, 'LIST')}\n")

    # Quit
    print(f"Server: {send_command(sock, 'QUIT')}")
    sock.close()

    # Cleanup
    os.remove(test_file)
    if os.path.exists(f"downloaded_{test_file}"):
        os.remove(f"downloaded_{test_file}")

if __name__ == '__main__':
    main()
