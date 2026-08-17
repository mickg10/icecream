FROM debian:bookworm-slim

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    autoconf automake bison build-essential ca-certificates ccache cmake curl flex gawk \
    git gperf libtool make meson nasm ninja-build patch perl pkg-config python3 \
    python3-pip python3-venv ruby scons unzip xz-utils yasm zip zstd \
 && rm -rf /var/lib/apt/lists/*

ENV CC=gcc CXX=g++ PROFILE_NAME=debian-gcc LANG=C.UTF-8 LC_ALL=C.UTF-8
WORKDIR /cell
