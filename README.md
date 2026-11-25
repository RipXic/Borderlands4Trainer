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

1. Download the release zip and unzip the `BL4Trainer` folder anywhere (keep its files together).
2. Run `BL4Trainer.exe`. If the game is not running it starts it through Steam and attaches by itself; if it is running
   it attaches right away.
3. In game press **Insert** to open or close the trainer.

**Back up your saves first** - level, skill point, item and save editor changes end up in your save because the game
saves them itself. Saves: `Documents\My Games\Borderlands 4\Saved\SaveGames`. Single player only.

Nothing in the game folder is changed. To remove it, close the game and delete the folder.

## Problems and crash reports

- **The trainer does not attach** ("Windows refused access to Borderlands4.exe"): if Steam or the game runs as
  administrator, `BL4Trainer.exe` offers to restart itself as administrator - click Yes. If it still fails, antivirus
  or security software is probably blocking it: add the trainer folder as an exception. `loader.log` next to
  `BL4Trainer.exe` records each attach step with the Windows error codes - attach it when you open an issue.
- **It attached but nothing opens**: the game shows "Trainer ready - press INSERT" when the panel works. The game restarts
  itself after a game patch: leave the `BL4Trainer.exe` window open until the main menu so it can attach again. Still
  nothing: open an issue with `loader.log` and `trainer.log`, how you start the game and your game version.
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
python package.py 1.0.2
```

`build.bat` finds Visual Studio with `vswhere`, builds MinHook and Dear ImGui once, then the trainer (`build\native.dll`
and `build\BL4Trainer.exe`). `package.py` puts the binaries next to the contents of `data\` in `release\BL4Trainer\`,
zips it as `release\BL4Trainer_1.0.2.zip` - the same layout as the release download - and prints SHA-256 checksums.

## What the trainer does (and does not do)

- `BL4Trainer.exe` (`loader/loader.cpp`) starts the game through Steam if needed and loads `native.dll` into
  `Borderlands4.exe` with `LoadLibraryW`. It copies the DLL into a `loaded\` folder next to itself first, so the original
  file is never locked.
- The DLL (`src/`) draws its panel through DirectX 12 hooks (MinHook) and works with the game's own engine objects and
  functions in memory (`src/ue.cpp`, `src/bl4.cpp`, `src/items.cpp`).
- No network code: the DLL opens no sockets and talks to nothing outside the game.
- Nothing in the game folder is changed. Edits to your character end up in your save because the game saves them.
- Files it writes, all inside its own folder: `loader.log`, `trainer.log`, `logs\`, `settings.json`, `data\` (switched-on options,
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
| `loader/` | `BL4Trainer.exe`: starts the game if needed and attaches the DLL |
| `res/` | Version information |
| `data/` | Files shipped next to the DLL: the panel layout, item / enemy / place catalogs, legendary recipes, the panel skin |
| `third_party/` | [Dear ImGui](https://github.com/ocornut/imgui) 1.91.9b, [MinHook](https://github.com/TsudaKageyu/minhook), [nlohmann/json](https://github.com/nlohmann/json) - each under its own license |

## Not affiliated

Borderlands 4 Trainer is a fan-made tool. It is not affiliated with, endorsed by or connected to Gearbox Software or
2K. Borderlands and its content belong to their owners.
