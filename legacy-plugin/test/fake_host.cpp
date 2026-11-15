// Stand-in loader for the Legacy of Terror plugin (run under Wine):
//
//   fake_host.exe <plugin dll> <image dump> <config toml> <mod dir> <mod name>
//
// Maps the whole game image dump as the "game", loads the real DLL the way D2RLoader does and
// checks: every engine function and patch site the plugin knows is found, the two call
// redirects land in the padding and give the right numbers, the server tick hook is taken, and
// a Diablo Clone asked for in the console sets the game's clone word and sends the message.
// Game routines that would need a real game (player count, the broadcast) are replaced by stand-ins
// after the plugin has checked them.
#include <D2RLPlugin/api.h>
#include <D2RLPlugin/lifecycle_events.h>
#include <D2RLPlugin/threads.h>

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

uint8_t*    g_image     = nullptr;
size_t      g_imageSize = 0;
std::string g_config;
int         g_failures = 0;
std::string g_log;

struct Hooked {
	uint64_t rva;
	void*    target;
};
std::vector<Hooked>              g_hooks;
D2RL::ConsoleCommandCallback     g_console = nullptr;
D2RL::Lifecycle::MonsterDeathCallback g_death = nullptr;
uint8_t                          g_packet[0x22] = {};
int                              g_broadcasts   = 0;
int                              g_originalTicks = 0;

void Check(bool ok, const char* what) {
	printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++g_failures;
}

bool ReadAll(const char* path, std::string& out) {
	FILE* f = fopen(path, "rb");
	if (f == nullptr) return false;
	fseek(f, 0, SEEK_END);
	out.resize(static_cast<size_t>(ftell(f)));
	fseek(f, 0, SEEK_SET);
	const size_t got = fread(out.data(), 1, out.size(), f);
	fclose(f);
	return got == out.size();
}

void __cdecl Log(const D2RL::PluginContext*, const char* message) noexcept {
	printf("  [plugin] %s\n", message);
	g_log += message;
	g_log += "\n";
}
bool __cdecl EnsureConfig(const D2RL::PluginContext*, const char*) noexcept { return true; }
bool __cdecl ReadConfig(const D2RL::PluginContext*, char* output, uint32_t outputSize, uint32_t* requiredSize) noexcept {
	if (requiredSize != nullptr) *requiredSize = static_cast<uint32_t>(g_config.size() + 1);
	if (output == nullptr || outputSize <= g_config.size()) return false;
	memcpy(output, g_config.c_str(), g_config.size() + 1);
	return true;
}
bool __cdecl WriteConfig(const D2RL::PluginContext*, const char* text) noexcept {
	g_config = text != nullptr ? text : "";
	return true;
}
bool Matches(uint64_t rva, const void* expected, uint32_t size) {
	return g_image != nullptr && rva + size <= g_imageSize && memcmp(g_image + rva, expected, size) == 0;
}
bool __cdecl PatchBytes(const D2RL::PluginContext*, uint64_t rva, const void* expected, uint32_t expectedSize, const void* bytes, uint32_t size) noexcept {
	if (!Matches(rva, expected, expectedSize) || rva + size > g_imageSize) return false;
	memcpy(g_image + rva, bytes, size);
	printf("  [host] patched %u bytes at 0x%llx\n", size, static_cast<unsigned long long>(rva));
	return true;
}
bool __cdecl PatchFill(const D2RL::PluginContext*, uint64_t, const void*, uint32_t, uint8_t, uint32_t) noexcept { return false; }
bool __cdecl PatchWrite(const D2RL::PluginContext*, uint64_t, const void*, uint32_t, uint64_t, uint32_t) noexcept { return false; }
bool __cdecl PatchRel32(const D2RL::PluginContext*, uint64_t, const void*, uint32_t, uint64_t, uint32_t, D2RL::Rel32PatchKind) noexcept { return false; }
bool __cdecl CheckExpectedBytes(const D2RL::PluginContext*, uint64_t rva, const void* expected, uint32_t size) noexcept { return Matches(rva, expected, size); }
bool __cdecl RegisterConsoleCommand(const D2RL::PluginContext*, const D2RL::ConsoleCommandRegistration* command) noexcept {
	if (command != nullptr) g_console = command->callback;
	return true;
}
void __cdecl WriteConsoleMessage(const D2RL::PluginContext*, const char* message, D2RL::ConsoleMessageKind) noexcept {
	printf("  [console] %s\n", message);
}

