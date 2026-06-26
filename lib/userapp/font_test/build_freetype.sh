#!/bin/bash

set -e

LIBZIP_PATH=${sdk_root}/sw_module/libs/${CHIPSET_U}

# create the output directory
# if [ ! -d freetype_output ]; then
#     mkdir freetype_output
# fi

if [ -e freetype ]; then
    cd freetype
else
    echo "clone repository freetype!"

    # clone the freetype repository
    git clone https://gitlab.freedesktop.org/freetype/freetype.git

    cd freetype
fi

./autogen.sh

# Clear any stale config from a previous (failed) build so the new
# --with-* flags below take full effect. Harmless on a fresh clone.
make distclean 2>/dev/null || true

# Disable optional deps that configure would otherwise auto-detect (and then
# try to link), none of which the cross sysroot ships and none of which TTF
# outline rendering needs:
#   --with-png=no       embedded color-bitmap (PNG) glyphs  -> drops -lpng16
#   --with-harfbuzz=no  complex-text shaping
#   --with-brotli=no    WOFF2 font decompression
#   --with-bzip2=no     bzip2-compressed PCF fonts
# zlib is kept and pointed at the SDK build per the original intent.
./configure ARCH=riscv CC=${CROSS_COMPILE}gcc --host=${CROSS_PLATFORM} \
    --with-zlib=$LIBZIP_PATH \
    --with-png=no --with-harfbuzz=no --with-brotli=no --with-bzip2=no \
    --prefix=$(pwd)/../libs

make -j4
make install

cd ../
rm -rf freetype