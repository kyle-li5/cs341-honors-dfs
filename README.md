# CS341 Honors Distributed File System
We are creating a Raspberry Pi NAS Server From Scratch (Distributed File System)

Our goals were to: 
* Handle client-server communication through TCP/IP
* Enable clients to perform basic actions such as storing, retrieving, manipulating, and deleting files from the server
* Share stored data across multiple nodes for redundancy
* Have load-balancing between server nodes

We used one Pi and simulated node communication and distribution with processes.

The DFS support uploading, downloading, and deleting files stored in the server, and we also provide a client TUI that presents info on the status of nodes and files.

Currently our DFS uses three processes to simulate three nodes.

## Installation and setup
### Prerequisites
*Server is intended to be run on a Linux-based system (Raspberry Pi)*
* Git
* Python 3
* Pip

### Steps
1. **Clone the repository**
   ```bash
   git clone https://github.com/kyle-li5/cs341-honors-dfs.git
   cd cs341-honors-dfs
   ```
2. **Create virtual environment**
   ```bash
   python -m venv venv
   ```
   Activate it:
   ```bash
   source venv/bin/activate
   ```
   *If activating it doesn't work try running ```sudo apt update && sudo apt install python3-venv```*
3. **Install dependencies**
    ```bash
   make tui
   ```
4. **Run makefile**
   ```bash
   make clean
   make
   ```
5. **Start the server**
   ```bash
   make run-server
   ```
   Or you can manually execute the binary executable:
   ```bash
   ./bin/server
   ```
6. **Start the client**

   In another process or on another machine, you can run the client:
   ```bash
   make run-tui
   ```
   Or you can manually run the python file:
   ```bash
   python3 src/dfs_tui.py
   ```
   If you are using the same computer for both the server and client, you can run dfs_tui with no arguments for localhost.
   You can also run
   ```bash
   python3 src/dfs_tui.py raspberrypi.local 9000
   ```
   Or
   ```bash
   python3 src/dfs_tui.py [hostname/IP] [port]
   ```
   To connect to the server by hostname or ip address.

## Usage and Examples
### TUI when first starting the app:
<img width="1212" height="1031" alt="341h-ss1" src="https://github.com/user-attachments/assets/bc9dbc2e-0cc5-442a-8ba8-169c3e070adb" />

### Typing "list" refreshes the interface:
<img width="1212" height="1025" alt="341h-ss1-refresh" src="https://github.com/user-attachments/assets/f6639aae-f967-482d-a1d6-a47ebefd7a60" />

### Can upload files using the "upload" command:
<img width="1212" height="1028" alt="341h-ss-upload1" src="https://github.com/user-attachments/assets/4889fcab-9aa5-457e-a824-dbdf572e1cfe" />
<img width="1210" height="1022" alt="341h-ss-upload2" src="https://github.com/user-attachments/assets/a9eff6d4-276a-4cf4-ade1-aa8e037d505e" />

### Can download files using the "download" command:
<img width="1212" height="1023" alt="341h-ss-download1" src="https://github.com/user-attachments/assets/bea08d25-6619-4905-b475-49442153f893" />
<img width="1212" height="1022" alt="341h-ss-download2" src="https://github.com/user-attachments/assets/c85e6fd0-0bd2-4305-8220-8f562fa72a4d" />

### Can delete files using the "delete" command:
<img width="1208" height="1023" alt="341h-ss-delete1" src="https://github.com/user-attachments/assets/abb9ed96-5e94-426c-b20c-9b193ed8bc6a" />


## Team Contributions:
**Hayden:** Implemented on NodeInternal class, handled direct interactions with server-side filesystem, and added metadata for identifying errors.

**Jacob:** Implemented fault-tolerance, file replication, and file distribution logic for the server.

**Joey:** Created the UI for the distributed file system, and implemented some tests to confirm basic functionality.

**Kyle:** Implemented server logic to split and distribute files into set-size chunks across nodes as well as create redundant copies of each chunk
