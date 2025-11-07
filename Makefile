CXX = g++
CXXFLAGS = -std=c++17 -O2 -pthread -Iinclude -I/usr/include/postgresql
LIBS = -lpq

all: server

# server depends on httplib single-header
server: server.cpp pg_store.h lru_cache.h
	$(CXX) $(CXXFLAGS) server.cpp -o server $(LIBS)

clean:
	rm -f server
