#!/usr/bin/env python3
"""
Edge case tests for the DFS server.

Tests two categories:
  1. Large files - files that span multiple 4096-byte read/write chunks
  2. Connection drops - mid-transfer disconnects during upload and download

Usage:
    python3 test_edge_cases.py

Requires the server and all three nodes to be running:
    ./server
"""

import socket
import os
import struct
import hashlib
import time
import sys

HOST = 'localhost'
PORT = 9000

# Must match BUFFER_SIZE in constants.hpp
CHUNK_SIZE = 4096

# ─── helpers ──────────────────────────────────────────────────────────────────

passed = 0
failed = 0


def result(name, ok, detail=''):
    global passed, failed
    status = 'PASS' if ok else 'FAIL'
    suffix = f'  ({detail})' if detail else ''
    print(f'  [{status}] {name}{suffix}')
    if ok:
        passed += 1
    else:
        failed += 1


def connect():
    """Open a connection and consume the welcome banner."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)
    sock.connect((HOST, PORT))
    sock.recv(4096)  # welcome message
    return sock


def send_line(sock, line):
    sock.sendall((line + '\n').encode())


def recv_line(sock):
    """Read until newline (tolerates recv returning less than a full line)."""
    buf = b''
    while True:
        ch = sock.recv(1)
        if not ch:
            raise ConnectionError('connection closed')
        if ch == b'\n':
            return buf.decode().strip()
        buf += ch


def upload(sock, name, data: bytes):
    """Upload raw bytes under the given name. Returns the server response line."""
    send_line(sock, f'UPLOAD {name} {len(data)}')
    sock.sendall(data)
    return recv_line(sock)


def download(sock, name):
    """Download a file. Returns bytes on success, raises on error."""
    send_line(sock, f'DOWNLOAD {name}')
    resp = recv_line(sock)
    if not resp.startswith('OK'):
        raise RuntimeError(f'download failed: {resp}')
    size = int(resp.split()[1])
    buf = b''
    while len(buf) < size:
        chunk = sock.recv(min(CHUNK_SIZE, size - len(buf)))
        if not chunk:
            raise ConnectionError('connection closed mid-download')
        buf += chunk
    return buf


def delete(sock, name):
    send_line(sock, f'DELETE {name}')
    return recv_line(sock)


def list_files(sock):
    send_line(sock, 'LIST')
    return recv_line(sock)


def quit_and_close(sock):
    try:
        send_line(sock, 'QUIT')
        sock.recv(64)
    except Exception:
        pass
    sock.close()


def abrupt_close(sock):
    """Send a TCP RST so the server gets an immediate connection error."""
    sock.setsockopt(
        socket.SOL_SOCKET,
        socket.SO_LINGER,
        struct.pack('ii', 1, 0)  # l_onoff=1, l_linger=0
    )
    sock.close()


def md5(data: bytes) -> str:
    return hashlib.md5(data).hexdigest()


def make_data(size: int) -> bytes:
    """Generate deterministic bytes of the requested size."""
    pattern = b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789'
    repeats = (size // len(pattern)) + 1
    return (pattern * repeats)[:size]


def server_alive():
    """Return True if the coordinator accepts a connection right now."""
    try:
        s = connect()
        quit_and_close(s)
        return True
    except Exception:
        return False


def file_exists_on_server(name):
    """Return True if the given filename appears in LIST output."""
    try:
        s = connect()
        resp = list_files(s)
        quit_and_close(s)
        return name in resp
    except Exception:
        return False


def cleanup(name):
    """Best-effort delete of a test file so tests don't pollute each other."""
    try:
        s = connect()
        delete(s, name)
        quit_and_close(s)
    except Exception:
        pass


# ─── large file tests ─────────────────────────────────────────────────────────

