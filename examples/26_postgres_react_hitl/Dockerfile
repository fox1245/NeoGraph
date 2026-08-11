# syntax=docker/dockerfile:1.6
#
# Multi-stage build for the example_postgres_react_hitl binary.
# Build stage compiles the binary against libpq-dev + libsqlite3-dev + libssl-dev;
# runtime stage carries only libpq5/libsqlite3-0/libssl3/CA-certs needed to
# actually run it. Final image weighs ~80 MB (vs ~700 MB for the build
# stage).
#
# The build context is the repo root: invoke as
#   docker build -f examples/26_postgres_react_hitl/Dockerfile -t neograph-pg-hitl ../..
# or via docker-compose.yml which sets `context: ../..` for you.

# --------------------------------------------------------------------- build
FROM debian:bookworm-slim AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        libpq-dev \
        libsqlite3-dev \
        libssl-dev \
        pkg-config \
        python3 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Build only what we need: the example, its core deps, and Postgres. Tests
# and Clay/raylib stay off so build time stays under a minute on a warm cache.
# The example uses SchemaProvider's default ConnPool HTTP/1.1 transport;
# exclude unused CurlH2Pool so this slim image needs no libcurl development package.
RUN cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DNEOGRAPH_BUILD_POSTGRES=ON \
        -DNEOGRAPH_BUILD_TESTS=OFF \
        -DNEOGRAPH_BUILD_LLM=ON \
        -DNEOGRAPH_BUILD_MCP=OFF \
        -DNEOGRAPH_BUILD_UTIL=ON \
        -DNEOGRAPH_USE_LIBCURL=OFF \
        -DNEOGRAPH_BUILD_EXAMPLES=ON \
    && cmake --build build --target example_postgres_react_hitl -j$(nproc)

# ------------------------------------------------------------------- runtime
FROM debian:bookworm-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        libpq5 \
        libsqlite3-0 \
        libssl3 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Default working dir lets the binary find a mounted .env via cppdotenv's
# CWD-relative load. Override with `-w /elsewhere` if you mount config
# in a different path.
WORKDIR /work

COPY --from=build /src/build/example_postgres_react_hitl /usr/local/bin/

ENTRYPOINT ["/usr/local/bin/example_postgres_react_hitl"]
CMD ["run", "What are the latest Vision Transformer papers?"]
