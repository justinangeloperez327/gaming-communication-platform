# Build from the repository root; migrations are embedded by SQLx.
FROM rust:1.99.1-slim-bookworm AS build
WORKDIR /src
COPY Cargo.toml Cargo.lock ./
COPY apps/server apps/server
RUN cargo build --locked --release -p gaming-comms-server

FROM debian:bookworm-slim
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/target/release/gaming-comms-server /usr/local/bin/gaming-comms-server
USER 10001:10001
ENTRYPOINT ["/usr/local/bin/gaming-comms-server"]
