#!/usr/bin/env bash
set -euo pipefail
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export VCPKG_ROOT="${VCPKG_ROOT:-$project_root/.deps/vcpkg}"
export VCPKG_DEFAULT_BINARY_CACHE="${VCPKG_DEFAULT_BINARY_CACHE:-$project_root/.cache/vcpkg}"
export VCPKG_DISABLE_METRICS=1
mkdir -p "$VCPKG_ROOT" "$VCPKG_DEFAULT_BINARY_CACHE"
revision=469a0504e4a6ebf6173d1c0f51a7fa34c382fd43
if [[ ! -f "$VCPKG_ROOT/.git/HEAD" ]]; then
    git -C "$VCPKG_ROOT" init
    git -C "$VCPKG_ROOT" remote add origin https://github.com/microsoft/vcpkg.git
    git -C "$VCPKG_ROOT" fetch --depth=1 origin "$revision"
    git -C "$VCPKG_ROOT" checkout --detach FETCH_HEAD
fi
[[ "$(git -C "$VCPKG_ROOT" rev-parse HEAD)" == "$revision" ]] || {
    echo 'The existing vcpkg checkout does not match the pinned revision.' >&2
    exit 1
}
if [[ ! -x "$VCPKG_ROOT/vcpkg" ]]; then
    "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
fi
printf 'Use VCPKG_ROOT=%s when configuring CMake.\n' "$VCPKG_ROOT"
