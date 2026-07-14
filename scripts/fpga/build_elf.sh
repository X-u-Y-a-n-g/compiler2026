#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <input.s> <output.elf>" >&2
  exit 2
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
assembly="$(realpath "$1")"
output="$2"
mkdir -p "$(dirname "$output")"
output="$(realpath -m "$output")"

work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

common_flags=(
  -O2
  -ffreestanding
  -fno-builtin
  -fno-stack-protector
  -march=rv64gc
  -mabi=lp64d
  -mcmodel=medany
)

riscv64-linux-gnu-gcc "${common_flags[@]}" -c \
  "$script_dir/start.S" -o "$work_dir/start.o"
riscv64-linux-gnu-gcc "${common_flags[@]}" -x c -c \
  "$script_dir/runtime.c.inc" -o "$work_dir/runtime.o"
riscv64-linux-gnu-gcc \
  -nostdlib -nostartfiles -static \
  -march=rv64gc -mabi=lp64d -mcmodel=medany \
  -Wl,--build-id=none \
  -Wl,-T,"$script_dir/link.ld" \
  "$work_dir/start.o" "$assembly" "$work_dir/runtime.o" \
  -lgcc -o "$output"

entry="$(riscv64-linux-gnu-readelf -h "$output" | awk '/Entry point address/ {print $4}')"
if [[ "$entry" != 0x90000000 ]]; then
  echo "error: unexpected FPGA ELF entry $entry" >&2
  exit 1
fi

echo "built $output"
riscv64-linux-gnu-readelf -l "$output" | sed -n '/LOAD/,+1p'
