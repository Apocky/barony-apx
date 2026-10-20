// apx: the fork's own Lua 5.4 content layer (D52). See lua_layer.hpp and the BLUA card.
//
// Safety model: one lua_State, created only on the host/single-player side. Only the base,
// coroutine, table, string, math and utf8 libraries are opened (the io, os, package and debug
// libraries are not even compiled in). Each script runs in its own environment table; every
// handler call is a protected call with an instruction budget; a handler that fails 5 times is
// disabled; the state has a 64 MB memory cap. A script error is logged and the game keeps running.
// APX_LUA_FATAL=1 turns a script error into exit code 3 (the control run of the self-test).
#include "../main.hpp"
#include "../game.hpp"
#include "../stat.hpp"
#include "../messages.hpp"
#include "../entity.hpp"
#include "../monster.hpp"
#include "../net.hpp"
#include "../player.hpp"
#include "../collision.hpp"
#include "../paths.hpp"
#include "../shops.hpp"
#include "trade_glue.hpp"
#include "../items.hpp"
#include "lua_layer.hpp"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <functional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr size_t kMemCap = 64u * 1024u * 1024u;
constexpr int kInstrBudget = 5000000;   // VM instructions per handler call
constexpr int kMaxHandlerErrors = 5;
constexpr int kSpawnBudgetPerSecond = 40; // apx.spawn calls accepted per second (BLUA-3 budget)
constexpr int kMaxDepth = 4;            // nested event dispatch (a handler spawns, which fires an event)

struct Handler { int ref; std::string script; int errors; bool disabled; };
struct Timer { int id; Uint32 due; Uint32 every; int ref; std::string script; bool dead; };

struct Layer {
	lua_State* L = nullptr;
	bool on = false;
	bool test = false;
	bool fatal = false;
	bool watch = false;
	size_t used = 0;
	int budget = 0;
	int depth = 0;
	int nextTimer = 1;
	std::string curScript = "?";
	std::string dir, testDir;
	std::vector<std::pair<std::string, std::vector<Handler>>> handlers; // event -> handlers
	std::vector<Timer> timers;
	std::mt19937_64 rng{0x41504b59ull};
	fs::file_time_type stamp{};
	// counters, written to the log at shutdown and on request (rule: every decline is visible)
	bool hadMonsters = false;
	int nSpawnRefused = 0;
	int nErrors = 0, nDisabled = 0, nDropped = 0, nEvents = 0, nLoaded = 0, nScriptFail = 0;
};
Layer G;

void logf(const char* fmt, ...) {
	char buf[1024];
	va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
	printlog("[APX-LUA] %s", buf);
}

void* allocator(void* ud, void* ptr, size_t osize, size_t nsize) {
	Layer* g = (Layer*)ud;
	if ( nsize == 0 ) { if ( ptr ) { g->used -= osize; free(ptr); } return nullptr; }
	size_t old = ptr ? osize : 0;
	if ( g->used - old + nsize > kMemCap ) { return nullptr; } // Lua raises "not enough memory"
	void* p = realloc(ptr, nsize);
	if ( p ) { g->used = g->used - old + nsize; }
	return p;
}

void instrHook(lua_State* L, lua_Debug*) {
	G.budget -= 1000;
	if ( G.budget <= 0 ) { luaL_error(L, "instruction budget exceeded (%d)", kInstrBudget); }
}

int msgh(lua_State* L) {
	const char* m = lua_tostring(L, 1);
	luaL_traceback(L, L, m ? m : "(error object is not a string)", 1);
	return 1;
}

void reportError(const std::string& where, const char* text) {
	G.nErrors++;
	logf("error in %s: %s", where.c_str(), text ? text : "?");
	if ( G.fatal ) { logf("APX_LUA_FATAL=1: exiting with code 3"); fflush(nullptr); exit(3); }
}

// pcall the function on the stack top with nargs arguments below it already pushed (fn first).
bool call(int nargs, const std::string& where) {
	lua_State* L = G.L;
	int base = lua_gettop(L) - nargs - 1;
	lua_pushcfunction(L, msgh);
	lua_insert(L, base + 1);
	G.budget = kInstrBudget;
	lua_sethook(L, instrHook, LUA_MASKCOUNT, 1000);
	int rc = lua_pcall(L, nargs, 0, base + 1);
	lua_sethook(L, nullptr, 0, 0);
	bool ok = rc == LUA_OK;
	if ( !ok ) { reportError(where, lua_tostring(L, -1)); lua_pop(L, 1); }
	lua_remove(L, base + 1);
	return ok;
}

std::vector<Handler>* handlersFor(const std::string& ev, bool create) {
	for ( auto& p : G.handlers ) { if ( p.first == ev ) { return &p.second; } }
	if ( !create ) { return nullptr; }
	G.handlers.push_back({ ev, {} });
	return &G.handlers.back().second;
}

bool validEvent(const std::string& e) {
	return e == "level_entered" || e == "monster_spawned" || e == "damage" || e == "death" || e == "item_picked" || e == "floor_cleared";
}

// ---- API -----------------------------------------------------------------------------------

