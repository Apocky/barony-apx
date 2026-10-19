// apx/telemetry.cpp -- see telemetry.hpp for the row schemas. BF1-1.
#include "telemetry.hpp"
#include "../main.hpp"
#include "../game.hpp"
#include "../entity.hpp"
#include "../monster.hpp"
#include "../player.hpp"
#include "../prng.hpp"
#include "../prng.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <queue>
#include <string>

#ifdef WINDOWS
#include <direct.h>
#else
#include <sys/stat.h>
#endif

ApxGenStats apxGen;
void apxRngMark(int slot, int gold, int items)
{
	if ( !apx::enabled || slot < 0 || slot >= 16 ) { return; }
	BaronyRNG probe = map_rng;               // copy: drawing from the copy leaves the real stream untouched
	apxGen.marks[slot] = probe.getU64();
	apxGen.markGold[slot] = gold;
	apxGen.markItems[slot] = items;
}
double apxGenMs = 0, apxLoadMs = 0, apxPathMapsMs = 0;

namespace apx
{
	bool enabled = false;
	uint64_t droppedRows = 0, botsStuck = 0, shotsFailed = 0;
	static std::string gOut, gRunId, gBuild, gKnobs;
	static std::chrono::steady_clock::time_point gT0;
	static Uint32 gSeed = 0;
	static std::map<std::string, FILE*> gFiles;

	static void mkdirs(const std::string& p)
	{
		std::string cur;
		for ( size_t i = 0; i <= p.size(); ++i )
		{
			if ( i == p.size() || p[i] == '/' || p[i] == '\\' )
			{
				if ( !cur.empty() && cur.back() != ':' )
				{
#ifdef WINDOWS
					_mkdir(cur.c_str());
#else
					mkdir(cur.c_str(), 0777);
#endif
				}
			}
			if ( i < p.size() ) { cur += p[i]; }
		}
	}

	Uint32 seedBase = 0;
	Uint32 deriveSeed(int floor, bool secret)
	{
		// FNV-1a over (base, floor, secret): one fixed seed per floor, so a whole descent repeats
		Uint32 h = 2166136261u;
		Uint32 in[3] = { seedBase, (Uint32)floor, secret ? 1u : 0u };
		for ( Uint32 v : in ) { for ( int b = 0; b < 4; ++b ) { h ^= (v >> (8 * b)) & 0xff; h *= 16777619u; } }
		return h ? h : 1u;
	}

	void seedRuntimeRngs()
	{
		if ( !seedBase ) { return; }
		Uint32 a = seedBase * 2246822519u + 1u, b = seedBase * 3266489917u + 7u;
		local_rng.seedBytes(&a, sizeof(a));
		net_rng.seedBytes(&b, sizeof(b));
	}
	void applyGameKey() { if ( seedBase ) { uniqueGameKey = seedBase; } }

