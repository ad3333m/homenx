#!/usr/bin/env bash
# Bash counterpart to build.ps1, adapted from build.sh.
# xmake handles incremental compilation; no separate source fingerprint cache.
set -Eeuo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

VARIANT="${VARIANT:-sysmodule}"
MODE="${MODE:-release}"
BACKEND="${BACKEND:-deko3d}"
JOBS="${JOBS:-2}"
TARGET=""
INSTALL=false
NO_INSTALL=false
VERBOSE=false

usage() {
    cat <<'HELP'
Usage: ./build-ps1.sh [options]
  --variant sysmodule|homebrew  Default: sysmodule
  --mode release|debug         Default: release
  --backend deko3d|sdl2        Default: deko3d
  --target NAME               Build/install a single xmake target
  --jobs N                    Parallel jobs (0 lets xmake decide)
  --install                   Stage homebrew in dist/homebrew-<mode>
  --no-install                Skip sysmodule staging and ZIP packaging
  --verbose                   Show verbose build output
  --help                      Show this help

Builds the same default targets as build.ps1, without the achievements overlay.
Staged builds also produce dist/HomeNX-<variant>-<mode>.zip, ready to extract
onto the SD card root. Override the archive location with ZIP_PATH.
HOMENX_BUILD can be supplied through the environment; no build number is forced.
HELP
}

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
while (($#)); do
    case "$1" in
        --variant|--mode|--backend|--target|--jobs)
            (($# >= 2)) || die "Missing value for $1"
            case "$1" in
                --variant) VARIANT="$2" ;;
                --mode) MODE="$2" ;;
                --backend) BACKEND="$2" ;;
                --target) TARGET="$2" ;;
                --jobs) JOBS="$2" ;;
            esac
            shift 2 ;;
        --install) INSTALL=true; shift ;;
        --no-install) NO_INSTALL=true; shift ;;
        --verbose) VERBOSE=true; shift ;;
        --help|-h) usage; exit 0 ;;
        *) die "Unknown option: $1" ;;
    esac
done
[[ "$VARIANT" == sysmodule || "$VARIANT" == homebrew ]] || die "Invalid variant"
[[ "$MODE" == release || "$MODE" == debug ]] || die "Invalid mode"
[[ "$BACKEND" == deko3d || "$BACKEND" == sdl2 ]] || die "Invalid backend"
[[ "$JOBS" =~ ^[0-9]+$ ]] || die "Jobs must be a nonnegative integer"

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
if [[ ! -f "$DEVKITPRO/portlibs/switch/include/jpeglib.h" &&
      -f "$HOME/Work/devkita64-rootfs/opt/devkitpro/portlibs/switch/include/jpeglib.h" ]]; then
    export DEVKITPRO="$HOME/Work/devkita64-rootfs/opt/devkitpro"
fi
export PATH="$HOME/.local/bin:$PATH"
[[ -d "$DEVKITPRO/devkitA64" ]] || die "devkitA64 not found under $DEVKITPRO"
for tool in git xmake cmake make zip; do
    command -v "$tool" >/dev/null || die "$tool is not installed"
done

if [[ ! -e lib/espeak-ng/.git || ! -e lib/Atmosphere-libs/.git ]]; then
    git submodule update --init --recursive
fi

homebrew_flag=n
[[ "$VARIANT" != homebrew ]] || homebrew_flag=y
printf '==> Configuring %s (%s, %s)\n' "$VARIANT" "$MODE" "$BACKEND"
xmake f --yes -p cross -a aarch64 --toolchain=devkita64 \
    -m "$MODE" "--homebrew=$homebrew_flag" "--backend=$BACKEND" -o "build/$VARIANT"

build_args=(build -y)
[[ "$JOBS" == 0 ]] || build_args+=(-j "$JOBS")
[[ "$VERBOSE" == false ]] || build_args+=(-v)
[[ -z "$TARGET" ]] || build_args+=("$TARGET")
xmake "${build_args[@]}"

install_dir=""
if [[ "$VARIANT" == sysmodule && "$NO_INSTALL" == false ]]; then
    install_dir="$REPO_ROOT/sd_out"
elif [[ "$VARIANT" == homebrew && "$INSTALL" == true ]]; then
    install_dir="$REPO_ROOT/dist/homebrew-$MODE"
fi
if [[ -n "$install_dir" ]]; then
    # Package a fresh install so old files in sd_out/ cannot leak into the ZIP.
    stage_dir="$(mktemp -d "${TMPDIR:-/tmp}/homenx-sd.XXXXXXXX")"
    archive_dir=""
    cleanup() {
        rm -rf -- "$stage_dir"
        [[ -z "$archive_dir" ]] || rm -rf -- "$archive_dir"
    }
    trap cleanup EXIT
    install_args=(install -o "$stage_dir")
    [[ -z "$TARGET" ]] || install_args+=("$TARGET")
    xmake "${install_args[@]}"
    [[ -d "$stage_dir/switch" || -d "$stage_dir/atmosphere" ]] ||
        die "Install did not produce an SD card layout"

    cat > "$stage_dir/INSTALL.txt" <<EOF
HomeNX $VARIANT ($MODE) — built $(date +%F)

Extract the contents of this archive directly onto the Nintendo Switch SD card
root and merge the folders, replacing matching files. Do not copy an enclosing
folder: switch/ and atmosphere/ (when included) belong directly on the SD root.
EOF
    if [[ -n "$TARGET" ]]; then
        printf '\nPartial build: only target %s and its installed files are included.\n' "$TARGET" >> "$stage_dir/INSTALL.txt"
    fi
    mkdir -p -- "$install_dir"
    cp -a -- "$stage_dir/." "$install_dir/"

    archive_suffix=""
    [[ -z "$TARGET" ]] || archive_suffix="-$TARGET"
    zip_path="${ZIP_PATH:-$REPO_ROOT/dist/HomeNX-$VARIANT-$MODE$archive_suffix.zip}"
    [[ "$zip_path" == /* ]] || zip_path="$REPO_ROOT/$zip_path"
    mkdir -p -- "$(dirname -- "$zip_path")"
    archive_dir="$(mktemp -d "$(dirname -- "$zip_path")/.homenx-zip.XXXXXXXX")"
    # Build beside the destination and rename only after zip succeeds.
    (
        cd "$stage_dir"
        zip -qr "$archive_dir/package.zip" .
    )
    mv -f -- "$archive_dir/package.zip" "$zip_path"
    printf 'SD card layout: %s\nZIP archive: %s\n' "$install_dir" "$zip_path"
fi
printf 'Outputs: build/%s/cross/aarch64/%s\n' "$VARIANT" "$MODE"
