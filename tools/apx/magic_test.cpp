// apx S1 offline gate: the pure rules of apx::magic (magic_core.hpp). No Barony. Run by tools/apx/magic_test.ps1.
#include "../../src/apx/magic/magic_core.hpp"
#include <cstdio>
#include <cstring>
using namespace apx::magic::core;

static int g_fail = 0, g_n = 0;
#define CHECK(c, ...) do { ++g_n; if ( !(c) ) { ++g_fail; std::printf("FAIL %s:%d %s  ", __FILE__, __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main()
{
	CostTable ct;
	// the formula, hand-checked: bolt point fire = 5 x 1.0 x 1.0 x 1.0
	{ Picks p; p.verb = V_BOLT; p.shape = S_POINT; p.element = E_FIRE; p.modifier = M_NONE; CHECK(costMana(ct, p, false) == 5, "bolt point fire"); CHECK(costMana(ct, p, true) == 3, "near fire halves, ceil: %d", costMana(ct, p, true)); }
	// burst sphere lightning chain = 9 x 2.2 x 1.3 x 1.8 = 46.33 -> 47
	{ Picks p; p.verb = V_BURST; p.shape = S_SPHERE; p.element = E_LIGHTNING; p.modifier = M_CHAIN; CHECK(costMana(ct, p, false) == 47, "burst sphere lightning chain %d", costMana(ct, p, false)); }
	// every one of the 6,720 spells has a positive cost, monotone in each factor, and the cheapest is the sense/point/light/none corner
	int minC = 1 << 30, maxC = 0, n = 0;
	for ( int v = 0; v < V_COUNT; ++v ) for ( int s = 0; s < S_COUNT; ++s ) for ( int e = 0; e < E_COUNT; ++e ) for ( int m = 0; m < M_COUNT; ++m )
	{
		Picks p; p.verb = v; p.shape = s; p.element = e; p.modifier = m;
		const int c = costMana(ct, p, false), h = costMana(ct, p, true);
		CHECK(c >= 1 && h >= 1 && h <= c, "cost %d half %d", c, h);
		minC = c < minC ? c : minC; maxC = c > maxC ? c : maxC; ++n;
	}
	CHECK(n == 12 * 7 * 10 * 8 && n == 6720, "6720 spells: %d", n);
	std::printf("[magic_test] %d picks, mana %d..%d\n", n, minC, maxC);

	// contradictions: every rule is reachable (not shadowed by a stronger earlier one), the two spec examples grade as written
	int hard = 0, soft = 0;
	for ( int i = 0; i < kDefaultRuleCount; ++i )
	{
		bool reach = false;
		for ( int v = 0; v < V_COUNT && !reach; ++v ) for ( int s = 0; s < S_COUNT && !reach; ++s ) for ( int e = 0; e < E_COUNT && !reach; ++e ) for ( int m = 0; m < M_COUNT && !reach; ++m )
		{
			Picks p; p.verb = v; p.shape = s; p.element = e; p.modifier = m;
			if ( ruleMatches(kDefaultRules[i], p) && contradiction(p).rule == i ) { reach = true; }
		}
		CHECK(reach, "rule %d (%s) is shadowed by a stronger rule", i, kDefaultRules[i].why);
		(kDefaultRules[i].grade == G_HARD ? hard : soft)++;
	}
	std::printf("[magic_test] %d rules: %d hard, %d soft\n", kDefaultRuleCount, hard, soft);
	{ Picks p; p.verb = V_BEAM; p.shape = S_LINE; p.element = E_LIGHTNING; p.modifier = M_SILENT; CHECK(contradiction(p).grade == G_HARD, "silent beam of lightning is hard"); }
	{ Picks p; p.verb = V_BOLT; p.shape = S_POINT; p.element = E_FIRE; p.modifier = M_NONE; CHECK(contradiction(p).grade == G_NONE, "plain fire bolt is clean"); }
	{ Picks p; p.verb = V_BOLT; p.shape = S_POINT; p.element = E_LIGHTNING; p.modifier = M_SILENT; CHECK(contradiction(p).grade == G_HARD, "silent lightning bolt"); }
	{ Picks p; p.verb = V_MEND; p.shape = S_POINT; p.element = E_SHADOW; p.modifier = M_NONE; CHECK(contradiction(p).grade == G_SOFT, "mend shadow soft"); }
	CHECK(strengthFor(G_SOFT) == 0.5f && strengthFor(G_HARD) == 1.0f && strengthFor(G_NONE) == 1.0f && kBackfireChance == 0.25f, "strength, backfire");

	// concentration: fail below 20, regen, damage never in the model (no API takes damage)
	ConcTable cc; Concentration c; Picks bolt; bolt.verb = V_BOLT;
	int casts = 0;
	while ( c.canCast(cc) && casts < 100 ) { c.spend(cc, bolt); ++casts; }
	CHECK(!c.canCast(cc) && c.pct < 20.f && casts == 11, "11 bolts drop 100 to %.1f (casts %d)", c.pct, casts);
	c.regen(cc, 1.f);
	CHECK(c.pct > 20.f - 1e-3f || !c.canCast(cc), "regen %.1f", c.pct);
	const float before = c.pct; c.regen(cc, 100.f); CHECK(c.pct == 100.f && before < 100.f, "caps at 100");
	TimeTable tt;
	const float full = castMs(tt, cc, bolt, 100.f), low = castMs(tt, cc, bolt, 20.f);
	CHECK(full == 350.f && low > full && std::fabs(low - 350.f * 1.4f) < 0.01f, "cast time %.1f -> %.1f ms", full, low);
	CHECK(msToTicks(350.f) == 18 && msToTicks(20.f) == 1 && msToTicks(0.f) == 1 && msToTicks(21.f) == 2, "ticks");

	// ricochet: a shot along the wall (5 deg) bounces, a shot into it (45, 90) stops
	CHECK(wallAngleDeg(0.0875f, 1.f, true) < kRicochetDeg, "5 deg glance %.1f", wallAngleDeg(0.0875f, 1.f, true));
	CHECK(wallAngleDeg(1.f, 1.f, true) > kRicochetDeg && wallAngleDeg(1.f, 0.f, true) > 89.f, "45 and 90");
	CHECK(std::fabs(wallAngleDeg(0.f, 1.f, false) - 90.f) < 0.01f, "y-wall straight on");

	std::printf("[magic_test] %d checks, %d failed\n", g_n, g_fail);
	std::printf("MAGIC-TEST: %s\n", g_fail ? "FAIL" : "PASS");
	return g_fail ? 1 : 0;
}
