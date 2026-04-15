#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_NAME="compiler-impl2026-env"

docker build --platform linux/amd64 -t "${IMAGE_NAME}" -f "${ROOT_DIR}/docker/impl2026/Dockerfile" "${ROOT_DIR}"

docker run --rm \
  --platform linux/amd64 \
  -v "${ROOT_DIR}:/workspace" \
  -w /workspace \
  "${IMAGE_NAME}" \
  bash -lc "cmake -S /workspace -B /tmp/compiler-impl2026-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ && cmake --build /tmp/compiler-impl2026-build"
