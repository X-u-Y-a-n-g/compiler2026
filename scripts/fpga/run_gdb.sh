#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "usage: $0 <program.elf> <input-file> [gdb-target]" >&2
  exit 2
fi

elf="$(realpath "$1")"
input="$(realpath "$2")"
target="${3:-${FPGA_GDB_TARGET:-host.docker.internal:3333}}"
timeout_sec="${FPGA_GDB_TIMEOUT:-600}"

padded_input="$(mktemp)"
trap 'rm -f "$padded_input"' EXIT
cp "$input" "$padded_input"
printf '\0' >> "$padded_input"

timeout "${timeout_sec}s" gdb-multiarch -q -nx -batch "$elf" \
  -ex "set debuginfod enabled off" \
  -ex "set remotetimeout 60" \
  -ex "target remote $target" \
  -ex "restore $padded_input binary 0xc0000000" \
  -ex "load" \
  -ex "break __done" \
  -ex "continue" \
  -ex "info registers pc sp a0" \
  -ex "x/4gx 0xcf000000" \
  -ex "x/gx 0xcf000018" \
  -ex "x/s 0xcf000100" \
  -ex "detach"
