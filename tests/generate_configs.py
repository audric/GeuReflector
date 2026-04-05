#!/usr/bin/env python3
"""Generate test config files and docker-compose.test.yml from topology.py.

Run: python3 generate_configs.py
"""

import os
import topology as T


def generate_reflector_conf(name: str) -> str:
    """Generate svxreflector.conf content for one reflector."""
    r = T.REFLECTORS[name]
    peers = {n: v for n, v in T.REFLECTORS.items() if n != name}

    cluster_str = ",".join(str(tg) for tg in T.CLUSTER_TGS)

    lines = [
        "[GLOBAL]",
        'TIMESTAMP_FORMAT="%c"',
        f"LISTEN_PORT={T.INTERNAL_CLIENT_PORT}",
        "TG_FOR_V1_CLIENTS=999",
        f"LOCAL_PREFIX={T.prefix_str(r['prefix'])}",
        f"CLUSTER_TGS={cluster_str}",
        f"TRUNK_LISTEN_PORT={T.INTERNAL_TRUNK_PORT}",
        f"HTTP_SRV_PORT={T.INTERNAL_HTTP_PORT}",
        "TRUNK_DEBUG=1",
        "",
        "[SERVER_CERT]",
        f"COMMON_NAME={T.service_name(name)}",
        "",
        "[USERS]",
    ]
    groups_seen = set()
    for client in T.TEST_CLIENTS:
        lines.append(f"{client['callsign']}={client['group']}")
        groups_seen.add(client['group'])
    lines += [
        "",
        "[PASSWORDS]",
    ]
    for client in T.TEST_CLIENTS:
        if client['group'] in groups_seen:
            lines.append(f"{client['group']}={client['password']}")
            groups_seen.discard(client['group'])

    # Trunk sections for each peer reflector
    for peer_name in sorted(peers):
        peer = peers[peer_name]
        section = T.trunk_section_name(name, peer_name)
        secret = T.trunk_secret(name, peer_name)
        lines += [
            "",
            f"[{section}]",
            f"HOST={T.service_name(peer_name)}",
            f"PORT={T.INTERNAL_TRUNK_PORT}",
            f"SECRET={secret}",
            f"REMOTE_PREFIX={T.prefix_str(peer['prefix'])}",
        ]

    # Test harness trunk sections
    lines += [
        "",
        "[TRUNK_TEST]",
        "HOST=192.0.2.1",
        "PORT=59302",
        f"SECRET={T.TEST_PEER['secret']}",
        f"REMOTE_PREFIX={T.prefix_str(T.TEST_PEER['prefix'])}",
        "",
        "[TRUNK_TEST_RX]",
        "HOST=192.0.2.1",
        "PORT=59303",
        f"SECRET={T.TEST_PEER_RX['secret']}",
        f"REMOTE_PREFIX={T.prefix_str(T.TEST_PEER_RX['prefix'])}",
        "",
    ]

    # Satellite server section (only on the primary reflector)
    primary = sorted(T.REFLECTORS)[0]
    if name == primary:
        lines += [
            "[SATELLITE]",
            f"LISTEN_PORT={T.SATELLITE['listen_port']}",
            f"SECRET={T.SATELLITE['secret']}",
            "",
        ]

    return "\n".join(lines)


def generate_docker_compose() -> str:
    """Generate docker-compose.test.yml content."""
    services = []
    volumes = []

    primary = sorted(T.REFLECTORS)[0]

    for name in sorted(T.REFLECTORS):
        svc = T.service_name(name)
        client_port = T.mapped_client_port(name)
        trunk_port = T.mapped_trunk_port(name)
        http_port = T.mapped_http_port(name)
        vol = f"pki-{name}"
        volumes.append(vol)

        # Satellite port only on the primary reflector
        sat_port_line = ""
        if name == primary:
            sat_port = T.mapped_satellite_port(name)
            sat_port_line = f'\n      - "{sat_port}:{T.SATELLITE["listen_port"]}/tcp"'

        services.append(f"""  {svc}:
    build:
      context: ..
      dockerfile: Dockerfile
    volumes:
      - ./configs/{svc}.conf:/etc/svxlink/svxreflector.conf:ro
      - {vol}:/var/lib/svxlink/pki
    ports:
      - "{client_port}:{T.INTERNAL_CLIENT_PORT}/tcp"
      - "{client_port}:{T.INTERNAL_CLIENT_PORT}/udp"
      - "{trunk_port}:{T.INTERNAL_TRUNK_PORT}/tcp"
      - "{http_port}:{T.INTERNAL_HTTP_PORT}/tcp"{sat_port_line}
    healthcheck:
      test: ["CMD-SHELL", "bash -c '(echo > /dev/tcp/localhost/{T.INTERNAL_HTTP_PORT}) 2>/dev/null'"]
      interval: 2s
      timeout: 2s
      retries: 15
    networks:
      - trunk_mesh""")

    services_block = "\n\n".join(services)
    volumes_block = "\n".join(f"  {v}:" for v in volumes)

    return f"""services:
{services_block}

volumes:
{volumes_block}

networks:
  trunk_mesh:
    driver: bridge
"""


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    configs_dir = os.path.join(script_dir, "configs")
    os.makedirs(configs_dir, exist_ok=True)

    # Generate per-reflector configs
    for name in sorted(T.REFLECTORS):
        path = os.path.join(configs_dir, f"{T.service_name(name)}.conf")
        content = generate_reflector_conf(name)
        with open(path, "w") as f:
            f.write(content)
        print(f"  wrote {os.path.relpath(path, script_dir)}")

    # Generate docker-compose
    compose_path = os.path.join(script_dir, "docker-compose.test.yml")
    with open(compose_path, "w") as f:
        f.write(generate_docker_compose())
    print(f"  wrote docker-compose.test.yml")


if __name__ == "__main__":
    main()
