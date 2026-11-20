// Legacy of Terror: an offline copy of Blizzard's Diablo 30th anniversary event (Nov 2026).
//
// Everything is done with the game's own code (D2R 3.4, image 0x6ab3782c / build 93847):
//  - Diablo Clone: the server keeps a per-game "clone pending" word at game+0x1a5d4
//    (0 none, 1 pending, 2 spawned). Battle.net's message makes the game set it to 1 in a Hell
//    game with players and send "Diablo has invaded Sanctuary" (0x405db5). The Super Unique
//    spawner (0x499c99) then puts monster 333 (diabloclone) in the next Super Unique's place.
//    This plugin does the same thing on the event's six-hour clock.
//  - Cow Portal: the game's own cow portal routine (0x5da2a0, the cube's "Cow Portal" output)
//    is the model: the plugin checks Baal on that difficulty for a player in the Rogue
//    Encampment and runs the routine's own last steps (free spot, portal object, game quest
//    flag 4/11 = one per game) at the camp fire instead of at the player. The game's code reads the "killed the Cow King" flag nowhere else than the Cow
//    King's own death, so there is no lockout to lift.
//  - Hell Bovine Minions: a revivable corpse is turned into a Hell Bovine pet of the killer
//    through the game's pet creation (0x4350e0, as the summon skills do), and removed the way
//    Revive removes the corpse it raises.
//  - Magic Find: every player gets a base Magic Find (stat 80, layer 0) at the start of each
//    game, so it shows on the character screen and goes through the game's diminishing returns
//    with the rest. Stat 80 is not a saved player stat, so nothing is written to the character.
//    The server does not send stat 80 to the client, so the client's copy of the player is
//    given the same value for the character screen.
//  - Vendors: every player gets a base "Reduces all Vendor Prices" (stat 87, layer 0) the same
//    way. The game's price routine (0x36f0c0) reads that stat for buying, gambling and repairs,
//    so the discount is the game's own, and it shows on the character screen.
//  The Hell Bovines' warp distance call is redirected through a 14-byte jump placed in unused
//  padding (int3 bytes) of the game's code; every patch checks the bytes it replaces first.

#include <D2RLPlugin/api.h>
#include <D2RLPlugin/data_tables.h>
#include <D2RLPlugin/lifecycle_events.h>
#include <D2RLPlugin/plugin_communication.h>
#include <D2RLPlugin/threads.h>

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <string>

