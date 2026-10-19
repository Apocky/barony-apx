// apx F4b offline gate (no Barony). Build: tools/apx/impact_test.ps1
// GATE: hits-to-kill per kind x material differs by kind for every real material.
// CONTROL: the UNIFORM material must FAIL that test (all kinds equal).
#include "../../src/apx/combat/impact.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace apx::combat;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// One representative move per kind: mace (impact), sword (shear), polearm (pierce).
static const int kKindClass[NUM_KINDS] = {CLS_MACE, CLS_SWORD, CLS_POLEARM};

static int hitsToKill(int matIdx, int kind)
{
	Move m; classMove(kKindClass[kind], m);
	const float per = 10.f * materialMultiplier(m, material(matIdx));
	return per <= 0.f ? 999 : (int)std::ceil(100.f / per - 1e-4f);
}

static bool kindsDiffer(int matIdx)
{
	const int a = hitsToKill(matIdx, 0), b = hitsToKill(matIdx, 1), c = hitsToKill(matIdx, 2);
	return !(a == b && b == c);
}

int main()
{
	// unit checks on the rule
	Material soft{}; soft.kind[IMPACT] = {1.f, 100.f, 2.f};
	Hit h = resolve(Move{IMPACT, 0.5f, 1.f}, soft);
	CHECK(h.hp == 0.f && h.stagger && !h.fracture, "below yield: hp %g stagger %d", h.hp, h.stagger);
	h = resolve(Move{IMPACT, 3.f, 1.f}, soft);
	CHECK(std::fabs(h.hp - 4.f) < 1e-5f && !h.stagger && !h.fracture, "between: hp %g", h.hp);
	h = resolve(Move{IMPACT, 300.f, 1.f}, soft);
	CHECK(h.fracture && std::fabs(h.hp - 299.f * 2.f * 1.5f) < 1e-2f, "fracture: hp %g", h.hp);
	Move dummy;
	CHECK(!classMove(CLS_MAGIC, dummy), "magic has no move");

	printf("hits-to-kill (100 HP, 10 base): material  IMPACT(mace) SHEAR(sword) PIERCE(polearm)\n");
	const int flesh = materialIndex("FLESH"), uniform = materialIndex("UNIFORM");
	int gatePass = 0, gateTotal = 0;
	for ( int i = 0; i < numMaterials(); ++i )
	{
		printf("  %-9s %4d %4d %4d%s\n", materialName(i), hitsToKill(i, 0), hitsToKill(i, 1), hitsToKill(i, 2),
			i == uniform ? "   <- control" : "");
		if ( i == flesh || i == uniform ) { continue; }
		++gateTotal;
		if ( kindsDiffer(i) ) { ++gatePass; } else { CHECK(false, "%s: kinds equal", materialName(i)); }
	}
	const bool controlFails = !kindsDiffer(uniform);
	CHECK(controlFails, "control UNIFORM should make kinds equal");

	// spec 04 section 3.2 expectations
	const int bone = materialIndex("BONE"), gel = materialIndex("GEL"), stone = materialIndex("STONE");
	CHECK(hitsToKill(bone, IMPACT) < hitsToKill(bone, PIERCE), "bone shatters to impact, arrows pass");
	CHECK(hitsToKill(gel, SHEAR) < hitsToKill(gel, IMPACT), "gel soaks impact, splits to shear");
	CHECK(hitsToKill(stone, IMPACT) < hitsToKill(stone, SHEAR), "stone glances swords, cracks to hammers");

	// table wiring: skeleton (type 9 in Monster order) is BONE; magic falls back
	CHECK(monsterMaterial(9) == bone, "skeleton material %d", monsterMaterial(9));
	CHECK(multiplierFor(9, CLS_MAGIC, 0.77f) == 0.77f, "magic uses fallback");
	CHECK(multiplierFor(9, CLS_MACE, 1.f) > multiplierFor(9, CLS_SWORD, 1.f), "skeleton: mace beats sword");

	printf("{\"gate_materials_differ\":\"%d/%d\",\"control_uniform_equal\":%s,\"failures\":%d,\"gate\":\"%s\"}\n",
		gatePass, gateTotal, controlFails ? "true" : "false", failures, failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
