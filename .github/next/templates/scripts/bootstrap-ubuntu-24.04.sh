#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -eq 0 ]]; then
  SUDO=()
else
  SUDO=(sudo)
fi

"${SUDO[@]}" apt-get update
"${SUDO[@]}" apt-get install -y \
  build-essential \
  cmake \
  ninja-build \
  clang-18 \
  llvm-18 \
  llvm-18-dev \
  libclang-18-dev \
  python3

cmake --preset llvm18-dev
cmake --build --preset llvm18-dev
ctest --preset llvm18-dev

install_root="${CVITE_INSTALL_ROOT:-$PWD/.local}"
cmake --install build/llvm18-dev --prefix "$install_root"

cat <<EOF

CVite development toolchain is ready.

Add the local install to this shell:
  export PATH="$install_root/bin:\$PATH"

Then run:
  cvite doctor
  cvite run examples/live-counter
EOF
