#! /bin/bash

set -e

SRC_DIR=$(cd $(dirname ${0})/../..; pwd)
BUILD_DIR=/mnt/build/subdir
CCACHE_DIR=/mnt/ccache
OUT_DIR=/mnt/out/
PERF_BUILD_DIR=${BUILD_DIR}/tools/perf

export PATH=/usr/lib/ccache/:${PATH}
export CCACHE_DIR

mkdir -p ${BUILD_DIR}

cp ${SRC_DIR}/config-ubuntu.4.4.129+ ${BUILD_DIR}/.config

cd ${SRC_DIR}
make -j $(nproc) O=${BUILD_DIR} deb-pkg

mkdir -p ${PERF_BUILD_DIR}
(
    cd tools/perf
    make LDFLAGS=-static O=${PERF_BUILD_DIR}
)
cp ${PERF_BUILD_DIR}/perf ${OUT_DIR}/
