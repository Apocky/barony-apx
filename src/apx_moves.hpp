// apx_moves.hpp -- F4d: weapon-family move sets and charged attacks as DATA (spec 04 section 3.1; D65, D67, D75).
// PURE: no engine includes, unit-tested offline (tools/apx/unit/test_moves.cpp). The engine glue lives in
// apx_stamina.hpp (family detection, input classification, sweep, after-hit effects).
//
// All numbers are ASSUMED starting values (spec 3.1); report measured fits, do not tune silently.
// Move ids are carried in the 'ATAK' packet's byte 15, so they stay below 256. Ids 1-4 are the F4a sword.
#pragma once

#include <cmath>

namespace apx
{
namespace moves
{
	enum Family : int { F_NONE = 0, F_SWORD, F_AXE, F_MACE, F_SPEAR, F_STAFF, F_DAGGER, F_UNARMED, F_SHIELD, F_COUNT };
	enum Kind : int { K_IMPACT = 0, K_SHEAR = 1, K_PIERCE = 2 };

	// after-hit effects (bit mask)
	enum Effect : unsigned
	{
		EF_NONE = 0,
		EF_SHIELD_ASIDE = 1u << 0,   // a raised shield on the target is knocked aside for shieldTicks
		EF_SHIELD_BREAK = 1u << 1,   // a raised shield on the target is degraded / broken
		EF_SLAM = 1u << 2,           // radial stagger and push around the target (radius), small creatures knocked down
		EF_PUSH = 1u << 3,           // push the target away along the swing (force)
		EF_KNOCKDOWN_SMALL = 1u << 4, // small creatures are knocked down for knockdownTicks
		EF_CRIT_BLEED = 1u << 5,     // guaranteed critical + bleeding on an unaware or staggered target
		EF_THROW = 1u << 6,          // the single target is thrown along the facing (force), and lands as an impact
		EF_SNEAK = 1u << 7,          // bonus against an unaware target (sneakMult)
		EF_DASH = 1u << 8,           // short dash along the facing before the strike (dash)
		EF_GRAPPLE = 1u << 9,        // hold the target briefly (knockdownTicks) instead of damage
	};

	struct MoveDef
	{
		int id;
		Family fam;
		const char* name;
		Kind kind;
		double arcHalf;       // half angle of the sweep in radians; 0 = a single ray (the engine's normal hit test)
		double reach;         // multiple of STRIKERANGE
		int extra;            // extra targets after the first (0 = single target)
		double falloff1;      // damage multiple of the 1st extra target
		double falloff2;      // damage multiple of every later extra target
		double damage;        // damage multiple on every target of this move
		double cost;          // total stamina as a multiple of one swing (the press already spent 1.0)
		int heavyPct;         // D75: percent of stagger resisted while this swing is out (0..100)
		unsigned effects;
		double armourIgnore;  // fraction of the target's AC ignored
		double force;         // push / throw force (engine velocity units)
		int knockdownTicks;   // knock-down / shield-aside duration
		double radius;        // slam radius in world units
		double dash;          // dash impulse
		double sneakMult;     // EF_SNEAK / EF_CRIT_BLEED damage multiple
		bool charged;         // needs the full (18-tick) wind-up
	};

	enum MoveId : int
	{
		M_NONE = 0,
		// sword (F4a)
		SWORD_THRUST = 1, SWORD_SLASH = 2, SWORD_VERTICAL = 3, SWORD_LUNGE = 4,
		// axe
		AXE_CHOP = 10, AXE_OVERHEAD = 11, AXE_HOOK = 12, AXE_CLEAVE = 13,
		// mace / hammer
		MACE_SMASH = 20, MACE_SWING = 21, MACE_SLAM = 22,
		// spear / polearm
		SPEAR_THRUST = 30, SPEAR_SWEEP = 31, SPEAR_BRACE = 32, SPEAR_RUN = 33,
		// staff
		STAFF_SWEEP = 40, STAFF_JAB = 41, STAFF_WHIRL = 42,
		// dagger
		DAGGER_STAB = 50, DAGGER_FLICK = 51, DAGGER_ASSASSINATE = 52,
		// unarmed
		UNARMED_JAB = 60, UNARMED_GRAPPLE = 61, UNARMED_THROW = 62,
		// shield
		SHIELD_BASH = 70, SHIELD_SHOVE = 71,
	};

