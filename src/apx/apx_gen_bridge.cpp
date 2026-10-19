// apx-gen bridge (see apx_gen_bridge.hpp). Flow per floor:
//  1. the level set's template map is already loaded (generateDungeon did it): read its floor,
//     wall and ceiling textures (the band's look), name, skybox and flags;
//  2. apx::generate + apx::dress_floor on (seed, size) -> tiles, rooms, entity rows, dress rows;
//  3. write the tiles as a Barony .lmp with no entities and reload it through loadMap, so every
//     size-dependent buffer (vismaps, lightmaps, shoparea, minimap) is sized by Barony's own code;
//  4. create the entity rows the vanilla generator would create (player starts, ladder, doors,
//     torches, monsters, items, gold, chests, keys, boss, merchant, table/chair fixtures);
//  5. in-engine W2 check on Barony's own tiles and door entities, and a floor hash (W5).
#include "../main.hpp"
#include "../game.hpp"
#include "../stat.hpp"
#include "../entity.hpp"
#include "../files.hpp"
#include "../items.hpp"
#include "../prng.hpp"
#include "../monster.hpp"
#include "../net.hpp"
#include "apx_gen_bridge.hpp"
#include "gen/include/apxgen/apxgen.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <new>
#include <set>
#include <vector>
#ifdef WINDOWS
#include <direct.h>
#define apx_getcwd _getcwd
#else
#include <unistd.h>
#define apx_getcwd getcwd
#endif

#ifndef APXGEN_VENDOR_ROOT
#define APXGEN_VENDOR_ROOT "."
#endif

ApxGenConfig apxgen;

void apxgenReadEnvironment()
{
	if ( const char* v = getenv("APXGEN") ) { apxgen.enabled = atoi(v) != 0; }
	if ( const char* v = getenv("APXGEN_SIZE") ) { apxgen.size = atoi(v); }
	if ( const char* v = getenv("APXGEN_POP") ) { apxgen.populationPct = atoi(v); }
	if ( const char* v = getenv("APXGEN_ROOT") ) { apxgen.kitsRoot = v; }
	if ( const char* v = getenv("APXGEN_CONTROL") ) { apxgen.control = atoi(v); }
}

bool apxgenParseArgument(const char* a)
{
	if ( !strcmp(a, "-apxgen") ) { apxgen.enabled = true; return true; }
	if ( !strncmp(a, "-apxgen-size=", 13) ) { apxgen.enabled = true; apxgen.size = atoi(a + 13); return true; }
	if ( !strncmp(a, "-apxgen-pop=", 12) ) { apxgen.populationPct = atoi(a + 12); return true; }
	if ( !strncmp(a, "-apxgen-root=", 13) ) { apxgen.kitsRoot = a + 13; return true; }
	if ( !strcmp(a, "-apxhost") ) { apxgen.lobby = 1; return true; }
	if ( !strncmp(a, "-apxjoin=", 9) ) { apxgen.lobby = 2; apxgen.joinAddress = a + 9; return true; }
	return false;
}

int apxgenWriteNet(unsigned char* p)
{
	p[0] = 'A'; p[1] = 'P'; p[2] = 'X';
	p[3] = apxgen.enabled ? 1 : 0;
	p[4] = (unsigned char)(apxgen.size & 0xff); p[5] = (unsigned char)((apxgen.size >> 8) & 0xff);
	p[6] = (unsigned char)(apxgen.populationPct & 0xff); p[7] = (unsigned char)((apxgen.populationPct >> 8) & 0xff);
	return 8;
}

void apxgenReadNet(const unsigned char* p, int len)
{
	if ( len < 8 || p[0] != 'A' || p[1] != 'P' || p[2] != 'X' )
	{
		if ( apxgen.enabled )
		{
			apxgen.enabled = false; // the host runs the vanilla generator: so must we
			printlog("apxgen: host sent no apx-gen settings; this client uses the vanilla generator");
		}
		return;
	}
	apxgen.enabled = p[3] != 0;
	apxgen.size = p[4] | (p[5] << 8);
	apxgen.populationPct = p[6] | (p[7] << 8);
	printlog("apxgen: settings from host: enabled %d size %d pop %d", (int)apxgen.enabled, apxgen.size, apxgen.populationPct);
}

namespace {

struct Library
{
	bool loaded = false, ok = false;
	std::string err;
	apx::Grammar grammar;
	std::vector<apx::Program> programs;
	apx::Params params;
	apx::DressLib dress;
};

Library& library()
{
	static Library lib;
	if ( lib.loaded ) { return lib; }
	lib.loaded = true;
	std::string root = apxgen.kitsRoot.empty() ? std::string(APXGEN_VENDOR_ROOT) : apxgen.kitsRoot;
	if ( !apx::load_grammar(root + "/grammar/default.grammar", lib.grammar, lib.err) ) { return lib; }
	if ( !apx::load_programs({ root + "/kits/rooms-apockalypsis", root + "/kits/rooms" }, lib.programs, lib.err) ) { return lib; }
	if ( !apx::load_districts(root + "/kits/districts.toml", lib.params, lib.err) ) { return lib; }
	if ( !apx::load_dress(root + "/kits", lib.dress, lib.err) ) { return lib; }
	lib.ok = true;
	printlog("apxgen: library %s: %d room programs, %d fixtures, %d function programs\n", root.c_str(),
		(int)lib.programs.size(), (int)lib.dress.fixtures.size(), (int)lib.dress.functions.size());
	return lib;
}

int modeOf(const std::map<int, int>& counts)
{
	int best = 0, bestN = -1;
	for ( auto& p : counts ) { if ( p.second > bestN ) { best = p.first; bestN = p.second; } }
	return best;
}

struct Textures { int floor = 0, wall = 0, ceiling = 0, water = 0, lava = 0; };

// The band's look comes from the level set's own template map (mine, swamp, labyrinth, ruins, hell...).
Textures bandTextures()
{
	std::map<int, int> f, w, c, wat, lav;
	for ( Uint32 x = 0; x < map.width; ++x )
	{
		for ( Uint32 y = 0; y < map.height; ++y )
		{
			const Sint32* t = &map.tiles[y * MAPLAYERS + x * MAPLAYERS * map.height];
			if ( t[1] ) { ++w[t[1]]; }
			else if ( t[0] && !(swimmingtiles && swimmingtiles[t[0]]) && !(lavatiles && lavatiles[t[0]]) ) { ++f[t[0]]; }
			if ( t[2] ) { ++c[t[2]]; }
			if ( !t[1] && t[0] && lavatiles && lavatiles[t[0]] ) { ++lav[t[0]]; }
			else if ( !t[1] && t[0] && swimmingtiles && swimmingtiles[t[0]] ) { ++wat[t[0]]; }
		}
	}
	Textures tx;
	tx.floor = modeOf(f);
	tx.wall = modeOf(w);
	tx.ceiling = modeOf(c);
	// liquids: the template's own water and lava first (the first swimmable tile overall is not
	// the band's water: index 22 rendered as a red checker in the mines), else the first global one
	tx.water = modeOf(wat);
	tx.lava = modeOf(lav);
	for ( Uint32 i = 1; i < numtiles; ++i )
	{
		if ( !tx.water && swimmingtiles && swimmingtiles[i] && !(lavatiles && lavatiles[i]) ) { tx.water = i; }
		if ( !tx.lava && lavatiles && lavatiles[i] ) { tx.lava = i; }
	}
	return tx;
}

bool writeLmp(const std::string& path, int W, int H, const std::vector<Sint32>& tiles,
	const char* name, Uint32 skybox, const Sint32* flags)
{
	FILE* fp = fopen(path.c_str(), "wb");
	if ( !fp ) { return false; }
	char nm[32] = {0}, author[32] = "apx-gen";
	strncpy(nm, name, 31);
	Uint32 w = W, h = H, zero = 0;
	fwrite("BARONY LMPV3.2", 1, 14, fp);
	fwrite(nm, 1, 32, fp);
	fwrite(author, 1, 32, fp);
	fwrite(&w, 4, 1, fp);
	fwrite(&h, 4, 1, fp);
	fwrite(&skybox, 4, 1, fp);
	fwrite(flags, sizeof(Sint32), MAPFLAGS, fp);
	fwrite(tiles.data(), sizeof(Sint32), tiles.size(), fp);
	fwrite(&zero, 4, 1, fp);
	return fclose(fp) == 0;
}

inline Sint32& tileAt(int x, int y, int z) { return map.tiles[z + y * MAPLAYERS + x * MAPLAYERS * map.height]; }
inline bool isWall(int x, int y)
{
	if ( x < 0 || y < 0 || x >= (int)map.width || y >= (int)map.height ) { return true; }
	return tileAt(x, y, 1) != 0;
}
inline bool isWalkTile(int x, int y)
{
	if ( isWall(x, y) ) { return false; }
	Sint32 f = tileAt(x, y, 0);
	return f != 0 && !(lavatiles && lavatiles[f]);
}

uint64_t fnv(uint64_t h, uint64_t v)
{
	for ( int i = 0; i < 8; ++i ) { h ^= (v >> (i * 8)) & 0xff; h *= 1099511628211ULL; }
	return h;
}

struct Placed { int x, y; };

} // namespace

