#!/usr/bin/env bash
# GZPro-Registry tests, headless under Wine: no window, and the game is never started. The game's own pak and the
# Kiryu costume are only read (the test Paks folders link to them or copy them); everything the sync writes goes into
# the work folder.
#   tests/run_tests.sh [work folder]     default: ${TMPDIR:-/tmp}/gzpro-registry-tests
#   VERBOSE=1 tests/run_tests.sh         also print every sync log
# Needs: ./build.sh first, wine (wine64), zig, repak, unzip, python3, the game installed, the Kiryu costume's zip
# (the newest zip in the Kiryu project's dist/; the costume is the *KiryuPS4_P.utoc/.ucas/.pak in it) and the Kiryu
# project's tools/ (assetregistry.py, iostore.py). The costume is copied into the work folder first, as
# GZPro-KiryuPS4_P: what is installed in the game is never used, so a mod manager can add or remove mods while the
# tests run.
# Settings: GAME_PAKS, KIRYU_ZIP, KIRYU_PAKS (instead of the zip: a folder with the costume's .pak/.utoc/.ucas),
# KIRYU_TOOLS, PYTHON, REPAK, WINE64, ZIG, WINEPREFIX (default: a prefix in the work folder; the first sync downloads
# Oodle into it).
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
W="${1:-${TMPDIR:-/tmp}/gzpro-registry-tests}"
case "$W" in /*) ;; *) W="$PWD/$W" ;; esac
GAME_PAKS="${GAME_PAKS:-$HOME/.steam/debian-installation/steamapps/common/Godzilla Destroy All Monsters Melee Remastered/GZPro/Content/Paks}"
KIRYU_PAKS="${KIRYU_PAKS:-}"
KIRYU_ZIP="${KIRYU_ZIP:-$(ls -t "$here"/../Kiryu/dist/*.zip 2>/dev/null | head -1)}"
KIRYU_TOOLS="${KIRYU_TOOLS:-$here/../Kiryu/tools}"
if [ -z "${PYTHON:-}" ]; then
  PYTHON=python3; [ -x "$KIRYU_TOOLS/../.venv/bin/python" ] && PYTHON="$KIRYU_TOOLS/../.venv/bin/python"
fi
REPAK="${REPAK:-$(command -v repak || ls -d "$HOME"/Tools/repak/*/repak 2>/dev/null | tail -1)}"
WINE64="${WINE64:-/usr/lib/wine/wine64}"
ZIG="${ZIG:-$(ls -d "$HOME"/Tools/zig-linux-x86_64-*/zig 2>/dev/null | tail -1)}"
DLL="$here/dist/GZPro-Registry/GZPro/Binaries/Win64/version.dll"
export REPAK WINEPREFIX="${WINEPREFIX:-$W/prefix}" WINEDEBUG=-all

die() { echo "$*"; exit 2; }
[ -f "$DLL" ] || die "run ./build.sh first"
[ -f "$GAME_PAKS/GZPro-Windows.pak" ] || die "the game's pak is not in $GAME_PAKS (set GAME_PAKS)"
# the costume's container: GZPro-KiryuPS4_P up to v2.1, TokyoSOS-KiryuPS4_P since v2.2
if [ -n "$KIRYU_PAKS" ]; then KIRYU_STEM="$(cd "$KIRYU_PAKS" 2>/dev/null && ls -- *KiryuPS4_P.utoc 2>/dev/null | head -1)"
elif [ -f "$KIRYU_ZIP" ]; then KIRYU_STEM="$(unzip -Z1 "$KIRYU_ZIP" 2>/dev/null | grep -E '^[^/]*KiryuPS4_P\.utoc$' | head -1)"
else KIRYU_STEM=""
fi
KIRYU_STEM="${KIRYU_STEM%.utoc}"
[ -n "$KIRYU_STEM" ] || die "no Kiryu costume (*KiryuPS4_P.utoc) in ${KIRYU_PAKS:-${KIRYU_ZIP:-../Kiryu/dist}} (set KIRYU_ZIP or KIRYU_PAKS)"
[ -f "$KIRYU_TOOLS/assetregistry.py" ] || die "the Kiryu project's tools are not in $KIRYU_TOOLS (set KIRYU_TOOLS)"
[ -x "$REPAK" ] || die "repak not found (set REPAK)"
[ -x "$WINE64" ] || die "wine64 not found (set WINE64)"
[ -x "$ZIG" ] || die "zig not found (set ZIG)"
if [ -e "$W" ] && [ ! -f "$W/.gzpro-registry-tests" ] && [ -n "$(ls -A "$W" 2>/dev/null)" ]; then
  die "$W is not empty and not a work folder of this script"
