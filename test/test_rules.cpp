// Logic test for the mod-lfg-progression rules. No AzerothCore -- only the rules.
#include "lfg_progression_rules.h"

#include <cstdio>
#include <vector>

using namespace LfgProgression;

static int g_fail = 0;

static void Expect(char const* what, bool got, bool want)
{
    bool const ok = got == want;
    if (!ok)
        ++g_fail;
    std::printf("%-66s %-7s %s\n", what, got ? "locked" : "open", ok ? "ok" : "FAIL");
}

static void ExpectN(char const* what, unsigned got, unsigned want)
{
    bool const ok = got == want;
    if (!ok)
        ++g_fail;
    std::printf("%-66s %-7u %s\n", what, got, ok ? "ok" : "FAIL");
}

static void ExpectProposal(char const* what, Settings const& s, std::initializer_list<QueueEntry> entries, bool wantRejected)
{
    std::vector<QueueEntry> const v(entries);
    Expect(what, !IsProposalAllowed(s, v), wantRejected);
}

// Queue entries in a proposal: a player alone, a group, a random bot alone.
static QueueEntry Solo(uint8_t tier)
{
    QueueEntry e;
    e.hasPlayers = true;
    e.lowestTier = e.highestTier = tier;
    return e;
}

static QueueEntry Party(uint8_t lowest, uint8_t highest, uint8_t botLevel = 0)
{
    QueueEntry e = Solo(lowest);
    e.highestTier = highest;
    e.highestBotLevel = botLevel;
    return e;
}

static QueueEntry Bot(uint8_t level)
{
    QueueEntry e;
    e.highestBotLevel = level;
    return e;
}

static QueueEntry Seasonal(QueueEntry e)
{
    e.seasonal = true;
    return e;
}

