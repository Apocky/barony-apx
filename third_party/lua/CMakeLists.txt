# Lua 5.4.7 (MIT), vendored from https://www.lua.org/ftp/lua-5.4.7.tar.gz
# SHA256 9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30 (checked 2026-10-19).
# Omitted on purpose: lua.c, luac.c, linit.c, liolib.c, loslib.c, loadlib.c, ldblib.c. The embedder
# (src/apx/lua_layer.cpp) opens only base, coroutine, table, string, math and utf8; there is no io, os,
# package or debug library in this build at all.
file(GLOB APX_LUA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
add_library(apx_lua STATIC ${APX_LUA_SOURCES})
set_target_properties(apx_lua PROPERTIES LINKER_LANGUAGE C)
target_include_directories(apx_lua PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
  target_compile_options(apx_lua PRIVATE /W1 /wd4244 /wd4267 /wd4702 /wd4334)
endif()
