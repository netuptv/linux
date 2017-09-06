#! /bin/bash

set -e

SCRIPT_DIR=$(cd $(dirname ${0}); pwd)
SRC_DIR=${SCRIPT_DIR}/..
BUILD_DIR=$(pwd)/build/kernel
CCACHE_DIR=$(pwd)/ccache/kernel
OUT_DIR=$(pwd)/out/kernel
REVISION=$(cd ${SRC_DIR}; git rev-parse HEAD)
[ -z "${BRANCH_NAME}" ] && \
    BRANCH_NAME=$(cd ${SRC_DIR}; git symbolic-ref -q --short HEAD || echo unknown)
IMAGE_TAG=$(head -n 1 ${SCRIPT_DIR}/build.tag)
IMAGE_NAME=build.netup:5000/iptv_2.0_build:${IMAGE_TAG}

mkdir -p ${OUT_DIR} ${BUILD_DIR} ${CCACHE_DIR}

docker run --rm -t \
    --volume ${SRC_DIR}:/mnt/src:ro \
    --volume ${BUILD_DIR}:/mnt/build \
    --volume ${CCACHE_DIR}:/mnt/ccache \
    --volume ${OUT_DIR}:/mnt/out \
    --user ${UID} \
    ${IMAGE_NAME} \
    /mnt/src/docker/scripts/make.sh

printf 'kernel_revision="%s %s %s"\n' "${REVISION}" "${BRANCH_NAME}" "${BUILD_URL}" > ${OUT_DIR}/build.info
