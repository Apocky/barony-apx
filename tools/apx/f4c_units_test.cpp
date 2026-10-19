// f4c_units_test.cpp -- offline tests of src/apx_combat_units.hpp (no Barony). Exit 0 = all pass.
//   cl /std:c++17 /EHsc /O2 /I src tools\apx\f4c_units_test.cpp /Fe:build\f4c_units_test.exe
// APX_F4C_UNITS_BREAK=1 runs the control: poise never breaks and zones never disable, so the
// tests must fail (proves the tests can fail).
#include "apx_combat_units.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

using namespace apx::combat;

static int fails = 0, checks = 0;
static bool control = false;
#define CHECK(cond, ...) do { ++checks; if ( !(cond) ) { ++fails; std::printf("FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while ( 0 )

static int hitsToBreak(Tier t, WeaponClass w)
{
	PoiseState p;
	p.init(poiseMax(t));
	for ( int n = 1; n <= 100; ++n )
		if ( p.hit(control ? 0 : poiseHit(w)) ) return n;
	return 999;
}

int main()
{
	const char* c = std::getenv("APX_F4C_UNITS_BREAK");
	control = c && c[0] == '1';

	// tiers
	CHECK(tierFromStats(10, false, false) == Tier::Fodder, "10 hp fodder");
	CHECK(tierFromStats(40, false, false) == Tier::Regular, "40 hp regular");
	CHECK(tierFromStats(100, false, false) == Tier::Elite, "100 hp elite");
	CHECK(tierFromStats(400, false, false) == Tier::Champion, "400 hp champion");
	CHECK(tierFromStats(10, true, false) == Tier::Boss, "boss flag wins");
	CHECK(tierFromStats(10, false, true) == Tier::Player, "player flag wins");

	// spec 04 section 2: fodder breaks in 1 hit, regular in 2-3 (sword)
	int fod = hitsToBreak(Tier::Fodder, WeaponClass::Sword), reg = hitsToBreak(Tier::Regular, WeaponClass::Sword);
	int regMace = hitsToBreak(Tier::Regular, WeaponClass::Mace), eli = hitsToBreak(Tier::Elite, WeaponClass::Sword);
	CHECK(fod == 1, "fodder breaks in 1 sword hit, got %d", fod);
	CHECK(reg >= 2 && reg <= 3, "regular breaks in 2-3 sword hits, got %d", reg);
	CHECK(regMace >= 2 && regMace <= 3, "regular breaks in 2-3 mace hits, got %d", regMace);
	CHECK(eli > reg, "elite takes more hits than regular (%d vs %d)", eli, reg);
	std::printf("hits to break (sword): fodder %d regular %d elite %d champion %d boss %d; regular mace %d dagger %d\n", fod, reg, eli,
		hitsToBreak(Tier::Champion, WeaponClass::Sword), hitsToBreak(Tier::Boss, WeaponClass::Sword), regMace, hitsToBreak(Tier::Regular, WeaponClass::Dagger));

	// stagger lasts kStaggerTicks, no re-stagger while staggered, poise refills after
	{
		PoiseState p;
		p.init(poiseMax(Tier::Fodder));
		bool broke = p.hit(control ? 0 : 10);
		CHECK(broke && p.staggered(), "a breaking hit staggers");
		CHECK(!p.hit(10), "no second break while staggered");
		int t = 0;
		while ( p.staggered() && t < 1000 ) { p.tick(); ++t; }
		CHECK(t == kStaggerTicks, "stagger lasts %d ticks, got %d", kStaggerTicks, t);
		CHECK(p.value == p.max, "poise refills after the stagger");
	}
	// regen: nothing for 2 s after a hit, then 10 percent of max per second
	{
		PoiseState p;
		p.init(100);
		p.hit(50);
		for ( int t = 0; t < kPoiseRegenDelay - 1; ++t ) p.tick();
		CHECK(p.value == 50, "no regen during the delay, got %.2f", p.value);
		for ( int t = 0; t < kTicksPerSecond + 1; ++t ) p.tick();
		CHECK(p.value > 59.5 && p.value < 60.5, "10 percent per second, got %.2f", p.value);
	}
	// damage: zone multipliers and the stagger bonus
	CHECK(hitDamage(10, Zone::Head, false) == 25, "head 2.5x");
	CHECK(hitDamage(10, Zone::Torso, false) == 10, "torso 1.0x");
	CHECK(hitDamage(10, Zone::Arms, false) == 6 && hitDamage(10, Zone::Legs, false) == 6, "limbs 0.6x");
	CHECK(hitDamage(10, Zone::Torso, true) == 15, "staggered 1.5x");
	CHECK(hitDamage(1, Zone::Legs, false) == 1, "a hit does at least 1");

	// zones disable at 0, never removed, a heal restores; torso is never disabled here
	{
		ZoneState z;
		z.init(100);
		CHECK(z.max[(int)Zone::Legs] == 35 && z.max[(int)Zone::Head] == 30, "pools 35/30 of 100");
		z.damage(Zone::Legs, control ? 0 : 35);
		CHECK(z.disabled(Zone::Legs) && !z.canRun(), "legs at 0: no running");
		CHECK(z.canAttack(), "arms still attack");
		z.damage(Zone::Arms, control ? 0 : 50);
		CHECK(!z.canAttack(), "arms at 0: no attacking");
		z.damage(Zone::Torso, 1000);
		CHECK(!z.disabled(Zone::Torso), "torso is the engine HP, not a zone pool");
		z.heal(10);
		CHECK(z.canRun() && z.hp[(int)Zone::Legs] == 10, "a heal re-enables the legs");
	}
	// pitch to zone
	CHECK(zoneFromPitch(-0.5, false) == Zone::Head, "looking up: head");
	CHECK(zoneFromPitch(0.0, false) == Zone::Torso, "level: torso");
	CHECK(zoneFromPitch(0.0, true) == Zone::Arms, "level glancing: arms");
	CHECK(zoneFromPitch(0.6, false) == Zone::Legs, "looking down: legs");
	// size-weighted rolls over 10,000 draws: small creatures hit legs most, large the head most
	{
		std::mt19937 rng(12345);
		int cnt[3][4] = {};
		for ( int s = 0; s < 3; ++s )
			for ( int i = 0; i < 10000; ++i ) cnt[s][(int)rollZoneForSize(s, (int)(rng() % 100))]++;
		CHECK(cnt[0][3] > cnt[0][0] * 5, "small: legs >> head (%d vs %d)", cnt[0][3], cnt[0][0]);
		CHECK(cnt[2][0] > cnt[0][0] * 3, "large hits the head far more than small (%d vs %d)", cnt[2][0], cnt[0][0]);
		std::printf("zone rolls per 10000 (head torso arms legs): small %d %d %d %d | medium %d %d %d %d | large %d %d %d %d\n",
			cnt[0][0], cnt[0][1], cnt[0][2], cnt[0][3], cnt[1][0], cnt[1][1], cnt[1][2], cnt[1][3], cnt[2][0], cnt[2][1], cnt[2][2], cnt[2][3]);
	}
	// downed
	{
		DownedState d;
		CHECK(d.onLethal(true) && d.downed, "co-op lethal -> downed");
		CHECK(d.onLethal(true), "a lethal hit while down keeps them down");
		int t = 0, r = 0;
		while ( (r = d.tick(false, false)) == 0 && t < 5000 ) ++t;
		CHECK(r == -1 && t + 1 == kDownedTicks, "bleeds out after %d ticks, got %d", kDownedTicks, t + 1);
		d.onLethal(true);
		for ( t = 0; t < kReviveTicks - 1; ++t ) d.tick(true, false);
		CHECK(d.downed, "not revived before 3 s");
		d.tick(true, true); // a hit resets the channel
		for ( t = 0; t < kReviveTicks - 1; ++t ) d.tick(true, false);
		CHECK(d.downed, "the hit restarted the channel");
		CHECK(d.tick(true, false) == +1 && !d.downed, "revived after a full 3 s channel");
		CHECK(d.reviveHP(80) == 20, "revive at 25 percent");
		DownedState solo;
		CHECK(solo.onLethal(false), "solo: the first lethal blow of a floor downs");
		solo.tick(false, false);
		while ( solo.downed ) solo.tick(false, false);
		CHECK(!solo.onLethal(false), "solo: the second on the same floor kills");
		solo.newFloor();
		CHECK(solo.onLethal(false), "solo: a new floor gives one again");
	}
	std::printf("%s: %d checks, %d failed%s\n", fails ? "FAIL" : "PASS", checks, fails, control ? " (control APX_F4C_UNITS_BREAK=1)" : "");
	return fails ? 1 : 0;
}
