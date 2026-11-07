# Orb Container Targeting

Native orb mod for **Dungeon Siege: Legends of Aranna** (retail 1.50.0.0056).

Orb spells (Orb of Fire and orbs from other mods) can also attack barrels, crates, cracks and breakable walls. The orb keeps its own shot, damage, rhythm and effects. The mod only widens the list of things the orb may pick. Living enemies always come first.

- Compatible with other orb mods (Firstie's Orbs, Refreshing Orbs).
- No `.dsres`, no Skrit or GAS override.
- Save-safe: nothing is written to saves, and a save loads without the mod.

Download the ready-to-use package from the Nexus Mods page (link to add).

## Installation

1. Copy `OrbContainerTargeting.dsdll` and `OrbContainerTargeting.ini` into the Dungeon Siege game folder (next to `DungeonSiege.exe`).
2. Launch the game.

To uninstall, delete both files (and `OrbContainerTargeting.log` if present).

## Configuration

`OrbContainerTargeting.ini`, read from the DSDLL folder at game start.

| Key | Default | Effect |
|---|---|---|
| `[General] Enabled` | `true` | `false`: the mod does nothing |
| `[Targeting] AttackContainers` | `true` | barrels, crates and other containers |
| `[Targeting] AttackNonContainerDestructibles` | `false` | cracks, breakable walls and doors |
| `[Diagnostics] Log` | `true` | short log next to the DSDLL |

If both targeting options are `false`, the game behaves as vanilla.

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

The output is `out\OrbContainerTargeting.dsdll`. Warnings are treated as errors. Set `VCVARS` to your `vcvarsall.bat` path if Visual Studio is installed elsewhere.

## Repository layout

| Path | Content |
|---|---|
| `src/OrbContainerTargeting.cpp` | the whole mod |
| `config/OrbContainerTargeting.ini` | default configuration |
| `build.cmd` | command-line build |
| `docs/PROOFS.md` | what was tested in the retail game, and the limits |
| `docs/evidence/` | logs from the retail sessions |

## Known limits

- An enemy that is visible and in line of sight but cannot be attacked for another reason can still hide a destructible. Two orb checks (attackable flag and start-up state) are not replicated.
- The mod assumes the game thread is the only caller of the patched functions.
- Other DSDLLs that also change orb targeting were not tested.

See [docs/PROOFS.md](docs/PROOFS.md) for the full test matrix.
