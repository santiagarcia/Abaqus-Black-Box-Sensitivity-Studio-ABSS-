#!/usr/bin/env bash
# ABSS installer for Linux workstations and HPC clusters. Run by ABSS-Installer-Linux-<version>.run (which unpacks
# the source next to this script); usable on its own from a source checkout too:  packaging/linux/install.sh [options]
#
# Everything installs into one folder owned by the user: no root, no system packages; internet access is needed only
# for conda-forge (or pass --toolchain with an existing conda prefix that has the dependencies). Steps:
#   1. a private toolchain (micromamba + conda-forge: compilers, CMake, Ninja, Eigen, SuiteSparse, Kokkos/OpenMP,
#      OpenBLAS, nlohmann_json, doctest, and Qt 6 + VTK for the desktop application)
#   2. build ABSS and run its test suite
#   3. install abss / abss-cli launchers, examples, docs, an environment-module file and a Slurm job template
#   4. find Abaqus (ABSS_ABAQUS_CMD, PATH, environment modules, /opt/SIMULIA, /usr/SIMULIA) and record it
#
# Options:
#   --prefix DIR        install folder (default: $HOME/abss)
#   --cli-only          headless build (no Qt/VTK); the default on clusters and machines without a display
#   --gui               force the desktop application even without a display
#   --jobs N            parallel build jobs (default: all cores)
#   --abaqus CMD        Abaqus command to record (default: detected)
#   --no-tests          skip the test suite
#   --no-shell-rc       do not add the install folder to ~/.bashrc
#   --toolchain DIR     reuse an existing conda prefix with the dependencies
#   --yes               non-interactive (the installer never asks anything anyway; kept for scripts)
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PREFIX="${HOME}/abss"
MODE="auto"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
ABAQUS_CMD="${ABSS_ABAQUS_CMD:-}"
RUN_TESTS=1
SHELL_RC=1
TOOLCHAIN=""
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="$2"; shift 2 ;;
    --cli-only) MODE="cli"; shift ;;
    --gui) MODE="gui"; shift ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --abaqus) ABAQUS_CMD="$2"; shift 2 ;;
    --no-tests) RUN_TESTS=0; shift ;;
    --no-shell-rc) SHELL_RC=0; shift ;;
    --toolchain) TOOLCHAIN="$2"; shift 2 ;;
    --yes|-y) shift ;;
    -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done

step() { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }
ok()   { printf '   \033[32mOK\033[0m  %s\n' "$*"; }
note() { printf '   ->  %s\n' "$*"; }
die()  { printf '\n\033[1;31mABSS installer: %s\033[0m\n' "$*" >&2; exit 1; }

# ---- 0. platform ------------------------------------------------------------------------------------------------
step "Checking this computer"
[ "$(uname -s)" = "Linux" ] || die "this installer is for Linux (use ABSS-Setup.exe on Windows)"
case "$(uname -m)" in
  x86_64) PLAT="linux-64" ;;
  aarch64) PLAT="linux-aarch64" ;;
  ppc64le) PLAT="linux-ppc64le" ;;
  *) die "unsupported CPU architecture $(uname -m)" ;;
