# Orb Shatter

Native orb mod for **Dungeon Siege: Legends of Aranna** (retail 1.50.0.0056).

Orb spells (vanilla ones and orbs from other mods) can also attack barrels, crates, cracks and breakable walls. The orb keeps its own shot, damage, rhythm and effects. Living enemies always come first.

- Compatible with other orb mods.
- No `.dsres`, no Skrit or GAS override.
- Save-safe

Download the ready-to-use package from the Nexus Mods page ([Shatter Orb - Nexus](https://www.nexusmods.com/dungeonsiege1/mods/326)).

## Installation

1. Copy `OrbShatter.dsdll` into the Dungeon Siege game folder (next to `DungeonSiege.exe`).
2. Launch the game. The mod creates `Documents\Dungeon Siege LOA\OrbShatter.ini`.

To uninstall, delete the DSDLL, and `OrbShatter.ini` and `OrbShatter.log` from `Documents\Dungeon Siege LOA`.

## Configuration

`Documents\Dungeon Siege LOA\OrbShatter.ini` is created with defaults on the first start and read at game start. The real Documents folder is used, including OneDrive redirection. An `OrbShatter.ini` placed next to the DSDLL takes priority (portable setup).

| Key | Default | Effect |
|---|---|---|
| `[General] Enabled` | `true` | `false`: the mod does nothing |
| `[Targeting] AttackContainers` | `true` | barrels, crates and other containers |
| `[Targeting] AttackNonContainerDestructibles` | `false` | cracks, breakable walls and doors |
| `[Diagnostics] Log` | `true` | short `OrbShatter.log` next to the INI |

If both targeting options are `false`, the game behaves as vanilla. A missing file or an invalid value falls back to the default.

Target priority: living enemies in line of sight, then other destructibles, then containers.

## How it works

The orb script (`spell_orb.skrit`) asks the engine for visible enemies with `GoMind::GetVisible`, then keeps the closest valid one. The mod does not change the script. It patches the function pointers the script engine uses (`GoMind::GetVisible`, `Go::HasMind`, `Go::GetMind`) and hooks `AIQuery::Is`:

1. While an orb queries enemies, `AIQuery::Is` lets admissible destructibles pass the check. The mod remembers them.
2. After the engine query, the mod removes them again if a real enemy is in line of sight (`GoMind::IsLosClear`). Otherwise it keeps the other destructibles first, then the containers.
3. `HasMind` and `GetMind` answer for those remembered objects only, for at most 400 ms.

All patches live in memory and are restored when the game closes. The mod checks the executable (PE timestamp and image size) at start and stays off on any other build.

## Building

Requirements: Visual Studio 2022 with the x86 C++ toolchain.

```
build.cmd
```

The output is `out\OrbShatter.dsdll`. Warnings are treated as errors. Set `VCVARS` to your `vcvarsall.bat` path if Visual Studio is installed elsewhere.

## Repository layout

| Path | Content |
|---|---|
| `src/OrbShatter.cpp` | the whole mod |
| `config/OrbShatter.ini` | copy of the INI the mod creates |
| `build.cmd` | command-line build |
| `docs/PROOFS.md` | what was tested in the retail game, and the limits |
| `docs/evidence/` | logs from the retail sessions |
| `tools/` | helper to check that a save holds no mod data |

## Known limits

- An enemy that is visible and in line of sight but cannot be attacked for another reason can still hide a destructible. Two orb checks (attackable flag and start-up state) are not replicated.
- The mod assumes the game thread is the only caller of the patched functions.

See [docs/PROOFS.md](docs/PROOFS.md) for the full test matrix.

## License

Orb Shatter is released under the MIT License (see [LICENSE](LICENSE)). You are free to use, modify and incorporate its source code into your own projects, provided that the original copyright and license notices are preserved.

Credit to the original Orb Shatter project in your mod description is appreciated.

## AI disclosure

This mod was created with the help of AI. The code was tested in the retail game by the author. The test results and the limits are in [docs/PROOFS.md](docs/PROOFS.md).