D2RL::Lifecycle::Result __cdecl RegisterDeath(const D2RL::PluginContext*, const D2RL::Lifecycle::MonsterDeathListener* listener, D2RL::Lifecycle::ListenerHandle* handle) noexcept {
	g_death = listener != nullptr ? listener->callback : nullptr;
	if (handle != nullptr) *handle = 1;
	return D2RL::Lifecycle::Result::Success;
}
D2RL::LifecycleService g_lifecycle {};
// UI-thread jobs are kept and run when the test says so
std::vector<std::pair<D2RL::Threads::Callback, void*>> g_uiJobs;
D2RL::Threads::Result __cdecl RunOnUi(const D2RL::PluginContext*, D2RL::Threads::Callback callback, void* user) noexcept {
	g_uiJobs.push_back({ callback, user });
	return D2RL::Threads::Result::Success;
}
D2RL::ThreadService g_threadService {};

D2RL::ServiceQueryResult __cdecl QueryService(const D2RL::PluginContext*, D2RL::ServiceId id, uint32_t, const void** service) noexcept {
	if (service == nullptr) return D2RL::ServiceQueryResult::UnknownService;
	*service = nullptr;
	if (id == D2RL::ServiceId::Lifecycle) {
		*service = &g_lifecycle;
		return D2RL::ServiceQueryResult::Success;
	}
	if (id == D2RL::ServiceId::Thread) {
		*service = &g_threadService;
		return D2RL::ServiceQueryResult::Success;
	}
	return D2RL::ServiceQueryResult::UnknownService;
}

uint64_t OriginalTick(void*) {
	++g_originalTicks;
	return 0x1234;
}

bool __cdecl InstallInlineHook(const D2RL::PluginContext*, const D2RL::InlineHookRegistration* hook) noexcept {
	if (hook == nullptr || hook->target == nullptr || !Matches(hook->rva, hook->expected, hook->expectedSize)) {
		printf("  [host] hook refused at 0x%llx (bytes do not match)\n", hook != nullptr ? static_cast<unsigned long long>(hook->rva) : 0ull);
		return false;
	}
	g_hooks.push_back({ hook->rva, hook->target });
	if (hook->original != nullptr) *hook->original = reinterpret_cast<void*>(&OriginalTick);
	printf("  [host] hook installed at 0x%llx\n", static_cast<unsigned long long>(hook->rva));
	return true;
}

// stand-ins for the game, put in place after the plugin's checks
int32_t FakeStat(void*, uint32_t stat, uint32_t) { return stat == 80 ? 50 : stat == 87 ? 10 : 0; }
int32_t FakePlayerCount(void*) { return 1; }
uint32_t g_fakePlayer[0x100] = { 0, 0, 7 }; // the server's copy: type 0, id 7
uint32_t g_clientCopy[0x100] = { 0, 0, 7 };  // the client's copy, in the client's unit table
int      g_setStats = 0, g_setValue = 0, g_clientSets = 0, g_clientValue = 0;
uint32_t g_setStat = 0, g_setLayer = 99;
void FakeSetStat(void* unit, uint32_t stat, int32_t value, uint16_t layer) {
	if (unit == g_clientCopy && stat == 80 && layer == 0) {
		++g_clientSets;
		g_clientValue = value;
	}
	if (unit == g_fakePlayer) {
		++g_setStats;
		g_setStat  = stat;
		g_setValue = value;
		g_setLayer = layer;
	}
}
using PlayerCb = void (*)(void*, void*, void*);
void FakeIterPlayers(void* game, PlayerCb callback, void* user) { callback(game, g_fakePlayer, user); }
void    FakeBroadcast(void*, const void* packet) {
	memcpy(g_packet, packet, sizeof(g_packet));
	++g_broadcasts;
}

// points a D2RCore stat thunk (ff 25 disp32) at a host function
void PointThunk(uint32_t rva, void* target) {
	const int32_t disp = *reinterpret_cast<int32_t*>(g_image + rva + 2);
	*reinterpret_cast<void**>(g_image + rva + 6 + disp) = target;
}
// replaces a routine by "movabs rax, target; jmp rax"
void Replace(uint32_t rva, void* target) {
	uint8_t code[12] = { 0x48, 0xb8 };
	memcpy(code + 2, &target, 8);
	code[10] = 0xff;
	code[11] = 0xe0;
	memcpy(g_image + rva, code, sizeof(code));
}

