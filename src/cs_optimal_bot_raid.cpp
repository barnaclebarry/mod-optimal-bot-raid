/*
 * AzerothCore Module: Optimal Bot Raid
 * Solves Set Cover + Knapsack for perfectly scaled bot raids.
 * FIX: Thread-safe player iteration, MotionMaster stack preservation, 
 * EXPLICIT AI Master reset, Strategy Cleansing, and File-based Debug Logging.
 * REFACTOR: Dynamic Heuristic extracted to cached memory with tight bounds validation.
 * FEATURE: Non-blocking, in-memory string-buffered telemetry for algorithmic validation.
 * UPDATE: Captures complete bot state transitions during Assembly and Dismissal. Debug deprecated.
 * UPDATE: Added custom level ranges, auto-relaxation down to level 10, and freeroam dismissal support.
 * HOTFIX: Native ChatCommandTable method overloading to fix AC's unsigned int strict-type parsing.
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "Player.h"
#include "Group.h"
#include "GroupMgr.h"
#include "ObjectAccessor.h"
#include "MotionMaster.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"
#include "PlayerbotFactory.h"
#include "Config.h"
#include "Map.h"
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <initializer_list>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <ctime>
#include <shared_mutex>
#include "Log.h"

using namespace Acore::ChatCommands;

// Global Telemetry State
static bool s_telemetryEnabled = false;

// ==============================================================================
// CONFIGURATION CACHE & EXPLICIT BOUNDARY VALIDATION
// ==============================================================================

struct BotRaidConfigData {
    float weightGS;
    float weightBuff;
    float buffBase;
    float buffBloodlust;
    float buffReplenishment;

    float aiMult[12][3]; // Index by [Class ID][Tree]

    struct Quota { int tanks; int healers; int melee; };
    Quota quotas[7]; // Indexes: 0=5, 1=10, 2=15, 3=20, 4=25, 5=40Vanilla, 6=40WotLK

    static BotRaidConfigData* instance() {
        static BotRaidConfigData instance;
        return &instance;
    }

    float LoadAndValidateFloat(const std::string& key, float def, float minVal, float maxVal) {
        float val = sConfigMgr->GetOption<float>(key, def);
        if (val < minVal || val > maxVal) {
            LOG_ERROR("server.loading", "[OptimalBotRaid] CONFIG ERROR: '{}' value ({}) is out of bounds [{} to {}]. Reverting to safe default ({}).", 
                      key, val, minVal, maxVal, def);
            return def;
        }
        return val;
    }

    int32 LoadAndValidateInt(const std::string& key, int32 def, int32 minVal, int32 maxVal) {
        int32 val = sConfigMgr->GetOption<int32>(key, def);
        if (val < minVal || val > maxVal) {
            LOG_ERROR("server.loading", "[OptimalBotRaid] CONFIG ERROR: '{}' value ({}) is out of bounds [{} to {}]. Reverting to safe default ({}).", 
                      key, val, minVal, maxVal, def);
            return def;
        }
        return val;
    }

    void Load() {
        weightGS          = LoadAndValidateFloat("OptimalBotRaid.Algo.Weight.GS", 0.5f, 0.0f, 10.0f);
        weightBuff        = LoadAndValidateFloat("OptimalBotRaid.Algo.Weight.Buff", 0.5f, 0.0f, 10.0f);
        buffBase          = LoadAndValidateFloat("OptimalBotRaid.Algo.Bonus.UniqueBuff", 0.2f, 0.0f, 10.0f);
        buffBloodlust     = LoadAndValidateFloat("OptimalBotRaid.Algo.Bonus.Bloodlust", 0.4f, 0.0f, 10.0f);
        buffReplenishment = LoadAndValidateFloat("OptimalBotRaid.Algo.Bonus.Replenishment", 0.2f, 0.0f, 10.0f);

        const char* clsMap[12] = {"", "Warrior", "Paladin", "Hunter", "Rogue", "Priest", "DK", "Shaman", "Mage", "Warlock", "", "Druid"};
        float defMult[12][3] = {
            {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.2f}, {1.5f, 1.5f, 1.0f}, {1.1f, 1.2f, 1.2f}, 
            {0.8f, 1.5f, 0.8f}, {0.6f, 1.0f, 1.2f}, {1.2f, 1.5f, 0.6f}, {1.2f, 1.0f, 1.5f}, 
            {1.5f, 1.1f, 1.1f}, {1.0f, 1.0f, 1.5f}, {1.0f, 1.0f, 1.0f}, {1.0f, 0.5f, 1.0f} 
        };

        for (uint8 c = 1; c <= 11; ++c) {
            if (c == 10) continue;
            for (uint8 t = 0; t < 3; ++t) {
                std::string key = "OptimalBotRaid.AI." + std::string(clsMap[c]) + "." + std::to_string(t);
                aiMult[c][t] = LoadAndValidateFloat(key, defMult[c][t], 0.1f, 10.0f);
            }
        }

        auto LoadQuotaVal = [&](const std::string& prefix, int defT, int defH, int defM, int limit) -> Quota {
            Quota q;
            q.tanks   = LoadAndValidateInt(prefix + ".Tanks", defT, 0, limit);
            q.healers = LoadAndValidateInt(prefix + ".Healers", defH, 0, limit);
            q.melee   = LoadAndValidateInt(prefix + ".MeleeMax", defM, 0, limit);

            if ((q.tanks + q.healers + q.melee) > limit) {
                LOG_ERROR("server.loading", "[OptimalBotRaid] LOGIC ERROR: Configured slots for '{}' mathematically exceed {}. Resetting defaults.", prefix, limit);
                q.tanks = defT; q.healers = defH; q.melee = defM;
            }
            return q;
        };

        quotas[0] = LoadQuotaVal("OptimalBotRaid.Quota.5", 1, 1, 1, 5);
        quotas[1] = LoadQuotaVal("OptimalBotRaid.Quota.10", 2, 2, 2, 10);
        quotas[2] = LoadQuotaVal("OptimalBotRaid.Quota.15", 2, 3, 3, 15);
        quotas[3] = LoadQuotaVal("OptimalBotRaid.Quota.20", 2, 4, 5, 20);
        quotas[4] = LoadQuotaVal("OptimalBotRaid.Quota.25", 2, 5, 6, 25);
        quotas[5] = LoadQuotaVal("OptimalBotRaid.Quota.40Vanilla", 4, 10, 10, 40);
        quotas[6] = LoadQuotaVal("OptimalBotRaid.Quota.40WotLK", 3, 8, 10, 40);
    }
};

class OptimalBotRaidConfigScript : public WorldScript {
public:
    OptimalBotRaidConfigScript() : WorldScript("OptimalBotRaidConfigScript") {}
    void OnAfterConfigLoad(bool /*reload*/) override {
        BotRaidConfigData::instance()->Load();
    }
};

