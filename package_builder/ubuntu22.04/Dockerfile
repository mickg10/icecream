FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    devscripts \
    dpkg-dev \
    equivs \
    fakeroot \
    git \
    rsync \
    build-essential \
  && rm -rf /var/lib/apt/lists/*

COPY build_deb.sh /usr/local/bin/build_deb.sh
RUN chmod +x /usr/local/bin/build_deb.sh

WORKDIR /work

