// apx/flow.cpp -- BF3-5 flow fields. Design and knob: flow.hpp.
#include "flow.hpp"
#include "tactics_int.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <queue>
#include <unordered_map>

namespace apx
{
namespace flow
{
	bool enabled = true;
	Stats ST;
	static const uint16_t INF = 0xFFFF;
	static const int STRAIGHT = 10, DIAG = 14, DOOR = 40; // the engine's A* step costs; a door tile +4 tiles
	static const double NEAR_TILES = 6.0;                  // nearer than this the engine's A* finishes the path
	static const int MAX_STEPS = 1024;

	struct Grid { int W = 0, H = 0; std::vector<uint8_t> walk, door; Uint32 built = 0; bool valid = false; };
	static Grid G;
	struct Field { int gx = -1, gy = -1; Uint32 built = 0; bool valid = false; std::vector<uint16_t> cost; };
	static std::unordered_map<Uint32, Field> F;
	static std::vector<float> gBuildUs;

	void init()
	{
		const char* e = getenv("APX_FLOW");
		enabled = !(e && *e && !strcmp(e, "0"));
	}

	void onLevelChange() { G = Grid(); F.clear(); }

	void resetStats() { ST = Stats(); gBuildUs.clear(); }

	void buildUsPercentiles(double& p50, double& p99)
	{
		p50 = p99 = 0;
		if ( gBuildUs.empty() ) { return; }
		std::vector<float> v = gBuildUs;
		std::sort(v.begin(), v.end());
		p50 = v[(size_t)(0.5 * (v.size() - 1) + 0.5)];
		p99 = v[(size_t)(0.99 * (v.size() - 1) + 0.5)];
	}

	void pathUsMeans(double& flowMean, double& astarMean)
	{
		flowMean = ST.flow_paths ? ST.flow_us_sum / ST.flow_paths : 0;
		astarMean = ST.astar_paths ? ST.astar_us_sum / ST.astar_paths : 0;
	}

	// the walk grid: the map-tile rule for every tile, then each entity's rule once (pathCheckObstacle's halves)
	static void buildGrid()
	{
		G.W = (int)map.width; G.H = (int)map.height;
		G.walk.assign((size_t)G.W * G.H, 0);
		G.door.assign((size_t)G.W * G.H, 0);
		for ( int y = 0; y < G.H; ++y )
		{
			for ( int x = 0; x < G.W; ++x ) { G.walk[x + (size_t)y * G.W] = pathTileBlocked(x, y) ? 0 : 1; }
		}
		for ( node_t* nd = map.entities ? map.entities->first : nullptr; nd; nd = nd->next )
		{
			Entity* e = (Entity*)nd->element;
			if ( !e ) { continue; }
			int u = (int)floor(e->x / 16), v = (int)floor(e->y / 16);
			if ( u < 0 || v < 0 || u >= G.W || v >= G.H ) { continue; }
			if ( pathEntityBlocksTile(e, u, v) ) { G.walk[u + (size_t)v * G.W] = 0; }
			if ( e->behavior == &actDoor ) { G.door[u + (size_t)v * G.W] = 1; }
		}
		G.built = ticks;
		G.valid = true;
		++ST.grid_builds;
	}

	static void ensureGrid()
	{
		if ( !G.valid || G.W != (int)map.width || G.H != (int)map.height || ticks - G.built >= (Uint32)(2 * TICKS_PER_SECOND) ) { buildGrid(); }
	}

	static inline bool stepOk(int x, int y, int dx, int dy)
	{
		int nx = x + dx, ny = y + dy;
		if ( nx < 0 || ny < 0 || nx >= G.W || ny >= G.H || !G.walk[nx + (size_t)ny * G.W] ) { return false; }
		if ( dx && dy && (!G.walk[nx + (size_t)y * G.W] || !G.walk[x + (size_t)ny * G.W]) ) { return false; } // the engine's diagonal rule
		return true;
	}

	// Dijkstra from the goal outward; cost[i] = cost of the cheapest walk from tile i to the goal
	static void buildField(Field& f, int gx, int gy)
	{
		tactics::Timer tm;
		f.cost.assign((size_t)G.W * G.H, INF);
		f.gx = gx; f.gy = gy;
		typedef std::pair<uint32_t, int> QE;
		std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
		if ( gx >= 0 && gy >= 0 && gx < G.W && gy < G.H )
		{
			f.cost[gx + (size_t)gy * G.W] = 0;
			pq.push({ 0u, gx + gy * G.W });
		}
		long long settled = 0;
		while ( !pq.empty() )
		{
			QE top = pq.top(); pq.pop();
			int i = top.second;
			if ( top.first != f.cost[i] ) { continue; }
			++settled;
			int x = i % G.W, y = i / G.W;
			uint32_t enter = top.first + (G.door[i] ? DOOR : 0); // a forward step INTO tile i pays its door cost
			for ( int dy = -1; dy <= 1; ++dy )
			{
				for ( int dx = -1; dx <= 1; ++dx )
				{
					if ( (!dx && !dy) || !stepOk(x, y, dx, dy) ) { continue; }
					int ni = (x + dx) + (y + dy) * G.W;
					uint32_t nc = enter + ((dx && dy) ? DIAG : STRAIGHT);
					if ( nc < f.cost[ni] && nc < INF ) { f.cost[ni] = (uint16_t)nc; pq.push({ nc, ni }); }
				}
			}
		}
		f.built = ticks;
		f.valid = true;
		++ST.flow_builds;
		ST.flow_cells_updated += settled;
		if ( gBuildUs.size() < 200000 ) { gBuildUs.push_back((float)tm.us()); }
	}

