#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_NAME="compiler-impl2026-env"

docker build --platform linux/amd64 -t "${IMAGE_NAME}" -f "${ROOT_DIR}/docker/impl2026/Dockerfile" "${ROOT_DIR}"

docker run --rm -it \
  --platform linux/amd64 \
  -v "${ROOT_DIR}:/workspace" \
  -w /workspace \
  "${IMAGE_NAME}" \
  /bin/bash
