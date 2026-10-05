/*
 * mod-lfg-progression
 *
 * Based on MekBits/mod-lfg-expansion (https://github.com/MekBits/mod-lfg-expansion),
 * GNU Affero General Public License v3.0.
 *
 * The Dungeon Finder stays inside the expansion a character has reached, in
 * both directions:
 *
 *   players      never get a dungeon from an expansion they have not reached
 *   random bots  follow the real players' expansion: at most 60 with a
 *                vanilla player, at most 70 with a TBC player (mod-playerbots)
 *
 * PLAYERS. Random Classic stops at 58 and Random Burning Crusade starts at 59
 * in LFGDungeons.dbc, so a level 59-60 is only offered TBC. For a vanilla
 * character under mod-individual-progression that is worse than wrong: IP
 * turns it away at the entrance of every Outland instance.
 *
 * The levels can NOT be fixed in data alone: the client's LFDFrame.lua only
 * shows a random dungeon if the player's level is inside the CLIENT's DBC
 * range (isRandomDungeonDisplayable). So the random id is swapped on queue in
 * OnPlayerQueueRandomDungeon, the same trick as mod-rdf-expansion -- only per
 * group instead of globally. The player picks "Random Burning Crusade", queues
 * for Random Classic, and the queue status shows Random Classic afterwards
 * (JoinLfg stores the swapped id with SetSelectedDungeons).
 *
 * The player lock is the core's own. LFGMgr::InitializeLockedDungeons works
 * out every player's locked dungeons on login and on every level change and
 * calls OnInitializeLockedDungeons for each dungeon; GetCompatibleDungeons()
 * removes the locked ones on join, also after a random dungeon is expanded.
 *
 * RANDOM BOTS. mod-playerbots lets random bots queue for exactly the dungeons
 * a real player is queued for (RandomPlayerbotMgr::CheckLfgQueue), and picks
 * them by level alone. A level 55 dungeon takes bots up to 65.
 *
 * Which bot has to fit whom is only known when the queue builds a proposal,
 * so the rule sits there: LFGQueue::CheckCompatibility asks GlobalScript's
 * CanCreateLfgProposal right before the proposal is created, and a no skips
 * the combination. That call runs in a map updater thread while maps update,
 * so it touches no Player: every queue entry's expansion and bot level are
 * stored on join, which runs in the world thread while no map updates --
 * players because CMSG_LFG_JOIN is PROCESS_THREADUNSAFE, bots because
 * PlayerbotHolder::HandleBotPackets is called from WorldScript::OnUpdate and
 * from the master's SessionScript::OnSessionUpdate under ProcessUnsafe.
 *
 * mod-individual-progression and mod-playerbots are optional. With IP, a
 * character's expansion is its IP tier; without it, its level. Without
 * playerbots there are no random bots, and the bot rule is not built.
 */

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "Config.h"
#include "Group.h"
#include "LFG.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#if __has_include("IndividualProgression.h")
#include "IndividualProgression.h"
#define LFGP_WITH_INDIVIDUAL_PROGRESSION 1
#endif

#if __has_include("PlayerbotAIConfig.h")
#include "PlayerbotAIConfig.h"
#define LFGP_WITH_PLAYERBOTS 1
#endif

#include "lfg_progression_rules.h"

namespace
{
    LfgProgression::Settings g_settings;

    // Playerbots' own definition of a random bot: the account is in
    // randomBotAccounts. That list is filled once at startup, so it is safe
    // to read from the map threads, where a bot's own level-up happens.
    // RandomPlayerbotMgr::IsRandomBot() is not -- it looks in currentBots,
    // which the world thread changes all the time.
    bool IsRandomBot([[maybe_unused]] Player* player)
    {
#ifdef LFGP_WITH_PLAYERBOTS
        WorldSession* session = player->GetSession();
        return session && session->IsHeadless() && sPlayerbotAIConfig.IsInRandomAccountList(session->GetAccountId());
#else
        return false;
#endif
    }

