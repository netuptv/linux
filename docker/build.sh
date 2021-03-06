#! /bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${0}")"; pwd)"
SRC_DIR="${SCRIPT_DIR}/.."
BUILD_DIR="$(pwd)/build/kernel"
CCACHE_DIR="$(pwd)/ccache/kernel"
OUT_DIR="$(pwd)/out/kernel"
REVISION="$(cd "${SRC_DIR}"; git rev-parse HEAD)"
[ -z "${BRANCH_NAME}" ] && \
    BRANCH_NAME="$(cd "${SRC_DIR}"; git symbolic-ref -q --short HEAD || echo unknown)"

mkdir -p "${OUT_DIR}" "${BUILD_DIR}" "${CCACHE_DIR}"

docker build --force-rm --iidfile "${BUILD_DIR}/image.id" - <<EOF
FROM debian:buster-slim

RUN sed 's/$/ contrib non-free/' -i /etc/apt/sources.list && \
    apt-get update && \
    apt-get install --no-install-recommends --assume-yes \
        gcc make flex bison ccache bc xz-utils \
        libc6-dev libssl-dev libelf-dev binutils-dev liblzma-dev libnuma-dev \
        zlib1g-dev libiberty-dev libslang2-dev python3 \
        dpkg-dev fakeroot build-essential:native kmod cpio && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*
EOF

CONTAINER_ID=
trap 'docker rm -f "${CONTAINER_ID}"' EXIT

CONTAINER_ID="$(docker run \
    --detach \
    --rm \
    --tty \
    --volume "${SRC_DIR}:/mnt/src" \
    --volume "${BUILD_DIR}:/mnt/build" \
    --volume "${CCACHE_DIR}:/mnt/ccache" \
    --volume "${OUT_DIR}:/mnt/out" \
    --user "${UID}" \
    "$( head -n1 "${BUILD_DIR}/image.id" )" \
    /mnt/src/docker/scripts/make.sh )"
docker logs --follow "${CONTAINER_ID}" &
[ $(docker wait "${CONTAINER_ID}") -eq 0 ]
trap - EXIT

printf 'kernel_revision="%s %s %s"\n' "${REVISION}" "${BRANCH_NAME}" "${BUILD_URL}" > "${OUT_DIR}/build.info"
