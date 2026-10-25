#!/bin/sh
# Usage: scripts/configure.sh [-b build_dir] [-a arch]
set -eu

cd "$(dirname "$(dirname "$(realpath "${0}")")")"

build_dir="${BUILD_DIR:-build}"
arch=

while getopts "b:a:h" option; do
    case "${option}" in
        b) build_dir="${OPTARG}" ;;
        a) arch="${OPTARG}" ;;
        *)
            echo "Usage: ${0} [-b build_dir] [-a arch]" >&2
            exit 2
            ;;
    esac
done

# shellcheck disable=SC2086
exec scripts/jinx-wrap init "${build_dir}" ${arch:+ARCH=${arch}}