int apxgenGenerateDungeon(const char* levelset, uint32_t seed)
{
	using clk = std::chrono::steady_clock;
	auto t0 = clk::now();
	Library& lib = library();
	if ( !lib.ok )
	{
		printlog("apxgen: FAILED library load: %s\n", lib.err.c_str());
		return -1;
	}

	// 1. band look from the template
	Textures tx = bandTextures();
	char tmplName[32] = {0}, tmplFile[sizeof(map.filename)] = {0};
	strncpy(tmplName, map.name, 31);
	memcpy(tmplFile, map.filename, sizeof(tmplFile));
	Uint32 skybox = map.skybox;
	Sint32 flags[MAPFLAGS];
	memcpy(flags, map.flags, sizeof(flags));
	if ( !tx.wall || !tx.floor )
	{
		printlog("apxgen: FAILED template '%s' has no wall/floor texture (wall %d floor %d)\n", levelset, tx.wall, tx.floor);
		return -1;
	}

	// 2. generate
	apx::Params P = lib.params;
	P.size = std::max(48, std::min(160, apxgen.size));
	apx::Result r = apx::generate((uint64_t)seed, lib.grammar, lib.programs, P);
	if ( !r.ok )
	{
		printlog("apxgen: FAILED seed %u size %d: %s\n", seed, P.size, r.verdict.why.c_str());
		return -1;
	}
	apx::Floor& f = r.floor;
	apx::dress_floor(f, lib.programs, lib.dress, (uint64_t)seed);
	auto t1 = clk::now();

	// 3. tiles -> .lmp -> loadMap
	const int W = f.W, H = f.H;
	std::vector<Sint32> tiles((size_t)W * H * MAPLAYERS, 0);
	int nWater = 0, nLava = 0, nPit = 0;
	for ( int x = 0; x < W; ++x )
	{
		for ( int y = 0; y < H; ++y )
		{
			Sint32* t = &tiles[(size_t)y * MAPLAYERS + (size_t)x * MAPLAYERS * H];
			const size_t i = f.idx(x, y);
			const bool border = x == 0 || y == 0 || x == W - 1 || y == H - 1;
			t[0] = tx.floor;
			t[1] = (f.tile[i] == apx::FLOOR && !border) ? 0 : tx.wall;
			t[2] = tx.ceiling;
			const uint8_t ft = f.feat.empty() ? 0 : f.feat[i];
			if ( t[1] == 0 )
			{
				if ( ft == apx::F_WATER && tx.water ) { t[0] = tx.water; ++nWater; }
			}
			else if ( ft == apx::F_DEEP || ft == apx::F_LAVA || ft == apx::F_CHASM )
			{
				// blocking features are out of the walkable set in apx; in Barony they are open tiles
				// that block walking: deep water, lava, or a pit (floor 0)
				t[1] = 0;
				if ( ft == apx::F_DEEP && tx.water ) { t[0] = tx.water; ++nWater; }
				else if ( ft == apx::F_LAVA && tx.lava ) { t[0] = tx.lava; ++nLava; }
				else { t[0] = 0; ++nPit; }
			}
		}
	}
	char cwd[1024] = ".";
	if ( !apx_getcwd(cwd, sizeof(cwd)) ) { strcpy(cwd, "."); }
	std::string lmp = std::string(cwd) + "/apxgen_floor.lmp";
	for ( auto& ch : lmp ) { if ( ch == '\\' ) { ch = '/'; } }
	if ( !writeLmp(lmp, W, H, tiles, tmplName, skybox, flags) )
	{
		printlog("apxgen: FAILED writing %s\n", lmp.c_str());
		return -1;
	}
	if ( loadMap(lmp.c_str(), &map, map.entities, map.creatures, nullptr) == -1 )
	{
		printlog("apxgen: FAILED loadMap %s\n", lmp.c_str());
		return -1;
	}
	memcpy(map.filename, tmplFile, sizeof(map.filename)); // band checks (fortress, ...) read it
	strncpy(map.name, tmplName, sizeof(map.name) - 1);
	map.trapexcludelocations = (bool*)calloc((size_t)W * H, sizeof(bool));
	map.monsterexcludelocations = (bool*)calloc((size_t)W * H, sizeof(bool));
	map.lootexcludelocations = (bool*)calloc((size_t)W * H, sizeof(bool));
	auto t2 = clk::now();

	// 4. entities
	std::vector<char> used((size_t)W * H, 0);
	auto at = [&](int sprite, int x, int y, list_t* creatures = nullptr) {
		Entity* e = newEntity(sprite, 1, map.entities, creatures);
		e->x = x * 16;
		e->y = y * 16;
		used[(size_t)y * W + x] = 1;
		return e;
	};
	const int entranceNode = f.mission.find(apx::Kind::Entrance);
	auto roomOf = [&](int node) -> const apx::RoomRow* {
		for ( auto& ro : f.rooms ) { if ( ro.node == node ) { return &ro; } }
		return nullptr;
	};
	auto inRoom = [&](const apx::RoomRow* ro, int x, int y) {
		return ro && x >= ro->x0 && x < ro->x1 && y >= ro->z0 && y < ro->z1;
	};
	// nearest free walkable tiles to (x, y), BFS order (deterministic)
	auto nearFree = [&](int sx, int sy, int n) {
		std::vector<Placed> out;
		std::vector<char> seen((size_t)W * H, 0);
		std::deque<Placed> q;
		q.push_back({ sx, sy });
		seen[(size_t)sy * W + sx] = 1;
		while ( !q.empty() && (int)out.size() < n )
		{
			Placed p = q.front(); q.pop_front();
			if ( isWalkTile(p.x, p.y) && !used[(size_t)p.y * W + p.x] ) { out.push_back(p); }
			static const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
			for ( int d = 0; d < 4; ++d )
			{
				int nx = p.x + DX[d], ny = p.y + DY[d];
				if ( nx < 0 || ny < 0 || nx >= W || ny >= H || seen[(size_t)ny * W + nx] || isWall(nx, ny) ) { continue; }
				seen[(size_t)ny * W + nx] = 1;
				q.push_back({ nx, ny });
			}
		}
		return out;
	};

	int nStarts = 0, nDoors = 0, nLocked = 0, nTorches = 0, nChests = 0, nKeys = 0, nBoss = 0, nMerchant = 0,
		nMonsters = 0, nItems = 0, nGold = 0, nFixtures = 0, nUnmapped = 0;
	std::vector<std::pair<int, Placed>> keyAt, lockAt; // (lock id, tile)
	Placed ladder = { -1, -1 }, start = { -1, -1 };

	// doors. apx-gen has no door tiles: a room's door_x/z is its centre and the critic treats the
	// whole box of a Lock room as the gate. So the openings are found on the ring of tiles just
	// outside each box: a run of walkable ring tiles on one side is one opening. A Lock room gets a
	// locked door in every opening, the opening walled down to one tile (Barony doors are one tile
	// wide; counted as narrowed); an ordinary room gets an open door where an opening is already one
	// tile wide. The in-engine W2 below re-checks reach on the resulting tiles.
	int nNarrowed = 0, nLockOpenings = 0, nLockUnsealed = 0, nWallRejected = 0;
	// a narrowing wall is kept only if the floor stays one piece: the start's component loses exactly
	// that tile (doors count as open). A wall that would cut a route is reverted and counted.
	Placed origin = { -1, -1 };
	if ( const apx::RoomRow* er = roomOf(entranceNode) )
	{
		for ( int y = er->z0; y < er->z1 && origin.x < 0; ++y )
		{
			for ( int x = er->x0; x < er->x1 && origin.x < 0; ++x ) { if ( isWalkTile(x, y) ) { origin = { x, y }; } }
		}
	}
	std::vector<char> seenR((size_t)W * H);
	auto reachCount = [&]() {
		if ( origin.x < 0 || !isWalkTile(origin.x, origin.y) ) { return -1; }
		std::fill(seenR.begin(), seenR.end(), 0);
		std::vector<int> st{ origin.y * W + origin.x };
		seenR[st[0]] = 1;
		int n = 0;
		while ( !st.empty() )
		{
			int c = st.back(); st.pop_back(); ++n;
			int cx = c % W, cy = c / W;
			static const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
			for ( int d = 0; d < 4; ++d )
			{
				int nx = cx + DX[d], ny = cy + DY[d], ni = ny * W + nx;
				if ( seenR[ni] || !isWalkTile(nx, ny) ) { continue; }
				seenR[ni] = 1;
				st.push_back(ni);
			}
		}
		return n;
	};
	int reachBase = reachCount();
	auto wallIfSafe = [&](int x, int y) {
		if ( isWall(x, y) ) { return false; }
		const Sint32 old = tileAt(x, y, 1);
		tileAt(x, y, 1) = tx.wall;
		const int n = reachCount();
		if ( reachBase >= 0 && n == reachBase - 1 ) { reachBase = n; ++nNarrowed; return true; }
		tileAt(x, y, 1) = old;
		++nWallRejected;
		return false;
	};
	for ( auto& ro : f.rooms )
	{
		const bool lock = f.mission.nodes[ro.node].kind == apx::Kind::Lock;
		// side 0 north (z0-1), 1 south (z1), 2 west (x0-1), 3 east (x1); (ix,iy) inward step
		for ( int side = 0; side < 4; ++side )
		{
			const bool horiz = side < 2;
			const int n = horiz ? ro.x1 - ro.x0 : ro.z1 - ro.z0;
			const int ix = side == 2 ? 1 : side == 3 ? -1 : 0, iy = side == 0 ? 1 : side == 1 ? -1 : 0;
			auto ringTile = [&](int k) {
				return horiz ? Placed{ ro.x0 + k, side == 0 ? ro.z0 - 1 : ro.z1 } : Placed{ side == 2 ? ro.x0 - 1 : ro.x1, ro.z0 + k };
			};
			for ( int k = 0; k < n; )
			{
				Placed p0 = ringTile(k);
				if ( p0.x <= 0 || p0.y <= 0 || p0.x >= W - 1 || p0.y >= H - 1 || !isWalkTile(p0.x, p0.y) ) { ++k; continue; }
				int k1 = k;
				while ( k1 < n && isWalkTile(ringTile(k1).x, ringTile(k1).y) ) { ++k1; }
				// candidates: ring tiles that lead into the box and out of it
				std::vector<int> cand;
				for ( int c = k; c < k1; ++c )
				{
					Placed q = ringTile(c);
					if ( isWalkTile(q.x + ix, q.y + iy) && isWalkTile(q.x - ix, q.y - iy) ) { cand.push_back(c); }
				}
				if ( !cand.empty() && (lock || k1 - k == 1) )
				{
					const int pick = cand[cand.size() / 2];
					if ( lock )
					{
						++nLockOpenings;
						for ( int c = k; c < k1; ++c )
						{
							if ( c == pick ) { continue; }
							Placed q = ringTile(c);
							wallIfSafe(q.x, q.y);
						}
					}
					Placed q = ringTile(pick);
					if ( lock )
					{
						// the ring's corner tiles sit beside its end tiles: close them too
						const int lx = horiz ? 1 : 0, ly = horiz ? 0 : 1;
						for ( int sgn = -1; sgn <= 1; sgn += 2 )
						{
							const int cx = q.x + sgn * lx, cy = q.y + sgn * ly;
							if ( cx > 0 && cy > 0 && cx < W - 1 && cy < H - 1 && !used[(size_t)cy * W + cx] ) { wallIfSafe(cx, cy); }
						}
					}
					// the ring runs along x on north/south sides: walls east-west of the door, passage along y
					const int sprite = horiz ? 3 : 2;
					const bool shaped = horiz ? (isWall(q.x - 1, q.y) && isWall(q.x + 1, q.y)) : (isWall(q.x, q.y - 1) && isWall(q.x, q.y + 1));
					if ( shaped && !used[(size_t)q.y * W + q.x] )
					{
						Entity* d = at(sprite, q.x, q.y);
						d->doorForceLockedUnlocked = lock ? 1 : 2;
						++nDoors;
						if ( lock ) { ++nLocked; lockAt.push_back({ f.mission.nodes[ro.node].id, q }); }
					}
					else if ( lock )
					{
						++nLockUnsealed;
						printlog("apxgen: lock %d opening at %d,%d (side %d) could not take a door\n", f.mission.nodes[ro.node].id, q.x, q.y, side);
					}
				}
				else if ( lock )
				{
					// no straight-through ring tile: the corridor runs along the outside of the wall and
					// turns in from the side. The door goes one tile inside the box edge, its passage
					// pointing into the room; the run's other entries are walled on the box edge.
					std::vector<int> entries;
					for ( int c = k; c < k1; ++c )
					{
						Placed q = ringTile(c);
						if ( isWalkTile(q.x + ix, q.y + iy) ) { entries.push_back(c); }
					}
					int pick = -1;
					for ( size_t e = 0; e < entries.size() && pick < 0; ++e )
					{
						// prefer the middle entry; take the first whose tile behind the door is open
						const int c = entries[(entries.size() / 2 + e) % entries.size()];
						Placed q = ringTile(c);
						if ( isWalkTile(q.x + 2 * ix, q.y + 2 * iy) ) { pick = c; }
					}
					if ( !entries.empty() )
					{
						++nLockOpenings;
						bool placed = false;
						if ( pick >= 0 )
						{
							for ( int c : entries )
							{
								if ( c == pick ) { continue; }
								Placed q = ringTile(c);
								wallIfSafe(q.x + ix, q.y + iy);
							}
							Placed rq = ringTile(pick);
							Placed d = { rq.x + ix, rq.y + iy };
							const int lx = horiz ? 1 : 0, ly = horiz ? 0 : 1;
							for ( int sgn = -1; sgn <= 1; sgn += 2 )
							{
								const int cx = d.x + sgn * lx, cy = d.y + sgn * ly;
								if ( !used[(size_t)cy * W + cx] ) { wallIfSafe(cx, cy); }
							}
							const bool shaped = horiz ? (isWall(d.x - 1, d.y) && isWall(d.x + 1, d.y)) : (isWall(d.x, d.y - 1) && isWall(d.x, d.y + 1));
							if ( shaped && !used[(size_t)d.y * W + d.x] && isWalkTile(d.x, d.y) )
							{
								Entity* de = at(horiz ? 3 : 2, d.x, d.y);
								de->doorForceLockedUnlocked = 1;
								++nDoors; ++nLocked;
								lockAt.push_back({ f.mission.nodes[ro.node].id, d });
								placed = true;
							}
						}
						if ( !placed )
						{
							++nLockUnsealed;
							printlog("apxgen: lock %d side opening at ring %d,%d (side %d) could not take a door\n",
								f.mission.nodes[ro.node].id, ringTile(k).x, ringTile(k).y, side);
						}
					}
				}
				k = k1;
			}
		}
	}

	// mission rows (apx emits one entity row per non-Room node; here each lands inside its room)
	for ( auto& ro : f.rooms )
	{
		const apx::Node& n = f.mission.nodes[ro.node];
		const int cx = (ro.x0 + ro.x1) / 2, cy = (ro.z0 + ro.z1) / 2;
		auto spot = nearFree(cx, cy, 1);
		if ( spot.empty() ) { continue; }
		Placed p = spot[0];
		switch ( n.kind )
		{
			case apx::Kind::Entrance:
			{
				auto s = nearFree(cx, cy, MAXPLAYERS);
				for ( auto& q : s ) { at(1, q.x, q.y); ++nStarts; }
				start = s.empty() ? p : s[0];
				break;
			}
			case apx::Kind::Exit:
			{
				Entity* l = at(11, p.x, p.y);
				l->behavior = &actLadder;
				ladder = p;
				break;
			}
			case apx::Kind::Key:
			{
				Placed kp = p;
				if ( apxgen.control == 1 )
				{
					// CONTROL: the key goes into its own lock's room; the in-engine W2 must fail
					for ( auto& lr : f.rooms )
					{
						if ( f.mission.nodes[lr.node].kind == apx::Kind::Lock && f.mission.nodes[lr.node].id == n.id )
						{
							auto s = nearFree((lr.x0 + lr.x1) / 2, (lr.z0 + lr.z1) / 2, 1);
							if ( !s.empty() ) { kp = s[0]; }
						}
					}
				}
				Entity* k = at(8, kp.x, kp.y);
				setSpriteAttributes(k, nullptr, nullptr);
				k->skill[10] = TOOL_SKELETONKEY + 2;
				k->skill[11] = 3;
				k->skill[12] = 0;
				k->skill[13] = 1;
				keyAt.push_back({ n.id, kp });
				++nKeys; ++nItems;
				break;
			}
			case apx::Kind::Treasure:
			{
				at(21, p.x, p.y); ++nChests;
				break;
			}
			case apx::Kind::Boss:
			{
				// MINOTAUR from floor 5; earlier a random monster of the band's curve stands in
				Entity* b = at(currentlevel >= 5 ? 77 : 10, p.x, p.y, map.creatures);
				b->skill[5] = nMonsters++;
				++nBoss;
				break;
			}
			case apx::Kind::Haven:
			{
				Entity* m = at(35, p.x, p.y, map.creatures); // shopkeeper (merchant)
				m->skill[5] = nMonsters++;
				++nMerchant;
				break;
			}
			case apx::Kind::Secret:
			{
				Entity* g = at(9, p.x, p.y); g->goldAmount = 0; ++nGold;
				break;
			}
			case apx::Kind::Nexus:
			{
				at(14, p.x, p.y); ++nFixtures; // fountain
				break;
			}
			case apx::Kind::Stronghold: // faction seat: no Barony counterpart yet
				++nUnmapped;
				break;
			default:
				break;
		}
	}
	if ( start.x < 0 )
	{
		printlog("apxgen: FAILED no entrance room\n");
		return -1;
	}

	// torches. Rooms: one per TORCH_FACES wall faces (at least 2), faces walked in perimeter order
	// (sorted by angle round the room centre) so they spread along all walls. Corridors: one per
	// TORCH_CORRIDOR corridor tiles. The first W1 pictures (one per 40 floor tiles, no corridor
	// torches) measured luma 7-13 against vanilla's 52 (eye.py), i.e. unlit halls.
	constexpr int TORCH_FACES = 6, TORCH_CORRIDOR = 12;
	apx::Rng rng((uint64_t)seed ^ 0x7a7c4e5ULL);
	extern int apxgenHallX, apxgenHallY, apxgenHallTiles;
	apxgenHallTiles = 0;
	std::vector<Placed> lightAt; // tiles lit by a torch or light source
	int nLights = 0;
	auto torchAt = [&](const Placed& p, int s) {
		Entity* t = newEntity(s, 1, map.entities, nullptr);
		t->x = p.x * 16 + (s == 4 ? 0 : s == 6 ? 16 : 8);
		t->y = p.y * 16 + (s == 5 ? 0 : s == 7 ? 16 : 8);
		++nTorches;
		lightAt.push_back(p);
	};
	std::vector<char> inBox((size_t)W * H, 0);
	for ( auto& ro : f.rooms )
	{
		std::vector<std::pair<Placed, int>> faces; // tile, side (4 west 5 north 6 east 7 south)
		int floorTiles = 0;
		for ( int y = ro.z0; y < ro.z1; ++y )
		{
			for ( int x = ro.x0; x < ro.x1; ++x )
			{
				if ( x >= 0 && y >= 0 && x < W && y < H ) { inBox[(size_t)y * W + x] = 1; }
				if ( !isWalkTile(x, y) ) { continue; }
				++floorTiles;
				if ( isWall(x - 1, y) ) { faces.push_back({ { x, y }, 4 }); }
				if ( isWall(x, y - 1) ) { faces.push_back({ { x, y }, 5 }); }
				if ( isWall(x + 1, y) ) { faces.push_back({ { x, y }, 6 }); }
				if ( isWall(x, y + 1) ) { faces.push_back({ { x, y }, 7 }); }
			}
		}
		if ( floorTiles > apxgenHallTiles )
		{
			auto c = nearFree((ro.x0 + ro.x1) / 2, (ro.z0 + ro.z1) / 2, 1);
			if ( !c.empty() ) { apxgenHallTiles = floorTiles; apxgenHallX = c[0].x; apxgenHallY = c[0].y; }
		}
		if ( faces.empty() ) { continue; }
		const int cx2 = ro.x0 + ro.x1, cy2 = ro.z0 + ro.z1; // twice the centre, integer
		auto octantKey = [&](const std::pair<Placed, int>& a) {
			// integer pseudo-angle (0..8) of the face's wall point round the centre: deterministic
			const long long dx = 2LL * a.first.x + (a.second == 4 ? -1 : a.second == 6 ? 1 : 0) - cx2;
			const long long dy = 2LL * a.first.y + (a.second == 5 ? -1 : a.second == 7 ? 1 : 0) - cy2;
			const long long ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
			const long long den = ax + ay ? ax + ay : 1;
			long long t = dy * 1000000 / den; // -1e6..1e6 along the half
			return dx >= 0 ? (dy >= 0 ? t : 4000000 + t) : 2000000 - t;
		};
		std::stable_sort(faces.begin(), faces.end(), [&](const std::pair<Placed, int>& a, const std::pair<Placed, int>& b) {
			return octantKey(a) < octantKey(b);
		});
		int want = std::max(2, (int)faces.size() / TORCH_FACES);
		const size_t step = std::max<size_t>(1, faces.size() / want), off = rng.next(step);
		for ( size_t i = off; i < faces.size() && want > 0; i += step, --want )
		{
			torchAt(faces[i].first, faces[i].second);
		}
	}
	int corridorRun = 0;
	for ( int y = 1; y < H - 1; ++y )
	{
		for ( int x = 1; x < W - 1; ++x )
		{
			if ( inBox[(size_t)y * W + x] || !isWalkTile(x, y) || used[(size_t)y * W + x] ) { continue; }
			if ( ++corridorRun < TORCH_CORRIDOR ) { continue; }
			const int side = isWall(x - 1, y) ? 4 : isWall(x, y - 1) ? 5 : isWall(x + 1, y) ? 6 : isWall(x, y + 1) ? 7 : 0;
			if ( !side ) { continue; }
			torchAt({ x, y }, side);
			corridorRun = 0;
		}
	}

	// room interiors far from every wall torch (lakes, big halls) get free-standing light sources:
	// greedily at the open tile farthest from any light while that distance exceeds LIGHT_COVER
	constexpr int LIGHT_COVER = 3, LIGHTS_PER_ROOM = 12; // 5/6 left start rooms at luma 14 vs vanilla 52 (eye.py, apx-gen 7057f3d9 floors)
	lightAt.push_back(start); // the arrival tile always counts as lit: see below
	{
		Entity* ls = newEntity(131, 1, map.entities, nullptr);
		ls->x = start.x * 16; ls->y = start.y * 16;
		ls->lightSourceAlwaysOn = 1; ls->lightSourceBrightness = 160; ls->lightSourceRadius = 6; ls->lightSourceFlicker = 0;
		ls->lightSourceRGB = 0xff | (0xc8 << 8) | (0x96 << 16);
		++nLights;
	}
	for ( auto& ro : f.rooms )
	{
		for ( int k = 0; k < LIGHTS_PER_ROOM; ++k )
		{
			int best = -1;
			Placed bp = { -1, -1 };
			for ( int y = std::max(1, ro.z0); y < std::min(H - 1, ro.z1); ++y )
			{
				for ( int x = std::max(1, ro.x0); x < std::min(W - 1, ro.x1); ++x )
				{
					if ( isWall(x, y) ) { continue; }
					int dmin = 1 << 30;
					for ( auto& l : lightAt )
					{
						const int dx = l.x - x, dy = l.y - y;
						dmin = std::min(dmin, dx * dx + dy * dy);
					}
					if ( dmin > best ) { best = dmin; bp = { x, y }; }
				}
			}
			if ( best <= LIGHT_COVER * LIGHT_COVER || bp.x < 0 ) { break; }
			Entity* ls = newEntity(131, 1, map.entities, nullptr);
			ls->x = bp.x * 16;
			ls->y = bp.y * 16;
			ls->lightSourceAlwaysOn = 1;
			ls->lightSourceBrightness = 160;
			ls->lightSourceRadius = 6;
			ls->lightSourceFlicker = 0;
			ls->lightSourceRGB = 0xff | (0xc8 << 8) | (0x96 << 16); // warm white, R in the low byte
			lightAt.push_back(bp);
			++nLights;
		}
	}

	// dress fixtures with a Barony counterpart: table, bench (chair)
	for ( auto& d : f.dress )
	{
		if ( d.cls != "fixture" ) { continue; }
		int sprite = d.id == "table" ? 59 : d.id == "bench" ? 60 : 0;
		if ( !sprite ) { ++nUnmapped; continue; }
		if ( !isWalkTile(d.x, d.z) || used[(size_t)d.z * W + d.x] ) { continue; }
		at(sprite, d.x, d.z);
		++nFixtures;
	}

	// population at 1x: monsters, items, gold on free floor tiles outside the entrance room
	const apx::RoomRow* entranceRoom = roomOf(entranceNode);
	std::vector<Placed> freeTiles;
	for ( int y = 1; y < H - 1; ++y )
	{
		for ( int x = 1; x < W - 1; ++x )
		{
			if ( isWalkTile(x, y) && !used[(size_t)y * W + x] && !inRoom(entranceRoom, x, y)
				&& !(lavatiles && lavatiles[tileAt(x, y, 0)]) && !(swimmingtiles && swimmingtiles[tileAt(x, y, 0)]) )
			{
				freeTiles.push_back({ x, y });
			}
		}
	}
	// Densities calibrated to vanilla floors 1-5 (D46 sheet): tiles per monster / item / gold pile.
	const int pct = std::max(0, apxgen.populationPct);
	const int wantMonsters = (int)((long long)freeTiles.size() * pct / (100LL * 95));
	const int wantItems = (int)((long long)freeTiles.size() * pct / (100LL * 70));
	const int wantGold = (int)((long long)freeTiles.size() * pct / (100LL * 120));
	auto take = [&]() -> Placed {
		while ( !freeTiles.empty() )
		{
			size_t i = (size_t)rng.next(freeTiles.size());
			Placed p = freeTiles[i];
			freeTiles[i] = freeTiles.back();
			freeTiles.pop_back();
			if ( !used[(size_t)p.y * W + p.x] ) { return p; }
		}
		return { -1, -1 };
	};
	// D72 population (DECISIONS-2, spec 04 section 2.1; values ASSUMED until Apocky tunes them):
	// the band's monster types are ranked by base level into thirds (fodder, regular, elite);
	// SPAWNED packs sit in dens (the stronghold, rooms dressed as dens, then spread out) and take at
	// most D72_SPAWN_PCT of the floor's creatures; a pack with an elite has at most 4 members and 1-2
	// elites; a large pack (5+) is at least 75% fodder with no elite; everything else is EMERGENT:
	// groups of 1-3 in rooms. Floor-mode Barony has no patrol routes (those are site-only in apx-gen).
	// APXGEN_CONTROL=2 breaks the rules (packs of 2-8 of any tier, half the floor spawned): must fail.
	constexpr int D72_SPAWN_PCT = 20, D72_ELITE_PACK_MAX = 4, D72_ELITES_MAX = 2, D72_LARGE = 5, D72_EMERGENT_MAX = 3;
	const bool breakPacks = apxgen.control == 2;
	std::vector<int> tierTypes[3];
	{
		BaronyRNG saved = map_rng; // sample the band's curve without moving the game's map RNG
		std::map<int, int> freq;
		for ( int k = 0; k < 400; ++k ) { ++freq[monsterCurve(currentlevel)]; }
		map_rng.~BaronyRNG(); // BaronyRNG has no copy-assignment: rebuild it in place from the copy
		new (&map_rng) BaronyRNG(saved);
		std::vector<std::pair<std::pair<int, int>, int>> ranked; // ((level, -freq), type)
		for ( auto& fq : freq )
		{
			if ( fq.first <= NOTHING || fq.first >= NUMMONSTERS ) { continue; }
			// rank by the curve alone: rarer = stronger, ties by type id. (Stat base levels are only
			// filled on the host, so ranking by them gave the client another tier order and hash.)
			ranked.push_back({ { -fq.second, fq.first }, fq.first });
		}
		std::sort(ranked.begin(), ranked.end());
		const size_t n = ranked.size();
		for ( size_t k = 0; k < n; ++k )
		{
			const int tier = n < 2 ? 0 : (int)(k * 3 / n); // bottom third fodder, top third elite
			tierTypes[tier].push_back(ranked[k].second);
		}
		std::string s;
		for ( int t = 0; t < 3; ++t )
		{
			s += t ? " | " : "";
			for ( int ty : tierTypes[t] ) { s += std::string(monstertypename[ty]) + " "; }
		}
		printlog("apxgen: D72 tiers (fodder | regular | elite) level %d: %s\n", currentlevel, s.c_str());
	}
	static int typeSprite[NUMMONSTERS] = { 0 };
	if ( !typeSprite[RAT] )
	{
		for ( int sp = 0; sp < 512; ++sp )
		{
			const Monster mt = editorSpriteTypeToMonster(sp);
			if ( mt > NOTHING && mt < NUMMONSTERS && !typeSprite[mt] ) { typeSprite[mt] = sp; }
		}
	}
	apx::Rng pr(apx::hash_words({ (uint64_t)seed, 0xD72 }));
	auto typeFor = [&](int tier) {
		int t = std::min(2, tier);
		while ( t > 0 && tierTypes[t].empty() ) { --t; }
		while ( t < 2 && tierTypes[t].empty() ) { ++t; }
		const auto& v = tierTypes[t];
		return v.empty() ? -1 : v[(size_t)pr.next(v.size())];
	};
	auto drawTier = [&]() { int r = (int)pr.next(100); return r < 60 ? 0 : r < 92 ? 1 : 2; };
	struct D72Group { bool spawned; int room; std::vector<int> tiers; int placed = 0; };
	std::vector<D72Group> groups;
	const int total = wantMonsters;
	int budget = total * (breakPacks ? 50 : D72_SPAWN_PCT) / 100;
	// dens, apx-gen's rule: the stronghold, rooms dressed as dens, then the room farthest from them
	std::vector<int> dens;
	const size_t wantDens = 3 + f.rooms.size() / 12;
	{
		const int sh = f.mission.find(apx::Kind::Stronghold);
		if ( sh >= 0 ) { dens.push_back(sh); }
		for ( auto& d : f.dress )
		{
			if ( dens.size() < wantDens && d.content == "den" && d.room >= 0 && f.mission.nodes[d.room].kind == apx::Kind::Room
				&& std::find(dens.begin(), dens.end(), d.room) == dens.end() ) { dens.push_back(d.room); }
		}
		while ( dens.size() < wantDens )
		{
			int best = -1;
			long long bd = -1;
			for ( auto& ro : f.rooms )
			{
				if ( f.mission.nodes[ro.node].kind != apx::Kind::Room || std::find(dens.begin(), dens.end(), ro.node) != dens.end() ) { continue; }
				long long dmin = 1LL << 40;
				for ( int dn : dens )
				{
					const apx::RoomRow* dr = roomOf(dn);
					if ( !dr ) { continue; }
					const long long dx = (ro.x0 + ro.x1) - (dr->x0 + dr->x1), dy = (ro.z0 + ro.z1) - (dr->z0 + dr->z1);
					dmin = std::min(dmin, dx * dx + dy * dy);
				}
				if ( dmin > bd ) { bd = dmin; best = ro.node; }
			}
			if ( best < 0 ) { break; }
			dens.push_back(best);
		}
	}
	int spawnedCreatures = 0;
	for ( int den : dens )
	{
		if ( budget < 2 ) { break; }
		D72Group g{ true, den, {} };
		if ( breakPacks )
		{
			const int n = 2 + (int)pr.next(7);
			for ( int k = 0; k < n; ++k ) { g.tiers.push_back((int)pr.next(3)); }
		}
		else
		{
			const int kind = (int)pr.next(100);
			if ( kind < 15 ) // elite-led pack: 2-4, 1-2 elites, the rest fodder or regular
			{
				const int n = 2 + (int)pr.next(D72_ELITE_PACK_MAX - 1);
				const int e = 1 + (int)pr.next(std::min(D72_ELITES_MAX, n - 1));
				for ( int k = 0; k < e; ++k ) { g.tiers.push_back(2); }
				while ( (int)g.tiers.size() < n ) { g.tiers.push_back((int)pr.next(2)); }
			}
			else if ( kind < 50 ) // fodder swarm: 4-6, at most one regular
			{
				const int n = 4 + (int)pr.next(3);
				for ( int k = 0; k < n; ++k ) { g.tiers.push_back(0); }
				if ( pr.next(2) ) { g.tiers.back() = 1; }
			}
			else // small mixed pack: 2-3, fodder and regular
			{
				const int n = 2 + (int)pr.next(2);
				for ( int k = 0; k < n; ++k ) { g.tiers.push_back((int)pr.next(2)); }
			}
		}
		if ( (int)g.tiers.size() > budget ) { g.tiers.resize(budget); }
		budget -= (int)g.tiers.size();
		spawnedCreatures += (int)g.tiers.size();
		groups.push_back(std::move(g));
	}
	{
		int left = total - spawnedCreatures;
		std::vector<int> order;
		for ( auto& ro : f.rooms ) { if ( f.mission.nodes[ro.node].kind == apx::Kind::Room && ro.node != entranceNode ) { order.push_back(ro.node); } }
		for ( size_t k = order.size(); k > 1; --k ) { std::swap(order[k - 1], order[(size_t)pr.next(k)]); }
		for ( size_t k = 0; left > 0 && !order.empty(); ++k )
		{
			D72Group g{ false, order[k % order.size()], {} };
			const int n = std::min(left, 1 + (int)pr.next(D72_EMERGENT_MAX));
			int elites = 0;
			for ( int j = 0; j < n; ++j )
			{
				int t = drawTier();
				if ( t >= 2 && elites >= D72_ELITES_MAX && !breakPacks ) { t = 0; }
				elites += t >= 2;
				g.tiers.push_back(t);
			}
			left -= n;
			groups.push_back(std::move(g));
		}
	}
	// place each group around its room's centre, members on free tiles of that room
	int monsterNoType = 0;
	for ( auto& g : groups )
	{
		const apx::RoomRow* ro = roomOf(g.room);
		if ( !ro ) { continue; }
		auto spots = nearFree((ro->x0 + ro->x1) / 2, (ro->z0 + ro->z1) / 2, (int)g.tiers.size() * 3);
		size_t si = 0;
		for ( int t : g.tiers )
		{
			while ( si < spots.size() && (!inRoom(ro, spots[si].x, spots[si].y) || used[(size_t)spots[si].y * W + spots[si].x]) ) { ++si; }
			if ( si >= spots.size() ) { break; }
			const int ty = typeFor(t);
			const int sprite = ty >= 0 && typeSprite[ty] ? typeSprite[ty] : 10;
			monsterNoType += sprite == 10;
			Entity* m = at(sprite, spots[si].x, spots[si].y, map.creatures);
			m->skill[5] = nMonsters++;
			++g.placed;
			++si;
		}
	}
	// D72 gate on what was placed (the same predicates as apx-gen's site_critic)
	std::string d72why;
	int d72Placed = 0, d72Spawned = 0, d72MaxGroup = 0, d72EliteGroups = 0;
	for ( auto& g : groups )
	{
		const int size = g.placed;
		int elites = 0, fodder = 0;
		for ( int k = 0; k < size; ++k ) { elites += g.tiers[k] >= 2; fodder += g.tiers[k] == 0; }
		d72Placed += size;
		d72Spawned += g.spawned ? size : 0;
		d72MaxGroup = std::max(d72MaxGroup, size);
		d72EliteGroups += elites > 0;
		if ( d72why.empty() && elites > 0 && size > D72_ELITE_PACK_MAX ) { d72why = "elite pack of " + std::to_string(size); }
		if ( d72why.empty() && elites > D72_ELITES_MAX ) { d72why = std::to_string(elites) + " elites in one group"; }
		if ( d72why.empty() && size >= D72_LARGE && (elites > 0 || fodder * 4 < size * 3) ) { d72why = "large pack not mostly fodder " + std::to_string(fodder) + "/" + std::to_string(size); }
		if ( d72why.empty() && !g.spawned && size > D72_EMERGENT_MAX ) { d72why = "emergent group of " + std::to_string(size); }
	}
	if ( d72why.empty() && d72Placed && d72Spawned * 100 > d72Placed * D72_SPAWN_PCT ) { d72why = "spawned share " + std::to_string(d72Spawned) + "/" + std::to_string(d72Placed); }
	if ( d72why.empty() && d72Placed < total * 9 / 10 ) { d72why = "placed " + std::to_string(d72Placed) + " of " + std::to_string(total); }
	printlog("apxgen: D72 %s groups %d placed %d/%d spawned %d (%d%%) max-group %d elite-groups %d dens %d untyped %d control %d why %s\n",
		d72why.empty() ? "PASS" : "FAIL", (int)groups.size(), d72Placed, total, d72Spawned, d72Placed ? d72Spawned * 100 / d72Placed : 0,
		d72MaxGroup, d72EliteGroups, (int)dens.size(), monsterNoType, apxgen.control, d72why.empty() ? "-" : d72why.c_str());
	for ( int i = 0; i < wantItems; ++i )
	{
		Placed p = take(); if ( p.x < 0 ) { break; }
		Entity* it = at(8, p.x, p.y);
		setSpriteAttributes(it, nullptr, nullptr);
		++nItems;
	}
	for ( int i = 0; i < wantGold; ++i )
	{
		Placed p = take(); if ( p.x < 0 ) { break; }
		Entity* g = at(9, p.x, p.y);
		g->goldAmount = 0;
		++nGold;
	}
	auto t3 = clk::now();

	// 5. in-engine W2 on Barony's tiles + door entities: reach with locks closed, a key opens its lock
	int walkTotal = 0;
	for ( int y = 0; y < H; ++y ) { for ( int x = 0; x < W; ++x ) { walkTotal += isWalkTile(x, y); } }
	std::map<int, int> lockTile; // tile index -> lock id
	for ( auto& l : lockAt ) { lockTile[l.second.y * W + l.second.x] = l.first; }
	std::set<int> open;
	std::vector<char> reach;
	for ( ;; )
	{
		reach.assign((size_t)W * H, 0);
		std::deque<int> q;
		q.push_back(start.y * W + start.x);
		reach[q.front()] = 1;
		while ( !q.empty() )
		{
			int c = q.front(); q.pop_front();
			int cx = c % W, cy = c / W;
			static const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
			for ( int d = 0; d < 4; ++d )
			{
				int nx = cx + DX[d], ny = cy + DY[d], ni = ny * W + nx;
				if ( !isWalkTile(nx, ny) || reach[ni] ) { continue; }
				auto lk = lockTile.find(ni);
				if ( lk != lockTile.end() && !open.count(lk->second) ) { continue; }
				reach[ni] = 1;
				q.push_back(ni);
			}
		}
		bool opened = false;
		for ( auto& k : keyAt )
		{
			if ( reach[k.second.y * W + k.second.x] && !open.count(k.first) ) { open.insert(k.first); opened = true; }
		}
		if ( !opened ) { break; }
	}
	int reached = 0;
	for ( char c : reach ) { reached += c; }
	const bool w2Connected = reached == walkTotal;
	std::set<int> lockIds;
	for ( auto& l : lockAt ) { lockIds.insert(l.first); }
	const bool w2Locks = open.size() == lockIds.size();
	const bool w2Exit = ladder.x >= 0 && reach[ladder.y * W + ladder.x];
	// keys must not sit behind their own lock: with only that lock closed, its key is reachable
	int keysBehindOwnLock = 0;
	for ( auto& k : keyAt )
	{
		std::vector<char> rr((size_t)W * H, 0);
		std::deque<int> q;
		q.push_back(start.y * W + start.x);
		rr[q.front()] = 1;
		while ( !q.empty() )
		{
			int c = q.front(); q.pop_front();
			int cx = c % W, cy = c / W;
			static const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
			for ( int d = 0; d < 4; ++d )
			{
				int nx = cx + DX[d], ny = cy + DY[d], ni = ny * W + nx;
				if ( !isWalkTile(nx, ny) || rr[ni] ) { continue; }
				auto lk = lockTile.find(ni);
				if ( lk != lockTile.end() && lk->second == k.first ) { continue; }
				rr[ni] = 1;
				q.push_back(ni);
			}
		}
		if ( !rr[k.second.y * W + k.second.x] ) { ++keysBehindOwnLock; }
	}
	// every lock seals its room: with only that lock's doors closed, no tile of its box is reached
	int locksUnsealed = 0;
	for ( auto& ro : f.rooms )
	{
		if ( f.mission.nodes[ro.node].kind != apx::Kind::Lock ) { continue; }
		const int id = f.mission.nodes[ro.node].id;
		std::vector<char> rr((size_t)W * H, 0);
		std::deque<int> q;
		q.push_back(start.y * W + start.x);
		rr[q.front()] = 1;
		bool leak = false;
		while ( !q.empty() && !leak )
		{
			int c = q.front(); q.pop_front();
			int cx = c % W, cy = c / W;
			if ( inRoom(&ro, cx, cy) )
			{
				leak = true;
				std::string nb;
				for ( int yy = cy - 3; yy <= cy + 3; ++yy )
				{
					for ( int xx = cx - 3; xx <= cx + 3; ++xx )
					{
						const int ii = yy * W + xx;
						nb += (xx == cx && yy == cy) ? 'X' : (lockTile.count(ii) ? 'L' : (inRoom(&ro, xx, yy) ? (isWalkTile(xx, yy) ? 'r' : 'R') : (isWalkTile(xx, yy) ? '.' : '#')));
					}
					nb += '|';
				}
				printlog("apxgen: lock %d leak at %d,%d box [%d,%d)-(%d,%d): %s\n", id, cx, cy, ro.x0, ro.z0, ro.x1, ro.z1, nb.c_str());
				break;
			}
			static const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
			for ( int d = 0; d < 4; ++d )
			{
				int nx = cx + DX[d], ny = cy + DY[d], ni = ny * W + nx;
				if ( !isWalkTile(nx, ny) || rr[ni] ) { continue; }
				auto lk = lockTile.find(ni);
				if ( lk != lockTile.end() && lk->second == id ) { continue; }
				rr[ni] = 1;
				q.push_back(ni);
			}
		}
		if ( leak && !inRoom(&ro, start.x, start.y) ) { ++locksUnsealed; }
	}
	const bool w2 = w2Connected && w2Locks && w2Exit && keysBehindOwnLock == 0 && locksUnsealed == 0;

	// W5 hash: Barony tiles + every entity row in list order (sprite, tile x, tile y)
	uint64_t h = 1469598103934665603ULL;
	h = fnv(h, ((uint64_t)W << 32) | (uint64_t)H);
	for ( size_t i = 0; i < (size_t)W * H * MAPLAYERS; ++i ) { h = fnv(h, (uint32_t)map.tiles[i]); }
	int nEntities = 0;
	// entity rows beside the floor file (sprite, x, y in world units): the sheet draws them top-down
	FILE* rowsOut = fopen((lmp.substr(0, lmp.size() - 4) + "_entities.txt").c_str(), "w");
	for ( node_t* nd = map.entities->first; nd; nd = nd->next )
	{
		Entity* e = (Entity*)nd->element;
		if ( !e ) { continue; }
		if ( rowsOut ) { fprintf(rowsOut, "%d %d %d\n", (int)e->sprite, (int)e->x, (int)e->y); }
		h = fnv(h, ((uint64_t)(uint32_t)e->sprite << 32) | ((uint64_t)((int)e->x & 0xffff) << 16) | (uint64_t)((int)e->y & 0xffff));
		++nEntities;
	}
	if ( rowsOut ) { fclose(rowsOut); }

	const double msGen = std::chrono::duration<double, std::milli>(t1 - t0).count();
	const double msLoad = std::chrono::duration<double, std::milli>(t2 - t1).count();
	const double msEnt = std::chrono::duration<double, std::milli>(t3 - t2).count();
	printlog("apxgen: floor seed %u level %d set '%s' size %dx%d theme %s attempt %d rooms %d corridors %d tiles; textures floor %d wall %d ceiling %d water %d lava %d; water %d lava %d pit %d\n",
		seed, currentlevel, levelset, W, H, f.theme.c_str(), f.attempt, (int)f.rooms.size(), f.corridor_tiles,
		tx.floor, tx.wall, tx.ceiling, tx.water, tx.lava, nWater, nLava, nPit);
	printlog("apxgen: entities %d: starts %d doors %d locked %d lock-openings %d lock-unsealed %d narrowed %d wall-rejected %d torches %d lights %d chests %d keys %d boss %d merchant %d monsters %d items %d gold %d fixtures %d unmapped-dress %d\n",
		nEntities, nStarts, nDoors, nLocked, nLockOpenings, nLockUnsealed, nNarrowed, nWallRejected, nTorches, nLights, nChests, nKeys, nBoss, nMerchant, nMonsters, nItems, nGold, nFixtures, nUnmapped);
	printlog("apxgen: W2 %s connected %d (%d/%d walk tiles) locks %d (%d/%d opened) exit %d keys-behind-own-lock %d control %d locks-unsealed %d\n",
		w2 ? "PASS" : "FAIL", (int)w2Connected, reached, walkTotal, (int)w2Locks, (int)open.size(), (int)lockIds.size(), (int)w2Exit,
		keysBehindOwnLock, apxgen.control, locksUnsealed);
	printlog("apxgen: hash %016llx apx-floor %016llx apx-dress %016llx client %d ms gen %.1f load %.1f entities %.1f\n",
		(unsigned long long)h, (unsigned long long)f.hash, (unsigned long long)f.dress_hash, multiplayer == CLIENT ? 1 : 0,
		msGen, msLoad, msEnt);
	printlog("successfully generated a dungeon with %d rooms, %d monsters, %d gold, %d items, %d decorations.\n",
		(int)f.rooms.size(), nMonsters, nGold, nItems, nFixtures);
	return 0;
}

