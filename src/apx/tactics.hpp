// apx/tactics.hpp -- BF3: the tactics layer over Barony's monster state machine (barony-apx).
// Spec: Apockalypsis specs/barony-fork/03 sections 4-5 and 11; card handovers/tasks/BF3-tactics-card.md (D62).
//
// D62 (Apocky): no attack cap. Every creature that can reach attacks; the limits are physical:
//   REACH      reach slots by body size: floor(2*PI*R / (2*r_attacker + elbow)), R = r_target + r_attacker +
//              reach. A creature with no free slot is not forbidden to attack; it waits on the ring outside
//              reach. The engine's own range re-check (lineTrace, STRIKERANGE) stays authoritative.
//   INTERLEAVE wind-up/recovery rhythm: at the swing-timer reset the timer starts at -(hash % 12) ticks,
//              so a crowd does not swing in lockstep. No cooldown is added on top of HITRATE.
//   ALLYAVOID  no swing through an ally: a body within its radius + 2.4 units of the swing line skips the
//              swing (the engine already skips it for a thin ray); blocked > 10 ticks -> reposition to a
//              free slot, else wait for a gap.
//   RING       waiting creatures hold a ring 3 tiles out, front slots first, facing the target; feint hop.
//   PACKS      packs and roles (leader, flanker); FLANK = route B with route A's tiles penalised in A*.
//   MORALE     leader dies -40, ally dies in sight -8, HP under 30 percent -15, burning -10, outnumbering
//              2:1 +5; below break the engine's own retreat (shouldRetreat) walks it away; regroup at home.
//   FLOW       flow fields (BF3-5): NOT BUILT in this slice; the knob is reported as 0.
//
// Knobs (env, read once at start; default ON, "0" = off): APX_TACTICS (master), APX_REACH, APX_INTERLEAVE,
// APX_ALLYAVOID, APX_RING, APX_PACKS, APX_MORALE. With a knob off its hook returns before touching game
// state, so the decision path is vanilla. Authority: the server decides (multiplayer != CLIENT).
#pragma once
#include <SDL.h>
#include <cstdint>
#include <string>

class Entity;
class Stat;
union SDL_Event;

namespace apx
{
namespace tactics
{
	struct Knobs
	{
		bool tactics = true, reach = true, interleave = true, allyavoid = true, ring = true, packs = true, morale = true;
	};
	extern Knobs K;           // live knob set (the BF3 trial switches it between arms)
	extern bool active;       // K.tactics && !client: the master guard every hook tests first
	std::string knobString(); // "tactics=1,reach=1,..." for telemetry rows

	void init();                                  // read env knobs (after argv parsing)
	void refresh();                               // recompute `active` after a knob change
	void onLevelChange();                         // clears all per-creature state

	// hooks (each called from ONE guarded line in the engine)
	void tick(Entity* my);                                        // top of actMonster's server part
	bool keepEngaged(Entity* my, Entity* target, double& dist);   // ATTACK state: line blocked by an ally only
	bool steer(Entity* my, Stat& st, Entity* target, double dist, double weightratio, double& dist2); // ATTACK movement
	void face(Entity* my, double& dir);                           // ATTACK rotation: face the target while stepping
	Sint32 swingReset(Entity* my, Entity* target, double dist);   // value for monsterHitTime at the swing reset
	void onSwingTrace(Entity* my, Entity* target, Entity* hitEntity); // after the engine's thin re-trace (counts ally blocks)
	bool swingLineBlocked(Entity* my, Entity* target);           // ally body in the swing line (skip the swing)
	bool moraleBroken(Entity* my);                                // OR-ed into Entity::shouldRetreat
	void onHPChange(Entity* e, int oldHP, int newHP);             // end of Entity::modHP
	int pathPenalty(int x, int y);                                // extra A* cost for a tile (route A of a flank)

	// pack assignment (the BF3 trial and the level-wide pass use it)
	void assignPack(Entity* e, Uint32 packId, uint8_t role, double homeX, double homeY);

	enum Role : uint8_t { ROLE_NONE = 0, ROLE_LEADER = 1, ROLE_BRUTE = 2, ROLE_FLANKER = 3 };
	enum Stance : uint8_t { STANCE_NORMAL = 0, STANCE_RETREAT = 1, STANCE_REGROUP = 2 };

	// telemetry: counters since the last reset (all monotonically increasing within a window)
	struct Counters
	{
		long long tactics_calls = 0, decides = 0;
		long long reach_assign = 0, reach_slots_sum = 0, reach_slots_n = 0, reach_overflow_creatures = 0;
		long long ally_blocked = 0, ally_blocked_wide = 0, ally_blocked_thin = 0, reposition = 0, gap_wait = 0;
		long long keep_engaged = 0, promoted = 0, steer_moves = 0, steer_holds = 0, target_dropped_by_tactics = 0;
		long long ring_holds = 0, ring_assign = 0, ring_slot_fail = 0, feints = 0, stuck = 0, stuck_episodes = 0, steer_ticks = 0;
		long long flank_started = 0, flank_arrived = 0, flank_failed = 0;
		long long retreats = 0, regroups = 0, morale_breaks = 0, morale_events = 0, surrenders_offered = 0, home_moved = 0;
		long long swing_resets = 0, jitter_sum = 0;
		long long packs_formed = 0;
	};
	extern Counters C;
	void resetCounters();
	// timing (microseconds): decide = one tick() call that decided; move = one steer() call
	void timingPercentiles(double& decideP50, double& decideP99, double& moveP50, double& moveP99, double& tickP99);
	void timingReset();
	void endOfGameTick();      // closes the per-game-tick sum used for tick_us_p99

	// read-only views for the trial sampler
	int reachSlotsFor(Entity* target, Entity* attackerType); // valid reach slots round target for this body size
	int holdsReachSlot(Entity* e);   // slot index or -1
	int holdsRingSlot(Entity* e);    // ring index or -1
	bool isFlanking(Entity* e);
	int stanceOf(Entity* e);
	int moraleOf(Entity* e);
	Uint32 packOf(Entity* e);
}

namespace bf3
{
	// the BF3 trial: -apx-bf3trial (see bf3trial.cpp). Rejects human input while it runs.
	extern bool on;
	void init(int argc, char** argv);
	bool swallowInput(const SDL_Event& event);   // top of the SDL poll loop: true = drop the event
	bool absorbPlayerDamage(Entity* player, int amount); // top of Entity::modHP for players: true = count and negate
	void onGameTick();                             // after ++ticks
	void onSwingAttempt(Entity* my, Entity* target, double dist); // swing timer reset (both arms)
	extern int exitCode;
}
}
