/* apx_lua_run: tiny runner for offline Lua tests. Opens base, table, string, math, utf8; os has only exit. */
#include <stdio.h>
#include <stdlib.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
static int l_exit(lua_State* L) { exit((int)luaL_optinteger(L, 1, 0)); return 0; }
int main(int argc, char** argv) {
  lua_State* L = luaL_newstate();
  luaL_requiref(L, "_G", luaopen_base, 1); luaL_requiref(L, "string", luaopen_string, 1); luaL_requiref(L, "table", luaopen_table, 1);
  luaL_requiref(L, "math", luaopen_math, 1); luaL_requiref(L, "utf8", luaopen_utf8, 1); lua_pop(L, 5);
  lua_newtable(L); lua_pushcfunction(L, l_exit); lua_setfield(L, -2, "exit"); lua_setglobal(L, "os");
  if (argc < 2) { fprintf(stderr, "usage: apx_lua_run file.lua\n"); return 2; }
  if (luaL_dofile(L, argv[1]) != LUA_OK) { fprintf(stderr, "%s\n", lua_tostring(L, -1)); return 1; }
  lua_close(L); return 0;
}
