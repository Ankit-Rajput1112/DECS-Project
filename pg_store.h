// Simple PostgreSQL-backed KV store using libpq

#pragma once
#include <libpq-fe.h>
#include <string>
#include <sstream>
#include <iostream>
#include <mutex>
#include <cstdlib>

class PGStore {
public:
    PGStore(): conn(nullptr) {}
    ~PGStore(){ disconnect(); }

    bool connect(const std::string &conninfo) {
        std::lock_guard<std::mutex> lk(mtx);
        conn = PQconnectdb(conninfo.c_str());
        return check_conn();
    }
    bool connect_from_env() {
        // Builds conninfo from environment variables if present, otherwise defaults to localhost
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
    bool ensure_table() {
        const char* q = "CREATE TABLE IF NOT EXISTS kv_store (key TEXT PRIMARY KEY, value BYTEA);";
        PGresult* r = PQexec(conn, q);
        if (!r) return false;
        bool ok = (PQresultStatus(r) == PGRES_COMMAND_OK);
        PQclear(r);
        return ok;
    }
    bool get(const std::string &key, std::string &value) {
        std::lock_guard<std::mutex> lk(mtx);
        const char* paramValues[1];
        int paramLengths[1];
        int paramFormats[1];
        paramValues[0] = key.c_str();
        paramLengths[0] = key.size();
        paramFormats[0] = 0; // text
        PGresult* res = PQexecParams(conn,
                                     "SELECT value FROM kv_store WHERE key = $1",
                                     1, nullptr, paramValues, nullptr, paramFormats, 1);
        if (!res) { last_err = "no result"; return false; }
        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            last_err = PQresultErrorMessage(res);
            PQclear(res);
            return false;
        }
        if (PQntuples(res) == 0) {
            PQclear(res);
            return false;
        }
        int len = PQgetlength(res, 0, 0);
        const char* ptr = PQgetvalue(res, 0, 0);
        value.assign(ptr, ptr + len);
        PQclear(res);
        return true;
    }
    bool put(const std::string &key, const std::string &value) {
        std::lock_guard<std::mutex> lk(mtx);
        const char* paramValues[2];
        int paramLengths[2];
        int paramFormats[2];
        paramValues[0] = key.c_str();
        paramLengths[0] = key.size();
        paramFormats[0] = 0; // text
        paramValues[1] = value.data();
        paramLengths[1] = value.size();
        paramFormats[1] = 1; // binary for bytea
        PGresult* res = PQexecParams(conn,
                                     "INSERT INTO kv_store(key, value) VALUES($1, $2) ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value",
                                     2, nullptr, paramValues, paramLengths, paramFormats, 0);
        if (!res) { last_err = "no result"; return false; }
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            last_err = PQresultErrorMessage(res);
            PQclear(res);
            return false;
        }
        PQclear(res);
        return true;
    }
    bool del(const std::string &key) {
        std::lock_guard<std::mutex> lk(mtx);
        const char* paramValues[1];
        int paramLengths[1];
        int paramFormats[1];
        paramValues[0] = key.c_str();
        paramLengths[0] = key.size();
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
        return true;
    }
    std::string last_error() {
        std::lock_guard<std::mutex> lk(mtx);
        return last_err;
    }
private:
    PGconn* conn;
    std::mutex mtx;
    std::string last_err;
    bool check_conn() {
        if (!conn) return false;
        if (PQstatus(conn) != CONNECTION_OK) {
            last_err = PQerrorMessage(conn);
            PQfinish(conn);
            conn = nullptr;
            return false;
        }
        return true;
    }
};