fi
mkdir -p "$W" && touch "$W/.gzpro-registry-tests"
for d in bin out paks paks2 paks3 fakegame tmp kiryu; do rm -rf "${W:?}/$d"; mkdir -p "$W/$d"; done
KIRYU_FROM="${KIRYU_PAKS:-$KIRYU_ZIP} ($KIRYU_STEM)"
for e in pak utoc ucas; do                       # copied in as GZPro-KiryuPS4_P, whatever the costume's files are called
  if [ -n "$KIRYU_PAKS" ]; then cp "$KIRYU_PAKS/$KIRYU_STEM.$e" "$W/kiryu/GZPro-KiryuPS4_P.$e"
  else unzip -p "$KIRYU_ZIP" "$KIRYU_STEM.$e" > "$W/kiryu/GZPro-KiryuPS4_P.$e"
  fi || die "could not copy $KIRYU_STEM.$e from $KIRYU_FROM"
  [ -s "$W/kiryu/GZPro-KiryuPS4_P.$e" ] || die "$KIRYU_STEM.$e is missing or empty in $KIRYU_FROM"
done
KIRYU_PAKS="$W/kiryu"

pass=0 fail=0 rc=0
ok()  { pass=$((pass + 1)); echo "  ok    $*"; }
bad() { fail=$((fail + 1)); echo "  FAIL  $*"; }
is()  { if eval "$1"; then ok "$2"; else bad "$2"; fi; }          # is '<shell test>' '<what it checks>'
has() { if tr -d '\r' < "$LOG" | grep -qE -- "$1"; then ok "log: $1"; else bad "log lacks: $1"; fi; }
hasnt() { if tr -d '\r' < "$LOG" | grep -qE -- "$1"; then bad "log has: $1"; else ok "log lacks: $1"; fi; }
z() { printf 'Z:%s' "$(printf '%s' "$1" | sed 's#/#\\#g')"; }
w64() { env -u DISPLAY -u WAYLAND_DISPLAY "$WINE64" "$@"; }
rt() { w64 "$W/bin/regtest.exe" "$@" 2>&1 | tr -d '\r'; }
same() { "$PYTHON" -I "$here/tests/compare_registries.py" "$KIRYU_TOOLS" "$1" "$2" | grep -q "same entries in the same order: True"; }
regfile() { "$REPAK" get "$1/GZPro-Registry/GZPro-Registry_99_P.pak" GZPro/AssetRegistry.bin > "$2" 2>/dev/null; }
sync() {  # sync <Paks folder> <title>
  echo "== $2"
  : > "$LOG"
  w64 'C:\windows\system32\rundll32.exe' "$(z "$W/bin/GZPro-RegistrySync.dll")",GZProRegistrySync --quiet "$(z "$1")" >/dev/null 2>&1
  rc=$?
  if [ -n "${VERBOSE:-}" ]; then sed 's/^/    | /' "$LOG"; fi
  is '[ $rc = 0 ]' "the sync exited with 0 (got $rc)"
}
pack() { "$REPAK" pack -q --version V11 --mount-point ../../../ "$1" "$2"; }    # pack <folder> <out .pak>

