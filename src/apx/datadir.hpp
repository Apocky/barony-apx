// apx/datadir.hpp -- the ONE rule for where Apocky's Labyrinth reads its own data (data/apx).
// 2026-10-22 (Apocky: the installed 0.2.0 read hostility/move/magic tables from the developer's source tree because
// those loaders tried the compile-time folder BEFORE the game's own folder). Order, used by every loader:
//   1. APX_PARTS_DATA (explicit override)
//   2. <folder of the running .exe>/data/apx   (what a release ships; found by data/apx/SCHEMA.md)
//   3. APX_PARTS_DATA_DIR (compile-time source folder; developer builds run from build/)
//   4. ./data/apx
// The choice is logged once, so a wrong folder is visible in log.txt.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include <SDL.h>

#ifndef APX_PARTS_DATA_DIR
#define APX_PARTS_DATA_DIR "data/apx"
#endif

void printlog(const char* str, ...);

namespace apx
{
	inline const std::string& dataDir()
	{
		static const std::string dir = []() -> std::string {
			auto has = [](const std::string& d) { FILE* f = fopen((d + "/SCHEMA.md").c_str(), "rb"); if ( f ) { fclose(f); return true; } return false; };
			std::string why, d;
			if ( const char* e = std::getenv("APX_PARTS_DATA") ) { if ( *e ) { d = e; why = "APX_PARTS_DATA"; } }
			if ( d.empty() )
			{
				char* bp = SDL_GetBasePath();
				const std::string rel = (bp ? std::string(bp) : std::string("./")) + "data/apx";
				if ( bp ) { SDL_free(bp); }
				if ( has(rel) ) { d = rel; why = "next to the exe"; }
			}
			if ( d.empty() && has(APX_PARTS_DATA_DIR) ) { d = APX_PARTS_DATA_DIR; why = "compile-time source folder (developer build)"; }
			if ( d.empty() ) { d = "data/apx"; why = "relative fallback"; }
			printlog("[apx] data folder: %s (%s)", d.c_str(), why.c_str());
			return d;
		}();
		return dir;
	}
}