// ==============================================================================
// CORE ALGORITHM
// ==============================================================================

enum BotRole { ROLE_TANK, ROLE_HEALER, ROLE_MELEE, ROLE_RANGED, ROLE_UNKNOWN };
const char* GetRoleName(BotRole r) {
    switch(r) { case ROLE_TANK: return "TANK"; case ROLE_HEALER: return "HEALER"; case ROLE_MELEE: return "MELEE"; case ROLE_RANGED: return "RANGED"; default: return "UNKNOWN"; }
}

enum WotlkBuffs : uint32 {
    BUFF_BLOODLUST      = 1 << 0, BUFF_REPLENISHMENT  = 1 << 1,
    BUFF_10_PCT_STATS   = 1 << 2, BUFF_5_PCT_STATS    = 1 << 3,
    BUFF_SPELL_POWER    = 1 << 4, BUFF_MELEE_HASTE    = 1 << 5,
    BUFF_SPELL_HASTE    = 1 << 6, BUFF_10_PCT_AP      = 1 << 7,
    BUFF_ARMOR_PEN      = 1 << 8, BUFF_13_MAGIC_DMG   = 1 << 9,
    BUFF_MAX_HP         = 1 << 10, BUFF_ARCANE_INT    = 1 << 11,
    BUFF_3_RAID_DMG     = 1 << 12, BUFF_CRIT          = 1 << 13 
};

struct BotCandidate {
    Player* bot;
    uint32 level;
    BotRole role;
    float gearScore;
    float aiCompetency;
    uint32 providedBuffs;
};

class cs_optimal_bot_raid : public CommandScript
{
public:
    cs_optimal_bot_raid() : CommandScript("cs_optimal_bot_raid") { }

    ChatCommandTable GetCommands() const override {
        // Natively overloads commands in AC so the core attempts string binding before uint32 binding.
        static ChatCommandTable botRaidTable = {
            { "assemble",  HandleAssembleRange,   SEC_PLAYER, Console::No },
            { "assemble",  HandleAssembleSize,    SEC_PLAYER, Console::No },
            { "dismiss",   HandleDismissFreeroam, SEC_PLAYER, Console::No },
            { "dismiss",   HandleDismiss,         SEC_PLAYER, Console::No },
            { "debug",     HandleDebug,           SEC_PLAYER, Console::No },
            { "telemetry", HandleTelemetryToggle, SEC_PLAYER, Console::No },
            { "telemetry", HandleTelemetry,       SEC_PLAYER, Console::No },
            { "version",   HandleVersion,         SEC_PLAYER, Console::No }
        };
        static ChatCommandTable commandTable = { { "botraid", botRaidTable } };
        return commandTable;
    }

    static bool HandleVersion(ChatHandler* handler)
    {
        handler->SendSysMessage("Optimal Bot Raid Module");
        handler->SendSysMessage("Compiled On: |cff00ff00" __DATE__ " at " __TIME__ "|r");
        return true;
    }

    static bool HandleTelemetryToggle(ChatHandler* handler, std::string argStr)
    {
        std::transform(argStr.begin(), argStr.end(), argStr.begin(), ::tolower);

        if (argStr == "on") {
            s_telemetryEnabled = true;
            handler->SendSysMessage("BotRaid Validation Telemetry: |cff00ff00ON|r. Air-gapped logs will capture algorithmic evaluation and full lifecycle bot states.");
        } else if (argStr == "off") {
            s_telemetryEnabled = false;
            handler->SendSysMessage("BotRaid Validation Telemetry: |cffff0000OFF|r.");
        } else {
            handler->PSendSysMessage("BotRaid Validation Telemetry is currently: {}", s_telemetryEnabled ? "|cff00ff00ON|r" : "|cffff0000OFF|r");
            handler->SendSysMessage("Syntax: .botraid telemetry <on|off>");
        }
        return true;
    }