# ---------------------------------------------------------------- build the test programs, read the inputs
echo "work folder: $W"
echo "Kiryu costume: $KIRYU_FROM"
w64 cmd /c exit >/dev/null 2>&1                                    # creates the prefix on the first run
DATA="$WINEPREFIX/drive_c/users/$USER/AppData/Local/GZPro-Registry"
LOG="$DATA/GZPro-Registry.log"
mkdir -p "$DATA"
cp "$DLL" "$W/bin/GZPro-RegistrySync.dll"
(cd "$here/src" && "$ZIG" cc -target x86_64-windows-gnu -O1 -Wall -Wno-unused-function -o "$W/bin/regtest.exe" \
  ../tests/regtest.c util.c registry.c utoc.c) || die "regtest.exe did not build"
"$ZIG" cc -target x86_64-windows-gnu -O1 -o "$W/bin/host.exe" "$here/tests/host.c" -lversion || die "host.exe did not build"
"$REPAK" get "$GAME_PAKS/GZPro-Windows.pak" GZPro/AssetRegistry.bin > "$W/out/base.bin" || die "repak could not read the game's pak"
"$REPAK" get "$KIRYU_PAKS/GZPro-KiryuPS4_P.pak" GZPro/AssetRegistry.bin > "$W/out/kiryu.bin" || die "repak could not read the Kiryu pak"
MG74=/Game/Characters/GZ1_Monsters/Mechagodzilla/DA_GZ1_Skin_Mechagodzilla74.DA_GZ1_Skin_Mechagodzilla74
KIRYU_PKG=/Game/Characters/GZ1_Monsters/Kiryu/DA_GZ1_Skin_KiryuPS4

# ---------------------------------------------------------------- registry.c / utoc.c on their own
echo "== registry.c: round trip, clone, copy; utoc.c: package ids"
out=$(rt roundtrip "$(z "$W/out/base.bin")")
is 'grep -q "roundtrip: identical" <<<"$out"' "the game's registry is written back byte for byte"
out=$(rt roundtrip "$(z "$W/out/kiryu.bin")")
is 'grep -q "roundtrip: identical" <<<"$out"' "the Kiryu mod's registry is written back byte for byte"
out=$(rt clone "$(z "$W/out/base.bin")" "$MG74" "$(z "$W/out/clone.bin")" \
  DA_GZ1_Skin_Mechagodzilla74 DA_GZ1_Skin_KiryuPS4 GZ1_Monsters/Mechagodzilla GZ1_Monsters/Kiryu)
is 'cmp -s "$W/out/clone.bin" "$W/out/kiryu.bin"' "a manifest-style copy of Mechagodzilla74 == the Kiryu mod's registry, byte for byte"
is 'grep -q "clone again: 1" <<<"$out"' "the same copy a second time is refused (already registered)"
out=$(rt copy "$(z "$W/out/base.bin")" "$(z "$W/out/kiryu.bin")" "$(z "$W/out/copy.bin")" "$(z "$KIRYU_PAKS/GZPro-KiryuPS4_P.utoc")")
is 'grep -qE "copy: 1 added, [0-9]+ already there, 0 skipped" <<<"$out"' "the Kiryu mod's registry adds exactly 1 entry"
is 'same "$W/out/copy.bin" "$W/out/kiryu.bin"' "the game's registry + that entry == the Kiryu mod's registry, entry for entry"
want=$("$PYTHON" -I -c 'import sys; sys.path.insert(0, sys.argv[1]); import iostore; print("%016x" % iostore.package_id(sys.argv[2]))' \
  "$KIRYU_TOOLS" "$KIRYU_PKG")
got=$(rt pkgid "$KIRYU_PKG")
is '[ "$got" = "$want" ]' "package id of the Kiryu costume = $want (as in the Python IoStore writer)"
out=$(rt container "$(z "$W/out/empty.utoc")" "$(z "$W/out/empty.ucas")")
is '[ "$(stat -c %s "$W/out/empty.utoc")" = 202 ] && [ "$(stat -c %s "$W/out/empty.ucas")" = 64 ]' "the empty container is 202 + 64 bytes"

