FROM fedora:latest

COPY --from=ghcr.io/astral-sh/uv:0.9.21 /uv /usr/local/bin/uv

COPY build_rpm.sh /usr/local/bin/build_rpm.sh
RUN chmod +x /usr/local/bin/build_rpm.sh

COPY verify_rpm.sh /usr/local/bin/verify_rpm.sh
RUN chmod +x /usr/local/bin/verify_rpm.sh

WORKDIR /work
