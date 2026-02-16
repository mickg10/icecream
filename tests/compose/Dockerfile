FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    autoconf automake libtool pkg-config \
    build-essential \
    libcap-ng-dev liblzo2-dev libzstd-dev libarchive-dev \
    ca-certificates \
    file \
    netcat-openbsd \
    python3 \
    time \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

RUN ./configure --prefix=/usr/local --sbindir=/usr/local/sbin --without-man \
  && make -j"$(nproc)" \
  && make install

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    libcap-ng0 liblzo2-2 libzstd1 libarchive13 \
    ca-certificates \
    file \
    netcat-openbsd \
    python3 \
    time \
  && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /usr/local/var/run /out /work

COPY --from=build /usr/local /usr/local
COPY tests/compose/worker.sh /usr/local/bin/icecc-compose-worker

RUN chmod +x /usr/local/bin/icecc-compose-worker

ENV PATH=/usr/local/sbin:/usr/local/bin:/usr/local/libexec/icecc/bin:$PATH