	double wallMs()
	{
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gT0).count();
	}

	std::string outDir() { return gOut; }

	void init(const char* outDir, bool outGiven)
	{
		const char* t = getenv("APX_TELEM");
		if ( t && !strcmp(t, "0") ) { enabled = false; return; }
		if ( !outGiven && !(t && !strcmp(t, "1")) ) { enabled = false; return; }
		gT0 = std::chrono::steady_clock::now();
		gOut = (outDir && *outDir) ? outDir : "apx-out";
		const char* b = getenv("APX_BUILD_HASH");
		gBuild = b ? b : "unknown";
		const char* k = getenv("APX_KNOBS");
		gKnobs = k ? k : "vanilla";
		time_t now = time(nullptr);
		char buf[64];
		strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", localtime(&now));
		gRunId = std::string(buf) + "-" + gBuild.substr(0, 8);
		mkdirs(gOut);
		enabled = true;
	}

	void close()
	{
		if ( enabled ) { closeEncounter("exit"); }
		for ( auto& f : gFiles ) { if ( f.second ) { fclose(f.second); } }
		gFiles.clear();
		enabled = false;
	}

	void commonFields(ApxRow& r)
	{
		r.str("run_id", gRunId).str("build_hash", gBuild).i("seed", (long long)mapseed)
			.i("floor", currentlevel).i("secret", secretlevel ? 1 : 0).i("depth", secretlevel ? 0 : currentlevel)
			.i("party_n", partyN).i("party_power", 0).str("knob_set", gKnobs).str("scenario", scenarioName).d("bot_skill", botSkill)
			.i("tick", (long long)ticks).d("sim_seconds", ticks / (double)TICKS_PER_SECOND).d("wall_ms", wallMs());
	}

	void write(const char* file, ApxRow& r)
	{
		if ( !enabled ) { return; }
		FILE*& f = gFiles[file];
		if ( !f )
		{
			f = fopen((gOut + "/" + file).c_str(), "ab");
		}
		if ( !f ) { ++droppedRows; return; }
		std::string line = r.done();
		line += "\n";
		if ( fwrite(line.data(), 1, line.size(), f) != line.size() || fflush(f) != 0 ) { ++droppedRows; }
	}

	void onTick()
	{
		simTick();
		encounterTick();
	}

	static int tileAt(int x, int y, int z)
	{
		return map.tiles[z + y * MAPLAYERS + x * MAPLAYERS * map.height];
	}

	// BFS over walkable tiles (floor present, no obstacle); -1 when unreachable
	static int bfs(int sx, int sy, int tx, int ty)
	{
		int w = map.width, h = map.height;
		if ( sx < 0 || sy < 0 || sx >= w || sy >= h ) { return -1; }
		std::vector<int> dist((size_t)w * h, -1);
		std::queue<std::pair<int, int>> q;
		dist[sx + (size_t)sy * w] = 0;
		q.push({ sx, sy });
		while ( !q.empty() )
		{
			auto p = q.front(); q.pop();
			int d = dist[p.first + (size_t)p.second * w];
			if ( abs(p.first - tx) + abs(p.second - ty) <= 1 ) { return d + 1; }
			static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
			for ( int k = 0; k < 4; ++k )
			{
				int nx = p.first + dx[k], ny = p.second + dy[k];
				if ( nx < 0 || ny < 0 || nx >= w || ny >= h ) { continue; }
				if ( dist[nx + (size_t)ny * w] >= 0 ) { continue; }
				if ( !tileAt(nx, ny, 0) || tileAt(nx, ny, OBSTACLELAYER) ) { continue; }
				dist[nx + (size_t)ny * w] = d + 1;
				q.push({ nx, ny });
			}
		}
		return -1;
	}

	void onFloorLoaded()
	{
		++floorsLoaded;
		if ( !enabled || multiplayer == CLIENT ) { return; }
		closeEncounter("floor");
		ApxRow r;
		commonFields(r);
		lastFloorHash = apxFloorHash();
		r.hex64("floor_hash", lastFloorHash);
		r.i("rooms", apxGen.rooms).i("monsters", apxGen.monsters).i("gold", apxGen.gold)
			.i("items", apxGen.items).i("decorations", apxGen.decorations)
			.i("map_w", map.width).i("map_h", map.height);
		long wall = 0, floorT = 0, water = 0, lava = 0;
		for ( int x = 0; x < (int)map.width; ++x )
		{
			for ( int y = 0; y < (int)map.height; ++y )
			{
				int f = tileAt(x, y, 0);
				if ( tileAt(x, y, OBSTACLELAYER) ) { ++wall; }
				if ( f )
				{
					++floorT;
					if ( swimmingtiles && swimmingtiles[f] ) { ++water; }
					if ( lavatiles && lavatiles[f] ) { ++lava; }
				}
			}
		}
		std::map<std::string, int> byType;
		int doors = 0, chests = 0, ladderX = -1, ladderY = -1;
		std::vector<std::pair<real_t, real_t>> mons;
		for ( node_t* n = map.entities->first; n; n = n->next )
		{
			Entity* e = (Entity*)n->element;
			if ( !e ) { continue; }
			if ( e->behavior == &actDoor ) { ++doors; }
			else if ( e->behavior == &actChest ) { ++chests; }
			else if ( e->behavior == &actLadder ) { ladderX = (int)(e->x / 16); ladderY = (int)(e->y / 16); }
			else if ( e->behavior == &actMonster )
			{
				int t = (int)e->getMonsterTypeFromSprite();
				if ( t > 0 && t < NUMMONSTERS ) { byType[monstertypename[t]]++; }
				else { byType["unknown"]++; }
				mons.push_back({ e->x / 16, e->y / 16 });
			}
		}
		std::string bt = "{";
		bool f1 = true;
		for ( auto& kv : byType )
		{
			if ( !f1 ) { bt += ","; }
			f1 = false;
			bt += "\"" + kv.first + "\":" + std::to_string(kv.second);
		}
		bt += "}";
		r.i("wall_tiles", wall).i("floor_tiles", floorT).i("water_tiles", water).i("lava_tiles", lava)
			.i("door_count", doors).i("chest_count", chests).raw("monsters_by_type", bt)
			.d("gen_ms", apxGenMs).d("load_ms", apxLoadMs).d("path_maps_ms", apxPathMapsMs);
		int ladderDist = -1;
		double nearest = -1;
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			if ( players[p] && players[p]->entity )
			{
				real_t px = players[p]->entity->x / 16, py = players[p]->entity->y / 16;
				if ( ladderX >= 0 ) { ladderDist = bfs((int)px, (int)py, ladderX, ladderY); }
				for ( auto& m : mons )
				{
					double d = sqrt((m.first - px) * (m.first - px) + (m.second - py) * (m.second - py));
					if ( nearest < 0 || d < nearest ) { nearest = d; }
				}
				break;
			}
		}
		{
			std::string mk = "[";
			for ( int k = 1; k <= 8; ++k )
			{
				char b[96];
				snprintf(b, sizeof(b), "%s[\"%016llx\",%u,%u]", k > 1 ? "," : "", (unsigned long long)apxGen.marks[k], apxGen.markGold[k], apxGen.markItems[k]);
				mk += b;
			}
			r.raw("rng_marks", mk + "]");
		}
		r.i("uid_at_gen", apxGen.uidAtGen).i("entities_at_gen", apxGen.entitiesAtGen)
			.i("ladder_dist_tiles", ladderDist).d("spawn_to_nearest_monster_tiles", nearest)
			.i("dropped_rows", (long long)droppedRows).i("bots_stuck", (long long)botsStuck)
			.i("shots_failed", (long long)shotsFailed);
		write("floors.jsonl", r);
	}
}