esac
CLUSTER=0
for c in sbatch qsub bsub; do command -v "$c" >/dev/null 2>&1 && CLUSTER=1; done
[ -n "${SLURM_JOB_ID:-}${PBS_JOBID:-}${LSB_JOBID:-}" ] && CLUSTER=1
HAVE_DISPLAY=0
[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && HAVE_DISPLAY=1
if [ "$MODE" = "auto" ]; then
  if [ "$CLUSTER" = 1 ] || [ "$HAVE_DISPLAY" = 0 ]; then MODE="cli"; else MODE="gui"; fi
fi
ok "Linux $(uname -m), $(getconf _NPROCESSORS_ONLN 2>/dev/null || echo '?') cores$( [ "$CLUSTER" = 1 ] && echo ', HPC scheduler detected')"
ok "build: $( [ "$MODE" = gui ] && echo 'desktop application + command line' || echo 'command line (headless)')"
mkdir -p "$PREFIX"
PREFIX="$(cd "$PREFIX" && pwd)"
ok "install folder: $PREFIX"
command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1 || [ -n "$TOOLCHAIN" ] || die "curl or wget is needed to download the toolchain"
fetch() { if command -v curl >/dev/null 2>&1; then curl -fsSL --retry 3 -o "$2" "$1"; else wget -q -O "$2" "$1"; fi; }

# ---- 1. private toolchain ---------------------------------------------------------------------------------------
ENV="${TOOLCHAIN:-$PREFIX/toolchain/env}"
if [ -z "$TOOLCHAIN" ]; then
  step "Private toolchain (conda-forge, user space)"
  MM="$PREFIX/toolchain/bin/micromamba"
  if [ ! -x "$MM" ]; then
    note "downloading micromamba"
    mkdir -p "$PREFIX/toolchain/bin"
    tmp="$(mktemp -d)"
    fetch "https://micro.mamba.pm/api/micromamba/$PLAT/latest" "$tmp/mm.tar.bz2"
    tar -xjf "$tmp/mm.tar.bz2" -C "$tmp" bin/micromamba
    mv "$tmp/bin/micromamba" "$MM"
    rm -rf "$tmp"
  fi
  export MAMBA_ROOT_PREFIX="$PREFIX/toolchain/root"
  PKGS="c-compiler cxx-compiler cmake>=3.24 ninja make pkg-config eigen nlohmann_json doctest suitesparse kokkos openblas"
  [ "$MODE" = gui ] && PKGS="$PKGS qt6-main vtk"
  if [ ! -d "$ENV/conda-meta" ]; then
    note "installing compilers and libraries (a few minutes, once)"
    # shellcheck disable=SC2086
    "$MM" create -y -q -p "$ENV" -c conda-forge $PKGS >/dev/null || die "the toolchain could not be installed from conda-forge"
  else
    # shellcheck disable=SC2086
    "$MM" install -y -q -p "$ENV" -c conda-forge $PKGS >/dev/null || die "the toolchain could not be updated"
  fi
  ok "toolchain at $ENV"
fi
export PATH="$ENV/bin:$PATH"
CC_BIN="$(ls "$ENV"/bin/*-gcc 2>/dev/null | head -n1 || true)"
CXX_BIN="$(ls "$ENV"/bin/*-g++ 2>/dev/null | head -n1 || true)"
[ -n "$CXX_BIN" ] || CXX_BIN="$(command -v g++ || true)"
[ -n "$CC_BIN" ] || CC_BIN="$(command -v gcc || true)"
[ -n "$CXX_BIN" ] || die "no C++ compiler in the toolchain"

# ---- 2. build and test ------------------------------------------------------------------------------------------
step "Building ABSS"
BUILD="$PREFIX/build"
GUI_FLAG=ON; [ "$MODE" = cli ] && GUI_FLAG=OFF
configure() {
  cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$ENV" \
        -DCMAKE_C_COMPILER="$CC_BIN" -DCMAKE_CXX_COMPILER="$CXX_BIN" -DABSS_BUILD_GUI="$1" \
        -DCMAKE_INSTALL_RPATH="$ENV/lib" -DCMAKE_BUILD_RPATH="$ENV/lib" >"$PREFIX/build-configure.log" 2>&1
}
if ! configure "$GUI_FLAG"; then
  if [ "$GUI_FLAG" = ON ]; then
    note "the desktop application cannot be configured here (see $PREFIX/build-configure.log); building the command line only"
    MODE="cli"; GUI_FLAG=OFF
    rm -rf "$BUILD"
    configure OFF || die "configuration failed, see $PREFIX/build-configure.log"
  else
    die "configuration failed, see $PREFIX/build-configure.log"
  fi
fi
cmake --build "$BUILD" -j "$JOBS" >"$PREFIX/build.log" 2>&1 || { tail -n 30 "$PREFIX/build.log" >&2; die "build failed, see $PREFIX/build.log"; }
ok "built"
if [ "$RUN_TESTS" = 1 ]; then
  step "Running the test suite"
  if ctest --test-dir "$BUILD" -LE "abaqus|gui" --output-on-failure >"$PREFIX/tests.log" 2>&1; then
    ok "$(grep -E 'tests passed' "$PREFIX/tests.log" | tail -n1)"
  else
    tail -n 30 "$PREFIX/tests.log" >&2
    die "tests failed, see $PREFIX/tests.log"
  fi
fi

# ---- 3. Abaqus ----------------------------------------------------------------------------------------------------
step "Looking for Abaqus"
if [ -z "$ABAQUS_CMD" ]; then
  for c in abaqus abq2025 abq2024 abq2023 abq2022; do
    if command -v "$c" >/dev/null 2>&1; then ABAQUS_CMD="$(command -v "$c")"; break; fi
  done
fi
ABAQUS_MODULE=""
if [ -z "$ABAQUS_CMD" ] && type module >/dev/null 2>&1; then
  ABAQUS_MODULE="$( (module -t avail abaqus 2>&1 || true) | grep -i '^abaqus' | sort -V | tail -n1 )"
fi
if [ -z "$ABAQUS_CMD" ]; then
  for c in /opt/SIMULIA/Commands/abaqus /usr/SIMULIA/Commands/abaqus /usr/local/bin/abaqus; do
    [ -x "$c" ] && { ABAQUS_CMD="$c"; break; }
  done
fi
if [ -n "$ABAQUS_CMD" ]; then ok "Abaqus: $ABAQUS_CMD"
elif [ -n "$ABAQUS_MODULE" ]; then ok "Abaqus module: $ABAQUS_MODULE (loaded automatically by the abss launchers)"
else note "Abaqus not found now; ABSS finds it later through ABSS_ABAQUS_CMD, PATH or 'module load abaqus'. The demos work without it."
fi

# ---- 4. install ---------------------------------------------------------------------------------------------------
step "Installing"
mkdir -p "$PREFIX/bin" "$PREFIX/libexec" "$PREFIX/share/abss" "$PREFIX/modulefiles/abss" "$PREFIX/etc"
for exe in abss abss-cli abss_elfhost; do
  [ -x "$BUILD/bin/$exe" ] && cp -f "$BUILD/bin/$exe" "$PREFIX/libexec/"
done
cat >"$PREFIX/etc/abss.env" <<EOF
# Written by the ABSS installer. Sourced by the abss / abss-cli launchers.
export LD_LIBRARY_PATH="$ENV/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$ENV/plugins\${QT_PLUGIN_PATH:+:\$QT_PLUGIN_PATH}"
${ABAQUS_CMD:+export ABSS_ABAQUS_CMD="\${ABSS_ABAQUS_CMD:-$ABAQUS_CMD}"}
ABSS_ABAQUS_MODULE="$ABAQUS_MODULE"
EOF
for exe in abss abss-cli; do
  [ -x "$PREFIX/libexec/$exe" ] || continue
  cat >"$PREFIX/bin/$exe" <<EOF
#!/usr/bin/env bash
# ABSS launcher (generated by the installer)
. "$PREFIX/etc/abss.env"
if [ -z "\${ABSS_ABAQUS_CMD:-}" ] && ! command -v abaqus >/dev/null 2>&1 && [ -n "\$ABSS_ABAQUS_MODULE" ] && type module >/dev/null 2>&1; then
  module load "\$ABSS_ABAQUS_MODULE" >/dev/null 2>&1 || true
fi
exec "$PREFIX/libexec/$exe" "\$@"
EOF
  chmod +x "$PREFIX/bin/$exe"
done
cp -r "$SRC/examples" "$PREFIX/share/abss/"
mkdir -p "$PREFIX/share/abss/docs"
cp "$SRC"/docs/*.md "$PREFIX/share/abss/docs/" 2>/dev/null || true
cp "$SRC/README.md" "$PREFIX/share/abss/" 2>/dev/null || true
[ -f "$SRC/packaging/WALKTHROUGH.html" ] && cp "$SRC/packaging/WALKTHROUGH.html" "$PREFIX/share/abss/"
VERSION="$(sed -n 's/^project(ABSS VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")"
# Environment modules (Lmod and Tcl)
cat >"$PREFIX/modulefiles/abss/$VERSION.lua" <<EOF
help([[Abaqus Black-Box Sensitivity Studio $VERSION]])
whatis("Name: ABSS")
whatis("Version: $VERSION")
prepend_path("PATH", "$PREFIX/bin")
setenv("ABSS_HOME", "$PREFIX")
EOF
cat >"$PREFIX/modulefiles/abss/$VERSION" <<EOF
#%Module1.0
module-whatis "Abaqus Black-Box Sensitivity Studio $VERSION"
prepend-path PATH $PREFIX/bin
setenv ABSS_HOME $PREFIX
EOF
# Slurm job template
mkdir -p "$PREFIX/share/abss/hpc"
cat >"$PREFIX/share/abss/hpc/abss_slurm.sh" <<'EOF'
#!/usr/bin/env bash
# ABSS sensitivity run on a Slurm cluster. Copy, edit the lines marked EDIT, then: sbatch abss_slurm.sh
#SBATCH --job-name=abss
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=16          # EDIT: cores for ABSS + concurrent Abaqus jobs
#SBATCH --time=12:00:00             # EDIT
#SBATCH --output=abss-%j.log
module load abaqus 2>/dev/null || true          # EDIT: your site's Abaqus module
MODEL=model.inp                                  # EDIT
PARAMS="mat:STEEL:elastic:E"                     # EDIT: ids from `abss-cli inspect $MODEL`
JOBS=$(( SLURM_CPUS_PER_TASK / 2 ))             # concurrent Abaqus jobs (licence permitting)
abss-cli run "$MODEL" --params "$PARAMS" --order 1 --jobs "$JOBS" --validate \
         --scratch "${TMPDIR:-/tmp}/abss_$SLURM_JOB_ID" --out "abss_$SLURM_JOB_ID.abss"
EOF
chmod +x "$PREFIX/share/abss/hpc/abss_slurm.sh"
# Desktop entry
if [ "$MODE" = gui ] && [ -x "$PREFIX/bin/abss" ]; then
  apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
  mkdir -p "$apps"
  cat >"$apps/abss.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=ABSS
Comment=Abaqus Black-Box Sensitivity Studio
Exec=$PREFIX/bin/abss %f
Terminal=false
Categories=Science;Engineering;
EOF
fi
if [ "$SHELL_RC" = 1 ] && [ -f "$HOME/.bashrc" ] && ! grep -q "ABSS installer" "$HOME/.bashrc"; then
  printf '\n# ABSS installer\nexport PATH="%s/bin:$PATH"\n' "$PREFIX" >>"$HOME/.bashrc"
  note "added $PREFIX/bin to PATH in ~/.bashrc"
fi
ok "installed"

step "Done"
"$PREFIX/bin/abss-cli" abaqus-info 2>/dev/null | sed 's/^/   /' || true
cat <<EOF

   Command line:   $PREFIX/bin/abss-cli inspect model.inp
$( [ -x "$PREFIX/bin/abss" ] && echo "   Desktop app:    $PREFIX/bin/abss" )
   Examples:       $PREFIX/share/abss/examples
   HPC:            module use $PREFIX/modulefiles && module load abss
                   Slurm template: $PREFIX/share/abss/hpc/abss_slurm.sh
   Open a new shell (or: export PATH="$PREFIX/bin:\$PATH") to use the commands.
EOF
