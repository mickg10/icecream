FROM debian:bookworm-slim AS build

COPY --from=ghcr.io/astral-sh/uv:0.9.21 /uv /usr/local/bin/uv

ARG BUILD_JOBS=2

RUN apt-get update && apt-get install -y --no-install-recommends \
    autoconf automake libtool pkg-config \
    build-essential \
    libcap-ng-dev liblzo2-dev libzstd-dev libarchive-dev libboost-dev libxxhash-dev \
    ca-certificates \
    file \
    netcat-openbsd \
    time \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

ENV UV_PROJECT_ENVIRONMENT=/opt/icecream-python \
    UV_PYTHON_INSTALL_DIR=/opt/uv-python \
    UV_CACHE_DIR=/opt/uv-cache
RUN uv sync --locked --managed-python --python "$(cat .python-version)"
ENV PATH=/opt/icecream-python/bin:$PATH

RUN ./autogen.sh \
  && ./configure --prefix=/usr/local --sbindir=/usr/local/sbin --without-man \
  && make -j"${BUILD_JOBS}" \
  && make install

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    libcap-ng0 liblzo2-2 libzstd1 libarchive13 libxxhash0 \
    ca-certificates \
    file \
    netcat-openbsd \
    time \
  && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /usr/local/var/run /out /work

COPY --from=build /usr/local /usr/local
COPY --from=build /opt/uv-python /opt/uv-python
COPY --from=build /opt/icecream-python /opt/icecream-python
COPY tests/compose/worker.sh /usr/local/bin/icecc-compose-worker

RUN chmod +x /usr/local/bin/icecc-compose-worker

ENV PATH=/opt/icecream-python/bin:/usr/local/sbin:/usr/local/bin:/usr/local/libexec/icecc/bin:$PATH \
    UV_OFFLINE=1 \
    UV_PYTHON_DOWNLOADS=never
