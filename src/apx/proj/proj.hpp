// apx/proj/proj.hpp -- apx::proj, physical projectiles (spec 04b section 3 and 6 "Projectiles", phase C4 and S1). Owned by the MAGIC lane;
// arrows and thrown weapons use it in C4.
//
//   Entity* spawn(const ProjDef&, Entity* owner, Vec3 pos, float yaw)     a projectile flying along yaw (radians, Barony's convention)
//
// Rules (04 section 7, 04b section 6): physical bodies only, no hitscan. A projectile moves in sub-steps no longer than half its radius, so a
// fast bolt cannot tunnel through a creature or a door. The FIRST creature it meets stops it, unless the impact rule says the stress
// (momentum / area) beats that creature's material fracture for the projectile's kind: then it breaks through and flies on (pierce); a
// line-shaped spell may also carry a fixed pierce count. A wall stops it, unless it glances under 20 degrees (apx::magic::core
// wallAngleDeg): then it ricochets with 80 percent of its speed, up to 3 bounces. Every hit goes through the combat core: the combat
// lane's apx::combat::applyHit when src/apx/combat/core.hpp exists in the tree, else the stub in proj.cpp (clearly marked, same contract:
// Barony's applyGenericMagicDamage for the damage, stagger and obituary). Counters and a p50/p99 per-tick cost make a silent downgrade visible (D90).
//
// Knob: APX_MAGIC_OLD=1 is the CONTROL for apx::magic as a whole (Barony's casting path); apx::proj itself has none (nothing used it before).
#pragma once
#include <cstdint>

class Entity;

namespace apx
{
namespace proj
{
	struct Vec3 { float x = 0.f, y = 0.f, z = 0.f; Vec3() {} Vec3(float ax, float ay, float az) : x(ax), y(ay), z(az) {} };

	struct HitEvent
	{
		Entity* owner;          // may be null (the owner died)
		Entity* target;         // the creature struck
		float x, y;             // where
		int damage;             // HP taken (after the combat core's rules)
		bool pierced;           // the projectile went on through this body
		bool killed;
		uintptr_t user;         // ProjDef::user
		int user2;
		int element;
	};
	struct StepEvent { Entity* owner; Entity* projectile; float x, y; int age; uintptr_t user; int user2; int element; };
	struct EndEvent { Entity* owner; float x, y; int why; uintptr_t user; int user2; int element; };   // why: 0 life, 1 wall, 2 creature, 3 bounced out
	typedef void (*HitFn)(const HitEvent&);
	typedef void (*StepFn)(const StepEvent&);
	typedef void (*EndFn)(const EndEvent&);

	struct ProjDef
	{
		int sprite = 168;
		float speed = 4.f;              // units per 50 Hz tick (16 units = 1 tile)
		int lifeTicks = 150;
		float radius = 2.f;             // body radius in units
		int kind = 0;                   // apx::combat::Kind: IMPACT 0, SHEAR 1, PIERCE 2
		float momentum = 6.f;           // kg m/s  } the impact rule's inputs: stress = momentum / area
		float area = 20.f;              // cm^2    }
		int damage = 0;                 // precomputed damage (a spell's own number); the combat core's rule stages run on it
		int element = -1;               // apx::magic element, -1 physical
		int pierceMax = 0;              // bodies it may fly through regardless of stress
		bool ricochet = true;
		int maxBounces = 3;
		float bounceKeep = 0.8f;
		float ricochetDeg = 20.f;
		const char* light = nullptr;    // Barony light name, or null
		int trailEvery = 0;             // ticks between StepFn calls (0 = never)
		bool friendly = false;          // may strike the owner's friends (area shapes, never bolts)
		HitFn onHit = nullptr;
		StepFn onStep = nullptr;
		EndFn onEnd = nullptr;
		uintptr_t user = 0;
		int user2 = 0;
	};

	Entity* spawn(const ProjDef& def, Entity* owner, Vec3 pos, float yaw);

	// A direct blow through the same door as a flying hit (area stamps, bursts, chain arcs): the combat core applies it. Returns HP taken.
	// source: the projectile entity if any (position for gibs and direction); null makes a short-lived stand-in at (x, y).
	int strike(Entity* owner, Entity* target, const ProjDef& def, float x, float y, Entity* source, bool* killed);

	// engine hooks
	void frame();                       // once per game tick: rebuilds the candidate list, closes the cost sample
	int live();                         // projectiles in flight
	void clearAll();                    // level change

	struct Counters
	{
		long long spawned = 0, hits = 0, stops = 0, pierces = 0, fracturePierces = 0, ricochets = 0, wallStops = 0, expired = 0, stubHits = 0, coreHits = 0, substeps = 0;
	};
	const Counters& counters();
	void perfDump(double* p50us, double* p99us, int* samples, int* maxLive);   // per game tick, ticks with at least one projectile
	bool usingCombatCore();             // true when hits go through apx::combat::applyHit
}
}
