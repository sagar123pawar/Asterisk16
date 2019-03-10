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

export CFLAGS=" -O0 -g \
    -I${EXTERNAL_PREFIX}/include \
    -DNDEBUG \
    -DLINUX \
    -D_GNU_SOURCE \
    -DGRANDSTREAM_NETWORKS"

export CXXFLAGS=" -O0 -g \
    -I${EXTERNAL_PREFIX}/include \
    -DNDEBUG \
    -DLINUX \
    -D_GNU_SOURCE \
    -DGRANDSTREAM_NETWORKS"

export LDFLAGS="-L${EXTERNAL_PREFIX}/lib"

export PKG_CONFIG_PATH="${PKG_CONFIG_PATH}:${EXTERNAL_PREFIX}/lib/pkgconfig"
export FONTCONFIG_CFLAGS="-I${EXTERNAL_PREFIX}/include"
export FONTCONFIG_LIBS="-L${EXTERNAL_PREFIX}/lib"

./configure \
    --prefix=${INSTALL_PREFIX} \
    --with-sdl=no \
    --with-pjproject=${EXTERNAL_PREFIX} \
    --without-pjproject-bundled