namespace {

const D2RL::PluginContext* g_ctx  = nullptr;
uintptr_t                  g_base = 0;

void Log(const char* format, ...) {
	if (g_ctx == nullptr) {
		return;
	}
	char    text[512];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	g_ctx->LogInfo(text);
}

int Clamp(int value, int low, int high) {
	return value < low ? low : value > high ? high : value;
}

// ---------------------------------------------------------------- settings

struct Config {
	int cloneEnabled   = 1;
	int cloneFirstHour = 2;
	int cloneEvery     = 6;
	int cowEnabled     = 1;
	int cowOffsetX     = 4;  // from the camp fire, in the game's position units (sub-tiles)
	int cowOffsetY     = -4; // +x is down-right on screen, +y down-left: (4, -4) is to the right
	int bovineEnabled  = 1;
	int bovineChance   = 3;
	int bovineLife     = 60;
	int bovineBonus    = 200; // extra life, % (Revive's own +200%)
	int bovineRange    = 200; // how far a warp may leave them behind, % of the game's 40 steps
	int mfBonus        = 100;
	int discount       = 15;
};
Config g_config;

// what the hooks read from other threads
std::atomic<int> g_mfActive { 0 };
std::atomic<int> g_discountActive { 0 };
std::atomic<int> g_bovineRangeActive { 100 };

struct Setting {
	const char* section;
	const char* key;
	int*        target;
	bool        isBool;
	int         min;
	int         max;
	int         step;
	const char* label; // nullptr = a menu header when section is nullptr
	const char* note;  // no double quotes
};

const Setting kSettings[] = {
	{ nullptr, nullptr, nullptr, false, 0, 0, 0, "Diablo Clone", nullptr },
	{ "diablo_clone", "enabled", &g_config.cloneEnabled, true, 0, 1, 0, "Diablo Clone every few hours (Hell)", "He takes the place of the next Super Unique in a Hell game running at that time." },
	{ "diablo_clone", "first_hour_utc", &g_config.cloneFirstHour, false, 0, 23, 1, "First time of day (UTC hour)", "Default 2 = 02:00 UTC = 6 PM Pacific." },
	{ "diablo_clone", "every_hours", &g_config.cloneEvery, false, 1, 24, 1, "Hours between spawns", nullptr },
	{ nullptr, nullptr, nullptr, false, 0, 0, 0, "Cow Portal", nullptr },
	{ "cow_portal", "enabled", &g_config.cowEnabled, true, 0, 1, 0, "Cow Portal in the Rogue Encampment", "Opens beside the camp fire once Baal is dead on that difficulty." },
	{ "cow_portal", "offset_x", &g_config.cowOffsetX, false, -20, 20, 1, "Place from the camp fire: x", "Higher x moves it down-right on screen." },
	{ "cow_portal", "offset_y", &g_config.cowOffsetY, false, -20, 20, 1, "Place from the camp fire: y", "Higher y moves it down-left on screen. 4 and -4 is to the right of the fire." },
	{ nullptr, nullptr, nullptr, false, 0, 0, 0, "Hell Bovine Minions", nullptr },
	{ "hell_bovines", "enabled", &g_config.bovineEnabled, true, 0, 1, 0, "Slain monsters rise as Hell Bovines", "They fight for you." },
	{ "hell_bovines", "chance_percent", &g_config.bovineChance, false, 1, 5, 1, "Chance per kill (%)", nullptr },
	{ "hell_bovines", "lifetime_seconds", &g_config.bovineLife, false, 30, 60, 5, "Seconds before each falls", nullptr },
	{ "hell_bovines", "life_bonus_percent", &g_config.bovineBonus, false, 0, 400, 25, "Extra life (%)", "As a Necromancer's Revive: raised monsters get +200% life." },
	{ "hell_bovines", "range_percent", &g_config.bovineRange, false, 100, 1000, 25, "Distance they survive a warp (%)",
		"When you Teleport or take a portal, Hell Bovines farther than this from where you arrive fall. 100 = 40 steps, as for traps and hydras." },
	{ nullptr, nullptr, nullptr, false, 0, 0, 0, "Magic Find and vendors", nullptr },
	{ "magic_find", "bonus_percent", &g_config.mfBonus, false, 0, 250, 10, "Base Magic Find (%)", "Every character has this much Magic Find to begin with; it shows on the character screen." },
	{ "vendors", "discount_percent", &g_config.discount, false, 0, 99, 1, "Vendor and gambling discount (%)", "Every character has this much Reduces all Vendor Prices to begin with; it shows on the character screen. Selling is not affected." },
};

void SetSetting(const Setting& setting, int number) {
	*setting.target = setting.isBool ? (number != 0 ? 1 : 0) : Clamp(number, setting.min, setting.max);
}

void ApplyConfig() {
	g_mfActive.store(g_config.mfBonus);
	g_discountActive.store(g_config.discount);
	g_bovineRangeActive.store(g_config.bovineRange);
}

const Setting* FindSetting(const char* section, const char* key) {
	for (const Setting& setting : kSettings) {
		if (setting.section != nullptr && strcmp(setting.section, section) == 0 && strcmp(setting.key, key) == 0) {
			return &setting;
		}
	}
	return nullptr;
}

void ParseConfig(char* text) {
	char  section[64] = "";
	char* line        = text;
	while (*line != '\0') {
		char* end = line;
		while (*end != '\0' && *end != '\n') {
			++end;
		}
		const char saved = *end;
		*end             = '\0';
		while (*line == ' ' || *line == '\t' || static_cast<unsigned char>(*line) >= 0x80) {
			++line;
		}
		if (*line == '[') {
			const char* close = strchr(line, ']');
			if (close != nullptr && static_cast<size_t>(close - line - 1) < sizeof(section)) {
				memcpy(section, line + 1, close - line - 1);
				section[close - line - 1] = '\0';
			}
		} else if (*line != '#' && *line != '\0') {
			char* equals = strchr(line, '=');
			if (equals != nullptr) {
				char   key[64] = "";
				size_t length  = 0;
				for (const char* c = line; c < equals && length + 1 < sizeof(key); ++c) {
					if (*c != ' ' && *c != '\t') {
						key[length++] = *c;
					}
				}
				const char* value = equals + 1;
				while (*value == ' ' || *value == '\t') {
					++value;
				}
				const int number = strncmp(value, "true", 4) == 0 ? 1 : strncmp(value, "false", 5) == 0 ? 0 : atoi(value);
				if (const Setting* setting = FindSetting(section, key)) {
					SetSetting(*setting, number);
				}
			}
		}
		if (saved == '\0') {
			break;
		}
		line = end + 1;
	}
}

void LoadConfig() {
	static char text[16384];
	memset(text, 0, sizeof(text));
	if (!g_ctx->ReadConfig(text, sizeof(text) - 1)) {
		Log("config: could not read legacy-of-terror.toml - using the defaults");
	} else {
		ParseConfig(text);
	}
	ApplyConfig();
}

// ---------------------------------------------------------------- I Hate TOMLs menu
// Same service and layout as in Horrors in the Night (see that plugin for how it was found).

struct MenuRegistration {
	uint32_t    structSize;
	uint32_t    flags;
	const char* description;
	bool (*get)(const char* key, char* out, uint32_t outSize);
	bool (*set)(const char* key, const char* value, char* error, uint32_t errorSize);
	void* reserved;
};

char             g_menuText[8192];
MenuRegistration g_menuRegistration {};
bool             g_menuJoined = false;

const Setting* MenuFind(const char* key) {
	if (key == nullptr) {
		return nullptr;
	}
	for (const Setting& setting : kSettings) {
		if (setting.section == nullptr) {
			continue;
		}
		const size_t sectionLength = strlen(setting.section);
		if (strncmp(key, setting.section, sectionLength) == 0 && key[sectionLength] == '_' && strcmp(key + sectionLength + 1, setting.key) == 0) {
			return &setting;
		}
	}
	return nullptr;
}

bool MenuGet(const char* key, char* out, uint32_t outSize) {
	const Setting* setting = MenuFind(key);
	if (setting == nullptr || out == nullptr || outSize == 0) {
		return false;
	}
	snprintf(out, outSize, setting->isBool ? (*setting->target != 0 ? "true" : "false") : "%d", *setting->target);
	return true;
}

// Rewrites "key = value" inside [section] of the config text (adds it when missing).
std::string RewriteConfig(const std::string& text, const char* section, const char* key, const char* value) {
	std::string out;
	bool        inSection = false, done = false, seen = false;
	size_t      at        = 0;
	const std::string header = std::string("[") + section + "]";
	while (at < text.size()) {
		size_t end = text.find('\n', at);
		end        = end == std::string::npos ? text.size() : end + 1;
		std::string line = text.substr(at, end - at);
		at               = end;
		size_t first     = line.find_first_not_of(" \t");
		const std::string trimmed = first == std::string::npos ? "" : line.substr(first);
		if (!trimmed.empty() && trimmed[0] == '[') {
			if (inSection && !done) {
				out += std::string(key) + " = " + value + "\r\n";
				done = true;
			}
			inSection = trimmed.compare(0, header.size(), header) == 0;
			seen      = seen || inSection;
		} else if (inSection && !done && trimmed.compare(0, strlen(key), key) == 0) {
			const size_t after = trimmed.find_first_not_of(" \t", strlen(key));
			if (after != std::string::npos && trimmed[after] == '=') {
				out += std::string(key) + " = " + value + "\r\n";
				done = true;
				continue;
			}
		}
		out += line;
	}
	if (!done) {
		if (!out.empty() && out.back() != '\n') {
			out += "\r\n";
		}
		out += seen ? std::string(key) + " = " + value + "\r\n" : "\r\n" + header + "\r\n" + key + " = " + value + "\r\n";
	}
	return out;
}

bool MenuSet(const char* key, const char* value, char* error, uint32_t errorSize) {
	const Setting* setting = MenuFind(key);
	if (setting == nullptr || value == nullptr) {
		if (error != nullptr && errorSize > 0) {
			snprintf(error, errorSize, "unknown setting");
		}
		return false;
	}
	while (*value == ' ' || *value == '"') {
		++value;
	}
	SetSetting(*setting, setting->isBool ? ((strncmp(value, "true", 4) == 0 || value[0] == '1') ? 1 : 0) : atoi(value));
	ApplyConfig();
	char number[32];
	MenuGet(key, number, sizeof(number));
	static char text[16384];
	memset(text, 0, sizeof(text));
	if (g_ctx->ReadConfig(text, sizeof(text) - 1)) {
		static_cast<void>(g_ctx->WriteConfig(RewriteConfig(text, setting->section, setting->key, number).c_str()));
	}
	return true;
}

void JoinSettingsMenu() {
	if (g_menuJoined) {
		return;
	}
	const D2RL::PluginCommunicationService* communication = nullptr;
	if (g_ctx->QueryService(&communication) != D2RL::ServiceQueryResult::Success || communication == nullptr
		|| communication->serviceSize < offsetof(D2RL::PluginCommunicationService, releaseService) + sizeof(void*) || communication->acquireService == nullptr) {
		return;
	}
	D2RL::PluginCommunication::AcquireServiceRequest request {};
	request.structSize       = D2RL::PluginCommunication::AcquireServiceRequestSize;
	request.providerPluginId = "i-hate-tomls";
	request.name             = "settings-menu";
	request.minimumVersion   = 1;
	request.minimumTableSize = 0x30;
	D2RL::PluginCommunication::AcquiredService acquired {};
	acquired.structSize = D2RL::PluginCommunication::AcquiredServiceSize;
	if (communication->acquireService(g_ctx, &request, &acquired) != D2RL::PluginCommunication::Result::Success || acquired.table == nullptr) {
		return;
	}
	size_t used = static_cast<size_t>(snprintf(g_menuText, sizeof(g_menuText), "title = \"Legacy of Terror\"\n"));
	for (const Setting& setting : kSettings) {
		if (used > sizeof(g_menuText) - 600) {
			break;
		}
		used += static_cast<size_t>(snprintf(g_menuText + used, sizeof(g_menuText) - used, "[[setting]]\n"));
		if (setting.section == nullptr) {
			used += static_cast<size_t>(snprintf(g_menuText + used, sizeof(g_menuText) - used, "type = \"header\"\nlabel = \"%s\"\n", setting.label));
			continue;
		}
		used += static_cast<size_t>(snprintf(g_menuText + used, sizeof(g_menuText) - used, "key = \"%s_%s\"\ntype = \"%s\"\nlabel = \"%s\"\n", setting.section, setting.key,
			setting.isBool ? "bool" : "int", setting.label));
		if (setting.note != nullptr) {
			used += static_cast<size_t>(snprintf(g_menuText + used, sizeof(g_menuText) - used, "note = \"%s\"\n", setting.note));
		}
		if (!setting.isBool) {
			used += static_cast<size_t>(snprintf(g_menuText + used, sizeof(g_menuText) - used, "min = %d\nmax = %d\nstep = %d\nshift_step = %d\nctrl_step = %d\n", setting.min,
				setting.max, setting.step, setting.step * 5, setting.step * 10));
		}
	}
	g_menuRegistration.structSize  = 0x28;
	g_menuRegistration.description = g_menuText;
	g_menuRegistration.get         = MenuGet;
	g_menuRegistration.set         = MenuSet;
	using AddFn     = void* (*)(const D2RL::PluginContext* context, const MenuRegistration* registration);
	const AddFn add = *reinterpret_cast<const AddFn*>(static_cast<const char*>(acquired.table) + 8);
	g_menuJoined    = add != nullptr && add(g_ctx, &g_menuRegistration) != nullptr;
	Log("settings menu: %s", g_menuJoined ? "joined I Hate TOMLs" : "I Hate TOMLs refused the registration");
}

// ---------------------------------------------------------------- engine (server side)

using Unit = void;
using Room = void;
using Game = void;

struct PetArgs { // argument block of the pet creation routine 0x4350e0 (0x30 bytes)
	uint32_t flags;    // bit 0: place at x, y (else at the owner's target)
	uint32_t pad0;
	Unit*    owner;
	int16_t  monClass;
	int16_t  pad1;
	int32_t  unknown14;
	int32_t  mode;     // monster mode it appears in (1 = neutral)
	int32_t  x;
	int32_t  y;
	int32_t  petType;  // pettype.txt row
	int32_t  petMax;   // most pets of that type the owner may have
	int32_t  unknown2c;
};
static_assert(sizeof(PetArgs) == 0x30);
static_assert(offsetof(PetArgs, monClass) == 0x10 && offsetof(PetArgs, mode) == 0x18 && offsetof(PetArgs, petType) == 0x24);

using TickFn         = uint64_t (*)(Game* game);
using BroadcastFn    = void (*)(Game* game, const void* packet);
using PlayerCountFn  = int32_t (*)(Game* game);
using OpenCowFn      = int32_t (*)(Game* game, Unit* player);
using GameQuestGetFn = int32_t (*)(Game* game, int32_t quest, int32_t flag);
using PlayerDataFn   = char* (*)(Unit* player);
using QuestGetFn     = int32_t (*)(void* questFlags, int32_t quest, int32_t flag);
using PlayerCallback = void (*)(Game* game, Unit* unit, void* user);
using IterPlayersFn  = void (*)(Game* game, PlayerCallback callback, void* user);
using UnitRoomFn     = Room* (*)(Unit* unit);
using RoomLevelFn    = int32_t (*)(Room* room);
using FindUnitFn     = Unit* (*)(Game* game, uint32_t type, uint32_t id);
using UnitOwnerFn    = Unit* (*)(Unit* unit);
using CreatePetFn    = Unit* (*)(Game* game, PetArgs* args);
using PetToClientFn  = void (*)(void* client, Unit* pet);
using PlayerClientFn = void* (*)(Unit* player);
using PetFinishFn    = void (*)(Game* game, Unit* pet, int32_t zero, int32_t ownerValue);
using OwnerValueFn   = int32_t (*)(Unit* owner);
using CorpseOkFn     = int32_t (*)(Unit* corpse);
using CorpseOk2Fn    = int32_t (*)(Unit* corpse, int32_t one);
using UnitPosFn      = void (*)(Unit* unit, int32_t* xy);
using KillFn         = void (*)(Game* game, Unit* victim, Unit* killer, int32_t flag);
using RoomForgetFn   = void (*)(Room* room, uint32_t type, uint32_t id);
using RemoveUnitFn   = void (*)(Game* game, Unit* unit);
using StatFn         = int32_t (*)(Unit* unit, uint32_t stat, uint32_t layer);
using MaxLifeFn     = int32_t (*)(Unit* unit);
using SetStatFn      = void (*)(Unit* unit, uint32_t stat, int32_t value, uint16_t layer);
using NextInRoomFn   = Unit* (*)(Unit* unit);
using FindSpotFn     = int32_t (*)(Game* game, Room* room, int32_t* xy, int32_t radius, int32_t collision, Room** found, int32_t a, int32_t b);
using MakePortalFn   = int32_t (*)(Game* game, Unit* player, Room* room, int32_t x, int32_t y, int32_t level, void* zero, int32_t objectClass, int32_t c, int32_t d);
using GameQuestSetFn = void (*)(Game* game, int32_t quest, int32_t flag);
using PetRangeFn     = uint64_t (*)(Game* game, Unit* player, void* petList, int32_t distanceSquared);

struct EngineFunction {
	const char* name;
	uint32_t    rva;
	uint8_t     entry[12];
	uint8_t     size; // bytes of entry to check
	uint8_t     later[12]; // the 12 bytes at rva + 0x20, checked instead when the entry is another plugin's hook
};
constexpr uint32_t kLaterOffset = 0x20;

enum : int {
	kBroadcast, kPlayerCount, kOpenCow, kGameQuestGet, kPlayerData, kQuestGet, kIterPlayers, kUnitRoom, kRoomLevel, kFindUnit, kUnitOwner, kCreatePet,
	kPetToClient, kPlayerClient, kPetFinish, kOwnerValue, kCorpseOk, kCorpseOk2, kUnitPos, kKill, kRoomForget, kRemoveUnit, kGetStat,
	kNextInRoom, kFindSpot, kMakePortal, kGameQuestSet, kSetStat, kMaxLife, kPetRange, kFunctionCount
};

const EngineFunction kFunctions[kFunctionCount] = {
	{ "Broadcast", 0x4f2390, { 0x4c, 0x8b, 0xc2, 0x45, 0x33, 0xc9, 0x48, 0x8d, 0x15, 0x13, 0x00, 0x00 }, 12, { 0xe9, 0x5b, 0xe1, 0xf8, 0xff, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc } },
	{ "PlayerCount", 0x4251e0, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x4c, 0x8d, 0x44, 0x24, 0x30, 0xc7 }, 12, { 0x06, 0x00, 0x0f, 0xb6, 0x8b, 0x01, 0x01, 0x00, 0x00, 0x8b, 0x54, 0x24 } },
	{ "OpenCowPortal", 0x5da2a0, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x55, 0x56, 0x57, 0x48, 0x83, 0xec, 0x50 }, 12, { 0x85, 0xc0, 0x0f, 0x85, 0x48, 0x01, 0x00, 0x00, 0x80, 0xbb, 0x06, 0x01 } },
	{ "GameQuestGet", 0x514fd0, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48 }, 12, { 0x14, 0x48, 0x8d, 0x4c, 0x24, 0x30, 0xc6, 0x44, 0x24, 0x30, 0x00, 0xe8 } },
	{ "PlayerData", 0x34b240, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x85, 0xc9 }, 12, { 0xcc, 0x83, 0x3b, 0x00, 0x74, 0x14, 0x48, 0x8d, 0x4c, 0x24, 0x30, 0xc6 } },
	{ "QuestGet", 0x325c50, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48 }, 12, { 0x48, 0x8d, 0x4c, 0x24, 0x38, 0xe8, 0x96, 0xfb, 0xff, 0xff, 0x84, 0xc0 } },
	{ "IterPlayers", 0x490d00, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89 }, 12, { 0x8b, 0xe8, 0x48, 0x8b, 0xf2, 0x48, 0x8b, 0xf9, 0x48, 0x85, 0xc9, 0x74 } },
	{ "UnitRoom", 0x34b440, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x85, 0xc9 }, 12, { 0xcc, 0x8b, 0x0b, 0x83, 0xe9, 0x02, 0x74, 0x25, 0x83, 0xe9, 0x02, 0x74 } },
	{ "RoomLevel", 0x2efc10, { 0x48, 0x85, 0xc9, 0x75, 0x03, 0x33, 0xc0, 0xc3, 0x48, 0x8b, 0x49, 0x18 }, 12, { 0x48, 0x85, 0xc9, 0x75, 0x03, 0x33, 0xc0, 0xc3, 0x48, 0x8b, 0x49, 0x18 } },
	{ "FindUnit", 0x48fe80, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48 }, 12, { 0x48, 0x8d, 0x4c, 0x24, 0x38, 0xe8, 0x46, 0xd1, 0xff, 0xff, 0x84, 0xc0 } },
	{ "UnitOwner", 0x4a53c0, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8, 0x02, 0x66 }, 12, { 0xec, 0x65, 0xea, 0xff, 0x83, 0xf8, 0x01, 0x74, 0x19, 0x48, 0x8d, 0x4c } },
	{ "CreatePet", 0x4350e0, { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x6c, 0x24, 0x20, 0x56, 0x57 }, 12, { 0x8b, 0xe9, 0x74, 0x0c, 0x44, 0x8b, 0x72, 0x1c, 0x8b, 0x5a, 0x20, 0xe9 } },
	{ "PetToClient", 0x480ad0, { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x83, 0xec, 0x50 }, 12, { 0xe9, 0x48, 0x85, 0xd2, 0x75, 0x13, 0x48, 0x8d, 0x4c, 0x24, 0x20, 0x88 } },
	{ "PlayerClient", 0x48fde0, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x85, 0xc9 }, 12, { 0x8b, 0x80, 0xf0, 0x00, 0x00, 0x00, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3 } },
	{ "PetFinish", 0x58d950, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89 }, 12, { 0x48, 0x85, 0xc9, 0x74, 0x05, 0x48, 0x85, 0xd2, 0x75, 0x26, 0x48, 0x8d } },
	{ "OwnerValue", 0x34b8b0, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x85, 0xc9 }, 12, { 0xcc, 0x8b, 0x83, 0x30, 0x01, 0x00, 0x00, 0x48, 0x83, 0xc4, 0x20, 0x5b } },
	{ "CorpseRevivable", 0x3501e0, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x85, 0xc9 }, 12, { 0xcc, 0x33, 0xc0, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3, 0x83, 0x39, 0x01 } },
	{ "CorpseHasBody", 0x33a9a0, { 0x40, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xf9, 0xe8, 0x32, 0x52 }, 12, { 0xfc, 0x4d, 0x9c, 0x01, 0x48, 0x8b, 0xcf, 0xe8, 0x94, 0xee, 0x00, 0x00 } },
	{ "UnitPos", 0x34a040, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b }, 12, { 0xc5, 0xff, 0xff, 0x84, 0xc0, 0x74, 0x01, 0xcc, 0x48, 0x85, 0xff, 0x75 } },
	{ "Kill", 0x44db20, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89 }, 12, { 0xca, 0x49, 0x8b, 0xf8, 0x48, 0x8b, 0xda, 0x45, 0x32, 0xff, 0xe8, 0x81 } },
	{ "RoomForget", 0x2ef4d0, { 0x48, 0x85, 0xc9, 0x74, 0x69, 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89 }, 12, { 0x00, 0xe8, 0x8a, 0x17, 0x73, 0x00, 0xba, 0x10, 0x00, 0x00, 0x00, 0x48 } },
	{ "RemoveUnit", 0x48faa0, { 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b }, 12, { 0xde, 0xff, 0xff, 0x84, 0xc0, 0x74, 0x01, 0xcc, 0x48, 0x85, 0xff, 0x75 } },
	// stat reader: a jump thunk into D2RCore, only the opcode is checked
	{ "GetStat", 0x2f5020, { 0xff, 0x25 }, 2 },
	// what the cow portal routine does after it has picked the place (0x5da34f)
	{ "NextInRoom", 0x34b4a0, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x85, 0xc9 }, 12, { 0xcc, 0x48, 0x8b, 0x83, 0x60, 0x01, 0x00, 0x00, 0x48, 0x83, 0xc4, 0x20 } },
	{ "FindFreeSpot", 0x517f90, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55 }, 12, { 0xc4, 0x48, 0x89, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x8c } },
	{ "MakePortal", 0x432ce0, { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x68, 0x20, 0x56, 0x57, 0x41, 0x55, 0x41 }, 12, { 0x85, 0xc9, 0x75, 0x11, 0x88, 0x48, 0x08, 0x48, 0x8d, 0x48, 0x08, 0xe8 } },
	{ "GameQuestSet", 0x5181c0, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48 }, 12, { 0x14, 0x48, 0x8d, 0x4c, 0x24, 0x30, 0xc6, 0x44, 0x24, 0x30, 0x00, 0xe8 } },
	{ "SetStat", 0x2f7d10, { 0xff, 0x25 }, 2 },
	// a unit's full max life, read by the summon skills before they add their life bonus (0x55f35f)
	{ "MaxLife", 0x2f4d20, { 0xff, 0x25 }, 2 },
	// what the game does with a "range" pet type's pets when their owner warps (called at 0x500caf)
	{ "PetRange", 0x501920, { 0x40, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xec, 0x28, 0x49 }, 12, { 0x48, 0x89, 0x5c, 0x24, 0x50, 0x48, 0x89, 0x6c, 0x24, 0x58, 0x4c, 0x89 } },
};
bool g_functionOk[kFunctionCount] = {};