    // mod-individual-progression's expansion for the character, or -1 if IP
    // does not control the account. isNormalAccount() looks the account name
    // up in the database and builds a regex per call, so it is only called
    // when the level alone does not decide (see PlayerExpansion).
    int IpTier([[maybe_unused]] Player* player)
    {
#ifdef LFGP_WITH_INDIVIDUAL_PROGRESSION
        if (!sIndividualProgression->enabled || !sIndividualProgression->isNormalAccount(player))
            return -1;
        if (sIndividualProgression->hasPassedProgression(player, PROGRESSION_TBC_TIER_5))
            return LfgProgression::EXPANSION_WOTLK;
        if (sIndividualProgression->hasPassedProgression(player, PROGRESSION_PRE_TBC))
            return LfgProgression::EXPANSION_TBC;
        return LfgProgression::EXPANSION_CLASSIC;
#else
        return -1;
#endif
    }

    uint8 PlayerExpansion(Player* player, uint8 level)
    {
        // Already in the highest expansion by level: IP can only lift.
        if (LfgProgression::ExpansionForLevel(g_settings, level) == LfgProgression::EXPANSION_WOTLK)
            return LfgProgression::EXPANSION_WOTLK;
        return LfgProgression::PlayerExpansion(g_settings, level, IpTier(player));
    }

    // InitializeLockedDungeons() calls the lock hook once per dungeon with the
    // same player, and IpTier() costs two synchronous auth database lookups
    // and two regexes per call. So the expansion is worked out once per pass
    // and forgotten in OnAfterInitializeLockedDungeons -- the IP tier can
    // change without the level changing. thread_local, because the locks are
    // worked out both in the world thread (login) and in the map threads
    // (level change).
    struct LockPass
    {
        ObjectGuid guid;
        uint8 level = 0;
        uint8 expansion = 0;
    };
    thread_local LockPass t_lockPass;

    uint8 PlayerExpansionForLockPass(Player* player, uint8 level)
    {
        if (t_lockPass.guid != player->GetGUID() || t_lockPass.level != level)
            t_lockPass = { player->GetGUID(), level, PlayerExpansion(player, level) };
        return t_lockPass.expansion;
    }

#ifdef LFGP_WITH_PLAYERBOTS
    // Queue entries by LFG's queue guid (the group's, otherwise the player's).
    // Written in the world thread on join and on level change in the map
    // threads, read by the LFG queue in a map updater thread.
    std::mutex g_queueMutex;
    std::map<ObjectGuid, LfgProgression::QueueEntry> g_queueEntries;

    // Only called from JoinLfg in the world thread while no map updates, so
    // every member of the group may be read, also those on other maps.
    LfgProgression::QueueEntry QueueEntryFor(Player* leader, std::set<uint32> const& dungeons)
    {
        LfgProgression::QueueEntry entry;

        entry.seasonal = !dungeons.empty() && std::all_of(dungeons.begin(), dungeons.end(), [](uint32 id)
        {
            lfg::LFGDungeonData const* dungeon = sLFGMgr->GetLFGDungeon(id);
            return dungeon && dungeon->seasonal;
        });

        auto add = [&entry](Player* member)
        {
            uint8 const level = member->GetLevel();
            if (IsRandomBot(member))
            {
                entry.highestBotLevel = std::max(entry.highestBotLevel, level);
                return;
            }
            uint8 const expansion = PlayerExpansion(member, level);
            entry.hasPlayers = true;
            entry.lowestTier = std::min(entry.lowestTier, expansion);
            entry.highestTier = std::max(entry.highestTier, expansion);
        };

        if (Group* group = leader->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource())
                    add(member);
        }
        else
            add(leader);

        return entry;
    }

    bool BotRuleActive()
    {
        return g_settings.enabled && g_settings.botLock;
    }
#endif
}

class LfgProgressionWorld : public WorldScript
{
public:
    LfgProgressionWorld() : WorldScript("LfgProgressionWorld") { }

    // Read here and not in the hooks: GetOption() logs "Missing property" on
    // every call when a key is missing, and the lock hook runs ~300 times per
    // character per level change.
    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_settings.enabled         = sConfigMgr->GetOption<bool>("LfgProgression.Enable", true);
        g_settings.classicMaxLevel = uint8(sConfigMgr->GetOption<uint32>("LfgProgression.ClassicMaxLevel", 60));
        g_settings.tbcMaxLevel     = uint8(sConfigMgr->GetOption<uint32>("LfgProgression.TbcMaxLevel", 70));

#ifdef LFGP_WITH_INDIVIDUAL_PROGRESSION
        char const* const ip = "found";
#else
        char const* const ip = "not found";
#endif

#ifdef LFGP_WITH_PLAYERBOTS
        g_settings.botLock = sConfigMgr->GetOption<bool>("LfgProgression.BotLock", true);

