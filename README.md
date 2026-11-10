# GZPro-Registry

A single-file mod for **Godzilla Destroy All Monsters Melee Remastered** that lets **any number of costume mods work
side by side**. Drop `version.dll` into the game once. After that, adding or removing costume mods just works.

It is a port of [MMS Registry](https://github.com/agentbenom00/mms-registry) (Marvel's Midnight Suns): the same launch
hook, pak reader/writer and report window. The asset registry code is rewritten for this game's
Unreal Engine 5.8 format.

## Why

The game finds its costumes (and every other "primary asset") only through one premade file,
`GZPro/AssetRegistry.bin`, which it reads from the highest-priority pak that has one. A mod that adds a new
costume has to ship a patched copy of that whole file. So two costume mods conflict: only one copy wins, and the
other mod's costume disappears. After a game update, a mod's old copy also hides any new official costumes.

GZPro-Registry builds one shared registry at every launch. It starts from the game's own registry and adds every
installed mod's entries. The result goes into `Content/Paks/GZPro-Registry/GZPro-Registry_99_P.pak`, which loads
after every normal mod pak.

## Install (players)

Needs the Steam version of the game, on Windows or on Linux / Steam Deck through Proton. Tested with the game's
`patch_day1` build (CL 28881, Unreal Engine 5.8.1).

1. Download `GZPro-Registry.zip` from [Releases](../../releases), or build it yourself (see Building).
2. Drag its `GZPro` folder into the game folder and let it merge. This puts `version.dll` in
   `<game>\GZPro\Binaries\Win64\`, next to `GZProSteam-Win64-Shipping.exe`.
   (Steam: right-click the game > Manage > Browse local files.)
3. Install costume mods as usual: with the GZPro Mod Manager, or by hand into `<game>\GZPro\Content\Paks\` or a
   subfolder of it. Then start the game.

With the GZPro Mod Manager instead of step 2: add the zip as a mod and confirm its DLL prompt. The manager names mods
after their `.pak`, and this one has none, so it lists it under a random id: click the name to rename it.

Linux (Proton): also set the game's Steam launch options to `WINEDLLOVERRIDES="version=n,b" %command%`.
Without it, Proton uses Wine's own version.dll and this one never runs.

**The window.** "GZPro-Registry" opens for about 4 seconds at every launch by default (`show_always=1`). It lists
what the mods add, pak by pak. When something changed (a mod was added, removed or updated, or the game was updated),
it rebuilds the registry first; that takes a few seconds. The first rebuild downloads the Oodle decompression library
once, because the game's pak is Oodle-compressed (see Credits). Offline? Put `oodle-data-shared.dll` or
`oo2core_9_win64.dll` next to `version.dll` instead.

`%LOCALAPPDATA%\GZPro-Registry\options.txt` is created on the first run:

| Option | Meaning |
|---|---|
| `show_always` | 1 = the window on every launch; 0 = only when the registry is rebuilt |
| `hold_seconds` | how long the window stays after the report (default 4) |
| `font_percent` | text size (default 125) |
| `full_list` | 1 = every entry on screen, 0 = totals only |

Log: `%LOCALAPPDATA%\GZPro-Registry\GZPro-Registry.log`. On Linux that folder is inside the game's Proton prefix,
`steamapps/compatdata/4251460/pfx/drive_c/users/steamuser/AppData/Local/GZPro-Registry/`.

**Uninstall:** delete `version.dll` and the `Content\Paks\GZPro-Registry` folder. Removing it in the GZPro Mod Manager
deletes only `version.dll`: delete the folder too, or the game keeps using the last shared registry.

## Compatibility

- Only one mod can be the game's `version.dll`. A mod that ships its own `version.dll` conflicts with this one.
- Costume mods that ship their own `GZPro/AssetRegistry.bin` work as they are (see For mod authors).
- After a game update, the next launch rebuilds the shared registry from the updated game's own registry.

## For mod authors

Nothing to do for a costume mod that already ships its own `GZPro/AssetRegistry.bin`, as costume mods made without
this tool do. The mod keeps working on its own, for players without GZPro-Registry. With GZPro-Registry installed,
every entry of the mod's registry that:

- the game's registry lacks, and
- whose package is in the mod's own container (the `.utoc` next to its `.pak`)

is added to the shared registry. Entries whose package isn't in the mod are skipped. These are usually left over
from a registry built on top of another mod. When two mods register the same entry, the pak that loads first in the
game wins: higher pak order, so `Name_2_P.pak` beats `Name_P.pak`.

A mod can also leave out the 6 MB registry and ship a small manifest instead: `GZPro/RegistryMods/<id>.json` in its
`.pak` (the game ignores it). The format is the same as MMS Registry's:

```json
{"format": 1, "id": "KiryuPS4", "title": "Kiryu (PS4 ver.)",
 "sources": ["/Game/Characters/GZ1_Monsters/Mechagodzilla/DA_GZ1_Skin_Mechagodzilla74.DA_GZ1_Skin_Mechagodzilla74"],
 "rules": [["DA_GZ1_Skin_Mechagodzilla74", "DA_GZ1_Skin_KiryuPS4"],
           ["GZ1_Monsters/Mechagodzilla", "GZ1_Monsters/Kiryu"]],
 "keep": []}
```

- `sources`: object paths of game entries to copy, such as an official costume's data asset.
- `rules`: `[old, new]` text replacements, applied in order to every name in the copied entry (package, object,
  package path, name and export-path tag values) so the copy points at your package. Names containing a `keep`
  string stay unchanged; strings and texts are kept as they are.
- A source can also be `{"source": path, "rules": [...]}` with its own rules, to copy one donor more than once.
- `id` must be unique. A source that isn't in the game's registry is skipped with a warning.

With the manifest above, the shared registry gets exactly the entry the Kiryu costume's own registry has. The tests
check this byte for byte: [`examples/KiryuPS4.json`](examples/KiryuPS4.json).

## How it works

- `GZProSteam-Win64-Shipping.exe` imports `version.dll`, and Windows looks next to the exe first. All of the game's
  imports are PE forwarders to `kernelbase.dll`, so nothing of the game runs through this DLL (as in MMS Registry).
- In `DllMain` (game exe only; `GZPRO_REGISTRY_SKIP=1` turns it off) a quick check compares the mod paks with the
  record in the registry pak. That record is `GZPro/GZPro-Registry/state.json`: the game paks plus every mod pak with a
  registry or manifest, by name, size and date. Only pak indexes are read; it takes a fraction of a second.
- If they differ, it copies itself to `%LOCALAPPDATA%\GZPro-Registry\GZPro-RegistrySync.dll` and runs
  `rundll32 GZPro-RegistrySync.dll,GZProRegistrySync --from-game <Paks>`, waiting for it to finish. That process
  rebuilds the registry and the game then starts. If the Paks folder isn't writable, the process asks for
  administrator rights.
- The rebuild:
  - reads `GZPro/AssetRegistry.bin` from `GZPro-Windows.pak` (pak version 12, Oodle);
  - adds the manifests' copies, then the mods' own registry entries, the highest-priority pak first;
  - checks the result by parsing it again;
  - writes `GZPro/AssetRegistry.bin`, `GZPro/GZPro-Registry/state.json` and `added.json` into the registry pak.
  The pak is pak version 11, uncompressed.
- Registry pak priority: UE gives base order 4 to every pak whose path starts with `Paks/GZPro-` (a prefix test, so
  the `GZPro-Registry` folder counts) and 3 to other paks under Content. The `_99_P` suffix adds 100 × (99 + 1), which
  makes 10004. A normal mod `GZPro-Name_P.pak` has 104.
- The game mounts a `.pak` only when a `.utoc`/`.ucas` pair with the same name is next to it. The registry pak gets an
  empty pair: one chunk, a container header with no packages. The bytes are in `src/container.h`.
- With no registry mods left, the registry pak is deleted and the game uses its own registry again.
- Pre-release builds kept their registry pak in `Paks\GZProRegistry` (`GZPro-ModRegistry_99_P`) or
  `Paks\DAMM-Registry` (`DAMM-Registry_99_P`). If one of them is still there, the sync deletes its `.pak`, `.utoc` and
  `.ucas` (and the folder, if nothing else is in it).

The registry format: `src/registry.c` (version 24, the fixed-tag store with 16-byte aligned sections, 4-part export
paths). An unchanged registry is written back byte for byte.

## Building

`./build.sh` cross-compiles `version.dll` with [zig](https://ziglang.org) (`zig cc -target x86_64-windows-gnu`) and
writes `dist/GZPro-Registry.zip`. `./update.sh` (Linux) runs the sync through the game's Proton without starting the
game.

Tests: `tests/run_tests.sh` runs 72 checks headless under Wine: the registry code on its own, 12 sync scenarios and
5 runs of a stand-in game exe. See [tests/README.md](tests/README.md).

## Credits

- Derived from [MMS Registry](https://github.com/agentbenom00/mms-registry) (MIT). The launch hook, pak
  reader/writer, JSON, zlib, Oodle loader, console and options are its code, renamed for this game.
- Oodle is downloaded at runtime from [WorkingRobot/OodleUE](https://github.com/WorkingRobot/OodleUE) builds, as
  [FModel](https://github.com/4sval/FModel) and [CUE4Parse](https://github.com/FabianFG/CUE4Parse) do. It is not part
  of this project.
- Pak, IoStore and asset registry formats as documented by CUE4Parse and [repak](https://github.com/trumank/repak).
- Made with Claude Code (Claude Opus 5.5).
- Not affiliated with or endorsed by the game's developers or publishers, or by Toho.

## Changelog

- 1.0.0 (2026-11-10): first release.

## License

[MIT](LICENSE)
