// props_rules.hpp -- BP1 pure prop rules (card BP1, D83). No Barony headers: unit-tested offline by
// tools/apx/props_units_test.cpp. The table is data/apx/props.json, embedded by gen_props_inc.py.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace apx
{
namespace props
{
	struct Outcome
	{
		int pm = 0;                 // chance per mille
		std::string spawnKind;      // a Barony monster name, or ""
		int spawnN = 0;             // D72: 1-3
		std::string spawnTier;      // fodder or regular
		std::vector<std::string> items; // one of these, chosen evenly
		std::string yield;          // "stub:F6:wood" style (given when F6 lands), or ""
	};
	struct PropRow
	{
		std::string id, behavior;
		int furnitureType;          // -1 any furniture (on destroy), -2 not furniture
		std::string collider, verb, label;
		int ticks, healEvery, hunger, encounterPm, encounterMax, regenMult;
		std::vector<Outcome> outcomes;
	};

	// the outcome for a roll r in [0, 1000): the first whose cumulative chance exceeds r, or -1 (nothing)
	inline int rollOutcome(const std::vector<Outcome>& os, int r)
	{
		int acc = 0;
		for ( size_t i = 0; i < os.size(); ++i )
		{
			acc += os[i].pm;
			if ( r < acc ) return (int)i;
		}
		return -1;
	}

	// a rest channel: ticks to run, a heal every healEvery ticks; a hit or a hostile in sight breaks it
	struct Rest
	{
		int left = 0, every = 25, healed = 0, sinceHeal = 0;
		bool active = false, broken = false, done = false;
		void start(int ticks, int healEvery) { left = ticks; every = healEvery > 0 ? healEvery : 25; healed = 0; sinceHeal = 0; active = true; broken = false; done = false; }
		// returns the HP to add this tick (0 or 1)
		int tick(bool hit, bool hostileInSight)
		{
			if ( !active ) return 0;
			if ( hit || hostileInSight ) { active = false; broken = true; return 0; }
			int heal = 0;
			if ( ++sinceHeal >= every ) { sinceHeal = 0; heal = 1; ++healed; }
			if ( --left <= 0 ) { active = false; done = true; }
			return heal;
		}
	};

	// BP1-2 repair at a field bench or anvil: one status step per use (BROKEN 0 .. EXCELLENT 4), paid in
	// grams of the item's main-part material from the F6 salvage ledger: 15 percent of the main part's
	// mass per step (ASSUMED), at least 1 g. Returns -1 when nothing to repair.
	inline int repairCostGrams(int mainPartGrams, int status)
	{
		if ( status >= 4 || status < 0 ) return -1;
		int g = mainPartGrams * 15 / 100;
		return g < 1 ? 1 : g;
	}
	// BP1-2 table: Flip for cover when a hostile is in sight and it is not flipped yet; Field bench otherwise
	inline bool tableFlips(bool hostileInSight, bool flipped) { return hostileInSight && !flipped; }

	// the history row a podium reads: stable per podium and floor
	inline int readPick(uint32_t podiumKey, int floor, int nRows)
	{
		if ( nRows <= 0 ) return -1;
		uint32_t h = podiumKey * 2654435761u ^ (uint32_t)(floor + 1) * 40503u;
		h ^= h >> 15;
		return (int)(h % (uint32_t)nRows);
	}
} // namespace props
} // namespace apx
