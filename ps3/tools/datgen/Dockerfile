# The toolchain for the native .dat schema: clang and lld for the PPC DWARF build, Rust for melee-dat,
# Aurora's headers and newlib's (see flake.nix, devShells.native). Build: docker build -t melee-datgen:1 .
FROM debian:trixie
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates clang cmake curl git libnewlib-dev lld llvm ninja-build \
    && rm -rf /var/lib/apt/lists/*
RUN curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain stable
RUN git clone https://github.com/r-burns/aurora /opt/aurora \
    && git -C /opt/aurora checkout e6a6f02ace4146e8a2f648d5c274dbb7dd89665c
ENV PATH=/root/.cargo/bin:$PATH AURORA_SRC=/opt/aurora NEWLIB_INCLUDE=/usr/include/newlib
