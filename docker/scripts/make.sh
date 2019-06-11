#! /bin/bash

set -e

SRC_DIR=$(cd $(dirname ${0})/../..; pwd)
BUILD_DIR=/mnt/build
CCACHE_DIR=/mnt/ccache
OUT_DIR=/mnt/out/
PERF_BUILD_DIR=${BUILD_DIR}/tools/perf

export PATH=/usr/lib/ccache/:${PATH}
export CCACHE_DIR

cp ${SRC_DIR}/config.2.0 ${BUILD_DIR}/.config

cd ${SRC_DIR}
make -j $(nproc) O=${BUILD_DIR} tar-pkg

RELEASE=$(cat ${BUILD_DIR}/include/config/kernel.release)
TAR_FILE=${BUILD_DIR}/linux-${RELEASE}-x86.tar

mv ${TAR_FILE} ${OUT_DIR}/linux-4.19.tar

mkdir -p ${PERF_BUILD_DIR}
(
    cd tools/perf
    make LDFLAGS=-static O=${PERF_BUILD_DIR}
)
cp ${PERF_BUILD_DIR}/perf ${OUT_DIR}/
