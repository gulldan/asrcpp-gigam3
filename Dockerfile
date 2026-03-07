# syntax=docker/dockerfile:1.7
#
# Build from repository root:
#   docker build -f Dockerfile -t asr-server-cpp .

# ============================================
# Stage 1: Toolchain base
# ============================================
FROM debian:trixie-slim AS build-base

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    pkg-config \
    wget \
    ca-certificates \
    libssl-dev \
    zlib1g-dev \
    libjsoncpp-dev \
    uuid-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# ============================================
# Stage 2: Web frontend build
# ============================================
FROM oven/bun:1.3.10 AS web-builder

WORKDIR /build

COPY client/src/web/ client/src/web/

RUN bun run client/src/web/build.ts

# ============================================
# Stage 3: Dependency warm-up
# Builds with stub sources so third-party deps are cached independently
# from frequent src/include changes.
# ============================================
FROM build-base AS deps-builder

COPY CMakeLists.txt CMakePresets.json ./

RUN set -eux; \
    mkdir -p src; \
    for f in config audio vad recognizer handler metrics server realtime_session offline_transcription whisper_api; do \
      printf 'int asr_stub_%s(void) { return 0; }\n' "${f}" > "src/${f}.cpp"; \
    done; \
    printf 'int main(void) { return 0; }\n' > src/main.cpp

RUN cmake --preset release && \
    cmake --build build/release --parallel $(nproc)

# ============================================
# Stage 4: Final app build
# ============================================
FROM build-base AS builder

WORKDIR /build

# Reuse prebuilt third-party artifacts from deps-builder
COPY --from=deps-builder /build/build/ /build/build/

# Copy only project sources needed for release build
COPY CMakeLists.txt CMakePresets.json ./
COPY include/ include/
COPY src/ src/
COPY third_party/ third_party/
COPY --from=web-builder /build/static/ static/

# Remove project artifacts built from stub sources while keeping third-party cache
RUN rm -rf \
    /build/build/release/CMakeFiles/asr_core.dir \
    /build/build/release/CMakeFiles/asr-server.dir \
    /build/build/release/libasr_core.a \
    /build/build/release/asr-server

# Re-configure with real sources and build app target
RUN cmake --preset release && \
    cmake --build build/release --parallel $(nproc) --target asr-server

# Runtime-writable upload path for non-root user
RUN mkdir -p /build/runtime/uploads/tmp

# ============================================
# Stage 5: Runtime
# ============================================
FROM gcr.io/distroless/cc-debian13 AS runtime

WORKDIR /app

# Copy binary
COPY --from=builder /build/build/release/asr-server /app/asr-server

# Copy onnxruntime shared library (downloaded by FetchContent)
# Supports both amd64 (x64) and arm64 (aarch64) package directories.
COPY --from=builder /build/build/_shared_deps/onnxruntime/onnxruntime-linux-*/lib/libonnxruntime* /usr/local/lib/

# Copy required system libraries (libgomp, libssl, zlib, libjsoncpp, libuuid)
# from the active Debian multiarch directory (x86_64-linux-gnu or aarch64-linux-gnu).
COPY --from=builder /lib/*-linux-gnu /lib/
COPY --from=builder /usr/lib/*-linux-gnu /usr/lib/

# Copy static files and models
COPY --from=web-builder /build/static/ /app/static/
COPY models/ /app/models/
COPY --from=builder --chown=65532:65532 /build/runtime/uploads/ /app/uploads/

ENV PROVIDER=cpu
ENV NUM_THREADS=4
ENV HOST=0.0.0.0
ENV HTTP_PORT=8081
ENV LD_LIBRARY_PATH=/usr/local/lib

EXPOSE 8081

USER 65532
ENTRYPOINT ["/app/asr-server"]
