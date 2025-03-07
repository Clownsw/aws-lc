#!/usr/bin/env bash
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: Apache-2.0 OR ISC

set -e

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
AWS_LC_DIR=$( cd -- "${SCRIPT_DIR}/../../../" &> /dev/null && pwd)

echo ARGS: "${@}"

IGNORE_MACOS=0

while getopts "m" option; do
  case ${option} in
  m )
    IGNORE_MACOS=1
    ;;
  * )
    echo "$(basename "${0}")" - Invalid argument: -"${?}"
    echo
    exit 1
    ;;
  esac
done

## macos x86_64 bindings
if [[ ! "${OSTYPE}" == "darwin"* ]]; then
  echo This script is not running on MacOS.
  if [[ ${IGNORE_MACOS} -eq 0 ]]; then
    echo Aborting. Use '-m' to ignore.
    echo
    exit 1
  else
    echo Ignoring non-MacOS. Bindings will not be generated for Mac.
    echo
  fi
else
  "${SCRIPT_DIR}"/_generate_bindings.sh
fi

pushd "${AWS_LC_DIR}"

## TODO: Find a way to pre-generate bindings for macos-aarch64 on the fly.

##
## These docker image can be built from Dockerfiles under: <AWS-LC-DIR>/tests/ci/docker_images/rust
##

## 386 build
docker run -v "${AWS_LC_DIR}":"${AWS_LC_DIR}" -w "${AWS_LC_DIR}" --rm --platform linux/386 rust:linux-386 /bin/bash "${SCRIPT_DIR}"/_generate_bindings.sh

## linux x86_64 build
docker run -v "${AWS_LC_DIR}":"${AWS_LC_DIR}" -w "${AWS_LC_DIR}" --rm --platform linux/amd64 rust:linux-x86_64 /bin/bash "${SCRIPT_DIR}"/_generate_bindings.sh

## linux aarch64 build
docker run -v "${AWS_LC_DIR}":"${AWS_LC_DIR}" -w "${AWS_LC_DIR}" --rm --platform linux/arm64 rust:linux-arm64 /bin/bash "${SCRIPT_DIR}"/_generate_bindings.sh

popd
