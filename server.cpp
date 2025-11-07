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

#include "lru_cache.h"
#include "pg_store.h"

#include "json.hpp"
using json = nlohmann::json;

using namespace std;

static volatile std::sig_atomic_t stop_flag = 0;

void handle_signal(int) { stop_flag = 1; }

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

    LRUCache cache(cache_capacity);

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

    // Read key
    svr.Get(R"(/kv/(.+))", [&](const httplib::Request& req, httplib::Response& res){
        string key = req.matches[1];
        string val;
        json j;

        if (cache.get(key, val)) {
            j["status"] = "ok";
            j["value"] = val;
            res.set_content(j.dump() + "\n", "application/json");
            return;
        }

        if (db.get(key, val)) {
            cache.put(key, val);
            j["status"] = "ok";
            j["value"] = val;
            res.set_content(j.dump() + "\n", "application/json");
            return;
        }

        j["status"] = "error";
        j["error"] = "Key not found";
        res.status = 404;
        res.set_content(j.dump() + "\n", "application/json");
    });

    // Create or Update key-value pair
    svr.Put(R"(/kv/(.+))", [&](const httplib::Request& req, httplib::Response& res){
        string key = req.matches[1];
        string body = req.body;
        json j;

        if (!db.put(key, body)) {
            j["status"] = "error";
            j["error"] = "DB write failed";
            res.status = 500;
            res.set_content(j.dump() + "\n", "application/json");
            return;
        }

        cache.put(key, body);
        j["status"] = "ok";
        res.status = 201;
        res.set_content(j.dump() + "\n", "application/json");
    });

    // DELETE key-value pair
    svr.Delete(R"(/kv/(.+))", [&](const httplib::Request& req, httplib::Response& res){
        string key = req.matches[1];
        json j;

        if (!db.del(key)) {
            if (!db.last_error().empty()) {
                j["status"] = "error";
                j["error"] = "DB delete error";
                res.status = 500;
                res.set_content(j.dump() + "\n", "application/json");
                return;
            }
        }

        cache.erase(key);
        j["status"] = "ok";
        j["message"] = "Deleted";
        res.status = 200;
        res.set_content(j.dump() + "\n", "application/json");
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
