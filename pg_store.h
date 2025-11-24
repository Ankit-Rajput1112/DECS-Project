#include <libpq-fe.h>
#include <string>
#include <sstream>
#include <iostream>
#include <mutex>
#include <cstdlib>
#include <atomic>
#include <chrono>

class PGStore {
public:
    PGStore(): conn(nullptr),
               get_queries_(0), put_queries_(0),
               total_db_ns_(0) {}

    ~PGStore(){ disconnect(); }

    // Connect using libpq conninfo string
    bool connect(const std::string &conninfo) {
        std::lock_guard<std::mutex> lk(mtx);
        if (conn) PQfinish(conn);
        conn = PQconnectdb(conninfo.c_str());
        return check_conn_locked();
    }

    // Build conninfo from environment variables if present and connect
    bool connect_from_env() {
        const char* host = std::getenv("PGHOST");
        const char* port = std::getenv("PGPORT");
        const char* dbname = std::getenv("PGDATABASE");
        const char* user = std::getenv("PGUSER");
        const char* pass = std::getenv("PGPASSWORD");

        std::ostringstream ss;
        if (host) ss << "host=" << host << " ";
        if (port) ss << "port=" << port << " ";
        if (dbname) ss << "dbname=" << dbname << " ";
        if (user) ss << "user=" << user << " ";
        if (pass) ss << "password=" << pass << " ";
        ss << "connect_timeout=5";
        return connect(ss.str());
    }

    void disconnect() {
        std::lock_guard<std::mutex> lk(mtx);
        if (conn) {
            PQfinish(conn);
            conn = nullptr;
        }
    }

    // Ensure table exists
    bool ensure_table() {
        std::lock_guard<std::mutex> lk(mtx);
        if (!conn) { last_err = "not connected"; return false; }
        const char* q = "CREATE TABLE IF NOT EXISTS kv_store (key TEXT PRIMARY KEY, value BYTEA);";
        PGresult* r = PQexec(conn, q);
        if (!r) { last_err = "no result from CREATE TABLE"; return false; }
        bool ok = (PQresultStatus(r) == PGRES_COMMAND_OK);
        if (!ok) last_err = PQresultErrorMessage(r);
        PQclear(r);
        return ok;
    }

    // Get value for key
    bool get(const std::string &key, std::string &value) {
        auto t0 = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lk(mtx);
        if (!conn) { last_err = "not connected"; return false; }

        const char* paramValues[1];
        int paramLengths[1];
        int paramFormats[1];
        paramValues[0] = key.c_str();
        paramLengths[0] = static_cast<int>(key.size());
        paramFormats[0] = 0; // send param as text

        // Request the value column as binary
        PGresult* res = PQexecParams(conn,"SELECT value FROM kv_store WHERE key = $1",
                                     1,          // nParams
                                     nullptr,    // paramTypes
                                     paramValues,
                                     paramLengths,
                                     paramFormats,
                                     1);         // resultFormat: 1 => binary
        if (!res) { last_err = "no result"; return false; }

        ExecStatusType st = PQresultStatus(res);
        if (st != PGRES_TUPLES_OK) {
            last_err = PQresultErrorMessage(res);
            PQclear(res);
            return false;
        }
        if (PQntuples(res) == 0) {
            PQclear(res);
            record_db_time(t0);
            // not found -> not an error, return false
            return false;
        }

        int len = PQgetlength(res, 0, 0);
        const char* ptr = PQgetvalue(res, 0, 0);
        if (ptr && len > 0) {
            value.assign(ptr, ptr + len);
        } else {
            value.clear();
        }
        PQclear(res);

        get_queries_.fetch_add(1, std::memory_order_relaxed);
        record_db_time(t0);
        return true;
    }

    // Insert/update key
    bool put(const std::string &key, const std::string &value) {
        auto t0 = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lk(mtx);
        if (!conn) { last_err = "not connected"; return false; }

        const char* paramValues[2];
        int paramLengths[2];
        int paramFormats[2];

        paramValues[0] = key.c_str();
        paramLengths[0] = static_cast<int>(key.size());
        paramFormats[0] = 0; // key as text

        paramValues[1] = value.data();
        paramLengths[1] = static_cast<int>(value.size());
        paramFormats[1] = 1; // value as binary (bytea)

        PGresult* res = PQexecParams(conn,
                                     "INSERT INTO kv_store(key, value) VALUES($1, $2) "
                                     "ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value",
                                     2, nullptr, paramValues, paramLengths, paramFormats, 0); // resultFormat 0 (text)
        if (!res) { last_err = "no result"; return false; }
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            last_err = PQresultErrorMessage(res);
            PQclear(res);
            return false;
        }
        PQclear(res);

        put_queries_.fetch_add(1, std::memory_order_relaxed);
        record_db_time(t0);
        return true;
    }

    // Delete key
    bool del(const std::string &key) {
        auto t0 = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lk(mtx);
        if (!conn) { last_err = "not connected"; return false; }

        const char* paramValues[1];
        int paramLengths[1];
        int paramFormats[1];

        paramValues[0] = key.c_str();
        paramLengths[0] = static_cast<int>(key.size());
        paramFormats[0] = 0; // text

        PGresult* res = PQexecParams(conn,
                                     "DELETE FROM kv_store WHERE key = $1",
                                     1, nullptr, paramValues, paramLengths, paramFormats, 0);
        if (!res) { last_err = "no result"; return false; }
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            last_err = PQresultErrorMessage(res);
            PQclear(res);
            return false;
        }
        PQclear(res);

        put_queries_.fetch_add(1, std::memory_order_relaxed); // count deletes as writes
        record_db_time(t0);
        return true;
    }

    // Thread-safe last error string
    std::string last_error() {
        std::lock_guard<std::mutex> lk(mtx);
        return last_err;
    }

    // DB statistics (for metrics)
    uint64_t stats_get_queries() const noexcept {
        return get_queries_.load(std::memory_order_relaxed);
    }
    uint64_t stats_put_queries() const noexcept {
        return put_queries_.load(std::memory_order_relaxed);
    }
    // average DB latency in milliseconds
    double stats_avg_db_latency_ms() const noexcept {
        uint64_t cnt = get_queries_.load(std::memory_order_relaxed) + put_queries_.load(std::memory_order_relaxed);
        if (cnt == 0) return 0.0;
        uint64_t total_ns = total_db_ns_.load(std::memory_order_relaxed);
        double avg_ms = (double)total_ns / 1e6 / (double)cnt;
        return avg_ms;
    }

private:
    PGconn* conn;
    mutable std::mutex mtx;
    std::string last_err;

    // DB stats
    std::atomic<uint64_t> get_queries_;
    std::atomic<uint64_t> put_queries_;
    std::atomic<uint64_t> total_db_ns_; // accumulated ns for read/write ops

    // internal helper: check connection is OK
    bool check_conn_locked() {
        if (!conn) return false;
        if (PQstatus(conn) != CONNECTION_OK) {
            last_err = PQerrorMessage(conn);
            PQfinish(conn);
            conn = nullptr;
            return false;
        }
   
        PQexec(conn, "SET client_min_messages = WARNING;");
        return true;
    }
   
    
    bool check_conn() {
        std::lock_guard<std::mutex> lk(mtx);
        return check_conn_locked();
    }

    // record elapsed time since t0 into total_db_ns_
    void record_db_time(const std::chrono::steady_clock::time_point &t0) {
        auto t1 = std::chrono::steady_clock::now();
        uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        total_db_ns_.fetch_add(ns, std::memory_order_relaxed);
    }
};