int api_log(lua_State* L) {
	size_t n; const char* s = luaL_checklstring(L, 1, &n);
	logf("%s: %.*s", G.curScript.c_str(), (int)std::min<size_t>(n, 800), s);
	return 0;
}
int api_print(lua_State* L) {
	int n = lua_gettop(L); std::string out;
	for ( int i = 1; i <= n; i++ ) { size_t l; const char* s = luaL_tolstring(L, i, &l); out.append(s, l); out += i < n ? "\t" : ""; lua_pop(L, 1); }
	logf("%s: %.*s", G.curScript.c_str(), (int)std::min<size_t>(out.size(), 800), out.c_str());
	return 0;
}
int api_message(lua_State* L) {
	const char* s = luaL_checkstring(L, 1);
	int p = (int)luaL_optinteger(L, 2, -1);
	for ( int i = 0; i < MAXPLAYERS; i++ ) {
		if ( (p < 0 || p == i) && players[i] && players[i]->isLocalPlayer() ) { messagePlayer(i, MESSAGE_MISC, "%s", s); }
	}
	return 0;
}
int api_on(lua_State* L) {
	std::string ev = luaL_checkstring(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	if ( !validEvent(ev) ) { return luaL_error(L, "unknown event '%s'", ev.c_str()); }
	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	handlersFor(ev, true)->push_back({ ref, G.curScript, 0, false });
	return 0;
}
int addTimer(lua_State* L, bool repeat) {
	lua_Integer t = luaL_checkinteger(L, 1);
	luaL_argcheck(L, t >= 1 && t <= 50 * 3600 * 24, 1, "ticks out of range");
	luaL_checktype(L, 2, LUA_TFUNCTION);
	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	int id = G.nextTimer++;
	G.timers.push_back({ id, ticks + (Uint32)t, repeat ? (Uint32)t : 0u, ref, G.curScript, false });
	lua_pushinteger(L, id);
	return 1;
}
int api_after(lua_State* L) { return addTimer(L, false); }
int api_every(lua_State* L) { return addTimer(L, true); }
int api_cancel(lua_State* L) {
	int id = (int)luaL_checkinteger(L, 1);
	for ( auto& t : G.timers ) { if ( t.id == id ) { t.dead = true; } }
	return 0;
}
int api_tick(lua_State* L) { lua_pushinteger(L, ticks); return 1; }
int api_floor(lua_State* L) { lua_pushinteger(L, currentlevel); return 1; }
int api_rand(lua_State* L) {
	lua_Integer n = luaL_checkinteger(L, 1);
	luaL_argcheck(L, n >= 1, 1, "n must be >= 1");
	lua_pushinteger(L, (lua_Integer)(G.rng() % (unsigned long long)n));
	return 1;
}
int api_seed(lua_State* L) { G.rng.seed((unsigned long long)luaL_checkinteger(L, 1)); return 0; }

int monsterByName(const char* name) {
	for ( int i = 1; i < NUMMONSTERS; i++ ) { if ( !strcmp(monstertypename[i], name) ) { return i; } }
	return -1;
}
Entity* entityByUid(lua_Integer uid) {
	Entity* e = uidToEntity((Sint32)uid);
	return e;
}
int api_spawn(lua_State* L) {
	const char* kind = luaL_checkstring(L, 1);
	lua_Number x = luaL_checknumber(L, 2), y = luaL_checknumber(L, 3);
	if ( multiplayer == CLIENT ) { lua_pushnil(L); lua_pushstring(L, "client"); return 2; }
	int k = monsterByName(kind);
	if ( k < 0 ) { lua_pushnil(L); lua_pushfstring(L, "unknown monster '%s'", kind); return 2; }
	if ( x < 1 || y < 1 || x >= (lua_Number)map.width - 1 || y >= (lua_Number)map.height - 1 ) { lua_pushnil(L); lua_pushstring(L, "outside the map"); return 2; }
	// per-second spawn budget (counted, visible in apx.stats().budget_refused_spawn); APX_LUA_NOBUDGET=1 is the test-mode control
	static Uint32 windowStart = 0; static int inWindow = 0;
	if ( ticks - windowStart >= TICKS_PER_SECOND ) { windowStart = ticks; inWindow = 0; }
	if ( inWindow >= kSpawnBudgetPerSecond && !(G.test && getenv("APX_LUA_NOBUDGET")) ) { G.nSpawnRefused++; lua_pushnil(L); lua_pushstring(L, "budget"); return 2; }
	Entity* e = summonMonster((Monster)k, (long)(x * 16.0), (long)(y * 16.0), false);
	if ( !e ) { lua_pushnil(L); lua_pushstring(L, "no room"); return 2; }
	inWindow++;
	lua_pushinteger(L, e->getUID());
	return 1;
}
// Make a monster hunt a player (or any entity). Wakes sleepers: sets its target and the PATH state.
int api_set_target(lua_State* L) {
	Entity* m = entityByUid(luaL_checkinteger(L, 1)); Entity* t = entityByUid(luaL_checkinteger(L, 2));
	if ( multiplayer == CLIENT || !m || !t || m->behavior != &actMonster ) { lua_pushboolean(L, 0); return 1; }
	m->monsterAcquireAttackTarget(*t, MONSTER_STATE_PATH);
	lua_pushboolean(L, 1); return 1;
}
// Send a monster walking to a tile (idle patrol): path generation by the engine's own pathfinder.
int api_path_to(lua_State* L) {
	Entity* m = entityByUid(luaL_checkinteger(L, 1)); lua_Integer x = luaL_checkinteger(L, 2), y = luaL_checkinteger(L, 3);
	if ( multiplayer == CLIENT || !m || m->behavior != &actMonster || x < 1 || y < 1 || x >= (lua_Integer)map.width - 1 || y >= (lua_Integer)map.height - 1 ) { lua_pushboolean(L, 0); return 1; }
	bool ok = m->monsterSetPathToLocation((int)x, (int)y, 2, GENERATE_PATH_TO_HUNT_MONSTER_TARGET);
	if ( ok ) { m->monsterState = MONSTER_STATE_PATH; }
	lua_pushboolean(L, ok); return 1;
}
// Line of sight between two map points (tile units) with no entity involved: marches the segment in quarter-tile steps and
// stops at a wall (non-zero obstacle layer). Used to reject ambush spawn points BEFORE a body exists. true = clear.
int api_tile_los(lua_State* L) {
	lua_Number x1 = luaL_checknumber(L, 1), y1 = luaL_checknumber(L, 2), x2 = luaL_checknumber(L, 3), y2 = luaL_checknumber(L, 4);
	if ( !map.tiles ) { lua_pushnil(L); return 1; }
	double dx = x2 - x1, dy = y2 - y1, len = sqrt(dx * dx + dy * dy);
	int steps = (int)(len * 4) + 1;
	for ( int i = 1; i < steps; i++ ) {
		double t = (double)i / steps, x = x1 + dx * t, y = y1 + dy * t;
		if ( x < 0 || y < 0 || x >= (double)map.width || y >= (double)map.height ) { lua_pushboolean(L, 0); return 1; }
		if ( map.tiles[OBSTACLELAYER + (int)y * MAPLAYERS + (int)x * MAPLAYERS * map.height] ) { lua_pushboolean(L, 0); return 1; }
	}
	lua_pushboolean(L, 1); return 1;
}
// ---- BT1 trade (D73): peddlers, discounts, and probes that run the REAL shop hooks without a UI ----------------------------
int api_spawn_peddler(lua_State* L) {
	lua_Number x = luaL_checknumber(L, 1), y = luaL_checknumber(L, 2);
	Entity* e = apx::tradeglue::spawnPeddler(x, y);
	if ( !e ) { lua_pushnil(L); lua_pushstring(L, "no room or client"); return 2; }
	lua_pushinteger(L, e->getUID()); return 1;
}
int api_spawn_haven(lua_State* L) {   // test and director use: a shopkeeper flagged as the floor's haven merchant (as the apx-gen bridge does)
	lua_Number x = luaL_checknumber(L, 1), y = luaL_checknumber(L, 2);
	if ( multiplayer == CLIENT || x < 1 || y < 1 || x >= (lua_Number)map.width - 1 || y >= (lua_Number)map.height - 1 ) { lua_pushnil(L); return 1; }
	Entity* e = summonMonster(SHOPKEEPER, (long)(x * 16.0), (long)(y * 16.0), false);
	if ( !e ) { lua_pushnil(L); return 1; }
	apx::tradeglue::markHaven(e);
	lua_pushinteger(L, e->getUID()); return 1;
}
int api_check_enemy(lua_State* L) {
	Entity* a = entityByUid(luaL_checkinteger(L, 1)); Entity* b = entityByUid(luaL_checkinteger(L, 2));
	if ( !a || !b ) { lua_pushnil(L); return 1; }
	lua_pushboolean(L, a->checkEnemy(b)); return 1;
}
int api_trade_discount(lua_State* L) {
	apx::tradeglue::setDiscount(luaL_checkstring(L, 1), (int)luaL_checkinteger(L, 2));
	return 0;
}
int api_trade_state(lua_State* L) {
	auto& c = apx::tradeglue::counters;
	lua_newtable(L);
	auto put = [&](const char* k, int v) { lua_pushinteger(L, v); lua_setfield(L, -2, k); };
	put("shops", c.shops); put("hamlet", c.hamlet); put("rows_realized", c.rowsRealized); put("rows_pending_pouch", c.rowsPendingPouch);
	put("rows_pending_service", c.rowsPendingService); put("class_refused", c.classRefused); put("purse_refused", c.purseRefused); put("peddlers", c.peddlers);
	lua_pushboolean(L, apx::tradeglue::on()); lua_setfield(L, -2, "on");
	return 1;
}
int api_shop_uids(lua_State* L) {
	lua_newtable(L); int i = 0;
	if ( map.creatures ) {
		for ( node_t* nd = map.creatures->first; nd; nd = nd->next ) {
			Entity* e = (Entity*)nd->element;
			if ( e && e->behavior == &actMonster && e->getStats() && e->getStats()->type == SHOPKEEPER ) { lua_pushinteger(L, e->getUID()); lua_rawseti(L, -2, ++i); }
		}
	}
	return 1;
}
// Opens nothing on screen: points player 0's shop globals at the shopkeeper so the real hooks answer for it.
struct ShopCtx {
	int oldType; Uint32 oldUid; bool ok = false; Entity* shop = nullptr;
	explicit ShopCtx(lua_Integer uid) {
		oldType = shopkeepertype[0]; oldUid = shopkeeper[0];
		shop = uidToEntity((Sint32)uid);
		if ( shop && shop->behavior == &actMonster && shop->getStats() && shop->getStats()->type == SHOPKEEPER ) { shopkeepertype[0] = shop->monsterStoreType; shopkeeper[0] = shop->getUID(); ok = true; }
	}
	~ShopCtx() { shopkeepertype[0] = oldType; shopkeeper[0] = oldUid; }
};
int api_shop_info(lua_State* L) {
	ShopCtx c(luaL_checkinteger(L, 1));
	if ( !c.ok ) { lua_pushnil(L); return 1; }
	Stat* st = c.shop->getStats();
	lua_newtable(L);
	lua_pushinteger(L, c.shop->monsterStoreType); lua_setfield(L, -2, "store_type");
	lua_pushinteger(L, st->GOLD); lua_setfield(L, -2, "purse");
	lua_newtable(L); int n = 0;
	for ( node_t* nd = st->inventory.first; nd; nd = nd->next ) {
		Item* it = (Item*)nd->element; if ( !it ) { continue; }
		lua_newtable(L);
		lua_pushstring(L, apx::tradeglue::typeName((int)it->type)); lua_setfield(L, -2, "type");
		lua_pushinteger(L, it->count); lua_setfield(L, -2, "count");
		lua_pushinteger(L, it->buyValue(0)); lua_setfield(L, -2, "price");
		lua_rawseti(L, -2, ++n);
	}
	lua_setfield(L, -2, "items");
	return 1;
}
// Sell probe through the real path pieces: isItemSellableToShop, Item::sellValue, the purse check. Does not move gold or items.
int api_shop_probe_sell(lua_State* L) {
	ShopCtx c(luaL_checkinteger(L, 1));
	const char* name = luaL_checkstring(L, 2);
	int t = apx::tradeglue::typeByName(name);
	if ( !c.ok || t < 0 ) { lua_pushnil(L); return 1; }
	Item* it = newItem(static_cast<ItemType>(t), EXCELLENT, 0, 1, 0, true, nullptr);
	bool sellable = isItemSellableToShop(0, it);
	int offer = it->sellValue(0);
	int vanillaValue = it->getGoldValue();
	bool purse = apx::tradeglue::purseRefuses(0, it);
	free(it);
	lua_pushboolean(L, sellable); lua_pushinteger(L, offer); lua_pushinteger(L, vanillaValue); lua_pushboolean(L, purse);
	return 4;
}
// Sells one probe item for real through the host path pieces that move money: shop purse down by the offer. Returns offer or nil.
int api_shop_drain(lua_State* L) {
	ShopCtx c(luaL_checkinteger(L, 1));
	if ( !c.ok ) { lua_pushnil(L); return 1; }
	c.shop->getStats()->GOLD = (Sint32)luaL_checkinteger(L, 2);
	lua_pushboolean(L, 1); return 1;
}
int api_monster_state(lua_State* L) {
	Entity* m = entityByUid(luaL_checkinteger(L, 1));
	if ( !m || m->behavior != &actMonster ) { lua_pushnil(L); return 1; }
	lua_pushinteger(L, m->monsterState); return 1;
}
int api_set_tile(lua_State* L) {
	lua_Integer x = luaL_checkinteger(L, 1), y = luaL_checkinteger(L, 2), z = luaL_checkinteger(L, 3), v = luaL_checkinteger(L, 4);
	if ( multiplayer == CLIENT || !map.tiles || x < 0 || y < 0 || z < 0 || x >= (lua_Integer)map.width || y >= (lua_Integer)map.height || z >= MAPLAYERS || v < 0 || v > 65535 ) {
		lua_pushboolean(L, 0); return 1;
	}
	map.tiles[z + y * MAPLAYERS + x * MAPLAYERS * map.height] = (Sint32)v; // not networked yet: host-only (see card step 6)
	lua_pushboolean(L, 1); return 1;
}
int api_get_tile(lua_State* L) {
	lua_Integer x = luaL_checkinteger(L, 1), y = luaL_checkinteger(L, 2), z = luaL_checkinteger(L, 3);
	if ( !map.tiles || x < 0 || y < 0 || z < 0 || x >= (lua_Integer)map.width || y >= (lua_Integer)map.height || z >= MAPLAYERS ) { lua_pushnil(L); return 1; }
	lua_pushinteger(L, map.tiles[z + y * MAPLAYERS + x * MAPLAYERS * map.height]); return 1;
}
Sint32* statField(Stat* s, const char* n) {
	if ( !strcmp(n, "hp") ) return &s->HP;       if ( !strcmp(n, "maxhp") ) return &s->MAXHP;
	if ( !strcmp(n, "mp") ) return &s->MP;       if ( !strcmp(n, "maxmp") ) return &s->MAXMP;
	if ( !strcmp(n, "str") ) return &s->STR;     if ( !strcmp(n, "dex") ) return &s->DEX;
	if ( !strcmp(n, "con") ) return &s->CON;     if ( !strcmp(n, "int") ) return &s->INT;
	if ( !strcmp(n, "per") ) return &s->PER;     if ( !strcmp(n, "chr") ) return &s->CHR;
	if ( !strcmp(n, "level") ) return &s->LVL;
	return nullptr;
}
int api_get_stat(lua_State* L) {
	Entity* e = entityByUid(luaL_checkinteger(L, 1)); const char* n = luaL_checkstring(L, 2);
	Stat* s = e ? e->getStats() : nullptr; Sint32* f = s ? statField(s, n) : nullptr;
	if ( !f ) { lua_pushnil(L); return 1; }
	lua_pushinteger(L, *f); return 1;
}
int api_set_stat(lua_State* L) {
	Entity* e = entityByUid(luaL_checkinteger(L, 1)); const char* n = luaL_checkstring(L, 2); lua_Integer v = luaL_checkinteger(L, 3);
	Stat* s = e ? e->getStats() : nullptr; Sint32* f = s ? statField(s, n) : nullptr;
	if ( !f || multiplayer == CLIENT || v < -9999 || v > 99999 ) { lua_pushboolean(L, 0); return 1; }
	if ( f == &s->HP ) { e->setHP((int)v); } else { *f = (Sint32)v; }
	lua_pushboolean(L, 1); return 1;
}
int api_hurt(lua_State* L) {
	Entity* e = entityByUid(luaL_checkinteger(L, 1)); lua_Integer a = luaL_checkinteger(L, 2);
	if ( !e || multiplayer == CLIENT || a < 1 || a > 9999 || !e->getStats() ) { lua_pushboolean(L, 0); return 1; }
	e->modHP(-(int)a); // goes through the same choke point as every other damage (fires damage and death events)
	lua_pushboolean(L, 1); return 1;
}
int api_monsters_near(lua_State* L) {
	lua_Number x = luaL_checknumber(L, 1), y = luaL_checknumber(L, 2), r = luaL_checknumber(L, 3);
	luaL_argcheck(L, r > 0 && r <= 128, 3, "radius 0..128 tiles");
	lua_newtable(L); int i = 0;
	if ( map.creatures ) {
		for ( node_t* nd = map.creatures->first; nd; nd = nd->next ) {
			Entity* e = (Entity*)nd->element; if ( !e || e->behavior != &actMonster ) { continue; }
			double dx = e->x / 16.0 - x, dy = e->y / 16.0 - y;
			if ( dx * dx + dy * dy <= r * r ) { lua_pushinteger(L, e->getUID()); lua_rawseti(L, -2, ++i); }
		}
	}
	return 1;
}
// Light at a tile for player 0's lightmap, 0..255 (max of r,g,b). nil when no lightmap exists (no game running).
int api_light_at(lua_State* L) {
	lua_Integer x = luaL_checkinteger(L, 1), y = luaL_checkinteger(L, 2);
	if ( x < 0 || y < 0 || x >= (lua_Integer)map.width || y >= (lua_Integer)map.height || lightmaps[0].size() < (size_t)map.width * map.height ) { lua_pushnil(L); return 1; }
	const auto& v = lightmaps[0][y + x * map.height];
	float m = std::max(v.x, std::max(v.y, v.z));
	lua_pushinteger(L, (lua_Integer)std::min(255.f, std::max(0.f, m * 255.f))); return 1;
}
int api_line_of_sight(lua_State* L) {
	Entity* a = entityByUid(luaL_checkinteger(L, 1)); Entity* b = entityByUid(luaL_checkinteger(L, 2));
	if ( !a || !b ) { lua_pushnil(L); return 1; }
	real_t dx = b->x - a->x, dy = b->y - a->y, dist = sqrt(dx * dx + dy * dy);
	if ( dist < 1 ) { lua_pushboolean(L, 1); return 1; }
	real_t d = lineTrace(a, a->x, a->y, atan2(dy, dx), dist, 0, false);
	lua_pushboolean(L, d >= dist - 2 || hit.entity == b); return 1;
}
// JSON encoder for apx.row: nil/boolean/number/string/table (array or string-keyed), depth 6.
void jsonValue(lua_State* L, int idx, std::string& out, int depth) {
	idx = lua_absindex(L, idx);
	switch ( lua_type(L, idx) ) {
	case LUA_TBOOLEAN: out += lua_toboolean(L, idx) ? "true" : "false"; break;
	case LUA_TNUMBER: {
		char b[48];
		if ( lua_isinteger(L, idx) ) { snprintf(b, sizeof(b), "%lld", (long long)lua_tointeger(L, idx)); }
		else { double d = lua_tonumber(L, idx); if ( d != d || d > 1e300 || d < -1e300 ) { d = 0; } snprintf(b, sizeof(b), "%.4f", d); }
		out += b; break; }
	case LUA_TSTRING: {
		size_t n; const char* t = lua_tolstring(L, idx, &n); out += '"';
		for ( size_t i = 0; i < n; i++ ) { unsigned char c = t[i];
			if ( c == '"' || c == '\\' ) { out += '\\'; out += (char)c; } else if ( c < 32 || c > 126 ) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; } else { out += (char)c; } }
		out += '"'; break; }
	case LUA_TTABLE: {
		if ( depth > 6 ) { out += "null"; break; }
		bool arr = lua_rawlen(L, idx) > 0; int count = 0;
		out += arr ? '[' : '{';
		if ( arr ) { for ( lua_Integer i = 1; i <= (lua_Integer)lua_rawlen(L, idx); i++ ) { if ( i > 1 ) { out += ','; } lua_rawgeti(L, idx, i); jsonValue(L, -1, out, depth + 1); lua_pop(L, 1); } }
		else { lua_pushnil(L); while ( lua_next(L, idx) ) {
			if ( lua_type(L, -2) == LUA_TSTRING ) { if ( count++ ) { out += ','; } jsonValue(L, -2, out, depth + 1); out += ':'; jsonValue(L, -1, out, depth + 1); }
			lua_pop(L, 1); } }
		out += arr ? ']' : '}'; break; }
	default: out += "null"; break;
	}
}
int g_rowsWritten = 0, g_rowsRefused = 0;
// One open handle per file (a per-row open cost 1.4 ms p99, measured). Flushed every 10 rows and at shutdown / test quit:
// a flush per row cost about 100 us of the 200 us decision budget (measured on a floor with 38 monsters).
std::map<std::string, std::ofstream> g_rowFiles;
std::map<std::string, size_t> g_rowSizes;
bool g_rowsDirty = false;
void flushRows() { g_rowsDirty = false; for ( auto& f : g_rowFiles ) { f.second.flush(); } }
int api_row(lua_State* L) {
	const char* name = luaL_checkstring(L, 1); luaL_checktype(L, 2, LUA_TTABLE);
	for ( const char* c = name; *c; c++ ) { if ( !(isalnum((unsigned char)*c) || *c == '_' || *c == '-') ) { return luaL_error(L, "bad row file name"); } }
	std::string line; jsonValue(L, 2, line, 0); line += '\n';
	auto& files = g_rowFiles; auto& sizes = g_rowSizes;
	std::string key = name;
	auto it = files.find(key);
	if ( it == files.end() ) {
		const char* d = getenv("APX_ROWS_DIR");
		std::string dir = d ? d : "C:/Apocrypha/diagnostics/apx-lua";
		std::error_code ec; fs::create_directories(dir, ec);
		fs::path f = fs::path(dir) / (key + ".jsonl");
		sizes[key] = fs::exists(f, ec) ? (size_t)fs::file_size(f, ec) : 0;
		it = files.emplace(key, std::ofstream(f, std::ios::app | std::ios::binary)).first;
	}
	if ( sizes[key] > 8u * 1024 * 1024 || !it->second ) { g_rowsRefused++; lua_pushboolean(L, 0); return 1; }
	it->second << line; sizes[key] += line.size();
	g_rowsDirty = true;   // flushed from apxLuaTick (off the decision path) and at shutdown
	bool good = it->second.good();
	g_rowsWritten++; lua_pushboolean(L, good); return 1;
}
int api_run_seed(lua_State* L) { lua_pushinteger(L, (lua_Integer)uniqueGameKey); return 1; }
int api_clock_us(lua_State* L) {
	lua_pushinteger(L, (lua_Integer)(SDL_GetPerformanceCounter() * 1000000ull / SDL_GetPerformanceFrequency())); return 1;
}
int api_entity_pos(lua_State* L) {
	Entity* e = entityByUid(luaL_checkinteger(L, 1));
	if ( !e ) { lua_pushnil(L); return 1; }
	lua_pushnumber(L, e->x / 16.0); lua_pushnumber(L, e->y / 16.0); return 2;
}
int api_player_uid(lua_State* L) {
	lua_Integer i = luaL_checkinteger(L, 1);
	if ( i < 0 || i >= MAXPLAYERS || !players[i] || !players[i]->entity ) { lua_pushnil(L); return 1; }
	lua_pushinteger(L, players[i]->entity->getUID()); return 1;
}
int api_monster_count(lua_State* L) {
	int n = 0;
	if ( map.creatures ) { for ( node_t* nd = map.creatures->first; nd; nd = nd->next ) { Entity* e = (Entity*)nd->element; if ( e && e->behavior == &actMonster && e->getStats() && e->getStats()->HP > 0 ) { n++; }   /* a dying body is not a monster */ } }
	lua_pushinteger(L, n); return 1;
}
int api_is_test(lua_State* L) { lua_pushboolean(L, G.test); return 1; }
int api_test_quit(lua_State* L) {
	if ( !G.test ) { return luaL_error(L, "test_quit is only available with -apx-luatest"); }
	logf("test_quit(%d) after %d errors caught, %d events", (int)luaL_optinteger(L, 1, 0), G.nErrors, G.nEvents);
	flushRows();
	mainloop = 0;
	return 0;
}
int api_stats(lua_State* L) {
	lua_newtable(L);
	auto put = [&](const char* k, int v) { lua_pushinteger(L, v); lua_setfield(L, -2, k); };
	put("errors", G.nErrors); put("disabled", G.nDisabled); put("dropped", G.nDropped);
	put("events", G.nEvents); put("scripts_loaded", G.nLoaded); put("scripts_failed", G.nScriptFail);
	put("mem_bytes", (int)G.used); put("budget_refused_spawn", G.nSpawnRefused);
	return 1;
}