	inline const double PI_ = 3.14159265358979323846;

	// id, family, name, kind, arcHalf, reach, extra, f1, f2, damage, cost, heavy, effects, armourIgnore, force, kdTicks, radius, dash, sneak, charged
	inline const MoveDef* table(int& n)
	{
		static const MoveDef T[] = {
			{ SWORD_THRUST, F_SWORD, "thrust", K_PIERCE, 0.0, 1.3, 0, 1.0, 1.0, 1.0, 1.0, 0, EF_NONE, 0.0, 0.0, 0, 0.0, 0.0, 1.0, false },
			{ SWORD_SLASH, F_SWORD, "slash", K_SHEAR, PI_ / 4, 1.0, 2, 0.7, 0.5, 1.0, 1.0, 0, EF_NONE, 0.0, 0.0, 0, 0.0, 0.0, 1.0, false },
			{ SWORD_VERTICAL, F_SWORD, "vertical", K_SHEAR, 0.0, 1.0, 0, 1.0, 1.0, 1.25, 1.2, 30, EF_NONE, 0.0, 0.0, 0, 0.0, 0.0, 1.0, false },
			{ SWORD_LUNGE, F_SWORD, "lunge", K_PIERCE, 0.0, 1.4, 0, 1.0, 1.0, 1.0, 2.0, 10, EF_DASH, 0.25, 0.0, 0, 0.0, 1.8, 1.0, true },

			{ AXE_CHOP, F_AXE, "chop", K_SHEAR, PI_ / 6, 1.0, 1, 0.7, 0.7, 1.0, 1.0, 20, EF_NONE, 0.0, 0.0, 0, 0.0, 0.0, 1.0, false },
			{ AXE_OVERHEAD, F_AXE, "overhead", K_SHEAR, 0.0, 1.0, 0, 1.0, 1.0, 1.3, 1.25, 40, EF_NONE, 0.0, 0.0, 0, 0.0, 0.0, 1.0, false },
			{ AXE_HOOK, F_AXE, "hook", K_SHEAR, 0.0, 1.0, 0, 1.0, 1.0, 0.8, 1.0, 10, EF_SHIELD_ASIDE, 0.0, 0.0, 50, 0.0, 0.0, 1.0, false },
			{ AXE_CLEAVE, F_AXE, "cleave", K_SHEAR, PI_ / 2, 1.0, 3, 0.8, 0.6, 1.1, 2.0, 60, EF_SHIELD_BREAK, 0.0, 0.0, 0, 0.0, 0.0, 1.0, true },

			{ MACE_SMASH, F_MACE, "smash", K_IMPACT, 0.0, 1.0, 0, 1.0, 1.0, 1.4, 1.3, 60, EF_NONE, 0.0, 0.0, 0, 0.0, 0.0, 1.0, false },
			{ MACE_SWING, F_MACE, "swing", K_IMPACT, PI_ / 6, 1.0, 1, 0.7, 0.7, 1.0, 1.0, 40, EF_NONE, 0.0, 0.0, 0, 0.0, 0.0, 1.0, false },
			{ MACE_SLAM, F_MACE, "slam", K_IMPACT, 0.0, 1.0, 0, 1.0, 1.0, 1.3, 2.0, 100, EF_SLAM | EF_KNOCKDOWN_SMALL, 0.0, 1.2, 50, 24.0, 0.0, 1.0, true },

			{ SPEAR_THRUST, F_SPEAR, "thrust", K_PIERCE, 0.12, 1.5, 1, 0.8, 0.8, 1.0, 1.0, 0, EF_NONE, 0.0, 0.0, 0, 0.0, 0.0, 1.0, false },
			{ SPEAR_SWEEP, F_SPEAR, "sweep", K_IMPACT, PI_ / 4, 1.3, 2, 0.7, 0.5, 0.8, 1.1, 10, EF_KNOCKDOWN_SMALL, 0.0, 0.0, 40, 0.0, 0.0, 1.0, false },
			{ SPEAR_BRACE, F_SPEAR, "brace", K_PIERCE, 0.0, 1.5, 0, 1.0, 1.0, 0.6, 0.9, 0, EF_PUSH, 0.0, 1.0, 0, 0.0, 0.0, 1.0, false },
			{ SPEAR_RUN, F_SPEAR, "run-through", K_PIERCE, 0.12, 1.6, 2, 0.9, 0.8, 1.1, 2.0, 30, EF_PUSH | EF_DASH, 0.0, 1.5, 0, 0.0, 1.8, 1.0, true },

			{ STAFF_SWEEP, F_STAFF, "sweep", K_IMPACT, PI_ / 3, 1.0, 2, 0.8, 0.6, 0.9, 1.0, 20, EF_NONE, 0.0, 0.0, 0, 0.0, 0.0, 1.0, false },
			{ STAFF_JAB, F_STAFF, "jab", K_IMPACT, 0.0, 1.1, 0, 1.0, 1.0, 0.8, 0.8, 0, EF_PUSH, 0.0, 0.6, 0, 0.0, 0.0, 1.0, false },
			{ STAFF_WHIRL, F_STAFF, "whirl", K_IMPACT, PI_, 1.0, 5, 0.7, 0.7, 1.0, 2.0, 40, EF_PUSH, 0.0, 1.0, 0, 0.0, 0.0, 1.0, true },

			{ DAGGER_STAB, F_DAGGER, "stab", K_PIERCE, 0.0, 0.9, 0, 1.0, 1.0, 0.8, 0.6, 0, EF_SNEAK, 0.0, 0.0, 0, 0.0, 0.0, 1.5, false },
			{ DAGGER_FLICK, F_DAGGER, "flick", K_SHEAR, PI_ / 6, 0.9, 1, 0.6, 0.6, 0.6, 0.6, 0, EF_SNEAK, 0.0, 0.0, 0, 0.0, 0.0, 1.5, false },
			{ DAGGER_ASSASSINATE, F_DAGGER, "assassinate", K_PIERCE, 0.0, 1.0, 0, 1.0, 1.0, 1.0, 2.0, 0, EF_CRIT_BLEED | EF_SNEAK, 0.0, 0.0, 0, 0.0, 0.0, 3.0, true },

			{ UNARMED_JAB, F_UNARMED, "jab", K_IMPACT, 0.0, 0.9, 0, 1.0, 1.0, 1.0, 0.7, 0, EF_NONE, 0.0, 0.0, 0, 0.0, 0.0, 1.0, false },
			{ UNARMED_GRAPPLE, F_UNARMED, "grapple", K_IMPACT, 0.0, 0.8, 0, 1.0, 1.0, 0.4, 1.0, 0, EF_GRAPPLE, 0.0, 0.0, 60, 0.0, 0.0, 1.0, false },
			{ UNARMED_THROW, F_UNARMED, "throw", K_IMPACT, 0.0, 0.8, 0, 1.0, 1.0, 1.2, 2.0, 20, EF_THROW | EF_PUSH, 0.0, 3.0, 50, 0.0, 0.0, 1.0, true },

			{ SHIELD_BASH, F_SHIELD, "bash", K_IMPACT, 0.0, 0.9, 0, 1.0, 1.0, 0.7, 1.0, 50, EF_PUSH, 0.0, 1.2, 0, 0.0, 0.0, 1.0, false },
			{ SHIELD_SHOVE, F_SHIELD, "shove-wall", K_IMPACT, PI_ / 4, 1.1, 4, 0.8, 0.8, 0.6, 2.0, 70, EF_PUSH, 0.0, 1.6, 0, 0.0, 0.0, 1.0, true },
		};
		n = static_cast<int>(sizeof(T) / sizeof(T[0]));
		return T;
	}

