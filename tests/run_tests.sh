#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

cleanup() {
  echo "=== Tearing down test environment ==="
  docker compose -f docker-compose.test.yml down -v --remove-orphans 2>/dev/null
}
trap cleanup EXIT

echo "=== Generating configs from topology.py ==="
python3 generate_configs.py

echo "=== Building and starting reflector mesh ==="
docker compose -f docker-compose.test.yml up -d --build --wait

echo "=== Running integration tests ==="
python3 test_trunk.py