template <typename Fn>
Fn Engine(int index) {
	return reinterpret_cast<Fn>(g_base + kFunctions[index].rva);
}

bool Have(std::initializer_list<int> indexes) {
	for (int index : indexes) {
		if (!g_functionOk[index]) {
			return false;
		}
	}
	return true;
}

uint32_t UnitType(Unit* unit) { return *reinterpret_cast<uint32_t*>(unit); }
uint32_t UnitClass(Unit* unit) { return *reinterpret_cast<uint32_t*>(static_cast<char*>(unit) + 0x04); }
uint32_t UnitId(Unit* unit) { return *reinterpret_cast<uint32_t*>(static_cast<char*>(unit) + 0x08); }
uint32_t UnitMode(Unit* unit) { return *reinterpret_cast<uint32_t*>(static_cast<char*>(unit) + 0x0c); }

uint32_t GameFrame(Game* game) { return *reinterpret_cast<uint32_t*>(static_cast<char*>(game) + 0x170); }
uint8_t  GameDifficulty(Game* game) { return *reinterpret_cast<uint8_t*>(static_cast<char*>(game) + 0x104); }
uint8_t  GameVersion(Game* game) { return *reinterpret_cast<uint8_t*>(static_cast<char*>(game) + 0x106); }
int32_t& GameClone(Game* game) { return *reinterpret_cast<int32_t*>(static_cast<char*>(game) + 0x1a5d4); }

