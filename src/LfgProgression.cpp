/*
 * mod-lfg-progression
 *
 * Makes the Dungeon Finder follow mod-individual-progression. The core only locks LFG dungeons by
 * the account's expansion and the player's level, so a level 60 still in the vanilla era is
 * offered Random Burning Crusade, the TBC heroics and Hellfire Ramparts, and the client
 * recommends the TBC random because it fits the level best. Joining one teleports the group
 * straight into an Outland instance, past individual progression's Dark Portal check.
 *
 * Here a dungeon from an expansion the player hasn't reached is locked, and its random entry is
 * taken out of the random list. Random Classic Dungeon, which stock data ends at level 58, stays
 * available while the player is held in the vanilla era, so the finder recommends it at 60. Individual progression keeps the era as rewarded quests 66000 + state:
 * The Burning Crusade opens at state 8 and Wrath of the Lich King at state 13, and nothing opens
 * past IndividualProgression.ProgressionLimit.
 *
 * Bots, game masters and IndividualProgression.ExcludedAccountsRegex accounts are never locked,
 * the same accounts individual progression leaves alone, so a bot can't keep a group out of a
 * dungeon the real players have unlocked.
 *
 * Released under the MIT License.
 */

#include "Config.h"
#include "AccountMgr.h"
#include "LFG.h"
#include "LFGMgr.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <mutex>
#include <regex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    constexpr uint32 PROGRESSION_QUEST_BASE = 66000;   // individual progression: state N = quest 66000 + N rewarded
    constexpr uint8 PROGRESSION_MAX = 18;
    constexpr uint8 NO_LIMIT = 0xFF;
    constexpr uint32 RANDOM_CLASSIC_DUNGEON = 258;     // LFGDungeons.dbc

    struct Config
    {
        bool enabled = true;
        bool individualProgression = false;
        uint8 tbcState = 8;
        uint8 wotlkState = 13;
        uint8 progressionLimit = 0;
        std::string excludedAccounts;
    };

    Config config;

    std::mutex excludedMutex;
    std::unordered_map<uint32, bool> excludedCache;

    // Bots are sessions without a socket. AzerothCore marks them with WorldSession::IsHeadless();
    // older playerbots core forks have WorldSession::IsBot() instead, and older stock cores have
    // neither. Looking for both at compile time lets the module build on all of them.
    template <typename Session, typename = void>
    struct HasIsHeadless : std::false_type { };

    template <typename Session>
    struct HasIsHeadless<Session, std::void_t<decltype(std::declval<Session&>().IsHeadless())>> : std::true_type { };

    template <typename Session, typename = void>
    struct HasIsBot : std::false_type { };

    template <typename Session>
    struct HasIsBot<Session, std::void_t<decltype(std::declval<Session&>().IsBot())>> : std::true_type { };

    template <typename Session>
    bool IsBotSession(Session* session)
    {
        if constexpr (HasIsHeadless<Session>::value)
            return session->IsHeadless();
        else if constexpr (HasIsBot<Session>::value)
            return session->IsBot();
        else
            return false;
    }

    bool IsExcludedAccount(uint32 accountId)
    {
        if (config.excludedAccounts.empty())
            return false;

        std::lock_guard<std::mutex> lock(excludedMutex);
        auto itr = excludedCache.find(accountId);
        if (itr != excludedCache.end())
            return itr->second;

        // AccountMgr::GetName asks the database, so each account is looked up once.
        std::string name;
        bool excluded = false;
        if (AccountMgr::GetName(accountId, name))
        {
            try
            {
                excluded = std::regex_match(name, std::regex(config.excludedAccounts));
            }
            catch (std::regex_error const&)
            {
                excluded = false;
            }
        }

        excludedCache[accountId] = excluded;
        return excluded;
    }

    uint8 ProgressionState(Player* player)
    {
        uint8 state = 0;
        for (uint8 i = 1; i <= PROGRESSION_MAX; ++i)
            if (player->GetQuestStatus(PROGRESSION_QUEST_BASE + i) == QUEST_STATUS_REWARDED)
                state = i;
        return state;
    }

    // Mirrors IndividualProgression::hasPassedProgression: a state past the limit never counts.
    bool HasReached(uint8 playerState, uint8 required)
    {
        if (config.progressionLimit && required > config.progressionLimit)
            return false;
        return playerState >= required;
    }

    // The newest expansion whose dungeons this player may queue for, or NO_LIMIT when the player
    // isn't gated at all.
    uint8 AllowedExpansion(Player* player)
    {
        if (!config.enabled || !config.individualProgression || !player)
            return NO_LIMIT;

        WorldSession* session = player->GetSession();
        if (!session || IsBotSession(session) || player->IsGameMaster() || IsExcludedAccount(session->GetAccountId()))
            return NO_LIMIT;

        uint8 state = ProgressionState(player);
        if (HasReached(state, config.wotlkState))
            return EXPANSION_WRATH_OF_THE_LICH_KING;
        if (HasReached(state, config.tbcState))
            return EXPANSION_THE_BURNING_CRUSADE;
        return EXPANSION_CLASSIC;
    }

    // One random dungeon entry of SMSG_LFG_PLAYER_INFO, written the way
    // WorldSession::HandleLfgPlayerLockInfoRequestOpcode writes it.
    void AppendRandomDungeon(WorldPacket& data, Player* player, uint32 entry)
    {
        uint8 level = player->GetLevel();
        data << uint32(entry);

        Quest const* quest = nullptr;
        bool done = false;
        if (lfg::LfgReward const* reward = sLFGMgr->GetRandomDungeonReward(entry, level))
        {
            quest = sObjectMgr->GetQuestTemplate(reward->firstQuest);
            if (quest)
            {
                done = !player->CanRewardQuest(quest, false);
                if (done)
                    quest = sObjectMgr->GetQuestTemplate(reward->otherQuest);
            }
        }

        if (!quest)
        {
            data << uint8(0) << uint32(0) << uint32(0) << uint32(0) << uint32(0) << uint8(0);
            return;
        }

        uint8 levelForXP = level;
        sScriptMgr->OnPlayerBeforeGetLevelForXPGain(player, levelForXP);

        data << uint8(done);
        data << uint32(quest->GetRewOrReqMoney(level));
        data << uint32(levelForXP < player->GetUInt32Value(PLAYER_FIELD_MAX_LEVEL) ? quest->XPValue(levelForXP) : 0);
        data << uint32(0);
        data << uint32(0);
        data << uint8(quest->GetRewItemsCount());
        for (uint8 i = 0; i < QUEST_REWARDS_COUNT; ++i)
        {
            if (uint32 itemId = quest->RewardItemId[i])
            {
                ItemTemplate const* item = sObjectMgr->GetItemTemplate(itemId);
                data << uint32(itemId);
                data << uint32(item ? item->DisplayInfoID : 0);
                data << uint32(quest->RewardItemIdCount[i]);
            }
        }
    }

    // Rebuilds SMSG_LFG_PLAYER_INFO without the random dungeons of a locked expansion. Returns
    // false when the packet needs no change.
    bool FilterPlayerInfo(WorldPacket const& original, Player* player, uint8 allowed, WorldPacket& filtered)
    {
        WorldPacket in(original);
        in.rpos(0);

        struct RandomEntry { uint32 entry; size_t start; size_t end; };
        std::vector<RandomEntry> randoms;

        uint8 count = in.read<uint8>();
        for (uint8 i = 0; i < count; ++i)
        {
            RandomEntry random;
            random.start = in.rpos();
            random.entry = in.read<uint32>();
            in.read_skip<uint8>();                          // done
            in.read_skip<uint32>();                         // money
            in.read_skip<uint32>();                         // xp
            in.read_skip<uint32>();
            in.read_skip<uint32>();
            uint8 items = in.read<uint8>();
            in.read_skip(size_t(items) * 3 * sizeof(uint32));
            random.end = in.rpos();
            randoms.push_back(random);
        }
        size_t lockBlock = in.rpos();

        std::vector<RandomEntry> kept;
        bool hasRegularRandom = false;
        for (RandomEntry const& random : randoms)
        {
            lfg::LFGDungeonData const* dungeon = sLFGMgr->GetLFGDungeon(random.entry & 0x00FFFFFF);
            if (dungeon && dungeon->expansion > allowed)
                continue;

            kept.push_back(random);
            if (dungeon && !dungeon->seasonal)
                hasRegularRandom = true;
        }

        // Random Classic Dungeon ends at level 58, where the core hands the player over to the
        // TBC random. A player held in the vanilla era gets it back, so the finder still has a
        // random to recommend. OnInitializeLockedDungeons lifts its level lock to match.
        bool addClassic = false;
        if (!hasRegularRandom && allowed == EXPANSION_CLASSIC)
        {
            lfg::LFGDungeonData const* classic = sLFGMgr->GetLFGDungeon(RANDOM_CLASSIC_DUNGEON);
            addClassic = classic && player->GetLevel() >= classic->minlevel;
        }

        if (kept.size() == randoms.size() && !addClassic)
            return false;

        filtered.Initialize(SMSG_LFG_PLAYER_INFO, original.size());
        filtered << uint8(kept.size() + (addClassic ? 1 : 0));
        if (addClassic)
            AppendRandomDungeon(filtered, player, sLFGMgr->GetLFGDungeon(RANDOM_CLASSIC_DUNGEON)->Entry());
        for (RandomEntry const& random : kept)
            filtered.append(original.contents() + random.start, random.end - random.start);
        if (original.size() > lockBlock)
            filtered.append(original.contents() + lockBlock, original.size() - lockBlock);
        return true;
    }

    thread_local bool resending = false;
}

