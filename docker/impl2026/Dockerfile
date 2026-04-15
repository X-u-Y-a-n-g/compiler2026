FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN set -eux; \
    for attempt in 1 2 3 4 5; do \
        apt-get -o Acquire::Retries=3 update && \
        apt-get install -y --no-install-recommends \
            bash \
            ca-certificates \
            clang \
            cmake \
            cargo \
            file \
            gcc \
            g++ \
            gcc-aarch64-linux-gnu \
            gcc-riscv64-linux-gnu \
            gdb \
            git \
            libc6-dev \
            ninja-build \
            openjdk-21-jdk \
            python3 \
            qemu-user \
            qemu-user-static \
            rustc && \
        break; \
        if [ "$attempt" -eq 5 ]; then \
            exit 1; \
        fi; \
        rm -rf /var/lib/apt/lists/*; \
        sleep $((attempt * 5)); \
    done; \
    rm -rf /var/lib/apt/lists/*

ENV CC=clang
ENV CXX=clang++

WORKDIR /workspace

CMD ["/bin/bash"]