constexpr int kModeDeath = 0;
constexpr int kModeDead  = 12;
constexpr int kFps       = 25;

// ---------------------------------------------------------------- state (server thread)

uint64_t g_rng = 0x9e3779b97f4a7c15ull;
uint32_t Random() {
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 7;
	g_rng ^= g_rng << 17;
	return static_cast<uint32_t>(g_rng >> 16);
}

Game*    g_game      = nullptr; // the game the last tick ran for
uint32_t g_lastFrame = 0;
int64_t  g_cloneSlot = INT64_MIN; // the six-hour window last seen
bool     g_cowDone   = false;     // this game has its Cow Portal (or cannot get one)
int      g_cowSearches = 0;       // seconds spent in town without seeing the camp fire

// Requests from the console (UI thread), carried out on the next server tick.
std::atomic<int> g_requestClone { 0 };
std::atomic<int> g_requestCows { 0 };

// counters for the console
std::atomic<int> g_clonesTriggered { 0 };
std::atomic<int> g_cowPortals { 0 };
std::atomic<int> g_bovinesRaised { 0 };
std::atomic<int> g_bovinesAlive { 0 };
std::atomic<int> g_bovinesFellEarly { 0 }; // killed before their time
std::atomic<int> g_lastCloneResult { 0 }; // 0 none yet, 1 pending, 2 not Hell, 3 already, 4 no players
std::atomic<int> g_ticks { 0 };

// numbers from the D2RMM mod's info.tsv
int g_hellBovineClass = 391;  // the plain Hell Bovine (fallback)
bool g_minionFromMod  = false; // the mod's Hell Bovine Minion row (pet AI) is used
int g_bovinePetType   = 6; // falls back to "revive"
bool g_petTypeFromMod = false;

struct PendingRise {
	uint32_t corpseId;
	uint32_t ownerId;
	uint32_t since;
};
constexpr int kMaxPending = 32;
PendingRise   g_pending[kMaxPending];
int           g_pendingCount = 0;

struct Minion {
	uint32_t id;
	uint32_t until;
};
constexpr int kMaxMinions = 64;
Minion        g_minions[kMaxMinions];
int           g_minionCount = 0;

// The base stats every player is given in each game: Magic Find (stat 80) and "Reduces all
// Vendor Prices" (stat 87). For each: the players who have it in this game (and how much), and
// what the character screen's copy of the local player should show.
struct StatGiven {
	uint32_t id;
	int32_t  value;
};
constexpr int kMaxPlayers = 16;
struct BaseStat {
	uint32_t              stat;
	std::atomic<int>*     active;                      // the setting
	StatGiven             given[kMaxPlayers] {};
	int                   givenCount = 0;
	std::atomic<int32_t>  serverTotal { INT32_MIN };   // the server copy's total, gear included (INT32_MIN = not known)
	std::atomic<int32_t>  clientValue { 0 };           // the base the client's copy should have
	int32_t               askedValue = INT32_MIN;      // server thread: what the last look was for
};
BaseStat g_baseStats[2] = { { 80, &g_mfActive }, { 87, &g_discountActive } };

// The character screen's copy of the player (see "base stats" below) is looked at when a
// value changes and otherwise at most every 10 seconds.
constexpr uint32_t   kClientLookFrames = 10 * 25;
uint32_t             g_clientAskedId    = 0xffffffffu; // server thread: the player the last look was for...
uint32_t             g_clientNextLook   = 0;           // ...and the game frame of the next one


void ResetGameState(Game* game) {
	g_game         = game;
	g_cloneSlot    = INT64_MIN; // as online: only a game running when a mark passes gets him
	g_cowDone      = false;
	g_cowSearches  = 0;
	for (BaseStat& base : g_baseStats) {
		base.givenCount  = 0;
		base.serverTotal = INT32_MIN;
		base.askedValue  = INT32_MIN;
	}
	g_pendingCount = 0;
	g_minionCount  = 0;
	g_bovinesAlive = 0;
	g_clientAskedId  = 0xffffffffu; // the new game's player is looked at in its first second
	g_clientNextLook = 0;
}

// ---------------------------------------------------------------- Diablo Clone

int64_t CurrentSlot() {
	const int64_t now   = static_cast<int64_t>(time(nullptr));
	const int64_t every = static_cast<int64_t>(g_config.cloneEvery) * 3600;
	const int64_t shifted = now - static_cast<int64_t>(g_config.cloneFirstHour) * 3600;
	return shifted >= 0 ? shifted / every : (shifted - every + 1) / every;
}

// What the game does when Battle.net says the clone spawns (0x405db5): Hell, players in the
// game, nothing pending yet -> pending, and every client is told.
int TriggerClone(Game* game) {
	if (GameClone(game) != 0) {
		return 3;
	}
	if (GameDifficulty(game) < 2) {
		return 2;
	}
	if (Engine<PlayerCountFn>(kPlayerCount)(game) <= 0) {
		return 4;
	}
	GameClone(game) = 1;
	uint8_t packet[0x22] = {};
	packet[0] = 0x5a; // event message
	packet[1] = 0x12; // Diablo Clone
	packet[2] = 0x04;
	packet[3] = 0x05; // "Diablo has invaded Sanctuary!"
	Engine<BroadcastFn>(kBroadcast)(game, packet);
	++g_clonesTriggered;
	return 1;
}

void CloneTick(Game* game) {
	const int64_t slot = CurrentSlot();
	const bool    forced = g_requestClone.exchange(0) != 0;
	if (slot != g_cloneSlot || forced) {
		const bool first = g_cloneSlot == INT64_MIN;
		g_cloneSlot      = slot;
		if ((!first || forced) && (g_config.cloneEnabled != 0 || forced)) {
			const int result = TriggerClone(game);
			g_lastCloneResult = result;
			Log("diablo clone: %s - %s", forced ? "asked for in the console" : "the six-hour mark", result == 1 ? "he takes the next Super Unique's place (Diablo Walks the Earth)"
				: result == 2 ? "not a Hell game, nothing happens" : result == 3 ? "this game already had him" : "no players");
		}
	}
}

// ---------------------------------------------------------------- Cow Portal

struct CowSearch {
	int opened;
	int tried;
	int atFire;
};

constexpr uint32_t kFireClass = 39; // objects.txt RogueBonfire, the camp fire of the Rogue Encampment

// The camp fire, among the units of the player's room and the rooms next to it (the walk the
// game itself uses to look around a unit: room+0 list / room+0x40 count, room+0xa8 first unit).
Unit* FindFire(Unit* player) {
	Room* room = Engine<UnitRoomFn>(kUnitRoom)(player);
	if (room == nullptr) {
		return nullptr;
	}
	Room* const*   nearby = *reinterpret_cast<Room* const* const*>(room);
	const uint32_t count = *reinterpret_cast<const uint32_t*>(static_cast<const char*>(room) + 0x40);
	for (uint32_t i = 0; nearby != nullptr && i < count && i < 64; ++i) {
		if (nearby[i] == nullptr) {
			continue;
		}
		Unit* unit = *reinterpret_cast<Unit* const*>(static_cast<const char*>(nearby[i]) + 0xa8);
		for (int guard = 0; unit != nullptr && guard < 4096; ++guard) {
			if (UnitType(unit) == 2 && UnitClass(unit) == kFireClass) {
				return unit;
			}
			unit = Engine<NextInRoomFn>(kNextInRoom)(unit);
		}
	}
	return nullptr;
}

