CXX      = g++
CXXFLAGS = -std=c++17 -Wall -pthread -I.
# -MMD -MP: auto-generate .d dependency files so header changes trigger recompilation
DEPFLAGS = -MMD -MP

# ── source lists ──────────────────────────────────────────────────────────────
SERVER_SRCS   = tcpServer/server.cpp node_server.cpp tcp_helpers.cpp node/nodeinternal.cpp
CLIENT_SRCS   = client.cpp tcp_helpers.cpp
NODETEST_SRCS = node/nodeinternal.cpp node/nodeinternal-test.cpp
MANAGER_SRCS  = manager.cpp   # old standalone prototype, not part of 'all'

# ── object files (compiled into build/ to keep root clean) ────────────────────
SERVER_OBJS   = $(patsubst %.cpp, build/%.o, $(SERVER_SRCS))
CLIENT_OBJS   = $(patsubst %.cpp, build/%.o, $(CLIENT_SRCS))
NODETEST_OBJS = $(patsubst %.cpp, build/%.o, $(NODETEST_SRCS))
MANAGER_OBJS  = $(patsubst %.cpp, build/%.o, $(MANAGER_SRCS))

# ── dependency files (auto-generated, one per .o) ─────────────────────────────
DEPS = $(SERVER_OBJS:.o=.d) $(CLIENT_OBJS:.o=.d) \
       $(NODETEST_OBJS:.o=.d) $(MANAGER_OBJS:.o=.d)

# ── primary targets ───────────────────────────────────────────────────────────
.PHONY: all server client node-test manager clean test test-edge run-server run-client tui run-tui

all: server client

server: $(SERVER_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

client: $(CLIENT_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

node-test: $(NODETEST_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

manager: $(MANAGER_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

# ── generic compile rule ──────────────────────────────────────────────────────
# Compiles any .cpp -> build/<same-subpath>.o and emits a .d file beside it.
build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Pull in auto-generated header dependencies (silently ignored if missing)
-include $(DEPS)

# ── convenience targets ───────────────────────────────────────────────────────
run-server: server
	./server

run-client: client
	./client localhost

tui:
	pip install --quiet textual

run-tui: tui
	python3 dfs_tui.py

test: server
	python3 testing/test_client.py

test-edge: server
	python3 testing/test_edge_cases.py

# ── cleanup ───────────────────────────────────────────────────────────────────
clean:
	rm -rf build server client node-test manager