        uint32 const botTier = sConfigMgr->GetOption<uint32>("LfgProgression.BotTier", LfgProgression::BOT_TIER_LOWEST);
        if (botTier > LfgProgression::BOT_TIER_HIGHEST)
            LOG_ERROR("module", "LfgProgression.BotTier = {} is invalid (0 or 1); using 0", botTier);
        g_settings.botTier = botTier == LfgProgression::BOT_TIER_HIGHEST ? LfgProgression::BOT_TIER_HIGHEST : LfgProgression::BOT_TIER_LOWEST;

        char const* const bots = !g_settings.botLock ? "found, random bots not held"
            : g_settings.botTier == LfgProgression::BOT_TIER_HIGHEST ? "found, random bots follow the highest player expansion"
            : "found, random bots follow the lowest player expansion";
#else
        char const* const bots = "not found";
#endif

        LOG_INFO("module", "LfgProgression: {} (vanilla up to level {}, TBC up to {}; individual progression {}, playerbots {})",
                 g_settings.enabled ? "enabled" : "disabled",
                 g_settings.classicMaxLevel, g_settings.tbcMaxLevel, ip, bots);
    }
};

class LfgProgressionGlobal : public GlobalScript
{
public:
    LfgProgressionGlobal() : GlobalScript("LfgProgressionGlobal", {
        GLOBALHOOK_ON_INITIALIZE_LOCKED_DUNGEONS,
        GLOBALHOOK_ON_AFTER_INITIALIZE_LOCKED_DUNGEONS,
#ifdef LFGP_WITH_PLAYERBOTS
        GLOBALHOOK_CAN_CREATE_LFG_PROPOSAL,
#endif
    }) { }

    void OnInitializeLockedDungeons(Player* player, uint8& level, uint32& lockData, lfg::LFGDungeonData const* dungeon) override
    {
        // A dungeon the core already locked keeps its own reason.
        if (lockData || !dungeon || !player || !g_settings.enabled)
            return;

        // Random bots have no expansion of their own; they are held in the
        // queue instead (see CanCreateLfgProposal).
        if (IsRandomBot(player))
            return;

        // Cheapest check first: only a dungeon from a later expansion than the
        // level's can be locked, and only then is IP looked up.
        bool const isRandomEntry = dungeon->type == lfg::LFG_TYPE_RANDOM;
        if (isRandomEntry || dungeon->expansion <= LfgProgression::ExpansionForLevel(g_settings, level))
            return;

        // "You have not completed the required quest", not the expansion reason: the client
        // reads LFG_LOCKSTATUS_INSUFFICIENT_EXPANSION as "you don't own The Burning Crusade",
        // which is wrong on a realm where everyone has the expansion and the era is earned.
        if (LfgProgression::IsPlayerLocked(g_settings, PlayerExpansionForLockPass(player, level), dungeon->expansion, isRandomEntry))
            lockData = lfg::LFG_LOCKSTATUS_QUEST_NOT_COMPLETED;
    }

    void OnAfterInitializeLockedDungeons(Player* /*player*/) override
    {
        t_lockPass = {};
    }

#ifdef LFGP_WITH_PLAYERBOTS
    // LFGQueue::CheckCompatibility, right before a full proposal is created. A
    // no skips the combination; the bot stays in the queue for another group.
    // Runs in a map updater thread: only the stored queue entries are read.
    bool CanCreateLfgProposal(lfg::Lfg5Guids const& guids) override
    {
        if (!BotRuleActive())
            return true;

        std::vector<LfgProgression::QueueEntry> entries;
        entries.reserve(guids.guids.size());
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            for (ObjectGuid const& guid : guids.guids)
            {
                if (guid.IsEmpty())
                    continue;
                auto it = g_queueEntries.find(guid);
                if (it != g_queueEntries.end())
                    entries.push_back(it->second);
            }
        }

