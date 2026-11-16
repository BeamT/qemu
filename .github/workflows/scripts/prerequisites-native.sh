#!/usr/bin/env bash

set -euo pipefail

export DEBIAN_FRONTEND="noninteractive"

# Debian 11 left the security mirrors when its LTS ended: deb.debian.org
# still serves a stale index whose packages are gone. Use the archive.
if grep -q '^VERSION_CODENAME=bullseye' /etc/os-release; then
    sed -i 's|http://deb.debian.org/debian-security|http://archive.debian.org/debian-security|' \
        /etc/apt/sources.list
fi

apt-get update -y -q -o Acquire::Check-Valid-Until=false
apt-get install -y -q --no-install-recommends \
    build-essential \
    git \
    libgcrypt-dev \
    libglib2.0-dev \
    libpixman-1-dev \
    libsdl2-dev \
    libslirp-dev \
    ninja-build \
    python3-pip \
    wget \
    zlib1g-dev \
&& :

# Even though ./configure installs meson, just specify a version
/usr/bin/pip3 install meson==1.7.0 tomli==2.2.1
