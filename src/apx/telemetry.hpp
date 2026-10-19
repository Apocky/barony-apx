// apx/telemetry.hpp -- BF1 instruments: JSONL telemetry for floors and encounters (barony-apx).
// Specs: Apockalypsis specs/barony-fork/04 section 8; card handovers/tasks/BF1-instruments-card.md.
//
// Master knob: APX_TELEM (env). "0" forces off. Otherwise telemetry is on when -apx-out=<dir> is given
// (or APX_TELEM=1, which writes to ./apx-out). With every knob off the game takes no instrument path.
// Authority: only a non-client process writes (multiplayer != CLIENT).
//
// ONE DEFINITION OF THE ROW SCHEMAS (all rows are one ASCII JSON object per line, flushed per row):
//
//   common (every row): run_id, build_hash, seed, floor, secret, depth, party_n, party_power (0 until
//     F5/F6), knob_set, tick, sim_seconds, wall_ms
//
//   floors.jsonl: floor_hash (BF1-2), rooms, monsters, gold, items, decorations, map_w, map_h,
//     wall_tiles, floor_tiles, water_tiles, lava_tiles, door_count, chest_count, monsters_by_type{},
//     gen_ms, load_ms, path_maps_ms, ladder_dist_tiles (BFS, -1 = no player/ladder yet),
//     spawn_to_nearest_monster_tiles, dropped_rows, bots_stuck, shots_failed
//
//   encounters.jsonl (BF1-3), ticks.jsonl (optional): see card; they use the same ApxRow writer.
//
// Counters carried by every row: dropped_rows (writes that failed), bots_stuck, shots_failed.
#pragma once
#include <cstdint>
#include <SDL.h>
#include <string>
#include <vector>

struct ApxRow
{
	std::string s;
	bool first = true;
	ApxRow() { s = "{"; }
	ApxRow& i(const char* k, long long v);
	ApxRow& d(const char* k, double v);          // %.3f
	ApxRow& str(const char* k, const std::string& v);
	ApxRow& hex64(const char* k, uint64_t v);    // 16 hex digits
	ApxRow& raw(const char* k, const std::string& json);
	ApxRow& intList(const char* k, const std::vector<int>& v);
	std::string done();                          // closes the object
private:
	void key(const char* k);
};

class Entity;
// Damage source kinds, set by producers via ApxDmgScope so Entity::setHP can attribute each HP delta (BF1-3).
enum ApxSrc { SRC_UNKNOWN = 0, SRC_MELEE, SRC_ARROW, SRC_THROWN, SRC_MAGIC, SRC_DOT, SRC_ENV, SRC_EXPLOSION, SRC_THORNS, SRC_N };
struct ApxDmgScope
{
	int prev;
	explicit ApxDmgScope(int kind);
	~ApxDmgScope();
};
struct ApxGenStats { int rooms = 0, monsters = 0, gold = 0, items = 0, decorations = 0; uint32_t uidAtGen = 0, entitiesAtGen = 0, markGold[16] = {}, markItems[16] = {}; uint64_t marks[16] = {}; };
uint64_t apxFloorHash();
struct SDL_Window;
void apxShotMaybe(SDL_Window* win);                // shot.cpp, called inside GO_SwapBuffers before the swap                           // floorhash.cpp
void apxRngMark(int slot, int gold, int items);                        // fingerprint of map_rng state, to localise nondeterminism
extern ApxGenStats apxGen;                       // set by generateDungeon (maps.cpp)
extern double apxGenMs, apxLoadMs, apxPathMapsMs; // set by the floor loader (game.cpp)

namespace apx
{
	extern bool enabled;
	extern uint64_t droppedRows, botsStuck, shotsFailed;
	extern Uint32 seedBase;                         // -apx-seed=N (0 = off)
	Uint32 deriveSeed(int floor, bool secret);
	void seedRuntimeRngs();                         // local_rng/net_rng from seedBase (character creation, combat rolls)
	void applyGameKey();                            // uniqueGameKey = seedBase when -apx-seed is set (treasure/station/mimic layout follow it)
	void init(const char* outDir, bool outGiven);   // call after argv parsing
	void close();
	double wallMs();
	void commonFields(ApxRow& r);                    // run_id .. wall_ms
	void write(const char* file, ApxRow& r);         // append + flush; counts dropped rows
	void onHp(Entity* victim, int delta, bool lethal); // from Entity::setHP (encounter.cpp); lethal = HP would have gone below 1
	extern long encountersClosed;
	Uint32 idleTicks();
	bool encounterOpen();
	void noContactRow();
	void clearDowned();
	bool isDowned(int p);
	void shotRequest(const char* name);              // next swap writes <out>/shots/<name>.png ("" = timestamped)
	extern uint64_t lastFloorHash;
	void onItemUse(bool consumed);
	void encounterTick();
	void closeEncounter(const char* why);
	long unattributedTotal();
	extern std::string scenarioName; extern double botSkill; extern int partyN;
	extern bool simOn; extern int simExit; extern long floorsLoaded;
	void simInit(const char* scenarioPath);
	bool simActive();
	int simBatch();
	void simTick();
	void botThink(Entity* my);
	void botPost(Entity* my);                        // end of actPlayer: apply the bot's Use selection (ladder, doors)
	long encHitsLanded();
	void onTick();                                 // after ++ticks
	void onFloorLoaded();                            // writes the floors.jsonl row
	std::string outDir();
}
