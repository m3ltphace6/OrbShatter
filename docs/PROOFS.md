# Retail proofs and limits (v1.0)

Environment: Dungeon Siege LOA 1.50 retail, isolated test home, Orb of Fire already cast in the test save.

| Check | Result | Evidence |
|---|---|---|
| Barrel destroyed by Orb of Fire | Verified | confirmed in game; log `TARGET_ACCEPTED barrel_glb_fh_r1` |
| Other containers | Verified | log `TARGET_ACCEPTED` for `crate_glb_hc_r1`, `crate_glb_fh_r1`, `barrel_glb_hc_r1` |
| Breakable wall / crevice | Verified | log `TARGET_ACCEPTED wall_glb_bricked_door_breakable` (category other); crevices confirmed in game |
| Enemies before destructibles | Verified | log `PRIORITY ... kept=monsters` (6 events in one session) |
| Other destructibles before containers (no enemy) | Not isolated | the rule is in the code; no log line shows it alone |
| `AttackContainers=false` | Verified | barrels intact in screenshot, barrel seen in log but never accepted |
| Default release INI | Verified | log `enabled=1 containers=1 non_container_destructibles=0`, `TARGET_ACCEPTED barrel_glb_fh_r1` |
| `Enabled=false` | Verified (one 40 s run) | log `DISABLED by INI: no engine change`, no hook installed, no `TARGET_ACCEPTED`; barrel intact in screenshot |
| `AttackNonContainerDestructibles=false` | Verified (one session) | log `containers=1 non_container_destructibles=0`; crate and barrel accepted; user reported the crack was not attacked while a crate/barrel was |
| Original cadence, projectile, damage | By design | the original orb script runs unchanged; only the candidate list changes |
| Speed cost | Verified | about 0.2 ms per 5 s window (`PERF` log lines) |
| Coexists with Ikkyo DSDLLs | Verified | ikkyo_loa + ikkyoplus_loa loaded together, long session |
| Save contents | Verified | a Quick-Save written during a mod session (DSDLL attached, 13:09) was unpacked and scanned: no `OrbContainer` text anywhere. The save's manifest lists only `ikkyo_loa.dsdll`, `ikkyoplus_loa.dsdll` and the test fixture; our DSDLL is not listed. Tool: `tools/scan_save_for_mod.py` |
| Reload after uninstall | Verified (one run) | that same save was loaded with the DSDLL removed: scene and party appear, no crash, no hang, no mod log (`docs/evidence/reload_without_dsdll.png`). The save file hash was identical before and after |
| Firstie's Orbs (Nexus 302) | **Not proven in retail** | Static reading only: its two orbs (Swirling Midges, Whelnar's) specialize `base_spell_orb_good/dark` and run the same `Orb_spell_orbiting` state and `[spell_orb]` block as Orb of Fire, so they reach the same `GoMind::GetVisible` call the mod hooks. A retail attempt (Orb of Fire template overridden with the Midge orb data) was inconclusive: the orb already active in the save kept its fire effect, and a scripted click on the spell icon did not cast. |
| Refreshing Orbs (Nexus 322, Longer Duration) | **Not proven in retail** | Static reading only: the .dsres replaces `spell_orb.skrit`. Its `FireSingleShot` function (the `GetVisible` call, line-of-sight and attackable checks, shot) is identical to vanilla. Only duration (flat 3600 s) and orb replacement (new orb replaces the old one) differ. The mod hooks the engine functions, not the script, so it should apply. Not run in the game. |

## Limits
- Works only with the retail `DSLOA.exe` it was built for (checked at start); otherwise it stays off.
- An enemy that is visible and in line of sight but not attackable for another reason can still hide a destructible. Two orb checks (attackable flag, start-up state) are not replicated.
- The test home is not fully isolated: the game also writes `DungeonSiege.ini`, `prefs.gas` and `Auto-Save.dsasave` in `Documents\Dungeon Siege LOA`.
- Code audit (not a retail test) of the Go::HasMind/Go::GetMind substitution: it only answers for objects the mod itself put in its list during the last orb GetVisible. The list holds at most 16 objects, clears at each orb query and entries expire after 400 ms. Inside that window any other engine code that asks GetMind about such a barrel would get the caster's mind. No effect was seen in the retail sessions, but it is not excluded.
- The game thread is assumed to be the only caller: the mod's lists are not locked.
