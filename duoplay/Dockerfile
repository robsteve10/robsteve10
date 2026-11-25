# DuoPlay bridge. Run with host networking so the console can be discovered and woken
# and phones reach the streaming port directly:
#   docker compose up -d        (see docker-compose.yml)
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git pkg-config ca-certificates libssl-dev protobuf-compiler python3 python3-protobuf \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY bridge /src/bridge
RUN cmake -S bridge -B build -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=/usr/bin/python3 \
    && cmake --build build -j"$(nproc)" --target duoplay-bridge

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libssl3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/duoplay-bridge /opt/duoplay/duoplay-bridge
COPY web /opt/duoplay/web
VOLUME /data
EXPOSE 8080/tcp 8443/tcp 9600/udp
ENTRYPOINT ["/opt/duoplay/duoplay-bridge", "--data-dir", "/data"]