        if (LfgProgression::IsProposalAllowed(g_settings, entries))
            return true;

        LOG_DEBUG("module", "LfgProgression: proposal {} rejected, a random bot is above the players' expansion", guids.toString());
        return false;
    }
#endif
};

class LfgProgressionPlayer : public PlayerScript
{
public:
    LfgProgressionPlayer() : PlayerScript("LfgProgressionPlayer", {
        PLAYERHOOK_ON_QUEUE_RANDOM_DUNGEON,
#ifdef LFGP_WITH_PLAYERBOTS
        PLAYERHOOK_CAN_JOIN_LFG,
        PLAYERHOOK_ON_LEVEL_CHANGED,
        PLAYERHOOK_ON_LOGOUT,
#endif
    }) { }

    // Called in LFGMgr::JoinLfg with the player who queues -- the group
    // leader, if there is a group -- before the random id is expanded into
    // dungeons.
    void OnPlayerQueueRandomDungeon(Player* player, uint32& rDungeonId) override
    {
        if (!player || !g_settings.enabled)
            return;

        // Random bots queue by level and are held in the queue
        // (CanCreateLfgProposal).
        if (IsRandomBot(player))
            return;

        uint8 groupExpansion = PlayerExpansion(player, player->GetLevel());
        if (Group* group = player->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == player || IsRandomBot(member))
                    continue;
                groupExpansion = LfgProgression::GroupExpansion({ groupExpansion, PlayerExpansion(member, member->GetLevel()) });
            }
        }

        uint32 const swapped = LfgProgression::RandomFor(g_settings, rDungeonId, groupExpansion);
        if (swapped == rDungeonId)
            return;

        LOG_INFO("module", "LfgProgression: {} (level {}) queues random {} -> {} (group expansion {})",
                 player->GetName(), player->GetLevel(), rDungeonId, swapped, groupExpansion);
        rDungeonId = swapped;
    }

#ifdef LFGP_WITH_PLAYERBOTS
    // First in LFGMgr::JoinLfg, with the player who queues -- the group
    // leader, if there is a group. Random bots come here too (they send
    // CMSG_LFG_JOIN, which HandleBotPackets runs in the world thread).
    bool OnPlayerCanJoinLfg(Player* player, uint8 /*roles*/, std::set<uint32>& dungeons, std::string const& /*comment*/) override
    {
        if (!player)
            return true;

        Group* group = player->GetGroup();
        ObjectGuid const key = group ? group->GetGUID() : player->GetGUID();

        // An entry from before BotLock was reloaded to 0 must not be read
        // again if it is reloaded back to 1.
        if (!BotRuleActive())
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            g_queueEntries.erase(key);
            return true;
        }

        LfgProgression::QueueEntry const entry = QueueEntryFor(player, dungeons);

        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_queueEntries[key] = entry;
        return true;
    }

    // A random bot can be randomized while it is in the queue.
    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        if (!player || !IsRandomBot(player))
            return;

        Group* group = player->GetGroup();
        ObjectGuid const key = group ? group->GetGUID() : player->GetGUID();
        uint8 const level = player->GetLevel();

        std::lock_guard<std::mutex> lock(g_queueMutex);
        auto it = g_queueEntries.find(key);
        if (it == g_queueEntries.end())
            return;
        // Only the bot's own level is known here, not the rest of the group's.
        it->second.highestBotLevel = group ? std::max(it->second.highestBotLevel, level) : level;
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_queueEntries.erase(player->GetGUID());
    }
#endif
};

#ifdef LFGP_WITH_PLAYERBOTS
class LfgProgressionGroup : public GroupScript
{
public:
    LfgProgressionGroup() : GroupScript("LfgProgressionGroup", {
        GROUPHOOK_ON_DISBAND
    }) { }

    void OnDisband(Group* group) override
    {
        if (!group)
            return;
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_queueEntries.erase(group->GetGUID());
    }
};
#endif

void AddLfgProgressionScripts()
{
    new LfgProgressionWorld();
    new LfgProgressionGlobal();
    new LfgProgressionPlayer();
#ifdef LFGP_WITH_PLAYERBOTS
    new LfgProgressionGroup();
#endif
}
