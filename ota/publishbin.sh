#!/bin/bash

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

version=$(cat ${DIR}/../version.txt)
cp ${2}/build/${1}.bin /home/matthew/go/src/github.com/matt-g-everett/iotd/data/ota/${1}_${version}.bin
