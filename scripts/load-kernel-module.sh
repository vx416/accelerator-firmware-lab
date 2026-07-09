#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
module_name="afl_kernel"
module_path="${repo_root}/kernel/${module_name}.ko"

cd "${repo_root}"

cmake -S . -B build
cmake --build build
make -C kernel

if lsmod | awk '{print $1}' | grep -qx "${module_name}"; then
  sudo rmmod "${module_name}"
fi

sudo insmod "${module_path}"

if [ ! -e /dev/afl0 ]; then
  echo "error: /dev/afl0 was not created" >&2
  exit 1
fi

echo "loaded ${module_name}: /dev/afl0 is ready"
