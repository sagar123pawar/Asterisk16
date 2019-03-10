#########################################################################
# File Name: build4PC.sh
# Author: ma6174
# mail: ma6174@163.com
# Created Time: 2019年02月22日 星期五 11时13分22秒
#########################################################################
#!/bin/bash

ROOT_DIR=$(cd `dirname $0`; pwd)

export INSTALL_PREFIX=/opt/asterisk
export EXTERNAL_PREFIX=/opt/asterisk

export CC="/usr/bin/gcc"
export CXX="/usr/bin/g++"

export CFLAGS=" -g \
    -DNDEBUG \
    -D_ASTERISK_ASTMM_H \
    -DGRANDSTREAM_NETWORKS \
    -D_GNU_SOURCE"

export CXXFLAGS=" -g \
    -DNDEBUG \
    -D_ASTERISK_ASTMM_H \
    -DGRANDSTREAM_NETWORKS \
    -D_GNU_SOURCE"

export LDFLAGS="-L${EXTERNAL_PREFIX}/lib"

./configure \
    --prefix=${INSTALL_PREFIX} \
    --enable-shared \
    --disable-speex-codec \
    --disable-speex-aec \
    --disable-bcg729 \
    --disable-gsm-codec \
    --disable-ilbc-codec \
    --disable-l16-codec \
    --disable-g722-codec \
    --disable-g7221-codec \
    --disable-opencore-amr \
    --disable-silk \
    --disable-opus \
    --disable-video \
    --disable-v4l2 \
    --disable-sound \
    --disable-ext-sound \
    --disable-sdl \
    --disable-libyuv \
    --disable-ffmpeg \
    --disable-openh264 \
    --disable-ipp \
    --disable-libwebrtc \
    --without-external-pa \
    --disable-resample \
    --disable-g711-codec \
    --enable-epoll



