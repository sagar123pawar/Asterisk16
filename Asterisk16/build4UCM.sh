#!/bin/sh
export CUR_PATH=$(cd `dirname $0`; pwd)
export INSTALL_PREFIX=/app/asterisk
export EXTERNAL_PREFIX=${CUR_PATH}/../Share/external/GXV317X
export PKG_CONFIG_PATH=${EXTERNAL_PREFIX}/lib/pkgconfig
export PKG_CONFIG_LIBDIR=${EXTERNAL_PREFIX}/lib/pkgconfg
export CC="/opt/codesourcery/bin/arm-linux-gcc"
export CXX="/opt/codesourcery/bin/arm-linux-g++"
export AR="/opt/codesourcery/bin/arm-linux-ar"
export LIBEDIT_DIR="internal"
export CFLAGS="-I${EXTERNAL_PREFIX}/include \
    -I/opt/codesourcery/arm-none-linux-gnueabi/libc/usr/include \
    -I/opt/codesourcery/arm-none-linux-gnueabi/libc/usr/include/libxml2 \
    -I${EXTERNAL_PREFIX}/include/openr2 \
    -DLINUX \
    -DSVIP_DETECT_TONE \
    -DSVIP_DETECT_CID \
    -DSVIP_DETECT_DTMF \
    -DSVIP_FUNC_LEC \
    -DSVIP_RES_MANAGER \
    -DCURRENT_DISCONNECT \
    -DENABLE_FAX_GATEWAY_SUPPORT \
    -DHAVE_SIP_BLF_EVENTLIST \
    -DHAVE_SIP_CEI \
    -DNEGOTIATE_CODEC_ORDER \
    -DGRANDSTREAM_NETWORKS \
    -DSVIP_T38_STACK \
    -DDAHDI_FLASHTIME \
    -DJB_NETEQ"

export LDFLAGS="-L${EXTERNAL_PREFIX}/lib \
    -L/opt/codesourcery/arm-none-linux-gnueabi/libc/usr/lib"
#export LIBS=""
export CXXFLAGS="-I${EXTERNAL_PREFIX}/include \
    -I/opt/codesourcery/arm-none-linux-gnueabi/libc/usr/include"


./configure \
    --build=i386-linux \
    --host=arm-linux \
    --prefix=${INSTALL_PREFIX} \
    --with-speex=${EXTERNAL_PREFIX} \
    --with-spandsp=${EXTERNAL_PREFIX} \
    --with-srtp=${EXTERNAL_PREFIX} \
    --with-libxml2=/opt/codesourcery/arm-none-linux-gnueabi/libc/usr \
    --with-libcurl=/opt/codesourcery/arm-none-linux-gnueabi/libc/usr \
    --with-sdl=no \
    --with-dahdi=${EXTERNAL_PREFIX} \
    --with-tonezone=${EXTERNAL_PREFIX} \
    --with-pri=${EXTERNAL_PREFIX} \
    --with-ss7=${EXTERNAL_PREFIX} \
    --with-openr2=${EXTERNAL_PREFIX} \
    --with-jansson=${EXTERNAL_PREFIX} \
    --with-pjproject=${EXTERNAL_PREFIX} \
    --with-iksemel=${EXTERNAL_PREFIX} \
    --with-sqlite3=${EXTERNAL_PREFIX} \
    --without-pjproject-bundled \
    ac_cv_lib_pj_pj_ssl_cert_load_from_files2=yes \
    ac_cv_lib_pjsip_pjsip_auth_clt_deinit=yes \
    ac_cv_lib_pjsip_pjsip_dlg_create_uas_and_inc_lock=yes \
    ac_cv_lib_pjsip_pjsip_endpt_set_ext_resolver=yes \
    ac_cv_lib_pjsip_pjsip_evsub_add_ref=yes \
    ac_cv_lib_pjsip_pjsip_get_dest_info=yes \
    ac_cv_lib_pjsip_pjsip_inv_add_ref=yes \
    ac_cv_lib_pjsip_pjsip_tsx_create_uac2=yes \
    ac_cv_lib_pjsip_pjsip_tsx_layer_find_tsx2=yes

