# Borderlands 4 Trainer

An in-game trainer for **Borderlands 4** (single player), with its own overlay panel. Press **Insert** in game to open
it (controller: **LB + D-pad Down**).

> Made for the Steam version **1.11.0-4906557**. On another game version the trainer may need an update; the trainer
> report (Settings > Support) says whether your game build is the one it was made for.

## Features

| Tab | What it does |
| --- | --- |
| Player | Presets on hotkeys (God run, Loot farm, Speed demon, Fly through walls), God mode, Invisible to enemies, no damage, overshield, level, health / shield |
| Combat | Infinite ammo and magazine, instant reload (with a reload speed slider), fire rate and damage, kill or freeze enemies around you |
| Skills | Action skill without cooldown or with fast refill, endless duration, skill points, specialization tokens, SDU tokens, respec (applies at once) |
| Movement | Speed, infinite jump, fly and fly through walls, saved positions, fast travel to any of 353 places (searchable list) |
| Loot | Spawn items and any legendary by name (searchable list), drops at your level, loot vacuum, item and storage tools |
| Builder / Gear Maker | Build weapons and gear part by part: maker, type, rarity, level, parts, perks and elements; read and edit perk numbers |
| Items | Item serial tools for your backpack, bank and equipped items |
| Difficulty | Difficulty presets, enemy health and numbers, bosses, rewards |
| Enemies | Spawn enemies, change enemies near you (rank, team, size, weapon), spawn swaps |
| World | Time of day, HUD markers, map waypoint teleport |
| Places | Teleport and travel to every station and landmark, sorted by kind and region |
| Camera | Matrix look (green screen effect + code rain), bullet time, free camera, third person over the shoulder, field of view, HUD on / off |
| Missions | Mission tools |
| Money | Cash, eridium, gain multipliers, loot luck, Vault Card tokens and level (cards active on your character) |
| Save | Progress tools and a save editor that applies when the game starts (your save is backed up first) |
| Settings | Panel hotkey, panel size and look, crash logs, trainer report |

Options you switch on are remembered and switched on again when you are back in the game.

## Using it

1. Download the release zip.
2. In Steam right-click Borderlands 4 > **Manage > Browse local files** and open `OakGame\Binaries\Win64` (the folder
   with `Borderlands4.exe`).
3. Copy `dsound.dll` **and** the `BL4Trainer` folder from the zip into it.
4. Start the game from Steam as usual. The game loads the trainer by itself - there is nothing else to run. When it is
   ready the game shows "Trainer ready - press INSERT to open it".
5. In game press **Insert** to open or close the trainer.

**Back up your saves first** - level, skill point, item and save editor changes end up in your save because the game
saves them itself. Saves: `Documents\My Games\Borderlands 4\Saved\SaveGames`. Single player only.

No game file is changed. To remove it, close the game and delete `dsound.dll` and the `BL4Trainer` folder from
`Win64`. To play without it for a while, rename `dsound.dll` to `dsound.dll.off`.

**Other mods:** `dsound.dll` also loads your other mods before the game starts, the way ASI loaders do: `*.asi` in
`Win64`, `Win64\scripts` and `Win64\plugins`, and `*.dll` in `Win64\mods` (alphabetical order). If `Win64` already
has a `dsound.dll` from another mod, rename it to `dsound_chain.dll` before copying the trainer's: both are loaded and
sound calls go through it. Pak mods are not affected. `BL4Trainer\loaded_mods.log` lists what was loaded.

**Updating from 1.0.x:** the trainer has no exe any more. Delete the old `BL4Trainer` folder; to keep your hotkey and
switched-on options, copy `settings.json` and `data\` from it into the new `BL4Trainer` folder in `Win64`.

## Problems and crash reports

- **No "Trainer ready" message and no `trainer.log`**: the game did not load the trainer. Check that `dsound.dll` and
  the `BL4Trainer` folder are both directly in `OakGame\Binaries\Win64` (not in a subfolder and not in the game's top
  folder) and that antivirus did not remove `dsound.dll`.
- **"Trainer ready" but nothing opens**: open an issue with `trainer.log`, how you start the game and your game version.
- The trainer writes `trainer.log` next to itself and keeps the last 20 sessions in `logs\`.
- If the game closes unexpectedly, that session's log is kept in `logs\crashes\crashes_###.log` (never deleted).
- **Settings > Support > Create trainer report** makes one text file (also copied to the clipboard) that checks the
  trainer, lists errors and crashes and says whether the problem is the trainer or something else. Your user name, PC
  name and folders are removed from it. Attach it when you open an issue.

