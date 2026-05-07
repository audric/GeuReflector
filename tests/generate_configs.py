#!/usr/bin/env python3
"""Generate test config files and docker-compose.test.yml from topology.py.

Run: python3 generate_configs.py                  # default 3-reflector mesh
     python3 generate_configs.py --topology twin  # 4-reflector twin topology
     TOPOLOGY=twin python3 generate_configs.py    # same via env var
"""

import os
import sys
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
        "COMMAND_PTY=/dev/shm/reflector_ctrl",
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

    # Trunk sections for each peer reflector. Honour the `peers` filter
    # so tests can build sparse trunk topologies (e.g. gateway routing).
    for peer_name in sorted(peers):
        if not T.should_trunk(name, peer_name):
            continue
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
        "[TRUNK_TEST_FILTER]",
        "HOST=192.0.2.1",
        "PORT=59304",
        f"SECRET={T.TEST_PEER_FILTER['secret']}",
        f"REMOTE_PREFIX={T.prefix_str(T.TEST_PEER_FILTER['prefix'])}",
        f"PEER_ID={T.TEST_PEER_FILTER['peer_id']}",
        f"BLACKLIST_TGS={T.TEST_PEER_FILTER['blacklist_tgs']}",
        f"ALLOW_TGS={T.TEST_PEER_FILTER['allow_tgs']}",
        f"TG_MAP={T.TEST_PEER_FILTER['tg_map']}",
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

    # MQTT publishing section
    lines += [
        "[MQTT]",
        f"HOST={T.MQTT['host']}",
        f"PORT={T.MQTT['port']}",
        f"USERNAME={T.MQTT['username']}",
        f"PASSWORD={T.MQTT['password']}",
        f"TOPIC_PREFIX=svxreflector/{name}",
        "STATUS_INTERVAL=1000",
    ]
    if name in T.MQTT_NAMES:
        lines.append(f"MQTT_NAME={T.MQTT_NAMES[name]}")
    lines.append("")

    if r.get("redis"):
        lines += [
            "[REDIS]",
            f"HOST={T.REDIS['host']}",
            f"PORT={T.REDIS['port']}",
            f"DB={T.redis_db_index(name)}",
            f"KEY_PREFIX={T.service_name(name)}",
            "",
        ]

    return "\n".join(lines)