// The cow portal routine's own last steps (0x5da34f), with the camp fire's spot instead of the
// player's: the nearest free spot, the portal object (0x3c) to the Cow Level (39), and the
// game's "a Cow Portal was opened in this game" flag (quest 4, flag 11).
int OpenBesideFire(Game* game, Unit* player, Unit* fire) {
	int32_t xy[2] = {};
	Engine<UnitPosFn>(kUnitPos)(fire, xy);
	xy[0] += g_config.cowOffsetX;
	xy[1] += g_config.cowOffsetY;
	Room* found = nullptr;
	Engine<FindSpotFn>(kFindSpot)(game, Engine<UnitRoomFn>(kUnitRoom)(fire), xy, 3, 0x400, &found, 4, 100);
	if (found == nullptr) {
		return 0;
	}
	if (Engine<MakePortalFn>(kMakePortal)(game, player, found, xy[0], xy[1], 39, nullptr, 0x3c, 0, 0) == 0) {
		return 0;
	}
	Engine<GameQuestSetFn>(kGameQuestSet)(game, 4, 11);
	return 1;
}

void CowPlayer(Game* game, Unit* player, void* user) {
	auto* search = static_cast<CowSearch*>(user);
	if (search->opened != 0 || player == nullptr || UnitMode(player) == kModeDeath || UnitMode(player) == 17 /* dead */) {
		return;
	}
	Room* room = Engine<UnitRoomFn>(kUnitRoom)(player);
	if (room == nullptr || Engine<RoomLevelFn>(kRoomLevel)(room) != 1) { // Rogue Encampment
		return;
	}
	char* data = Engine<PlayerDataFn>(kPlayerData)(player);
	if (data == nullptr) {
		return;
	}
	void* quests = *reinterpret_cast<void**>(data + 0x40 + static_cast<size_t>(GameDifficulty(game)) * 8);
	const int bossQuest = GameVersion(game) == 1 ? 26 : 40; // Diablo (classic) / Baal (expansion)
	if (quests == nullptr || Engine<QuestGetFn>(kQuestGet)(quests, bossQuest, 0) == 0) {
		return; // not killed on this difficulty: the routine would refuse (and say so)
	}
	search->tried = 1;
	Unit* fire = FindFire(player);
	if (fire == nullptr && ++g_cowSearches < 10) {
		search->tried = 0; // the fire is not near yet: look again next second
		return;
	}
	search->opened = fire != nullptr ? OpenBesideFire(game, player, fire) : Engine<OpenCowFn>(kOpenCow)(game, player);
	search->atFire = fire != nullptr && search->opened != 0;
}

void CowTick(Game* game) {
	const bool forced = g_requestCows.exchange(0) != 0;
	if (forced) {
		g_cowDone = false;
	}
	if (g_cowDone || (g_config.cowEnabled == 0 && !forced)) {
		return;
	}
	if (Engine<GameQuestGetFn>(kGameQuestGet)(game, 4, 11) != 0) {
		g_cowDone = true; // a Cow Portal was already opened in this game (cube or plugin)
		return;
	}
	CowSearch search {};
	Engine<IterPlayersFn>(kIterPlayers)(game, CowPlayer, &search);
	if (search.opened != 0) {
		g_cowDone = true;
		++g_cowPortals;
		Log("cow portal: opened in the Rogue Encampment %s", search.atFire != 0 ? "beside the camp fire" : "next to you (the camp fire was not found)");
	} else if (search.tried != 0) {
		g_cowDone = true; // the game refused (no free spot); do not retry every second
		Log("cow portal: the game could not open it here (type terror cows to try again)");
	}
}

// ---------------------------------------------------------------- Hell Bovine Minions

bool SafeReadBytes(uintptr_t address, void* out, size_t size);
bool g_ownKill = false; // the plugin's own "time is up" kill is under way (see KillHook)

// The player a kill belongs to: the killer itself, or the owner of the pet, mercenary or
// missile that made it.
Unit* KillOwner(Game* game, const D2RL::Lifecycle::UnitIdentity& killer) {
	if (killer.type == D2RL::Lifecycle::UnitType::Invalid || killer.id == D2RL::Lifecycle::InvalidUnitId) {
		return nullptr;
	}
	Unit* unit = Engine<FindUnitFn>(kFindUnit)(game, static_cast<uint32_t>(killer.type), killer.id);
	for (int step = 0; unit != nullptr && step < 3; ++step) {
		if (UnitType(unit) == 0) {
			return unit;
		}
		unit = Engine<UnitOwnerFn>(kUnitOwner)(unit);
	}
	return nullptr;
}

void __cdecl OnMonsterDeath(const D2RL::PluginContext*, const D2RL::Lifecycle::MonsterDeathEvent* event, void*) noexcept {
	if (!D2RL::Lifecycle::HasMonsterDeathEventField(event, D2RL::Lifecycle::MonsterDeathEventRequiredSize) || g_config.bovineEnabled == 0 || g_game == nullptr
		|| !Have({ kFindUnit, kUnitOwner }) || g_pendingCount >= kMaxPending) {
		return;
	}
	if (Random() % 100 >= static_cast<uint32_t>(g_config.bovineChance)) {
		return;
	}
	Game* game    = g_game;
	Unit* monster = Engine<FindUnitFn>(kFindUnit)(game, 1, event->monster.id);
	if (monster == nullptr) {
		return;
	}
	if (Unit* owner = Engine<UnitOwnerFn>(kUnitOwner)(monster); owner != nullptr && UnitType(owner) == 0) {
		return; // a player's own pet or mercenary
	}
	Unit* player = KillOwner(game, event->killer);
	if (player == nullptr) {
		return;
	}
	g_pending[g_pendingCount++] = { event->monster.id, UnitId(player), GameFrame(game) };
}

bool RaiseBovine(Game* game, const PendingRise& rise) {
	Unit* corpse = Engine<FindUnitFn>(kFindUnit)(game, 1, rise.corpseId);
	Unit* owner  = Engine<FindUnitFn>(kFindUnit)(game, 0, rise.ownerId);
	if (corpse == nullptr || owner == nullptr) {
		return true; // gone: drop it
	}
	if (UnitMode(corpse) != kModeDead) {
		return GameFrame(game) - rise.since > 5 * kFps; // still falling; give up after 5 s
	}
	if (Engine<CorpseOkFn>(kCorpseOk)(corpse) == 0 || Engine<CorpseOk2Fn>(kCorpseOk2)(corpse, 1) == 0 || g_minionCount >= kMaxMinions) {
		return true; // not a corpse Revive could raise
	}
	int32_t xy[2] = {};
	Engine<UnitPosFn>(kUnitPos)(corpse, xy);

	PetArgs args {};
	args.flags    = 1;
	args.owner    = owner;
	args.monClass = static_cast<int16_t>(g_hellBovineClass);
	args.mode     = 1;
	args.x        = xy[0];
	args.y        = xy[1];
	args.petType  = g_bovinePetType;
	args.petMax   = kMaxMinions;
	Unit* pet = Engine<CreatePetFn>(kCreatePet)(game, &args);
	if (pet == nullptr) {
		return true;
	}
	// The life a summon skill adds (0x55f333): the skill's calc1 in percent on top of the
	// pet's max life, written to max life and life. For Revive calc1 is 200 (plus Skeleton
	// Mastery), so a revived monster has three times its life; without it the cows rose with
	// a plain Hell Bovine's life and fell in a few hits among many monsters.
	if (g_config.bovineBonus > 0 && Have({ kMaxLife, kSetStat })) {
		const int64_t base = Engine<MaxLifeFn>(kMaxLife)(pet);
		int64_t       life = base + base * g_config.bovineBonus / 100;
		life               = life > 0x7fffffff ? 0x7fffffff : life;
		if (base > 0) {
			Engine<SetStatFn>(kSetStat)(pet, 7, static_cast<int32_t>(life), 0);
			Engine<SetStatFn>(kSetStat)(pet, 6, static_cast<int32_t>(life), 0);
		}
	}
	// what the summon skills do next (0x55c3b1): tell the owner's client, finish the pet
	if (void* client = Engine<PlayerClientFn>(kPlayerClient)(owner)) {
		Engine<PetToClientFn>(kPetToClient)(client, pet);
	}
	Engine<PetFinishFn>(kPetFinish)(game, pet, 0, Engine<OwnerValueFn>(kOwnerValue)(owner));

	// the corpse goes, as with Revive (0x55c370)
	const uint32_t corpseId = UnitId(corpse);
	if (Room* room = Engine<UnitRoomFn>(kUnitRoom)(corpse)) {
		Engine<RoomForgetFn>(kRoomForget)(room, 1, corpseId);
	}
	Engine<RemoveUnitFn>(kRemoveUnit)(game, corpse);

	g_minions[g_minionCount++] = { UnitId(pet), GameFrame(game) + static_cast<uint32_t>(g_config.bovineLife) * kFps };
	++g_bovinesRaised;
	g_bovinesAlive = g_minionCount;
	return true;
}

