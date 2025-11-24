CXX = g++
CXXFLAGS = -std=c++17 -O2 -pthread -Wall -Iinclude -I/usr/include/postgresql
SERVER_LIBS = -lpq
LOADLIBS ?=
all: server loadgen

# Build server including metrics implementation
server: server.cpp server_metrics_additions.cpp pg_store.h lru_cache.h
	$(CXX) $(CXXFLAGS) server.cpp server_metrics_additions.cpp -o server $(SERVER_LIBS)

# Build load generator
loadgen: loadgen.cpp
	$(CXX) $(CXXFLAGS) loadgen.cpp -o loadgen $(LOADLIBS)

# debug build
debug: CXXFLAGS += -g -O0 -DCACHE_DEBUG
debug: clean server loadgen

.PHONY: all server loadgen debug clean
clean:
	rm -f server loadgen
