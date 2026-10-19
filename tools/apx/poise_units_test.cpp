// poise_units_test.cpp -- offline tests of src/apx/poise_rules.hpp (D75 physical poise; no Barony). Exit 0 = all pass.
//   cmake -S tools/apx/units -B build/units && cmake --build build/units --config Release
//   build/units/Release/poise_units_test.exe
// APX_POISE_UNITS_BREAK=1 runs the control: armour adds no poise (perMass 0), so an armoured fighter is staggered as
// often as a robed one and the crowd tests must fail (proves the tests can fail).
#include "apx/poise_rules.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace apx::poise;
using apx::combat::WeaponClass;

static int fails = 0, checks = 0;
static bool control = false;
#define CHECK(cond, ...) do { ++checks; if ( !(cond) ) { ++fails; std::printf("FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while ( 0 )

// kits by Barony item weight (items.json weight_value) and D66 material
static const Piece kRobed[] = { { 1, M_UNIFORM }, { 10, M_UNIFORM }, { 10, M_UNIFORM } };          // doublet, hood, cloak
static const Piece kLeather[] = { { 100, M_HIDE }, { 25, M_HIDE }, { 30, M_HIDE }, { 15, M_HIDE }, { 30, M_PLANT } };
static const Piece kIron[] = { { 250, M_METAL }, { 40, M_METAL }, { 40, M_METAL }, { 20, M_HIDE }, { 40, M_METAL } };

struct CrowdResult { long long staggers = 0, locks = 0; double staggeredShare = 0; };

// 7 attackers, one blow each every 45 ticks (HITRATE), phases spread: about 6.2 blows per second for 20 s
static CrowdResult crowdPhysical(double maxPoise, double poisePerHit, bool acting, const Table& T)
{
	State s;
	s.init(maxPoise);
	for ( int t = 0; t < 1000; ++t )
	{
		for ( int a = 0; a < 7; ++a ) { if ( (t + a * 7) % 45 == 0 ) { s.hit(poisePerHit, apx::combat::kStaggerTicks, T); } }
		s.tick(acting, T);
	}
	return { s.staggers, s.locks, s.staggeredTicks / 1000.0 };
}
// the control: F4c's flat pool (Player 40), a stagger only at zero, regen after 2 s unhit; locks timed the same way
static CrowdResult crowdF4c(double poisePerHit)
{
	apx::combat::PoiseState p;
	p.init(apx::combat::poiseMax(apx::combat::Tier::Player));
	CrowdResult r;
	int sinceEnd = 1 << 20, staggered = 0;
	for ( int t = 0; t < 1000; ++t )
	{
		for ( int a = 0; a < 7; ++a )
		{
			if ( (t + a * 7) % 45 == 0 && p.hit((int)std::lround(poisePerHit)) )
			{
				++r.staggers;
				if ( sinceEnd < (int)(table().lockGapS * kTicksPerSecond) ) { ++r.locks; }
			}
		}
		const bool was = p.staggered();
		p.tick();
		staggered += was ? 1 : 0;
		if ( was && !p.staggered() ) { sinceEnd = 0; }
		else if ( !p.staggered() && sinceEnd < (1 << 20) ) { ++sinceEnd; }
	}
	r.staggeredShare = staggered / 1000.0;
	return r;
}

