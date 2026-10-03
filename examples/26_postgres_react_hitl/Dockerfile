# syntax=docker/dockerfile:1.6
#
# Multi-stage build for the example_postgres_react_hitl binary.
# Build stage installs the typed SchemaProvider SDK package before NeoGraph.
# Runtime includes libcurl, libpq, SQLite, OpenSSL, and CA certificates.
#
# Supply the SchemaProvider source as a named BuildKit context:
#   docker build --build-context schemaprovider=../SchemaProvider \
#     -f examples/26_postgres_react_hitl/Dockerfile -t neograph-pg-hitl .
# Compose supplies the same additional context from SCHEMAPROVIDER_SOURCE.

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
        libcurl4-openssl-dev \
        libasio-dev \
        pkg-config \
        python3 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
COPY --from=schemaprovider / /sdk

RUN cmake -S /sdk -B /sdk-build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/schemaprovider \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DBUILD_SHARED_LIBS=OFF \
        -DSP_BUILD_TESTS=OFF \
        -DSP_BUILD_BENCHMARKS=OFF \
        -DSP_BUILD_NEOGRAPH_SPIKE=OFF \
        -DYYJSON_ROOT=/src/deps/yyjson \
    && cmake --build /sdk-build --target sp_runtime -j$(nproc) \
    && cmake --install /sdk-build

# NeoGraph discovers SchemaProvider::runtime from its installed CMake package.
RUN cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=/opt/schemaprovider \
        -DNEOGRAPH_BUILD_POSTGRES=ON \
        -DNEOGRAPH_BUILD_TESTS=OFF \
        -DNEOGRAPH_BUILD_LLM=ON \
        -DNEOGRAPH_BUILD_MCP=OFF \
        -DNEOGRAPH_BUILD_UTIL=ON \
        -DNEOGRAPH_USE_LIBCURL=ON \
        -DNEOGRAPH_BUILD_EXAMPLES=ON \
    && cmake --build build --target example_postgres_react_hitl -j$(nproc)

# ------------------------------------------------------------------- runtime
FROM debian:bookworm-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        libpq5 \
        libsqlite3-0 \
        libssl3 \
        libcurl4 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Separate custody and key parents are private before Docker initializes volumes.
RUN install -d -m 0700 /var/lib/neograph-history /var/lib/neograph-keys

# Default working dir lets the binary find a mounted .env via cppdotenv's
# CWD-relative load. Override with `-w /elsewhere` if you mount config
# in a different path.
WORKDIR /work

COPY --from=build /src/build/example_postgres_react_hitl /usr/local/bin/

ENTRYPOINT ["/usr/local/bin/example_postgres_react_hitl"]
CMD ["run", "What are the latest Vision Transformer papers?"]
