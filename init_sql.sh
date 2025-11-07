#!/bin/bash
# Usage: PGHOST=localhost PGPORT=5432 PGDATABASE=kvdb PGUSER=kvuser PGPASSWORD=kvpass ./init_sql.sh

psql <<'SQL'
CREATE TABLE IF NOT EXISTS kv_store (
  key TEXT PRIMARY KEY,
  value BYTEA
);
SQL