void BovineTick(Game* game) {
	for (int i = 0; i < g_pendingCount;) {
		if (RaiseBovine(game, g_pending[i])) {
			g_pending[i] = g_pending[--g_pendingCount];
		} else {
			++i;
		}
	}
	const uint32_t frame = GameFrame(game);
	for (int i = 0; i < g_minionCount;) {
		Unit* unit = Engine<FindUnitFn>(kFindUnit)(game, 1, g_minions[i].id);
		bool  drop = unit == nullptr || UnitMode(unit) == kModeDeath || UnitMode(unit) == kModeDead;
		if (drop) {
			++g_bovinesFellEarly;
		}
		if (!drop && static_cast<int32_t>(frame - g_minions[i].until) >= 0) {
			g_ownKill = true;
			Engine<KillFn>(kKill)(game, unit, nullptr, 0); // they fall after their time
			g_ownKill = false;
			drop = true;
		}
		if (drop) {
			g_minions[i] = g_minions[--g_minionCount];
		} else {
			++i;
		}
	}
	g_bovinesAlive = g_minionCount;
}

// Why a Hell Bovine fell before its time: the game's kill routine (0x44db20) is watched, and when
// its victim is a Hell Bovine Minion that this plugin did not kill itself, one line goes to the
// log: its age, life, the killer, and the game code the kill came from (return addresses found
// on the stack, as offsets in the game image). Nothing is changed; the kill goes on as it was.
KillFn            g_originalKill = nullptr;
std::atomic<int>  g_killsLogged { 0 };
constexpr int     kMaxKillLines = 200;

void KillHook(Game* game, Unit* victim, Unit* killer, int32_t flag) {
	if (!g_ownKill && game != nullptr && victim != nullptr && g_minionFromMod && UnitType(victim) == 1 && UnitClass(victim) == static_cast<uint32_t>(g_hellBovineClass)
		&& g_killsLogged.load() < kMaxKillLines) {
		++g_killsLogged;
		const uint32_t id    = UnitId(victim);
		const uint32_t frame = GameFrame(game);
		int            age   = -1;
		for (int i = 0; i < g_minionCount; ++i) {
			if (g_minions[i].id == id) {
				age = static_cast<int>(frame - (g_minions[i].until - static_cast<uint32_t>(g_config.bovineLife) * kFps));
			}
		}
		const int32_t life    = g_functionOk[kGetStat] ? Engine<StatFn>(kGetStat)(victim, 6, 0) >> 8 : -1;
		const int32_t maxLife = g_functionOk[kMaxLife] ? Engine<MaxLifeFn>(kMaxLife)(victim) >> 8 : -1;
		char          from[400] = "";
		size_t        used      = 0;
		uintptr_t     stack[256] = {};
		const uintptr_t top = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
		if (SafeReadBytes(top, stack, sizeof(stack))) {
			int found = 0;
			for (uintptr_t value : stack) {
				if (value > g_base + 0x1000 && value < g_base + 0x1c00000 && found < 14 && used + 12 < sizeof(from)) {
					used += static_cast<size_t>(snprintf(from + used, sizeof(from) - used, " %llx", static_cast<unsigned long long>(value - g_base)));
					++found;
				}
			}
		}
		Log("hell bovine %u killed at frame %u, %s%d frames after rising (mode %u, life %d of %d, kill flag %d) by %s (type %u class %u id %u); from game code:%s", id, frame,
			age < 0 ? "not yet listed, " : "", age < 0 ? 0 : age, UnitMode(victim), life, maxLife, flag, killer == nullptr ? "no one" : "a unit", killer != nullptr ? UnitType(killer) : 0,
			killer != nullptr ? UnitClass(killer) : 0, killer != nullptr ? UnitId(killer) : 0, from);
	}
	g_originalKill(game, victim, killer, flag);
}

// When a player warps (Teleport, a waypoint, a portal, stairs) the game goes through the
// player's pets by pet type (0x500a40). For a "range" type such as the traps, the hydras and the
// Hell Bovine Minions it calls 0x501920(game, player, that type's pet list, 0x640) from 0x500caf:
// each pet whose squared distance from where the player arrived is over 0x640 (40 steps) is
// removed. That one call comes here: for the Hell Bovines' list the distance is the setting's,
// for every other list it is passed on as the game gave it.
// The list is {first node, ...}; a node holds the pet's unit id at +4 and the next node at +0x18.
uint64_t PetRangeHook(Game* game, Unit* player, void* petList, int32_t distanceSquared) {
	if (game != nullptr && petList != nullptr && g_minionFromMod && g_functionOk[kFindUnit]) {
		const char* node = *reinterpret_cast<const char* const*>(petList);
		if (node != nullptr) {
			Unit* pet = Engine<FindUnitFn>(kFindUnit)(game, 1, *reinterpret_cast<const uint32_t*>(node + 4));
			if (pet != nullptr && UnitClass(pet) == static_cast<uint32_t>(g_hellBovineClass)) {
				const int64_t steps = 40ll * g_bovineRangeActive.load(std::memory_order_relaxed) / 100;
				distanceSquared     = static_cast<int32_t>(steps * steps);
			}
		}
	}
	return Engine<PetRangeFn>(kPetRange)(game, player, petList, distanceSquared);
}

// ---------------------------------------------------------------- base stats (Magic Find, vendor prices)

// Every player gets a base Magic Find (stat 80) and a base "Reduces all Vendor Prices" (stat 87)
// at the start of each game, on the base layer, as if the character had them without gear: the
// drop code and the price code then read them as they read the gear's, and they show on the
// character screen. Neither stat is saved with a character, so nothing is written to the save.
// The character screen shows the client's copy of the player, and the server does not send
// these stats to it, so the client's copy gets the same base values (on the UI thread). The client
// keeps its players in a table of 128 lists at 0x2a23910 (next unit at +0x158), as found for
// Horrors in the Night; the copy has the same unit id as the server's.
// The game can make a new copy of the player (at the same place in memory, too), so the copy is
// not taken at its word once written: each look compares each stat, gear included, with the
// server copy's, and puts the base back when they differ.
const D2RL::ThreadService* g_threads = nullptr;
std::atomic<uint32_t>      g_clientMfId { 0xffffffffu };
std::atomic<bool>          g_clientJobQueued { false };
std::atomic<int>           g_clientWrites { 0 }; // times the copy was given its base, for the console
uintptr_t                  g_clientPlayer = 0;   // UI thread only

bool SafeReadBytes(uintptr_t address, void* out, size_t size) {
	SIZE_T done = 0;
	return address > 0x10000 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, size, &done) && done == size;
}

uintptr_t FindClientPlayer(uint32_t id) {
	uintptr_t buckets[128] = {};
	if (!SafeReadBytes(g_base + 0x2a23910, buckets, sizeof(buckets))) {
		return 0;
	}
	for (uintptr_t unit : buckets) {
		for (int guard = 0; unit != 0 && guard < 64; ++guard) {
			uint32_t header[3] = {};
			if (!SafeReadBytes(unit, header, sizeof(header)) || header[0] != 0) {
				break;
			}
			if (header[2] == id) {
				return unit;
			}
			if (!SafeReadBytes(unit + 0x158, &unit, sizeof(unit))) {
				break;
			}
		}
	}
	return 0;
}

void __cdecl ClientMfJob(const D2RL::PluginContext*, void*) noexcept {
	g_clientJobQueued = false;
	const uint32_t id = g_clientMfId.load();
	if (id == 0xffffffffu) {
		return;
	}
	uint32_t header[3] = {};
	if (g_clientPlayer == 0 || !SafeReadBytes(g_clientPlayer, header, sizeof(header)) || header[0] != 0 || header[2] != id) {
		g_clientPlayer = FindClientPlayer(id);
	}
	if (g_clientPlayer == 0) {
		return;
	}
	Unit* const client = reinterpret_cast<Unit*>(g_clientPlayer);
	for (BaseStat& base : g_baseStats) {
		const int32_t value = base.clientValue.load();
		const int32_t total = base.serverTotal.load();
		// Without the server's total to compare with, the base is written at every look.
		const bool known = total != INT32_MIN && g_functionOk[kGetStat];
		if (!known || Engine<StatFn>(kGetStat)(client, base.stat, 0) != total) {
			Engine<SetStatFn>(kSetStat)(client, base.stat, value, 0);
			++g_clientWrites;
		}
	}
}

// Server thread, once a second: a look at the client's copy when what it should show changed (a
// new game, a setting), else when the last look is 10 seconds old.
void QueueClientMf(uint32_t frame) {
	const uint32_t id = g_clientMfId.load();
	if (g_threads == nullptr || id == 0xffffffffu) {
		return;
	}
	bool changed = id != g_clientAskedId;
	for (BaseStat& base : g_baseStats) {
		changed = changed || base.clientValue.load() != base.askedValue;
	}
	if (!changed && static_cast<int32_t>(frame - g_clientNextLook) < 0) {
		return;
	}
	if (g_clientJobQueued.exchange(true)) {
		return; // the last look has not run yet
	}
	if (g_threads->runOnUiThread(g_ctx, ClientMfJob, nullptr) != D2RL::Threads::Result::Success) {
		g_clientJobQueued = false;
		return;
	}
	g_clientAskedId = id;
	for (BaseStat& base : g_baseStats) {
		base.askedValue = base.clientValue.load();
	}
	g_clientNextLook = frame + kClientLookFrames;
}