const luaL_Reg kApi[] = {
	{ "log", api_log }, { "message", api_message }, { "on", api_on }, { "after", api_after }, { "every", api_every },
	{ "cancel", api_cancel }, { "tick", api_tick }, { "floor", api_floor }, { "rand", api_rand }, { "seed", api_seed },
	{ "spawn", api_spawn }, { "set_tile", api_set_tile }, { "get_tile", api_get_tile }, { "get_stat", api_get_stat },
	{ "set_stat", api_set_stat }, { "hurt", api_hurt }, { "set_target", api_set_target }, { "path_to", api_path_to }, { "monster_state", api_monster_state }, { "spawn_peddler", api_spawn_peddler }, { "spawn_haven", api_spawn_haven }, { "check_enemy", api_check_enemy }, { "trade_discount", api_trade_discount }, { "trade_state", api_trade_state }, { "shop_uids", api_shop_uids }, { "shop_info", api_shop_info }, { "shop_probe_sell", api_shop_probe_sell }, { "shop_set_purse", api_shop_drain }, { "tile_los", api_tile_los }, { "row", api_row }, { "run_seed", api_run_seed }, { "clock_us", api_clock_us }, { "monsters_near", api_monsters_near }, { "light_at", api_light_at }, { "line_of_sight", api_line_of_sight }, { "entity_pos", api_entity_pos }, { "player_uid", api_player_uid },
	{ "monster_count", api_monster_count }, { "is_test", api_is_test }, { "test_quit", api_test_quit },
	{ "stats", api_stats }, { nullptr, nullptr }
};

