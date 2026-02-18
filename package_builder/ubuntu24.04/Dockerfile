FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
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
