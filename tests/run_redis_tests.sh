#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

cleanup() {
  echo "=== Tearing down Redis test environment ==="
  docker compose -f docker-compose.redis.yml down -v --remove-orphans 2>/dev/null
}
trap cleanup EXIT

echo "=== Generating Redis-test configs from topology_redis.py ==="
python3 generate_redis_configs.py

echo "=== Building and starting Redis test stack ==="
docker compose -f docker-compose.redis.yml up -d --build --wait

echo "=== Running Redis integration tests ==="
python3 test_redis.py