## Building from source

Requirements: Windows 10/11 x64, Visual Studio 2022 (or newer) with the *Desktop development with C++* workload, and
Python 3 for packaging.

```bat
build.bat
python package.py 1.1.1
```

`build.bat` finds Visual Studio with `vswhere`, builds MinHook and Dear ImGui once, then the trainer (`build\native.dll`)
and the drop-in loader (`build\dsound.dll`). `package.py` puts `native.dll` next to the contents of `data\` in
`release\package\BL4Trainer\`, adds `dsound.dll` and `INSTALL.txt`, zips it as `release\BL4Trainer_1.1.1.zip` - the
same layout as the release download - and prints SHA-256 checksums.

## What the trainer does (and does not do)

- `dsound.dll` (`proxy/dsound_proxy.cpp`) is a drop-in loader, the same well-known way other mod loaders work: the game
  imports `dsound.dll`, and Windows looks for it in the game's own folder first. It passes every DirectSound call on,
  unchanged, to Windows' own `dsound.dll` in System32 (or to `dsound_chain.dll` if you kept another mod's), and starts
  one thread that loads `BL4Trainer\native.dll` - only inside `Borderlands4.exe`. Like ASI loaders, it points the
  game's entry for one call at its own code, loads the mod files you put in the folders above, puts the entry back and
  lets the game start. There is no exe: no other program starts the game, opens its process or writes into it.
- The DLL (`src/`) draws its panel through DirectX 12 hooks (MinHook) and works with the game's own engine objects and
  functions in memory (`src/ue.cpp`, `src/bl4.cpp`, `src/items.cpp`).
- No network code: the DLL opens no sockets and talks to nothing outside the game.
- No game file is changed (it only adds `dsound.dll` and its folder). Edits to your character end up in your save
  because the game saves them.
- Files it writes, all inside its own folder: `trainer.log`, `loaded_mods.log`, `logs\`, `settings.json`, `data\` (switched-on options,
  edited values, positions), `save_backups\` and `serials.txt` (when you export item serials).
- Never touches premium currency, SHiFT-managed items (golden keys) or paid content: Vault Card tools only work on
  cards that are active on your character.

## License

Source-available, **not** open source: you may read it and build it for your own use, but not redistribute it or builds
of it. See [LICENSE](LICENSE). Official download: https://github.com/RipXic/Borderlands4Trainer/releases

## Layout

| Path | Contents |
| --- | --- |
| `src/` | The trainer DLL: overlay and panel (`overlay.cpp`, `dx12_hook.cpp`, `panel.cpp`, `skin.cpp`, `ui_kit.h`), engine access (`ue.cpp`), the game features (`bl4.cpp`, `items.cpp`, `savefile.cpp`, `serial.cpp`), logs and the trainer report (`core.cpp`, `report.cpp`) |
| `src/trinity/` | Controller input (XInput / DualSense), ported from Trinity (MIT) |
| `proxy/` | `dsound.dll`: the drop-in loader the game loads by itself; passes sound calls on and loads the trainer |
| `res/` | Version information |
| `install.txt`, `data_readme.txt` | `INSTALL.txt` and `BL4Trainer\README.txt` in the release zip |
| `data/` | Files shipped next to the DLL: the panel layout, item / enemy / place catalogs, legendary recipes, the panel skin |
| `third_party/` | [Dear ImGui](https://github.com/ocornut/imgui) 1.91.9b, [MinHook](https://github.com/TsudaKageyu/minhook), [nlohmann/json](https://github.com/nlohmann/json) - each under its own license |

## Not affiliated

Borderlands 4 Trainer is a fan-made tool. It is not affiliated with, endorsed by or connected to Gearbox Software or
2K. Borderlands and its content belong to their owners.
