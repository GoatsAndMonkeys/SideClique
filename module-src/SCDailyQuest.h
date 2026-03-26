#pragma once
// SCDailyQuest — Wastelad: LORD-style daily RPG for SideClique
//
// Based on the in-game meta game "Wastelad" from the Fallout universe.
// Daily seeded quest: same encounters for all clique members, scaled to level.
// 10 actions per day. Combat, shop, special events, and Chairman Cheng boss.

#include <cstdint>
#include <cstring>

#define DQ_MAX_ACTIONS     10
#define DQ_MAX_STIMPAKS    3
#define DQ_START_HP        30
#define DQ_HP_PER_LEVEL    10
#define DQ_START_ATK       5
#define DQ_ATK_PER_LEVEL   3
#define DQ_START_DEF       2
#define DQ_DEF_PER_LEVEL   2
#define DQ_MAX_LEVEL       20

// Special event types
#define DQ_EVENT_COMBAT    0  // regular monster
#define DQ_EVENT_CHENG     1  // Chairman Cheng (final boss, lvl 12+)
#define DQ_EVENT_STRANGER  2  // Mysterious Stranger (weapon upgrade)
#define DQ_EVENT_RAIDER    3  // Raider ambush (lose caps or fight)
#define DQ_EVENT_BOS       4  // Brotherhood patrol (heal/buff)
#define DQ_EVENT_VAULT_REP 5  // Vault-Tec Rep (riddle for XP)
#define DQ_EVENT_TAVERN    6  // Wasteland Tavern (gamble caps)
#define DQ_EVENT_ARENA     7  // Thunderdome (PvP challenge)

// ── Player (persistent, ~44 bytes) ────────────────────────────────────

struct DQPlayer {
    uint32_t nodeNum;
    char     name[12];
    uint8_t  level;
    uint16_t xp;
    uint16_t caps;        // Fallout currency
    uint8_t  weapon;      // tier 0-5
    uint8_t  armor;       // tier 0-5
    uint8_t  stimpaks;
    uint32_t lastPlayDay;
    uint8_t  actionsUsed;
    uint8_t  questHp;
    uint8_t  inCombat;
    uint8_t  monsterHp;
    uint8_t  monsterIdx;
    uint8_t  chengKills;  // how many times defeated Chairman Cheng
    uint8_t  eventType;   // current special event type
};

// ── Wasteland monsters ────────────────────────────────────────────────

struct DQMonster {
    const char *name;
    uint8_t baseHp;
    uint8_t baseAtk;
    uint16_t xpReward;
    uint16_t capsReward;
};

static const DQMonster DQ_MONSTERS[] = {
    {"Radroach",        8,  3,  10,  5},
    {"Feral Ghoul",    12,  5,  20, 10},
    {"Mole Rat Pack",  15,  6,  30, 15},
    {"Raider",         20,  8,  45, 25},
    {"Raider Veteran", 25, 10,  65, 35},
    {"Super Mutant",   35, 13,  90, 50},
    {"Assaultron",     30, 16, 120, 65},
    {"Sentry Bot",     45, 15, 150, 80},
    {"Deathclaw",      55, 20, 220,120},
    {"Mirelurk Queen", 60, 18, 280,150},
    {"Behemoth",       75, 22, 350,200},
    {"Cheng's Guard",  50, 25, 400,250},
};
static const uint8_t DQ_MONSTER_COUNT = 12;

// Chairman Cheng — final boss (separate from regular monsters)
static const DQMonster DQ_CHENG = {"Chairman Cheng", 100, 30, 1000, 500};

// ── Wasteland locations ───────────────────────────────────────────────

static const char *DQ_LOCATIONS[] = {
    "Vault 111 Ruins", "Concord Outskirts", "Lexington Ruins",
    "Cambridge Crater", "Diamond City Gate", "Goodneighbor Alley",
    "The Glowing Sea", "Quincy Ruins", "Nuka-World Gate",
    "Fort Hagen", "Corvega Factory", "Sentinel Site",
};
static const uint8_t DQ_LOCATION_COUNT = 12;

// ── Weapons & Armor (Fallout themed) ──────────────────────────────────

static const char *DQ_WEAPONS[] = {
    "Bare Fists", "Baseball Bat", "10mm Pistol",
    "Combat Shotgun", "Laser Rifle", "Fat Man"
};
static const char *DQ_ARMORS[] = {
    "Vault Suit", "Leather Armor", "Combat Armor",
    "Metal Armor", "Power Armor", "X-01 Power Armor"
};

// ── Vault-Tec Rep riddles ─────────────────────────────────────────────

struct DQRiddle {
    const char *question;
    char answer;  // single letter answer
    uint16_t xpReward;
};

