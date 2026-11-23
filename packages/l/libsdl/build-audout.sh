#!/usr/bin/env bash
set -euo pipefail

sdk="$1"
destination="$2"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
cd "$work"

curl -fsSL --connect-timeout 10 --max-time 60 \
    https://www.libsdl.org/release/SDL2-2.28.5.tar.gz -o SDL2.tar.gz
curl -fsSL --connect-timeout 10 --max-time 60 \
    https://raw.githubusercontent.com/devkitPro/pacman-packages/0ae8790/switch/SDL2/SDL2-2.28.5.patch \
    -o SDL2.patch
sha256sum --check - <<'CHECKSUMS'
332cb37d0be20cb9541739c61f79bae5a477427d79ae85e352089afdaf6666e4  SDL2.tar.gz
f5cf0336980c88559594cc30205022bdcf84be00e437e8b28f43a15dcbe0a58d  SDL2.patch
CHECKSUMS

tar -xf SDL2.tar.gz
cd SDL2-2.28.5
patch -s -p1 -i ../SDL2.patch

portlibs="$sdk/portlibs/switch"
export PATH="$sdk/devkitA64/bin:$portlibs/bin:$PATH"
export CPPFLAGS="-D__SWITCH__ -I$portlibs/include -isystem $sdk/libnx/include"
export CFLAGS="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIC -fno-strict-aliasing -O2 -ffunction-sections -fdata-sections"
export LDFLAGS="-L$portlibs/lib -L$sdk/libnx/lib"
export LIBS="-lnx"
./configure --prefix="$destination" --host=aarch64-none-elf \
    --disable-shared --enable-static --enable-audio --enable-joystick \
    --disable-power --disable-filesystem --enable-cpuinfo --enable-pthreads \
    --enable-timers --enable-video
make -j"${JOBS:-2}"
make DESTDIR="$work/install" install
cp -a "$work/install$destination/include/." "$destination/include"
mkdir -p "$destination/lib"
cp "$work/install$destination/lib/libSDL2.a" "$destination/lib/libSDL2.a.tmp"
mv -f "$destination/lib/libSDL2.a.tmp" "$destination/lib/libSDL2.a"
