#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
readonly EXPECTED_ATMOSPHERE="6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8"
readonly EXPECTED_LIBNX="b156b4774fb2df7b93402169509266da7547fc4c"

if [[ -z "${ATMOSPHERE_ROOT:-}" ]]; then
  echo "Set ATMOSPHERE_ROOT to the pinned Atmosphere checkout." >&2
  echo "Expected revision: $EXPECTED_ATMOSPHERE" >&2
  exit 2
fi
ATMOSPHERE_ROOT="$(cd -- "$ATMOSPHERE_ROOT" && pwd)"
if ! git -C "$ATMOSPHERE_ROOT" rev-parse --verify HEAD >/dev/null 2>&1; then
  echo "ATMOSPHERE_ROOT is not a Git checkout: $ATMOSPHERE_ROOT" >&2
  exit 2
fi

SYSTEM_DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
if [[ ! -d "$SYSTEM_DEVKITPRO/devkitA64" ]]; then
  echo "devkitA64 not found under $SYSTEM_DEVKITPRO" >&2
  exit 2
fi

actual_atmosphere="$(git -C "$ATMOSPHERE_ROOT" rev-parse HEAD)"
if [[ "$actual_atmosphere" != "$EXPECTED_ATMOSPHERE" ]]; then
  echo "Atmosphere revision mismatch: expected $EXPECTED_ATMOSPHERE, got $actual_atmosphere" >&2
  exit 2
fi

git -C "$ROOT" submodule update --init deps/libnx
actual_libnx="$(git -C "$ROOT/deps/libnx" rev-parse HEAD)"
if [[ "$actual_libnx" != "$EXPECTED_LIBNX" ]]; then
  echo "libnx revision mismatch: expected $EXPECTED_LIBNX, got $actual_libnx" >&2
  exit 2
fi

PROJECT_TOOLCHAIN="$ROOT/toolchain"
mkdir -p "$PROJECT_TOOLCHAIN"
ln -sfn "$SYSTEM_DEVKITPRO/devkitA64" "$PROJECT_TOOLCHAIN/devkitA64"
if [[ -d "$SYSTEM_DEVKITPRO/portlibs" ]]; then
  ln -sfn "$SYSTEM_DEVKITPRO/portlibs" "$PROJECT_TOOLCHAIN/portlibs"
fi

export DEVKITPRO="$PROJECT_TOOLCHAIN"
export DEVKITA64="$PROJECT_TOOLCHAIN/devkitA64"
export PATH="$DEVKITA64/bin:$SYSTEM_DEVKITPRO/tools/bin:$PATH"

for tool in npdmtool elf2nso build_pfs0; do
  command -v "$tool" >/dev/null || {
    echo "Missing required Switch packaging tool: $tool" >&2
    exit 2
  }
done

make -C "$ROOT/deps/libnx" -j"$(nproc)"
make -C "$ROOT/deps/libnx" install

grep -q "FsStorageForBatchRead" "$DEVKITPRO/libnx/include/switch/services/fs.h"
grep -q "GetRunningApplicationProcessId" \
  "$ATMOSPHERE_ROOT/libraries/libstratosphere/include/stratosphere/pgl/pgl_shell_api.hpp"

# Never pull, rebase, reset, or otherwise mutate the user's Git checkout here.
make -C "$ROOT/sysmodule" ATMOSPHERE_ROOT="$ATMOSPHERE_ROOT" nx_release

ARTIFACT="$ROOT/sysmodule/out/nintendo_nx_arm64_armv8a/release/nx_pctl_manager.nsp"
if [[ ! -s "$ARTIFACT" ]]; then
  echo "Expected non-empty NSP was not produced: $ARTIFACT" >&2
  exit 1
fi
file "$ARTIFACT"
echo "Build finished: $ARTIFACT"
