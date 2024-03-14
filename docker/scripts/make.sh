#! /bin/bash

set -e

SRC_DIR=$(cd $(dirname ${0})/../..; pwd)
BUILD_DIR=/mnt/build
CCACHE_DIR=/mnt/ccache
OUT_DIR=/mnt/out/
PERF_BUILD_DIR="${BUILD_DIR}/make/tools/perf"

export PATH=/usr/lib/ccache/:${PATH}
export CCACHE_DIR

mkdir -p "${BUILD_DIR}/make"
cp "${SRC_DIR}/config" "${BUILD_DIR}/make/.config"

cd ${SRC_DIR}
make -j "$(nproc)" O="${BUILD_DIR}/make" tar-pkg

RELEASE="$(cat ${BUILD_DIR}/make/include/config/kernel.release)"
TAR_FILE="${BUILD_DIR}/make/linux-${RELEASE}-x86.tar"

mv ${TAR_FILE} ${OUT_DIR}/linux-6.6.tar

mkdir -p ${PERF_BUILD_DIR}
(
    cd tools/perf
    make LDFLAGS=-static O=${PERF_BUILD_DIR}
)
cp ${PERF_BUILD_DIR}/perf ${OUT_DIR}/