int api_readonly(lua_State* L) { return luaL_error(L, "apx is read-only"); }

// Builds the table every script env inherits from: safe base functions, libraries, and `apx`.
void buildSafeTable(lua_State* L) {
	lua_newtable(L); // safe
	static const char* baseFns[] = { "assert", "error", "ipairs", "next", "pairs", "pcall", "select", "tonumber", "tostring",
		"type", "xpcall", "setmetatable", "getmetatable", "rawget", "rawset", "rawequal", "rawlen", nullptr };
	for ( int i = 0; baseFns[i]; i++ ) { lua_getglobal(L, baseFns[i]); lua_setfield(L, -2, baseFns[i]); }
	lua_pushliteral(L, "Lua 5.4"); lua_setfield(L, -2, "_VERSION");
	for ( const char* lib : { "string", "table", "math", "utf8", "coroutine" } ) { lua_getglobal(L, lib); lua_setfield(L, -2, lib); }
	lua_pushcfunction(L, api_print); lua_setfield(L, -2, "print");
	// apx: a read-only proxy over the real API table; apx.run is the per-run data table.
	lua_newtable(L);                       // real
	luaL_setfuncs(L, kApi, 0);
	lua_newtable(L); lua_setfield(L, -2, "run");
	lua_newtable(L); lua_setfield(L, -2, "shared"); // scripts share live Lua values here (functions allowed); never saved
	lua_newtable(L);                       // proxy
	lua_newtable(L);                       // proxy metatable
	lua_pushvalue(L, -3); lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, api_readonly); lua_setfield(L, -2, "__newindex");
	lua_pushboolean(L, 0); lua_setfield(L, -2, "__metatable");
	lua_setmetatable(L, -2);
	lua_setfield(L, -3, "apx");            // safe.apx = proxy ; leaves real on the stack
	lua_pop(L, 1);                         // drop real
}

