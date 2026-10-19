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

	// band rank, D75: weakest types first (the engine's default LVL, then MAXHP), fodder while the weaker spawns
	// are under 55 percent of the band, the strongest of two or more left elite. Real Monster ids (monster.hpp).
	{
		enum { HUMAN = 1, RAT = 2, GOBLIN = 3, SLIME = 4, TROLL = 5, SPIDER = 7, GHOUL = 8, SKELETON = 9 };
		CHECK(strengthOf(RAT).lvl == 1 && strengthOf(SKELETON).lvl == 2 && strengthOf(SPIDER).lvl == 5 && strengthOf(TROLL).lvl == 12,
			"generated strengths: rat %d skeleton %d spider %d troll %d", strengthOf(RAT).lvl, strengthOf(SKELETON).lvl,
			strengthOf(SPIDER).lvl, strengthOf(TROLL).lvl);
		CHECK(strengthOf(SLIME).estimated == 1 && strengthOf(GOBLIN).estimated == 0, "slime's LVL is the estimated one");
		// the pre-D75 rank (F2 a90e21c3: commonest third fodder), the control's rule
		auto oldRank = [](std::vector<std::pair<int, int>> counts, std::vector<int> out[3]) {
			std::vector<std::pair<std::pair<int, int>, int>> r;
			for ( auto& c : counts ) { r.push_back({ { -c.second, c.first }, c.first }); }
			std::sort(r.begin(), r.end());
			for ( size_t k = 0; k < r.size(); ++k ) { out[r.size() < 2 ? 0 : (int)(k * 3 / r.size())].push_back(r[k].second); }
		};
		auto band = [&](std::vector<std::pair<int, int>> counts, std::vector<int> out[3]) {
			if ( control ) { oldRank(counts, out); } else { bandTiers(counts, out); }
		};
		{
			std::vector<int> t[3];
			band({ { SKELETON, 2400 }, { RAT, 1600 } }, t); // floor 1, the Mines: rat 40 pct, skeleton 60 pct
			CHECK(t[0] == std::vector<int>({ RAT, SKELETON }) && t[1].empty() && t[2].empty(), "floor 1: rats and skeletons fodder (D75)");
		}
		{
			std::vector<int> t[3];
			band({ { TROLL, 400 }, { RAT, 1600 }, { SPIDER, 400 }, { SKELETON, 1600 } }, t); // floors 2-4
			CHECK(t[0] == std::vector<int>({ RAT, SKELETON }) && t[1] == std::vector<int>({ SPIDER }) && t[2] == std::vector<int>({ TROLL }),
				"floors 2-4: rat and skeleton fodder, spider regular, troll elite");
		}
		{
			std::vector<int> t[3];
			band({ { SPIDER, 800 }, { GOBLIN, 1200 }, { SLIME, 1200 }, { GHOUL, 800 } }, t); // the swamp
			CHECK(t[0] == std::vector<int>({ SPIDER, GOBLIN, SLIME }) && t[1] == std::vector<int>({ GHOUL }) && t[2].empty(),
				"swamp: spider, goblin, slime fodder (weaker share 0, 20, 50 pct); one type left is regular, not elite");
		}
		{
			std::vector<int> t[3];
			band({ { HUMAN, 10 } }, t);
			CHECK(t[0].size() == 1 && t[1].empty() && t[2].empty(), "a band of one type is fodder");
		}
		// sampling noise: 4000 draws of the floors 2-4 curve, 2000 times, never move a type across the cut
		{
			std::mt19937 rng(0xD75);
			int moved = 0;
			for ( int trial = 0; trial < 2000; ++trial )
			{
				int n[4] = { 0, 0, 0, 0 };
				for ( int k = 0; k < kBandSamples; ++k ) { const int r = (int)(rng() % 10); ++n[r < 4 ? 0 : r < 8 ? 1 : r == 8 ? 2 : 3]; }
				std::vector<int> t[3];
				band({ { RAT, n[0] }, { SKELETON, n[1] }, { SPIDER, n[2] }, { TROLL, n[3] } }, t);
				moved += !(t[0] == std::vector<int>({ RAT, SKELETON }) && t[1] == std::vector<int>({ SPIDER }) && t[2] == std::vector<int>({ TROLL }));
			}
			CHECK(moved == 0, "sampled floors 2-4 bands changed tiers in %d of 2000", moved);
		}
		// the input order does not matter
		{
			std::vector<int> t[3], u[3];
			band({ { RAT, 1600 }, { SKELETON, 1600 }, { SPIDER, 400 }, { TROLL, 400 } }, t);
			band({ { SPIDER, 400 }, { TROLL, 400 }, { SKELETON, 1600 }, { RAT, 1600 } }, u);
			CHECK(t[0] == u[0] && t[1] == u[1] && t[2] == u[2], "band tiers depend on the input order");
		}
	}

	std::printf("%s: %d checks, %d failed%s\n", fails ? "FAIL" : "PASS", checks, fails, control ? " (control APX_TIER_UNITS_BREAK=1)" : "");
	return fails ? 1 : 0;
}
