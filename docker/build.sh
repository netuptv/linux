#! /bin/bash

set -e

if [ "${1}" = '-d' ]; then
    DEBUG_OPTS="-t -i"
    CMD=/bin/bash
else
    DEBUG_OPTS=
    CMD=/mnt/src/build.sh
fi

SRC_DIR=$(cd $(dirname ${0})/..; pwd)
BUILD_DIR=$(pwd)/build
OUT_DIR=$(pwd)/out

mkdir -p ${BUILD_DIR} ${OUT_DIR}

#    --volume ${BUILD_DIR}:/mnt/build \
docker run --rm \
    --volume ${SRC_DIR}:/mnt/src \
    --volume ${OUT_DIR}:/mnt/out \
    ${DEBUG_OPTS} \
    iptv/nbs_build:2015-08-11 \
    ${CMD}