# ---------------------------------------------------------------- the sync: fake mods next to the game's pak
P="$W/paks"
"$PYTHON" -I "$here/tests/make_fake_mods.py" "$KIRYU_TOOLS" "$W/out/base.bin" "$P" >/dev/null || die "make_fake_mods.py failed"
ln -s "$GAME_PAKS/GZPro-Windows.pak" "$P/"
for e in pak utoc ucas; do ln -s "$KIRYU_PAKS/GZPro-KiryuPS4_P.$e" "$P/"; done

sync "$P" "1. first build: 3 mods with their own registry (one is a duplicate), 1 manifest mod, 1 mod without entries"
has "Found 4 mod paks with registry entries \(3 with their own registry, 1 with manifests\)"
has "Reason: the registry pak does not exist yet"
has "\+ costume +DA_GZ1_Skin_TestC +<- DA_GZ1_Skin_Mechagodzilla74_Black"
has "\+ costume +DA_GZ1_Skin_TestC2 +<- DA_GZ1_Skin_Mechagodzilla74$"
has "DA_GZ1_Skin_Nobody is not in the game's registry - skipped"
has "Pak GZPro-Dup_1_P.pak \(its own registry"
has "DA_GZ1_Skin_TestB.DA_GZ1_Skin_TestB is registered by another mod already - kept that one"
has "DA_GZ1_Skin_Ghost: its package is not in this mod - skipped"
has "\+ costume +DA_GZ1_Skin_KiryuPS4"
has "4 added: 4 costumes, 0 other"
hasnt "GZPro-Music_P"
is '[ -f "$P/GZPro-Registry/GZPro-Registry_99_P.pak" ] && cmp -s "$P/GZPro-Registry/GZPro-Registry_99_P.utoc" "$W/out/empty.utoc" && cmp -s "$P/GZPro-Registry/GZPro-Registry_99_P.ucas" "$W/out/empty.ucas"' \
  "the registry pak and its empty .utoc/.ucas are written"
regfile "$P" "$W/out/merged.bin"
out=$(rt find "$(z "$W/out/merged.bin")" /Game/Characters/GZ1_Monsters/Godzilla2K/DA_GZ1_Skin_TestB.DA_GZ1_Skin_TestB)
is 'grep -q "^found" <<<"$out"' "the registry in the pak parses and has the new entries"

sync "$P" "2. again, nothing changed"
has "Registry is up to date - nothing to do"
has "Registered by the mods: 4 costumes"
hasnt "Building the shared registry"

touch "$P/GZPro-TestCostume_P.pak"
sync "$P" "3. a mod pak changed"
has "Reason: changed mod pak GZPro-TestCostume_P.pak"
has "4 added: 4 costumes, 0 other"

mkdir -p "$W/tmp/removed" && mv "$P"/GZPro-Dup_1_P.* "$W/tmp/removed/"
sync "$P" "4. the duplicate that won removed: TestB now comes from TestCostume"
has "Reason: removed mod pak GZPro-Dup_1_P.pak"
hasnt "registered by another mod already"
has "4 added: 4 costumes, 0 other"

mv "$P"/GZPro-TestCostume_P.* "$P"/GZPro-KiryuPS4_P.* "$P"/~mods/GZPro-ManifestMod_P.* "$W/tmp/removed/"
sync "$P" "5. every costume mod removed"
has "Removed the registry pak \(no mods need it any more\)"
is '[ ! -e "$P/GZPro-Registry" ]' "the registry folder is gone"

sync "$P" "6. again, no mods"
has "No mod with registry entries found"
has "Registry is up to date - nothing to do"