bool readFile(const fs::path& p, std::string& out) {
	std::ifstream f(p, std::ios::binary); if ( !f ) { return false; }
	std::ostringstream ss; ss << f.rdbuf(); out = ss.str(); return true;
}

void loadDir(const std::string& dir, const std::string& only = "") {
	std::error_code ec;
	if ( !fs::is_directory(dir, ec) ) { logf("script folder not found: %s", dir.c_str()); return; }
	std::vector<fs::path> files;
	for ( auto& e : fs::directory_iterator(dir, ec) ) { if ( e.is_regular_file() && e.path().extension() == ".lua" ) {
		if ( !only.empty() && ("," + only + ",").find("," + e.path().stem().string() + ",") == std::string::npos ) { continue; }
		files.push_back(e.path()); } }
	std::sort(files.begin(), files.end());
	lua_State* L = G.L;
	for ( auto& p : files ) {
		std::string name = p.filename().string(), src;
		if ( !readFile(p, src) || src.size() > 512 * 1024 ) { logf("cannot read %s", name.c_str()); G.nScriptFail++; continue; }
		G.curScript = name;
		int top = lua_gettop(L);
		if ( luaL_loadbufferx(L, src.data(), src.size(), ("@" + name).c_str(), "t") != LUA_OK ) {
			reportError(name, lua_tostring(L, -1)); G.nScriptFail++; lua_settop(L, top); continue;
		}
		// env = setmetatable({}, {__index = SAFE})
		lua_newtable(L); lua_newtable(L);
		lua_getfield(L, LUA_REGISTRYINDEX, "apx_safe"); lua_setfield(L, -2, "__index");
		lua_setmetatable(L, -2);
		lua_setupvalue(L, -2, 1);
		if ( call(0, name) ) { G.nLoaded++; logf("loaded %s", name.c_str()); } else { G.nScriptFail++; }
		lua_settop(L, top);
	}
	G.curScript = "?";
}

