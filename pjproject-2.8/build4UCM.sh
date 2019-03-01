#!/bin/sh
DIR_ROOT=$(cd `dirname $0`; pwd)
export PJDIR=${DIR_ROOT}

export INSTALL_PREFIX=/app/asterisk
export EXTERNAL_PREFIX=${DIR_ROOT}/../GXV317X
export CC="/opt/codesourcery/bin/arm-linux-gcc"
export CXX="/opt/codesourcery/bin/arm-linux-g++"
export AR="/opt/codesourcery/bin/arm-linux-ar "
export CFLAGS="-g -I./ -I/opt/codesourcery/arm-none-linux-gnueabi/libc/usr/include \
               -I${EXTERNAL_PREFIX}/include \
			   -DNDEBUG \
               -DGRANDSTREAM_NETWORKS \
               -D_GNU_SOURCE"
export LDFLAGS="-L/opt/codesourcery/arm-none-linux-gnueabi/libc/usr/lib \
                -L${EXTERNAL_PREFIX}/lib -lsrtp"
#export LIBS=""
export CXXFLAGS="-g -L/opt/codesourcery/arm-none-linux-gnueabi/libc/usr/include -DNDEBUG"

MALLOC_DEBUG_LDFLAGS="-L${PJDIR}/pjsip-apps/lib -lasterisk_malloc_debug"

echo  Compiling asterisk debug malloc stubs....
cd ${DIR_ROOT}/third_party/asterisk
`${CC} -fPIC -shared asterisk_malloc_debug.c -o libasterisk_malloc_debug.so`

if [ -d ${PJDIR}/pjsip-apps/lib ];
then
    cp -f libasterisk_malloc_debug.so ${PJDIR}/pjsip-apps/lib
else
    mkdir -p ${PJDIR}/pjsip-apps/lib
    cp -f libasterisk_malloc_debug.so ${PJDIR}/pjsip-apps/lib
fi
cd -
echo Asterisk debug malloc build done.

LDFLAGS="${LDFLAGS} ${MALLOC_DEBUG_LDFLAGS}"

#Remove old files
rm -rf ${EXTERNAL_PREFIX}/lib/libg7221codec.so*
rm -rf ${EXTERNAL_PREFIX}/lib/libgsmcodec.so*
rm -rf ${EXTERNAL_PREFIX}/lib/libilbccodec.so*
rm -rf ${EXTERNAL_PREFIX}/lib/libpj*.so*
rm -rf ${EXTERNAL_PREFIX}/lib/pkgconfig/libpjproject.pc
rm -rf ${EXTERNAL_PREFIX}/include/pj*

# terminate as soon as any command fails
set -e

./configure \
    --build=i386-linux \
    --host=arm-linux \
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
    --enable-epoll \
    --with-external-srtp=${EXTERNAL_PREFIX} \
    --with-ssl=/opt/codesourcery/arm-none-linux-gnueabi/libc/usr \
    ac_cv_func_malloc_0_nonnull=yes \
    ac_cv_func_realloc_0_nonnull=yes

make clean
make
make install

# Copy files
cp -af ${INSTALL_PREFIX}/lib/libpj*.so* ${EXTERNAL_PREFIX}/lib
cp -af ${INSTALL_PREFIX}/lib/pkgconfig/libpjproject.pc ${EXTERNAL_PREFIX}/lib/pkgconfig
cp -af ${INSTALL_PREFIX}/include/pj* ${EXTERNAL_PREFIX}/include
cp -af ${INSTALL_PREFIX}/lib/libpj*.so* \
    ${DIR_ROOT}/../../../UCMRelease/prog/asterisk/lib