def test_large_files():
    print('\n=== Large File Tests ===')

    sizes = [
        ('boundary_exact',  CHUNK_SIZE,         'exactly one chunk (4096 B)'),
        ('boundary_plus1',  CHUNK_SIZE + 1,      'one byte over one chunk (4097 B)'),
        ('boundary_minus1', CHUNK_SIZE - 1,      'one byte under one chunk (4095 B)'),
        ('two_chunks',      CHUNK_SIZE * 2,      'exactly two chunks (8192 B)'),
        ('multi_chunk',     CHUNK_SIZE * 10 + 7, 'ten chunks + 7 bytes (40967 B)'),
        ('one_mb',          1024 * 1024,         '1 MB'),
        ('ten_mb',          10 * 1024 * 1024,    '10 MB'),
    ]

    for name, size, label in sizes:
        fname = f'large_test_{name}.bin'
        original = make_data(size)
        try:
            # upload
            s = connect()
            resp = upload(s, fname, original)
            if not resp.startswith('OK'):
                result(f'upload {label}', False, f'server said: {resp}')
                quit_and_close(s)
                continue
            quit_and_close(s)

            # download and verify
            s = connect()
            received = download(s, fname)
            quit_and_close(s)

            if len(received) != size:
                result(f'roundtrip {label}', False,
                       f'got {len(received)} bytes, expected {size}')
            elif md5(received) != md5(original):
                result(f'roundtrip {label}', False, 'content mismatch')
            else:
                result(f'roundtrip {label}', True, f'{size} bytes, md5 ok')
        except Exception as e:
            result(f'roundtrip {label}', False, str(e))
        finally:
            cleanup(fname)


# ─── connection-drop tests ────────────────────────────────────────────────────

