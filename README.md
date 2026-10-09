# mod-lfg-progression

An [AzerothCore](https://www.azerothcore.org) module that keeps the Dungeon
Finder inside the expansion a character has reached. Per player, with no
config switch when some of your players are in vanilla and others in TBC.

- A **random dungeon** from a later expansion is swapped on queue for the
  group's own: a level 60 who picks *Random Burning Crusade* queues for
  *Random Classic*.
- **Specific dungeons** from a later expansion are shown as locked
  (`LFG_LOCKSTATUS_QUEST_NOT_COMPLETED`).
- With [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots),
  **random bots** follow the real players' expansion: a vanilla player is not
  grouped with level 61-65 bots in Stratholme.

Works on its own (a character's expansion is its level) and together with
[mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression)
(a character's expansion is its progression tier).

## Patch Notes: Era-Bound Dungeon Finder

Category: Dungeons

- The **Dungeon Finder** now follows your progression. Dungeons from an era you haven't reached are locked.
- At level 59 and 60, choosing **Random Burning Crusade Dungeon** while still in the classic era puts you in the queue for a **Random Classic Dungeon** instead.
- Groups queue for the era of their least-progressed member.
- Players you're matched with in the Dungeon Finder are never above the level cap of your era.

## Why

From the client's own `LFGDungeons.dbc`:

| Random dungeon | Levels | Expansion |
|---|---|---|
| 258 Random Classic | 15-58 | vanilla |
| 259 Random Burning Crusade | 59-68 | TBC |
| 260 Random Burning Crusade Heroic | 70-73 | TBC |
| 261 Random Lich King | 69-80 | WotLK |
| 262 Random Lich King Heroic | 80-83 | WotLK |

A level 59-60 can only pick Random Burning Crusade, and a level 69-70 is
offered Random Lich King. Under mod-individual-progression that is worse than
wrong: IP turns a vanilla character away at the entrance of every Outland
instance, and a TBC character at Northrend.

### Why not just fix the levels in `lfgdungeons_dbc`

The server would take it: `LFGDungeons.dbc` can be overridden in SQL. But the
client's `Interface/FrameXML/LFDFrame.lua` only shows a random dungeon if the
player's level is inside the **client's** DBC range:

```lua
local function isRandomDungeonDisplayable(id)
	local name, typeID, minLevel, maxLevel, _, _, _, expansionLevel = GetLFGDungeonInfo(id);
	local myLevel = UnitLevel("player");
	return myLevel >= minLevel and myLevel <= maxLevel and EXPANSION_LEVEL >= expansionLevel;
end
```

Random Classic would still be hidden at 59-60, and without Random Burning
Crusade a level 60 would have no random to pick. Changing the client's DBC
means patching every client.

## How it works

### Random dungeons are swapped on queue

`OnPlayerQueueRandomDungeon` is called in `LFGMgr::JoinLfg` with the player who
queues (the group leader), **before** the random id is expanded into dungeons.
It is the same trick `mod-rdf-expansion` uses globally; here it happens per
group:

1. The group's expansion is the **lowest** among its players.
2. If the chosen random is from a later expansion, it is swapped down: a
   vanilla group that picks Random Burning Crusade queues for Random Classic.
   Heroic stays heroic where the expansion has one.

The player still picks "Random Burning Crusade Dungeon" in the dropdown -- it is
the only one the client shows at 59-60 -- but the queue then reads Random
Classic, because `JoinLfg` stores the swapped id with `SetSelectedDungeons`.
The reward is Random Classic's: `GetRandomDungeonReward()` falls back to the
last row when the level is above the table's highest `maxLevel`.

### Specific dungeons are locked

`LFGMgr::InitializeLockedDungeons()` works out every character's locked
dungeons on login and on every level change, and calls
`OnInitializeLockedDungeons` for each dungeon. A specific dungeon from a later
expansion than the player's gets `LFG_LOCKSTATUS_QUEST_NOT_COMPLETED`, and
the client shows it as locked with "You have not completed the required
quest". The expansion reason would read "you don't own The Burning Crusade",
which is wrong when every account has the expansion and the era is earned.
The random entries are **not** locked -- a locked random is greyed out, and
then the swap above could never happen.

On join, `GetCompatibleDungeons()` removes locked dungeons, so matchmaking can
never place anyone in them. Random pools are expanded before the locks are
checked, so they are covered.

A dungeon's expansion is the `ExpansionLevel` column of `LFGDungeons.dbc`, not
its level range: Stratholme is vanilla even though its range reaches above 60.

### Random bots follow the players (mod-playerbots)

Playerbots lets random bots queue for exactly the dungeons a real player is
queued for (`RandomPlayerbotMgr::CheckLfgQueue`), and picks them by level
alone: up to `MinLevel + 10` for a specific dungeon. A level 60 in Stratholme
gets bots up to 65.

Which bot has to fit whom is only known when the queue builds a proposal, so
the rule sits there. `LFGQueue::CheckCompatibility` calls the
`CanCreateLfgProposal` global hook right before a full proposal is created,
and a no skips that combination; the bot stays in the queue for another
group.

1. On join (`OnPlayerCanJoinLfg`) every queue entry -- a player alone or a
   group -- is stored with the lowest and highest expansion among its real
   players and the level of its highest random bot.
2. In the proposal, random bots in an entry without a real player may be at
   most the cap of the players' expansion: 60 for vanilla, 70 for TBC, no cap
   for WotLK. With players in different expansions, `BotTier` picks the lowest
   (default) or the highest.
3. A random bot you invited into your own group is your choice and is not
   checked. Seasonal bosses (listed as vanilla/TBC but level 78-82) are
   exempt.

The proposal hook runs in a map updater thread while maps update, so it only
reads the stored entries and never a `Player`. They are written on join, which
runs in the world thread while no map updates (players because
`CMSG_LFG_JOIN` is `PROCESS_THREADUNSAFE`, bots because playerbots handles
their packets from `WorldScript::OnUpdate` and the master's session update).
A random bot that is randomized while queued updates its entry on level
change.

This is independent of playerbots' own level rules: it decides which of the
bots playerbots already queued may join a given group.

## A character's expansion

| Account | Expansion |
|---|---|
| controlled by mod-individual-progression | IP's tier: vanilla until Naxxramas-40 (`PROGRESSION_PRE_TBC`), TBC until Sunwell (`PROGRESSION_TBC_TIER_5`), then WotLK |
| excluded by IP, IP disabled, or not installed | the level: up to and including 60 vanilla, up to and including 70 TBC |

The level is a floor for IP characters, because IP holds a vanilla character
at 60 and a TBC character at 70. So IP is only asked when the level alone does
not decide. `isNormalAccount()` looks up the account name in the auth database
and builds a regex per call, and the lock hook runs once per dungeon, so the
expansion is worked out once per pass of `InitializeLockedDungeons()`.

| Player | Picks | Gets |
|---|---|---|
| vanilla (IP), level 60 | Random Burning Crusade | Random Classic; Hellfire Ramparts locked |
| TBC (IP, Naxx40 cleared), level 60 | Random Burning Crusade | Random Burning Crusade |
| TBC, level 69-70 | Random Lich King | Random Burning Crusade; Utgarde Keep locked |
| WotLK | anything | unchanged |

## Optional modules

Both are detected at compile time with `__has_include`; the startup line says
which were found.

- **mod-individual-progression** (`IndividualProgression.h`): the IP tier
  decides, as above.
- **mod-playerbots** (`PlayerbotAIConfig.h`): random bots (accounts in
  `randomBotAccounts`) are never locked themselves -- they have no expansion
  of their own -- but follow the players' expansion in the queue, as above.
  Your own altbots count like players. Without playerbots the bot rule is not
  built.
- **Death Knight bots** get the end of the Ebon Hold chain (*Where Kings Walk*
  or *Warchief's Blessing*). The core locks every dungeon for a Death Knight
  who has not finished it, and bots never play it, so a party with a DK bot
  could not queue ("does not have the required quest"). Playerbots only
  rewards the chain with `AiPlayerbot.PreQuests = 1` and wipes it again when
  it randomizes a bot, so the lock hook hands it out whenever the core locks
  a bot for it. Real players still have to finish the chain themselves.

A module that is present in `modules/` but disabled in CMake still has its
header found, and the build then fails at link time. Remove the directory
instead.

## Requirements

- [AzerothCore](https://www.azerothcore.org/) wotlk (master) and a WoW 3.3.5a (12340) client.
- Optional: [mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression)
  so a character's expansion is its progression tier. Without it the level decides.
- Optional: [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) (with its core
  fork) so random bots follow the players' expansion.

## Installation

1. Clone into `modules/` of your AzerothCore source tree and rebuild:
   ```bash
   cd azerothcore-wotlk/modules
   git clone https://github.com/buildthehomelab/wow-mod-lfg-progression.git mod-lfg-progression
   ```
   The folder must be `mod-lfg-progression`: AzerothCore builds the loader
   name from it.
2. Copy `conf/mod_lfg_progression.conf.dist` to `mod_lfg_progression.conf` in your
   config directory (the defaults also apply without it).

`Server.log` after start:
`LfgProgression: enabled (vanilla up to level 60, TBC up to 70; individual progression found, playerbots found, random bots follow the lowest player expansion)`,
and for every swapped queue
`LfgProgression: <name> (level 60) queues random 259 -> 258 (group expansion 0)`.

With playerbots, a rejected proposal is logged at debug level
(`LfgProgression: proposal … rejected, a random bot is above the players' expansion`).

To check in game as a vanilla level 60: pick Random Burning Crusade; the queue
should read Random Classic and the dungeon should be vanilla. Hellfire
Ramparts should show as locked under Specific Dungeons.

## Configuration

| Key | Default | |
|---|---|---|
| `LfgProgression.Enable` | 1 | enable the module |
| `LfgProgression.ClassicMaxLevel` | 60 | highest vanilla level for a character without an IP tier, and the bot cap when the players are vanilla |
| `LfgProgression.TbcMaxLevel` | 70 | highest TBC level for a character without an IP tier, and the bot cap when the players are TBC |
| `LfgProgression.BotLock` | 1 | with playerbots: random bots follow the real players' expansion |
| `LfgProgression.BotTier` | 0 | with playerbots: players in different expansions; 0 = bots follow the lowest, 1 = the highest |

## Tests

```bash
./test/run.sh
```

Needs only g++ and tests the rules. The `core-build` workflow compiles the
module against AzerothCore.

## Troubleshooting

- **The build fails at link time.** A module that is present in `modules/` but disabled in CMake
  still has its header found. Remove the directory of the optional module you don't use.
- **Not sure which optional modules were found.** Read the `LfgProgression: enabled (...)` line in
  `Server.log` after start; it says whether individual progression and playerbots were found.
- **A Death Knight in the party "does not have the required quest".** A bot is fixed on the
  next lock refresh (opening the Dungeon Finder, joining the group, level change). A real
  player Death Knight has to finish the Ebon Hold chain; that lock is the core's.
- **Random bots are still too high for the group.** The rule needs `LfgProgression.BotLock = 1`,
  and it skips random bots you invited into your own group and the seasonal bosses.

## Credits

Author: [buildthehomelab](https://github.com/buildthehomelab)

Based on [MekBits/mod-lfg-expansion](https://github.com/MekBits/mod-lfg-expansion) (AGPL-3.0).
Changes here: the module and its settings are renamed to `mod-lfg-progression` / `LfgProgression.*`,
and later-expansion dungeons are locked as "quest not completed" instead of "insufficient
expansion".

## License

GNU Affero General Public License v3.0, see [LICENSE](LICENSE).
