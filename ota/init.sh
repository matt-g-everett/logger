#!/bin/bash

OTA_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null && pwd )"
PUBLISH_DIR="${OTA_DIR}/../publish"

mkdir -p ${PUBLISH_DIR}

if [ ! -e ${PUBLISH_DIR}/logger.bin ]; then
    ln -sfn ${OTA_DIR}/../build/logger.bin ${PUBLISH_DIR}/logger.bin
fi
