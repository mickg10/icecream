FROM homebrew/brew:latest

RUN brew install autoconf automake bison ccache cmake flex gcc gperf libtool llvm \
    meson nasm ninja pkg-config python scons yasm zstd

ENV CC=/home/linuxbrew/.linuxbrew/opt/llvm/bin/clang \
    CXX=/home/linuxbrew/.linuxbrew/opt/llvm/bin/clang++ \
    PATH=/home/linuxbrew/.linuxbrew/opt/llvm/bin:/home/linuxbrew/.linuxbrew/bin:/usr/bin:/bin \
    PROFILE_NAME=linuxbrew LANG=C.UTF-8 LC_ALL=C.UTF-8
WORKDIR /cell