class LfgProgressionWorldScript : public WorldScript
{
public:
    LfgProgressionWorldScript() : WorldScript("LfgProgressionWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        config.enabled = sConfigMgr->GetOption<bool>("LfgProgression.Enable", true);
        config.tbcState = sConfigMgr->GetOption<uint8>("LfgProgression.TbcState", 8);
        config.wotlkState = sConfigMgr->GetOption<uint8>("LfgProgression.WotlkState", 13);

        // Individual progression's own settings, so both modules agree on who is gated.
        config.individualProgression = sConfigMgr->GetOption<bool>("IndividualProgression.Enable", false, false);
        config.progressionLimit = sConfigMgr->GetOption<uint8>("IndividualProgression.ProgressionLimit", 0, false);
        config.excludedAccounts = sConfigMgr->GetOption<std::string>("IndividualProgression.ExcludedAccountsRegex", "", false);

        {
            std::lock_guard<std::mutex> lock(excludedMutex);
            excludedCache.clear();
        }

        if (!config.enabled)
            LOG_INFO("server.loading", "mod-lfg-progression: disabled");
        else if (!config.individualProgression)
            LOG_INFO("server.loading", "mod-lfg-progression: individual progression is off, the Dungeon Finder is not gated");
        else
            LOG_INFO("server.loading", "mod-lfg-progression: enabled, TBC dungeons at progression {}, WotLK at {}, limit {}",
                uint32(config.tbcState), uint32(config.wotlkState), uint32(config.progressionLimit));
    }
};

