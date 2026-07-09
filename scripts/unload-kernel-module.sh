#!/usr/bin/env bash
set -euo pipefail

module_name="afl_kernel"

if lsmod | awk '{print $1}' | grep -qx "${module_name}"; then
  sudo rmmod "${module_name}"
  echo "unloaded ${module_name}"
else
  echo "${module_name} is not loaded"
fi
