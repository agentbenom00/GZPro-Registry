# Tests

`tests/run_tests.sh [work folder]` runs everything headless under Wine: no window, and the game is never started.
The game's own pak is only read: the test Paks folders link to it or copy it, and everything the sync writes goes
into the work folder. The Kiryu costume is copied into the work folder first. The tests never use what is installed
in the game, so a mod manager can add or remove mods while they run.

Needs `./build.sh` first, plus wine (`wine64`), zig, repak, unzip, python3, the game installed and the Kiryu costume's
zip (the newest zip in the Kiryu project's `dist/`). The costume is the `*KiryuPS4_P` container in it
(`TokyoSOS-KiryuPS4_P` since v2.2, `GZPro-KiryuPS4_P` before), copied in as `GZPro-KiryuPS4_P`. It also needs the Kiryu project's `tools/`
(`assetregistry.py` and `iostore.py`, the Python registry and IoStore code that the C code was checked against). Paths
can be set with `GAME_PAKS`, `KIRYU_ZIP`, `KIRYU_PAKS` (instead of the zip: a folder with the costume's
`.pak`/`.utoc`/`.ucas`), `KIRYU_TOOLS`, `PYTHON`, `REPAK`, `WINE64`, `ZIG` and `WINEPREFIX`. `VERBOSE=1` prints every
sync log.

| Part | What it checks |
|---|---|
| registry.c | The game's and the Kiryu mod's registries are written back byte for byte. A manifest-style copy of Mechagodzilla74 is byte-identical to the Kiryu mod's registry. The Kiryu registry adds exactly one entry, and the result matches it entry for entry. Package ids match the Python IoStore writer. |
| 1–7 | Fake mods (`make_fake_mods.py`) next to the game's pak: a first build, a duplicate (the higher pak order wins), an entry whose package isn't in the mod, a manifest with a missing source, a pak without entries, no changes, a changed mod, a removed mod, all mods removed (the registry pak is deleted), the old registry paks of pre-release builds (`GZProRegistry`, `DAMM-Registry`) are deleted. |
| 8–11 | Game + Kiryu (the shared registry matches the Kiryu mod's own, entry for entry), a game update, a broken registry, a manifest that isn't JSON, a pak without `.utoc`, a registry pak this tool didn't build. |
| 12 | `examples/KiryuPS4.json` instead of the Kiryu mod's registry gives the Kiryu mod's registry byte for byte. |
| 13–17 | A stand-in game exe (`host.c`) that imports `version.dll` like the game: `DllMain` builds a missing registry before the game goes on, the forwarded calls work, an unchanged setup only runs the quick check, the old registry paks of pre-release builds are deleted at launch (a file they don't know stays, and after that only the quick check runs), and without `WINEDLLOVERRIDES` Wine never loads this DLL. |

`compare_registries.py <Kiryu tools> <a.bin> <b.bin>` compares two registries entry by entry (names, tag values,
bundles, chunks, flags).
