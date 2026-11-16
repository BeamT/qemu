#!/usr/bin/env bash

set -euo pipefail

export DEBIAN_FRONTEND="noninteractive"

dpkg --add-architecture arm64

# Debian 11 left the security mirrors when its LTS ended: deb.debian.org
# still serves a stale index whose packages are gone. Use the archive.
if grep -q '^VERSION_CODENAME=bullseye' /etc/os-release; then
    sed -i 's|http://deb.debian.org/debian-security|http://archive.debian.org/debian-security|' \
        /etc/apt/sources.list
fi

apt-get update -y -q -o Acquire::Check-Valid-Until=false
apt-get install -y -q --no-install-recommends \
    binutils-aarch64-linux-gnu \
    build-essential \
    crossbuild-essential-arm64 \
    gcc-aarch64-linux-gnu \
    git \
    libgcrypt20-dev:arm64 \
    libglib2.0-dev:arm64 \
    libgpg-error-dev:arm64 \
    libpixman-1-dev:arm64 \
    libsdl2-dev:arm64 \
    libslirp-dev:arm64 \
    ninja-build \
    python3-pip \
    zlib1g-dev:arm64 \
&& :

/usr/bin/pip3 install meson==1.7.0 tomli==2.2.1
