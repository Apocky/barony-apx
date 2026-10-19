// tiers.cpp -- creature tiers at run time and the D74 damage rule at the hit sites. See tiers.hpp; the rules
// themselves (band rank, damage by tier and depth) are src/apx/tier_rules.hpp, unit-tested offline.
#include "../main.hpp"
#include "../game.hpp"
#include "../stat.hpp"
#include "../entity.hpp"
#include "../player.hpp"
#include "../monster.hpp"
#include "../prng.hpp"
#include "tiers.hpp"
#include "tier_rules.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <new>
#include <string>
#include <unordered_map>

namespace apx
{
namespace tierdmg
{
	static void floorSummary(int level);
	static void floorHeader(int level, const char* mapName);
}

namespace tiers
{
	static std::unordered_map<Uint32, Tier> tags;
	static int seenLevel = -1000;            // the floor the tags belong to
	static std::string seenMap;
	struct Band
	{
		bool valid = false, generator = false;
		int level = -1000;
		std::string map;
		signed char tierOfType[NUMMONSTERS];
	};
	static Band band;

	const char* sourceName(Source s)
	{
		static const char* N[] = { "tag", "boss", "band", "maxhp", "none" };
		return N[(int)s];
	}

	// a new floor: the previous floor's damage summary, its tags dropped, its band kept only if it is this floor's
	static void syncFloor()
	{
		const std::string m = map.name;
		if ( seenLevel == currentlevel && seenMap == m ) { return; }
		if ( seenLevel != -1000 ) { apx::tierdmg::floorSummary(seenLevel); }
		seenLevel = currentlevel;
		seenMap = m;
		tags.clear();
		apx::tierdmg::floorHeader(currentlevel, m.c_str()); // the knobs in every floor's log (start-up lines come before the log opens)
		if ( band.valid && (band.level != currentlevel || band.map != m) ) { band.valid = false; }
	}

	static void setBand(int level, const char* mapName, const std::vector<int> t[3], bool generator)
	{
		band.valid = true;
		band.generator = generator;
		band.level = level;
		band.map = mapName ? mapName : "";
		for ( int i = 0; i < NUMMONSTERS; ++i ) { band.tierOfType[i] = -1; }
		std::string s;
		for ( int k = 0; k < 3; ++k )
		{
			s += k ? "| " : "";
			for ( int ty : t[k] )
			{
				if ( ty <= NOTHING || ty >= NUMMONSTERS ) { continue; }
				band.tierOfType[ty] = (signed char)k;
				s += std::string(monstertypename[ty]) + " ";
			}
		}
		printlog("[apx-tiers] band level %d (%s, %s): fodder | regular | elite = %s", level, band.map.c_str(),
			generator ? "the generator's rank" : "sampled", s.c_str());
	}

	void recordBand(int level, const char* mapName, const std::vector<int> b[3])
	{
		setBand(level, mapName, b, true);
	}

	// Not an apx-gen floor: the same rank (spec 04 section 2.1) from 400 draws of this floor's monster curve, with
	// the map RNG seeded by the floor and put back (the game's map RNG does not move; same answer every time).
	static void ensureBand()
	{
		if ( band.valid ) { return; }
		BaronyRNG saved(map_rng);
		const Uint32 seed[2] = { 0xD74D74u, (Uint32)currentlevel };
		map_rng.seedBytes(seed, sizeof(seed));
		std::map<int, int> freq;
		for ( int k = 0; k < 400; ++k ) { ++freq[monsterCurve(currentlevel)]; }
		map_rng.~BaronyRNG(); // BaronyRNG has no copy-assignment: rebuilt in place from the copy (as apx_gen_bridge.cpp)
		new (&map_rng) BaronyRNG(saved);
		std::vector<std::pair<int, int>> counts;
		for ( auto& f : freq ) { if ( f.first > NOTHING && f.first < NUMMONSTERS ) { counts.push_back(f); } }
		std::vector<int> t[3];
		apx::tierrules::bandTiers(counts, t);
		setBand(currentlevel, map.name, t, false);
	}

	Tier tierOf(Entity* e, Source* why)
	{
		Source w = Source::NotMonster;
		Tier t = Tier::Regular;
		if ( e && e->behavior == &actPlayer ) { t = Tier::Player; }
		else if ( e && e->behavior == &actMonster )
		{
			syncFloor();
			Stat* s = e->getStats();
			auto it = tags.find(e->getUID());
			if ( it != tags.end() ) { w = Source::Tag; t = it->second; }
			else if ( e->isBossMonster() ) { w = Source::Boss; t = Tier::Boss; }
			else if ( s )
			{
				ensureBand();
				const int k = (s->type > NOTHING && s->type < NUMMONSTERS) ? band.tierOfType[s->type] : -1;
				if ( k >= 0 ) { w = Source::Band; t = k == 0 ? Tier::Fodder : k == 1 ? Tier::Regular : Tier::Elite; }
				else { w = Source::MaxHP; t = apx::combat::tierFromStats(s->MAXHP, false, false); }
			}
		}
		if ( why ) { *why = w; }
		return t;
	}

	void tag(Entity* e, Tier t)
	{
		if ( !e ) { return; }
		syncFloor();
		tags[e->getUID()] = t;
	}

	void untag(Entity* e)
	{
		if ( e ) { tags.erase(e->getUID()); }
	}
} // namespace tiers

namespace tierdmg
{
	LastHit last;
	Counters C;
	static Counters F;          // this floor
	static int overrideV = -1;
	static bool logHits = false;

