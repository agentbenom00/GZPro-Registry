GZPRO-REGISTRY 1.0.0
====================

For Godzilla Destroy All Monsters Melee Remastered. Lets any number of costume mods work side by side.

The game finds its costumes only through one file, its asset registry (GZPro/AssetRegistry.bin). So every costume
mod has to ship its own copy of that file, and only one copy can win: with two costume mods installed, one of the
costumes disappears. This mod is a single file, version.dll. Every time the game starts, it builds one shared
registry from the game's own registry plus every installed costume mod
(Content\Paks\GZPro-Registry\GZPro-Registry_99_P.pak). Add or remove a costume mod, press Play: done.


INSTALL
-------
Drag the "GZPro" folder from this zip into the game folder (the one with the Engine and GZPro folders) and let it
merge. That puts version.dll in   <game>\GZPro\Binaries\Win64\   next to GZProSteam-Win64-Shipping.exe.

  Steam: right-click the game > Manage > Browse local files

Linux (Proton) only: also set the game's Steam launch options to
    WINEDLLOVERRIDES="version=n,b" %command%
(right-click the game > Properties > General > Launch options). Without it, Proton never loads this version.dll.

Install costume mods as usual: with the GZPro Mod Manager, or by hand in Content\Paks or any subfolder of it.

Uninstall: delete version.dll and the Content\Paks\GZPro-Registry folder.


WHAT TO EXPECT
--------------
- Every launch it checks the mod paks first.
- By default a window "GZPro-Registry" opens on every launch for about 4 seconds and lists what the mods add, pak by
  pak (see options.txt below). When something changed (a mod added/removed/updated, or a game update) it rebuilds the
  shared registry first, then the game starts.
- The very first rebuild downloads the Oodle decompression library once (needed to read the game's files). Offline?
  Put oodle-data-shared.dll or oo2core_9_win64.dll next to version.dll instead.
- If the game is in a protected folder, Windows asks for permission to update the registry pak.
- If something is wrong (e.g. a broken mod) a message says so, and the game still starts.
- Costume mods need nothing special: a mod that ships its own registry keeps working with or without this mod.
- Another mod that also uses version.dll can't be used at the same time.
- Log: %LOCALAPPDATA%\GZPro-Registry\GZPro-Registry.log
  (Linux: inside the game's Proton prefix,
   steamapps/compatdata/4251460/pfx/drive_c/users/steamuser/AppData/Local/GZPro-Registry/)

options.txt (in %LOCALAPPDATA%\GZPro-Registry, created on the first run; delete it to get the defaults back):
  show_always=1    1 = the window on every launch (adds about hold_seconds to the start), 0 = only when the registry is rebuilt
  hold_seconds=4   how long the window stays after the report
  font_percent=125 text size in percent of the console's own (100 = unchanged)
  full_list=1      1 = every added entry on the screen, 0 = only the totals (the log file always has everything)


FOR MOD AUTHORS
---------------
Nothing to change: if your costume mod ships its own GZPro/AssetRegistry.bin, every entry in it that the game's
registry lacks, and whose package is in your mod's own .utoc, is added to the shared registry. If two mods register
the same entry, the copy from the pak that loads first in the game is kept.

Or leave out the 6 MB registry and ship a small manifest at  GZPro/RegistryMods/<id>.json  in your .pak (the game
ignores it). It copies an official costume's entry and renames it to point at your files:

  {"format": 1, "id": "KiryuPS4", "title": "Kiryu (PS4 ver.)",
   "sources": ["/Game/Characters/GZ1_Monsters/Mechagodzilla/DA_GZ1_Skin_Mechagodzilla74.DA_GZ1_Skin_Mechagodzilla74"],
   "rules": [["DA_GZ1_Skin_Mechagodzilla74", "DA_GZ1_Skin_KiryuPS4"], ["GZ1_Monsters/Mechagodzilla", "GZ1_Monsters/Kiryu"]],
   "keep": []}

"rules" are [old, new] text replacements applied in order to every name in the copied entry; names containing a
"keep" string stay unchanged. "id" must be unique. Details: README.md on the project page.


CREDITS
-------
Derived from MMS Registry (Marvel's Midnight Suns) by agentbenom00, MIT license.
Oodle is downloaded at runtime from the WorkingRobot/OodleUE builds; it is not part of this mod.
Made with Claude Code. Not affiliated with or endorsed by the game's developers or publishers, or by Toho.