int main()
{
	const char* c = std::getenv("APX_POISE_UNITS_BREAK");
	control = c && c[0] == '1';
	Table T;
	if ( control ) { T.perMass = 0.0; }

	// max poise: body + armour mass x material + Endurance
	const double robed = maxPoise(1, armourPoise(kRobed, 3, T), 0, T);
	const double leather = maxPoise(1, armourPoise(kLeather, 5, T), 0, T);
	const double iron = maxPoise(1, armourPoise(kIron, 5, T), 0, T);
	std::printf("max poise (CON 1, Endurance 0): robed %.1f, leather %.1f, iron %.1f\n", robed, leather, iron);
	CHECK(robed >= 30.0 && robed < 33.0, "robed %.1f is about the body alone", robed);
	CHECK(iron >= 2.5 * robed, "an iron kit (%.1f) at least 2.5x the robed body (%.1f)", iron, robed);
	CHECK(leather > robed + 10.0 && leather < iron - 20.0, "leather %.1f between robes and iron", leather);
	CHECK(maxPoise(1, 0.0, 7, T) == maxPoise(1, 0.0, 0, T) + 7 * T.perEndurance, "Endurance 7 adds %.0f", 7 * T.perEndurance);
	CHECK(maxPoise(-40, 0.0, 0, T) == T.minMax, "a frail body keeps the floor");
	CHECK(maxPoise(5, 0.0, 0, T) > maxPoise(1, 0.0, 0, T), "CON adds poise (the body)");

	// a hit staggers only if it exceeds half the poise left: weak hits wear poise down, a fresh pool shrugs them off
	{
		State s;
		s.init(robed);
		CHECK(!s.hit(3.0, 30, T) && !s.staggered(), "a fodder sword (poise 3) cannot stagger a fresh robed pool");
		CHECK(std::fabs(s.value - (robed - 3.0)) < 1e-9, "but it wears it down by 3");
		State f;
		f.init(robed);
		CHECK(!f.hit(10.0, 30, T), "a regular sword (10) cannot stagger a fresh robed pool (%.1f)", robed);
		CHECK(f.hit(10.0, 30, T) == false && f.hit(10.0, 30, T) == true, "the third regular sword in a row staggers it");
		State h;
		h.init(robed);
		CHECK(h.hit(0.6 * robed, 30, T), "a heavy blow (60 percent of the pool) staggers a fresh pool");
		CHECK(!h.hit(50.0, 30, T), "no new stagger while staggered");
		int weak = 0;
		State w;
		w.init(iron);
		while ( !w.hit(3.0, 30, T) && weak < 1000 ) { ++weak; }
		CHECK(weak >= 25, "an iron kit takes %d fodder hits before one staggers (wear, then a share)", weak + 1);
	}
	// stance and swing multiply poise
	CHECK(guardMult(true, true, true, T) == T.guardShield && guardMult(true, false, true, T) == T.guardBrace, "shield block and brace");
	CHECK(guardMult(true, true, false, T) == 1.0 && guardMult(false, true, true, T) == 1.0, "no stamina or no block: x1");
	CHECK(swingMult(true, WeaponClass::Mace, T) == 1.75 && swingMult(true, WeaponClass::Dagger, T) == 1.0
		&& swingMult(false, WeaponClass::Mace, T) == 1.0, "heavy swings resist (mace 1.75), only while swinging");
	CHECK(swingMult(true, WeaponClass::Mace, T) > swingMult(true, WeaponClass::Axe, T) && swingMult(true, WeaponClass::Axe, T) > swingMult(true, WeaponClass::Sword, T),
		"mace > axe > sword");
	// recovery: slowly while acting, faster after 2 s unhit; the footing comes back when a stagger ends
	{
		State s;
		s.init(40.0);
		s.value = 20.0;
		s.sinceHit = 0;
		for ( int t = 0; t < 50; ++t ) { s.tick(true, T); }
		CHECK(std::fabs(s.value - (20.0 + 40.0 * T.regenActingPct / 100.0)) < 1e-6, "acting for 1 s regains %.0f percent: %.2f", T.regenActingPct, s.value);
		State q;
		q.init(40.0);
		q.value = 20.0;
		q.sinceHit = 0;
		for ( int t = 0; t < 50; ++t ) { q.tick(false, T); }
		CHECK(q.value == 20.0, "standing still while being hit regains nothing");
		State r;
		r.init(40.0);
		r.hit(30.0, 30, T);
		for ( int t = 0; t < 30; ++t ) { r.tick(false, T); }
		CHECK(!r.staggered() && r.value == 40.0, "the stagger ends after 30 ticks with the footing back");
	}
	// gear changes keep the share of the pool
	{
		State s;
		s.init(40.0);
		s.value = 10.0;
		s.setMax(80.0);
		CHECK(s.value == 20.0 && s.max == 80.0, "a quarter of the pool stays a quarter");
	}

	// the crowd, offline: 7 attackers, a passive fighter, 20 s (the trial's arms measure the same in the game)
	const CrowdResult fr = crowdPhysical(robed, 3.0, false, T), fa = crowdPhysical(iron, 3.0, false, T);
	const CrowdResult rr = crowdPhysical(robed, 10.0, false, T), ra = crowdPhysical(iron, 10.0, false, T);
	const CrowdResult cf = crowdF4c(3.0), cr = crowdF4c(10.0);
	const CrowdResult act = crowdPhysical(robed, 3.0, true, T);
	const CrowdResult rb = crowdPhysical(iron, 10.0 / guardMult(true, true, true, T), true, T); // armoured, shield up, acting
	std::printf("fodder crowd (poise 3): robed %lld staggers %lld locks, iron %lld/%lld, F4c control %lld/%lld; robed acting %lld\n",
		fr.staggers, fr.locks, fa.staggers, fa.locks, cf.staggers, cf.locks, act.staggers);
	std::printf("regular crowd (poise 10): robed %lld staggers %lld locks, iron %lld/%lld, iron blocking %lld/%lld, F4c control %lld/%lld\n",
		rr.staggers, rr.locks, ra.staggers, ra.locks, rb.staggers, rb.locks, cr.staggers, cr.locks);
	CHECK(fa.staggers * 2 <= fr.staggers && fr.staggers > 0, "fodder crowd: iron %lld staggers vs robed %lld (want at most half)", fa.staggers, fr.staggers);
	// a passive fighter under seven regular swords is knocked about in any armour; armour lessens it, a raised
	// shield (stamina) ends the lock
	CHECK(ra.staggers < rr.staggers && ra.locks < rr.locks, "regular crowd: iron %lld/%lld vs robed %lld/%lld (armour helps)",
		ra.staggers, ra.locks, rr.staggers, rr.locks);
	CHECK(rb.locks == 0, "regular crowd: an armoured fighter blocking with a shield is not locked (%lld locks)", rb.locks);
	CHECK(cr.locks >= 5, "the F4c control locks any fighter under a regular crowd: %lld", cr.locks);
	CHECK(act.staggers <= fr.staggers, "acting regains poise: %lld vs %lld staggers", act.staggers, fr.staggers);

	std::printf("%s: %d checks, %d failed%s\n", fails ? "FAIL" : "PASS", checks, fails, control ? " (control APX_POISE_UNITS_BREAK=1)" : "");
	return fails ? 1 : 0;
}
