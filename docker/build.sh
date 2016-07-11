#! /bin/bash

set -ex

SCRIPT_DIR=$(cd $(dirname ${0}); pwd)
SRC_DIR=${SCRIPT_DIR}/..
if [ "${JENKINS_HOME}" ]; then
    mkdir -p jenkins
    cd jenkins
fi
OUT_DIR=$(pwd)/out/linux-4.4
REVISION=$(cd ${SRC_DIR}; git rev-parse HEAD)
IMAGE_TAG=$(head -n 1 ${SCRIPT_DIR}/build.tag)
IMAGE_NAME=build.netup:5000/iptv_2.0_build:${IMAGE_TAG}

mkdir -p ${OUT_DIR}

docker run --rm \
    --volume ${SRC_DIR}:/mnt/src \
    --volume ${OUT_DIR}:/mnt/out \
    --user ${UID} \
    -t \
    ${IMAGE_NAME} \
    /mnt/src/docker/scripts/make.sh

printf "kernel_44_revision=%s\n" "${REVISION}" > ${OUT_DIR}/build.info
