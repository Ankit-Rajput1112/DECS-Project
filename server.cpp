// KV HTTP server using cpp-httplib and PostgreSQL (libpq)

#include "httplib.h"
#include <libpq-fe.h>

#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <iomanip>

#include "lru_cache.h"
#include "pg_store.h"

#include "json.hpp"
using json = nlohmann::json;

using namespace std;

static volatile std::sig_atomic_t stop_flag = 0;
void handle_signal(int) { stop_flag = 1; }

// Forward declarations for metrics (defined in server_metrics_additions.cpp)
#include <atomic>
extern std::atomic<uint64_t> g_total_requests;
extern std::atomic<uint64_t> g_total_success;
extern std::atomic<uint64_t> g_total_errors;
extern std::atomic<uint64_t> g_cache_hits;
extern std::atomic<uint64_t> g_cache_misses;

// register_metrics_endpoint defined in server_metrics_additions.cpp
void register_metrics_endpoint(httplib::Server &svr, class PGStore *db = nullptr);

// Logging helpers
static std::mutex g_log_mtx;

static std::string now_str() {
    using namespace std::chrono;
    auto t = system_clock::now();
    auto tt = system_clock::to_time_t(t);
    auto ms = duration_cast<milliseconds>(t.time_since_epoch()).count() % 1000;
    std::ostringstream ss;
    ss << std::put_time(std::localtime(&tt), "%F %T") << "." << std::setw(3) << std::setfill('0') << ms;
    return ss.str();
}

static void log_info(const std::string &msg) {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    std::cout << "[" << now_str() << "] " << msg << std::endl;
}