// ---------------------------------------------------------------- W1 instruments
#include "../player.hpp"
#include "../interface/consolecommand.hpp"
#include <algorithm>

namespace {
std::vector<float> g_frameMs;
bool g_frameRec = false;
std::chrono::steady_clock::time_point g_lastFrame;
long long g_netBytes[MAXPLAYERS] = { 0 };
long long g_netPackets[MAXPLAYERS] = { 0 };
std::chrono::steady_clock::time_point g_netSince = std::chrono::steady_clock::now();
}
int apxgenHallX = -1, apxgenHallY = -1, apxgenHallTiles = 0;

void apxgenFrameTick()
{
	auto now = std::chrono::steady_clock::now();
	if ( g_frameRec ) { g_frameMs.push_back(std::chrono::duration<float, std::milli>(now - g_lastFrame).count()); }
	g_lastFrame = now;
}

void apxgenNetSent(int hostnum, int bytes)
{
	if ( hostnum >= 0 && hostnum < MAXPLAYERS ) { g_netBytes[hostnum] += bytes; ++g_netPackets[hostnum]; }
}

static Uint32 g_savedFpsLimit = 0;
static ConsoleCommand ccmd_apxfps_begin("/apxfps_begin", "apx: start recording frame times (uncap: fps limit 300, vsync off)", [](int argc, const char** argv) {
	if ( argc > 1 && !strcmp(argv[1], "uncap") )
	{
		g_savedFpsLimit = fpsLimit;
		fpsLimit = 300;
		SDL_GL_SetSwapInterval(0);
		printlog("apxfps: uncapped (fps limit %u -> 300, swap interval 0)\n", g_savedFpsLimit);
	}
	g_frameMs.clear();
	g_frameRec = true;
	g_lastFrame = std::chrono::steady_clock::now();
	printlog("apxfps: begin\n");
});
static ConsoleCommand ccmd_apxfps_end("/apxfps_end", "apx: frame-time p50/p99 since /apxfps_begin", [](int, const char**) {
	g_frameRec = false;
	const bool uncapped = g_savedFpsLimit != 0;
	if ( uncapped )
	{
		fpsLimit = g_savedFpsLimit;
		g_savedFpsLimit = 0;
		SDL_GL_SetSwapInterval(verticalSync ? 1 : 0);
	}
	std::vector<float> v = g_frameMs;
	if ( v.size() < 2 ) { printlog("apxfps: too few frames (%d)\n", (int)v.size()); return; }
	v.erase(v.begin()); // the frame that straddled the command
	std::sort(v.begin(), v.end());
	auto q = [&](double p) { return v[std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5))]; };
	double sum = 0; for ( float x : v ) { sum += x; }
	printlog("apxfps: frames %d seconds %.2f frame-ms p50 %.2f p99 %.2f max %.2f fps-p50 %.1f fps-p99 %.1f map %ux%u uncapped %d\n",
		(int)v.size(), sum / 1000.0, q(0.5), q(0.99), v.back(), 1000.0 / q(0.5), 1000.0 / q(0.99), map.width, map.height, uncapped ? 1 : 0);
});
static ConsoleCommand ccmd_apxhall("/apxhall", "apx: teleport to the largest room", [](int, const char**) {
	if ( apxgenHallX < 0 || !players[clientnum] || !players[clientnum]->entity ) { printlog("apxhall: no hall\n"); return; }
	players[clientnum]->entity->x = apxgenHallX * 16 + 8;
	players[clientnum]->entity->y = apxgenHallY * 16 + 8;
	printlog("apxhall: at %d,%d (room %d floor tiles)\n", apxgenHallX, apxgenHallY, apxgenHallTiles);
});
static ConsoleCommand ccmd_apxnet("/apxnet", "apx: bytes/s sent per host since the last call", [](int, const char**) {
	auto now = std::chrono::steady_clock::now();
	double s = std::chrono::duration<double>(now - g_netSince).count();
	for ( int i = 0; i < MAXPLAYERS; ++i )
	{
		if ( g_netBytes[i] ) { printlog("apxnet: host %d bytes %lld packets %lld seconds %.2f bytes/s %.0f map %ux%u\n", i, g_netBytes[i], g_netPackets[i], s, g_netBytes[i] / s, map.width, map.height); }
		g_netBytes[i] = 0; g_netPackets[i] = 0;
	}
	printlog("apxnet: window %.2f s\n", s);
	g_netSince = now;
});
