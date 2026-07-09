#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fwctl="${repo_root}/build/fwctl"

cd "${repo_root}"

"${repo_root}/scripts/load-kernel-module.sh"
trap '"${repo_root}/scripts/unload-kernel-module.sh"' EXIT

"${fwctl}" version
"${fwctl}" selftest
"${fwctl}" run-vector-add
"${fwctl}" run-vector-add 1,1 1,1
"${fwctl}" run-vector-add 5,6,7 50,60,70
"${fwctl}" run-vector-add-async 2,4,6 20,40,60
"${fwctl}" run-matrix-mul
"${fwctl}" run-matrix-mul 2 2 3 1,2,3,4,5,6 7,8,9,10,11,12
"${fwctl}" memcopy
"${fwctl}" telemetry
"${fwctl}" trigger-fault
"${fwctl}" trigger-timeout
"${fwctl}" reset
"${fwctl}" dump-status
"${fwctl}" dump-trace
"${repo_root}/build/afl-kernel-verifier"

echo "kernel smoke test: PASS"
