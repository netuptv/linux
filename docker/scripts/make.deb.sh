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
make -j $(nproc) O=${BUILD_DIR} bindeb-pkg

RELEASE=$(cat ${BUILD_DIR}/include/config/kernel.release)
mv ${BUILD_DIR}/../*.deb ${OUT_DIR}/