void loadAll() {
	loadDir(G.dir);
	if ( G.test ) { const char* t = getenv("APX_LUA_TESTS"); loadDir(G.testDir, t ? t : "control_error,selftest"); }
	std::error_code ec;
	G.stamp = fs::exists(G.dir, ec) ? fs::last_write_time(G.dir, ec) : fs::file_time_type{};
}

void openState() {
	G.used = 0; G.nextTimer = 1;
	G.L = lua_newstate(allocator, &G);
	if ( !G.L ) { logf("lua_newstate failed"); G.on = false; return; }
	lua_State* L = G.L;
	luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
	luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
	luaL_requiref(L, "table", luaopen_table, 1); lua_pop(L, 1);
	luaL_requiref(L, "math", luaopen_math, 1); lua_pop(L, 1);
	luaL_requiref(L, "utf8", luaopen_utf8, 1); lua_pop(L, 1);
	luaL_requiref(L, "coroutine", luaopen_coroutine, 1); lua_pop(L, 1);
	buildSafeTable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, "apx_safe");
	// strip the real globals so the main state cannot be reached by accident
	for ( const char* n : { "dofile", "loadfile", "load", "require", "collectgarbage", "print" } ) { lua_pushnil(L); lua_setglobal(L, n); }
	loadAll();
}