void ApxRow::key(const char* k)
{
	if ( !first ) { s += ","; }
	first = false;
	s += "\"";
	s += k;
	s += "\":";
}
ApxRow& ApxRow::i(const char* k, long long v) { key(k); s += std::to_string(v); return *this; }
ApxRow& ApxRow::d(const char* k, double v)
{
	key(k);
	char b[64];
	snprintf(b, sizeof(b), "%.3f", v);
	s += b;
	return *this;
}
ApxRow& ApxRow::str(const char* k, const std::string& v)
{
	key(k);
	s += "\"";
	for ( unsigned char c : v )
	{
		if ( c == '"' || c == '\\' ) { s += '\\'; s += (char)c; }
		else if ( c < 32 || c > 126 ) { s += '?'; }
		else { s += (char)c; }
	}
	s += "\"";
	return *this;
}
ApxRow& ApxRow::hex64(const char* k, uint64_t v)
{
	char b[24];
	snprintf(b, sizeof(b), "\"%016llx\"", (unsigned long long)v);
	key(k);
	s += b;
	return *this;
}
ApxRow& ApxRow::raw(const char* k, const std::string& json) { key(k); s += json; return *this; }
ApxRow& ApxRow::intList(const char* k, const std::vector<int>& v)
{
	key(k);
	s += "[";
	for ( size_t j = 0; j < v.size(); ++j ) { if ( j ) { s += ","; } s += std::to_string(v[j]); }
	s += "]";
	return *this;
}
std::string ApxRow::done() { return s + "}"; }
