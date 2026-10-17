FROM debian:bookworm-slim AS builder
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake make libsqlite3-dev python3 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j2 && ctest --test-dir build --output-on-failure

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libsqlite3-0 && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 edge && useradd --uid 10001 --gid edge --no-create-home edge \
    && mkdir -p /data /etc/edge-gateway && chown edge:edge /data
COPY --from=builder /src/build/edge-gateway /usr/local/bin/edge-gateway
COPY config/gateway.conf /etc/edge-gateway/gateway.conf
RUN sed -i 's|^database=.*|database=/data/gateway.db|' /etc/edge-gateway/gateway.conf
WORKDIR /data
USER 10001:10001
VOLUME /data
# Host network preserves the default loopback binding. See docs/deployment.md.
ENTRYPOINT ["/usr/local/bin/edge-gateway"]
CMD ["--config", "/etc/edge-gateway/gateway.conf"]
