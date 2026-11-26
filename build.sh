
#!/usr/bin/env bash
set -Eeuo pipefail

# Apply the build number to the environment table
export HOMENX_BUILD=190

# Resolve the repository from this script's location.
REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

# Configuration — override any of these with environment variables.
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
# Use the alternate root only if it supplies both required media libraries.
fallback_devkitpro="$HOME/Work/devkita64-rootfs/opt/devkitpro"
if { [[ ! -f "$DEVKITPRO/portlibs/switch/include/jpeglib.h" ]] ||
     [[ ! -f "$DEVKITPRO/portlibs/switch/lib/libavformat.a" ]]; } &&
   [[ -f "$fallback_devkitpro/portlibs/switch/include/jpeglib.h" ]] &&
   [[ -f "$fallback_devkitpro/portlibs/switch/lib/libavformat.a" ]]; then
    export DEVKITPRO="$fallback_devkitpro"
fi
export PATH="$HOME/.local/bin:$PATH"

JOBS="${JOBS:-2}"
SD_ROOT="${SD_ROOT:-$HOME/Work/HomeNX-SD-root}"
ZIP_PATH="${ZIP_PATH:-$HOME/Work/HomeNX-SD-Ready.zip}"
RELEASE_DIR="$REPO_ROOT/build/cross/aarch64/release"

# Fail early instead of producing an incomplete package.
die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

require_file() {
    [[ -f "$1" ]] || die "Required file not found: $1"
}

require_dir() {
    [[ -d "$1" ]] || die "Required directory not found: $1"
}

step() {
    printf '\n\033[1;36m==> %s\033[0m\n' "$*"
}

# Verify the environment.
[[ -x "$DEVKITPRO" ]] || [[ -d "$DEVKITPRO" ]] ||
    die "DEVKITPRO directory does not exist: $DEVKITPRO"
command -v xmake >/dev/null || die "xmake is not installed"
command -v make >/dev/null || die "make is not installed"
command -v zip >/dev/null || die "zip is not installed"
command -v cmake >/dev/null || die "cmake is not installed"
command -v python3 >/dev/null || die "python3 is not installed"
[[ -f "$DEVKITPRO/portlibs/switch/lib/libavformat.a" ]] ||
    die "Missing libavformat.a under $DEVKITPRO/portlibs/switch/lib (switch-ffmpeg)"
[[ -f "$DEVKITPRO/portlibs/switch/include/jpeglib.h" ]] ||
    die "Missing jpeglib.h under $DEVKITPRO/portlibs/switch/include (switch-libjpeg-turbo)"

# Match the output layout used by the packaging steps below.
step "Configuring HomeNX"
xmake f --yes -p cross -a aarch64 -m release --toolchain=devkita64 \
    --homebrew=n --backend=deko3d -o build

# Build.
step "Building SwitchU daemon"

# Preserve the workaround from the original script.
rm -f "$RELEASE_DIR/switchu-daemon"
xmake b switchu-daemon

step "Building HomeNX applications"
xmake b -j"$JOBS" CardsNX SwitchU HomeNX-Loader Homebrew-Manager SwitchU-Manager homenx-video homenx-music

step "Building the achievements overlay (Tesla/Ultrahand .ovl)"
make -C "$REPO_ROOT/projects/achievements" -j"$JOBS"
require_file "$REPO_ROOT/projects/achievements/homenx-achievements.ovl"

# Install.
step "Installing to SD-card staging directory"
mkdir -p "$SD_ROOT"
xmake install -o "$SD_ROOT" --root

# Prepare package directories.
step "Assembling SD-card package"

BIN_DIR="$SD_ROOT/switch/SwitchU/bin"
mkdir -p "$BIN_DIR" \
    "$SD_ROOT/switch/SwitchU/cards" \
    "$SD_ROOT/switch/SwitchU/lockscreen"

require_file "$SD_ROOT/switch/.overlays/homenx-music.ovl"
require_file "$SD_ROOT/atmosphere/contents/4200000000484D59/exefs.nsp"

# Build marker used by the updater
mkdir -p "$SD_ROOT/switch/SwitchU"
printf 'build %s\n' "$HOMENX_BUILD" > "$SD_ROOT/switch/SwitchU/homenx-build.txt"

# Application renaming for the SD-card layout.
declare -A APPS=(
    ["HomeNX-Loader.nro"]="homenx-loader.nro"
    ["Homebrew Manager.nro"]="homebrew-manager.nro"
    ["CardsNX.nro"]="cardsnx.nro"
    ["NXToggle.nro"]="homenx-toggle.nro"
)

for source in "${!APPS[@]}"; do
    require_file "$RELEASE_DIR/$source"
    cp -f -- "$RELEASE_DIR/$source" "$BIN_DIR/${APPS[$source]}"
done

require_file "$REPO_ROOT/projects/achievements/homenx-achievements.ovl"
cp -f "$REPO_ROOT/projects/achievements/homenx-achievements.ovl" \
    "$BIN_DIR/homenx-achievements.ovl"

require_dir "$REPO_ROOT/build/cards-romfs"
cp -a "$REPO_ROOT/build/cards-romfs/." \
    "$SD_ROOT/switch/SwitchU/cards/"

# The lock screen's picture is the user's own (wallpaper.png/.jpg goes here).
# Ship any default one placed in romfs/lockscreen, but the folder may simply be
# absent — git keeps no empty directories and CI doesn't have it either.
if [ -d "$REPO_ROOT/romfs/lockscreen" ]; then
    cp -a "$REPO_ROOT/romfs/lockscreen/." \
        "$SD_ROOT/switch/SwitchU/lockscreen/"
fi

# Remove obsolete install locations from previous package layouts.
for old in \
    "switch/NXToggle" \
    "switch/HomeNX-Loader" \
    "switch/Homebrew Manager" \
    "switch/CardsNX" \
    "switch/SwitchU/sounds/sounds" \
    "switch/SwitchU/i18n/i18n"
do
    rm -rf -- "$SD_ROOT/$old"
done

# Generate installation instructions with the current date.
cat > "$SD_ROOT/INSTALL.txt" <<EOF
HomeNX build — compiled $(date +%F)

Copy the contents of this folder to the root of your Switch SD card
and merge folders.

Optional lock screen wallpaper: place wallpaper.png or wallpaper.jpg
in switch/SwitchU/lockscreen/.
EOF

# Package the complete SD-card directory.
step "Creating ZIP archive"
mkdir -p "$(dirname "$ZIP_PATH")"
rm -f -- "$ZIP_PATH"

(
    cd "$SD_ROOT"
    zip -qr "$ZIP_PATH" . -x 'atmosphere/contents/4200000000484D58' 'atmosphere/contents/4200000000484D58/*'
)

# Summary.
step "Build completed successfully"
printf 'Repository: %s\n' "$REPO_ROOT"
printf 'SD staging: %s\n' "$SD_ROOT"
printf 'ZIP archive: %s\n' "$ZIP_PATH"
printf 'ZIP size:    '
du -h "$ZIP_PATH" | cut -f1
printf 'SD files:    '
find "$SD_ROOT" -type f | wc -l