void closeState() {
	if ( G.L ) { lua_close(G.L); G.L = nullptr; }
	G.handlers.clear(); G.timers.clear(); G.used = 0;
}

// Dispatch an event; `push` pushes ONE argument (a table) onto the stack.
void dispatch(const char* ev, const std::function<void(lua_State*)>& push) {
	if ( !G.on || !G.L || multiplayer == CLIENT ) { return; }
	auto* hs = handlersFor(ev, false);
	if ( !hs || hs->empty() ) { return; }
	if ( G.depth >= kMaxDepth ) { G.nDropped++; return; }
	G.depth++; G.nEvents++;
	lua_State* L = G.L;
	for ( size_t i = 0; i < hs->size(); i++ ) { // index loop: a handler may register more handlers
		Handler h = (*hs)[i];
		if ( h.disabled ) { continue; }
		int top = lua_gettop(L);
		lua_rawgeti(L, LUA_REGISTRYINDEX, h.ref);
		push(L);
		std::string saved = G.curScript; G.curScript = h.script;
		bool ok = call(1, std::string(ev) + "@" + h.script);
		G.curScript = saved;
		if ( !ok && ++(*hs)[i].errors >= kMaxHandlerErrors ) {
			(*hs)[i].disabled = true; G.nDisabled++;
			logf("handler for %s from %s disabled after %d errors", ev, h.script.c_str(), kMaxHandlerErrors);
		}
		lua_settop(L, top);
	}
	G.depth--;
}

const char* kindName(Entity* e) {
	if ( e->behavior == &actPlayer ) { return "player"; }
	Stat* s = e->getStats();
	return (s && s->type > 0 && s->type < NUMMONSTERS) ? monstertypename[s->type] : "other";
}
void pushEntityTable(lua_State* L, Entity* e) {
	lua_newtable(L);
	lua_pushinteger(L, e->getUID()); lua_setfield(L, -2, "uid");
	lua_pushstring(L, kindName(e)); lua_setfield(L, -2, "kind");
	lua_pushnumber(L, e->x / 16.0); lua_setfield(L, -2, "x");
	lua_pushnumber(L, e->y / 16.0); lua_setfield(L, -2, "y");
	if ( e->behavior == &actPlayer ) { lua_pushinteger(L, e->skill[2]); lua_setfield(L, -2, "player"); }
}

} // namespace

// ---- public hooks ----------------------------------------------------------------------------

void apxLuaInit(bool testMode) {
	const char* en = getenv("APX_LUA");
	if ( en && !strcmp(en, "0") ) { logf("off (APX_LUA=0)"); return; }
	G.test = testMode;
	G.fatal = getenv("APX_LUA_FATAL") && !strcmp(getenv("APX_LUA_FATAL"), "1");
	G.watch = getenv("APX_LUA_WATCH") && !strcmp(getenv("APX_LUA_WATCH"), "1");
	const char* d = getenv("APX_LUA_DIR");
	std::string base = d ? d : "";
	if ( base.empty() ) {
		char* bp = SDL_GetBasePath();
		base = (bp ? std::string(bp) : std::string("./")) + "scripts";
		if ( bp ) { SDL_free(bp); }
		G.dir = base + "/apx"; G.testDir = base + "/apx-test";
	} else { G.dir = base + "/apx"; G.testDir = base + "/apx-test"; }
	G.on = true;
	logf("starting (Lua 5.4, dir=%s, test=%d, fatal=%d, watch=%d)", G.dir.c_str(), (int)G.test, (int)G.fatal, (int)G.watch);
	openState();
}