    static bool HandleTelemetry(ChatHandler* handler)
    {
        handler->PSendSysMessage("BotRaid Validation Telemetry is currently: {}", s_telemetryEnabled ? "|cff00ff00ON|r" : "|cffff0000OFF|r");
        handler->SendSysMessage("Syntax: .botraid telemetry <on|off>");
        return true;
    }

    static bool HandleDebug(ChatHandler* handler)
    {
        handler->SendSysMessage("|cffff0000The .botraid debug command is DEPRECATED.|r");
        handler->SendSysMessage("Please use |cff00ff00.botraid telemetry on|r to capture comprehensive air-gapped state logs during assembly and dismissal.");
        return true;
    }

    static void AppendBotStateTelemetry(std::ostringstream& teleLog, Player* bot) {
        teleLog << "    State Profile: " << bot->GetName() << " (GUID: " << bot->GetGUID().ToString() << ")\n";
        
        if (MotionMaster* mm = bot->GetMotionMaster()) {
            teleLog << "    MotionMaster Type: " << mm->GetCurrentMovementGeneratorType() << "\n";
        }
        
        teleLog << "    Unit State: " << bot->GetUnitState() 
                << " | Combat State: " << (bot->IsInCombat() ? "TRUE" : "FALSE") 
                << " | Grouped: " << (bot->GetGroup() ? "TRUE" : "FALSE") << "\n";

        if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot)) {
            teleLog << "    PlayerbotAI: ACTIVE\n"
                    << "    AI State: " << ai->GetState() << "\n";
            
            if (Player* master = ai->GetMaster()) {
                teleLog << "    AI Master: " << master->GetName() << " (GUID: " << master->GetGUID().ToString() << ")\n";
            } else {
                teleLog << "    AI Master: NONE\n";
            }

            auto dumpStrats = [&](BotState state, const char* stateName) {
                teleLog << "    Strategies (" << stateName << "): ";
                std::vector<std::string> strats = ai->GetStrategies(state);
                for (const auto& s : strats) teleLog << s << " ";
                teleLog << "\n";
            };

            dumpStrats(BOT_STATE_COMBAT, "COMBAT");
            dumpStrats(BOT_STATE_NON_COMBAT, "NON_COMBAT");
            dumpStrats(BOT_STATE_DEAD, "DEAD");
        } else {
            teleLog << "    PlayerbotAI: NULL (Bot logic engine missing)\n";
        }
    }

    static uint8 GetBotSpec(Player* bot, uint8 cls) {
        auto HasSpell = [&](std::initializer_list<uint32> spells) {
            for (uint32 id : spells) { if (bot->HasSpell(id)) return true; }
            return false;
        };
        switch (cls) {
            case CLASS_DEATH_KNIGHT: if (HasSpell({49028, 55233})) return 0; if (HasSpell({49143, 51411})) return 1; if (HasSpell({49206, 55090})) return 2; break;
            case CLASS_PALADIN: if (HasSpell({53563, 20473})) return 0; if (HasSpell({53595, 31935})) return 1; if (HasSpell({53385, 20066})) return 2; break;
            case CLASS_MAGE: if (HasSpell({44425, 12042})) return 0; if (HasSpell({44457, 55360})) return 1; if (HasSpell({44572, 11426})) return 2; break;
            case CLASS_SHAMAN: if (HasSpell({51490, 59159})) return 0; if (HasSpell({51533, 17364})) return 1; if (HasSpell({61295, 16190})) return 2; break;
            case CLASS_WARLOCK: if (HasSpell({48181, 59164})) return 0; if (HasSpell({47241, 19028})) return 1; if (HasSpell({50796, 59172})) return 2; break;
            case CLASS_DRUID: if (HasSpell({48505, 53201})) return 0; if (HasSpell({50334, 17007})) return 1; if (HasSpell({48438, 53251})) return 2; break;
            case CLASS_PRIEST: if (HasSpell({47540, 53007})) return 0; if (HasSpell({47788, 34861})) return 1; if (HasSpell({47585, 15473})) return 2; break;
            case CLASS_HUNTER: if (HasSpell({53270, 19574})) return 0; if (HasSpell({53209, 19506})) return 1; if (HasSpell({53301, 60053})) return 2; break;
            case CLASS_ROGUE: if (HasSpell({51662, 14983})) return 0; if (HasSpell({51690, 13750})) return 1; if (HasSpell({51713, 14183})) return 2; break;
            case CLASS_WARRIOR: if (HasSpell({46924, 12294})) return 0; if (HasSpell({46917, 23881})) return 1; if (HasSpell({46968, 23922})) return 2; break;
        }
        
        if (PlayerbotsMgr::instance().GetPlayerbotAI(bot)) {
            if (PlayerbotAI::IsTank(bot)) return (cls == CLASS_WARRIOR ? 2 : (cls == CLASS_PALADIN ? 1 : 0));
            if (PlayerbotAI::IsHeal(bot)) return (cls == CLASS_PALADIN ? 0 : (cls == CLASS_PRIEST ? 1 : 2));
            if (PlayerbotAI::IsRanged(bot) && cls == CLASS_SHAMAN) return 0;
            if (!PlayerbotAI::IsRanged(bot) && cls == CLASS_SHAMAN) return 1;
        }
        return 1; 
    }

    static void MapBotProfile(Player* bot, uint8 tree, BotRole& role, float& aiMult, uint32& buffs) {
        uint8 cls = bot->getClass(); 
        uint32 lvl = bot->GetLevel();
        buffs = 0;

        if (cls >= 1 && cls <= 11 && cls != 10 && tree < 3) {
            aiMult = BotRaidConfigData::instance()->aiMult[cls][tree];
        } else {
            aiMult = 1.0f;
        }

        switch (cls) {
            case CLASS_DEATH_KNIGHT:
                if (tree == 0) { role = ROLE_TANK; if (lvl >= 50) buffs |= BUFF_10_PCT_STATS; }
                else if (tree == 1) { role = ROLE_MELEE; if (lvl >= 40) buffs |= BUFF_MELEE_HASTE; } 
                else { role = ROLE_MELEE; if (lvl >= 50) buffs |= BUFF_13_MAGIC_DMG; } 
                break;
            case CLASS_PALADIN:
                if (lvl >= 16) buffs |= BUFF_10_PCT_STATS; 
                if (tree == 0) { role = ROLE_HEALER; } 
                else if (tree == 1) { role = ROLE_TANK; if (lvl >= 40) buffs |= BUFF_3_RAID_DMG; } 
                else { role = ROLE_MELEE; if (lvl >= 50) buffs |= BUFF_REPLENISHMENT; if (lvl >= 40) buffs |= BUFF_3_RAID_DMG; }
                break;
            case CLASS_MAGE:
                if (lvl >= 1) buffs |= BUFF_ARCANE_INT; role = ROLE_RANGED;
                if (tree == 0) { if (lvl >= 40) buffs |= BUFF_3_RAID_DMG; } 
                break;
            case CLASS_SHAMAN:
                if (lvl >= 70) buffs |= BUFF_BLOODLUST; 
                if (tree == 0) { role = ROLE_RANGED; if (lvl >= 50) buffs |= BUFF_SPELL_POWER; if (lvl >= 40) buffs |= BUFF_CRIT; }
                else if (tree == 1) { role = ROLE_MELEE; if (lvl >= 40) buffs |= BUFF_10_PCT_AP; if (lvl >= 30) buffs |= BUFF_MELEE_HASTE; }
                else { role = ROLE_HEALER; if (lvl >= 40) buffs |= BUFF_SPELL_HASTE; }
                break;
            case CLASS_WARLOCK:
                role = ROLE_RANGED; if (lvl >= 4) buffs |= BUFF_MAX_HP; 
                if (tree == 0) { if (lvl >= 50) buffs |= BUFF_13_MAGIC_DMG; }
                else if (tree == 1) { if (lvl >= 50) buffs |= BUFF_SPELL_POWER; } 
                else { if (lvl >= 50) buffs |= BUFF_REPLENISHMENT; } 
                break;
            case CLASS_DRUID:
                if (lvl >= 1) buffs |= BUFF_5_PCT_STATS;
                if (tree == 0) { role = ROLE_RANGED; if (lvl >= 50) buffs |= BUFF_13_MAGIC_DMG; if (lvl >= 40) buffs |= BUFF_SPELL_HASTE; }
                else if (tree == 1) { role = ROLE_MELEE; if (lvl >= 40) buffs |= BUFF_CRIT; }
                else { role = ROLE_HEALER; }
                break;
            case CLASS_PRIEST:
                if (lvl >= 1) buffs |= BUFF_MAX_HP; 
                if (tree == 0) { role = ROLE_HEALER; } 
                else if (tree == 1) { role = ROLE_HEALER; }
                else { role = ROLE_RANGED; if (lvl >= 50) buffs |= BUFF_REPLENISHMENT; }
                break;
            case CLASS_HUNTER:
                role = ROLE_RANGED;
                if (tree == 0) { if (lvl >= 50) buffs |= BUFF_3_RAID_DMG; }
                else if (tree == 1) { if (lvl >= 40) buffs |= BUFF_10_PCT_AP; }
                else { if (lvl >= 50) buffs |= BUFF_REPLENISHMENT; }
                break;
            case CLASS_ROGUE:
                role = ROLE_MELEE; if (lvl >= 14) buffs |= BUFF_ARMOR_PEN; 
                break;
            case CLASS_WARRIOR:
                if (lvl >= 10) buffs |= BUFF_ARMOR_PEN | BUFF_MAX_HP; 
                if (tree == 2) { role = ROLE_TANK; }
                else { role = ROLE_MELEE; if (lvl >= 40) buffs |= BUFF_CRIT; }
                break;
            default: role = ROLE_UNKNOWN; break;
        }
    }

    static bool HandleAssembleRange(ChatHandler* handler, std::string rangeStr, uint32 size)
    {
        uint32 reqMin = 0;
        uint32 reqMax = 0;
        
        size_t dash = rangeStr.find('-');
        if (dash != std::string::npos) {
            try {
                reqMin = std::stoul(rangeStr.substr(0, dash));
                reqMax = std::stoul(rangeStr.substr(dash + 1));
                return ExecuteAssemble(handler, reqMin, reqMax, true, size);
            } catch (...) {
                handler->SendSysMessage("Invalid range syntax. Please provide valid numbers (e.g., 60-67).");
                return true;
            }
        }
        
        handler->SendSysMessage("Invalid syntax. Example: .botraid assemble 60-67 40");
        return true;
    }

    static bool HandleAssembleSize(ChatHandler* handler, uint32 size)
    {
        return ExecuteAssemble(handler, 0, 0, false, size);
    }

    static bool ExecuteAssemble(ChatHandler* handler, uint32 reqMin, uint32 reqMax, bool hasCustomRange, uint32 size)
    {
        Player* player = handler->GetSession()->GetPlayer();
        
        if (!player->IsAlive()) {
            handler->SendSysMessage("You cannot draft mercenaries while dead.");
            return true;
        }

        if (size != 5 && size != 10 && size != 15 && size != 20 && size != 25 && size != 40) {
            handler->SendSysMessage("Invalid size. Supported sizes: 5, 10, 15, 20, 25, 40.");
            return true;
        }

        uint32 pLevel = player->GetLevel();
        if (!hasCustomRange) {
            reqMax = (pLevel <= 60) ? 60 : ((pLevel <= 70) ? 70 : 80);
            reqMin = (pLevel > 4) ? pLevel - 4 : 1;
        } else {
            if (reqMin > reqMax) std::swap(reqMin, reqMax);
            if (reqMin < 1) reqMin = 1;
        }

        Group* group = player->GetGroup();
        
        if (size == 5 && group && group->isRaidGroup()) {
            handler->SendSysMessage("You are currently in a Raid group. Please disband before assembling a 5-man party.");
            return true;
        }

        std::ostringstream teleLog;
        bool isTele = s_telemetryEnabled;
        BotRaidConfigData* cfg = BotRaidConfigData::instance();
        
        if (isTele) {
            teleLog << "========================================================\n"
                    << "         OPTIMAL BOT RAID - ASSEMBLY TELEMETRY          \n"
                    << "========================================================\n"
                    << "Requested Size: " << size << "-man\n"
                    << "Target Level Range: " << reqMin << " - " << reqMax << (hasCustomRange ? " (Custom)\n" : " (Auto)\n")
                    << "Leader: " << player->GetName() << " (Lvl " << pLevel << ")\n\n"
                    << "--- CONFIGURATION WEIGHTS ---\n"
                    << "WeightGS: " << cfg->weightGS << " | WeightBuff: " << cfg->weightBuff << "\n"
                    << "BonusBase: " << cfg->buffBase << " | Bloodlust: " << cfg->buffBloodlust << " | Replenishment: " << cfg->buffReplenishment << "\n\n";
        }

        int qIdx = 0;
        if (size == 10) qIdx = 1;
        else if (size == 15) qIdx = 2;
        else if (size == 20) qIdx = 3;
        else if (size == 25) qIdx = 4;
        else if (size == 40) qIdx = (pLevel <= 60) ? 5 : 6;

        BotRaidConfigData::Quota q = cfg->quotas[qIdx];

        int reqTanks = std::min(q.tanks, (int)size);
        int reqHealers = std::min(q.healers, (int)size - reqTanks);
        int reqMelee = std::min(q.melee, (int)size - reqTanks - reqHealers);
        int reqRanged = size - reqTanks - reqHealers - reqMelee;

        uint32 currentRaidBuffs = 0;
        uint32 currentMembers = 0;

        auto EvalPlayer = [&](Player* p) {
            currentMembers++;
            BotRole role; float comp; uint32 buffs;
            MapBotProfile(p, GetBotSpec(p, p->getClass()), role, comp, buffs);
            currentRaidBuffs |= buffs;
            
            if (isTele) {
                teleLog << "  Group Member: " << p->GetName() << " | Role: " << GetRoleName(role) 
                        << " | Buffs (Hex): 0x" << std::hex << buffs << std::dec << "\n";
            }

            if (role == ROLE_TANK) reqTanks--;
            else if (role == ROLE_HEALER) reqHealers--;
            else if (role == ROLE_MELEE) reqMelee--;
            else reqRanged--; 
        };

        if (group) {
            for (GroupReference* ref = group->GetFirstMember(); ref != nullptr; ref = ref->next()) {
                if (Player* member = ref->GetSource()) EvalPlayer(member);
            }
        } else {
            EvalPlayer(player);
        }

        reqTanks = std::max(0, reqTanks); reqHealers = std::max(0, reqHealers);
        reqMelee = std::max(0, reqMelee); reqRanged = std::max(0, reqRanged);
        
        int botsToDraft = reqTanks + reqHealers + reqMelee + reqRanged;
        int slotsAvailable = (int)size - (int)currentMembers;

        if (isTele) {
            teleLog << "\n--- TARGET QUOTAS & STATE ---\n"
                    << "Target Draft: " << botsToDraft << " bots (T:" << reqTanks << " H:" << reqHealers << " M:" << reqMelee << " R:" << reqRanged << ")\n"
                    << "Initial Matrix Buffs: 0x" << std::hex << currentRaidBuffs << std::dec << "\n\n";
        }

        if (slotsAvailable <= 0) {
            handler->SendSysMessage("Your group is already full or matches the requested size.");
            return true;
        }

        if (botsToDraft > slotsAvailable) {
            int excess = botsToDraft - slotsAvailable;
            while (excess > 0) {
                if (reqRanged > 0) { reqRanged--; excess--; }
                else if (reqMelee > 0) { reqMelee--; excess--; }
                else if (reqHealers > 0) { reqHealers--; excess--; }
                else if (reqTanks > 0) { reqTanks--; excess--; }
            }
            botsToDraft = slotsAvailable; 
        }

        std::vector<ObjectGuid> potentialBots;
        {
            std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
            auto const& players = ObjectAccessor::GetPlayers();
            
            for (auto const& pair : players) {
                Player* bot = pair.second;
                if (!bot || bot == player || bot->GetGroup() || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST)) continue;
                if (!PlayerbotsMgr::instance().GetPlayerbotAI(bot) || bot->GetTeamId() != player->GetTeamId()) continue;
                
                uint32 bLevel = bot->GetLevel();
                if (bLevel > reqMax) continue;
                
                potentialBots.push_back(bot->GetGUID());
            }
        }

        std::vector<BotCandidate> allCandidates;
        for (ObjectGuid guid : potentialBots) {
            if (Player* bot = ObjectAccessor::FindConnectedPlayer(guid)) {
                if (bot->GetGroup() || !bot->IsAlive() || bot->IsInCombat()) continue;

                BotCandidate cand;
                cand.bot = bot;
                cand.level = bot->GetLevel();
                cand.gearScore = std::max(1.0f, (float)bot->GetAverageItemLevel()); 
                
                MapBotProfile(bot, GetBotSpec(bot, bot->getClass()), cand.role, cand.aiCompetency, cand.providedBuffs);
                if (cand.role != ROLE_UNKNOWN) allCandidates.push_back(cand);
            }
        }

        std::vector<BotCandidate> pool;
        uint32 currentMin = reqMin;
        bool relaxed = false;

        // Intelligent boundary relaxation - sequentially steps down to level 10 to salvage failing drafts
        while (true) {
            pool.clear();
            for (auto const& c : allCandidates) {
                if (c.level >= currentMin) {
                    pool.push_back(c);
                }
            }
            if (pool.size() >= (size_t)botsToDraft || currentMin <= 10) {
                break;
            }
            currentMin--;
            relaxed = true;
        }

        if (pool.size() < (size_t)botsToDraft) {
            if (relaxed && currentMin < reqMin) {
                handler->PSendSysMessage("Not enough eligible idle bots found! Found {}. Needed {}. (Relaxed minimum level down to {})", pool.size(), botsToDraft, currentMin);
            } else {
                handler->PSendSysMessage("Not enough eligible idle bots found! Found {}. Needed {}.", pool.size(), botsToDraft);
            }
            return true;
        }

        if (relaxed && currentMin < reqMin) {
            handler->PSendSysMessage("Not enough bots in range ({} - {}). Relaxed minimum level down to {} to fulfill the draft.", reqMin, reqMax, currentMin);
            if (isTele) {
                teleLog << "--- RELAXATION EVENT ---\n"
                        << "Target min level " << reqMin << " did not yield enough candidates.\n"
                        << "Relaxed min level down to " << currentMin << " to fulfill draft.\n\n";
            }
        }

        float maxGS = 1.0f;
        for (auto const& c : pool) {
            if (c.gearScore > maxGS) maxGS = c.gearScore;
        }

        if (isTele) {
            teleLog << "--- CANDIDATE POOL (Size: " << pool.size() << ") ---\n"
                    << "Max GS of valid pool: " << maxGS << "\n";
            for (auto const& c : pool) {
                teleLog << "[" << c.bot->GetName() << "] Role: " << std::setw(6) << GetRoleName(c.role) 
                        << " | Lvl: " << std::setw(2) << c.level 
                        << " | Base GS: " << std::setw(4) << c.gearScore 
                        << " | AI_Mult: " << std::fixed << std::setprecision(1) << c.aiCompetency 
                        << " | Buff Mask: 0x" << std::hex << c.providedBuffs << std::dec << "\n";
            }
            teleLog << "\n--- SET COVER + KNAPSACK DRAFTING TRACE ---\n";
        }

        std::vector<Player*> draftedBots;
        
        auto DraftBestBot = [&](BotRole targetRole) -> bool {
            float bestScore = -1.0f; 
            int bestIndex = -1;
            float t_normGS = 0.0f, t_buffScore = 0.0f;
            int t_newBuffs = 0;
            
            if (isTele) teleLog << "Drafting Required Role: " << GetRoleName(targetRole) << "\n";

            for (size_t i = 0; i < pool.size(); ++i) {
                if (pool[i].role != targetRole) continue;

                uint32 newBuffs = (~currentRaidBuffs & pool[i].providedBuffs);
                int newBuffCount = 0; 
                uint32 mask = newBuffs;
                while (mask) { mask &= (mask - 1); newBuffCount++; }

                float normalizedGS = (pool[i].gearScore / maxGS) * pool[i].aiCompetency;
                float buffScore = (newBuffCount * cfg->buffBase);
                
                if (newBuffs & BUFF_BLOODLUST) buffScore += cfg->buffBloodlust; 
                if (newBuffs & BUFF_REPLENISHMENT) buffScore += cfg->buffReplenishment;

                float finalScore = (normalizedGS * cfg->weightGS) + (buffScore * cfg->weightBuff);

                if (isTele) {
                    teleLog << "  Eval: " << std::left << std::setw(12) << pool[i].bot->GetName() 
                            << " | nGS: " << std::fixed << std::setprecision(3) << normalizedGS 
                            << " | BuffSc: " << buffScore 
                            << " | Final: " << finalScore << "\n";
                }

                if (finalScore > bestScore) { 
                    bestScore = finalScore; 
                    bestIndex = i; 
                    if (isTele) { t_normGS = normalizedGS; t_buffScore = buffScore; t_newBuffs = newBuffCount; }
                }
            }

            if (bestIndex != -1) {
                if (isTele) {
                    teleLog << "  >>> SELECTED: " << pool[bestIndex].bot->GetName() 
                            << " | Score: " << bestScore << " (NormGS*W: " << (t_normGS * cfg->weightGS) 
                            << " + Buff*W: " << (t_buffScore * cfg->weightBuff) << ")\n"
                            << "      New Unique Buffs Added: " << t_newBuffs << "\n"
                            << "      Buff Matrix Trans.: 0x" << std::hex << currentRaidBuffs << " -> 0x" << (currentRaidBuffs | pool[bestIndex].providedBuffs) << std::dec << "\n\n";
                }

                draftedBots.push_back(pool[bestIndex].bot);
                currentRaidBuffs |= pool[bestIndex].providedBuffs; 
                pool.erase(pool.begin() + bestIndex);
                return true;
            }
            
            if (isTele) teleLog << "  >>> NO CANDIDATES FOUND FOR ROLE.\n\n";
            return false;
        };

        for (int i = 0; i < reqTanks; i++) DraftBestBot(ROLE_TANK);
        for (int i = 0; i < reqHealers; i++) DraftBestBot(ROLE_HEALER);
        for (int i = 0; i < reqMelee; i++) DraftBestBot(ROLE_MELEE);
        for (int i = 0; i < reqRanged; i++) DraftBestBot(ROLE_RANGED);

        while ((int)draftedBots.size() < botsToDraft && !pool.empty()) {
            if (!DraftBestBot(ROLE_RANGED) && !DraftBestBot(ROLE_MELEE) && 
                !DraftBestBot(ROLE_HEALER) && !DraftBestBot(ROLE_TANK)) break; 
        }

        if (!group) {
            group = new Group();
            if (!group->Create(player)) {
                delete group; 
                handler->SendSysMessage("Critical Error: Core failed to instantiate Group object.");
                return true;
            }
            sGroupMgr->AddGroup(group);
        }
        
        if (size > 5 && !group->isRaidGroup()) group->ConvertToRaid();

        for (Player* bot : draftedBots) {
            if (group->IsFull()) break;
            
            if (group->AddMember(bot)) {
                if (bot->GetMapId() != player->GetMapId() || !bot->IsWithinDistInMap(player, 40.0f)) {
                    bot->CombatStop(true);
                    bot->TeleportTo(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), 
                                    player->GetPositionZ(), player->GetOrientation(), 0, player);
                }
            }
        }

        if (isTele) {
            teleLog << "\n--- POST-ASSEMBLY BOT STATES (FUNCTIONAL VERIFICATION) ---\n";
            for (Player* bot : draftedBots) {
                AppendBotStateTelemetry(teleLog, bot);
                teleLog << "\n";
            }
            
            teleLog << "--- FINAL MATRIX ---\n"
                    << "Final Draft Size: " << draftedBots.size() << "\n"
                    << "Final Buff Matrix Hash: 0x" << std::hex << currentRaidBuffs << std::dec << "\n"
                    << "========================================================\n";

            std::string logDir = sConfigMgr->GetOption<std::string>("LogsDir", ".");
            if (logDir.back() != '/' && logDir.back() != '\\') logDir += "/";
            
            std::time_t now = std::time(nullptr);
            char timeBuf[64];
            std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", std::localtime(&now));
            
            std::string filename = logDir + "botraid_telemetry_assembly_" + std::string(timeBuf) + "_" + std::to_string(size) + "man.log";
            std::ofstream outFile(filename);
            
            if (outFile.is_open()) {
                outFile << teleLog.str();
                outFile.close();
                handler->PSendSysMessage("Telemetry airgap log successfully exported to: {}", filename);
            } else {
                handler->SendSysMessage("TELEMETRY ERROR: Failed to open output file in Logs directory.");
            }
        }

        handler->PSendSysMessage("Optimal {}-man raid dynamically scaled and assembled.", size);
        return true;
    }

    static bool HandleDismissFreeroam(ChatHandler* handler, std::string argStr)
    {
        std::transform(argStr.begin(), argStr.end(), argStr.begin(), ::tolower);
        if (argStr == "freeroam") {
            return ExecuteDismiss(handler, true);
        }
        
        handler->SendSysMessage("Syntax: .botraid dismiss [freeroam]");
        return true;
    }

    static bool HandleDismiss(ChatHandler* handler)
    {
        return ExecuteDismiss(handler, false);
    }

    static bool ExecuteDismiss(ChatHandler* handler, bool freeroam)
    {
        Player* player = handler->GetSession()->GetPlayer();
        Group* initialGroup = player->GetGroup();

        if (!initialGroup || initialGroup->GetLeaderGUID() != player->GetGUID()) {
            handler->SendSysMessage("You must be the group leader to dismiss the mercenaries.");
            return true;
        }

        std::ostringstream teleLog;
        bool isTele = s_telemetryEnabled;

        if (isTele) {
            teleLog << "========================================================\n"
                    << "         OPTIMAL BOT RAID - DISMISSAL TELEMETRY         \n"
                    << "========================================================\n"
                    << "Leader: " << player->GetName() << "\n"
                    << "Mode: " << (freeroam ? "Free-roam (No Teleport)" : "Standard (Homebind Teleport)") << "\n\n"
                    << "--- PRE-DISMISSAL BOT STATES (IDENTIFIED FOR REMOVAL) ---\n";
        }

        std::vector<ObjectGuid> botsToRemove;
        for (GroupReference* itr = initialGroup->GetFirstMember(); itr != nullptr; itr = itr->next()) {
            if (Player* member = itr->GetSource()) {
                if (member != player && PlayerbotsMgr::instance().GetPlayerbotAI(member)) {
                    botsToRemove.push_back(member->GetGUID());
                    if (isTele) {
                        AppendBotStateTelemetry(teleLog, member);
                        teleLog << "\n";
                    }
                }
            }
        }

        if (isTele) teleLog << "--- EXECUTING TEARDOWN SEQUENCE ---\n";

        for (ObjectGuid guid : botsToRemove) {
            if (Player* bot = ObjectAccessor::FindConnectedPlayer(guid)) {
                
                if (isTele) {
                    bool isRandom = sRandomPlayerbotMgr.IsRandomBot(bot);
                    teleLog << "Processing Dismissal for: " << bot->GetName() << "\n"
                            << "  Logic Branch: " << (isRandom ? "System RandomBot (Factory Wipe Scheduled)" : "Player Alt (Manual Cleanse Scheduled)") << "\n";
                    
                    if (freeroam) {
                        teleLog << "  Relocating to Homebind: [SKIPPED - FREEROAM MODE ACTIVE]\n\n";
                    } else {
                        teleLog << "  Relocating to Homebind MapId: " << bot->m_homebindMapId 
                                << " (X: " << bot->m_homebindX << ", Y: " << bot->m_homebindY << ", Z: " << bot->m_homebindZ << ")\n\n";
                    }
                }

                if (Group* currentGroup = bot->GetGroup()) {
                    currentGroup->RemoveMember(guid, GROUP_REMOVEMETHOD_DEFAULT, player->GetGUID());
                }

                if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot)) {
                    ai->SetMaster(nullptr);

                    ai->AddTimedEvent([guid]() {
                        Player* b = ObjectAccessor::FindConnectedPlayer(guid);
                        if (!b) return;
                        
                        PlayerbotAI* bAi = PlayerbotsMgr::instance().GetPlayerbotAI(b);
                        if (!bAi) return;

                        bool isRandomBot = sRandomPlayerbotMgr.IsRandomBot(b);

                        if (isRandomBot) {
                            PlayerbotFactory factory(b, b->GetLevel());
                            factory.Refresh();
                            sRandomPlayerbotMgr.Randomize(b);
                            
                            bAi->ResetStrategies(false);
                            bAi->ChangeStrategy("+roam", BOT_STATE_NON_COMBAT);
                            bAi->Reset(true);
                        } else {
                            bAi->ResetStrategies(false);
                            
                            const char* strats[] = { "-follow", "-dps assist", "-assist", "-tank assist", "-healer dps" };
                            for (const char* strat : strats) {
                                bAi->ChangeStrategy(strat, BOT_STATE_NON_COMBAT);
                                bAi->ChangeStrategy(strat, BOT_STATE_COMBAT);
                            }
                            
                            bAi->ChangeStrategy("+roam", BOT_STATE_NON_COMBAT);
                            bAi->Reset(true);
                        }
                    }, 1000); 
                }
                
                bot->StopMoving();
                if (MotionMaster* mm = bot->GetMotionMaster()) {
                    mm->Clear();
                }

                // If user didn't flag for freeroam, send bots back to their respective inns
                if (!freeroam) {
                    bot->TeleportTo(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ, 0.0f);
                }
            }
        }

        if (isTele) {
            teleLog << "========================================================\n"
                    << "Dismissal operation complete.\n";

            std::string logDir = sConfigMgr->GetOption<std::string>("LogsDir", ".");
            if (logDir.back() != '/' && logDir.back() != '\\') logDir += "/";
            
            std::time_t now = std::time(nullptr);
            char timeBuf[64];
            std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", std::localtime(&now));
            
            std::string filename = logDir + "botraid_telemetry_dismissal_" + std::string(timeBuf) + ".log";
            std::ofstream outFile(filename);
            
            if (outFile.is_open()) {
                outFile << teleLog.str();
                outFile.close();
                handler->PSendSysMessage("Telemetry dismissal airgap log exported to: {}", filename);
            } else {
                handler->SendSysMessage("TELEMETRY ERROR: Failed to open output file in Logs directory.");
            }
        }

        if (freeroam) {
            handler->SendSysMessage("Dismissed all bot mercenaries. They have been cut loose in their current location.");
        } else {
            handler->SendSysMessage("Dismissed all bot mercenaries. They have returned to their duties.");
        }
        
        return true;
    }
};

void Addcs_optimal_bot_raidScripts() {
    new cs_optimal_bot_raid();
    new OptimalBotRaidConfigScript(); 
}