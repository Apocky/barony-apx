// tiers.hpp -- creature tiers at run time and the D74 damage rule at the hit sites (engine glue for
// src/apx/tier_rules.hpp, which holds the rules and is unit-tested offline).
//
// A creature's tier (spec 04 section 2): 1 a tag (the director or the BF3 trial assigns it), 2 the boss flag,
// 3 its type's band rank on this floor (spec 04 section 2.1, the generator's rule: apx-gen floors record the
// generator's own rank, other floors use the same rank from a fixed-seed sample of the floor's monster curve),
// 4 F4c tierFromStats by MAXHP for a type outside the band.
//
// D74: a hostile creature's hit on a player or a player's follower deals its tier's damage (melee, arrows,
// thrown weapons; spells and damage over time unchanged). Knob APX_TIER_DAMAGE=0 restores vanilla damage.
// Telemetry: counters per tier and path, a summary line per floor, "[apx-tierdmg]" per hit while logging is on
// (APX_TIER_DAMAGE_LOG=1 or the BF3 trial).
#pragma once

#include "../apx_combat_units.hpp"
#include <vector>

class Entity;

namespace apx
{
namespace tiers
{
	using apx::combat::Tier;
	enum class Source : unsigned char { Tag, Boss, Band, MaxHP, NotMonster };
	const char* sourceName(Source s);

	Tier tierOf(Entity* e, Source* why = nullptr);
	void tag(Entity* e, Tier t);   // the director (or the trial) assigns a creature's tier; cleared with the floor
	void untag(Entity* e);
	// the generator's band rank for the floor it builds (apx-gen floors), used by tierOf on that floor
	void recordBand(int level, const char* mapName, const std::vector<int> band[3]);
} // namespace tiers

namespace tierdmg
{
	using apx::combat::Tier;
	enum Path : int { MELEE = 0, ARROW = 1, THROWN = 2, NPATHS = 3 };
	constexpr int NTIERS = 6;

	bool enabled();            // APX_TIER_DAMAGE (default 1), unless overridden
	void setOverride(int v);   // the BF3 trial, per arm: -1 the knob, 0 off (vanilla), 1 on
	void setLogHits(bool on);  // a "[apx-tierdmg]" line per hit

	// A hostile creature's hit on a player or a player's follower: the damage its tier deals. damage is the
	// engine's damage for the hit after armour; returned unchanged for any other hit, or with the rule off.
	int onCreatureHit(Entity* attacker, Entity* target, int damage, Path path);

	struct LastHit { unsigned attacker = 0; unsigned tick = 0; Tier tier = Tier::Regular; int vanilla = 0, tiered = 0, path = 0; bool on = false; };
	extern LastHit last;       // the latest hit the rule saw (the trial attributes per-hit damage with it)

	struct Counters
	{
		long long hits[NTIERS][NPATHS] = {};
		long long vanilla[NTIERS][NPATHS] = {};
		long long out[NTIERS][NPATHS] = {};
	};
	extern Counters C;         // since start
} // namespace tierdmg
} // namespace apx
