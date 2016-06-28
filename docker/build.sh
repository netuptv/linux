#! /bin/bash

set -ex

SCRIPT_DIR=$(cd $(dirname ${0}); pwd)
SRC_DIR=${SCRIPT_DIR}/..
if [ "${JENKINS_HOME}" ]; then
    TTY_ARGS="-t"
    mkdir -p jenkins
    cd jenkins
    OUT_DIR=$(pwd)/out/linux-3.14
else
    TTY_ARGS="-it"
    OUT_ROOT=$(pwd)/out/linux-3.14
    OUT_DIR=${OUT_ROOT}/build_$(date -u +%F_%H-%M-%S)
fi
REVISION=$(cd ${SRC_DIR}; git rev-parse HEAD)
IMAGE_TAG=$(head -n 1 ${SCRIPT_DIR}/build.tag)
IMAGE_NAME=build.netup:5000/iptv_2.0_build:${IMAGE_TAG}

mkdir -p ${OUT_DIR}

docker run --rm \
    --volume ${SRC_DIR}:/mnt/src \
    --volume ${OUT_DIR}:/mnt/out \
    --user ${UID} \
    ${TTY_ARGS} \
    ${IMAGE_NAME} \
    /mnt/src/docker/scripts/make.sh

printf "kernel_314_revision=%s\n" "${REVISION}" > ${OUT_DIR}/build.info

if [ -z "${JENKINS_HOME}" ]; then
    rm -f ${OUT_ROOT}/latest
    ln -s $(basename ${OUT_DIR}) ${OUT_ROOT}/latest
fi
