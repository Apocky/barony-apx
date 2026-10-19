// poise_rules.hpp -- D75: a player's physical poise (spec 04 sections 2 and 4). Pure logic, no Barony headers:
// unit-tested offline by tools/apx/poise_units_test.cpp. Engine glue: src/apx_combat.hpp (F4c applyHit and tick).
// Creatures keep F4c's poise by tier. Every number in Table is ASSUMED until fitted from telemetry (spec 04 section 8).
//
// D75 (Apocky, 2026-10-19): no stagger immunity timer. Poise comes from the body, the armour's mass and material
// (D66) and Endurance; a hit starts a stagger only if its poise damage exceeds a share of the current poise, so weak
// hits wear poise down but cannot stagger alone; blocking or bracing (stamina) multiplies poise; heavy weapons resist
// stagger during their swing; poise recovers slowly while the player attacks and moves, not only after 2 s unhit.
#pragma once

#include "../apx_combat_units.hpp"
#include <algorithm>

namespace apx
{
namespace poise
{
	using apx::combat::WeaponClass;

	// D66 materials, in the order of apx/combat/impact_materials.inc
	enum Material : int { M_FLESH, M_BONE, M_GEL, M_STONE, M_CHITIN, M_METAL, M_ETHEREAL, M_PLANT, M_HIDE, M_UNIFORM, NUM_MATERIALS };

	struct Table
	{
		// the body
		double base = 30.0;             // an untrained, unarmoured body
		double perCON = 1.0;            // per point of CON (negative CON removes)
		double minMax = 10.0;           // max poise never below this
		// armour: per unit of Barony item weight (items.json weight_value) times its material's factor. Rigid
		// materials spread a blow over the body (D66 impact response): METAL 1.0 ... soft cloth 0.25.
		double perMass = 0.15;
		double material[NUM_MATERIALS] = { 0.25, 0.7, 0.25, 0.9, 0.8, 1.0, 0.25, 0.6, 0.5, 0.25 };
		// training
		double perEndurance = 4.0;      // per F4a Endurance level (0-7)
		// staggering: a hit staggers only if its poise damage exceeds this share of the poise left before it
		double staggerShare = 0.5;
		// stance multipliers (they divide the incoming poise damage, the same as multiplying poise)
		double guardShield = 2.0;       // blocking with a shield, stamina left (F4a drains it)
		double guardBrace = 1.5;        // bracing: blocking without a shield (weapon, staff), stamina left
		// during the player's own melee swing, by apx::combat::WeaponClass (Unarmed, Dagger, Sword, Axe, Mace, Staff,
		// Polearm, Ranged, Magic): heavy weapons carry the body through the blow
		double swing[9] = { 1.0, 1.0, 1.1, 1.5, 1.75, 1.25, 1.5, 1.0, 1.0 };
		// recovery
		double regenActingPct = 4.0;    // percent of max per second while attacking or moving, hit or not
		double regenIdlePct = 10.0;     // percent of max per second after idleDelayS unhit (F4c's rate)
		double idleDelayS = 2.0;
		double refillOnEnd = 1.0;       // share of max restored when a stagger ends (the footing is regained; F4c 1.0)
		// telemetry: a stagger that starts within this of the previous one's end is a stagger lock
		double lockGapS = 1.0;
	};
	inline const Table& table()
	{
		static const Table t;
		return t;
	}

	constexpr int kTicksPerSecond = 50;

	struct Piece { int weight; int material; };
	inline double armourPoise(const Piece* p, int n, const Table& T = table())
	{
		double s = 0.0;
		for ( int i = 0; i < n; ++i )
		{
			if ( p[i].weight > 0 && p[i].material >= 0 && p[i].material < NUM_MATERIALS ) { s += p[i].weight * T.material[p[i].material] * T.perMass; }
		}
		return s;
	}
	inline double maxPoise(int con, double armour, int endurance, const Table& T = table())
	{
		return std::max(T.minMax, T.base + T.perCON * con + armour + T.perEndurance * std::max(0, endurance));
	}
	inline double guardMult(bool blocking, bool shield, bool staminaLeft, const Table& T = table())
	{
		if ( !blocking || !staminaLeft ) { return 1.0; }
		return shield ? T.guardShield : T.guardBrace;
	}
	inline double swingMult(bool swinging, WeaponClass w, const Table& T = table())
	{
		const int k = (int)w;
		return (swinging && k >= 0 && k < 9) ? T.swing[k] : 1.0;
	}

	struct State
	{
		double max = 30.0, value = 30.0, minValue = 30.0;
		int staggerLeft = 0;                 // ticks; > 0 = staggered (F4c: paralysed, taking 1.5x)
		int sinceHit = 1 << 20;              // ticks since the last hit
		int sinceStaggerEnd = 1 << 20;       // ticks since the last stagger ended
		long long hits = 0, worn = 0, staggers = 0, locks = 0, staggeredTicks = 0;

		void init(double m) { max = value = minValue = m; staggerLeft = 0; sinceHit = sinceStaggerEnd = 1 << 20; }
		bool staggered() const { return staggerLeft > 0; }
		// gear, CON or Endurance changed: the same share of the pool is left
		void setMax(double m)
		{
			if ( m <= 0.0 || m == max ) { return; }
			value = max > 0.0 ? value * m / max : m;
			max = m;
			value = std::min(value, max);
		}
		// a hit of poise damage d, already divided by the stance and swing multipliers. true: the stagger starts.
		bool hit(double d, int staggerTicks, const Table& T = table())
		{
			++hits;
			sinceHit = 0;
			if ( staggered() || d <= 0.0 ) { return false; }   // already off its feet: no new stagger
			const double before = value;
			value = std::max(0.0, value - d);
			minValue = std::min(minValue, value);
			if ( d <= T.staggerShare * before ) { ++worn; return false; } // weak against what is left: wears, cannot stagger
			staggerLeft = staggerTicks;
			++staggers;
			if ( sinceStaggerEnd < (int)(T.lockGapS * kTicksPerSecond) ) { ++locks; }
			return true;
		}
		// one tick. acting: the player attacked or moved this tick.
		void tick(bool acting, const Table& T = table())
		{
			if ( sinceHit < (1 << 20) ) { ++sinceHit; }
			if ( staggerLeft > 0 )
			{
				++staggeredTicks;
				if ( --staggerLeft == 0 )
				{
					value = std::min(max, value + T.refillOnEnd * max);
					sinceStaggerEnd = 0;
				}
				return;
			}
			if ( sinceStaggerEnd < (1 << 20) ) { ++sinceStaggerEnd; }
			double pct = acting ? T.regenActingPct : 0.0;
			if ( sinceHit >= (int)(T.idleDelayS * kTicksPerSecond) ) { pct += T.regenIdlePct; }
			value = std::min(max, value + max * pct / 100.0 / kTicksPerSecond);
		}
	};
} // namespace poise
} // namespace apx