	// steepest descent; stops `nearTiles` from the goal (0 = at the goal). nullptr if the start cannot reach the goal
	static list_t* descend(Field& f, int sx, int sy, double nearTiles, int* stepsOut, int* costOut, bool* doorOut)
	{
		int cur = sx + sy * G.W, gi = f.gx + f.gy * G.W;
		if ( sx < 0 || sy < 0 || sx >= G.W || sy >= G.H || f.cost[cur] == INF ) { return nullptr; }
		list_t* path = (list_t*)malloc(sizeof(list_t));
		path->first = nullptr; path->last = nullptr;
		int steps = 0, cost = 0;
		bool door = false;
		while ( steps < MAX_STEPS && cur != gi )
		{
			int x = cur % G.W, y = cur / G.W;
			if ( nearTiles > 0 && hypot(x - f.gx, y - f.gy) <= nearTiles ) { break; }
			int best = -1;
			uint32_t bc = f.cost[cur];
			for ( int dy = -1; dy <= 1; ++dy )
			{
				for ( int dx = -1; dx <= 1; ++dx )
				{
					if ( (!dx && !dy) || !stepOk(x, y, dx, dy) ) { continue; }
					int ni = (x + dx) + (y + dy) * G.W;
					if ( f.cost[ni] < bc ) { bc = f.cost[ni]; best = ni; }
				}
			}
			if ( best < 0 ) { list_FreeAll(path); free(path); return nullptr; } // no way down (the grid changed)
			int bx = best % G.W, by = best / G.W;
			cost += ((bx != x && by != y) ? DIAG : STRAIGHT) + (G.door[best] ? DOOR : 0);
			door = door || G.door[best];
			cur = best;
			++steps;
			pathnode_t* pn = (pathnode_t*)malloc(sizeof(pathnode_t));
			*pn = pathnode_t{ bx, by, 0, 0, -1, -1 };
			node_t* node = list_AddNodeLast(path);
			node->element = pn;
			node->size = sizeof(pathnode_t);
			node->deconstructor = &defaultDeconstructor;
		}
		if ( stepsOut ) { *stepsOut = steps; }
		if ( costOut ) { *costOut = cost; }
		if ( doorOut ) { *doorOut = door; }
		if ( !path->first ) { free(path); return nullptr; }
		return path;
	}

	list_t* chasePath(Entity* my, Entity* target, int tx, int ty)
	{
		tactics::Timer tm;
		int sx = (int)floor(my->x / 16), sy = (int)floor(my->y / 16);
		list_t* path = nullptr;
		if ( enabled && tactics::K.tactics && multiplayer != CLIENT && target
			&& (target->behavior == &actPlayer || target->behavior == &actMonster) && hypot(sx - tx, sy - ty) > NEAR_TILES )
		{
			Stat* st = my->getStats();
			if ( st && tactics::eligible(my, *st) )
			{
				ensureGrid();
				Field& f = F[target->getUID()];
				bool moved = f.gx != tx || f.gy != ty;
				if ( !f.valid || (moved && ticks - f.built >= 10) || ticks - f.built >= 25 ) { buildField(f, tx, ty); }
				path = descend(f, sx, sy, NEAR_TILES, nullptr, nullptr, nullptr);
				if ( path )
				{
					++ST.flow_paths;
					double us = tm.us();
					ST.flow_us_sum += us;
					tactics::addPathUs(us);
					return path;
				}
				++ST.flow_fallback_astar;
			}
		}
		path = generatePath(sx, sy, tx, ty, my, target, GeneratePathTypes::GENERATE_PATH_TO_HUNT_MONSTER_TARGET); // the engine's own call
		++ST.astar_paths;
		if ( !path ) { ++ST.astar_null; }
		double us = tm.us();
		ST.astar_us_sum += us;
		tactics::addPathUs(us);
		return path;
	}

	static void astarCost(list_t* p, int sx, int sy, int& steps, int& cost)
	{
		steps = 0; cost = 0;
		int px = sx, py = sy;
		for ( node_t* n = p ? p->first : nullptr; n; n = n->next )
		{
			pathnode_t* pn = (pathnode_t*)n->element;
			if ( !pn ) { continue; }
			cost += ((pn->x != px && pn->y != py) ? DIAG : STRAIGHT) + ((pn->x >= 0 && pn->y >= 0 && pn->x < G.W && pn->y < G.H && G.door[pn->x + (size_t)pn->y * G.W]) ? DOOR : 0);
			px = pn->x; py = pn->y;
			++steps;
		}
	}

	Compare compare(Entity* probe, int sx, int sy, int gx, int gy)
	{
		Compare c;
		ensureGrid();
		Field f;
		buildField(f, gx, gy);
		c.buildUs = gBuildUs.empty() ? 0 : gBuildUs.back();
		list_t* fp = descend(f, sx, sy, 0, &c.fieldSteps, &c.fieldCost, &c.door);
		c.fieldOk = fp != nullptr;
		if ( fp ) { list_FreeAll(fp); free(fp); }
		// the reference: the engine's A* with its long search budget (GENERATE_PATH_ACHIEVEMENT: 1600 expansions)
		list_t* ap = generatePath(sx, sy, gx, gy, probe, nullptr, GeneratePathTypes::GENERATE_PATH_ACHIEVEMENT);
		c.astarOk = ap != nullptr;
		astarCost(ap, sx, sy, c.astarSteps, c.astarCost);
		if ( ap ) { list_FreeAll(ap); free(ap); }
		return c;
	}
}
}
