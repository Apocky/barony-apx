// apx F4b (D66; spec 04 sections 3.2, 4): damage kinds against materials.
// Pure logic, no engine headers: unit-tested offline by tools/apx/impact_test.cpp.
#pragma once

namespace apx { namespace combat {

enum Kind : int { IMPACT = 0, SHEAR = 1, PIERCE = 2, NUM_KINDS = 3 };

// Barony's damage table columns (DamageTableType order, src/monster.hpp).
enum BaronyClass : int { CLS_SWORD, CLS_MACE, CLS_AXE, CLS_POLEARM, CLS_RANGED, CLS_MAGIC, CLS_UNARMED, NUM_CLASSES };

struct Move { Kind kind; float momentum; float area; };   // kg m/s, cm^2
struct Response { float yield, fracture, k; };              // per kind
struct Material { Response kind[NUM_KINDS]; };
struct ImpactHit { float hp; bool stagger; bool fracture; };

// The reference move for a Barony class; false for magic (stays on the old table).
bool classMove(int baronyClass, Move& out);

// s = p / A against the material's response for the move's kind.
ImpactHit resolve(const Move& m, const Material& mat);

// Damage of this move on mat relative to flesh, clamped 0..3.
float materialMultiplier(const Move& m, const Material& mat);

int numMaterials();
const char* materialName(int i);
const Material& material(int i);
int materialIndex(const char* name);   // -1 if unknown

// Multiplier for a Barony monster type and class, from the generated table (seed level x material).
// Returns fallback for magic, unknown monsters or out-of-range input.
float multiplierFor(int monsterType, int baronyClass, float fallback);
// The same for a specific move (kind, momentum, area): the D93 core's impact rule (seed level x material response).
float multiplierForMove(int monsterType, const Move& m, float fallback);
int monsterMaterial(int monsterType);  // -1 if out of range

// Engine switch (-apximpact) and a use counter, so a silent fallback to the old table is visible.
void setEnabled(bool on);
bool enabled();
unsigned long long uses();
void countUse();

}} // namespace apx::combat