static const DQRiddle DQ_RIDDLES[] = {
    {"War. War never changes.\nWhat Vault number was\nthe Lone Wanderer from?\n[A]101 [B]111 [C]76", 'a', 50},
    {"What company made\nthe Pip-Boy?\n[A]Vault-Tec [B]RobCo [C]Nuka", 'b', 50},
    {"What is the currency\nof the wasteland?\n[A]Credits [B]Coins [C]Caps", 'c', 50},
    {"What faction wears\nPower Armor & collects\ntech?\n[A]NCR [B]BoS [C]Minutemen", 'b', 60},
    {"What creature is the\nmost feared in the\nwasteland?\n[A]Radscorpion [B]Yao Guai [C]Deathclaw", 'c', 60},
    {"Nuka-Cola Quantum\nglows what color?\n[A]Green [B]Blue [C]Red", 'b', 40},
};
static const uint8_t DQ_RIDDLE_COUNT = 6;

// ── Tavern NPC quotes ─────────────────────────────────────────────────

static const char *DQ_TAVERN_QUOTES[] = {
    "\"Patrolling the Mojave\nalmost makes you wish\nfor a nuclear winter.\"",
    "\"Another settlement\nneeds your help.\"",
    "\"Do you have a Geiger\ncounter? Mine is in\nthe shop.\"",
    "\"They asked me how well\nI understood physics.\nI said I had a\ntheoretical degree.\"",
    "\"Truth is, the game\nwas rigged from\nthe start.\"",
    "\"I don't want to set\nthe world on fire...\"",
};
static const uint8_t DQ_TAVERN_QUOTE_COUNT = 6;

// ── XP thresholds ─────────────────────────────────────────────────────

static const uint16_t DQ_XP_THRESH[] = {
    0, 50, 120, 220, 360, 550, 800, 1100, 1500, 2000,
    2600, 3400, 4400, 5600, 7000, 8800, 11000, 13500, 16500, 20000
};

// ── Daily seed hash ───────────────────────────────────────────────────

static uint32_t dqDaySeed(uint32_t day) {
    uint32_t h = 0x811c9dc5;
    for (int i = 0; i < 4; i++) {
        h ^= (day >> (i * 8)) & 0xFF;
        h *= 0x01000193;
    }
    return h;
}

// ── Determine event type for an encounter ─────────────────────────────

static uint8_t dqGetEventType(uint32_t day, uint8_t encounterIdx, uint8_t playerLevel) {
    uint32_t seed = dqDaySeed(day) + encounterIdx * 0x9e3779b9;
    uint8_t roll = seed % 100;

    // Chairman Cheng: only if level 12+, 5% chance
    if (playerLevel >= 12 && roll < 5) return DQ_EVENT_CHENG;

    // Special events: ~25% chance total
    if (roll < 10) return DQ_EVENT_STRANGER;   // 5% Mysterious Stranger
    if (roll < 17) return DQ_EVENT_RAIDER;     // 7% Raider ambush
    if (roll < 22) return DQ_EVENT_BOS;        // 5% Brotherhood patrol
    if (roll < 27) return DQ_EVENT_VAULT_REP;  // 5% Vault-Tec Rep riddle
    if (roll < 30) return DQ_EVENT_TAVERN;     // 3% Wasteland Tavern

    return DQ_EVENT_COMBAT; // 70% regular combat
}

// ── Get scaled monster for encounter ──────────────────────────────────

static void dqGetMonster(uint32_t day, uint8_t encounterIdx, uint8_t playerLevel,
                         DQMonster &out, uint8_t &scaledHp, uint8_t &scaledAtk) {
    uint32_t seed = dqDaySeed(day) + encounterIdx * 0x9e3779b9;

    int targetIdx = (playerLevel / 2) + ((seed >> 16) % 3) - 1;
    if (targetIdx < 0) targetIdx = 0;
    if (targetIdx >= DQ_MONSTER_COUNT) targetIdx = DQ_MONSTER_COUNT - 1;

    out = DQ_MONSTERS[targetIdx];

    float scale = 0.8f + (playerLevel * 0.1f);
    if (scale > 2.5f) scale = 2.5f;
    scaledHp = (uint8_t)(out.baseHp * scale);
    scaledAtk = (uint8_t)(out.baseAtk * scale);
    if (scaledHp < 5) scaledHp = 5;
    if (scaledAtk < 2) scaledAtk = 2;
}

static const char *dqGetLocation(uint32_t day) {
    return DQ_LOCATIONS[dqDaySeed(day) % DQ_LOCATION_COUNT];
}

static uint8_t dqPlayerMaxHp(const DQPlayer &p) {
    return DQ_START_HP + p.level * DQ_HP_PER_LEVEL;
}

static uint8_t dqPlayerAtk(const DQPlayer &p) {
    return DQ_START_ATK + p.level * DQ_ATK_PER_LEVEL + p.weapon * 3;
}

static uint8_t dqPlayerDef(const DQPlayer &p) {
    return DQ_START_DEF + p.level * DQ_DEF_PER_LEVEL + p.armor * 2;
}

static bool dqCheckLevelUp(DQPlayer &p) {
    if (p.level >= DQ_MAX_LEVEL) return false;
    if (p.xp >= DQ_XP_THRESH[p.level + 1]) {
        p.level++;
        return true;
    }
    return false;
}
