# HTTP-Based Key-Value Server

## Overview

This project implements a **high-performance, multi-tier Key-Value (KV) data store** featuring:

* A multithreaded **HTTP REST server**
* A **thread-safe LRU cache** for low-latency reads
* A **PostgreSQL persistent backend**
* **Real-time metrics**: cache hits, misses, request counts
* A powerful **load generator** that measures throughput, latency, CPU %, and IO %
* **Performance graphs** to identify CPU, memory, or disk bottlenecks

The system is designed to explore bottlenecks in a layered storage system — CPU, cache, or storage — under increasing load.

---

## Repository Structure

```
.
├── server.cpp                      # Main HTTP server with metrics
├── server_metrics_additions.cpp    # /metrics endpoint
├── lru_cache.h                     # In-memory LRU cache
├── pg_store.h / pg_store.cpp       # PostgreSQL interface
├── include/httplib.h               # HTTP framework
├── include/json.hpp                # JSON parser
├── loadgen.cpp                     # Load generator (Phase 2)
├── results.csv                     # Loadgen CSV output
├── graphs/                         # Generated performance plots
├── init_sql.sh                     # Database initialization script
├── Makefile                        # Build configuration
└── README.md                       # Project documentation
```

---

# System Architecture

```
+-----------------------------------------------------------+
|                        Clients                            |
|           (thousands of concurrent HTTP requests)         |
+------------------------------+----------------------------+
                               |
                               v
+-----------------------------------------------------------+
|                   HTTP REST Server (Frontend)             |
|   - cpp-httplib based routing                              |
|   - Thread pool for concurrency                           |
|   - /metrics endpoint (JSON)                              |
+---------------+----------------------+---------------------+
                | Cache Hit            | Cache Miss
                v                       v
+----------------------------+    +--------------------------+
|       LRU In-memory Cache  |    |   PostgreSQL Database    |
|  - O(1) get/put operations |    |   Persistent KV storage  |
+----------------------------+    +--------------------------+
```

---

# Features

## ✔ 1. Fully Functional HTTP KV Store

Endpoints:

* `PUT /kv/{key}` → Insert/update value
* `GET /kv/{key}` → Read value
* `DELETE /kv/{key}` → Remove

All KV operations update both **cache** and **PostgreSQL**.

---

## ✔ 2. Thread-Safe LRU Cache

* Configurable size
* Mutex-protected for multi-threaded correctness
* Tracks cache **hits** and **misses**

---

## ✔ 3. Persistent Storage — PostgreSQL

Schema:

```sql
CREATE TABLE kv_store (
    key TEXT PRIMARY KEY,
    value TEXT
);
```

The DB stores all key-value pairs for durability.

---

## ✔ 4. Real-time Metrics (`/metrics`)

Example output:

```json
{
  "total_requests": 123400,
  "total_success": 120510,
  "total_errors": 2990,
  "cache_hits": 98500,
  "cache_misses": 21800,
  "uptime_seconds": 420,
  "timestamp_ms": 1732399200000
}
```

Metrics help observe:

* Hit ratio
* Server request rate
* Cache efficiency

---

## ✔ 5. Advanced Load Generator

The provided `loadgen` tool supports:

### Workloads:

* `workload=get` → pure reads
* `workload=set` → pure writes
* `workload=mix` → 50/50 read/write

### Metrics captured:

* Total requests
* Success & errors
* Throughput (req/s)
* Average latency
* **CPU utilization (%)**
* **IO utilization (%)**
* Writes results automatically to `results.csv`

Example CSV:

```
clients,throughput,avg_latency_ms,cpu_util_pct,io_util_pct
1,4823,0.21,18.3,2.1
2,8190,0.24,35.2,3.1
3,8270,0.36,52.8,4.0
```

---


# Installation & Setup

## 1. Install Dependencies

```bash
sudo apt install postgresql libpq-dev g++ make python3-matplotlib
```

## 2. Initialize PostgreSQL

```bash
sudo service postgresql start
./init_sql.sh
```

## 3. Build Server

```bash
make
```

## 4. Run Server

```bash
PGHOST=localhost PGPORT=5432 PGDATABASE=kvdb \
PGUSER=ankit PGPASSWORD='ankit@123' ./server 8080 1000
```

---

# Running Load Tests

## Example: 10 clients, 20 seconds, mixed workload

```bash
taskset -c 8-15 ./loadgen 127.0.0.1 8080 10 20 mix
```

The script logs:

* Throughput
* Latency
* CPU %
* IO %
  And stores results in CSV.

---

# Example Metrics-driven Analysis

### Throughput curve:

* Fast growth from 1 → 2 → 3 clients
* Saturation at 4–6 clients

### Latency curve:

* Low until CPU reaches 80% utilization
* Sharp rise after saturation point

### CPU vs IO utilization:

* CPU dominates before IO → CPU bottleneck
* IO rises only when DB operations increase heavily

---


# Conclusion

This project provides a complete **KV storage system** with:

* Clean architecture
* High performance
* Real-time metrics
* Advanced load testing
* Bottleneck evaluation

It demonstrates how system performance evolves across layers — HTTP → Cache → DB → Hardware.
