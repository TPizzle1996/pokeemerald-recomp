#!/usr/bin/env bash
set -euo pipefail

readonly script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
readonly repo_root=$(cd -- "$script_dir/.." && pwd)
readonly platform=${1:-}
readonly version=${2:-v0.1.0-alpha}
readonly source_date_epoch=${SOURCE_DATE_EPOCH:-$(git -C "$repo_root" log -1 --format=%ct)}
readonly release_dir="$repo_root/release"
export SOURCE_DATE_EPOCH="$source_date_epoch"

case "$version" in
    v[0-9]*-alpha) ;;
    *)
        printf 'Invalid alpha release version: %s\n' "$version" >&2
        exit 2
        ;;
esac

require_file() {
    if [[ ! -f "$1" ]]; then
        printf 'Required release input is missing: %s\n' "$1" >&2
        exit 1
    fi
}

copy_common_files() {
    local stage=$1
    local readme=$2

    install -d -m 0755 "$stage/images"
    install -m 0644 "$readme" "$stage/README.md"
    install -m 0644 "$repo_root/LICENSE" "$stage/LICENSE"
    install -m 0644 "$repo_root/images/rayquaza.png" "$stage/images/rayquaza.png"
}

audit_stage() {
    local stage=$1
    local path
    local relative
    local lower

    while IFS= read -r -d '' path; do
        relative=${path#"$stage"/}
        lower=${relative,,}
        case "$lower" in
            *.gba|*.sav|*.state|*.o|*.obj|*.log|core|core.*|*/core|*/core.*|\
            rom/*|*/rom/*|profiles/*|*/profiles/*|games/emerald/*|*/games/emerald/*|\
            quick.png|*/quick.png|slot*.png|*/slot*.png)
                printf 'Forbidden release path: %s\n' "$relative" >&2
                exit 1
                ;;
        esac
    done < <(find "$stage" -mindepth 1 -print0)
}

normalize_timestamps() {
    find "$1" -exec touch --date="@$source_date_epoch" {} +
}

package_linux() {
    local stage_parent="$repo_root/dist/linux-x64"
    local stage="$stage_parent/pokeemerald-recomp"
    local top="pokeemerald-recomp-${version}-linux-x64"
    local artifact="$release_dir/${top}.tar.gz"
    local temporary="${artifact}.tmp"

    require_file "$repo_root/pokeemerald-linux64"
    require_file "$repo_root/resources/linux64/README.txt"
    rm -rf -- "$stage"
    install -d -m 0755 "$stage"
    copy_common_files "$stage" "$repo_root/resources/linux64/README.txt"
    install -m 0755 "$repo_root/pokeemerald-linux64" "$stage/pokeemerald-recomp"
    strip --strip-debug "$stage/pokeemerald-recomp"
    audit_stage "$stage"
    normalize_timestamps "$stage"
    install -d -m 0755 "$release_dir"
    rm -f -- "$temporary"
    tar --sort=name --mtime="@$source_date_epoch" --owner=0 --group=0 \
        --numeric-owner --transform="s,^pokeemerald-recomp,${top}," \
        -C "$stage_parent" -cf - pokeemerald-recomp | gzip -n > "$temporary"
    mv -- "$temporary" "$artifact"
    printf '%s\n' "$artifact"
}

package_windows() {
    local stage_parent="$repo_root/dist/windows-x64"
    local stage="$stage_parent/pokeemerald-recomp"
    local archive_parent="$repo_root/dist/.windows-release"
    local top="pokeemerald-recomp-${version}-windows-x64"
    local artifact="$release_dir/${top}.zip"
    local temporary="${artifact}.tmp.zip"
    local sdl_root="$repo_root/build/windows64-deps/SDL2-2.32.10"
    local image_root="$repo_root/build/windows64-deps/SDL2_image-2.8.12"
    local windows_objcopy=${OBJCOPY:-x86_64-w64-mingw32-objcopy}

    require_file "$repo_root/PokemonEmeraldRecomp.exe"
    require_file "$repo_root/resources/windows64/README.txt"
    require_file "$sdl_root/x86_64-w64-mingw32/bin/SDL2.dll"
    require_file "$image_root/x86_64-w64-mingw32/bin/SDL2_image.dll"
    if ! command -v "$windows_objcopy" >/dev/null 2>&1; then
        windows_objcopy=objcopy
    fi

    rm -rf -- "$stage" "$archive_parent"
    install -d -m 0755 "$stage/licenses"
    copy_common_files "$stage" "$repo_root/resources/windows64/README.txt"
    install -m 0755 "$repo_root/PokemonEmeraldRecomp.exe" "$stage/pokeemerald-recomp.exe"
    "$windows_objcopy" --strip-debug "$stage/pokeemerald-recomp.exe"
    install -m 0644 "$sdl_root/x86_64-w64-mingw32/bin/SDL2.dll" "$stage/SDL2.dll"
    install -m 0644 "$image_root/x86_64-w64-mingw32/bin/SDL2_image.dll" "$stage/SDL2_image.dll"
    install -m 0644 "$sdl_root/LICENSE.txt" "$stage/licenses/SDL2.txt"
    install -m 0644 "$image_root/LICENSE.txt" "$stage/licenses/SDL2_image.txt"
    audit_stage "$stage"
    normalize_timestamps "$stage"

    install -d -m 0755 "$archive_parent/$top" "$release_dir"
    cp -a -- "$stage/." "$archive_parent/$top/"
    normalize_timestamps "$archive_parent"
    rm -f -- "$temporary"
    (
        cd -- "$archive_parent"
        find "$top" -print | LC_ALL=C sort | zip -X -q "$temporary" -@
    )
    mv -- "$temporary" "$artifact"
    rm -rf -- "$archive_parent"
    printf '%s\n' "$artifact"
}

require_file "$repo_root/README.md"
require_file "$repo_root/LICENSE"
require_file "$repo_root/images/rayquaza.png"

case "$platform" in
    linux) package_linux ;;
    windows) package_windows ;;
    *)
        printf 'Usage: %s {linux|windows} [version]\n' "$0" >&2
        exit 2
        ;;
esac
