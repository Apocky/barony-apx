// Offline unit test for src/apx/variants.hpp (card BV1 V1-2). No Barony. Build: tests/apx/run_variants_test.ps1
#include "../../src/apx/variants.hpp"
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
using namespace apx::variants;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s : ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// chi-like check: every node's measured share within 5 sigma (and 0.0005 absolute) of its analytic probability
static int compare(const char* base, int depth, int n, bool flat, bool printWorst)
{
	std::map<int, int> hist;
	for ( int i = 0; i < n; ++i ) { ++hist[roll(0xC0FFEEULL, static_cast<uint32_t>(i), base, depth, flat)]; }
	int bad = 0; double worst = 0; int worstNode = -1;
	const int root = find(base);
	for ( int i = 0; i < nodeCount(); ++i )
	{
		int r = i; while ( node(r).parent >= 0 ) { r = node(r).parent; }
		if ( r != root ) { continue; }
		const double p = probability(i, depth);
		const double q = hist.count(i) ? hist[i] / static_cast<double>(n) : 0.0;
		const double sd = std::sqrt(p * (1 - p) / n) + 1e-9;
		const double z = std::fabs(q - p) / sd;
		if ( z > worst ) { worst = z; worstNode = i; }
		if ( z > 5.0 && std::fabs(q - p) > 0.0005 ) { ++bad; }
	}
	if ( printWorst ) { std::printf("  %s depth %d flat=%d: worst z=%.2f at %s (p=%.5f)\n", base, depth, flat ? 1 : 0, worst, worstNode >= 0 ? node(worstNode).id : "-", worstNode >= 0 ? probability(worstNode, depth) : 0.0); }
	return bad;
}

int main()
{
	const int N = nodeCount();
	CHECK(N == 30, "node count %d", N);
	// table integrity: a parent precedes its children, levels step by one, loot non-empty
	for ( int i = 0; i < N; ++i )
	{
		const VNode& n = node(i);
		if ( n.parent >= 0 ) { CHECK(n.parent < i && node(n.parent).level + 1 == n.level, "node %s order or level", n.id); }
		CHECK(n.lootCount > 0, "node %s loot empty", n.id);
	}
	// probabilities: they sum to 1 per base and depth
	const char* bases[2] = { "rat", "skeleton" };
	for ( const char* b : bases )
	{
		for ( int depth : { 1, 3, 6, 12 } )
		{
			double sum = 0; const int root = find(b);
			for ( int i = 0; i < N; ++i ) { int r = i; while ( node(r).parent >= 0 ) { r = node(r).parent; } if ( r == root ) { sum += probability(i, depth); } }
			CHECK(std::fabs(sum - 1.0) < 1e-9, "%s depth %d probabilities sum to %.9f", b, depth, sum);
		}
	}
	// 100,000 rolls match the rarities
	int bad = 0;
	for ( const char* b : bases ) { for ( int depth : { 1, 4, 8 } ) { bad += compare(b, depth, 100000, false, true); } }
	CHECK(bad == 0, "%d nodes outside 5 sigma of their analytic rarity", bad);
	// depth gating: no spawn ever lands on a node whose depth_min is above the floor's depth (all depths 1..10, both bases)
	{
		int early = 0, seenRare = 0;
		for ( const char* b : bases )
		{
			for ( int depth = 1; depth <= 10; ++depth )
			{
				for ( int i = 0; i < 40000; ++i )
				{
					const int r = roll(7, static_cast<uint32_t>(i), b, depth);
					if ( node(r).level > 0 && (depth < node(r).dmin || depth > node(r).dmax) ) { ++early; }
					if ( node(r).level == 3 ) { ++seenRare; }
				}
			}
		}
		CHECK(early == 0, "%d spawns landed on a node outside its depth window", early);
		CHECK(seenRare > 0, "rare variants should still appear somewhere in 800,000 spawns");
		// shallow floors: a depth-5 rare (rat.undead.accursed.gravewhisker) can never appear on floors 1-4
		int gw = 0;
		for ( int depth = 1; depth <= 4; ++depth ) { for ( int i = 0; i < 100000; ++i ) { if ( roll(9, i, "rat", depth) == find("rat.undead.accursed.gravewhisker") ) { ++gw; } } }
		CHECK(gw == 0, "gravewhisker rolled %d times above depth 5", gw);
	}
	// determinism: same key same result, different floor seeds differ somewhere
	{
		int diff = 0;
		for ( int i = 0; i < 2000; ++i )
		{
			CHECK(roll(11, i, "rat", 6) == roll(11, i, "rat", 6), "same key must give the same variant");
			if ( roll(11, i, "rat", 6) != roll(12, i, "rat", 6) ) { ++diff; }
		}
		CHECK(diff > 20, "different floor seeds should change some rolls (%d)", diff);
	}
	// non-base share and rare share, per base (ASSUMED targets: 8-15 percent non-base on shallow floors)
	{
		int non = 0, rare = 0; const int M = 200000;
		for ( int i = 0; i < M; ++i ) { int r = roll(3, i, "rat", 6); if ( node(r).level > 0 ) { ++non; } if ( node(r).level == 3 ) { ++rare; } }
		const double share = non / static_cast<double>(M), rs = rare / static_cast<double>(M);
		std::printf("  rat depth 6: non-base share %.4f, rare share %.5f (1 in %.0f)\n", share, rs, rs > 0 ? 1.0 / rs : 0.0);
		CHECK(share > 0.08 && share < 0.16, "non-base share %.4f outside 8-16 percent", share);
	}
	// loot: each resolved row drops at its own chance; extends adds, reweight scales (rat.elemental.fire)
	{
		const int i = find("rat.elemental.fire");
		CHECK(i >= 0, "fire rat exists");
		double hide = 0, sulfur = 0; int rows = 0;
		for ( int k = 0; k < node(i).lootCount; ++k ) { const VLoot& r = lootRow(node(i), k); if ( !std::strcmp(r.id, "mat:hide") ) { hide = r.p; } if ( !std::strcmp(r.id, "mat:sulfur") ) { sulfur = r.p; } ++rows; }
		CHECK(std::fabs(hide - 0.18) < 1e-9, "fire rat hide %.3f (0.6 x 0.3 reweight)", hide);
		CHECK(sulfur > 0.3, "fire rat adds sulfur (%.3f)", sulfur);
		CHECK(rows > node(find("rat")).lootCount, "a variant's table extends its parent's (%d rows vs base %d)", rows, node(find("rat")).lootCount);
		uint64_t s = 99; int hits = 0; const int K = 100000;
		for ( int j = 0; j < K; ++j ) { for ( const char* d : drops(i, s) ) { if ( !std::strcmp(d, "mat:sulfur") ) { ++hits; } } }
		const double q = hits / static_cast<double>(K);
		CHECK(std::fabs(q - sulfur) < 5 * std::sqrt(sulfur * (1 - sulfur) / K), "sulfur drop %.4f vs table %.4f", q, sulfur);
	}
	// control: the FLAT list (ignoring the tree) must fail the rarity check
	{
		const int badFlat = compare("rat", 8, 100000, true, true);
		CHECK(badFlat > 0, "CONTROL: the flat list should NOT match the tree's rarities, but it did");
		std::printf("  flat control: %d nodes outside 5 sigma (must be > 0)\n", badFlat);
	}
	std::printf(fails ? "VARIANTS UNIT FAIL: %d\n" : "VARIANTS UNIT PASS\n", fails);
	return fails ? 1 : 0;
}