def test_connection_drops():
    print('\n=== Connection Drop Tests ===')

    # ── 1. Drop immediately after connecting (before any command) ──────────────
    fname = 'drop_test_bare.bin'
    try:
        s = connect()
        abrupt_close(s)
        time.sleep(0.1)
        result('bare connect then RST', server_alive(), 'server must still accept new connections')
    except Exception as e:
        result('bare connect then RST', False, str(e))

    # ── 2. Send UPLOAD command, send NO data, then close (graceful FIN) ────────
    fname = 'drop_test_no_data.bin'
    data = make_data(CHUNK_SIZE * 2)
    try:
        s = connect()
        send_line(s, f'UPLOAD {fname} {len(data)}')
        # deliberately send nothing and close gracefully
        quit_and_close(s)
        time.sleep(0.2)
        ok = server_alive() and not file_exists_on_server(fname)
        result('upload cmd sent, no data, graceful close',
               ok, 'server alive and incomplete file not listed')
    except Exception as e:
        result('upload cmd sent, no data, graceful close', False, str(e))
    finally:
        cleanup(fname)

    # ── 3. Send UPLOAD command, send NO data, then RST ────────────────────────
    fname = 'drop_test_no_data_rst.bin'
    data = make_data(CHUNK_SIZE * 2)
    try:
        s = connect()
        send_line(s, f'UPLOAD {fname} {len(data)}')
        abrupt_close(s)
        time.sleep(0.2)
        ok = server_alive() and not file_exists_on_server(fname)
        result('upload cmd sent, no data, RST',
               ok, 'server alive and incomplete file not listed')
    except Exception as e:
        result('upload cmd sent, no data, RST', False, str(e))
    finally:
        cleanup(fname)

    # ── 4. Send UPLOAD + partial data (half), then close gracefully ───────────
    fname = 'drop_test_half_data.bin'
    data = make_data(CHUNK_SIZE * 4)   # 16 KB; cut at 8 KB
    half = len(data) // 2
    try:
        s = connect()
        send_line(s, f'UPLOAD {fname} {len(data)}')
        s.sendall(data[:half])
        quit_and_close(s)
        time.sleep(0.2)
        ok = server_alive() and not file_exists_on_server(fname)
        result('upload half data, graceful close',
               ok, 'server alive and partial file not listed')
    except Exception as e:
        result('upload half data, graceful close', False, str(e))
    finally:
        cleanup(fname)

    # ── 5. Send UPLOAD + partial data (half), then RST ───────────────────────
    fname = 'drop_test_half_rst.bin'
    data = make_data(CHUNK_SIZE * 4)
    half = len(data) // 2
    try:
        s = connect()
        send_line(s, f'UPLOAD {fname} {len(data)}')
        s.sendall(data[:half])
        abrupt_close(s)
        time.sleep(0.2)
        ok = server_alive() and not file_exists_on_server(fname)
        result('upload half data, RST',
               ok, 'server alive and partial file not listed')
    except Exception as e:
        result('upload half data, RST', False, str(e))
    finally:
        cleanup(fname)

    # ── 6. Drop mid-upload at a chunk boundary ────────────────────────────────
    fname = 'drop_test_chunk_boundary.bin'
    data = make_data(CHUNK_SIZE * 3)   # 12 KB; stop at exactly 4 KB
    try:
        s = connect()
        send_line(s, f'UPLOAD {fname} {len(data)}')
        s.sendall(data[:CHUNK_SIZE])   # send exactly one chunk then drop
        abrupt_close(s)
        time.sleep(0.2)
        ok = server_alive() and not file_exists_on_server(fname)
        result('upload drop at chunk boundary (RST)',
               ok, 'server alive and partial file not listed')
    except Exception as e:
        result('upload drop at chunk boundary (RST)', False, str(e))
    finally:
        cleanup(fname)

    # ── 7. Full upload succeeds after a previously broken upload ──────────────
    fname = 'drop_test_recovery.bin'
    data = make_data(CHUNK_SIZE * 2)
    try:
        # broken upload first
        s = connect()
        send_line(s, f'UPLOAD {fname} {len(data)}')
        s.sendall(data[:CHUNK_SIZE // 2])
        abrupt_close(s)
        time.sleep(0.2)

        # now do a full upload of the same filename
        s = connect()
        resp = upload(s, fname, data)
        quit_and_close(s)
        if not resp.startswith('OK'):
            result('full upload after broken upload', False, f'server said: {resp}')
        else:
            # download and verify
            s = connect()
            received = download(s, fname)
            quit_and_close(s)
            ok = len(received) == len(data) and md5(received) == md5(data)
            result('full upload after broken upload', ok,
                   'data correct after prior broken attempt')
    except Exception as e:
        result('full upload after broken upload', False, str(e))
    finally:
        cleanup(fname)

    # ── 8. Drop right after receiving the DOWNLOAD OK line (before data) ──────
    fname = 'drop_test_dl_before_data.bin'
    data = make_data(CHUNK_SIZE * 2)
    try:
        # upload first so the file exists
        s = connect()
        upload(s, fname, data)
        quit_and_close(s)

        # connect, request download, read OK header, then drop
        s = connect()
        send_line(s, f'DOWNLOAD {fname}')
        resp = recv_line(s)   # consume "OK <size>"
        if not resp.startswith('OK'):
            result('download drop before data received', False,
                   f'unexpected response: {resp}')
        else:
            abrupt_close(s)
            time.sleep(0.2)
            result('download drop before data received', server_alive(),
                   'server alive after client dropped before reading data')
    except Exception as e:
        result('download drop before data received', False, str(e))
    finally:
        cleanup(fname)

    # ── 9. Drop after receiving partial download data ─────────────────────────
    fname = 'drop_test_dl_partial.bin'
    data = make_data(CHUNK_SIZE * 4)   # 16 KB; read only first 4 KB
    try:
        s = connect()
        upload(s, fname, data)
        quit_and_close(s)

        s = connect()
        send_line(s, f'DOWNLOAD {fname}')
        resp = recv_line(s)
        if not resp.startswith('OK'):
            result('download drop after partial receive', False,
                   f'unexpected response: {resp}')
        else:
            size = int(resp.split()[1])
            # read one chunk then drop
            s.recv(CHUNK_SIZE)
            abrupt_close(s)
            time.sleep(0.2)
            # make sure a complete download still works after this
            s2 = connect()
            received = download(s2, fname)
            quit_and_close(s2)
            ok = server_alive() and md5(received) == md5(data)
            result('download drop after partial receive', ok,
                   'server alive and file intact for next client')
    except Exception as e:
        result('download drop after partial receive', False, str(e))
    finally:
        cleanup(fname)

    # ── 10. Rapid successive broken connections (stress) ─────────────────────
    try:
        for _ in range(10):
            s = connect()
            abrupt_close(s)
        time.sleep(0.3)
        result('10x rapid RST connections', server_alive(),
               'server survives burst of abrupt disconnects')
    except Exception as e:
        result('10x rapid RST connections', False, str(e))


# ─── main ─────────────────────────────────────────────────────────────────────

def main():
    try:
        connect().close()
    except ConnectionRefusedError:
        print('ERROR: cannot connect to server. Start it with: ./server')
        sys.exit(1)

    test_large_files()
    test_connection_drops()

    print(f'\n=== Results: {passed} passed, {failed} failed ===')
    sys.exit(0 if failed == 0 else 1)


if __name__ == '__main__':
    main()