	static int knob()
	{
		static int k = -1;
		if ( k < 0 )
		{
			const char* v = getenv("APX_TIER_DAMAGE");
			k = (v && *v) ? (atoi(v) != 0 ? 1 : 0) : 1;
			const char* lg = getenv("APX_TIER_DAMAGE_LOG");
			if ( lg && *lg && atoi(lg) ) { logHits = true; }
		}
		return k;
	}

	bool enabled() { const int k = knob(); return overrideV >= 0 ? overrideV != 0 : k != 0; }
	void setOverride(int v) { overrideV = v; }
	void setLogHits(bool on) { logHits = on; }

	static const char* pathName(int p)
	{
		static const char* N[] = { "melee", "arrow", "thrown" };
		return N[p];
	}

	static void floorSummary(int level)
	{
		std::string s;
		for ( int t = 0; t < NTIERS; ++t )
		{
			long long h = 0, v = 0, o = 0;
			for ( int p = 0; p < NPATHS; ++p ) { h += F.hits[t][p]; v += F.vanilla[t][p]; o += F.out[t][p]; }
			if ( !h ) { continue; }
			char b[160];
			snprintf(b, sizeof(b), " %s hits %lld, vanilla %lld -> %lld (%.2f -> %.2f per hit);", apx::combat::tierName((Tier)t), h, v, o,
				(double)v / h, (double)o / h);
			s += b;
		}
		if ( !s.empty() ) { printlog("[apx-tierdmg] floor %d summary:%s", level, s.c_str()); }
		F = Counters();
	}

	// a hostile creature striking a player or a player's follower (the only blows the rule touches)
	static bool hostileOnPlayerSide(Entity* attacker, Entity* target)
	{
		if ( attacker->behavior != &actMonster || attacker->monsterAllyGetPlayerLeader() ) { return false; }
		return target->behavior == &actPlayer || (target->behavior == &actMonster && target->monsterAllyGetPlayerLeader());
	}

	static int poiseKnob()
	{
		static int k = -1;
		if ( k < 0 )
		{
			const char* v = getenv("APX_TIER_POISE");
			k = (v && *v) ? (atoi(v) != 0 ? 1 : 0) : 1;
		}
		return k;
	}

	bool poiseEnabled() { return enabled() && poiseKnob() != 0; }

	static void floorHeader(int level, const char* mapName)
	{
		const apx::tierrules::DepthRow& r = apx::tierrules::depthRow(level);
		printlog("[apx-tierdmg] floor %d (%s): APX_TIER_DAMAGE=%d APX_TIER_POISE=%d override %d -> damage rule %s, poise rule %s; fodder %d%% of"
			" vanilla, cap %d (reference player hit %d), poise %d%%; regular, elite and above vanilla", level, mapName ? mapName : "",
			knob(), poiseKnob(), overrideV, enabled() ? "on" : "off", poiseEnabled() ? "on" : "off", r.fodderPct, r.fodderCap,
			r.refPlayerHit, apx::tierrules::kFodderPoisePct);
	}

	int poiseForHit(Entity* attacker, Entity* target, int poise)
	{
		if ( !attacker || !target || poise <= 0 || multiplayer == CLIENT || !hostileOnPlayerSide(attacker, target) ) { return poise; }
		const Tier t = tiers::tierOf(attacker);
		const bool on = poiseEnabled();
		const int out = on ? apx::tierrules::poiseForTier(t, poise) : poise;
		++C.poiseHits[(int)t]; C.poiseVanilla[(int)t] += poise; C.poiseOut[(int)t] += out;
		if ( last.attacker == attacker->getUID() && last.tick == ticks ) { last.poiseVanilla = poise; last.poiseTiered = out; }
		return out;
	}

	int onCreatureHit(Entity* attacker, Entity* target, int damage, Path path)
	{
		if ( !attacker || !target || damage <= 0 || multiplayer == CLIENT ) { return damage; }
		if ( !hostileOnPlayerSide(attacker, target) ) { return damage; } // hostile creatures on players and followers only
		tiers::Source why = tiers::Source::NotMonster;
		const Tier t = tiers::tierOf(attacker, &why);
		const bool on = enabled();
		const int out = on ? apx::tierrules::damageForTier(t, currentlevel, damage) : damage;
		const int ti = (int)t, pi = (int)path;
		++C.hits[ti][pi]; C.vanilla[ti][pi] += damage; C.out[ti][pi] += out;
		++F.hits[ti][pi]; F.vanilla[ti][pi] += damage; F.out[ti][pi] += out;
		last.attacker = attacker->getUID(); last.tick = ticks; last.tier = t;
		last.vanilla = damage; last.tiered = out; last.path = pi; last.on = on;
		last.poiseVanilla = last.poiseTiered = -1; // filled by poiseForHit when F4c takes the blow's poise
		if ( logHits )
		{
			Stat* s = attacker->getStats();
			printlog("[apx-tierdmg] %s atk=%u type=%s tier=%s src=%s floor=%d vanilla=%d out=%d on=%d target=%u", pathName(pi),
				attacker->getUID(), s ? monstertypename[s->type] : "?", apx::combat::tierName(t), tiers::sourceName(why), currentlevel,
				damage, out, (int)on, target->getUID());
		}
		return out;
	}
} // namespace tierdmg
} // namespace apx