def generate_satellite_node_conf() -> str:
    """Generate svxreflector.conf for a satellite-mode reflector that
    connects to the primary reflector as a satellite peer."""
    sat = T.SATELLITE_NODE
    parent = T.sat_node_parent()
    parent_svc = T.service_name(parent)
    parent_prefix = T.prefix_str(T.REFLECTORS[parent]["prefix"])

    lines = [
        "[GLOBAL]",
        'TIMESTAMP_FORMAT="%c"',
        f"LISTEN_PORT={T.INTERNAL_CLIENT_PORT}",
        "TG_FOR_V1_CLIENTS=999",
        f"LOCAL_PREFIX={parent_prefix}",
        f"HTTP_SRV_PORT={T.INTERNAL_HTTP_PORT}",
        f"SATELLITE_OF={parent_svc}",
        f"SATELLITE_PORT={T.SATELLITE['listen_port']}",
        f"SATELLITE_SECRET={T.SATELLITE['secret']}",
        f"SATELLITE_ID={sat['id']}",
        "",
        "[SERVER_CERT]",
        f"COMMON_NAME={T.sat_node_service_name()}",
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
    lines += [
        "",
        "[MQTT]",
        f"HOST={T.MQTT['host']}",
        f"PORT={T.MQTT['port']}",
        f"USERNAME={T.MQTT['username']}",
        f"PASSWORD={T.MQTT['password']}",
        f"TOPIC_PREFIX=svxreflector/{sat['name']}",
        "STATUS_INTERVAL=1000",
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

        depends_on_line = ""
        if T.REFLECTORS[name].get("redis"):
            depends_on_line = """
    depends_on:
      redis:
        condition: service_healthy"""

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
      - trunk_mesh{depends_on_line}""")

    # Satellite-mode reflector — joins the primary as a satellite. Used
    # to verify satellite-mode behavior (MQTT publishing, V2 client server).
    sat_svc = T.sat_node_service_name()
    sat_vol = f"pki-{T.SATELLITE_NODE['name']}"
    volumes.append(sat_vol)
    services.append(f"""  {sat_svc}:
    build:
      context: ..
      dockerfile: Dockerfile
    volumes:
      - ./configs/{sat_svc}.conf:/etc/svxlink/svxreflector.conf:ro
      - {sat_vol}:/var/lib/svxlink/pki
    ports:
      - "{T.sat_node_mapped_client_port()}:{T.INTERNAL_CLIENT_PORT}/tcp"
      - "{T.sat_node_mapped_client_port()}:{T.INTERNAL_CLIENT_PORT}/udp"
      - "{T.sat_node_mapped_http_port()}:{T.INTERNAL_HTTP_PORT}/tcp"
    healthcheck:
      test: ["CMD-SHELL", "bash -c '(echo > /dev/tcp/localhost/{T.INTERNAL_HTTP_PORT}) 2>/dev/null'"]
      interval: 2s
      timeout: 2s
      retries: 15
    networks:
      - trunk_mesh""")

    # Mosquitto broker for MQTT testing
    services.append("""  mosquitto:
    image: eclipse-mosquitto:2
    command: sh -c 'echo "listener 1883" > /tmp/m.conf && echo "allow_anonymous true" >> /tmp/m.conf && mosquitto -c /tmp/m.conf -v'
    ports:
      - "11883:1883/tcp"
    healthcheck:
      test: ["CMD-SHELL", "mosquitto_pub -h localhost -t test -m test || exit 1"]
      interval: 2s
      timeout: 2s
      retries: 10
    networks:
      - trunk_mesh""")

    # Redis used by any reflector flagged `redis: True`.
    any_redis = any(r.get("redis") for r in T.REFLECTORS.values())
    if any_redis:
        services.append("""  redis:
    image: redis:7-alpine
    command: ["redis-server", "--save", "", "--appendonly", "no"]
    ports:
      - "16379:6379/tcp"
    healthcheck:
      test: ["CMD", "redis-cli", "ping"]
      interval: 2s
      timeout: 2s
      retries: 10
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


def generate_twin_reflector_conf(name: str) -> str:
    """Generate svxreflector.conf for one reflector in the TWIN topology."""
    r = T.TWIN_REFLECTORS[name]
    cluster_str = ",".join(str(tg) for tg in T.TWIN_CLUSTER_TGS)

    lines = [
        "[GLOBAL]",
        'TIMESTAMP_FORMAT="%c"',
        f"LISTEN_PORT={T.INTERNAL_CLIENT_PORT}",
        "TG_FOR_V1_CLIENTS=999",
        f"LOCAL_PREFIX={T.prefix_str(r['prefix'])}",
        f"CLUSTER_TGS={cluster_str}",
        f"TRUNK_LISTEN_PORT={T.INTERNAL_TRUNK_PORT}",
        f"HTTP_SRV_PORT={T.INTERNAL_HTTP_PORT}",
        "COMMAND_PTY=/dev/shm/reflector_ctrl",
        "TRUNK_DEBUG=1",
    ]

    # Add TWIN_LISTEN_PORT when this reflector is part of a twin pair
    if r.get("twin_of"):
        lines.append(f"TWIN_LISTEN_PORT={T.INTERNAL_TWIN_PORT}")

    lines += [
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

    # [TWIN] section — points at the twin partner's Docker service host
    if r.get("twin_of"):
        twin_partner = r["twin_of"]
        secret = T.twin_trunk_secret(f"twin_{min(name, twin_partner)}_{max(name, twin_partner)}")
        lines += [
            "",
            "[TWIN]",
            f"HOST={T.service_name(twin_partner)}",
            f"PORT={T.INTERNAL_TWIN_PORT}",
            f"SECRET={secret}",
        ]

    # Trunk sections derived from TWIN_TRUNKS
    for trunk in T.twin_trunks_for(name):
        section = f"TRUNK_{trunk['name']}"
        secret = T.twin_trunk_secret(trunk["name"])
        pair_members = trunk["pair_members"]
        is_pair_member = name in pair_members
        # The "other side" peers from this reflector's perspective
        other_peers = [p for p in trunk["peers"] if p != name]

        if trunk["paired"] and not is_pair_member:
            # Non-pair side: PAIRED=1, HOST= lists all pair members
            pair_hosts = ",".join(T.service_name(p) for p in pair_members)
            # REMOTE_PREFIX: all pair members share the same prefix
            remote_prefix = T.prefix_str(T.TWIN_REFLECTORS[pair_members[0]]["prefix"])
            lines += [
                "",
                f"[{section}]",
                f"HOST={pair_hosts}",
                f"PORT={T.INTERNAL_TRUNK_PORT}",
                f"SECRET={secret}",
                f"REMOTE_PREFIX={remote_prefix}",
                "PAIRED=1",
            ]
        elif trunk["paired"] and is_pair_member:
            # Pair member side: single-host trunk back to non-pair peer(s)
            non_pair_peers = [p for p in trunk["peers"] if p not in pair_members]
            for peer in non_pair_peers:
                remote_prefix = T.prefix_str(T.TWIN_REFLECTORS[peer]["prefix"])
                lines += [
                    "",
                    f"[{section}]",
                    f"HOST={T.service_name(peer)}",
                    f"PORT={T.INTERNAL_TRUNK_PORT}",
                    f"SECRET={secret}",
                    f"REMOTE_PREFIX={remote_prefix}",
                ]
        else:
            # Normal (non-paired) trunk: one section per remote peer
            for peer in other_peers:
                remote_prefix = T.prefix_str(T.TWIN_REFLECTORS[peer]["prefix"])
                lines += [
                    "",
                    f"[{section}]",
                    f"HOST={T.service_name(peer)}",
                    f"PORT={T.INTERNAL_TRUNK_PORT}",
                    f"SECRET={secret}",
                    f"REMOTE_PREFIX={remote_prefix}",
                ]

    # Satellite server section — only on the designated twin member so
    # integration tests can verify satellite delivery via the TWIN link.
    if name == T.TWIN_SATELLITE_PARENT:
        lines += [
            "",
            "[SATELLITE]",
            f"LISTEN_PORT={T.SATELLITE['listen_port']}",
            f"SECRET={T.SATELLITE['secret']}",
        ]

    # MQTT publishing section
    lines += [
        "",
        "[MQTT]",
        f"HOST={T.MQTT['host']}",
        f"PORT={T.MQTT['port']}",
        f"USERNAME={T.MQTT['username']}",
        f"PASSWORD={T.MQTT['password']}",
        f"TOPIC_PREFIX=svxreflector/{name}",
        "STATUS_INTERVAL=1000",
    ]
    # Twin pair members must have MQTT_NAME to avoid retained-topic collision
    # when both reflectors share the same broker (enforced by Reflector::initialize).
    if r.get("twin_of"):
        lines.append(f"MQTT_NAME={name}")
    lines.append("")

    if r.get("redis"):
        lines += [
            "[REDIS]",
            f"HOST={T.REDIS['host']}",
            f"PORT={T.REDIS['port']}",
            f"DB={sorted(T.TWIN_REFLECTORS).index(name)}",
            f"KEY_PREFIX={T.service_name(name)}",
            "",
        ]

    return "\n".join(lines)


def generate_twin_docker_compose() -> str:
    """Generate docker-compose.test.yml for the twin topology."""
    services = []
    volumes = []

    for name in sorted(T.TWIN_REFLECTORS):
        r = T.TWIN_REFLECTORS[name]
        svc = T.service_name(name)
        client_port = T.twin_mapped_client_port(name)
        trunk_port = T.twin_mapped_trunk_port(name)
        http_port = T.twin_mapped_http_port(name)
        vol = f"pki-{name}"
        volumes.append(vol)

        # Extra port mapping for [TWIN] listen port when relevant
        twin_port_line = ""
        if r.get("twin_of"):
            twin_port = T.twin_mapped_twin_port(name)
            twin_port_line = f'\n      - "{twin_port}:{T.INTERNAL_TWIN_PORT}/tcp"'

        # Satellite listen port mapping — only on the designated twin parent
        sat_port_line = ""
        if name == T.TWIN_SATELLITE_PARENT:
            sat_port = T.twin_mapped_satellite_port(name)
            sat_port_line = (
                f'\n      - "{sat_port}:{T.SATELLITE["listen_port"]}/tcp"')

        depends_on_line = ""
        if r.get("redis"):
            depends_on_line = """
    depends_on:
      redis:
        condition: service_healthy"""

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
      - "{http_port}:{T.INTERNAL_HTTP_PORT}/tcp"{twin_port_line}{sat_port_line}
    healthcheck:
      test: ["CMD-SHELL", "bash -c '(echo > /dev/tcp/localhost/{T.INTERNAL_HTTP_PORT}) 2>/dev/null'"]
      interval: 2s
      timeout: 2s
      retries: 15
    networks:
      - trunk_mesh{depends_on_line}""")

    # Mosquitto broker
    services.append("""  mosquitto:
    image: eclipse-mosquitto:2
    command: sh -c 'echo "listener 1883" > /tmp/m.conf && echo "allow_anonymous true" >> /tmp/m.conf && mosquitto -c /tmp/m.conf -v'
    ports:
      - "21883:1883/tcp"
    healthcheck:
      test: ["CMD-SHELL", "mosquitto_pub -h localhost -t test -m test || exit 1"]
      interval: 2s
      timeout: 2s
      retries: 10
    networks:
      - trunk_mesh""")

    any_redis = any(r.get("redis") for r in T.TWIN_REFLECTORS.values())
    if any_redis:
        services.append("""  redis:
    image: redis:7-alpine
    command: ["redis-server", "--save", "", "--appendonly", "no"]
    ports:
      - "26379:6379/tcp"
    healthcheck:
      test: ["CMD", "redis-cli", "ping"]
      interval: 2s
      timeout: 2s
      retries: 10
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
    # Topology selection: --topology twin  or  TOPOLOGY=twin env var
    topology = os.environ.get("TOPOLOGY", "default")
    args = sys.argv[1:]
    if "--topology" in args:
        idx = args.index("--topology")
        if idx + 1 < len(args):
            topology = args[idx + 1]

    script_dir = os.path.dirname(os.path.abspath(__file__))

    if topology == "twin":
        configs_dir = os.path.join(script_dir, "configs")
        os.makedirs(configs_dir, exist_ok=True)

        print("Generating TWIN topology configs...")
        for name in sorted(T.TWIN_REFLECTORS):
            path = os.path.join(configs_dir, f"{T.service_name(name)}.conf")
            content = generate_twin_reflector_conf(name)
            with open(path, "w") as f:
                f.write(content)
            print(f"  wrote {os.path.relpath(path, script_dir)}")

        compose_path = os.path.join(script_dir, "docker-compose.test.yml")
        with open(compose_path, "w") as f:
            f.write(generate_twin_docker_compose())
        print(f"  wrote docker-compose.test.yml")

    else:
        configs_dir = os.path.join(script_dir, "configs")
        os.makedirs(configs_dir, exist_ok=True)

        # Generate per-reflector configs
        for name in sorted(T.REFLECTORS):
            path = os.path.join(configs_dir, f"{T.service_name(name)}.conf")
            content = generate_reflector_conf(name)
            with open(path, "w") as f:
                f.write(content)
            print(f"  wrote {os.path.relpath(path, script_dir)}")

        # Satellite-mode reflector config
        sat_path = os.path.join(configs_dir,
                                f"{T.sat_node_service_name()}.conf")
        with open(sat_path, "w") as f:
            f.write(generate_satellite_node_conf())
        print(f"  wrote {os.path.relpath(sat_path, script_dir)}")

        # Generate docker-compose
        compose_path = os.path.join(script_dir, "docker-compose.test.yml")
        with open(compose_path, "w") as f:
            f.write(generate_docker_compose())
        print(f"  wrote docker-compose.test.yml")


if __name__ == "__main__":
    main()
