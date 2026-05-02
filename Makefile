CXX      = g++
CXXFLAGS = -std=c++17 -Wall -pthread -Iinclude
# -MMD -MP: auto-generate .d dependency files so header changes trigger recompilation
DEPFLAGS = -MMD -MP

# ── source lists ──────────────────────────────────────────────────────────────
SERVER_SRCS   = tcpServer/server.cpp src/node_server.cpp src/tcp_helpers.cpp node/nodeinternal.cpp
CLIENT_SRCS   = src/client.cpp src/tcp_helpers.cpp
NODETEST_SRCS = node/nodeinternal.cpp node/nodeinternal-test.cpp

# ── object files (compiled into build/ to keep root clean) ────────────────────
SERVER_OBJS   = $(patsubst %.cpp, build/%.o, $(SERVER_SRCS))
CLIENT_OBJS   = $(patsubst %.cpp, build/%.o, $(CLIENT_SRCS))
NODETEST_OBJS = $(patsubst %.cpp, build/%.o, $(NODETEST_SRCS))

# ── dependency files (auto-generated, one per .o) ─────────────────────────────
DEPS = $(SERVER_OBJS:.o=.d) $(CLIENT_OBJS:.o=.d) \
       $(NODETEST_OBJS:.o=.d)

# ── output directory ──────────────────────────────────────────────────────────
BINDIR = bin

# ── primary targets ───────────────────────────────────────────────────────────
.PHONY: all server client node-test clean test test-edge run-server run-client tui run-tui

all: $(BINDIR)/server $(BINDIR)/client

$(BINDIR)/server: $(SERVER_OBJS)
	@mkdir -p $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BINDIR)/client: $(CLIENT_OBJS)
	@mkdir -p $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BINDIR)/node-test: $(NODETEST_OBJS)
	@mkdir -p $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

# ── generic compile rule ──────────────────────────────────────────────────────
# Compiles any .cpp -> build/<same-subpath>.o and emits a .d file beside it.
build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Pull in auto-generated header dependencies (silently ignored if missing)
-include $(DEPS)

# ── convenience targets ───────────────────────────────────────────────────────
run-server: $(BINDIR)/server
	./$(BINDIR)/server

run-client: $(BINDIR)/client
	./$(BINDIR)/client localhost

tui:
	pip install --quiet textual

run-tui: tui
	python3 dfs_tui.py

test: $(BINDIR)/server
	python3 testing/test_client.py

test-edge: $(BINDIR)/server
	python3 testing/test_edge_cases.py

# ── cleanup ───────────────────────────────────────────────────────────────────
clean:
	rm -rf build bin server client node-test
