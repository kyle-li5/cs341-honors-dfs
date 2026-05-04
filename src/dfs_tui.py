#!/usr/bin/env python3
"""
dfs_tui.py — Textual TUI for the Distributed File System.

Three live panels that auto-refresh every 3 s:
  • Storage Nodes  — health, file count, bytes, and load bar per node.
                     Queried *directly* from node ports 9001-9003 via
                     NODE_STATUS so you see the actual node internals.
  • Files          — one row per logical file showing total size, chunk
                     count, and which nodes hold each chunk.
  • Activity Log   — scrolling record of every command and its result.

Command bar (same commands as client.cpp):
  list / l / refresh / r   — refresh all panels
  upload  <local-path>     — upload a file through the coordinator
  download <filename>      — download to the current directory
  delete  <filename>       — delete from the DFS
  help / ?                 — show command reference
  quit / q                 — exit
  F5 or Ctrl+L             — manual refresh

Requirements:
  pip install textual        (Python 3.9+)

Usage:
  python3 dfs_tui.py                         # localhost:9000
  python3 dfs_tui.py raspberrypi.local 9000  # Pi by hostname
  python3 dfs_tui.py 192.168.1.42 9000       # Pi by IP
"""

import asyncio
import sys
from pathlib import Path
from typing import Optional

from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical
from textual.widgets import DataTable, Footer, Header, Input, Label, RichLog, Static
from textual import on

# ── connection settings (mirror constants.hpp) ─────────────────────────────
HOST             = sys.argv[1] if len(sys.argv) > 1 else "localhost"
COORDINATOR_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 9000
NODE_BASE_PORT   = 9001   # constants.hpp: NODE_BASE_PORT
NUM_NODES        = 3      # constants.hpp: NUM_NODES
REFRESH_SECS     = 3.0    # auto-refresh cadence
CONNECT_TIMEOUT  = 2.0    # per-node query timeout


# ── formatting helpers ─────────────────────────────────────────────────────

