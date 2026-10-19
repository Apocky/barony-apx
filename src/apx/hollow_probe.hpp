// hollow_probe.hpp -- the H1b content probe (card BH1b rules 3 and 5): the engine reads what the lint
// accepted. Runs once on floor start when APX_BH_PROBE is set (any value; "content" is the batch), reads
// every class in APX_HOLLOW_DIR (data/apx/hollow), creates every consumable with the real newItem,
// spawns every creature and boss with summonMonster (16 per tick around player 0), tags each with its
// content tier and checks apx::tiers::tierOf reports it, resolves every monster and item name in the
// content, writes one JSON row to APX_BH_OUT and exits 0 (all counters 0) or 3.
// Counters: rows_unresolved, spawn_failed, item_failed, tier_mismatch, rows_loaded per class.
#pragma once

#include "../main.hpp"
#include "../game.hpp"
#include "../entity.hpp"
#include "../items.hpp"
#include "../monster.hpp"
#include "tiers.hpp"
#include "rapidjson/document.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace apx
{
namespace hollow
{
#include "hollow_enums.inc"

	inline int lookup(const std::pair<const char*, int>* t, size_t n, const std::string& name)
	{
		for ( size_t i = 0; i < n; ++i ) if ( name == t[i].first ) return t[i].second;
		return -1;
	}
	inline int monsterOf(const std::string& s) { return lookup(kApxMonsterNames, sizeof(kApxMonsterNames) / sizeof(kApxMonsterNames[0]), s); }
	inline int itemOf(const std::string& s) { return lookup(kApxItemNames, sizeof(kApxItemNames) / sizeof(kApxItemNames[0]), s); }

	inline bool readJson(const std::string& path, rapidjson::Document& d)
	{
		FILE* f = std::fopen(path.c_str(), "rb");
		if ( !f ) return false;
		std::string s;
		char buf[4096];
		size_t n;
		while ( (n = std::fread(buf, 1, sizeof buf, f)) > 0 ) s.append(buf, n);
		std::fclose(f);
		d.Parse(s.c_str());
		return !d.HasParseError();
	}

	struct Probe
	{
		int phase = 0;
		Uint32 t0 = 0;
		std::string dir, out;
		std::map<std::string, int> loaded;
		int rows_unresolved = 0, spawn_failed = 0, item_failed = 0, tier_mismatch = 0, items_made = 0, spawned = 0;
		int band_rat = -1, band_skeleton = -1;
		struct Spawn { int kind; apx::combat::Tier tier; std::string id; };
		std::vector<Spawn> todo;
		size_t next = 0;
	};
	inline Probe& P() { static Probe p; return p; }

	inline apx::combat::Tier tierFromName(const std::string& s)
	{
		using apx::combat::Tier;
		if ( s == "fodder" ) return Tier::Fodder;
		if ( s == "regular" ) return Tier::Regular;
		if ( s == "elite" ) return Tier::Elite;
		if ( s == "champion" ) return Tier::Champion;
		return Tier::Boss;
	}

	inline void finish()
	{
		Probe& p = P();
		std::string cls;
		for ( auto& kv : p.loaded ) cls += (cls.empty() ? "" : ",") + std::string("\"") + kv.first + "\":" + std::to_string(kv.second);
		bool ok = p.rows_unresolved == 0 && p.spawn_failed == 0 && p.item_failed == 0 && p.tier_mismatch == 0 && !p.loaded.empty();
		char row[2048];
		snprintf(row, sizeof row,
			"{\"probe\":\"content\",\"ok\":%s,\"rows_loaded\":{%s},\"items_made\":%d,\"spawned\":%d,\"rows_unresolved\":%d,\"spawn_failed\":%d,"
			"\"item_failed\":%d,\"tier_mismatch\":%d,\"floor1_band_tier_rat\":%d,\"floor1_band_tier_skeleton\":%d,\"level\":%d}",
			ok ? "true" : "false", cls.c_str(), p.items_made, p.spawned, p.rows_unresolved, p.spawn_failed, p.item_failed, p.tier_mismatch,
			p.band_rat, p.band_skeleton, currentlevel);
		printlog("[apx-bh1b] %s", row);
		if ( !p.out.empty() )
		{
			if ( FILE* f = std::fopen(p.out.c_str(), "ab") ) { std::fprintf(f, "%s\n", row); std::fclose(f); }
		}
		std::exit(ok ? 0 : 3);
	}

	inline void load(Probe& p)
	{
		rapidjson::Document man;
		if ( !readJson(p.dir + "/manifest.json", man) || !man.HasMember("classes") ) { ++p.rows_unresolved; return; }
		for ( auto& c : man["classes"].GetArray() )
		{
			std::string file = c["file"].GetString();
			rapidjson::Document d;
			if ( !readJson(p.dir + "/" + file, d) || !d.IsArray() ) { ++p.rows_unresolved; continue; }
			std::string stem = file.substr(0, file.size() - 5);
			p.loaded[stem] = (int)d.Size();
			for ( auto& r : d.GetArray() )
			{
				if ( stem == "consumables" )
				{
					int t = itemOf(r["base_type"].GetString());
					if ( t < 0 ) { ++p.item_failed; continue; }
					Item* it = newItem((ItemType)t, EXCELLENT, 0, 1, 0, true, nullptr);
					if ( !it || (int)it->type != t ) { ++p.item_failed; continue; }
					++p.items_made;
					free(it);
				}
				else if ( stem == "creatures" || stem == "bosses" )
				{
					int k = monsterOf(r["kind"].GetString());
					if ( k < 0 ) { ++p.rows_unresolved; continue; }
					p.todo.push_back({ k, tierFromName(r["tier"].GetString()), r["id"].GetString() });
				}
				else if ( stem == "npcs" )
				{
					if ( monsterOf(r["kind"].GetString()) < 0 ) ++p.rows_unresolved;
				}
				else if ( stem == "quests" )
				{
					if ( itemOf(r["reward"].GetString()) < 0 ) ++p.rows_unresolved;
				}
				else if ( stem == "merchants" )
				{
					for ( auto& s : r["sells"].GetArray() )
					{
						std::string k = s["kind"].GetString(), ref = s["ref"].GetString();
						if ( k == "provision" && itemOf(ref) < 0 ) ++p.rows_unresolved;
					}
				}
			}
		}
	}

	inline void tick(Entity* my)
	{
		static const char* env = std::getenv("APX_BH_PROBE");
		if ( !env || !env[0] || !my || my->skill[2] != 0 ) return;
		Probe& p = P();
		if ( p.phase == 0 )
		{
			const char* d = std::getenv("APX_HOLLOW_DIR");
			const char* o = std::getenv("APX_BH_OUT");
			p.dir = d ? d : "data/apx/hollow";
			p.out = o ? o : "";
			p.t0 = ::ticks;
			p.phase = 1;
			return;
		}
		if ( p.phase == 1 && ::ticks - p.t0 >= 100 )
		{
			load(p);
			// the floor's own band (no tag) for floor-1 rats and skeletons (D75: fodder), reported
			for ( int k : { (int)RAT, (int)SKELETON } )
			{
				Entity* e = summonMonster((Monster)k, (long)my->x + 48, (long)my->y + 48, false);
				int t = e ? (int)apx::tiers::tierOf(e) : -2;
				(k == RAT ? p.band_rat : p.band_skeleton) = t;
				if ( e ) e->setHP(0);
			}
			p.phase = 2;
			return;
		}
		if ( p.phase == 2 )
		{
			for ( int n = 0; n < 16 && p.next < p.todo.size(); ++n, ++p.next )
			{
				auto& s = p.todo[p.next];
				const int ring[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 } };
				Entity* e = nullptr;
				for ( int r = 2; r <= 5 && !e; ++r )
					for ( auto& d : ring )
					{
						e = summonMonster((Monster)s.kind, (long)my->x + d[0] * 16 * r, (long)my->y + d[1] * 16 * r, false);
						if ( e ) break;
					}
				Stat* st = e ? e->getStats() : nullptr;
				if ( !e || !st || (int)st->type != s.kind ) { ++p.spawn_failed; printlog("[apx-bh1b] spawn failed %s", s.id.c_str()); continue; }
				++p.spawned;
				if ( s.tier != apx::combat::Tier::Boss )
				{
					apx::tiers::tag(e, s.tier);
					if ( apx::tiers::tierOf(e) != s.tier ) ++p.tier_mismatch;
					apx::tiers::untag(e);
				}
				e->setHP(0);
			}
			if ( p.next >= p.todo.size() ) { p.phase = 3; p.t0 = ::ticks; }
			return;
		}
		if ( p.phase == 3 && ::ticks - p.t0 >= 10 ) finish();
	}
} // namespace hollow
} // namespace apx
