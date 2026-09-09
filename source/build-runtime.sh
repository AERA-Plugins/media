#!/bin/bash
set -euo pipefail
source_dir=$(cd -- "$(dirname -- "$0")" && pwd)
output=${1:?Pass a staged runtime directory}
sysroot=${AERA_BROWSER_SYSROOT:-/tmp/aera-webkit-sysroot}
cc=${AERA_CXX:-/tmp/aera-webkit/clang++}
pkg_config=${AERA_PKG_CONFIG:-/tmp/aera-webkit/pkg-config}
strip=${AERA_STRIP:-/home/koaan/android/fox_14.1/prebuilts/clang/host/linux-x86/clang-r510928/bin/llvm-strip}
browser_root=${AERA_BROWSER_ROOT:-/tmp/aera-browser-v130-root}
read -r -a cflags <<< "$($pkg_config --cflags gstreamer-app-1.0 gio-unix-2.0)"
read -r -a libs <<< "$($pkg_config --libs gstreamer-app-1.0 gio-unix-2.0)"
target=(--target=aarch64-alpine-linux-musl --sysroot="$sysroot" --gcc-toolchain="$sysroot/usr")
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
"$cc" "${target[@]}" -std=c++17 -Os -g0 -Wall -Wextra -Werror \
  "${cflags[@]}" "$source_dir/media_worker.cpp" -c -o "$build/media_worker.o"
"$cc" "${target[@]}" --ld-path=/home/koaan/android/fox_14.1/prebuilts/clang/host/linux-x86/clang-r510928/bin/ld.lld \
  "$build/media_worker.o" "${libs[@]}" -Wl,-z,relro,-z,now,--gc-sections \
  -o "$build/aera-media"
"$strip" --strip-unneeded "$build/aera-media"
rm -rf "$output"
mkdir -p "$output/usr/bin" "$output/usr/lib/gstreamer-1.0" \
  "$output/usr/libexec/gstreamer-1.0" "$output/lib" "$output/etc" "$output/sdcard"
cp "$build/aera-media" "$output/usr/bin/"
cp -L "$sysroot/lib/ld-musl-aarch64.so.1" "$output/lib/"
ln -s ld-musl-aarch64.so.1 "$output/lib/libc.musl-aarch64.so.1"
plugins=(app audioconvert audioparsers audioresample coreelements fdkaac flac
  isomp4 matroska mpg123 openh264 opus playback typefindfunctions
  videoconvertscale videoparsersbad volume vpx wavparse)
for plugin in "${plugins[@]}"; do
  cp -L "$sysroot/usr/lib/gstreamer-1.0/libgst${plugin}.so" \
    "$output/usr/lib/gstreamer-1.0/"
done
cp -L "$browser_root/usr/lib/gstreamer-1.0/libgstaeraaudio.so" \
  "$output/usr/lib/gstreamer-1.0/"
cp -L "$sysroot/usr/libexec/gstreamer-1.0/gst-plugin-scanner" \
  "$output/usr/libexec/gstreamer-1.0/"

# Copy the complete transitive DT_NEEDED closure, using the SONAME requested by
# each object. This keeps the runtime small without relying on recovery libs.
changed=1
while ((changed)); do
  changed=0
  while IFS= read -r object; do
    while IFS= read -r dependency; do
      [[ -e "$output/usr/lib/$dependency" || -e "$output/lib/$dependency" ]] && continue
      candidate=$(find "$sysroot/usr/lib" "$sysroot/lib" -maxdepth 2 \
        -name "$dependency" -print -quit)
      [[ -n "$candidate" ]] || { echo "Missing runtime dependency: $dependency" >&2; exit 1; }
      cp -L "$candidate" "$output/usr/lib/$dependency"
      changed=1
    done < <(readelf -d "$object" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')
  done < <(find "$output/usr/bin" "$output/usr/lib" -type f)
done
"$strip" --strip-unneeded "$output/usr/lib/"*.so* \
  "$output/usr/lib/gstreamer-1.0/"*.so 2>/dev/null || true
touch "$output/sdcard/.keep"
echo "Staged AERA Media runtime: $(du -sh "$output" | cut -f1)"
