ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive

# Source is supplied separately at runtime, read-only at /source.
RUN apt-get update && apt-get install -y --no-install-recommends \
        autoconf automake bash build-essential ca-certificates clang coreutils \
        default-jre-headless \
        file findutils gawk git grep gzip libarchive-dev libboost-dev \
        libcap-ng-dev liblzo2-dev libxxhash-dev libzstd-dev libtool make \
        netcat-openbsd pkg-config procps python3 python3-pytest sed sudo tar \
        time util-linux xz-utils zstd \
    && rm -rf /var/lib/apt/lists/*

# Recipe/runner edits must not invalidate the package-install layer.
ARG DEV_PROFILE=ubuntu24.04
ARG RECIPE_REVISION=unknown
LABEL org.icecream.dev.profile="${DEV_PROFILE}" \
      org.icecream.dev.recipe="${RECIPE_REVISION}" \
      org.icecream.dev.purpose="native-build-and-local-qa-toolchain"

COPY run-qa.sh /usr/local/bin/run-qa.sh
RUN chmod 0755 /usr/local/bin/run-qa.sh

ENTRYPOINT ["/usr/local/bin/run-qa.sh"]
