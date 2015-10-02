#! /bin/bash

set -ex

cd $(dirname ${0})/../..

TARGET_DIR=/mnt/out/linux-3.14

mkdir -p ${TARGET_DIR}

[ -f .config ] || cp config.2.0 .config

make -j 8
make tar-pkg

RELEASE=$(cat include/config/kernel.release)
#TAR_FILE=${BUILD_DIR}/linux-${RELEASE}-x86.tar
TAR_FILE=linux-${RELEASE}-x86.tar

mv ${TAR_FILE} ${TARGET_DIR}/linux-3.14.tar
