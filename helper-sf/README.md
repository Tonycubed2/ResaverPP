# ReSaverHelper for Starfield (beta)

An optional SFSE plugin for ReSaver Plus: it lets ReSaver Plus act in the running game, and gives you a rescue key for
a stuck game.

- **Quests (from the quest's details in ReSaver Plus):** read a quest's state from the game (running, completed,
  current stage, stages done), and the game's own console commands for it: `setstage`, `completequest`, `stopquest`,
  `resetquest`.
- **Get my controls back:** your controls are forced back on (the game's `ForceEnablePlayerControls`). **Give the
  controls back to the game** undoes it once the scene or mod that stuck them is over.
- **Save the game now:** a new save, `ReSaverPP_Rescue_<date>_<time>`. Your other saves are not touched.
- **Move to me (version 2, Find NPCs in ReSaver Plus):** a lost companion or any NPC is put next to you (the game's
  `moveto player`) and picks its AI package again (`evp`).
- **Give in the running game (version 2, the player's inventory in ReSaver Plus):** the item you picked is given to
  your character with the game's own `player.additem`, without editing the save.
- **Ask the AI** can use the same commands (`APPLY` additem / removeitem / addperk / setstage / completequest /
  objective / moveto, `MOVETO`, `GATHER`). Every form id is checked in the game first (it must exist and be the right
  kind: an item, a perk, a quest, an actor or object), so nothing but these fixed commands can run.
- **Hold Ctrl for 8 seconds in the game:** the save and the controls at once, even when ReSaver Plus is not running.

## Requirements

- Starfield (Steam) with **SFSE** (Starfield Script Extender) installed.
- **Address Library for SFSE Plugins** for your game version (`versionlib-1-16-244-0.bin` for 1.16.244) in
  `Data\SFSE\Plugins`.

## Install

Put `ReSaverHelper.dll` (and `ReSaverHelper.ini`) in `Data\SFSE\Plugins` (or install the zip as a mod in your mod
manager). Start the game with `sfse_loader.exe`.

The helper's log is `Documents\My Games\Starfield\SFSE\ReSaverHelper.log`.

## Settings (ReSaverHelper.ini)

- `sNotify` - how the helper confirms what it did: `hud` (a notification), `box` (a message box) or `none`
- `bListen` - `0` = do not accept commands from ReSaver Plus
- `bRescueHotkey` / `iRescueHoldSeconds` - the hold-Ctrl rescue and how long Ctrl must be held (3 to 60 seconds)

## How it works

Everything it does in the game goes through the game's own console (the function Starfield itself uses to run console
commands) and reads quests straight from the game. Its three addresses come from the Address Library by id, so a game
update needs only the new Address Library file, as long as Bethesda does not change those functions.

## License

Apache License 2.0, like ReSaver Plus. It uses no SFSE or CommonLib code (the few SFSE declarations it needs are in
`src/SFSE.h`, written from SFSE's documented plugin API).