void apxLuaShutdown() {
	if ( !G.on ) { return; }
	logf("shutdown: events=%d errors=%d handlers_disabled=%d dropped=%d scripts_loaded=%d scripts_failed=%d mem=%zu",
		G.nEvents, G.nErrors, G.nDisabled, G.nDropped, G.nLoaded, G.nScriptFail, G.used);
	flushRows();
	closeState(); G.on = false;
}

void apxLuaReload() {
	if ( !G.on ) { return; }
	logf("reload");
	closeState(); G.nLoaded = 0; G.nScriptFail = 0; openState(); // run data is lost on reload (debug only)
}

void apxLuaTick() {
	static bool announced = false; // init runs before the log file is open, so say it once here
	if ( !announced ) {
		announced = true;
		if ( G.on ) { logf("ready: Lua 5.4, dir=%s, test=%d, fatal=%d, watch=%d, scripts_loaded=%d, scripts_failed=%d", G.dir.c_str(), (int)G.test, (int)G.fatal, (int)G.watch, G.nLoaded, G.nScriptFail); }
		else { logf("off"); }
	}
	if ( !G.on || !G.L || multiplayer == CLIENT ) { return; }
	if ( G.watch && ticks % 50 == 0 ) {
		std::error_code ec; auto t = fs::exists(G.dir, ec) ? fs::last_write_time(G.dir, ec) : fs::file_time_type{};
		if ( t != G.stamp ) { apxLuaReload(); return; }
	}
	if ( ticks % 25 == 0 && G.hadMonsters ) {
		int alive = 0;
		if ( map.creatures ) { for ( node_t* nd = map.creatures->first; nd; nd = nd->next ) { Entity* e = (Entity*)nd->element; if ( e && e->behavior == &actMonster && e->getStats() && e->getStats()->HP > 0 ) { alive++; } } }
		if ( alive == 0 ) {
			G.hadMonsters = false;
			dispatch("floor_cleared", [&](lua_State* L) { lua_newtable(L); lua_pushinteger(L, currentlevel); lua_setfield(L, -2, "floor"); });
		}
	}
	if ( g_rowsDirty && ticks % 250 == 0 ) { flushRows(); }
	if ( G.timers.empty() ) { return; }
	lua_State* L = G.L;
	for ( size_t i = 0; i < G.timers.size(); i++ ) { // handlers may add timers: index loop, copy before call
		if ( G.timers[i].dead || G.timers[i].due > ticks ) { continue; }
		Timer t = G.timers[i];
		if ( t.every ) { G.timers[i].due = ticks + t.every; } else { G.timers[i].dead = true; }
		int top = lua_gettop(L);
		lua_rawgeti(L, LUA_REGISTRYINDEX, t.ref);
		std::string saved = G.curScript; G.curScript = t.script;
		bool ok = call(0, "timer@" + t.script);
		G.curScript = saved;
		if ( !ok ) { G.timers[i].dead = true; G.nDisabled++; logf("timer %d from %s removed after an error", t.id, t.script.c_str()); }
		lua_settop(L, top);
	}
	G.timers.erase(std::remove_if(G.timers.begin(), G.timers.end(), [&](const Timer& t) {
		if ( t.dead ) { luaL_unref(L, LUA_REGISTRYINDEX, t.ref); } return t.dead; }), G.timers.end());
}

void apxLuaLevelEntered() {
	if ( !G.on || !G.L || multiplayer == CLIENT ) { return; }
	int n = 0;
	if ( map.creatures ) { // generator-made monsters first, so a script can see them before level_entered
		std::vector<Entity*> mons;
		for ( node_t* nd = map.creatures->first; nd; nd = nd->next ) { Entity* e = (Entity*)nd->element; if ( e && e->behavior == &actMonster ) { mons.push_back(e); } }
		n = (int)mons.size(); G.hadMonsters = n > 0;
		for ( Entity* e : mons ) { apxLuaMonsterSpawned(e, "generator"); }
	}
	dispatch("level_entered", [&](lua_State* L) {
		lua_newtable(L);
		lua_pushinteger(L, currentlevel); lua_setfield(L, -2, "floor");
		lua_pushboolean(L, secretlevel); lua_setfield(L, -2, "secret");
		lua_pushinteger(L, mapseed); lua_setfield(L, -2, "seed");
		lua_pushinteger(L, n); lua_setfield(L, -2, "monsters");
		lua_pushinteger(L, map.width); lua_setfield(L, -2, "width");
		lua_pushinteger(L, map.height); lua_setfield(L, -2, "height");
		lua_pushstring(L, map.name); lua_setfield(L, -2, "name");
	});
}

void apxLuaMonsterSpawned(Entity* e, const char* source) {
	if ( !G.on || !e ) { return; }
	G.hadMonsters = true;
	dispatch("monster_spawned", [&](lua_State* L) { pushEntityTable(L, e); lua_pushstring(L, source); lua_setfield(L, -2, "source"); });
}

void apxLuaItemPicked(int player, int type, int count) {
	if ( !G.on ) { return; }
	dispatch("item_picked", [&](lua_State* L) {
		lua_newtable(L);
		lua_pushinteger(L, player); lua_setfield(L, -2, "player");
		lua_pushinteger(L, type); lua_setfield(L, -2, "item_type");
		lua_pushinteger(L, count); lua_setfield(L, -2, "count");
	});
}

void apxLuaHpChanged(Entity* e, int oldHP, int newHP) {
	if ( !G.on || !e || newHP == oldHP ) { return; }
	if ( newHP < oldHP ) {
		dispatch("damage", [&](lua_State* L) {
			pushEntityTable(L, e);
			lua_pushinteger(L, oldHP - newHP); lua_setfield(L, -2, "amount");
			lua_pushinteger(L, newHP); lua_setfield(L, -2, "hp");
			Stat* s = e->getStats(); lua_pushinteger(L, s ? s->MAXHP : 0); lua_setfield(L, -2, "maxhp");
		});
	}
	if ( oldHP > 0 && newHP <= 0 ) {
		dispatch("death", [&](lua_State* L) { pushEntityTable(L, e); });
	}
}
