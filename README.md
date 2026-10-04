# LFG Progression

An [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) module that makes the Dungeon Finder
follow [mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression).

Without it, the Dungeon Finder only looks at your level and your account's expansion. A level 60
who is still in the vanilla era gets offered **Random Burning Crusade Dungeon**, the TBC heroics and
Hellfire Ramparts, and the finder even picks the TBC random for you because it fits your level
best. Queueing for one teleports the group straight into an Outland dungeon, past individual
progression's Dark Portal check.

With the module:

- **Dungeons from an era you haven't reached are locked.** The Burning Crusade dungeons open at
  progression 8 (the Dark Portal), Wrath of the Lich King dungeons at 13 (Northrend). In the
  Specific Dungeons list they show the lock icon, with the "requires expansion" reason.
- **Their random entries disappear.** Random Burning Crusade and Random Wrath of the Lich King
  (and their heroics) aren't offered until you reach that era, so the finder recommends the best
  random of your own era, Random Classic Dungeon at 60.
- **Random Classic stays available while you're held in vanilla**, even at the level where the
  client would normally move you to the TBC random. Which dungeons it can pick still depends on
  each dungeon's own level range.
- **Groups are checked per player.** A group can only queue for a dungeon every real player in it
  has unlocked.
- **Bots are never locked**, so a playerbot can't keep a group out of a dungeon its real players
  have unlocked. Game masters and the accounts in `IndividualProgression.ExcludedAccountsRegex` are
  left alone too, the same as individual progression does.

`IndividualProgression.ProgressionLimit` is respected: on a server capped at 7 (vanilla only), no
TBC or WotLK dungeon opens for anyone. When individual progression is off, or not installed, the
module does nothing.

Holiday bosses follow their dungeon's era, so **Ahune** (in the Slave Pens) is locked until The
Burning Crusade opens. The Headless Horseman, Coren Direbrew and the Crown Chemical Co. are in
vanilla dungeons and stay open.

## Patch Notes: Era-Aware Dungeon Finder

Category: Dungeons

- The **Dungeon Finder** now follows your personal progression. Dungeons from an era you haven't
  reached yet are locked, and their random dungeons are no longer offered.
- At level 60 in the vanilla era, the Dungeon Finder recommends **Random Classic Dungeon** instead
  of **Random Burning Crusade Dungeon**.
- Fixed an issue where the Dungeon Finder could teleport a group into an Outland dungeon before
  its players had opened the Dark Portal.

> Your progression decides which dungeons you can queue for, not just your level. Bots in your
> group go wherever you go.

## Installation

Clone it into your AzerothCore `modules` folder, **as `mod-lfg-progression`**. The folder name
matters, because AzerothCore derives the module's loader name from it:

```bash
cd azerothcore-wotlk/modules
git clone https://github.com/buildthehomelab/wow-mod-lfg-progression.git mod-lfg-progression
```

Re-run CMake, rebuild the worldserver, and copy `conf/mod_lfg_progression.conf.dist` to
`mod_lfg_progression.conf` in your config directory. The module needs no SQL and no client patch.

It doesn't link against individual progression. It reads a player's era the way individual
progression stores it, as rewarded quests 66000 + state, and reads individual progression's
`Enable`, `ProgressionLimit` and `ExcludedAccountsRegex` settings from its config. Bots are
recognized as headless sessions (`WorldSession::IsHeadless()`), or by `WorldSession::IsBot()` on
older playerbots cores.

To check that it's loaded, look for this line in the worldserver log at startup:

```
mod-lfg-progression: enabled, TBC dungeons at progression 8, WotLK at 13, limit 7
```

## Configuration

| Option | Default | Description |
|---|---|---|
| `LfgProgression.Enable` | `1` | Master switch. |
| `LfgProgression.TbcState` | `8` | Individual progression state that opens The Burning Crusade dungeons. |
| `LfgProgression.WotlkState` | `13` | Individual progression state that opens the Wrath of the Lich King dungeons. |

## How it works

- `GlobalScript::OnInitializeLockedDungeons` locks every LFG dungeon whose expansion is past the
  player's era with `LFG_LOCKSTATUS_INSUFFICIENT_EXPANSION`. The core uses that lock map both for
  what the client shows and for which dungeons a queue may pick, so a locked dungeon can never be
  matched.
- `ServerScript::CanPacketSend` rewrites `SMSG_LFG_PLAYER_INFO`, the packet that lists the random
  dungeons the client offers, without the randoms of locked expansions. The client picks its
  recommended random from that list.

## License

MIT. See [LICENSE](LICENSE).