class LfgProgressionGlobalScript : public GlobalScript
{
public:
    LfgProgressionGlobalScript() : GlobalScript("LfgProgressionGlobalScript", { GLOBALHOOK_ON_INITIALIZE_LOCKED_DUNGEONS }) { }

    void OnInitializeLockedDungeons(Player* player, uint8& /*level*/, uint32& lockData, lfg::LFGDungeonData const* dungeon) override
    {
        if (!dungeon)
            return;

        uint8 allowed = AllowedExpansion(player);

        // Random Classic Dungeon ends at level 58, where the core moves players on to the TBC
        // random. A player held in the vanilla era keeps it; its dungeons keep their own level
        // checks. FilterPlayerInfo puts it back in the random list.
        if (allowed == EXPANSION_CLASSIC && dungeon->id == RANDOM_CLASSIC_DUNGEON && lockData == lfg::LFG_LOCKSTATUS_TOO_HIGH_LEVEL)
        {
            lockData = 0;
            return;
        }

        // A dungeon the core already locks keeps the core's reason. The era lock reads "You have
        // not completed the required quest": individual progression is kept as quests, and the
        // expansion reason told players they don't own The Burning Crusade.
        if (!lockData && dungeon->expansion > allowed)
            lockData = lfg::LFG_LOCKSTATUS_QUEST_NOT_COMPLETED;
    }
};

class LfgProgressionServerScript : public ServerScript
{
public:
    LfgProgressionServerScript() : ServerScript("LfgProgressionServerScript", { SERVERHOOK_CAN_PACKET_SEND }) { }

    bool CanPacketSend(WorldSession* session, WorldPacket const& packet) override
    {
        if (resending || packet.GetOpcode() != SMSG_LFG_PLAYER_INFO || !session)
            return true;

        Player* player = session->GetPlayer();
        uint8 allowed = AllowedExpansion(player);
        if (!player || allowed == NO_LIMIT)
            return true;

        WorldPacket filtered;
        try
        {
            if (!FilterPlayerInfo(packet, player, allowed, filtered))
                return true;
        }
        catch (ByteBufferException const&)
        {
            LOG_ERROR("module", "mod-lfg-progression: could not read SMSG_LFG_PLAYER_INFO for {}, sent it unchanged", player->GetName());
            return true;
        }

        resending = true;
        session->SendPacket(&filtered);
        resending = false;
        return false;
    }
};

void AddLfgProgressionScripts()
{
    new LfgProgressionWorldScript();
    new LfgProgressionGlobalScript();
    new LfgProgressionServerScript();
}
