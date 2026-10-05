#pragma once

// The rules for which expansion the Dungeon Finder may send a player or a
// random bot to, without a single AzerothCore type. The module translates Player, Group and
// LFGDungeonData into the arguments below, so the whole decision can be tested
// with g++ alone (test/run.sh).
//
// A dungeon's expansion is LFGDungeons.dbc's own ExpansionLevel column, not
// its levels -- so Stratholme is vanilla, even though its LFG range reaches
// above 60.

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace LfgProgression
{
    // Same numbers as the core's Expansions enum (SharedDefines.h).
    constexpr uint8_t EXPANSION_CLASSIC = 0;
    constexpr uint8_t EXPANSION_TBC     = 1;
    constexpr uint8_t EXPANSION_WOTLK   = 2;

    // Random dungeons in LFGDungeons.dbc (the same ids mod-rdf-expansion and
    // mod-individual-progression use), and their levels in the client's DBC:
    //   258 Random Classic                 15-58
    //   259 Random Burning Crusade         59-68
    //   260 Random Burning Crusade Heroic  70-73
    //   261 Random Lich King               69-80
    //   262 Random Lich King Heroic        80-83
    constexpr uint32_t RDF_CLASSIC      = 258;
    constexpr uint32_t RDF_TBC          = 259;
    constexpr uint32_t RDF_TBC_HEROIC   = 260;
    constexpr uint32_t RDF_WOTLK        = 261;
    constexpr uint32_t RDF_WOTLK_HEROIC = 262;

    // Which expansion random bots follow when the players in a proposal are
    // in different ones (LfgProgression.BotTier).
    constexpr uint8_t BOT_TIER_LOWEST  = 0;
    constexpr uint8_t BOT_TIER_HIGHEST = 1;

    struct Settings
    {
        bool enabled = true;
        bool botLock = true;    // random bots are held to the players' expansion
        uint8_t botTier = BOT_TIER_LOWEST;
        uint8_t classicMaxLevel = 60;
        uint8_t tbcMaxLevel = 70;
    };

    // Highest level in an expansion, or 0 for none: WotLK (and anything newer)
    // has no cap.
    inline uint8_t MaxLevelFor(Settings const& s, uint8_t expansion)
    {
        switch (expansion)
        {
            case EXPANSION_CLASSIC: return s.classicMaxLevel;
            case EXPANSION_TBC:     return s.tbcMaxLevel;
            default:                return 0;
        }
    }

    // --- random bots (only with mod-playerbots) ------------------------------

    // A queue entry in an LFG proposal: a player alone or a group, stored when
    // it queued.
    struct QueueEntry
    {
        bool hasPlayers = false;                  // at least one who is not a random bot
        uint8_t lowestTier = EXPANSION_WOTLK;     // among them
        uint8_t highestTier = EXPANSION_CLASSIC;
        uint8_t highestBotLevel = 0;              // highest random bot in the entry
        bool seasonal = false;                    // queued for seasonal bosses only
    };

    // Random bots follow the expansion of the real players in the proposal,
    // not their own level: with a vanilla player at most 60, with a TBC player
    // at most 70. Only entries without a player are checked -- a random bot
    // you invited yourself is your own choice. The seasonal bosses are listed
    // as vanilla/TBC in LFGDungeons.dbc but are level 78-82.
    inline bool IsProposalAllowed(Settings const& s, std::vector<QueueEntry> const& entries)
    {
        if (!s.enabled || !s.botLock)
            return true;

        bool anyPlayers = false;
        uint8_t lowest = EXPANSION_WOTLK;
        uint8_t highest = EXPANSION_CLASSIC;
        for (QueueEntry const& e : entries)
        {
            if (e.seasonal)
                return true;
            if (!e.hasPlayers)
                continue;
            anyPlayers = true;
            lowest = std::min(lowest, e.lowestTier);
            highest = std::max(highest, e.highestTier);
        }

        if (!anyPlayers)
            return true;

        uint8_t const cap = MaxLevelFor(s, s.botTier == BOT_TIER_HIGHEST ? highest : lowest);
        if (!cap)
            return true;

        for (QueueEntry const& e : entries)
            if (!e.hasPlayers && e.highestBotLevel > cap)
                return false;
        return true;
    }

    // --- players -------------------------------------------------------------

    // The expansion a level belongs to: up to and including 60 is vanilla, up
    // to and including 70 TBC, above that WotLK.
    inline uint8_t ExpansionForLevel(Settings const& s, uint8_t level)
    {
        if (level <= s.classicMaxLevel)
            return EXPANSION_CLASSIC;
        if (level <= s.tbcMaxLevel)
            return EXPANSION_TBC;
        return EXPANSION_WOTLK;
    }

    // ipTier: the expansion from mod-individual-progression (0/1/2), or -1 for
    // an account IP does not control (excluded, bot, IP disabled or not
    // installed). The level is a floor: IP holds a vanilla character at 60 and
    // a TBC character at 70, so an IP character is always at least in its
    // level's expansion. IP is what lifts a level 60 to TBC once it has
    // cleared Naxxramas.
    inline uint8_t PlayerExpansion(Settings const& s, uint8_t level, int ipTier)
    {
        uint8_t const byLevel = ExpansionForLevel(s, level);
        if (ipTier < 0)
            return byLevel;
        return std::max<uint8_t>(byLevel, static_cast<uint8_t>(ipTier));
    }

    // Players are locked out of a SPECIFIC dungeon from a later expansion.
    // The random entries are never locked: the client shows a locked random
    // as grey, and then a level 60 could not queue random at all, because
    // Random Classic stops at 58 in the client's DBC. It is swapped on queue
    // instead (RandomFor below).
    inline bool IsPlayerLocked(Settings const& s, uint8_t playerExpansion, uint8_t dungeonExpansion, bool isRandomEntry)
    {
        if (!s.enabled || isRandomEntry)
            return false;
        return dungeonExpansion > playerExpansion;
    }

    // The group's expansion is the lowest among its players: the dungeon must
    // be enterable by all of them, and IP turns a vanilla character away at
    // the entrance of an Outland instance.
    inline uint8_t GroupExpansion(std::initializer_list<uint8_t> members)
    {
        uint8_t lowest = EXPANSION_WOTLK;
        for (uint8_t e : members)
            lowest = std::min(lowest, e);
        return lowest;
    }

    inline uint8_t RandomExpansion(uint32_t rDungeonId)
    {
        switch (rDungeonId)
        {
            case RDF_CLASSIC:      return EXPANSION_CLASSIC;
            case RDF_TBC:
            case RDF_TBC_HEROIC:   return EXPANSION_TBC;
            case RDF_WOTLK:
            case RDF_WOTLK_HEROIC: return EXPANSION_WOTLK;
            default:               return 0xFF;   // unknown: left alone
        }
    }

    inline bool IsHeroicRandom(uint32_t rDungeonId)
    {
        return rDungeonId == RDF_TBC_HEROIC || rDungeonId == RDF_WOTLK_HEROIC;
    }

    // Swap a random dungeon from a later expansion for the group's own. The
    // same trick as mod-rdf-expansion, only per group instead of globally.
    // Heroic stays heroic where the expansion has one, otherwise normal
    // (vanilla has none).
    inline uint32_t RandomFor(Settings const& s, uint32_t rDungeonId, uint8_t groupExpansion)
    {
        if (!s.enabled)
            return rDungeonId;

        uint8_t const exp = RandomExpansion(rDungeonId);
        if (exp == 0xFF || exp <= groupExpansion)
            return rDungeonId;

        bool const heroic = IsHeroicRandom(rDungeonId);
        switch (groupExpansion)
        {
            case EXPANSION_CLASSIC: return RDF_CLASSIC;
            case EXPANSION_TBC:     return heroic ? RDF_TBC_HEROIC : RDF_TBC;
            default:                return rDungeonId;
        }
    }
}
