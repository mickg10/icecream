FROM fedora:42

RUN dnf -y install \
    autoconf automake bison ccache clang clang-tools-extra cmake curl flex gcc gcc-c++ \
    git gperf libcxx-devel libcxxabi-devel libtool lld make meson nasm ninja-build \
    patch perl pkgconf-pkg-config python3 python3-pip ruby scons unzip xz yasm zip zstd \
 && dnf clean all

ENV CC=clang CXX=clang++ PROFILE_NAME=fedora-clang-libcxx \
    CXXFLAGS=-stdlib=libc++ LDFLAGS=-stdlib=libc++ LANG=C.UTF-8 LC_ALL=C.UTF-8
WORKDIR /cell
