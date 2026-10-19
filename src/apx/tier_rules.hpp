// tier_rules.hpp -- creature tiers and the damage a tier deals per hit (spec 04 sections 2 and 2.1; D72, D74).
// Pure logic, no Barony headers: unit-tested offline by tools/apx/tier_units_test.cpp in seconds. The engine
// glue (who has which tier, the hit sites) is src/apx/tiers.cpp.
//
// D74 (2026-10-19, Apocky): a creature's damage per hit is set by its tier. Fodder hits well below an unblessed
// player's hit at the same depth; regular = vanilla; elite and above unchanged. A crowd's danger comes from its
// numbers and position, not from each hit. Every number below is ASSUMED, fitted later from telemetry (spec 04
// section 8), never tuned silently.
#pragma once

#include "../apx_combat_units.hpp"
#include <algorithm>
#include <utility>
#include <vector>

namespace apx
{
namespace tierrules
{
	using apx::combat::Tier;

	// ------------------------------------------------------------------ band rank (spec 04 section 2.1; D72, D75)
	// D75 (Apocky, 2026-10-19): tiers by strength. A band's types are ordered weakest first: base LVL, then MAXHP,
	// then type id (monster_strength.inc, generated from the engine's setDefaultMonsterStats: constants, the same on
	// the host, every client and the generator). A type is fodder while the band's spawns weaker than it are under
	// kFodderBeforePct of the band ("the weakest types up to about 60 percent"): the type that crosses the line is
	// fodder too, so floor 1 (rat 40 percent, skeleton 60) is all fodder. Of the types left, the strongest is elite
	// when two or more are left; the rest are regular. Floors 2-4 (rat 40, skeleton 40, spider 10, troll 10): rat and
	// skeleton fodder, spider regular, troll elite. The curves' shares come in 10-percent steps: 55 sits between them,
	// so sampling noise cannot move a type across the line (the rule is "about 60" without the coin toss at 60).
	struct ApxStrength { int lvl, maxhp, str, estimated; };
#include "monster_strength.inc"
	constexpr int kFodderBeforePct = 55;
	constexpr int kBandSamples = 4000;   // draws of a floor's monster curve per rank (generator and run time)

	inline const ApxStrength& strengthOf(int type)
	{
		static const ApxStrength none = { 1, 0, 0, 1 };
		return (type >= 0 && type < kApxStrengthTypes) ? kApxStrength[type] : none;
	}
	inline bool weakerThan(int a, int b) // strictly weaker: LVL, then MAXHP, then id
	{
		const ApxStrength& x = strengthOf(a);
		const ApxStrength& y = strengthOf(b);
		if ( x.lvl != y.lvl ) { return x.lvl < y.lvl; }
		if ( x.maxhp != y.maxhp ) { return x.maxhp < y.maxhp; }
		return a < b;
	}

	// counts: (type, draws from the band's curve). out[0] fodder, out[1] regular, out[2] elite, weakest first.
	// One function for the generator's placement (apx_gen_bridge.cpp) and the run-time tier (tiers.cpp).
	inline void bandTiers(const std::vector<std::pair<int, int>>& counts, std::vector<int> out[3])
	{
		std::vector<std::pair<int, int>> v(counts.begin(), counts.end());
		std::sort(v.begin(), v.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) { return weakerThan(a.first, b.first); });
		long long total = 0;
		for ( const auto& c : v ) { total += c.second; }
		std::vector<int> tier(v.size(), 1);
		long long before = 0;
		size_t left = 0;
		for ( size_t k = 0; k < v.size(); ++k )
		{
			if ( before * 100 < (long long)kFodderBeforePct * total ) { tier[k] = 0; }
			else { ++left; }
			before += v[k].second;
		}
		if ( left >= 2 ) { tier[v.size() - 1] = 2; } // the strongest of the types left
		for ( size_t k = 0; k < v.size(); ++k ) { out[tier[k]].push_back(v[k].first); }
	}

	// ------------------------------------------------------------------ damage per hit by tier (D74)
	// One row per depth band (Barony floor = currentlevel; the start level 0 counts as floor 1).
	//   refPlayerHit  an unblessed player's hit there with a depth-matched weapon (ASSUMED; Apocky, D74: a +2
	//                 blessing gets a floor-1 player about 7)
	//   fodderPct     fodder's hit as a share of the vanilla hit (about one third of a regular's hit)
	//   fodderCap     fodder's largest hit: well below refPlayerHit (under half of it)
	struct DepthRow { int floorFrom; int refPlayerHit; int fodderPct; int fodderCap; };
	constexpr DepthRow kDepth[] = {
		{ 1, 5, 33, 2 },    // the Mines, floors 1-4: vanilla rat bite 3-6 and sword skeleton 5-9 become 1-2
		{ 5, 8, 33, 3 },    // floors 5-9
		{ 10, 11, 33, 4 },  // floors 10-14
		{ 15, 14, 33, 5 },  // floors 15-19
		{ 20, 17, 33, 6 },  // floors 20-24
		{ 25, 20, 33, 7 },  // floors 25-29
		{ 30, 23, 33, 8 },  // floors 30 and deeper
	};
	constexpr int kDepthRows = (int)(sizeof(kDepth) / sizeof(kDepth[0]));

	inline const DepthRow& depthRow(int floor)
	{
		int i = 0;
		while ( i + 1 < kDepthRows && floor >= kDepth[i + 1].floorFrom ) { ++i; }
		return kDepth[i];
	}

	// The damage a hit deals by the attacker's tier. vanilla is the engine's damage for this hit after armour (the
	// hit as a regular deals it); a blocked hit (0) stays 0. Fodder: a third of it, at least 1, at most the cap.
	inline int damageForTier(Tier attacker, int floor, int vanilla)
	{
		if ( vanilla <= 0 || attacker != Tier::Fodder ) { return vanilla; } // regular = vanilla; elite and above unchanged
		const DepthRow& r = depthRow(floor);
		const int third = (vanilla * r.fodderPct + 50) / 100;
		return std::max(1, std::min(r.fodderCap, third));
	}

	// The same rule on the second damage channel: a creature's poise damage per hit (F4c, by weapon class) by its
	// tier. Fodder breaks a target's poise with a third of its weapon's poise hit, at least 1; regular and above
	// unchanged. With the weapon's full poise hit, 12 fodder sword skeletons kept a passive floor-1 character
	// staggered (F4c: paralysed, taking 1.5x) 18-19 times per 20 s (MEASURED d74-s1): the crowd's danger came from
	// each hit again. Not depth-scaled: F4c's poise pools and poise hits are not.
	constexpr int kFodderPoisePct = 33;
	inline int poiseForTier(Tier attacker, int poiseHit)
	{
		if ( poiseHit <= 0 || attacker != Tier::Fodder ) { return poiseHit; }
		return std::max(1, (poiseHit * kFodderPoisePct + 50) / 100);
	}
} // namespace tierrules
} // namespace apx
