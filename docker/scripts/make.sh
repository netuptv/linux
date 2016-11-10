#! /bin/bash

set -e

SRC_DIR=$(cd $(dirname ${0})/../..; pwd)
BUILD_DIR=/mnt/build
CCACHE_DIR=/mnt/ccache
OUT_DIR=/mnt/out/

export PATH=/usr/lib/ccache/:${PATH}
export CCACHE_DIR

[ -f ${BUILD_DIR}/.config ] || cp ${SRC_DIR}/config.2.0 ${BUILD_DIR}/.config

cd ${SRC_DIR}
make -j $(nproc) O=${BUILD_DIR} tar-pkg

RELEASE=$(cat ${BUILD_DIR}/include/config/kernel.release)
TAR_FILE=${BUILD_DIR}/linux-${RELEASE}-x86.tar

mv ${TAR_FILE} ${OUT_DIR}/linux-4.4.tar
