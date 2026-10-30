FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    git \
    cmake \
    ninja-build \
    clang-18 \
    lld-18 \
    llvm-18 \
    make \
    python3 \
    python3-pyelftools \
    wget \
    xz-utils \
    pkg-config \
    sudo \
    unzip \
    xxd \
    && rm -rf /var/lib/apt/lists/*

RUN ln -s /usr/bin/clang-18 /usr/bin/clang && \
    ln -s /usr/bin/clang++-18 /usr/bin/clang++ && \
    ln -s /usr/bin/ld.lld-18 /usr/bin/ld.lld && \
    ln -s /usr/bin/llvm-ar-18 /usr/bin/llvm-ar && \
    ln -s /usr/bin/llvm-nm-18 /usr/bin/llvm-nm

WORKDIR /opt
RUN git clone --recurse-submodules https://github.com/ps5-payload-dev/sdk.git ps5-payload-sdk-source && \
    cd ps5-payload-sdk-source && \
    make DESTDIR=/opt/ps5-payload-sdk install

ENV PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
WORKDIR /src

