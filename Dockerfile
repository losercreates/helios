# ============================================================================
# Stage 1: Build Environment
# ============================================================================
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    liburing-dev \
    libssl-dev \
    pkg-config \
    ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src/helios

# Copy source tree and configuration files
COPY CMakeLists.txt ./
COPY src/ ./src/
COPY config/ ./config/

# Configure and compile Helios optimized for release
RUN cmake -B build-release -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=gcc \
    -DCMAKE_CXX_COMPILER=g++ \
    -DHELIOS_BUILD_TESTS=OFF \
    -DHELIOS_BUILD_BENCHMARKS=OFF \
 && cmake --build build-release --target helios -j$(nproc) \
 && strip build-release/bin/helios

# ============================================================================
# Stage 2: Minimal Hardened Runtime
# ============================================================================
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    liburing2 \
    libssl3 \
    ca-certificates \
    libcap2-bin \
 && rm -rf /var/lib/apt/lists/*

# Create unprivileged system user and group
RUN groupadd -r helios && useradd -r -g helios -s /usr/sbin/nologin -d /var/empty helios

# Copy binary from builder stage
COPY --from=builder /src/helios/build-release/bin/helios /usr/local/bin/helios

# Grant cap_net_bind_service so helios can bind to low ports (<1024) as non-root
RUN setcap 'cap_net_bind_service=+ep' /usr/local/bin/helios

# Copy default configurations
COPY config/helios_production.yaml /etc/helios/helios_production.yaml
COPY config/helios_mvp.yaml /etc/helios/helios_mvp.yaml

# Expose proxy ingress (8080) and Prometheus telemetry (9090)
EXPOSE 8080 9090

# Switch to unprivileged helios user
USER helios

ENTRYPOINT ["/usr/local/bin/helios"]
CMD ["-c", "/etc/helios/helios_production.yaml", "--daemon"]
