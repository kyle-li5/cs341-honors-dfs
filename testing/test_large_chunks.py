#!/usr/bin/env python3
"""
test_large_chunks.py — Verify large-file chunking and STATUS display.

Creates a 100 MB file (fast) and a 1 GB file (if --big passed), uploads each,
then exercises every protocol path the TUI uses:
  LIST   → parse logical file list
  STATUS → parse per-chunk distribution

Prints a simulated TUI row for each file so you can visually confirm what
_render_files would show.

Usage:
  python3 testing/test_large_chunks.py          # 100 MB test
  python3 testing/test_large_chunks.py --big    # 1 GB test (slower)
  python3 testing/test_large_chunks.py --clean  # delete test files and exit
"""

import os
import socket
import sys
import time

HOST = "localhost"
PORT = 9000
CHUNK_SIZE = 1024 * 1024  # must match server.cpp


# ── low-level helpers ───────────────────────────────────────────────────────

def connect():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    welcome = recv_line(s)
    return s

def recv_line(s):
    buf = b""
    while not buf.endswith(b"\n"):
        ch = s.recv(1)
        if not ch:
            break
        buf += ch
    return buf.decode().strip()

def send_line(s, line):
    s.sendall((line + "\n").encode())

def recv_exact(s, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = s.recv(min(65536, n - len(buf)))
        if not chunk:
            break
        buf.extend(chunk)
    return bytes(buf)


# ── protocol commands (mirrors TUI Coordinator methods) ─────────────────────

def cmd_list(s):
    """LIST → [(filename, total_size), ...]"""
    send_line(s, "LIST")
    hdr = recv_line(s).split()
    if len(hdr) < 2 or hdr[0] != "OK":
        return []
    count = int(hdr[1])
    files = []
    for _ in range(count):
        parts = recv_line(s).split()
        if len(parts) >= 2:
            files.append((parts[0], int(parts[1])))
    return files

def cmd_status_file(s, filename):
    """STATUS <filename> → [{index, size, nodes}, ...]"""
    send_line(s, f"STATUS {filename}")
    hdr = recv_line(s).split()
    if len(hdr) < 3 or hdr[0] != "OK":
        print(f"  [!] STATUS {filename} returned: {' '.join(hdr)}")
        return []
    chunk_count = int(hdr[1])
    total_size  = int(hdr[2])
    chunks = []
    for _ in range(chunk_count):
        parts = recv_line(s).split()
        if len(parts) >= 4 and parts[0] == "CHUNK":
            n_nodes = int(parts[3])
            nodes = [int(parts[4 + i]) for i in range(n_nodes) if 4 + i < len(parts)]
            chunks.append({"index": int(parts[1]), "size": int(parts[2]), "nodes": nodes})
    return chunks, total_size

def cmd_upload(s, filepath):
    data = open(filepath, "rb").read()
    basename = os.path.basename(filepath)
    send_line(s, f"UPLOAD {basename} {len(data)}")
    s.sendall(data)
    return recv_line(s)

def cmd_delete(s, filename):
    send_line(s, f"DELETE {filename}")
    return recv_line(s)


# ── TUI rendering (mirrors _render_files logic) ──────────────────────────────

def fmt_bytes(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return f"{n:,.0f} {unit}"
        n /= 1024
    return f"{n:.1f} TB"

def render_distribution(chunks):
    if not chunks:
        return "—"
    if len(chunks) == 1:
        return "  ".join(f"N{n}" for n in chunks[0]["nodes"])
    parts = []
    for c in chunks:
        node_str = ",".join(str(n) for n in c["nodes"])
        parts.append(f"c{c['index']}:[{node_str}]")
    return "  ".join(parts)

def print_tui_row(filename, total_size, chunks):
    dist = render_distribution(chunks)
    print(f"  {'Filename':<30} {'Size':>10}  {'Chunks':>6}  Distribution")
    print(f"  {filename:<30} {fmt_bytes(total_size):>10}  {len(chunks):>6}  {dist}")


# ── test helpers ─────────────────────────────────────────────────────────────

def make_test_file(path, size_bytes):
    """Write a repeating pattern so content is verifiable."""
    block = (b"ABCDEFGHIJKLMNOP" * 64)  # 1 KB block
    written = 0
    with open(path, "wb") as f:
        while written < size_bytes:
            to_write = min(len(block), size_bytes - written)
            f.write(block[:to_write])
            written += to_write

def verify_chunks(filename, chunks, total_size, expected_total):
    errors = []
    if total_size != expected_total:
        errors.append(f"total_size {total_size} != expected {expected_total}")

    expected_chunk_count = (expected_total + CHUNK_SIZE - 1) // CHUNK_SIZE
    if len(chunks) != expected_chunk_count:
        errors.append(f"got {len(chunks)} chunks, expected {expected_chunk_count}")

    # Check indices are contiguous 0..N-1
    indices = [c["index"] for c in chunks]
    if indices != list(range(len(chunks))):
        errors.append(f"chunk indices not contiguous: {indices}")

    # Check sizes sum to total
    size_sum = sum(c["size"] for c in chunks)
    if size_sum != expected_total:
        errors.append(f"chunk sizes sum to {size_sum}, expected {expected_total}")

    # Check last chunk size
    if chunks:
        last_expected = expected_total - (expected_chunk_count - 1) * CHUNK_SIZE
        if chunks[-1]["size"] != last_expected:
            errors.append(f"last chunk size {chunks[-1]['size']} != expected {last_expected}")

    # Check every chunk has at least 1 node
    for c in chunks:
        if not c["nodes"]:
            errors.append(f"chunk {c['index']} has no nodes")

    return errors


# ── main ─────────────────────────────────────────────────────────────────────

def run_test(label, size_bytes, tmp_path, s):
    basename = os.path.basename(tmp_path)
    print(f"\n{'='*60}")
    print(f"TEST: {label}  ({fmt_bytes(size_bytes)})")
    print(f"{'='*60}")

    # --- generate ---
    print(f"Generating {tmp_path} ...", end=" ", flush=True)
    t0 = time.time()
    make_test_file(tmp_path, size_bytes)
    print(f"done ({time.time()-t0:.1f}s)")

    # --- upload ---
    print(f"Uploading {basename} ...", end=" ", flush=True)
    t0 = time.time()
    resp = cmd_upload(s, tmp_path)
    elapsed = time.time() - t0
    if not resp.startswith("OK"):
        print(f"FAILED: {resp}")
        return False
    print(f"done ({elapsed:.1f}s,  {size_bytes/elapsed/1e6:.1f} MB/s)")

    # --- LIST ---
    files = cmd_list(s)
    names = [f for f, _ in files]
    if basename not in names:
        print(f"FAIL: {basename} not in LIST response: {names}")
        return False
    listed_size = next(sz for fn, sz in files if fn == basename)
    if listed_size != size_bytes:
        print(f"FAIL: LIST size {listed_size} != {size_bytes}")
        return False
    print(f"LIST OK — {basename} visible, size matches")

    # --- STATUS ---
    result = cmd_status_file(s, basename)
    if not result:
        print("FAIL: STATUS returned nothing")
        return False
    chunks, total_size = result

    errors = verify_chunks(basename, chunks, total_size, size_bytes)
    if errors:
        print("FAIL — chunk verification errors:")
        for e in errors:
            print(f"  • {e}")
        return False

    expected_n = (size_bytes + CHUNK_SIZE - 1) // CHUNK_SIZE
    print(f"STATUS OK — {len(chunks)}/{expected_n} chunks, all indices & sizes correct")

    # --- TUI simulation ---
    print("\nTUI _render_files would show:")
    print_tui_row(basename, total_size, chunks)

    # --- cleanup ---
    print(f"\nDeleting {basename} from DFS ...", end=" ", flush=True)
    resp = cmd_delete(s, basename)
    print("OK" if resp.startswith("OK") else f"WARN: {resp}")
    os.remove(tmp_path)

    return True


def cleanup_only(s, names):
    for name in names:
        resp = cmd_delete(s, name)
        print(f"DELETE {name}: {resp}")
        local = f"/tmp/{name}"
        if os.path.exists(local):
            os.remove(local)


def main():
    big = "--big" in sys.argv
    clean = "--clean" in sys.argv

    s = connect()

    if clean:
        test_names = ["dfs_test_100mb.bin", "dfs_test_1gb.bin"]
        cleanup_only(s, test_names)
        send_line(s, "QUIT")
        s.close()
        return

    all_ok = True

    # Always run 100 MB
    ok = run_test(
        "100 MB file",
        100 * 1024 * 1024,
        "/tmp/dfs_test_100mb.bin",
        s,
    )
    all_ok = all_ok and ok

    if big:
        ok = run_test(
            "1 GB file",
            1024 * 1024 * 1024,
            "/tmp/dfs_test_1gb.bin",
            s,
        )
        all_ok = all_ok and ok

    send_line(s, "QUIT")
    s.close()

    print(f"\n{'='*60}")
    print("ALL TESTS PASSED" if all_ok else "SOME TESTS FAILED")
    print(f"{'='*60}\n")
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
