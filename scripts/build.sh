#!/bin/sh
# Usage: scripts/build.sh [jinx update arguments]
# Builds everything by default
set -eu

cd "$(dirname "$(dirname "$(realpath "${0}")")")"

if test "${#}" -eq 0; then
    set -- '*'
fi

exec scripts/jinx-wrap update -b "${@}"