	inline const MoveDef* find(int id)
	{
		int n = 0;
		const MoveDef* t = table(n);
		for ( int i = 0; i < n; ++i ) { if ( t[i].id == id ) { return &t[i]; } }
		return nullptr;
	}

	// per family: the move for no direction, horizontal mouse motion, vertical mouse motion, and the charged attack
	struct FamilyMap { Family fam; int none, horiz, vert, charged; };
	inline const FamilyMap* familyMaps(int& n)
	{
		static const FamilyMap M[] = {
			{ F_SWORD,   SWORD_THRUST,  SWORD_SLASH,   SWORD_VERTICAL, SWORD_LUNGE },
			{ F_AXE,     AXE_CHOP,      AXE_HOOK,      AXE_OVERHEAD,   AXE_CLEAVE },
			{ F_MACE,    MACE_SMASH,    MACE_SWING,    MACE_SMASH,     MACE_SLAM },
			{ F_SPEAR,   SPEAR_THRUST,  SPEAR_SWEEP,   SPEAR_BRACE,    SPEAR_RUN },
			{ F_STAFF,   STAFF_SWEEP,   STAFF_SWEEP,   STAFF_JAB,      STAFF_WHIRL },
			{ F_DAGGER,  DAGGER_STAB,   DAGGER_FLICK,  DAGGER_STAB,    DAGGER_ASSASSINATE },
			{ F_UNARMED, UNARMED_JAB,   UNARMED_JAB,   UNARMED_GRAPPLE, UNARMED_THROW },
			{ F_SHIELD,  SHIELD_BASH,   SHIELD_BASH,   SHIELD_BASH,    SHIELD_SHOVE },
		};
		n = static_cast<int>(sizeof(M) / sizeof(M[0]));
		return M;
	}

