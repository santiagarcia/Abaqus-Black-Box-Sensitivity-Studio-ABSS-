#!/usr/bin/env bash
# Build dist/ABSS-Installer-Linux-<version>.run: one self-extracting file (a short shell header followed by a
# gzipped tar of the ABSS source). On the target it unpacks to a temporary folder and runs packaging/linux/install.sh
# with the user's options. Works from Linux, macOS or MSYS2/Git Bash on Windows.
#   packaging/linux/make_installer.sh [out-dir]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${1:-$ROOT/dist}"
VERSION="$(sed -n 's/^project(ABSS VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
mkdir -p "$OUT"
RUN="$OUT/ABSS-Installer-Linux-$VERSION.run"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/abss-src"
# Source only: never build trees, distributions or private validation material (third-party proprietary models).
( cd "$ROOT" && tar -cf - --exclude='*/__pycache__' \
    CMakeLists.txt cmake include src apps tests tools examples docs packaging scripts README.md ) | tar -xf - -C "$stage/abss-src"
cat >"$RUN" <<'HEADER'
#!/usr/bin/env bash
# ABSS - Abaqus Black-Box Sensitivity Studio: installer for Linux workstations and HPC clusters.
#   chmod +x ABSS-Installer-Linux-*.run && ./ABSS-Installer-Linux-*.run [--prefix DIR] [--cli-only] [--help]
# Installs into your home folder (default ~/abss); needs no root rights and no system packages.
set -euo pipefail
WORK="$(mktemp -d "${TMPDIR:-/tmp}/abss-install.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
LINE=$(awk '/^__ABSS_PAYLOAD_BELOW__$/ { print NR + 1; exit 0 }' "$0")
tail -n +"$LINE" "$0" | tar -xz -C "$WORK" || { echo "ABSS installer: the archive is damaged (download it again)" >&2; exit 1; }
bash "$WORK/abss-src/packaging/linux/install.sh" "$@"
exit $?
__ABSS_PAYLOAD_BELOW__
HEADER
( cd "$stage" && tar -czf - abss-src ) >>"$RUN"
chmod +x "$RUN"
echo "Linux installer: $RUN ($(du -h "$RUN" | cut -f1))"
