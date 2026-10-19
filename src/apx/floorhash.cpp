// apx/floorhash.cpp -- BF1-2: deterministic floor hash (FNV-1a 64).
// Inputs, in order: map width, height, every tile of every layer, then the entity rows sorted by
// (sprite, x, y, yaw, extra). No pointers, no uids, no tick counters, so spawn order and runtime
// state cannot change it. Debug knobs (all env, all default off):
//   APX_HASH_POKE=1  adds 1 to one wall-layer tile of the hashed copy: the hash MUST change (control)
//   APX_HASH_BAD=1   mixes `ticks` into the hash: the repeat gate MUST fail (control)
//   APX_DUMP_FLOOR=1 writes the sorted rows to <out>/floor_dump_<floor>_<hash>.txt for diffing
#include "telemetry.hpp"
#include "../main.hpp"
#include "../game.hpp"
#include "../entity.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <vector>

namespace
{
	struct Fnv
	{
		uint64_t h = 1469598103934665603ull;
		void byte(uint8_t b) { h ^= b; h *= 1099511628211ull; }
		void i32(int32_t v) { for ( int k = 0; k < 4; ++k ) { byte((uint8_t)((uint32_t)v >> (8 * k))); } }
	};
	bool envOn(const char* n) { const char* v = getenv(n); return v && v[0] == '1'; }
}

uint64_t apxFloorHash()
{
	Fnv f;
	const int w = (int)map.width, h = (int)map.height;
	f.i32(w);
	f.i32(h);
	const bool poke = envOn("APX_HASH_POKE");
	const size_t n = (size_t)w * h * MAPLAYERS;
	for ( size_t i = 0; i < n; ++i )
	{
		int32_t v = map.tiles[i];
		if ( poke && i == (size_t)(OBSTACLELAYER + 5 * MAPLAYERS + 5 * MAPLAYERS * h) ) { v += 1; }
		f.i32(v);
	}
	// entity rows: sprite, x, y (1/16 tile = 1 world unit, rounded), yaw (whole degrees), extra
	typedef std::tuple<int, int, int, int, int> Row;
	std::vector<Row> rows;
	for ( node_t* nd = map.entities->first; nd; nd = nd->next )
	{
		Entity* e = (Entity*)nd->element;
		if ( !e ) { continue; }
		// players and their limbs are not part of the floor (their head sprite is a random pick of the quickstart character)
		if ( e->behavior == &actPlayer || e->behavior == &actPlayerLimb ) { continue; }
		double deg = e->yaw * 180.0 / PI;
		int yaw = (int)std::lround(deg) % 360;
		if ( yaw < 0 ) { yaw += 360; }
		int extra = 0;
		if ( e->behavior == &actMonster ) { extra = (int)e->getMonsterTypeFromSprite(); }
		else if ( e->behavior == &actItem ) { extra = e->skill[10]; }        // item type
		else if ( e->behavior == &actGoldBag ) { extra = e->goldAmount; }
		rows.emplace_back(e->sprite, (int)std::lround(e->x), (int)std::lround(e->y), yaw, extra);
	}
	std::sort(rows.begin(), rows.end());
	for ( auto& r : rows )
	{
		f.i32(std::get<0>(r)); f.i32(std::get<1>(r)); f.i32(std::get<2>(r)); f.i32(std::get<3>(r)); f.i32(std::get<4>(r));
	}
	if ( envOn("APX_HASH_BAD") ) { f.i32((int32_t)ticks); }
	if ( envOn("APX_DUMP_FLOOR") )
	{
		char name[512];
		snprintf(name, sizeof(name), "%s/floor_dump_%d_%016llx.txt", apx::outDir().c_str(), currentlevel, (unsigned long long)f.h);
		if ( FILE* o = fopen(name, "wb") )
		{
			for ( auto& r : rows )
			{
				fprintf(o, "%d %d %d %d %d\n", std::get<0>(r), std::get<1>(r), std::get<2>(r), std::get<3>(r), std::get<4>(r));
			}
			fclose(o);
		}
	}
	return f.h;
}