	constexpr double kYawThreshold = 0.06;   // rad of yaw change between press and release that picks the horizontal move
	constexpr double kPitchThreshold = 0.04; // rad of pitch change that picks the vertical move

	// The move a swing becomes: charged first, then the dominant mouse motion, else the family default.
	inline int classify(Family fam, double dyaw, double dpitch, bool charged)
	{
		int n = 0;
		const FamilyMap* m = familyMaps(n);
		for ( int i = 0; i < n; ++i )
		{
			if ( m[i].fam != fam ) { continue; }
			if ( charged ) { return m[i].charged; }
			const double ay = std::fabs(dyaw), ap = std::fabs(dpitch);
			if ( ay > kYawThreshold && ay >= ap ) { return m[i].horiz; }
			if ( ap > kPitchThreshold ) { return m[i].vert; }
			return m[i].none;
		}
		return M_NONE;
	}

	// does this move strike more than the engine's single ray? (the sweep timer is used)
	inline bool usesSweep(int id)
	{
		const MoveDef* d = find(id);
		return d && d->extra > 0 && d->arcHalf > 0.0;
	}

	// damage multiple of the k-th extra target (k = 0 is the first extra target)
	inline double extraScale(const MoveDef& d, int k) { return k <= 0 ? d.falloff1 : d.falloff2; }

	// integer damage after the move's multiples (isExtra: this is an extra target, k its index)
	inline int shapeDamage(const MoveDef& d, int damage, bool isExtra, int k)
	{
		double v = damage * d.damage;
		if ( isExtra ) { v *= extraScale(d, k); }
		return static_cast<int>(v);
	}

	// stamina still to spend at release, in swings (the press spent 1.0); negative = refund
	inline double extraCostSwings(const MoveDef& d) { return d.cost - 1.0; }

	// stagger-resist percent of the swing in flight (0 when none): the contract F3 reads (D75)
	inline int heavyPct(int id)
	{
		const MoveDef* d = find(id);
		return d ? d->heavyPct : 0;
	}
}
}
