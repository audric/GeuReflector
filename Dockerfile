# Stage 1: Build
FROM debian:bookworm AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    libsigc++-2.0-dev \
    libssl-dev \
    libjsoncpp-dev \
    libpopt-dev \
    libopus-dev \
    libgsm1-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY src/ src/
RUN cmake -S src -B build -DLOCAL_STATE_DIR=/var -DUSE_ALSA=OFF \
    && cmake --build build


# Stage 2: Runtime
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    libsigc++-2.0-0v5 \
    libssl3 \
    libjsoncpp25 \
    libpopt0 \
    libopus0 \
    libgsm1 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/bin/svxreflector /usr/bin/svxreflector
COPY --from=builder /src/build/lib/lib*.so* /usr/lib/

# 5300 tcp+udp: SvxLink client connections and audio
# 5302 tcp:     server-to-server trunk links
# 8080 tcp:     HTTP status endpoint (optional, enable via HTTP_SRV_PORT)
EXPOSE 5300/tcp 5300/udp 5302/tcp 8080/tcp

ENTRYPOINT ["svxreflector"]
CMD ["--config", "/etc/svxlink/svxreflector.conf"]
