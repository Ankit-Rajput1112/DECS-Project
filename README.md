# HTTP-Based Key-Value Server (Phase 1)    

## Overview
This project implements a **multi-tier HTTP-based Key-Value (KV) store** with an in-memory cache and a persistent PostgreSQL backend.  
It supports concurrent client connections and basic **CRUD** (Create, Read, Update, Delete) operations through RESTful APIs.  

Phase 1 demonstrates a **functionally correct system** consisting of:
- A multithreaded HTTP server handling client requests  
- An LRU-based in-memory cache  
- A persistent PostgreSQL database  
- An initial load generator for simple concurrency testing  

---

## 📁 Repository Structure
```
.
├── server.cpp              # Main HTTP server
├── lru_cache.h             # In-memory LRU cache
├── pg_store.h              # PostgreSQL database interface
├── include/httplib.h       # HTTP library
├── include/json.hpp        # JSON parser
├── init_sql.sh             # DB initialization script
├── Makefile                # Build configuration
└── README.md               # Project documentation
```

---

## System Architecture
The system follows a **three-tier design**:

+----------------------------+
|           Clients          |
|  (Concurrent HTTP Threads) |
+-------------+--------------+
              |
              v
+-----------------------------------+
|   HTTP Server (Frontend)          |
|   - Thread Pool                   |
|   - REST API Handlers             |
|   - Cache Access Layer            |
+-------------+---------------------+
     | Cache Hit             | Cache Miss
     v                       v
+----------------+    +----------------------+
| In-Memory LRU  |    | PostgreSQL Database  |
| KV Cache       |    | Persistent Storage   |
+----------------+    +----------------------+

---

## Components

### 1. HTTP Server (`server.cpp`)
- Built using **cpp-httplib** for lightweight RESTful HTTP handling.  
- Supports concurrent clients using a **thread pool**.  
- Routes implemented:
  - `POST /kv` — Create key-value pair  
  - `GET /kv/{key}` — Retrieve value  
  - `DELETE /kv/{key}` — Remove key  

### 2. In-Memory Cache (`lru_cache.h`)
- Implements **LRU (Least Recently Used)** eviction policy.  
- Reduces database access latency by caching frequent keys.  
- Thread-safe design using mutex locks.  

### 3. Persistent Storage (`pg_store.h`, `init_sql.sh`)
- PostgreSQL used for durable KV persistence.  
- Stores all key-value pairs in a simple table:
  ```sql
  CREATE TABLE kv_store (
      key TEXT PRIMARY KEY,
      value TEXT
  );
  ```
- Data consistency ensured between cache and database.  

### 4. Load Generator
- Simulates multiple concurrent clients using threads.  
- Sends HTTP requests (GET/POST/DELETE) to the server.  
- Used to validate functional correctness and concurrency.

---

## Build and Run Instructions

### **1. Install Dependencies**
- PostgreSQL (server and client)
- g++ / clang++ (C++17)  
- libpq for PostgreSQL C++ connector  

### **2. Initialize Database**
Start postgresql service and Run the included SQL setup script:
```bash
sudo service postgresql start
PGHOST=localhost PGPORT=5432 PGDATABASE=kvdb PGUSER=kvuser PGPASSWORD=kvpass ./init_sql.sh
```

### **3. Build Server**
```bash
make
```

### **4. Run Server**
```bash
PGHOST=localhost PGPORT=5432 PGDATABASE=kvdb PGUSER=ankit PGPASSWORD='ankit@123' ./server 8080 1000
```

---

## Usage Examples

1. **Create Key-Value Pair**
```bash
curl -X PUT localhost:8080/kv/ankit -d "shekhawat"
```

2. **Read Key**
```bash
curl http://localhost:8080/kv/ankit
```

3. **Delete Key**
```bash
curl -X DELETE http://localhost:8080/kv/ankit
```