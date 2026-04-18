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

echo "=== Logging tests ==="
python3 test_logging.py -v || exit 1

echo ""
echo "=================================="
echo "Running TWIN protocol tests"
echo "=================================="

echo "=== Generating TWIN topology configs ==="
python3 generate_configs.py --topology twin

echo "=== Tearing down default mesh ==="
docker compose -f docker-compose.test.yml down -v --remove-orphans

echo "=== Building and starting TWIN mesh ==="
docker compose -f docker-compose.test.yml up -d --build --wait

echo "=== Running TWIN integration tests ==="
python3 test_twin.py

echo "=== Restoring default topology ==="
python3 generate_configs.py
