// apx/move/move.hpp -- apx::move, our own player movement (spec 04b section 5, D93 "Rewrite movement too").
//
// Replaces Barony's per-frame player force/drag integration in actPlayer and its all-or-nothing clipMove for the player.
// Barony code that stays: the stat-based base speed (Player::PlayerMovement_t::getSpeedFactor: DEX, encumbrance, effects,
// legs/downed scale), the tile and body occupancy predicate (barony_clear: wall, floor, bodies, passable flags), the
// network packets and everything that reads my->x/y/vel_x/vel_y afterwards.
//
//   impulse(e, vx, vy, ticks)      combat / spells / traps push an entity: velocity (units per tick) falling linearly to 0 over
//                                   ticks; open-floor distance = |v| * (ticks + 1) / 2; state is Knocked while it lasts
//   speedScale(e, source, k)       a multiplier on the base speed per source (1.0 clears it)
//   state(player)                  walk, sprint, sneak, swim, levitate, crawl, knocked
//
// Per sim tick (50 Hz): step(player) integrates; collide(e, vx, vy) moves with slide and corner nudge; logTick(player) writes the
// JSONL row when APX_MOVE_LOG=<dir> is set (the gate's instrument). frame() applies impulses to creatures once per game tick.
// Table: data/apx/move/move.json (APX_MOVE_DATA=<dir> overrides). Knob: APX_MOVE_OLD=1 runs Barony's movement (the CONTROL).
// Counters and counters log: "[apx-move]" lines (D90): table load, sprint start/stop, impulse in/out, cost p50/p99.
#pragma once
#include "move_core.hpp"

class Entity;

namespace apx
{
namespace move
{
	using core::State;
	enum class Source : int { Gear = 0, Spell, Effect, Terrain, Combat, Script, COUNT };

	bool active();                                      // false when APX_MOVE_OLD=1
	void impulse(Entity* e, float vx, float vy, int ticks);
	void speedScale(Entity* e, Source s, float k);
	State state(int player);

	// engine hooks
	void step(int player);                              // once per sim tick, from actPlayer (local players)
	double collide(Entity* e, double vx, double vy);    // returns the distance moved; leaves hit.entity like clipMove
	void logTick(int player);                           // after collision, same tick; no-op unless APX_MOVE_LOG is set
	void frame();                                       // once per game tick: impulses on creatures, periodic cost summary
	bool lookActive();
	void lookX(float& rotX, float dx, float sens, bool smooth, bool confused, bool mobile, bool limit, float dt);
	void lookY(float& rotY, float dy, float sens, bool smooth, bool reverse, bool confused, bool mobile, float dt);
	const core::Table& table();
}
}
