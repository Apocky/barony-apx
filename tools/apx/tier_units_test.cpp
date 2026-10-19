// tier_units_test.cpp -- offline tests of src/apx/tier_rules.hpp (no Barony). Exit 0 = all pass.
//   cmake -S tools/apx/units -B build/units && cmake --build build/units --config Release
//   build/units/Release/tier_units_test.exe
// APX_TIER_UNITS_BREAK=1 runs the control: fodder is scored through the regular row (no tier rule, vanilla
// damage), so the fodder tests must fail (proves the tests can fail).
#include "apx/tier_rules.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <vector>

using namespace apx::tierrules;
using apx::combat::Tier;

static int fails = 0, checks = 0;
static bool control = false;
#define CHECK(cond, ...) do { ++checks; if ( !(cond) ) { ++fails; std::printf("FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while ( 0 )

static int fodder(int floor, int v) { return damageForTier(control ? Tier::Regular : Tier::Fodder, floor, v); }

int main()
{
	const char* c = std::getenv("APX_TIER_UNITS_BREAK");
	control = c && c[0] == '1';

	// D74 floor 1: a third of the vanilla hit, 1-2 per hit (vanilla rat bite 3-6, sword skeleton 5-9)
	CHECK(fodder(1, 7) == 2, "floor 1 sword skeleton 7 -> 2, got %d", fodder(1, 7));
	CHECK(fodder(1, 9) == 2, "floor 1 9 -> 2 (cap), got %d", fodder(1, 9));
	CHECK(fodder(1, 5) == 2, "floor 1 5 -> 2, got %d", fodder(1, 5));
	CHECK(fodder(1, 4) == 1, "floor 1 rat bite 4 -> 1, got %d", fodder(1, 4));
	CHECK(fodder(1, 3) == 1, "floor 1 3 -> 1, got %d", fodder(1, 3));
	CHECK(fodder(1, 1) == 1, "a landed hit stays at least 1, got %d", fodder(1, 1));
	CHECK(fodder(1, 0) == 0, "a blocked hit stays 0, got %d", fodder(1, 0));
	CHECK(fodder(1, 30) == 2, "floor 1 cap 2 for any weapon, got %d", fodder(1, 30));
	CHECK(fodder(0, 7) == fodder(1, 7), "the start level counts as floor 1");
	for ( int v = 1; v <= 40; ++v )
	{
		const int f = fodder(1, v);
		CHECK(f >= 1 && f <= 2, "floor 1 fodder hit in 1-2 for vanilla %d, got %d", v, f);
	}

	// every floor: fodder never above vanilla, at least 1, well below an unblessed player's hit (under half),
	// about a third of vanilla until the cap, non-decreasing in vanilla
	for ( int floor = 0; floor <= 40; ++floor )
	{
		const DepthRow& r = depthRow(floor);
		int prev = 0;
		for ( int v = 1; v <= 60; ++v )
		{
			const int f = fodder(floor, v);
			CHECK(f <= v, "floor %d fodder %d above vanilla %d", floor, f, v);
			CHECK(f >= 1, "floor %d fodder 0 for vanilla %d", floor, v);
			CHECK(2 * f < r.refPlayerHit, "floor %d fodder %d not well below the player's %d", floor, f, r.refPlayerHit);
			if ( v <= 3 * r.fodderCap ) { CHECK(std::fabs(f - v / 3.0) <= 1.0, "floor %d fodder %d not a third of %d", floor, f, v); }
			CHECK(f >= prev, "floor %d fodder falls from %d to %d at vanilla %d", floor, prev, f, v);
			prev = f;
		}
	}

	// the cap rises with depth: a floor-30 fodder can hit harder than a floor-1 one
	CHECK(fodder(30, 30) > fodder(10, 30) && fodder(10, 30) > fodder(1, 30), "caps 1 < 10 < 30: %d %d %d", fodder(1, 30), fodder(10, 30), fodder(30, 30));
	for ( int i = 1; i < kDepthRows; ++i )
	{
		CHECK(kDepth[i].floorFrom > kDepth[i - 1].floorFrom, "depth rows in order");
		CHECK(kDepth[i].fodderCap >= kDepth[i - 1].fodderCap && kDepth[i].refPlayerHit > kDepth[i - 1].refPlayerHit, "row %d rises", i);
	}

	// regular = vanilla; elite, champion, boss and player unchanged, on every floor
	for ( int floor = 0; floor <= 40; floor += 3 )
	{
		for ( int v = -3; v <= 60; ++v )
		{
			CHECK(damageForTier(Tier::Regular, floor, v) == v, "regular changed %d on floor %d", v, floor);
			CHECK(damageForTier(Tier::Elite, floor, v) == v, "elite changed %d on floor %d", v, floor);
			CHECK(damageForTier(Tier::Champion, floor, v) == v, "champion changed %d", v);
			CHECK(damageForTier(Tier::Boss, floor, v) == v, "boss changed %d", v);
			CHECK(damageForTier(Tier::Player, floor, v) == v, "player changed %d", v);
		}
	}

	// poise damage by tier (the second channel): fodder a third of the weapon's F4c poise hit, at least 1
	{
		using apx::combat::WeaponClass;
		using apx::combat::poiseHit;
		auto fp = [](int p) { return poiseForTier(control ? Tier::Regular : Tier::Fodder, p); };
		CHECK(fp(poiseHit(WeaponClass::Sword)) == 3, "fodder sword poise 10 -> 3, got %d", fp(poiseHit(WeaponClass::Sword)));
		CHECK(fp(poiseHit(WeaponClass::Unarmed)) == 2, "fodder bite poise 6 -> 2, got %d", fp(poiseHit(WeaponClass::Unarmed)));
		CHECK(fp(poiseHit(WeaponClass::Mace)) == 5, "fodder mace poise 16 -> 5, got %d", fp(poiseHit(WeaponClass::Mace)));
		CHECK(fp(poiseHit(WeaponClass::Ranged)) == 1, "fodder ranged poise 4 -> 1, got %d", fp(poiseHit(WeaponClass::Ranged)));
		CHECK(fp(0) == 0, "no poise hit stays 0");
		// a player's poise (F4c 40) takes over three times the fodder hits to break: no stagger lock from a crowd
		const int playerPoise = apx::combat::poiseMax(Tier::Player);
		const int swordHits = (playerPoise + fp(poiseHit(WeaponClass::Sword)) - 1) / std::max(1, fp(poiseHit(WeaponClass::Sword)));
		CHECK(swordHits >= 3 * ((playerPoise + 9) / 10), "fodder swords need %d hits to break a player's poise (vanilla 4)", swordHits);
		for ( int p = 1; p <= 40; ++p )
		{
			CHECK(poiseForTier(Tier::Regular, p) == p && poiseForTier(Tier::Elite, p) == p && poiseForTier(Tier::Boss, p) == p, "poise %d changed above fodder", p);
			const int f = fp(p);
			CHECK(f >= 1 && f <= p && std::fabs(f - p / 3.0) <= 1.0, "fodder poise %d -> %d not a third", p, f);
		}
	}

	// band rank (spec 04 section 2.1, F2): commonest third fodder, rarest third elite, ties by type id
	{
		std::vector<int> t[3];
		bandTiers({ { 7, 160 }, { 11, 240 } }, t); // floor 1, the Mines: rat 40 pct, skeleton 60 pct (ids illustrative)
		CHECK(t[0].size() == 1 && t[0][0] == 11 && t[1].size() == 1 && t[1][0] == 7 && t[2].empty(), "floor 1: skeleton fodder, rat regular");
	}
	{
		std::vector<int> t[3];
		bandTiers({ { 7, 160 }, { 11, 160 }, { 3, 40 }, { 9, 40 } }, t); // floors 2-4: rat, skeleton, spider, troll
		CHECK(t[0].size() == 2 && t[0][0] == 7 && t[0][1] == 11, "floors 2-4: the two commonest are fodder, tie by id");
		CHECK(t[1].size() == 1 && t[1][0] == 3 && t[2].size() == 1 && t[2][0] == 9, "floors 2-4: rarer pair split regular | elite by id");
	}
	{
		std::vector<int> t[3];
		bandTiers({ { 5, 10 } }, t);
		CHECK(t[0].size() == 1 && t[1].empty() && t[2].empty(), "a band of one type is fodder");
		std::vector<int> u[3];
		bandTiers({ { 1, 6 }, { 2, 5 }, { 3, 4 }, { 4, 3 }, { 5, 2 }, { 6, 1 } }, u);
		CHECK(u[0].size() == 2 && u[1].size() == 2 && u[2].size() == 2 && u[0][0] == 1 && u[2][1] == 6, "six types split in thirds by share");
	}

	// the generator's own rank as it stood inline in apx_gen_bridge.cpp (F2, a90e21c3 .. eb834ccd) and bandTiers,
	// which replaced it: the same tiers for 20,000 random bands (1-9 types, many ties), so placement is unchanged
	{
		std::mt19937 rng(0xD74);
		int differ = 0;
		for ( int trial = 0; trial < 20000; ++trial )
		{
			std::map<int, int> freq; // type -> draws, iterated by type as the generator's std::map
			const int types = 1 + (int)(rng() % 9);
			for ( int k = 0; k < types; ++k ) { freq[1 + (int)(rng() % 40)] = 1 + (int)(rng() % 6) * (1 + (int)(rng() % 70)); }
			std::vector<int> old[3];
			{
				std::vector<std::pair<std::pair<int, int>, int>> ranked;
				for ( auto& fq : freq ) { ranked.push_back({ { -fq.second, fq.first }, fq.first }); }
				std::sort(ranked.begin(), ranked.end());
				const size_t n = ranked.size();
				for ( size_t k = 0; k < n; ++k ) { old[n < 2 ? 0 : (int)(k * 3 / n)].push_back(ranked[k].second); }
			}
			std::vector<std::pair<int, int>> counts(freq.begin(), freq.end());
			std::vector<int> now[3];
			bandTiers(counts, now);
			if ( control ) { std::reverse(now[0].begin(), now[0].end()); if ( now[0].size() > 1 ) { now[1].push_back(now[0].back()); now[0].pop_back(); } }
			differ += !(old[0] == now[0] && old[1] == now[1] && old[2] == now[2]);
		}
		CHECK(differ == 0, "bandTiers differs from the generator's inline rank in %d of 20000 bands", differ);
	}

	std::printf("%s: %d checks, %d failed%s\n", fails ? "FAIL" : "PASS", checks, fails, control ? " (control APX_TIER_UNITS_BREAK=1)" : "");
	return fails ? 1 : 0;
}
