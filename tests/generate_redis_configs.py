#!/usr/bin/env python3
"""Generate Redis-test config files and docker-compose.redis.yml from
topology_redis.py.

Run: python3 generate_redis_configs.py
"""

import os
import topology_redis as T


def generate_reflector_conf(name: str) -> str:
    r = T.REFLECTORS[name]
    lines = [
        "[GLOBAL]",
        'TIMESTAMP_FORMAT="%c"',
        f"LISTEN_PORT={T.INTERNAL_CLIENT_PORT}",
        "TG_FOR_V1_CLIENTS=999",
        f"LOCAL_PREFIX={','.join(r['prefix'])}",
        f"HTTP_SRV_PORT={T.INTERNAL_HTTP_PORT}",
        "",
        "[SERVER_CERT]",
        f"COMMON_NAME={T.service_name(name)}",
        "",
        # Intentionally NO [USERS] or [PASSWORDS] — they would be ignored
        # anyway when [REDIS] is present. Tests HSET users into Redis
        # directly.
        "[REDIS]",
        f"HOST={T.REDIS['host']}",
        f"PORT={T.REDIS['port']}",
        f"DB={T.redis_db_index(name)}",
        f"KEY_PREFIX={T.service_name(name)}",
        "",
        f"[{T.TRUNK_TEST['section']}]",
        f"HOST={T.TRUNK_TEST['host']}",
        f"PORT={T.TRUNK_TEST['port']}",
        f"SECRET={T.TRUNK_TEST['secret']}",
        f"REMOTE_PREFIX={T.TRUNK_TEST['remote_prefix']}",
        "",
    ]
    return "\n".join(lines)


def generate_docker_compose() -> str:
    services = []
    volumes = []

    for name in sorted(T.REFLECTORS):
        svc = T.service_name(name)
        client_port = T.mapped_client_port(name)
        http_port = T.mapped_http_port(name)
        vol = f"pki-redis-{name}"
        volumes.append(vol)
        services.append(f"""  {svc}:
    build:
      context: ..
      dockerfile: Dockerfile
    volumes:
      - ./configs-redis/{svc}.conf:/etc/svxlink/svxreflector.conf:ro
      - {vol}:/var/lib/svxlink/pki
    ports:
      - "{client_port}:{T.INTERNAL_CLIENT_PORT}/tcp"
      - "{client_port}:{T.INTERNAL_CLIENT_PORT}/udp"
      - "{http_port}:{T.INTERNAL_HTTP_PORT}/tcp"
    depends_on:
      redis:
        condition: service_healthy
    healthcheck:
      test: ["CMD-SHELL", "bash -c '(echo > /dev/tcp/localhost/{T.INTERNAL_HTTP_PORT}) 2>/dev/null'"]
      interval: 2s
      timeout: 2s
      retries: 15
    networks:
      - redis_mesh""")

    services.append(f"""  redis:
    image: redis:7-alpine
    command: ["redis-server", "--save", "", "--appendonly", "no"]
    ports:
      - "{T.redis_port()}:6379/tcp"
    healthcheck:
      test: ["CMD", "redis-cli", "ping"]
      interval: 2s
      timeout: 2s
      retries: 10
    networks:
      - redis_mesh""")

    services_block = "\n\n".join(services)
    volumes_block = "\n".join(f"  {v}:" for v in volumes)

    return f"""services:
{services_block}

volumes:
{volumes_block}

networks:
  redis_mesh:
    driver: bridge
"""


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    configs_dir = os.path.join(script_dir, "configs-redis")
    os.makedirs(configs_dir, exist_ok=True)

    for name in sorted(T.REFLECTORS):
        path = os.path.join(configs_dir, f"{T.service_name(name)}.conf")
        with open(path, "w") as f:
            f.write(generate_reflector_conf(name))
        print(f"  wrote {os.path.relpath(path, script_dir)}")

    compose_path = os.path.join(script_dir, "docker-compose.redis.yml")
    with open(compose_path, "w") as f:
        f.write(generate_docker_compose())
    print(f"  wrote docker-compose.redis.yml")


if __name__ == "__main__":
    main()
