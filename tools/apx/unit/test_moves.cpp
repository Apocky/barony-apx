// Offline unit test for src/apx_moves.hpp (no Barony). Build and run: tools/apx/unit/run_unit.ps1
#include "../../../src/apx_moves.hpp"
#include <cstdio>
#include <set>
using namespace apx::moves;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL %s:%d %s : ", __FILE__, __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main()
{
	int n = 0; const MoveDef* t = table(n);
	CHECK(n == 26, "table size %d", n);
	std::set<int> ids;
	for ( int i = 0; i < n; ++i )
	{
		const MoveDef& d = t[i];
		CHECK(ids.insert(d.id).second, "duplicate id %d", d.id);
		CHECK(d.id > 0 && d.id < 256, "id %d does not fit a byte", d.id);
		CHECK(d.fam > F_NONE && d.fam < F_COUNT, "family of %s", d.name);
		CHECK(d.extra >= 0 && d.extra <= 5, "%s extra %d", d.name, d.extra);
		CHECK(d.falloff1 > 0.0 && d.falloff1 <= 1.0 && d.falloff2 > 0.0 && d.falloff2 <= 1.0, "%s falloff", d.name);
		CHECK(d.damage > 0.0 && d.damage <= 2.0, "%s damage %.2f", d.name, d.damage);
		CHECK(d.cost >= 0.5 && d.cost <= 2.5, "%s cost %.2f", d.name, d.cost);
		CHECK(d.heavyPct >= 0 && d.heavyPct <= 100, "%s heavy %d", d.name, d.heavyPct);
		CHECK(d.reach >= 0.8 && d.reach <= 1.7, "%s reach %.2f", d.name, d.reach);
		CHECK(d.extra == 0 || d.arcHalf > 0.0, "%s has extras but no arc", d.name);
		CHECK(!(d.effects & EF_SLAM) || d.radius > 0.0, "%s slam without radius", d.name);
		CHECK(!(d.effects & (EF_PUSH | EF_THROW)) || d.force > 0.0, "%s push without force", d.name);
		CHECK(!(d.effects & EF_DASH) || d.dash > 0.0, "%s dash without impulse", d.name);
		CHECK(!(d.effects & (EF_KNOCKDOWN_SMALL | EF_SHIELD_ASIDE | EF_GRAPPLE)) || d.knockdownTicks > 0, "%s ticks", d.name);
		CHECK(find(d.id) == &d, "find(%d)", d.id);
	}
	CHECK(find(0) == nullptr && find(99) == nullptr, "find on absent ids");

	// every family: 4 mapped moves that exist, belong to the family, default and charged distinct, charged is flagged charged
	int mn = 0; const FamilyMap* m = familyMaps(mn);
	CHECK(mn == F_COUNT - 1, "families mapped %d", mn);
	for ( int i = 0; i < mn; ++i )
	{
		const int ids4[4] = { m[i].none, m[i].horiz, m[i].vert, m[i].charged };
		for ( int j = 0; j < 4; ++j )
		{
			const MoveDef* d = find(ids4[j]);
			CHECK(d != nullptr, "family %d slot %d id %d missing", m[i].fam, j, ids4[j]);
			if ( d ) { CHECK(d->fam == m[i].fam, "family %d slot %d belongs to %d", m[i].fam, j, d->fam); CHECK(d->charged == (j == 3), "family %d slot %d charged flag", m[i].fam, j); }
		}
		CHECK(m[i].none != m[i].charged, "family %d default == charged", m[i].fam);
	}

	// classification truth table, for every family
	for ( int i = 0; i < mn; ++i )
	{
		const Family f = m[i].fam;
		CHECK(classify(f, 0.0, 0.0, false) == m[i].none, "f%d none", f);
		CHECK(classify(f, 0.07, 0.0, false) == m[i].horiz, "f%d horiz+", f);
		CHECK(classify(f, -0.2, 0.05, false) == m[i].horiz, "f%d horiz- dominant", f);
		CHECK(classify(f, 0.0, 0.05, false) == m[i].vert, "f%d vert", f);
		CHECK(classify(f, 0.05, 0.0, false) == m[i].none, "f%d below yaw threshold", f);
		CHECK(classify(f, 0.01, 0.03, false) == m[i].none, "f%d below pitch threshold", f);
		CHECK(classify(f, 0.5, 0.5, true) == m[i].charged, "f%d charged wins", f);
	}
	CHECK(classify(F_NONE, 1, 1, true) == M_NONE, "no family, no move");

	// sword must equal the F4a measured constants (regression against the shipped sword)
	const MoveDef* s = find(SWORD_SLASH);
	CHECK(s->extra == 2 && std::fabs(s->arcHalf - PI_ / 4) < 1e-9 && s->falloff1 == 0.7 && s->falloff2 == 0.5, "sword slash constants");
	CHECK(shapeDamage(*s, 16, true, 0) == 11 && shapeDamage(*s, 16, true, 1) == 8 && shapeDamage(*s, 16, false, 0) == 16, "slash 16 -> 11 / 8 / 16 (F4a MEASURED)");
	CHECK(shapeDamage(*find(SWORD_VERTICAL), 15, false, 0) == 18, "vertical 15 -> 18 (F4a MEASURED)");
	CHECK(std::fabs(extraCostSwings(*find(SWORD_LUNGE)) - 1.0) < 1e-9 && std::fabs(extraCostSwings(*find(SWORD_VERTICAL)) - 0.2) < 1e-9, "sword extra costs");
	CHECK(find(SWORD_THRUST)->reach == 1.3 && find(SWORD_LUNGE)->reach == 1.4 && find(SWORD_LUNGE)->armourIgnore == 0.25, "sword reach and armour");

	// heavy ordering (D75): slam > cleave > run/others; every charged heavy attack >= its family's default
	CHECK(heavyPct(MACE_SLAM) == 100 && heavyPct(MACE_SLAM) > heavyPct(AXE_CLEAVE) && heavyPct(AXE_CLEAVE) > heavyPct(SWORD_LUNGE), "heavy ordering");
	CHECK(heavyPct(SPEAR_THRUST) == 0 && heavyPct(DAGGER_STAB) == 0 && heavyPct(99) == 0, "light moves resist nothing");
	for ( int i = 0; i < mn; ++i ) { CHECK(heavyPct(m[i].charged) >= heavyPct(m[i].none) || m[i].fam == F_DAGGER, "family %d charged heavier than default", m[i].fam); }

	// design intent per family
	CHECK(usesSweep(AXE_CLEAVE) && find(AXE_CLEAVE)->arcHalf >= PI_ / 2 - 1e-9 && (find(AXE_CLEAVE)->effects & EF_SHIELD_BREAK), "cleave wide + breaks shields");
	CHECK(find(MACE_SLAM)->radius >= 24.0 && (find(MACE_SLAM)->effects & EF_KNOCKDOWN_SMALL), "slam radial knockdown");
	CHECK(find(SPEAR_RUN)->extra == 2 && (find(SPEAR_RUN)->effects & EF_PUSH), "run-through 3 in line + push");
	CHECK(find(STAFF_WHIRL)->arcHalf >= PI_ - 1e-9 && find(STAFF_WHIRL)->extra >= 4, "whirl is 360");
	CHECK((find(DAGGER_ASSASSINATE)->effects & EF_CRIT_BLEED) && find(DAGGER_ASSASSINATE)->sneakMult >= 3.0, "assassinate crit");
	CHECK((find(UNARMED_THROW)->effects & EF_THROW) && find(UNARMED_THROW)->force > 2.0, "throw");
	CHECK(find(SHIELD_SHOVE)->extra >= 3 && (find(SHIELD_SHOVE)->effects & EF_PUSH), "shove wall moves a group");
	CHECK(find(DAGGER_STAB)->cost < 1.0 && find(DAGGER_STAB)->sneakMult > 1.0, "dagger is fast and likes sneaking");
	for ( int i = 0; i < n; ++i ) { if ( t[i].charged ) { CHECK(t[i].cost >= 2.0 - 1e-9, "%s charged costs 2x", t[i].name); } }

	std::printf(fails ? "UNIT FAIL: %d\n" : "UNIT PASS: all checks\n", fails);
	return fails ? 1 : 0;
}
