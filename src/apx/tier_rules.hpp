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

	// ------------------------------------------------------------------ band rank (spec 04 section 2.1, F2)
	// A band's monster types rank by their share of the band's monster curve: the commonest third is fodder, the
	// middle third regular, the rarest third elite; ties by type id; a band of one type is all fodder.
	// counts: (type, draws from the curve). out[0] fodder, out[1] regular, out[2] elite, each in rank order.
	// One function for the generator's placement (apx_gen_bridge.cpp) and the runtime tier (tiers.cpp).
	inline void bandTiers(const std::vector<std::pair<int, int>>& counts, std::vector<int> out[3])
	{
		std::vector<std::pair<int, int>> ranked; // (-draws, type): commonest first
		ranked.reserve(counts.size());
		for ( const auto& c : counts ) { ranked.push_back({ -c.second, c.first }); }
		std::sort(ranked.begin(), ranked.end());
		const size_t n = ranked.size();
		for ( size_t k = 0; k < n; ++k )
		{
			out[n < 2 ? 0 : (int)(k * 3 / n)].push_back(ranked[k].second); // bottom third fodder, top third elite
		}
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
