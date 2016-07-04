#! /bin/bash

set -ex

cd $(dirname ${0})/../..

TARGET_DIR=/mnt/out/linux-4.4

mkdir -p ${TARGET_DIR}

[ -f .config ] || cp config.2.0 .config

make -j 8
make tar-pkg

RELEASE=$(cat include/config/kernel.release)
TAR_FILE=linux-${RELEASE}-x86.tar

mv ${TAR_FILE} ${TARGET_DIR}/linux-4.4.tar