void MfPlayer(Game*, Unit* player, void*) {
	if (player == nullptr) {
		return;
	}
	const uint32_t id = UnitId(player);
	if (g_clientMfId.load() == 0xffffffffu || g_baseStats[0].givenCount == 0) {
		g_clientMfId = id; // the first player of the game is the one playing here
	}
	const bool local = g_clientMfId.load() == id;
	for (BaseStat& base : g_baseStats) {
		const int32_t value = base.active->load(std::memory_order_relaxed);
		if (local) {
			base.clientValue = value;
		}
		bool known = false;
		for (int i = 0; i < base.givenCount && !known; ++i) {
			if (base.given[i].id == id) {
				known = true;
				if (base.given[i].value != value) {
					Engine<SetStatFn>(kSetStat)(player, base.stat, value, 0);
					base.given[i].value = value;
				}
			}
		}
		if (!known && base.givenCount < kMaxPlayers) {
			Engine<SetStatFn>(kSetStat)(player, base.stat, value, 0);
			base.given[base.givenCount++] = { id, value };
		}
		// what the character screen should show: the server copy's total with the gear
		if (local && g_functionOk[kGetStat]) {
			base.serverTotal = Engine<StatFn>(kGetStat)(player, base.stat, 0);
		}
	}
}

// ---------------------------------------------------------------- server tick

TickFn g_originalTick = nullptr;
bool   g_cloneReady = false, g_cowReady = false, g_bovineReady = false, g_mfReady = false;

uint64_t TickHook(Game* game) {
	const uint64_t result = g_originalTick(game);
	if (game == nullptr) {
		return result;
	}
	const uint32_t frame = GameFrame(game);
	if (game != g_game || frame < g_lastFrame) {
		ResetGameState(game);
	}
	g_lastFrame = frame;
	++g_ticks;
	if (g_bovineReady && (g_pendingCount > 0 || (g_minionCount > 0 && frame % 5 == 0))) {
		BovineTick(game);
	}
	if (frame % kFps == 0 || g_requestClone.load() != 0 || g_requestCows.load() != 0) {
		if (g_cloneReady) {
			CloneTick(game);
		}
		if (g_cowReady) {
			CowTick(game);
		}
		if (g_mfReady) {
			Engine<IterPlayersFn>(kIterPlayers)(game, MfPlayer, nullptr);
			QueueClientMf(frame);
		}
	}
	return result;
}

// ---------------------------------------------------------------- redirected calls

// Replaces the 5-byte call at `site` with a call to a 14-byte absolute jump written into
// int3 padding at `cave`, which goes to `target`. Both places are checked first.
bool RedirectCall(const char* name, uint32_t site, const uint8_t (&siteBytes)[5], uint32_t cave, void* target) {
	uint8_t padding[16];
	memset(padding, 0xcc, sizeof(padding));
	uint8_t jump[14] = { 0xff, 0x25, 0, 0, 0, 0 };
	const uint64_t address = reinterpret_cast<uint64_t>(target);
	memcpy(jump + 6, &address, sizeof(address));
	if (!g_ctx->CheckExpectedBytes(site, siteBytes, sizeof(siteBytes)) || !g_ctx->CheckExpectedBytes(cave, padding, sizeof(padding))) {
		Log("%s: the game code is not the expected one - left off", name);
		return false;
	}
	if (!g_ctx->PatchBytes(cave, padding, sizeof(jump), jump, sizeof(jump))) {
		Log("%s: could not write the jump - left off", name);
		return false;
	}
	uint8_t call[5] = { 0xe8 };
	const int32_t relative = static_cast<int32_t>(static_cast<int64_t>(cave) - static_cast<int64_t>(site + 5));
	memcpy(call + 1, &relative, sizeof(relative));
	if (!g_ctx->PatchBytes(site, siteBytes, sizeof(siteBytes), call, sizeof(call))) {
		Log("%s: could not redirect the call - left off", name);
		return false;
	}
	return true;
}

// ---------------------------------------------------------------- the mod's numbers

std::wstring Widen(const char* text) {
	std::wstring out;
	for (; text != nullptr && *text != '\0'; ++text) {
		out += static_cast<wchar_t>(static_cast<unsigned char>(*text));
	}
	return out;
}

std::wstring GameDirectory() {
	wchar_t path[1024] = {};
	const DWORD length = GetModuleFileNameW(nullptr, path, 1000);
	if (length == 0 || length >= 1000) {
		return L".";
	}
	if (wchar_t* slash = wcsrchr(path, L'\\')) {
		*slash = L'\0';
	}
	return path;
}

bool ReadWholeFile(const std::wstring& file, std::string& text) {
	HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		return false;
	}
	char  buffer[4096];
	DWORD read = 0;
	text.clear();
	while (ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) && read > 0 && text.size() < 65536) {
		text.append(buffer, read);
	}
	CloseHandle(handle);
	return true;
}

// <game>\mods\<mod>\<mod>.mpq\data\global\excel\d2rloader\legacy-of-terror\info.tsv, written by the D2RMM mod.
void LoadModInfo() {
	const char* mod = g_ctx->activeMod;
	if (mod == nullptr || mod[0] == '\0') {
		Log("mod: no mod is active - Hell Bovines use the Revive pet type (install the Legacy of Terror D2RMM mod for their own)");
		return;
	}
	const std::wstring name = Widen(mod);
	std::wstring       bases[3];
	int                count = 0;
	if (g_ctx->modDirectory != nullptr && g_ctx->modDirectory[0] != L'\0') {
		std::wstring base = g_ctx->modDirectory;
		while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) {
			base.pop_back();
		}
		bases[count++] = base + L"\\" + name + L".mpq";
		bases[count++] = base;
	}
	bases[count++] = GameDirectory() + L"\\mods\\" + name + L"\\" + name + L".mpq";
	std::string text;
	for (int i = 0; i < count && text.empty(); ++i) {
		ReadWholeFile(bases[i] + L"\\data\\global\\excel\\d2rloader\\legacy-of-terror\\info.tsv", text);
	}
	if (text.empty()) {
		Log("mod: info.tsv not found in the mod '%s' - turn on the Legacy of Terror mod in D2RMM and Install; Hell Bovines use the Revive pet type", mod);
		return;
	}
	size_t at = 0;
	while (at < text.size()) {
		size_t end = text.find('\n', at);
		end        = end == std::string::npos ? text.size() : end;
		const std::string line = text.substr(at, end - at);
		at                     = end + 1;
		const size_t tab       = line.find('\t');
		if (tab == std::string::npos) {
			continue;
		}
		const std::string key   = line.substr(0, tab);
		const int         value = atoi(line.c_str() + tab + 1);
		if (key == "bovine_pettype" && value > 0) {
			g_bovinePetType  = value;
			g_petTypeFromMod = true;
		} else if (key == "revive_pettype" && value > 0 && !g_petTypeFromMod) {
			g_bovinePetType = value;
		} else if (key == "bovine_minion" && value > 0) {
			g_hellBovineClass = value;
			g_minionFromMod   = true;
		} else if (key == "hellbovine" && value > 0 && !g_minionFromMod) {
			g_hellBovineClass = value;
		}
	}
	Log("mod: Hell Bovine Minion is monster row %d%s, pet type row %d%s", g_hellBovineClass, g_minionFromMod ? "" : " (the plain Hell Bovine - it will not walk: reinstall the mod)", g_bovinePetType,
		g_petTypeFromMod ? " (Hell Bovine Minion)" : " (Revive - the mod's own pet type is missing)");
}

// ---------------------------------------------------------------- set-up

const uint8_t kTickEntry[16] = { 0x40, 0x55, 0x56, 0x48, 0x83, 0xec, 0x38, 0x48, 0x8b, 0xe9, 0x33, 0xf6, 0x0f, 0xb6, 0x89, 0x06 };
constexpr uint32_t kTickRva = 0x488310;
const uint8_t kKillEntry[16] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 }; // 0x44db20

// the clone code this plugin relies on, checked rather than assumed
const uint8_t kCloneSpawnCheck[10] = { 0x41, 0x83, 0xbe, 0xd4, 0xa5, 0x01, 0x00, 0x01, 0x75, 0x44 }; // 0x499c99: cmp [game+0x1a5d4],1
const uint8_t kRangeSite[5] = { 0xe8, 0x6c, 0x0c, 0x00, 0x00 }; // 0x500caf: call 0x501920 (range pets on a warp)
constexpr uint32_t kCaveRange = 0x3ecd60;

// A routine is the expected one when its first bytes match. Another plugin's hook replaces those
// bytes with a jump (and passes the call on to the routine), so when they are a jump the bytes
// 0x20 further in are compared instead: the same game, hooked, is still used.
bool IsJump(const uint8_t* code) {
	return code[0] == 0xe9 || code[0] == 0xeb || (code[0] == 0xff && code[1] == 0x25) || (code[0] == 0x48 && code[1] == 0xb8) || (code[0] == 0x49 && code[1] == 0xbb)
		|| (code[0] == 0x68) || (code[0] == 0x48 && code[1] == 0xff && code[2] == 0x25);
}

