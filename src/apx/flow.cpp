// apx/flow.cpp -- BF3-5 flow fields. Design and knob: flow.hpp.
#include "flow.hpp"
#include "tactics_int.hpp"
#include "../magic/magic.hpp"
#include "../mod_tools.hpp"
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

	// base: the level's grounded path map (pathMapGrounded, the A*'s starting copy); walk: base with the tile of every
	// entity the A* does not skip blocked; door: an actDoor stands there
	struct Grid { int W = 0, H = 0; std::vector<uint8_t> base, walk, door; Uint32 built = 0; bool valid = false; };
	static Grid G;
	struct Field { int gx = -1, gy = -1; Uint32 built = 0; bool valid = false, goalOpen = true; std::vector<uint16_t> cost; };
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

	// The engine's A* at run time (paths.cpp generatePath, a monster walking): it starts from pathMapGrounded and
	// blocks the tile of every entity except passable ones, doors, door frames, unlocked iron doors, magic missiles,
	// monsters it is not an enemy of (all monsters here: one field serves a whole pack; an inert mimic still blocks),
	// colliders NPCs may path through (/pathing_collider_npc, default on), and its own target (the field's goal).
	// The load-time rule (pathEntityBlocksTile) differs: it blocks unlocked iron doors and passable shrines, and not
	// furniture or chests; using it gave field-only and A*-only tiles (s2-exclusive: astar_only 1).
	static bool astarSkips(Entity* e)
	{
		if ( e->flags[PASSABLE] ) { return true; } // (the spear-trap exception is for humans and followers only)
		if ( e->behavior == &actDoorFrame || e->behavior == &actDoor || (e->behavior == &actIronDoor && e->doorLocked == 0)
			|| e->behavior == &actMagicMissile ) { return true; }
		if ( e->behavior == &actMonster && !e->isInertMimic() ) { return true; }
		if ( e->isDamageableCollider() && (e->colliderHasCollision & EditorEntityData_t::COLLIDER_COLLISION_FLAG_NPC) ) { return true; }
		return false;
	}

	static void buildGrid()
	{
		G.W = (int)map.width; G.H = (int)map.height;
		G.base.assign((size_t)G.W * G.H, 0);
		G.door.assign((size_t)G.W * G.H, 0);
		for ( int y = 0; y < G.H; ++y )
		{
			for ( int x = 0; x < G.W; ++x )
			{
				G.base[x + (size_t)y * G.W] = pathMapGrounded ? (pathMapGrounded[y + (size_t)x * G.H] != 0) : !pathTileBlocked(x, y);
			}
		}
		G.walk = G.base;
		for ( node_t* nd = map.entities ? map.entities->first : nullptr; nd; nd = nd->next )
		{
			Entity* e = (Entity*)nd->element;
			if ( !e ) { continue; }
			// the engine's own tile for an entity: x / 16 truncated, clamped to the map
			int u = std::min(std::max(0, (int)(e->x / 16)), G.W - 1), v = std::min(std::max(0, (int)(e->y / 16)), G.H - 1);
			if ( e->behavior == &actDoor ) { G.door[u + (size_t)v * G.W] = 1; }
			if ( !astarSkips(e) ) { G.walk[u + (size_t)v * G.W] = 0; }
		}
		G.built = ticks;
		G.valid = true;
		++ST.grid_builds;
	}

	static void ensureGrid()
	{
		if ( !G.valid || G.W != (int)map.width || G.H != (int)map.height || ticks - G.built >= (Uint32)(2 * TICKS_PER_SECOND) ) { buildGrid(); }
	}

	// a step from tile (x, y) into (x + dx, y + dy), as the A* allows it: the tile entered is open (or is the goal,
	// where the target stands: the A* skips its target), and a diagonal passes two open sides
	static inline bool stepInto(int x, int y, int dx, int dy, int goalIdx)
	{
		int nx = x + dx, ny = y + dy;
		if ( nx < 0 || ny < 0 || nx >= G.W || ny >= G.H ) { return false; }
		int ni = nx + ny * G.W;
		if ( !G.walk[ni] && ni != goalIdx ) { return false; }
		if ( dx && dy && (!G.walk[nx + (size_t)y * G.W] || !G.walk[x + (size_t)ny * G.W]) ) { return false; }
		return true;
	}

	// Dijkstra from the goal outward; cost[i] = cost of the cheapest walk from tile i to the goal. A tile the A* could
	// start from (on the path map) gets a cost even when an entity blocks it (the A* never checks its own start tile),
	// but no walk passes through a blocked tile. goalOpen: the target stands on the goal (chase fields); a bare goal
	// tile (the self-test) must itself be open, as for the A*.
	static void buildField(Field& f, int gx, int gy, bool goalOpen)
	{
		tactics::Timer tm;
		f.cost.assign((size_t)G.W * G.H, INF);
		f.gx = gx; f.gy = gy;
		f.goalOpen = goalOpen;
		typedef std::pair<uint32_t, int> QE;
		std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
		const int gi = gx + gy * G.W;
		if ( gx >= 0 && gy >= 0 && gx < G.W && gy < G.H && G.base[gi] && (goalOpen || G.walk[gi]) )
		{
			f.cost[gi] = 0;
			pq.push({ 0u, gi });
		}
		long long settled = 0;
		while ( !pq.empty() )
		{
			QE top = pq.top(); pq.pop();
			int i = top.second;
			if ( top.first != f.cost[i] ) { continue; }
			++settled;
			if ( i != gi && !G.walk[i] ) { continue; } // a walker may stand here, none walks through
			int x = i % G.W, y = i / G.W;
			uint32_t enter = top.first + (G.door[i] ? DOOR : 0); // a step INTO tile i pays its door cost
			for ( int dy = -1; dy <= 1; ++dy )
			{
				for ( int dx = -1; dx <= 1; ++dx )
				{
					if ( !dx && !dy ) { continue; }
					int nx = x + dx, ny = y + dy;
					if ( nx < 0 || ny < 0 || nx >= G.W || ny >= G.H ) { continue; }
					int ni = nx + ny * G.W;
					if ( !G.base[ni] ) { continue; } // the A* starts only on the path map
					if ( dx && dy && (!G.walk[nx + (size_t)y * G.W] || !G.walk[x + (size_t)ny * G.W]) ) { continue; }
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
	static list_t* descend(Field& f, int sx, int sy, double nearTiles, int* stepsOut, int* costOut, bool* doorOut, int* diagOut = nullptr)
	{
		int cur = sx + sy * G.W, gi = f.gx + f.gy * G.W;
		if ( sx < 0 || sy < 0 || sx >= G.W || sy >= G.H || f.cost[cur] == INF ) { return nullptr; }
		list_t* path = (list_t*)malloc(sizeof(list_t));
		path->first = nullptr; path->last = nullptr;
		int steps = 0, cost = 0, diag = 0;
		bool door = false;
		const int goalIdx = f.goalOpen ? gi : -1;
		while ( steps < MAX_STEPS && cur != gi )
		{
			int x = cur % G.W, y = cur / G.W;
			if ( nearTiles > 0 && hypot(x - f.gx, y - f.gy) <= nearTiles ) { break; }
			// steepest descent on the walk cost itself: the neighbour minimising step + door + its field cost (on a
			// cheapest walk that sum equals this tile's cost, so the path costs exactly cost[start]). The lowest field
			// value alone picked diagonals worth 14 over straights worth 10 (s2-exclusive: field dearer than A* 23x)
			int best = -1;
			uint32_t bc = 0xFFFFFFFFu;
			for ( int dy = -1; dy <= 1; ++dy )
			{
				for ( int dx = -1; dx <= 1; ++dx )
				{
					if ( (!dx && !dy) || !stepInto(x, y, dx, dy, goalIdx) ) { continue; }
					int ni = (x + dx) + (y + dy) * G.W;
					if ( f.cost[ni] >= f.cost[cur] ) { continue; } // strictly downhill: no loops
					uint32_t c = (uint32_t)f.cost[ni] + ((dx && dy) ? DIAG : STRAIGHT) + (G.door[ni] ? DOOR : 0);
					if ( c < bc ) { bc = c; best = ni; }
				}
			}
			if ( best < 0 ) { list_FreeAll(path); free(path); return nullptr; } // no way down (the grid changed)
			int bx = best % G.W, by = best / G.W;
			cost += ((bx != x && by != y) ? DIAG : STRAIGHT) + (G.door[best] ? DOOR : 0);
			diag += (bx != x && by != y) ? 1 : 0;
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
		if ( diagOut ) { *diagOut = diag; }
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
				if ( !f.valid || (moved && ticks - f.built >= 10) || ticks - f.built >= 25 ) { buildField(f, tx, ty, true); }
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

	static void astarCost(list_t* p, int sx, int sy, int& steps, int& cost, int& diag, int& diagNeg, int& cornerCut)
	{
		steps = 0; cost = 0; diag = 0; diagNeg = 0; cornerCut = 0;
		int px = sx, py = sy;
		for ( node_t* n = p ? p->first : nullptr; n; n = n->next )
		{
			pathnode_t* pn = (pathnode_t*)n->element;
			if ( !pn ) { continue; }
			bool dg = pn->x != px && pn->y != py;
			cost += (dg ? DIAG : STRAIGHT) + ((pn->x >= 0 && pn->y >= 0 && pn->x < G.W && pn->y < G.H && G.door[pn->x + (size_t)pn->y * G.W]) ? DOOR : 0);
			if ( dg )
			{
				++diag;
				if ( pn->x < px ) { ++diagNeg; }
				// a diagonal the field does not allow: past a blocked side (the A*'s second side test reads tile
				// (dx, newy), paths.cpp generatePath, not (pathnode.x, newy))
				if ( px >= 0 && pn->y >= 0 && px < G.W && pn->y < G.H && !G.walk[px + (size_t)pn->y * G.W] ) { ++cornerCut; }
			}
			px = pn->x; py = pn->y;
			++steps;
		}
	}

	Compare compare(Entity* probe, int sx, int sy, int gx, int gy)
	{
		Compare c;
		ensureGrid();
		Field f;
		buildField(f, gx, gy, false); // a bare goal tile: the A* gets no target here either
		c.buildUs = gBuildUs.empty() ? 0 : gBuildUs.back();
		list_t* fp = descend(f, sx, sy, 0, &c.fieldSteps, &c.fieldCost, &c.door, &c.fieldDiag);
		c.fieldOk = fp != nullptr;
		if ( fp ) { list_FreeAll(fp); free(fp); }
		// the reference: the engine's A* with its long search budget (GENERATE_PATH_ACHIEVEMENT: 1600 expansions)
		list_t* ap = generatePath(sx, sy, gx, gy, probe, nullptr, GeneratePathTypes::GENERATE_PATH_ACHIEVEMENT);
		c.astarOk = ap != nullptr;
		astarCost(ap, sx, sy, c.astarSteps, c.astarCost, c.astarDiag, c.astarDiagNeg, c.astarCornerCut);
		if ( ap ) { list_FreeAll(ap); free(ap); }
		// the control: upstream's diagonal side test (APX_ASTAR_DIAG off for this one call)
		bool saved = tactics::astarDiagOn;
		tactics::astarDiagOn = false;
		list_t* up = generatePath(sx, sy, gx, gy, probe, nullptr, GeneratePathTypes::GENERATE_PATH_ACHIEVEMENT);
		tactics::astarDiagOn = saved;
		c.upOk = up != nullptr;
		int upNeg = 0, upCut = 0;
		astarCost(up, sx, sy, c.upSteps, c.upCost, c.upDiag, upNeg, upCut);
		if ( up ) { list_FreeAll(up); free(up); }
		return c;
	}
}
}
