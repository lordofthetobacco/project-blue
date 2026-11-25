# Project Blue dedicated server image.
#
# Build from the REPOSITORY ROOT (the native simulation is built from the
# C++ sources there):
#   server/scripts/stage-assets.sh            # fills server/assets/
#   docker build -f server/Dockerfile -t blue-server .
#
# NOTE: Docker was not available on the machine this file was written on.
# Every RUN step was rehearsed by hand in a clean WSL directory (see
# server/README.md), but the image itself has not been built or run yet.
#
# Debian 13 (trixie) for both stages: the C++ code needs GCC >= 13
# (<format>), which Debian 12 does not ship, and the runtime must provide
# the same (or newer) glibc/libstdc++ the builder linked against.
ARG RUST_IMAGE=rust:1.99.0-slim-trixie@sha256:2752b332db73fdbb7dc576f06c82ed1f312005784ef913d7e04a28f5f55dc581
ARG RUNTIME_IMAGE=gcr.io/distroless/cc-debian13:nonroot@sha256:532c7a94f9e96ba9db75eb9388f4227751131c7cb0a411d1a8521af4096135de

FROM ${RUST_IMAGE} AS builder

# cmake (trixie: 3.31, the repo needs >= 3.25), make and g++ (14) for the
# native build; git and ca-certificates because CMake's FetchContent clones
# glm, SDL3, JoltPhysics and netcode at build time (network required).
RUN apt-get update \
 && apt-get install -y --no-install-recommends cmake make g++ git ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# --- Layer 1: native libraries (blue_sim_c + netcode) ----------------------
# Only what the BLUE_SIM_ONLY configure and build read, so Rust-only changes
# reuse this layer. Tests are off, so tests/sim is not needed.
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake/ cmake/
COPY src/ src/
# Jolt is compiled with AVX2/BMI/F16C/LZCNT (x86-64-v3 class); the server
# refuses to start the native sim on an older CPU.
RUN cmake -S . -B /native \
        -DBLUE_SIM_ONLY=ON -DBLUE_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo \
 && cmake --build /native --target blue_sim_c netcode --parallel "$(nproc)"

# --- Layer 2: the Rust workspace -------------------------------------------
# The workspace has no external crates, so there is no dependency layer to
# cache separately (cargo-chef would add nothing).
COPY server/Cargo.toml server/Cargo.lock server/rust-toolchain.toml \
     server/rustfmt.toml server/clippy.toml server/
COPY server/crates/ server/crates/

# --- Layer 3: the server binary, linked against layer 1 ---------------------
WORKDIR /src/server
ENV BLUE_NATIVE_PREBUILT=/native
RUN cargo build --release --locked -p blue-server \
 && strip --strip-debug target/release/blue-server \
 && cp target/release/blue-server /blue-server

FROM ${RUNTIME_IMAGE}

LABEL org.opencontainers.image.title="blue-server" \
      org.opencontainers.image.description="Project Blue authoritative dedicated game server (UDP netcode.io, 60 Hz)" \
      org.opencontainers.image.source="https://github.com/lordofthetobacco/project-blue" \
      org.opencontainers.image.vendor="Project Blue"

COPY --from=builder /blue-server /usr/local/bin/blue-server
# Cooked scene and weapon data staged by server/scripts/stage-assets.*.
COPY server/assets/ /assets/

ENV BLUE_SIM=native \
    BLUE_ASSET_DIR=/assets \
    BLUE_SCENE=/assets/range.mscn \
    BLUE_LOG_FORMAT=json \
    BLUE_METRICS_ADDR=0.0.0.0:9100

USER nonroot
# Game traffic (netcode.io) and /healthz, /readyz, /metrics.
EXPOSE 27015/udp 9100/tcp
# The server disconnects clients and exits 0 within 2 s of SIGTERM.
STOPSIGNAL SIGTERM
# No shell in the image: no HEALTHCHECK; orchestrators probe
# http://<pod>:9100/healthz (liveness) and /readyz (readiness).
ENTRYPOINT ["/usr/local/bin/blue-server"]
