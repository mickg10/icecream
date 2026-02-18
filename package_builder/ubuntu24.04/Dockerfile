FROM ubuntu:24.04

ARG http_proxy
ARG https_proxy
ARG no_proxy
ARG HTTP_PROXY
ARG HTTPS_PROXY
ARG NO_PROXY
ARG ICECREAM_BUILDER_INSECURE

ENV http_proxy="${http_proxy}" \
    https_proxy="${https_proxy}" \
    no_proxy="${no_proxy}" \
    HTTP_PROXY="${HTTP_PROXY}" \
    HTTPS_PROXY="${HTTPS_PROXY}" \
    NO_PROXY="${NO_PROXY}"

ENV DEBIAN_FRONTEND=noninteractive

RUN if [ "${ICECREAM_BUILDER_INSECURE:-}" = "1" ] || [ "${ICECREAM_BUILDER_INSECURE:-}" = "true" ]; then \
        printf '%s\n' \
            'Acquire::https::Verify-Peer "false";' \
            'Acquire::https::Verify-Host "false";' \
            > /etc/apt/apt.conf.d/99icecream-builder-insecure; \
    fi \
  && apt-get update && apt-get install -y --no-install-recommends \
    asciidoc \
    ca-certificates \
    docbook-xsl \
    devscripts \
    dpkg-dev \
    equivs \
    fakeroot \
    git \
    pkg-config \
    rsync \
    xmlto \
    build-essential \
  && rm -rf /var/lib/apt/lists/*

COPY build_deb.sh /usr/local/bin/build_deb.sh
RUN chmod +x /usr/local/bin/build_deb.sh

COPY verify_deb.sh /usr/local/bin/verify_deb.sh
RUN chmod +x /usr/local/bin/verify_deb.sh

WORKDIR /work
