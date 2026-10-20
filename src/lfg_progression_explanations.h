#pragma once

#include "lfg_progression_rules.h"
#include <string>

namespace LfgProgression
{
    inline char const* ExpansionName(uint8_t expansion)
    {
        switch (expansion)
        {
            case EXPANSION_CLASSIC: return "Classic";
            case EXPANSION_TBC: return "The Burning Crusade";
            case EXPANSION_WOTLK: return "Wrath of the Lich King";
            default: return "Unknown era";
        }
    }

    // Describe the same effective era used by matchmaking, including its level
    // floor. Never tell an IP-excluded character to complete progression raids.
    inline std::string UnlockRequirement(Settings const& settings, uint8_t level, int ipTier, uint8_t target)
    {
        if (!settings.enabled || target <= PlayerExpansion(settings, level, ipTier) || target > EXPANSION_WOTLK)
            return {};

        if (ipTier >= 0)
        {
            if (target == EXPANSION_TBC)
                return "unlock Burning Crusade progression (stage 8; normally complete Into the Breach after Naxxramas-40)";
            return "unlock Wrath progression (stage 13; normally defeat Kil'jaeden in Sunwell Plateau)";
        }

        unsigned const requiredLevel = unsigned(target == EXPANSION_TBC ? settings.classicMaxLevel : settings.tbcMaxLevel) + 1;
        return "reach level " + std::to_string(requiredLevel);
    }
}
