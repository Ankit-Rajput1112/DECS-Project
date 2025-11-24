#include "httplib.h"
#include "json.hpp"
#include <atomic>
#include <chrono>
#include <string>

using json = nlohmann::json;

// Global atomic counters
std::atomic<uint64_t> g_total_requests{0};
std::atomic<uint64_t> g_total_success{0};
std::atomic<uint64_t> g_total_errors{0};
std::atomic<uint64_t> g_cache_hits{0};
std::atomic<uint64_t> g_cache_misses{0};


void register_metrics_endpoint(httplib::Server &svr, class PGStore *db /*=nullptr*/) {
    // capture server start time for uptime calculation
    static auto start_time = std::chrono::steady_clock::now();

    svr.Get("/metrics", [&](const httplib::Request & /*req*/, httplib::Response &res) {
        json m;
        m["total_requests"] = g_total_requests.load();
        m["total_success"] = g_total_success.load();
        m["total_errors"] = g_total_errors.load();
        m["cache_hits"] = g_cache_hits.load();
        m["cache_misses"] = g_cache_misses.load();

        // uptime
        auto now = std::chrono::steady_clock::now();
        auto uptime_s = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        m["uptime_seconds"] = uptime_s;
        m["timestamp_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

        res.set_content(m.dump(), "application/json");
        res.status = 200;
    });
}
