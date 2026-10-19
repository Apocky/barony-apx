// apx/flow.hpp -- BF3-5 flow fields (barony-apx). Card: Apockalypsis handovers/tasks/BF3-tactics-card.md step 5.
//
// One field per chased target: Dijkstra from the target's tile over a walk grid marked with the engine's own
// passability rules (paths.cpp pathTileBlocked + pathEntityBlocksTile, the two halves of pathCheckObstacle),
// step costs as the engine's A* (straight 10, diagonal 14, a diagonal only past two open sides), a door tile +40.
// A field is rebuilt when its target changes tile (at most every 0.2 s) and at least every 0.5 s; the walk grid
// every 2 s. A monster in the PATH state more than 6 tiles from its target descends the field to within 6 tiles;
// nearer, for special goals, or when the descent is blocked, the engine's A* runs (flow_fallback_astar).
// Knob APX_FLOW (default on; 0 = the engine's A* for every path, still timed for the cost comparison).
#pragma once
#include <SDL.h>
#include <cstdint>
#include <vector>

class Entity;
struct list_t;

namespace apx
{
namespace flow
{
	extern bool enabled;                          // APX_FLOW
	void init();
	void onLevelChange();
	// the PATH-state path for a monster chasing `target`: a field descent when far, else the engine's A*
	// (generatePath with the engine's own arguments). Always timed (pathUs) for the BF3-5 cost gate.
	list_t* chasePath(Entity* my, Entity* target, int tx, int ty);

	struct Stats
	{
		long long grid_builds = 0, flow_builds = 0, flow_cells_updated = 0, flow_paths = 0, flow_fallback_astar = 0;
		long long astar_paths = 0, astar_null = 0;
		double flow_us_sum = 0, astar_us_sum = 0;
	};
	extern Stats ST;
	void resetStats();
	void buildUsPercentiles(double& p50, double& p99);    // field build time
	void pathUsMeans(double& flowMean, double& astarMean); // one chasePath call, by kind

	// self-test (BF3-5a): field steps and cost against the engine's A* between two tiles
	struct Compare { bool fieldOk = false, astarOk = false; int fieldSteps = 0, astarSteps = 0, fieldCost = 0, astarCost = 0; bool door = false; double buildUs = 0; };
	Compare compare(Entity* probe, int sx, int sy, int gx, int gy);
}
}
