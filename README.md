# Legacy of Terror (offline)

An offline copy of Blizzard's Diablo 30th anniversary event (November 2026). Two parts:

| Part | Does |
|---|---|
| `d2rmm-mod/LegacyOfTerror` (D2RMM mod) | Terror Zones everywhere; the Hell Bovine Minion pet type |
| `legacy-plugin/build/d2rl-legacy-of-terror.dll` (D2RLoader plugin) | Diablo Clone every 6 hours, Cow Portal, Hell Bovine Minions, +100% Magic Find, 15% off at vendors |

## What you get

- **Terror Zones everywhere.** Every zone is terrorized at once, on each difficulty where you
  have killed Baal. Untick **Terrorized** in the game creation (gear) menu for a game without them.
- **Diablo Clone, Hell only.** At 02:00, 08:00, 14:00 and 20:00 UTC (midnight, 6 AM, noon and
  6 PM Pacific), a Hell game that is running gets "Diablo Walks the Earth". The next Super
  Unique you meet is Diablo Clone instead. As online, you have to be in the game when the time comes.
- **Cow Portal.** In the Rogue Encampment a red portal to the Cow Level opens beside the camp
  fire, once Baal is dead on that difficulty (`offset_x` / `offset_y` move it). No Wirt's Leg. Killing the Cow King does not lock you out.
- **Hell Bovine Minions.** A slain monster sometimes rises as a Hell Bovine that fights the
  monsters around it (it does not follow you) and falls after a while (3% and 60 s by default; 1-5% and 30-60 s in the settings). Only
  corpses a Necromancer could revive can rise. They stay out of the party frame.
- **Base 100% Magic Find** for every character (shown on the character screen, on top of your gear's MF, nothing saved).
- **15% off** buying, gambling and repairs (added to "Reduces all Vendor Prices"). Selling is unchanged.

## Install

1. Copy `d2rmm-mod/LegacyOfTerror` into `D2RMM 1.8.0/mods/`, tick it in D2RMM, press Install.
2. Copy `d2rl-legacy-of-terror.dll` into `Diablo II Resurrected/d2rloader/plugins/`.
3. Start the game through the loader.

Settings: `d2rloader/config/legacy-of-terror.toml` (or the I Hate TOMLs menu in game).
Console (Ctrl + `): `terror` shows the status, `terror clone` brings Diablo Clone now (Hell),
`terror cows` tries the Cow Portal again.

## Not yet seen in game

Checked against the game's code, not yet confirmed in play:
- Whether the terror zone schedule is happy with a single zone holding every level.
- Where the Cow Portal lands beside the camp fire.
- Hell Bovines with the Act 5 barbarians' AI (fighting nearby monsters without following you).
  The cow has no run animation, which that AI may ask for.
- Base Magic Find on the character screen.
- The "Diablo Walks the Earth" message offline.

## Build and test

Needs mingw-w64 (`x86_64-w64-mingw32-g++`), the [D2RLoader Plugin SDK](https://github.com/D2RLoader/PluginSDK)
headers, and for the tests Node and Wine.

```
SDK=/path/to/PluginSDK/include legacy-plugin/build.sh
SDK=/path/to/PluginSDK/include legacy-plugin/test/run.sh <vanilla data folder> <game image dump>
```

- `test/run_mod.js` runs the D2RMM mod script outside D2RMM, against files extracted from the game.
- `test/fake_host.cpp` loads the plugin DLL the way D2RLoader does, over a dump of the game image,
  and checks every engine function and patch site, the vendor discount, the base Magic Find and
  the Diablo Clone trigger.

The game addresses are for D2R 3.4 (image stamp 0x6ab3782c). On another build, the parts whose
code is not found switch themselves off and the log says which.

No Blizzard files are included: the mod builds everything from the installed game's own data.