int main(int argc, char** argv) {
    int port = 8080;
    size_t cache_capacity = 1000;
    string pg_conninfo;             // if empty, will use defaults or env

    if (argc >= 2) port = atoi(argv[1]);
    if (argc >= 3) cache_capacity = atol(argv[2]);
    if (argc >= 4) pg_conninfo = argv[3];

    cout << "Starting KV HTTP server (httplib + PostgreSQL) on port " << port << "\n";
    cout << "Cache capacity: " << cache_capacity << "\n";
    if (!pg_conninfo.empty()) cout << "Using PG conninfo: " << pg_conninfo << "\n";

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Thread-safe usage: protect cache and DB with mutexes
    LRUCache cache(cache_capacity);
    static std::mutex g_cache_mtx;
    static std::mutex g_db_mtx;

    PGStore db;
    if (!pg_conninfo.empty()) {
        if (!db.connect(pg_conninfo)) {
            cerr << "Failed to connect to Postgres with provided conninfo\n";
            return 1;
        }
    } else {
        if (!db.connect_from_env()) {
            cerr << "Failed to connect to Postgres using environment/defaults\n";
            return 1;
        }
    }
    
    if (!db.ensure_table()) {
        cerr << "Failed to ensure kv table exists\n";
        return 1;
    }

    httplib::Server svr;

    
    register_metrics_endpoint(svr, &db);

    // prints cache hits/misses and basic counters every 30s
    std::thread metrics_logger([&](){
        uint64_t last_total_requests = 0;
        while (!stop_flag) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            uint64_t hits = g_cache_hits.load();
            uint64_t misses = g_cache_misses.load();
            uint64_t total = g_total_requests.load();
            uint64_t success = g_total_success.load();
            uint64_t errors = g_total_errors.load();
            uint64_t delta = total - last_total_requests;
            last_total_requests = total;
            double hit_rate = (hits + misses > 0) ? (100.0 * (double)hits / (double)(hits + misses)) : 0.0;
            std::ostringstream ss;
            ss << "[CACHE-METRICS] hits=" << hits
               << " misses=" << misses
               << " hit_rate=" << std::fixed << std::setprecision(2) << hit_rate << "%"
               << " total_requests=" << total
               << " (+ " << delta << " in last 30s)"
               << " success=" << success
               << " errors=" << errors;
            log_info(ss.str());
        }
    });
    metrics_logger.detach();

    // Read key (GET)
    svr.Get(R"(/kv/(.+))", [&](const httplib::Request& req, httplib::Response& res){
        g_total_requests.fetch_add(1);

        string key = req.matches[1];
        string val;
        json j;

        // Check cache under lock
        {
            std::lock_guard<std::mutex> lk(g_cache_mtx);
            if (cache.get(key, val)) {
                g_cache_hits.fetch_add(1);
                // Log cache hit
                {
                    std::ostringstream ss;
                    ss << "CACHE HIT key=\"" << key << "\"";
                    log_info(ss.str());
                }

                j["status"] = "ok";
                j["value"] = val;
                res.set_content(j.dump() + "\n", "application/json");
                g_total_success.fetch_add(1);
                return;
            } else {
                g_cache_misses.fetch_add(1);
                // Log cache miss
                {
                    std::ostringstream ss;
                    ss << "CACHE MISS key=\"" << key << "\"";
                    log_info(ss.str());
                }
            }
        }

        // Not in cache: fetch from DB (serialize DB calls to be safe)
        {
            std::lock_guard<std::mutex> lk(g_db_mtx);
            if (db.get(key, val)) {
                // log DB GET success
                {
                    std::ostringstream ss;
                    ss << "DB GET key=\"" << key << "\" len=" << val.size();
                    log_info(ss.str());
                }

                // insert into cache under lock
                {
                    std::lock_guard<std::mutex> lk2(g_cache_mtx);
                    cache.put(key, val);
                    // Log cache put
                    std::ostringstream ss;
                    ss << "CACHE PUT key=\"" << key << "\" (from DB)";
                    log_info(ss.str());
                }

                j["status"] = "ok";
                j["value"] = val;
                res.set_content(j.dump() + "\n", "application/json");
                g_total_success.fetch_add(1);
                return;
            } else {
                // if db.get failed, log last_error if any
                std::string dbe = db.last_error();
                if (!dbe.empty()) {
                    std::ostringstream ss;
                    ss << "DB GET ERROR key=\"" << key << "\" err=\"" << dbe << "\"";
                    log_info(ss.str());
                } else {
                    // not found - already will return 404
                    std::ostringstream ss;
                    ss << "DB GET NOTFOUND key=\"" << key << "\"";
                    log_info(ss.str());
                }
            }
        }

        // not found
        j["status"] = "error";
        j["error"] = "Key not found";
        res.status = 404;
        res.set_content(j.dump() + "\n", "application/json");
        g_total_errors.fetch_add(1);
    });

    // Create or Update key-value pair (PUT)
    svr.Put(R"(/kv/(.+))", [&](const httplib::Request& req, httplib::Response& res){
        g_total_requests.fetch_add(1);

        string key = req.matches[1];
        string body = req.body;
        json j;
        string value_to_store;
        
        try {
            auto parsed = json::parse(body);
            if (parsed.is_object() && parsed.contains("value")) {
                value_to_store = parsed["value"].get<std::string>();
            } else {
                // store raw JSON string
                value_to_store = body;
            }
        } catch (const std::exception &e) {
            // not JSON: store raw body
            value_to_store = body;
        }

        // Log incoming PUT
        {
            std::ostringstream ss;
            ss << "REQ PUT key=\"" << key << "\" body_len=" << value_to_store.size();
            log_info(ss.str());
        }

        // DB write (serialize DB calls)
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(g_db_mtx);
            ok = db.put(key, value_to_store);
            if (ok) {
                std::ostringstream ss;
                ss << "DB PUT key=\"" << key << "\" len=" << value_to_store.size();
                log_info(ss.str());
            } else {
                std::ostringstream ss;
                ss << "DB PUT ERROR key=\"" << key << "\" err=\"" << db.last_error() << "\"";
                log_info(ss.str());
            }
        }
        if (!ok) {
            j["status"] = "error";
            j["error"] = "DB write failed";
            res.status = 500;
            res.set_content(j.dump() + "\n", "application/json");
            g_total_errors.fetch_add(1);
            return;
        }

        // update cache under lock
        {
            std::lock_guard<std::mutex> lk(g_cache_mtx);
            cache.put(key, value_to_store);
            // Log cache put/update
            std::ostringstream ss;
            ss << "CACHE PUT key=\"" << key << "\" len=" << value_to_store.size();
            log_info(ss.str());
        }

        j["status"] = "ok";
        res.status = 201;
        res.set_content(j.dump() + "\n", "application/json");
        g_total_success.fetch_add(1);
    });

    // DELETE key-value pair
    svr.Delete(R"(/kv/(.+))", [&](const httplib::Request& req, httplib::Response& res){
        g_total_requests.fetch_add(1);

        string key = req.matches[1];
        json j;

        // Log incoming DELETE
        {
            std::ostringstream ss;
            ss << "REQ DELETE key=\"" << key << "\"";
            log_info(ss.str());
        }

        bool ok = false;
        std::string last_err;
        {
            std::lock_guard<std::mutex> lk(g_db_mtx);
            ok = db.del(key);
            last_err = db.last_error();
            if (ok) {
                std::ostringstream ss;
                ss << "DB DELETE key=\"" << key << "\"";
                log_info(ss.str());
            } else if (!last_err.empty()) {
                std::ostringstream ss;
                ss << "DB DELETE ERROR key=\"" << key << "\" err=\"" << last_err << "\"";
                log_info(ss.str());
            } else {
                // delete returned false but no error -> likely not found
                std::ostringstream ss;
                ss << "DB DELETE NOTFOUND key=\"" << key << "\"";
                log_info(ss.str());
            }
        }

        if (!ok) {
            if (!last_err.empty()) {
                j["status"] = "error";
                j["error"] = string("DB delete error: ") + last_err;
                res.status = 500;
                res.set_content(j.dump() + "\n", "application/json");
                g_total_errors.fetch_add(1);
                return;
            } else {
                // not found
                j["status"] = "error";
                j["error"] = "Key not found";
                res.status = 404;
                res.set_content(j.dump() + "\n", "application/json");
                g_total_errors.fetch_add(1);
                return;
            }
        }

        // remove from cache under lock
        {
            std::lock_guard<std::mutex> lk(g_cache_mtx);
            cache.erase(key);
            std::ostringstream ss;
            ss << "CACHE ERASE key=\"" << key << "\"";
            log_info(ss.str());
        }

        j["status"] = "ok";
        j["message"] = "Deleted";
        res.status = 200;
        res.set_content(j.dump() + "\n", "application/json");
        g_total_success.fetch_add(1);
    });

    // Health check
    svr.Get("/health", [&](const httplib::Request&, httplib::Response& res){
        json j;
        j["status"] = "ok";
        res.set_content(j.dump() + "\n", "application/json");
    });

    // run server
    std::thread server_thread([&](){
        svr.listen("0.0.0.0", port);
    });

    // wait until signal
    while (!stop_flag) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    cout << "Shutting down server...\n";
    svr.stop();
    if (server_thread.joinable()) server_thread.join();
    db.disconnect();
    return 0;
}
