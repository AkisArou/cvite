#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
prefix=${CVITE_INSTALL_PREFIX:-"$root/.local"}

if [[ $(uname -s) != Linux ]]; then
  echo "This bootstrap script currently supports Linux only." >&2
  exit 1
fi

if ! command -v apt-get >/dev/null 2>&1; then
  echo "apt-get is required by this Ubuntu bootstrap script." >&2
  exit 1
fi

sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  ninja-build \
  clang-18 \
  llvm-18 \
  llvm-18-dev \
  libclang-18-dev \
  python3

cmake --preset llvm18 -S "$root"
cmake --build --preset llvm18
ctest --preset llvm18
cmake --install "$root/build/llvm18" --prefix "$prefix"

cat <<EOF

CVite was installed to:
  $prefix

Add it to the current shell with:
  export PATH="$prefix/bin:\$PATH"

Then validate the toolchain with:
  cvite doctor
EOF
