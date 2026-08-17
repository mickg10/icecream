FROM debian:trixie-slim

ARG DEBIAN_FRONTEND=noninteractive
ARG CONAN_VERSION=2.19.1
RUN apt-get update && apt-get install -y --no-install-recommends \
    autoconf automake bison build-essential ca-certificates ccache cmake curl flex gawk \
    git gperf libtool make meson nasm ninja-build patch perl pkg-config python3 \
    python3-pip python3-venv ruby scons unzip xz-utils yasm zip zstd \
 && python3 -m venv /opt/conan \
 && /opt/conan/bin/pip install --no-cache-dir "conan==${CONAN_VERSION}" \
 && /opt/conan/bin/conan profile detect --force \
 && rm -rf /var/lib/apt/lists/* /root/.cache

ENV CC=gcc CXX=g++ PATH=/opt/conan/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    PROFILE_NAME=conan-gcc CONAN_HOME=/opt/conan-home LANG=C.UTF-8 LC_ALL=C.UTF-8
WORKDIR /cell
