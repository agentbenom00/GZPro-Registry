#!/usr/bin/env bash
# Linux helper: sync the registry pak now, without starting the game (the game's version.dll does this by itself at
# every launch, once Steam's launch options have WINEDLLOVERRIDES="version=n,b" %command%). Runs the installed
# version.dll's sync through the game's own Proton, in the game's own prefix.
#   ./update.sh            update if needed
#   ./update.sh --force    rebuild anyway (removes the registry pak first)
set -euo pipefail
steam="${STEAM_DIR:-$HOME/.steam/debian-installation}"
[ -d "$steam/steamapps" ] || steam="$HOME/.local/share/Steam"
game="$steam/steamapps/common/Godzilla Destroy All Monsters Melee Remastered"
paks="$game/GZPro/Content/Paks"
dll="$game/GZPro/Binaries/Win64/version.dll"
prefix="$steam/steamapps/compatdata/4251460"
# the Proton the game was last run with (compatdata/4251460/version), else GE-Proton / Proton Experimental
want="$(cat "$prefix/version" 2>/dev/null || true)"
proton=""
for p in "$steam/compatibilitytools.d/$want/proton" "$steam/steamapps/common/$want/proton"; do
  [ -n "$want" ] && [ -f "$p" ] && { proton="$p"; break; }
done
[ -n "$proton" ] || proton="$(ls -d "$steam"/compatibilitytools.d/GE-Proton*/proton "$steam/steamapps/common/Proton - Experimental/proton" 2>/dev/null | tail -1 || true)"
[ -n "$proton" ] || { echo "no Proton found under $steam"; exit 1; }
data="$prefix/pfx/drive_c/users/steamuser/AppData/Local/GZPro-Registry"
[ -f "$dll" ] || { echo "version.dll is not installed in $game/GZPro/Binaries/Win64"; exit 1; }
grep -aq GZPro-RegistryProxy "$dll" || { echo "$dll is not GZPro-Registry's version.dll"; exit 1; }
if pgrep -f "GZProSteam-Win64-Shipping\.exe" >/dev/null; then
  echo "The game is running: close it first"; exit 2
fi
[ "${1:-}" = "--force" ] && rm -f "$paks/GZPro-Registry/GZPro-Registry_99_P.pak"
mkdir -p "$data"
cp "$dll" "$data/GZPro-RegistrySync.dll"
: > "$data/GZPro-Registry.log"
STEAM_COMPAT_CLIENT_INSTALL_PATH="$steam" STEAM_COMPAT_DATA_PATH="$prefix" \
  "$proton" run rundll32.exe 'C:\users\steamuser\AppData\Local\GZPro-Registry\GZPro-RegistrySync.dll',GZProRegistrySync \
  --quiet "Z:$paks" >/dev/null 2>&1 || true
tail -n +2 "$data/GZPro-Registry.log"
! grep -q "^error" "$data/GZPro-Registry.log"
