// apx/tactics_int.hpp -- BF3 internals shared by tactics.cpp, tactics_pack.cpp and bf3trial.cpp.
#pragma once
#include "tactics.hpp"
#include "../main.hpp"
#include "../game.hpp"
#include "../entity.hpp"
#include "../monster.hpp"
#include "../stat.hpp"
#include "../collision.hpp"
#include "../paths.hpp"
#include "../net.hpp"
#include "../player.hpp"
#include <chrono>
#include <unordered_map>
#include <vector>

namespace apx
{
namespace tactics
{
	// geometry (world units; a tile is 16). Engine: STRIKERANGE 24, TOUCHRANGE 32, HITRATE 45 ticks.
	static const double TILE = 16.0;
	static const double CHASE = 16.0;          // vanilla chaseRange: a melee creature stops when its trace distance <= 16
	static const double ENGAGE_R = 5 * 16.0;   // tactics manage movement within 5 tiles of the target
	// waiting ring radius: 2.75 tiles. The card says 3 to 5 tiles (ASSUMED); the engine's darkness rule
	// (actmonster.cpp ATTACK state: target beyond TOUCHRANGE and beyond its light, which is never below
	// TOUCHRANGE * 1.5 = 48) sends a creature to PATH, i.e. back into the crowd, so the ring sits inside 48.
	static const double RING_R = 44.0;
	static const double HOLD_EPS = 3.0;        // close enough to a goal point: stand
	static const double SWING_HALF = 2.4;      // swing room beyond a blocking body (0.15 tile, prototype LINE)
	static const int DECIDE = 10;              // decide every 10 ticks (0.2 s), staggered by uid
	static const int BLOCK_TICKS = 10;         // blocked by an ally longer than this: reposition
	static const int JITTER = 12;              // swing-timer start in -11..0 ticks
	static const int FEINT_PERIOD = 75, FEINT_TICKS = 12;
	static const double FEINT_HOP = 8.0;
	static const int MORALE_START = 70, MORALE_BREAK = 40, MORALE_RETURN = 60; // ASSUMED (no pack templates yet)
	static const int FLANK_PENALTY = 40;       // A* cost added per route-A tile (4 straight steps)
	static const int ZONE_PENALTY = 60;        // A* cost added per tile whose centre is inside a target's reach zone (keepOut)
	static const double HELP_R = 20 * 16.0;    // a call for help reaches the nearest idle friendly pack within 20 tiles
	// outside reach: a creature without a reach slot keeps at least this far from its target (trace reach 24 + body)
	inline double keepOut(double rTarget) { return rTarget + STRIKERANGE + 6; }

	enum RouteKind : uint8_t { ROUTE_NONE = 0, ROUTE_FLANK = 1, ROUTE_HOME = 2, ROUTE_GOAL = 3 };
	// how steer moved a creature on its last steered tick (diagnostics: the trial's dump and holder counters)
	enum SteerKind : uint8_t { STEER_NONE = 0, STEER_STRAIGHT = 1, STEER_ROUTE = 2, STEER_ARC = 3, STEER_OUT = 4, STEER_AT_GOAL = 5, STEER_SWING = 6, STEER_PACK = 7 };

	struct TacticsState
	{
		// card fields (BF3-1)
		Uint32 packId = 0; uint8_t role = ROLE_NONE; uint8_t stance = STANCE_NORMAL; int8_t morale = MORALE_START;
		Uint32 ringSlot = 0; Uint32 slotTarget = 0; Uint32 decideTick = 0;
		bool seen = false;
		Entity* self = nullptr;          // a stale entry (freed entity, reused uid) is reset on sight
		// reach slots and ring
		int reachIdx = -1, ringIdx = -1;
		Uint32 blockedSince = 0; bool wantReposition = false;
		// movement bookkeeping
		Uint32 steeredTick = 0; bool faceTarget = false; bool faceMove = false; Uint32 stillSince = 0; bool stuckCounted = false;
		Uint32 nextFeint = 0, feintUntil = 0;
		// packs, flank, morale
		double homeX = 0, homeY = 0;
		uint8_t flank = 0;               // 0 never, 1 active, 2 arrived, 3 failed
		Uint32 flankTarget = 0, flankStart = 0, progressTick = 0;
		double progressX = 0, progressY = 0;
		uint8_t routeKind = ROUTE_NONE; std::vector<int> route; size_t routeAt = 0;
		bool routeOk = false; int routeGoal = -1; Uint32 routeBuilt = 0;   // ROUTE_GOAL: a path was found, its goal tile, when planned
		int avoidTile = -1; Uint32 avoidUntil = 0;                         // a locked door it bumped: kept out of its routes for 5 s
		uint8_t lastSteer = STEER_NONE; double goalX = 0, goalY = 0;       // diagnostics
		bool lowHp = false, burning = false, outnumbered = false, surrendered = false;
		Uint32 retreatStart = 0, lastRecover = 0, lastHomePath = 0;
		Uint32 helpTarget = 0, helpSince = 0; double helpX = 0, helpY = 0;   // answering a call for help
		double lastThreatDist = 1e9;
	};
	extern std::unordered_map<Uint32, TacticsState> S;   // by creature uid

	struct TargetSlots
	{
		Uint32 assignTick = 0; bool assigned = false;
		double R = 0, ringR = 0, base = 0;
		int n = 0, k = 0, validN = 0, ringValidN = 0;
		std::vector<char> valid, ringValid;
	};
	extern std::unordered_map<Uint32, TargetSlots> T;    // by target uid

	struct Timer
	{
		std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
		double us() const { return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count(); }
	};
	struct HitSave { hit_t h; HitSave() : h(hit) {} ~HitSave() { hit = h; } };

	TacticsState* find(Entity* e);
	double bodyR(Entity* e);
	bool eligible(Entity* my, Stat& st);
	bool walkable(int tx, int ty);
	bool wallClear(Entity* from, double x0, double y0, double x1, double y1);
	bool corridorBlocked(Entity* my, Entity* target);
	bool moveTo(Entity* my, TacticsState& s, double gx, double gy, double speed, double& dist2);
	void stand(Entity* my, Entity* target, double speed);
	void routeAround(Entity* target, Entity* my, double& gx, double& gy, bool ownSlot); // radial step out; the arc when no path
	bool needsRoute(Entity* target, Entity* my, double gx, double gy, bool ownSlot);  // the straight way crosses a wall or the zone
	bool goalFor(Entity* my, TacticsState& s, Entity* target, double& gx, double& gy, bool& ownSlot); // reach slot, ring slot or queue
	bool planGoalRoute(Entity* my, TacticsState& s, Entity* target, double gx, double gy);     // engine A* to the goal, zone penalised
	bool followRoute(Entity* my, TacticsState& s, double speed, double& dist2, double fx, double fy, Entity* keepOutOf);
	void addDecideUs(double us);
	void addMoveUs(double us);
	double angDiff(double a, double b);

	// tactics_pack.cpp
	void packDecide(Entity* my, Stat& st, TacticsState& s, Entity* target);
	bool packSteer(Entity* my, Stat& st, TacticsState& s, Entity* target, double d, double speed, double& dist2);
	void moraleEvent(Entity* e, TacticsState& s, int delta);
	void helpTick(Entity* my, TacticsState& s);
	void packsReset();
	Entity* packTarget(Entity* my, TacticsState& s); // the most common live target among packmates within 10 tiles
}

namespace bf3
{
	void onCreatureHP(Entity* e, int oldHP, int newHP);
	void onSwingThin(Entity* my, Entity* target);
	void onSwingWide(Entity* my, Entity* target);
}
}
