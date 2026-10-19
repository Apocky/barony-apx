// props_units_test.cpp -- offline tests of src/apx/props_rules.hpp and the embedded table (card BP1).
// Build: cmake -S tools/apx/units -B build/units && cmake --build build/units --config Release
// APX_PROPS_UNITS_BREAK=1 is the control: the cage chances are read as 0, so the share checks fail.
#include "apx/props_rules.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

namespace apx { namespace props {
#include "apx/props_table.inc"
} }
using namespace apx::props;

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if ( !(c) ) { ++fails; std::printf("FAIL %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while ( 0 )

int main()
{
	const char* b = std::getenv("APX_PROPS_UNITS_BREAK");
	bool brk = b && b[0] == '1';
	const PropRow* cage = nullptr;
	for ( auto& p : kPropTable ) if ( p.id == "cage_bones" ) cage = &p;
	CHECK(cage && cage->outcomes.size() == 2, "cage_bones row with 2 outcomes");
	if ( cage )
	{
		std::vector<Outcome> os = cage->outcomes;
		if ( brk ) for ( auto& o : os ) o.pm = 0;
		std::mt19937 rng(7);
		int n = 100000, cnt[3] = { 0, 0, 0 };
		for ( int i = 0; i < n; ++i ) { int k = rollOutcome(os, (int)(rng() % 1000)); cnt[k < 0 ? 2 : k]++; }
		double s = 100.0 / n;
		std::printf("cage_bones over %d rolls: skeleton %.2f%% trinket %.2f%% nothing %.2f%%\n", n, cnt[0] * s, cnt[1] * s, cnt[2] * s);
		CHECK(std::fabs(cnt[0] * s - 25.0) < 0.5, "skeleton share 25");
		CHECK(std::fabs(cnt[1] * s - 45.0) < 0.5, "trinket share 45");
		CHECK(std::fabs(cnt[2] * s - 30.0) < 0.5, "nothing share 30");
		CHECK(os[0].spawnN >= 1 && os[0].spawnN <= 3 && os[0].spawnTier == "fodder", "D72 spawn: 1-3 fodder");
	}
	// rest: 500 ticks heal 20; a hit breaks it
	{
		Rest r;
		r.start(500, 25);
		int hp = 0;
		while ( r.active ) hp += r.tick(false, false);
		CHECK(hp == 20 && r.done, "rest heals 20 over 500 ticks (got %d)", hp);
		r.start(500, 25);
		for ( int t = 0; t < 100; ++t ) r.tick(false, false);
		r.tick(true, false);
		CHECK(r.broken && !r.active, "a hit breaks the rest");
		r.start(500, 25);
		r.tick(false, true);
		CHECK(r.broken, "a hostile in sight breaks the rest");
	}
	// the podium read pick is stable and spread
	{
		CHECK(readPick(1234, 1, 60) == readPick(1234, 1, 60), "stable");
		int hits[60] = {};
		for ( uint32_t k = 0; k < 6000; ++k ) hits[readPick(k * 7919u, 2, 60)]++;
		int lo = 1 << 30, hi = 0;
		for ( int h : hits ) { lo = h < lo ? h : lo; hi = h > hi ? h : hi; }
		CHECK(lo > 50 && hi < 150, "spread over 60 rows (min %d max %d)", lo, hi);
	}
	// every use verb has a label (the world prompt)
	for ( auto& p : kPropTable )
		if ( p.verb == "rest" || p.verb == "sit" || p.verb == "bench" || p.verb == "read" ) CHECK(!p.label.empty(), "%s has a label", p.id.c_str());
	std::printf("%s: %d checks, %d failed%s\n", fails ? "FAIL" : "PASS", checks, fails, brk ? " (control)" : "");
	return fails ? 1 : 0;
}
