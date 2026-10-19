// apx F4b: the impact rule (spec 04 section 4). See impact.hpp.
#include "impact.hpp"
#include <cstring>

namespace apx { namespace combat {

namespace {
struct ApxMaterial { Response kind[NUM_KINDS]; };
struct ApxMonsterMaterial { int material; float level; };
#include "impact_materials.inc"

const float kShatter = 1.5f; // past fracture: bone shatters, stone cracks (keep in step with gen_materials.py)

// sword, mace, axe, polearm, ranged, magic, unarmed (momentum from G11:22-24; areas ASSUMED)
const Move kClassMoves[NUM_CLASSES] = {
	{SHEAR, 18.f, 0.6f}, {IMPACT, 24.f, 8.f}, {SHEAR, 22.f, 0.8f}, {PIERCE, 20.f, 0.25f},
	{PIERCE, 2.4f, 0.04f}, {IMPACT, 0.f, 1.f}, {IMPACT, 8.f, 10.f},
};
}

bool classMove(int c, Move& out)
{
	if ( c < 0 || c >= NUM_CLASSES || c == CLS_MAGIC ) { return false; }
	out = kClassMoves[c];
	return true;
}

Hit resolve(const Move& m, const Material& mat)
{
	const Response& r = mat.kind[m.kind];
	const float s = m.area > 0.f ? m.momentum / m.area : 0.f;
	Hit h{0.f, true, false};
	if ( s < r.yield ) { return h; }   // stagger only
	h.stagger = false;
	h.hp = (s - r.yield) * r.k;
	if ( s >= r.fracture ) { h.fracture = true; h.hp *= kShatter; }
	return h;
}

float materialMultiplier(const Move& m, const Material& mat)
{
	const float ref = resolve(m, material(0)).hp; // index 0 is FLESH
	if ( ref <= 0.f ) { return 1.f; }
	const float v = resolve(m, mat).hp / ref;
	return v < 0.f ? 0.f : (v > 3.f ? 3.f : v);
}

int numMaterials() { return kApxNumMaterials; }
const char* materialName(int i) { return (i >= 0 && i < kApxNumMaterials) ? kApxMaterialNames[i] : "?"; }
const Material& material(int i)
{
	static_assert(sizeof(ApxMaterial) == sizeof(Material), "material layout");
	if ( i < 0 || i >= kApxNumMaterials ) { i = 0; }
	return reinterpret_cast<const Material&>(kApxMaterials[i]);
}
int materialIndex(const char* name)
{
	for ( int i = 0; i < kApxNumMaterials; ++i ) { if ( !strcmp(name, kApxMaterialNames[i]) ) { return i; } }
	return -1;
}

int monsterMaterial(int t) { return (t >= 0 && t < kApxNumSeedRows) ? kApxMonsterMaterials[t].material : -1; }

float multiplierFor(int t, int c, float fallback)
{
	Move m;
	if ( !classMove(c, m) || t < 0 || t >= kApxNumSeedRows ) { return fallback; }
	const ApxMonsterMaterial& mm = kApxMonsterMaterials[t];
	return mm.level * materialMultiplier(m, material(mm.material));
}

}} // namespace apx::combat
