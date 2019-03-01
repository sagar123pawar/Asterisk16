#########################################################################
# File Name: build4PC.sh
# Author: ma6174
# mail: ma6174@163.com
# Created Time: 2019年02月22日 星期五 11时13分22秒
#########################################################################
#!/bin/bash

ROOT_DIR=$(cd `dirname $0`; pwd)

export INSTALL_PREFIX=/opt/usr/asterisk
export EXTERNAL_PREFIX=/opt/usr/asterisk

export CC="/usr/bin/gcc"
export CXX="/usr/bin/g++"

#echo  Compiling asterisk debug malloc stubs....
#cd ${ROOT_DIR}/third_party/asterisk
#`${CC} -fPIC -shared asterisk_malloc_debug.c -o libasterisk_malloc_debug.so`

#if [ -d ${ROOT_DIR}/pjsip-apps/lib ];
#then
#    cp -f libasterisk_malloc_debug.so ${ROOT_DIR}/pjsip-apps/lib
#else
#    mkdir -p ${ROOT_DIR}/pjsip-apps/lib
#    cp -f libasterisk_malloc_debug.so ${ROOT_DIR}/pjsip-apps/lib
#fi
#cd -
#echo Asterisk debug malloc build done.

export CFLAGS=" \
    -DNDEBUG \
    -D_ASTERISK_ASTMM_H \
    -DGRANDSTREAM_NETWORKS \
    -D_GNU_SOURCE"

export CXXFLAGS=" \
    -DNDEBUG \
    -D_ASTERISK_ASTMM_H \
    -DGRANDSTREAM_NETWORKS \
    -D_GNU_SOURCE"

export LDFLAGS="-L${EXTERNAL_PREFIX}/lib" # \
#    -L${ROOT_DIR}/pjsip-apps/lib -lasterisk_malloc_debug"

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
