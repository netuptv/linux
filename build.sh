#! /bin/bash

set -ex

cd $(dirname ${0})

#BUILD_DIR=/mnt/build/linux-3.14
TARGET_DIR=/mnt/out/linux-3.14

#mkdir -p ${BUILD_DIR}
mkdir -p ${TARGET_DIR}

#cp .config ${BUILD_DIR}/
#make O=${BUILD_DIR}
#make O=${BUILD_DIR} tar-pkg

make -j 8
make tar-pkg

RELEASE=$(cat include/config/kernel.release)
#TAR_FILE=${BUILD_DIR}/linux-${RELEASE}-x86.tar
TAR_FILE=linux-${RELEASE}-x86.tar

mv ${TAR_FILE} ${TARGET_DIR}/
