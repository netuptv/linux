#! /bin/bash

set -ex

SCRIPT_DIR=$(cd $(dirname ${0}); pwd)
SRC_DIR=${SCRIPT_DIR}/..
BUILD_DIR=$(pwd)/build/
OUT_DIR=$(pwd)/out/

IMAGE_TAG=$(head -n 1 ${SCRIPT_DIR}/build.tag)
IMAGE_NAME=build.netup:5000/iptv_2.0_build:${IMAGE_TAG}

mkdir -p ${BUILD_DIR} ${OUT_DIR}

#    --volume ${BUILD_DIR}:/mnt/build \
docker run --rm -it \
    --volume ${SRC_DIR}:/mnt/src \
    --volume ${OUT_DIR}:/mnt/out \
    ${IMAGE_NAME} \
    /mnt/src/docker/scripts/make.sh
