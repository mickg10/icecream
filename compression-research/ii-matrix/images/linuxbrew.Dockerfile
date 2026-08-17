FROM homebrew/brew:latest

ENV HOMEBREW_NO_ANALYTICS=1 HOMEBREW_NO_ENV_HINTS=1 HOMEBREW_NO_AUTO_UPDATE=1 \
    HOMEBREW_DOWNLOAD_CONCURRENCY=1
RUN brew update \
 && for formula in autoconf automake bison ccache cmake flex gperf libtool llvm \
        meson nasm ninja pkg-config python scons yasm zstd; do \
        brew install "${formula}"; \
    done \
 && brew cleanup --prune=all
RUN set -eu; \
    for formula in autoconf automake bison ccache cmake flex gperf libtool llvm \
        meson nasm ninja pkg-config python scons yasm zstd; do \
        if ! brew list --versions "${formula}" >/dev/null 2>&1; then \
            brew install "${formula}" || brew install "${formula}"; \
        fi; \
    done; \
    for formula in autoconf automake bison ccache cmake flex gperf libtool llvm \
        meson nasm ninja pkg-config python scons yasm zstd; do \
        brew list --versions "${formula}" >/dev/null; \
    done; \
    brew cleanup --prune=all

ENV CC=/home/linuxbrew/.linuxbrew/opt/llvm/bin/clang \
    CXX=/home/linuxbrew/.linuxbrew/opt/llvm/bin/clang++ \
    PATH=/home/linuxbrew/.linuxbrew/opt/llvm/bin:/home/linuxbrew/.linuxbrew/bin:/usr/bin:/bin \
    PROFILE_NAME=linuxbrew LANG=C.UTF-8 LC_ALL=C.UTF-8
WORKDIR /cell