void Console(const char* args) {
	D2RL::ConsoleCommandContext command {};
	command.contextSize = D2RL::ConsoleCommandContextSize;
	command.name        = "terror";
	command.args        = args;
	command.argsLength  = static_cast<uint32_t>(strlen(args));
	command.input       = args;
	if (g_console != nullptr) g_console(nullptr, &command, nullptr);
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 6) {
		printf("usage: fake_host <dll> <dump> <toml> <mod dir> <mod name>\n");
		return 2;
	}
	std::string dump;
	if (!ReadAll(argv[2], dump) || !ReadAll(argv[3], g_config)) {
		printf("cannot read the dump or the config\n");
		return 2;
	}
	g_imageSize = dump.size();
	g_image     = static_cast<uint8_t*>(VirtualAlloc(nullptr, g_imageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
	memcpy(g_image, dump.data(), dump.size());
	dump.clear();
	dump.shrink_to_fit();

	g_lifecycle.serviceSize                  = D2RL::LifecycleServiceSize;
	g_lifecycle.serviceVersion               = D2RL::LifecycleService::AbiVersion;
	g_lifecycle.registerMonsterDeathListener = RegisterDeath;
	g_threadService.serviceSize    = D2RL::ThreadServiceSize;
	g_threadService.serviceVersion = D2RL::ThreadService::AbiVersion;
	g_threadService.runOnUiThread  = RunOnUi;

	static D2RL::PluginApi api {};
	api.apiSize                = D2RL::PluginApiSize;
	api.logInfo = api.logWarn = api.logError = Log;
	api.ensureConfig           = EnsureConfig;
	api.readConfig             = ReadConfig;
	api.writeConfig            = WriteConfig;
	api.patchBytes             = PatchBytes;
	api.patchFill              = PatchFill;
	api.patchWrite             = PatchWrite;
	api.patchRel32             = PatchRel32;
	api.checkExpectedBytes     = CheckExpectedBytes;
	api.registerConsoleCommand = RegisterConsoleCommand;
	api.writeConsoleMessage    = WriteConsoleMessage;
	api.installInlineHook      = InstallInlineHook;
	api.queryService           = QueryService;
	const std::string  mod    = argv[5];
	const std::wstring modDir = std::wstring(argv[4], argv[4] + strlen(argv[4]));
	static D2RL::PluginContext context {};
	context.contextSize  = static_cast<uint32_t>(sizeof(context));
	context.abiVersion   = D2RL_PLUGIN_ABI_VERSION;
	context.api          = &api;
	context.loadScope    = D2RL::LoadScope::Global;
	context.exeBase      = reinterpret_cast<uintptr_t>(g_image);
	context.pluginId     = "legacy-of-terror";
	context.activeMod    = mod.c_str();
	context.modDirectory = modDir.c_str();
	context.buildVersion = "stand-in";

	const HMODULE dll = LoadLibraryA(argv[1]);
	if (dll == nullptr) {
		printf("cannot load %s (error %lu)\n", argv[1], GetLastError());
		return 2;
	}
	const auto load = reinterpret_cast<D2RL::LoadPluginFn>(reinterpret_cast<void*>(GetProcAddress(dll, "D2RLoaderLoadPlugin")));
	Check(load != nullptr && load(&context), "the plugin loads");

	Check(g_log.find("ready: diablo clone on, cow portal on, hell bovines on, magic find on, vendors on (0 engine functions not found)") != std::string::npos,
		"every part is on and every engine function matches the dump");
	Check(g_log.find("pet type row 22 (Hell Bovine Minion)") != std::string::npos && g_log.find("Hell Bovine Minion is monster row") != std::string::npos
		&& g_log.find("plain Hell Bovine") == std::string::npos, "the mod's numbers are read from info.tsv (the minion row and its pet type)");
	Check(g_hooks.size() == 1 && g_hooks[0].rva == 0x488310, "the server tick hook is taken at 0x488310");
	Check(g_death != nullptr, "the monster death listener is registered");

	// the two redirects
	const auto callTarget = [](uint32_t site) -> uint64_t {
		if (g_image[site] != 0xe8) return 0;
		return site + 5 + static_cast<int64_t>(*reinterpret_cast<int32_t*>(g_image + site + 1));
	};
	Check(callTarget(0x44236c) == 0x2f5c60, "the drop code's Magic Find read is left as the game has it");
	Check(callTarget(0x36f22b) == 0x3ecd80 && g_image[0x3ecd80] == 0xff && g_image[0x3ecd81] == 0x25, "vendor discount read goes to the padding jump");
	Check(g_image[0x3ecd60] == 0xcc && g_image[0x3ecd8e] == 0xcc, "the jump stays inside its 14 bytes");

	PointThunk(0x2f5020, reinterpret_cast<void*>(&FakeStat));
	PointThunk(0x2f7d10, reinterpret_cast<void*>(&FakeSetStat));
	using StatFn = int32_t (*)(void*, uint32_t, uint32_t);
	const int32_t price = reinterpret_cast<StatFn>(g_image + 0x3ecd80)(nullptr, 87, 0);
	printf("  [host] reduced prices 10 -> %d\n", price);
	Check(price == 25, "a player with 10% reduced prices gets 25% in the price code");

	// the tick and the clone
	Replace(0x4251e0, reinterpret_cast<void*>(&FakePlayerCount));
	Replace(0x490d00, reinterpret_cast<void*>(&FakeIterPlayers));
	Replace(0x4f2390, reinterpret_cast<void*>(&FakeBroadcast));
	using TickFn = uint64_t (*)(void*);
	const TickFn tick = reinterpret_cast<TickFn>(g_hooks.empty() ? nullptr : g_hooks[0].target);
	std::vector<uint8_t> game(0x20000, 0);
	game[0x104] = 2; // Hell
	*reinterpret_cast<uint32_t*>(game.data() + 0x170) = 1;
	Check(tick != nullptr && tick(game.data()) == 0x1234 && g_originalTicks == 1, "the tick runs the game's own routine and passes its result on");
	*reinterpret_cast<uint32_t*>(game.data() + 0x170) = 25;
	tick(game.data()); // the first full second only notes the current six-hour window
	Check(g_setStats == 1 && g_setStat == 80 && g_setValue == 100 && g_setLayer == 0, "each player gets base Magic Find 100 (stat 80, base layer)");
	*reinterpret_cast<uint32_t*>(game.data() + 0x170) = 50;
	tick(game.data());
	Check(g_setStats == 1, "only once per game");
	*reinterpret_cast<void**>(g_image + 0x2a23910 + 8 * 5) = g_clientCopy; // the client's players, list 5
	for (auto& job : g_uiJobs) job.first(&context, job.second);
	g_uiJobs.clear();
	Check(g_clientSets == 1 && g_clientValue == 100, "the client's copy of the player gets the same 100 (for the character screen)");
	*reinterpret_cast<uint32_t*>(game.data() + 0x170) = 75;
	tick(game.data());
	for (auto& job : g_uiJobs) job.first(&context, job.second);
	g_uiJobs.clear();
	Check(g_clientSets == 1, "and is not written again every second");
	Check(*reinterpret_cast<int32_t*>(game.data() + 0x1a5d4) == 0 && g_broadcasts == 0, "no clone at start-up (only when a six-hour mark passes)");
	Console("clone");
	*reinterpret_cast<uint32_t*>(game.data() + 0x170) = 26;
	tick(game.data());
	Check(*reinterpret_cast<int32_t*>(game.data() + 0x1a5d4) == 1, "terror clone sets the game's clone word to 1 (pending)");
	Check(g_broadcasts == 1 && g_packet[0] == 0x5a && g_packet[1] == 0x12 && g_packet[2] == 0x04 && g_packet[3] == 0x05, "and sends the game's own Diablo Clone message (5a 12 04 05)");
	Console("clone");
	*reinterpret_cast<uint32_t*>(game.data() + 0x170) = 27;
	tick(game.data());
	Check(g_broadcasts == 1, "a game that already has him is left alone");
	std::vector<uint8_t> normal(0x20000, 0);
	*reinterpret_cast<uint32_t*>(normal.data() + 0x170) = 5;
	tick(normal.data());
	Console("clone");
	*reinterpret_cast<uint32_t*>(normal.data() + 0x170) = 6;
	tick(normal.data());
	Check(*reinterpret_cast<int32_t*>(normal.data() + 0x1a5d4) == 0 && g_broadcasts == 1, "not in Normal or Nightmare");
	Console("");

	printf("%s (%d failed)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES", g_failures);
	return g_failures == 0 ? 0 : 1;
}
