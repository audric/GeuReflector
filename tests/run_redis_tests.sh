#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

COMPOSE="docker compose -f docker-compose.redis.yml"

cmd=${1:-all}

usage() {
  cat <<'USAGE'
Usage: run_redis_tests.sh [up|test|down|all|help]

  up     Generate configs, build, and start the Redis test stack (leave running).
  test   Run python3 test_redis.py against the already-running stack.
  down   Tear the stack down (docker compose down -v --remove-orphans).
  all    Full cycle: up -> test -> down. This is the default and CI-friendly.
  help   This message.

Typical dev loop:
  bash run_redis_tests.sh up        # once
  bash run_redis_tests.sh test      # many times as you edit test_redis.py
  bash run_redis_tests.sh down      # when finished
USAGE
}

is_stack_up() {
  $COMPOSE ps --status=running --services 2>/dev/null | grep -q '^reflector-r1$'
}

cmd_up() {
  echo "=== Generating Redis-test configs from topology_redis.py ==="
  python3 generate_redis_configs.py
  echo "=== Building and starting Redis test stack ==="
  $COMPOSE up -d --build --wait
  echo "=== Stack is up. Run 'bash run_redis_tests.sh test' to run tests."
}

cmd_test() {
  if ! is_stack_up; then
    echo "Stack is not running. Run 'bash run_redis_tests.sh up' first." >&2
    exit 1
  fi
  echo "=== Running Redis integration tests ==="
  python3 test_redis.py
}

cmd_down() {
  echo "=== Tearing down Redis test environment ==="
  $COMPOSE down -v --remove-orphans
}

case "$cmd" in
  up)            cmd_up ;;
  test)          cmd_test ;;
  down)          cmd_down ;;
  all)           cmd_up; cmd_test; cmd_down ;;
  help|-h|--help) usage ;;
  *)             echo "Unknown command: $cmd" >&2; usage >&2; exit 1 ;;
esac
