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

### Installation and setup
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

### Usage and Examples

### Team Contributions:
**Hayden:**

**Jacob:**

**Joey:**

**Kyle:**