legacy() {  # legacy <Paks folder>: the registry paks pre-release builds left (as GZPro Registry and DAMM-Registry)
  local d s e
  for d in GZProRegistry/GZPro-ModRegistry_99_P DAMM-Registry/DAMM-Registry_99_P; do
    mkdir -p "$1/${d%/*}"
    cp "$P/GZPro-Music_P.pak" "$1/$d.pak"
    for e in utoc ucas; do cp "$W/out/empty.$e" "$1/$d.$e"; done
  done
}
legacy "$P"
sync "$P" "7. old registry paks of pre-release builds, no mods"
has "Deleted an old registry pak of this tool in Paks.GZProRegistry"
has "Deleted an old registry pak of this tool in Paks.DAMM-Registry"
has "Registry is up to date - nothing to do"
is '[ ! -e "$P/GZProRegistry" ] && [ ! -e "$P/DAMM-Registry" ] && [ ! -e "$P/GZPro-Registry" ]' \
  "both old folders are gone, and no registry pak was built"

# ---------------------------------------------------------------- the real setup, game updates, broken mods
P2="$W/paks2"
cp "$GAME_PAKS/GZPro-Windows.pak" "$P2/"                           # a copy: test 9 changes its date
for e in pak utoc ucas; do ln -s "$KIRYU_PAKS/GZPro-KiryuPS4_P.$e" "$P2/"; done

sync "$P2" "8. the game + the Kiryu costume"
has "1 added: 1 costume, 0 other"
regfile "$P2" "$W/out/user_setup.bin"
is 'same "$W/out/user_setup.bin" "$W/out/kiryu.bin"' "the shared registry == the Kiryu mod's own registry, entry for entry"

touch "$P2/GZPro-Windows.pak"
sync "$P2" "9. a game update (the game's pak changed)"
has "Reason: the game files changed"

T="$W/tmp/broken"; mkdir -p "$T/GZPro"; yes "not an asset registry" | head -c 4000 > "$T/GZPro/AssetRegistry.bin"
pack "$T" "$P2/GZPro-Broken_P.pak" && cp "$P/GZPro-Music_P.utoc" "$P2/GZPro-Broken_P.utoc"
T="$W/tmp/badmanifest"; mkdir -p "$T/GZPro/RegistryMods"; echo '{"id": "Bad", "sources": [' > "$T/GZPro/RegistryMods/Bad.json"
pack "$T" "$P2/GZPro-BadManifest_P.pak"
sync "$P2" "10. a broken registry, a manifest that isn't JSON, a pak without .utoc"
has "manifest Bad.json skipped: GZPro-BadManifest_P.pak: its manifest is not valid JSON"
has "GZPro-Broken_P.pak: its registry was skipped: not an asset registry of this game"
has "GZPro-BadManifest_P.pak .*no .utoc next to it"
has "1 added: 1 costume, 0 other"
rm -f "$P2"/GZPro-Broken_P.* "$P2"/GZPro-BadManifest_P.*

cp "$P/GZPro-Music_P.pak" "$P2/GZPro-Registry/GZPro-Registry_99_P.pak"
sync "$P2" "11. a registry pak this tool did not build"
has "Reason: the registry pak was not built by this tool"
has "1 added: 1 costume, 0 other"

P3="$W/paks3"
ln -s "$GAME_PAKS/GZPro-Windows.pak" "$P3/"
T="$W/tmp/example"; mkdir -p "$T/GZPro/RegistryMods"; cp "$here/examples/KiryuPS4.json" "$T/GZPro/RegistryMods/"
pack "$T" "$P3/GZPro-KiryuManifest_P.pak"
for e in utoc ucas; do ln -s "$KIRYU_PAKS/GZPro-KiryuPS4_P.$e" "$P3/GZPro-KiryuManifest_P.$e"; done
sync "$P3" "12. examples/KiryuPS4.json instead of the Kiryu mod's own registry"
has "\+ costume +DA_GZ1_Skin_KiryuPS4 +<- DA_GZ1_Skin_Mechagodzilla74$"
regfile "$P3" "$W/out/example.bin"
is 'cmp -s "$W/out/example.bin" "$W/out/kiryu.bin"' "the shared registry == the Kiryu mod's own registry, byte for byte"

