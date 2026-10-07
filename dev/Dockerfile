ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}

COPY --from=ghcr.io/astral-sh/uv:0.9.21 /uv /uvx /usr/local/bin/

ENV DEBIAN_FRONTEND=noninteractive

# Source is supplied separately at runtime, read-only at /source.
RUN apt-get update && apt-get install -y --no-install-recommends \
        autoconf automake bash build-essential ca-certificates clang coreutils \
        default-jre-headless \
        file findutils gawk git grep gzip libarchive-dev libboost-dev \
        libcap-ng-dev liblzo2-dev libxxhash-dev libzstd-dev libtool make \
        netcat-openbsd pkg-config procps python3 sed sudo tar \
        time util-linux xz-utils zstd \
    && rm -rf /var/lib/apt/lists/*

# The Python environment is resolved once while building the SDK.  Runtime
# containers are deliberately offline and use this locked environment only.
ENV UV_PROJECT_ENVIRONMENT=/opt/icecream-python \
    UV_PYTHON_INSTALL_DIR=/opt/uv-python \
    UV_CACHE_DIR=/opt/uv-cache \
    UV_PYTHON_DOWNLOADS=automatic
WORKDIR /opt/icecream-python-src
COPY pyproject.toml uv.lock .python-version ./
RUN uv python install "$(cat .python-version)" \
    && uv sync --locked --managed-python --python "$(cat .python-version)"

# Recipe/runner edits must not invalidate the package-install layer.
ARG DEV_PROFILE=ubuntu24.04
ARG RECIPE_REVISION=unknown
LABEL org.icecream.dev.profile="${DEV_PROFILE}" \
      org.icecream.dev.recipe="${RECIPE_REVISION}" \
      org.icecream.dev.purpose="native-build-and-local-qa-toolchain"

ENV VIRTUAL_ENV=/opt/icecream-python \
    PATH=/opt/icecream-python/bin:$PATH \
    UV_OFFLINE=1 \
    UV_PYTHON_DOWNLOADS=never

COPY run-qa.sh /usr/local/bin/run-qa.sh
RUN chmod 0755 /usr/local/bin/run-qa.sh

# The icecc service identity: the root-side daemon tests (sourcearm-live,
# daemonpositive) and the e2e daemons' capng privilege drop both require a
# NON-ROOT icecc user to exist (getpwnam("icecc"), uid/gid != 0) or they
# skip -- the exact skips that hid the shipped fd bug.
RUN groupadd --system icecc 2>/dev/null || true \
    && useradd --system --gid icecc --home-dir /var/cache/icecc --shell /usr/sbin/nologin icecc 2>/dev/null || true \
    && mkdir -p /var/cache/icecc \
    && chown icecc:icecc /var/cache/icecc \
    && (command -v setcap >/dev/null || apt-get update -qq && apt-get install -y -qq --no-install-recommends libcap2-bin)

ENTRYPOINT ["/usr/local/bin/run-qa.sh"]
