#! /bin/bash

set -e

SRC_DIR=$(cd $(dirname ${0})/../..; pwd)
BUILD_DIR=/mnt/build
MAKE_DIR="${BUILD_DIR}/make"
CCACHE_DIR=/mnt/ccache
OUT_DIR=/mnt/out/
PERF_BUILD_DIR="${MAKE_DIR}/tools/perf"

export PATH=/usr/lib/ccache/:${PATH}
export CCACHE_DIR

mkdir -p "${MAKE_DIR}"
cp "${SRC_DIR}/config.2.0" "${MAKE_DIR}/.config"

cd ${SRC_DIR}
make -j "$(nproc)" O="${MAKE_DIR}"
make -j "$(nproc)" O="${MAKE_DIR}" install modules_install

cd "${BUILD_DIR}"/ngbe-*/src
make CFLAGS_EXTRA="-DNGBE_NO_LRO" KSRC="${SRC_DIR}" KOBJ="${MAKE_DIR}" MANDIR="${BUILD_DIR}/man" install

tar cfC ${OUT_DIR}/linux-5.4.tar / boot lib/modules

mkdir -p ${PERF_BUILD_DIR}
(
    cd "${SRC_DIR}/tools/perf"
    make LDFLAGS=-static O=${PERF_BUILD_DIR}
)
cp ${PERF_BUILD_DIR}/perf ${OUT_DIR}/