int main()
{
    Settings const def;
    constexpr int NOT_IP = -1;

    std::printf("--- random bots in a proposal (locked = rejected) ---\n");
    // The symptom: a vanilla player in Stratholme got level 61-65 bots.
    ExpectProposal("vanilla + random bot 60", def, { Solo(EXPANSION_CLASSIC), Bot(60) }, false);
    ExpectProposal("vanilla + random bot 61", def, { Solo(EXPANSION_CLASSIC), Bot(61) }, true);
    ExpectProposal("vanilla + random bots 58, 59, 65", def, { Solo(EXPANSION_CLASSIC), Bot(58), Bot(59), Bot(65) }, true);
    ExpectProposal("IP TBC at level 60 + random bot 61", def, { Solo(PlayerExpansion(def, 60, EXPANSION_TBC)), Bot(61) }, false);
    ExpectProposal("TBC + random bot 70", def, { Solo(EXPANSION_TBC), Bot(70) }, false);
    ExpectProposal("TBC + random bot 71", def, { Solo(EXPANSION_TBC), Bot(71) }, true);
    ExpectProposal("WotLK + random bot 80", def, { Solo(EXPANSION_WOTLK), Bot(80) }, false);
    // Accounts without IP: the level decides.
    ExpectProposal("no IP, level 60 + random bot 61", def, { Solo(PlayerExpansion(def, 60, NOT_IP)), Bot(61) }, true);
    ExpectProposal("no IP, level 61 + random bot 65", def, { Solo(PlayerExpansion(def, 61, NOT_IP)), Bot(65) }, false);

    std::printf("\n--- mixed expansions ---\n");
    ExpectN("BotTier without a key -> lowest", def.botTier, BOT_TIER_LOWEST);
    ExpectProposal("vanilla + TBC queued apart + random bot 61", def, { Solo(EXPANSION_CLASSIC), Solo(EXPANSION_TBC), Bot(61) }, true);
    ExpectProposal("vanilla/TBC group + random bot 61", def, { Party(EXPANSION_CLASSIC, EXPANSION_TBC), Bot(61) }, true);
    ExpectProposal("vanilla + WotLK queued apart + random bot 75", def, { Solo(EXPANSION_CLASSIC), Solo(EXPANSION_WOTLK), Bot(75) }, true);
    {
        Settings s;
        s.botTier = BOT_TIER_HIGHEST;
        ExpectProposal("BotTier 1: vanilla + TBC queued apart + random bot 61", s, { Solo(EXPANSION_CLASSIC), Solo(EXPANSION_TBC), Bot(61) }, false);
        ExpectProposal("BotTier 1: vanilla/TBC group + random bot 61", s, { Party(EXPANSION_CLASSIC, EXPANSION_TBC), Bot(61) }, false);
        ExpectProposal("BotTier 1: vanilla + TBC + random bot 71", s, { Solo(EXPANSION_CLASSIC), Solo(EXPANSION_TBC), Bot(71) }, true);
        ExpectProposal("BotTier 1: vanilla + WotLK + random bot 75", s, { Solo(EXPANSION_CLASSIC), Solo(EXPANSION_WOTLK), Bot(75) }, false);
    }

    std::printf("\n--- exceptions ---\n");
    // The seasonal bosses are vanilla/TBC in LFGDungeons.dbc but level 78-82.
    ExpectProposal("seasonal boss: vanilla + random bot 80", def, { Seasonal(Solo(EXPANSION_CLASSIC)), Seasonal(Bot(80)) }, false);
    ExpectProposal("seasonal boss on the bot's entry only", def, { Solo(EXPANSION_CLASSIC), Seasonal(Bot(80)) }, false);
    // A random bot you invited yourself is your own choice.
    ExpectProposal("vanilla group with an invited random bot 65", def, { Party(EXPANSION_CLASSIC, EXPANSION_CLASSIC, 65) }, false);
    ExpectProposal("vanilla group with an invited random bot 65 + random bot 61", def, { Party(EXPANSION_CLASSIC, EXPANSION_CLASSIC, 65), Bot(61) }, true);
    ExpectProposal("random bots only (playerbots rejects those itself)", def, { Bot(61), Bot(80) }, false);
    ExpectProposal("no known entries", def, { }, false);
    { Settings s; s.botLock = false; ExpectProposal("BotLock 0: vanilla + random bot 80", s, { Solo(EXPANSION_CLASSIC), Bot(80) }, false); }
    { Settings s; s.enabled = false; ExpectProposal("Enable 0: vanilla + random bot 80", s, { Solo(EXPANSION_CLASSIC), Bot(80) }, false); }

    std::printf("\n--- a player's expansion ---\n");
    ExpectN("level 58, no IP", PlayerExpansion(def, 58, NOT_IP), EXPANSION_CLASSIC);
    ExpectN("level 60, no IP -> vanilla", PlayerExpansion(def, 60, NOT_IP), EXPANSION_CLASSIC);
    ExpectN("level 61, no IP -> TBC", PlayerExpansion(def, 61, NOT_IP), EXPANSION_TBC);
    ExpectN("level 70, no IP -> TBC", PlayerExpansion(def, 70, NOT_IP), EXPANSION_TBC);
    ExpectN("level 71, no IP -> WotLK", PlayerExpansion(def, 71, NOT_IP), EXPANSION_WOTLK);
    ExpectN("level 60, IP vanilla", PlayerExpansion(def, 60, EXPANSION_CLASSIC), EXPANSION_CLASSIC);
    ExpectN("level 60, IP TBC (Naxx40 cleared) -> TBC", PlayerExpansion(def, 60, EXPANSION_TBC), EXPANSION_TBC);
    ExpectN("level 70, IP WotLK (Sunwell cleared) -> WotLK", PlayerExpansion(def, 70, EXPANSION_WOTLK), EXPANSION_WOTLK);
    ExpectN("level 65, IP vanilla (impossible) -> level is the floor", PlayerExpansion(def, 65, EXPANSION_CLASSIC), EXPANSION_TBC);
    { Settings s; s.classicMaxLevel = 58;
      ExpectN("ClassicMaxLevel 58: level 59, no IP -> TBC", PlayerExpansion(s, 59, NOT_IP), EXPANSION_TBC); }

    std::printf("\n--- specific dungeons ---\n");
    Expect("vanilla -> Hellfire Ramparts", IsPlayerLocked(def, EXPANSION_CLASSIC, EXPANSION_TBC, false), true);
    Expect("vanilla -> Stratholme", IsPlayerLocked(def, EXPANSION_CLASSIC, EXPANSION_CLASSIC, false), false);
    Expect("TBC -> Utgarde Keep", IsPlayerLocked(def, EXPANSION_TBC, EXPANSION_WOTLK, false), true);
    Expect("TBC -> Ramparts", IsPlayerLocked(def, EXPANSION_TBC, EXPANSION_TBC, false), false);
    Expect("WotLK -> Stratholme (older is always open)", IsPlayerLocked(def, EXPANSION_WOTLK, EXPANSION_CLASSIC, false), false);
    // The client would grey out a locked random, and then 59-60 could not queue random at all.
    Expect("vanilla -> the Random Burning Crusade entry (swapped instead)", IsPlayerLocked(def, EXPANSION_CLASSIC, EXPANSION_TBC, true), false);
    { Settings s; s.enabled = false; Expect("Enable 0: vanilla -> Ramparts", IsPlayerLocked(s, EXPANSION_CLASSIC, EXPANSION_TBC, false), false); }

    std::printf("\n--- random dungeon on queue ---\n");
    // The symptom: a level 60 was offered, and got, TBC dungeons.
    ExpectN("vanilla queues Random BC -> Random Classic", RandomFor(def, RDF_TBC, EXPANSION_CLASSIC), RDF_CLASSIC);
    ExpectN("TBC queues Random BC -> unchanged", RandomFor(def, RDF_TBC, EXPANSION_TBC), RDF_TBC);
    ExpectN("TBC queues Random Lich King -> Random BC", RandomFor(def, RDF_WOTLK, EXPANSION_TBC), RDF_TBC);
    ExpectN("TBC queues Random LK Heroic -> Random BC Heroic", RandomFor(def, RDF_WOTLK_HEROIC, EXPANSION_TBC), RDF_TBC_HEROIC);
    ExpectN("vanilla queues Random BC Heroic -> Random Classic", RandomFor(def, RDF_TBC_HEROIC, EXPANSION_CLASSIC), RDF_CLASSIC);
    ExpectN("WotLK queues Random LK -> unchanged", RandomFor(def, RDF_WOTLK, EXPANSION_WOTLK), RDF_WOTLK);
    ExpectN("WotLK queues Random Classic -> unchanged (never up)", RandomFor(def, RDF_CLASSIC, EXPANSION_WOTLK), RDF_CLASSIC);
    ExpectN("unknown random 999 -> unchanged", RandomFor(def, 999, EXPANSION_CLASSIC), 999u);
    { Settings s; s.enabled = false; ExpectN("Enable 0: vanilla Random BC -> unchanged", RandomFor(s, RDF_TBC, EXPANSION_CLASSIC), RDF_TBC); }

    std::printf("\n--- group ---\n");
    ExpectN("TBC leader + vanilla member -> vanilla", GroupExpansion({ EXPANSION_TBC, EXPANSION_CLASSIC }), EXPANSION_CLASSIC);
    ExpectN("WotLK + TBC + WotLK -> TBC", GroupExpansion({ EXPANSION_WOTLK, EXPANSION_TBC, EXPANSION_WOTLK }), EXPANSION_TBC);
    ExpectN("solo WotLK", GroupExpansion({ EXPANSION_WOTLK }), EXPANSION_WOTLK);

    std::printf("\n%s\n", g_fail ? "FAILED" : "all ok");
    return g_fail ? 1 : 0;
}
