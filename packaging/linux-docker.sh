#!/bin/bash
# Build inside Ubuntu 24.04.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake ninja-build pkg-config ca-certificates xz-utils \
    libgtk-4-dev libsqlite3-dev zlib1g-dev libexpat1-dev libzip-dev libxlsxwriter-dev >/dev/null
rm -rf /work && cp -r /src /work && cd /work
rm -rf build dist
BUILD=/tmp/build bash packaging/linux.sh
mkdir -p /out && cp dist/linux/M365-BEC-linux-x64.tar.gz /out/
cp /tmp/build/bec-tool /out/