# ---------------------------------------------------------------- in the "game": DllMain, quick check, forwarders
F="$W/fakegame/GZPro"; mkdir -p "$F/Binaries/Win64" "$F/Content"
cp "$W/bin/host.exe" "$F/Binaries/Win64/GZProSteam-Win64-Shipping.exe"
cp "$DLL" "$F/Binaries/Win64/version.dll"
ln -s "$P2" "$F/Content/Paks"
[ -f "$DATA/options.txt" ] && cp "$DATA/options.txt" "$W/tmp/options.txt.saved"
printf 'show_always=0\r\nhold_seconds=0\r\nfont_percent=125\r\nfull_list=1\r\n' > "$DATA/options.txt"
game() { (cd "$F/Binaries/Win64" && env -u DISPLAY -u WAYLAND_DISPLAY "$@" "$WINE64" ./GZProSteam-Win64-Shipping.exe 2>&1 | tr -d '\r' | grep "^host:"); }

echo "== 13. the game starts with no registry pak (WINEDLLOVERRIDES=version=n,b)"
rm -rf "$P2/GZPro-Registry"; : > "$LOG"
out=$(game WINEDLLOVERRIDES="version=n,b")
is 'grep -q "GZProRegistrySync export: yes" <<<"$out"' "the game loaded this version.dll"
is 'grep -q "GetFileVersionInfoW=1 VerQueryValueW=1" <<<"$out"' "the game's version.dll calls work (forwarders)"
has "Reason: the registry pak does not exist yet"
is '[ -f "$P2/GZPro-Registry/GZPro-Registry_99_P.pak" ]' "the registry pak was built before the game went on"

echo "== 14. the game starts again (nothing changed)"
: > "$LOG"
t0=$(date +%s%N); out=$(game WINEDLLOVERRIDES="version=n,b"); t1=$(date +%s%N)
is 'grep -q "GZProRegistrySync export: yes" <<<"$out" && [ ! -s "$LOG" ]' \
  "quick check only: no sync process, nothing logged (the stand-in game ran for $(( (t1 - t0) / 1000000 )) ms)"

echo "== 15. the game starts with the old registry paks of pre-release builds still there"
legacy "$P2"; echo "not a pak" > "$P2/GZProRegistry/notes.txt"; : > "$LOG"
out=$(game WINEDLLOVERRIDES="version=n,b")
has "Deleted an old registry pak of this tool in Paks.GZProRegistry"
has "Deleted an old registry pak of this tool in Paks.DAMM-Registry"
has "Registry is up to date - nothing to do"
is '[ -z "$(ls "$P2/GZProRegistry" | grep -v "^notes.txt$")" ] && [ ! -e "$P2/DAMM-Registry" ] && [ -f "$P2/GZPro-Registry/GZPro-Registry_99_P.pak" ]' \
  "the old registry paks are deleted (a file they do not know is kept), the new one stays"

echo "== 16. the game starts again (the old folder is still there, with only that file)"
: > "$LOG"
out=$(game WINEDLLOVERRIDES="version=n,b")
is 'grep -q "GZProRegistrySync export: yes" <<<"$out" && [ ! -s "$LOG" ]' "quick check only: no sync, nothing logged"
rm -rf "$P2/GZProRegistry"

echo "== 17. the game starts without the override"
rm -rf "$P2/GZPro-Registry"; : > "$LOG"
out=$(game WINEDLLOVERRIDES="")
is 'grep -q "GZProRegistrySync export: no" <<<"$out"' "Wine uses its own version.dll: this one is not loaded"
is '[ ! -e "$P2/GZPro-Registry" ] && [ ! -s "$LOG" ]' "nothing was built or logged"

if [ -f "$W/tmp/options.txt.saved" ]; then cp "$W/tmp/options.txt.saved" "$DATA/options.txt"; else rm -f "$DATA/options.txt"; fi
echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
