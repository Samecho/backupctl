FROM ubuntu:24.04
ARG VCPKG_COMMIT=469a0504e4a6ebf6173d1c0f51a7fa34c382fd43
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential clang cmake ninja-build git curl ca-certificates zip unzip tar \
    pkg-config autoconf automake libtool python3 gdb time linux-tools-generic \
    && rm -rf /var/lib/apt/lists/*
ENV VCPKG_ROOT=/opt/vcpkg VCPKG_DISABLE_METRICS=1 VCPKG_MAX_CONCURRENCY=4
RUN git init /opt/vcpkg \
    && git -C /opt/vcpkg remote add origin https://github.com/microsoft/vcpkg.git \
    && git -C /opt/vcpkg fetch --depth=1 origin ${VCPKG_COMMIT} \
    && git -C /opt/vcpkg checkout --detach FETCH_HEAD \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics
COPY vcpkg.json /opt/dependencies/vcpkg.json
RUN /opt/vcpkg/vcpkg install --x-manifest-root=/opt/dependencies \
    --x-install-root=/opt/dependencies/vcpkg_installed --triplet=x64-linux
WORKDIR /work
CMD ["bash"]
