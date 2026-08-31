# syntax=docker/dockerfile:1.7
#
# The supervisor is deliberately a small tool image.  Campaign inputs and the
# driver checkout are mounted by s8_protected_launcher.py; no source tree or
# measurement artifact is copied into this image.
#
# The farm-node registry digest is not present in the local Docker store.  The
# operator must first retag the locally verified config ID under this
# content-addressed name (see docs/S8_SUPERVISOR_IMAGE.md).  --pull=false then
# makes an accidental registry lookup impossible.
FROM icecream/s8-supervisor-base:ubuntu22-config-fe001a6138f017608b8846b43bf268a76a9d7a5b66c3364ba3f881da2ff0c54b

USER root
ENV DEBIAN_FRONTEND=noninteractive \
    PYTHONUNBUFFERED=1

# Ubuntu's docker.io package supplies the Docker CLI at /usr/bin/docker.  The
# daemon is never started in this image; the protected launcher mounts the
# host daemon socket and runs as the caller's UID/GID.
RUN apt-get update \
 && apt-get install --yes --no-install-recommends \
      python3=3.10.6-1~22.04.1 \
      git=1:2.34.1-1ubuntu1.17 \
      docker.io=29.1.3-0ubuntu3~22.04.2 \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
ENTRYPOINT ["/bin/sh"]
CMD ["-lc", "command -v python3 && command -v git && command -v docker"]
