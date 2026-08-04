FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
        liblzo2-2 libzstd1 libarchive13 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY bin/ /ice/bin/
COPY entrypoint.sh /ice/entrypoint.sh
RUN chmod +x /ice/entrypoint.sh && mkdir -p /var/cache/icecream
ENTRYPOINT ["/ice/entrypoint.sh"]
