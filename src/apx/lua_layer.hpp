// apx: the fork's own Lua 5.4 content layer (D52). Host only (multiplayer != CLIENT).
// Card: Apockalypsis handovers/tasks/BLUA-lua-layer-card.md
#pragma once

class Entity;

// Called once from main() after argument parsing. Does nothing unless APX_LUA is not "0".
void apxLuaInit(bool testMode);
void apxLuaShutdown();

// Hooks (each is a single call at its site; each returns at once when the layer is off):
void apxLuaTick();                                  // game.cpp, right after ++ticks
void apxLuaLevelEntered();                          // end of assignActions (maps.cpp)
void apxLuaMonsterSpawned(Entity* e, const char* source); // summonMonsterNoSmoke (actmonster.cpp)
void apxLuaHpChanged(Entity* e, int oldHP, int newHP);    // Entity::modHP (entity.cpp)

// Debug: reload scripts (console command /apx_lua_reload, or APX_LUA_WATCH=1 polling).
void apxLuaReload();