bool FunctionMatches(const EngineFunction& function, bool& hooked) {
	const uint8_t* code = reinterpret_cast<const uint8_t*>(g_base + function.rva);
	hooked              = false;
	if (memcmp(code, function.entry, function.size) == 0) {
		return true;
	}
	if (function.size == 12 && IsJump(code) && memcmp(code + kLaterOffset, function.later, sizeof(function.later)) == 0) {
		hooked = true;
		return true;
	}
	return false;
}

bool g_priceReady = false;
bool g_rangeReady = false;

void SetUp() {
	int missing = 0;
	for (int i = 0; i < kFunctionCount; ++i) {
		bool hooked     = false;
		g_functionOk[i] = FunctionMatches(kFunctions[i], hooked);
		if (!g_functionOk[i]) {
			Log("engine: %s (0x%x) is not the expected code", kFunctions[i].name, kFunctions[i].rva);
			++missing;
		} else if (hooked) {
			Log("engine: %s (0x%x) is hooked by another plugin - used through its hook", kFunctions[i].name, kFunctions[i].rva);
		}
	}
	const D2RL::PluginApi* api = g_ctx->GetApi();
	D2RL::InlineHookRegistration hook = D2RL::MakeInlineHook(kTickRva, kTickEntry, sizeof(kTickEntry), reinterpret_cast<void*>(&TickHook),
		reinterpret_cast<void**>(&g_originalTick));
	const bool tickOk = api != nullptr && D2RL::ApiInstallInlineHook(api) != nullptr && D2RL::ApiInstallInlineHook(api)(g_ctx, &hook) && g_originalTick != nullptr;
	if (!tickOk) {
		Log("server tick: hook failed - Diablo Clone, Cow Portal and Hell Bovines are off");
	}
	// the kill routine, watched for the log only (see KillHook); the Hell Bovines work without it
	if (tickOk && g_functionOk[kKill]) {
		D2RL::InlineHookRegistration killHook = D2RL::MakeInlineHook(kFunctions[kKill].rva, kKillEntry, sizeof(kKillEntry), reinterpret_cast<void*>(&KillHook),
			reinterpret_cast<void**>(&g_originalKill));
		if (!D2RL::ApiInstallInlineHook(api)(g_ctx, &killHook) || g_originalKill == nullptr) {
			g_originalKill = nullptr;
			Log("hell bovines: the kill routine could not be watched (no 'killed' lines in this log)");
		}
	}
	g_cloneReady  = tickOk && Have({ kBroadcast, kPlayerCount }) && g_ctx->CheckExpectedBytes(0x499c99, kCloneSpawnCheck, sizeof(kCloneSpawnCheck));
	g_cowReady    = tickOk && Have({ kOpenCow, kGameQuestGet, kPlayerData, kQuestGet, kIterPlayers, kUnitRoom, kRoomLevel, kNextInRoom, kFindSpot, kMakePortal, kGameQuestSet, kUnitPos });
	g_bovineReady = tickOk && Have({ kFindUnit, kUnitOwner, kCreatePet, kPetToClient, kPlayerClient, kPetFinish, kOwnerValue, kCorpseOk, kCorpseOk2, kUnitPos, kKill, kRoomForget, kRemoveUnit });

	if (g_bovineReady) {
		const D2RL::LifecycleService* lifecycle = nullptr;
		bool registered = false;
		if (g_ctx->QueryService(&lifecycle) == D2RL::ServiceQueryResult::Success && lifecycle != nullptr
			&& D2RL::HasLifecycleServiceField(lifecycle, D2RL::LifecycleServiceRequiredSize) && lifecycle->registerMonsterDeathListener != nullptr) {
			const D2RL::Lifecycle::MonsterDeathListener deaths {
				.structSize = D2RL::Lifecycle::MonsterDeathListenerSize,
				.flags      = 0,
				.callback   = OnMonsterDeath,
				.userData   = nullptr,
			};
			D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
			registered = lifecycle->registerMonsterDeathListener(g_ctx, &deaths, &handle) == D2RL::Lifecycle::Result::Success;
		}
		if (!registered) {
			Log("hell bovines: the loader's monster death event is not available - off");
			g_bovineReady = false;
		}
	}

	g_mfReady    = tickOk && Have({ kSetStat, kIterPlayers });
	if (g_ctx->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
		g_threads = nullptr;
		Log("magic find: the loader's UI thread service is missing - the base Magic Find works but may not show on the character screen");
	}
	g_priceReady = g_mfReady; // the vendor discount is a base stat, given with the Magic Find

	// only with the mod's own pet type: the fallback (Revive's) is a "warp" type and never gets here
	g_rangeReady = g_bovineReady && g_minionFromMod && Have({ kPetRange })
		&& RedirectCall("hell bovine range", 0x500caf, kRangeSite, kCaveRange, reinterpret_cast<void*>(&PetRangeHook));
	if (g_bovineReady && !g_rangeReady) {
		Log("hell bovines: the warp distance setting is off (they keep the game's 40 steps)");
	}

	Log("ready: diablo clone %s, cow portal %s, hell bovines %s, magic find %s, vendors %s (%d engine functions not found)", g_cloneReady ? "on" : "OFF", g_cowReady ? "on" : "OFF",
		g_bovineReady ? "on" : "OFF", g_mfReady ? "on" : "OFF", g_priceReady ? "on" : "OFF", missing);
}

D2RL::ConsoleCommandResult __cdecl OnConsole(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept {
	JoinSettingsMenu();
	char args[64] = "";
	if (command != nullptr && command->args != nullptr) {
		const size_t length = command->argsLength < sizeof(args) - 1 ? command->argsLength : sizeof(args) - 1;
		memcpy(args, command->args, length);
		args[length] = '\0';
	}
	const char* rest = args;
	while (*rest == ' ') {
		++rest;
	}
	char text[700];
	if (strncmp(rest, "clone", 5) == 0) {
		g_requestClone = 1;
		g_ctx->WriteConsoleMessage("Legacy of Terror: Diablo Clone asked for - in a Hell game he takes the next Super Unique's place.");
	} else if (strncmp(rest, "cows", 4) == 0) {
		g_requestCows = 1;
		g_ctx->WriteConsoleMessage("Legacy of Terror: trying the Cow Portal again (stand in the Rogue Encampment, Baal dead on this difficulty).");
	} else {
		const int64_t next   = (CurrentSlot() + 1) * static_cast<int64_t>(g_config.cloneEvery) * 3600 + static_cast<int64_t>(g_config.cloneFirstHour) * 3600;
		const int64_t wait   = next - static_cast<int64_t>(time(nullptr));
		const char*   clone  = g_lastCloneResult == 1 ? "pending/spawned" : g_lastCloneResult == 2 ? "last mark: not a Hell game" : g_lastCloneResult == 3 ? "last mark: already had him"
			: g_lastCloneResult == 4 ? "last mark: no players" : "none yet";
		snprintf(text, sizeof(text),
			"Legacy of Terror: diablo clone %s (next in %lldh %02lldm, %s, %d so far), cow portal %s (%d opened), hell bovines %s (%d%%, %ds, +%d%% life, warp distance %d%%%s, %d raised, %d up, %d slain early), base magic find %s (%d%%, character screen set %dx), vendors %s (-%d%%). Commands: terror clone | cows",
			g_cloneReady && g_config.cloneEnabled ? "on" : "off", static_cast<long long>(wait / 3600), static_cast<long long>((wait / 60) % 60), clone, g_clonesTriggered.load(),
			g_cowReady && g_config.cowEnabled ? "on" : "off", g_cowPortals.load(), g_bovineReady && g_config.bovineEnabled ? "on" : "off", g_config.bovineChance, g_config.bovineLife,
			g_config.bovineBonus, g_config.bovineRange, g_rangeReady ? "" : " (off)", g_bovinesRaised.load(), g_bovinesAlive.load(), g_bovinesFellEarly.load(), g_mfReady ? "on" : "off", g_mfActive.load(), g_clientWrites.load(), g_priceReady ? "on" : "off", g_discountActive.load());
		g_ctx->WriteConsoleMessage(text);
	}
	return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo kPluginInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "legacy-of-terror",
	.name        = "Legacy of Terror",
	.version     = "1.0.3",
	.author      = "Latedozer",
	.description = "Offline copy of the Diablo 30th anniversary event: Diablo Clone every six hours in Hell, a Cow Portal in the Rogue Encampment, +100% Magic Find, 15% off at vendors, and Hell Bovine Minions.",
	.flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
	.reserved    = {},
};

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &kPluginInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr || context->GetApi() == nullptr || context->exeBase == 0) {
		return false;
	}
	g_ctx  = context;
	g_base = context->exeBase;
	LARGE_INTEGER counter {};
	QueryPerformanceCounter(&counter);
	g_rng ^= static_cast<uint64_t>(counter.QuadPart) * 0x2545f4914f6cdd1dull;
	if (g_rng == 0) {
		g_rng = 1;
	}
	Log("Legacy of Terror %s loading (D2R %s, SDK %s)", kPluginInfo.version, context->buildVersion != nullptr ? context->buildVersion : "?", D2RL_SDK_VERSION);
	LoadConfig();
	LoadModInfo();
	SetUp();
	static_cast<void>(context->RegisterConsoleCommand("terror", OnConsole, "Legacy of Terror: status, clone, cows"));
	JoinSettingsMenu();
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	g_discountActive = 0;
}
