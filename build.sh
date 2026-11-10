#!/usr/bin/env bash
# Build the release: dist/GZPro-Registry.zip =
#   GZPro/Binaries/Win64/version.dll   (everything: launch hook + registry builder)
#   README.txt
#   INFO.txt                           (the description the GZPro Mod Manager shows; it is not installed)
# Needs zig (~/Tools/zig-*) to cross-compile the DLL.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
zig="${ZIG:-$(ls -d "$HOME"/Tools/zig-linux-x86_64-*/zig | tail -1)}"
out="$here/dist/GZPro-Registry"
rm -rf "$out" "$here/dist/GZPro-Registry.zip"
mkdir -p "$out/GZPro/Binaries/Win64"
(cd "$here/src" && "$zig" cc -target x86_64-windows-gnu -shared -O2 -s -Wall -Wno-unused-function \
  -Wl,--image-base=0x180000000 -o "$out/GZPro/Binaries/Win64/version.dll" \
  version.c util.c zlib.c pak.c oodle.c json.c registry.c utoc.c sync.c version.def)
rm -f "$out/GZPro/Binaries/Win64/version.lib" "$out/GZPro/Binaries/Win64/version.pdb"
for f in README.txt INFO.txt; do if [ -f "$here/$f" ]; then cp "$here/$f" "$out/"; fi; done
(cd "$out" && python3 -c 'import shutil; shutil.make_archive("../GZPro-Registry", "zip", ".")')
ls -la "$out/GZPro/Binaries/Win64/version.dll" "$here/dist/GZPro-Registry.zip"