def fmt_bytes(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return f"{n:,.0f} {unit}"
        n /= 1024
    return f"{n:.1f} TB"


def load_bar(used: int, total: int, width: int = 14) -> str:
    """Unicode block progress bar."""
    if total == 0:
        return "░" * width
    filled = min(round(width * used / total), width)
    return "█" * filled + "░" * (width - filled)


# ── direct node queries (shows node internals, bypasses coordinator cache) ──

async def fetch_node_status(node_id: int) -> dict:
    """
    Open a one-shot connection to node NODE_BASE_PORT+node_id and send
    NODE_STATUS.  Response: "STATUS <chunk_count> <total_bytes>"
    """
    try:
        r, w = await asyncio.wait_for(
            asyncio.open_connection(HOST, NODE_BASE_PORT + node_id),
            timeout=CONNECT_TIMEOUT,
        )
        w.write(b"NODE_STATUS\n")
        await w.drain()
        line = (await asyncio.wait_for(r.readline(), timeout=CONNECT_TIMEOUT)).decode().strip()
        w.close()
        p = line.split()
        if len(p) == 3 and p[0] == "STATUS":
            return {"id": node_id, "chunks": int(p[1]), "bytes": int(p[2]), "reachable": True}
    except Exception:
        pass
    return {"id": node_id, "chunks": 0, "bytes": 0, "reachable": False}


# ── coordinator connection ─────────────────────────────────────────────────

class Coordinator:
    """
    Persistent async TCP connection to the coordinator on port 9000.
    Mirrors the same protocol logic as client.cpp.
    All command methods are serialised via an asyncio.Lock so concurrent
    refresh cycles and user commands never interleave on the socket.
    """

    def __init__(self, host: str, port: int):
        self._host, self._port = host, port
        self._reader: Optional[asyncio.StreamReader] = None
        self._writer: Optional[asyncio.StreamWriter] = None
        self._lock = asyncio.Lock()
        self.connected = False

    async def connect(self) -> str:
        """Open the connection and return the server's welcome message."""
        self._reader, self._writer = await asyncio.open_connection(self._host, self._port)
        self.connected = True
        return (await self._reader.readline()).decode().strip()

    async def close(self):
        if self._writer:
            try:
                self._writer.write(b"QUIT\n")
                await self._writer.drain()
            except Exception:
                pass
            self._writer.close()
        self.connected = False

    # ── internal helpers ───────────────────────────────────────────────────

    def _guard(self) -> Optional[str]:
        if not self.connected:
            return "ERROR not connected to coordinator"
        return None

    async def _send(self, data: bytes):
        self._writer.write(data)
        await self._writer.drain()

    async def _readline(self) -> str:
        return (await self._reader.readline()).decode().strip()

    async def _readexact(self, n: int) -> bytes:
        buf = bytearray()
        while len(buf) < n:
            chunk = await self._reader.read(n - len(buf))
            if not chunk:
                break
            buf.extend(chunk)
        return bytes(buf)

    # ── public commands ────────────────────────────────────────────────────

    async def list_files(self) -> list[tuple[str, int]]:
        """
        LIST\n → "OK <count>\n" then "<filename> <size>\n" per logical file.
        """
        if err := self._guard():
            return []
        async with self._lock:
            await self._send(b"LIST\n")
            hdr = await self._readline()
            p = hdr.split()
            if len(p) < 2 or p[0] != "OK":
                return []
            count = int(p[1])
            files: list[tuple[str, int]] = []
            for _ in range(count):
                line = await self._readline()
                lp = line.split()
                if len(lp) >= 2:
                    files.append((lp[0], int(lp[1])))
            return files

    async def status_file(self, filename: str) -> list[dict]:
        """
        STATUS <filename>\n → "OK <chunk_count> <total_size>\n"
        then "CHUNK <idx> <size> <node_count> <node0> …\n" per chunk.
        Returns a list of dicts: {index, size, nodes}.
        """
        if err := self._guard():
            return []
        async with self._lock:
            await self._send(f"STATUS {filename}\n".encode())
            hdr = await self._readline()
            p = hdr.split()
            if len(p) < 3 or p[0] != "OK":
                return []
            chunk_count = int(p[1])
            chunks: list[dict] = []
            for _ in range(chunk_count):
                line = await self._readline()
                lp = line.split()
                if len(lp) >= 4 and lp[0] == "CHUNK":
                    n_nodes = int(lp[3])
                    nodes = [int(lp[4 + i]) for i in range(n_nodes) if 4 + i < len(lp)]
                    chunks.append({"index": int(lp[1]), "size": int(lp[2]), "nodes": nodes})
            return chunks

    async def upload(self, filepath: str) -> str:
        """
        UPLOAD <filename> <size>\n<bytes>
        Returns the server's "OK …" or "ERROR …" response line.
        """
        if err := self._guard():
            return err
        path = Path(filepath)
        if not path.exists():
            return f"ERROR file not found: {filepath}"
        data = path.read_bytes()
        cmd = f"UPLOAD {path.name} {len(data)}\n".encode()
        async with self._lock:
            await self._send(cmd)
            await self._send(data)
            return await self._readline()

    async def download(self, filename: str) -> tuple[str, bytes]:
        """
        DOWNLOAD <filename>\n
        Server responds "OK <size>\n" then <size> raw bytes.
        Returns (status_string, file_bytes).
        """
        if err := self._guard():
            return err, b""
        async with self._lock:
            await self._send(f"DOWNLOAD {filename}\n".encode())
            resp = await self._readline()
            if not resp.startswith("OK"):
                return resp, b""
            size = int(resp.split()[1])
            data = await self._readexact(size)
        return f"OK {size}", data

    async def delete(self, filename: str) -> str:
        if err := self._guard():
            return err
        async with self._lock:
            await self._send(f"DELETE {filename}\n".encode())
            return await self._readline()

    async def kill_node(self, node_id: int) -> str:
        if err := self._guard():
            return err
        async with self._lock:
            await self._send(f"KILL_NODE {node_id}\n".encode())
            return await self._readline()

    async def revive_node(self, node_id: int) -> str:
        if err := self._guard():
            return err
        async with self._lock:
            await self._send(f"REVIVE_NODE {node_id}\n".encode())
            return await self._readline()


# ── Textual app ────────────────────────────────────────────────────────────

class DFSApp(App):
    """Textual TUI for the Distributed File System."""

    TITLE = f"Distributed File System  ·  {HOST}:{COORDINATOR_PORT}"

    CSS = """
    /* ── overall layout ─────────────────────────────────────── */
    Screen { layout: vertical; }

    #main { height: 1fr; }

    /* ── left panel: storage nodes ──────────────────────────── */
    #nodes-panel {
        width: 40;
        border: solid $primary;
        padding: 1 1 0 1;
    }

    .panel-title {
        text-style: bold;
        color: $accent;
        padding-bottom: 1;
    }

    .node-card {
        height: auto;
        padding-bottom: 1;
    }

    /* ── right panel: file listing ──────────────────────────── */
    #files-panel {
        border: solid $primary;
        padding: 0 1;
    }

    #files-table { height: 1fr; }

    /* ── bottom section ─────────────────────────────────────── */
    #cmd-input {
        height: 3;
        border: solid $accent;
    }

    #log-panel {
        height: 8;
        border: solid $panel;
        padding: 0 1;
    }
    """

    BINDINGS = [
        Binding("f5",     "refresh", "Refresh", show=True),
        Binding("ctrl+l", "refresh", "Refresh", show=False),
    ]

    def __init__(self):
        super().__init__()
        self._coord = Coordinator(HOST, COORDINATOR_PORT)

    # ── layout ─────────────────────────────────────────────────────────────

    def compose(self) -> ComposeResult:
        yield Header()
        with Horizontal(id="main"):
            # Left: per-node health cards sourced directly from node ports
            with Vertical(id="nodes-panel"):
                yield Label("Storage Nodes", classes="panel-title")
                for i in range(NUM_NODES):
                    yield Static(classes="node-card", id=f"node-{i}")
            # Right: file table sourced from coordinator LIST + STATUS per file
            with Vertical(id="files-panel"):
                yield Label("Files", id="files-title", classes="panel-title")
                tbl = DataTable(id="files-table", zebra_stripes=True, cursor_type="row")
                tbl.add_columns("Filename", "Size", "Chunks", "Distribution")
                yield tbl
        yield Input(
            placeholder="list  upload <path>  download <name>  delete <name>  kill_node <id>  revive_node <id>  help  quit",
            id="cmd-input",
        )
        yield RichLog(id="log-panel", markup=True, highlight=True)
        yield Footer()

    # ── startup ────────────────────────────────────────────────────────────

    async def on_mount(self):
        self._node_reachable: dict[int, bool] = {}
        self._log = self.query_one("#log-panel", RichLog)
        try:
            welcome = await self._coord.connect()
            self._log.write(f"[green]● Connected to {HOST}:{COORDINATOR_PORT}[/]")
            self._log.write(f"[dim]{welcome}[/]")
        except Exception as e:
            self._log.write(f"[red]● Cannot connect to coordinator: {e}[/]")
            self._log.write("[dim]Start the server first:  make && ./server[/]")

        self.set_interval(REFRESH_SECS, self.action_refresh)
        await self._refresh()

    # ── data refresh (queries node ports directly) ──────────────────────────

    async def _fetch_file_data(self) -> list[tuple[tuple[str, int], list[dict]]]:
        """Ask the coordinator for the logical file list then chunk details per file."""
        if not self._coord.connected:
            return []
        files = await self._coord.list_files()
        result: list[tuple[tuple[str, int], list[dict]]] = []
        for fname, size in files:
            chunks = await self._coord.status_file(fname)
            result.append(((fname, size), chunks))
        return result

    async def _refresh(self):
        """Fetch node status (direct) and file metadata (via coordinator) concurrently."""
        node_stats, file_data = await asyncio.gather(
            asyncio.gather(*[fetch_node_status(i) for i in range(NUM_NODES)]),
            self._fetch_file_data(),
        )
        self._render_nodes(list(node_stats))
        self._render_files(file_data)

    def _render_nodes(self, stats: list[dict]):
        total_bytes = sum(s["bytes"] for s in stats if s["reachable"])
        for s in stats:
            nid  = s["id"]
            port = NODE_BASE_PORT + nid
            card = self.query_one(f"#node-{nid}", Static)
            prev = self._node_reachable.get(nid)
            if not s["reachable"]:
                if prev is not False:
                    self.query_one("#log-panel", RichLog).write(
                        f"[red]*** NODE DOWN: Node {nid} (port {port}) went offline ***[/]"
                    )
                self._node_reachable[nid] = False
                card.update(
                    f"[bold]Node {nid}[/]  [dim]:{port}[/]  [red]● OFFLINE[/]\n"
                    f"  [dim]—[/]\n"
                )
            else:
                if prev is False:
                    self.query_one("#log-panel", RichLog).write(
                        f"[green]*** NODE UP: Node {nid} (port {port}) is back online ***[/]"
                    )
                self._node_reachable[nid] = True
                pct   = (s["bytes"] / total_bytes * 100) if total_bytes else 0
                bar   = load_bar(s["bytes"], total_bytes)
                chunks = f"{s['chunks']} chunk{'s' if s['chunks'] != 1 else ''}"
                card.update(
                    f"[bold cyan]Node {nid}[/]  [dim]:{port}[/]  [green]●[/]\n"
                    f"  {chunks}  ·  {fmt_bytes(s['bytes'])}\n"
                    f"  [yellow]{bar}[/]  {pct:.0f}%\n"
                )

    def _render_files(self, file_data: list[tuple[tuple[str, int], list[dict]]]):
        tbl = self.query_one("#files-table", DataTable)
        tbl.clear()
        for (filename, size), chunks in file_data:
            if not chunks:
                dist = "—"
            elif len(chunks) == 1:
                nodes = chunks[0]["nodes"]
                dist = "  ".join(f"N{n}" for n in nodes)
            else:
                parts = []
                for c in chunks:
                    node_str = ",".join(str(n) for n in c["nodes"])
                    parts.append(f"c{c['index']}:[{node_str}]")
                dist = "  ".join(parts)
            tbl.add_row(filename, fmt_bytes(size), str(len(chunks)), dist)
        total = len(file_data)
        self.query_one("#files-title", Label).update(f"Files  ({total} total)")

    async def action_refresh(self):
        await self._refresh()

    # ── command input ──────────────────────────────────────────────────────

    @on(Input.Submitted)
    async def on_input_submitted(self, event: Input.Submitted):
        line = event.value.strip()
        event.input.value = ""
        if not line:
            return
        try:
            await self._dispatch(line)
        except Exception as e:
            self._log.write(f"[red]Unexpected error: {e}[/]")

    async def _dispatch(self, line: str):
        parts = line.split(None, 1)
        cmd = parts[0].lower()
        arg = parts[1].strip() if len(parts) > 1 else ""

        # ── quit ───────────────────────────────────────────────────────────
        if cmd in ("quit", "q", "exit"):
            await self._coord.close()
            self.exit()

        # ── refresh ────────────────────────────────────────────────────────
        elif cmd in ("list", "l", "refresh", "r", "status", "s"):
            await self._refresh()
            self._log.write("[cyan]Refreshed.[/]")

        # ── upload ─────────────────────────────────────────────────────────
        elif cmd in ("upload", "u"):
            if not arg:
                self._log.write("[yellow]Usage: upload <local-filepath>[/]")
                return
            name = Path(arg).name
            self._log.write(f"Uploading [bold]{name}[/]…")
            resp = await self._coord.upload(arg)
            if resp.startswith("OK"):
                sz = Path(arg).stat().st_size if Path(arg).exists() else 0
                self._log.write(f"[green]✓ {name} uploaded  ({fmt_bytes(sz)})[/]")
                if "WARNING" in resp:
                    warn = resp[resp.find("WARNING"):]
                    self._log.write(f"[yellow]⚠ {warn}[/]")
            else:
                self._log.write(f"[red]{resp}[/]")
            await self._refresh()

        # ── download ───────────────────────────────────────────────────────
        elif cmd in ("download", "d", "dl"):
            if not arg:
                self._log.write("[yellow]Usage: download <filename>[/]")
                return
            self._log.write(f"Downloading [bold]{arg}[/]…")
            resp, data = await self._coord.download(arg)
            if data:
                Path(arg).write_bytes(data)
                self._log.write(f"[green]✓ Saved {arg}  ({fmt_bytes(len(data))})[/]")
            else:
                self._log.write(f"[red]{resp}[/]")

        # ── delete ─────────────────────────────────────────────────────────
        elif cmd in ("delete", "del", "rm"):
            if not arg:
                self._log.write("[yellow]Usage: delete <filename>[/]")
                return
            resp = await self._coord.delete(arg)
            if resp.startswith("OK"):
                self._log.write(f"[green]✓ Deleted {arg}[/]")
            else:
                self._log.write(f"[red]{resp}[/]")
            await self._refresh()

        # ── kill_node ──────────────────────────────────────────────────────
        elif cmd in ("kill_node", "kill"):
            if not arg:
                self._log.write("[yellow]Usage: kill_node <node_id>[/]")
                return
            try:
                nid = int(arg)
            except ValueError:
                self._log.write("[red]Node id must be a number[/]")
                return
            resp = await self._coord.kill_node(nid)
            if resp.startswith("OK"):
                self._log.write(f"[red]*** NODE DOWN: Node {nid} killed ***[/]")
            else:
                self._log.write(f"[red]{resp}[/]")
            await self._refresh()

        # ── revive_node ────────────────────────────────────────────────────
        elif cmd in ("revive_node", "revive"):
            if not arg:
                self._log.write("[yellow]Usage: revive_node <node_id>[/]")
                return
            try:
                nid = int(arg)
            except ValueError:
                self._log.write("[red]Node id must be a number[/]")
                return
            resp = await self._coord.revive_node(nid)
            if resp.startswith("OK"):
                self._log.write(f"[yellow]Node {nid} restarting — health check will confirm when online[/]")
            else:
                self._log.write(f"[red]{resp}[/]")
            await self._refresh()

        # ── help ───────────────────────────────────────────────────────────
        elif cmd in ("help", "h", "?"):
            self._log.write(
                "[bold]Commands[/]\n"
                "  [cyan]list[/] / [cyan]l[/] / [cyan]refresh[/]        refresh all panels\n"
                "  [cyan]upload[/] [dim]<local-path>[/]       upload a local file to the DFS\n"
                "  [cyan]download[/] [dim]<filename>[/]       download a file to this directory\n"
                "  [cyan]delete[/] [dim]<filename>[/]         delete a file from the DFS\n"
                "  [cyan]kill_node[/] [dim]<id>[/]            kill a storage node\n"
                "  [cyan]revive_node[/] [dim]<id>[/]          revive a killed storage node\n"
                "  [cyan]quit[/] / [cyan]q[/]                 exit\n"
                "  [cyan]F5[/] / [cyan]Ctrl+L[/]             refresh everything"
            )

        else:
            self._log.write(f"[red]Unknown command:[/] {cmd}  (type [bold]help[/])")


# ── entry point ────────────────────────────────────────────────────────────

if __name__ == "__main__":
    DFSApp().run()
