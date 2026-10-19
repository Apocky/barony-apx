// apx_combat_units.hpp -- F4c pure combat logic (spec 04 sections 2, 4, 5; D34). No Barony
// headers: everything here is unit-tested offline by tools/apx/f4c_units_test.cpp in seconds.
// Engine glue lives in apx_combat.hpp. Ticks are Barony ticks (50 per second).
//
// All numbers are ASSUMED starting points from spec 04 (tiers, zones, downed) and gameplay/11
// (stagger 1.5x); they are fitted from telemetry later, never tuned silently.
#pragma once

#include <algorithm>
#include <cstdint>

namespace apx
{
namespace combat
{
	constexpr int kTicksPerSecond = 50;

	// ------------------------------------------------------------------ tiers and poise
	enum class Tier : uint8_t { Fodder, Regular, Elite, Champion, Boss, Player };
	inline const char* tierName(Tier t)
	{
		static const char* N[] = { "fodder", "regular", "elite", "champion", "boss", "player" };
		return N[(int)t];
	}
	// Barony has no tiers: a creature's tier by its MAXHP (ASSUMED thresholds), bosses by flag.
	inline Tier tierFromStats(int maxhp, bool boss, bool player)
	{
		if ( player ) return Tier::Player;
		if ( boss ) return Tier::Boss;
		if ( maxhp < 20 ) return Tier::Fodder;
		if ( maxhp < 60 ) return Tier::Regular;
		if ( maxhp < 150 ) return Tier::Elite;
		return Tier::Champion;
	}
	// poise pool by tier (spec 04 section 2: fodder breaks in 1 hit, regular in 2-3)
	inline int poiseMax(Tier t)
	{
		switch ( t )
		{
			case Tier::Fodder: return 8;
			case Tier::Regular: return 24;
			case Tier::Elite: return 60;
			case Tier::Champion: return 100;
			case Tier::Boss: return 150;
			case Tier::Player: return 40;
		}
		return 24;
	}
	// weapon classes and their poise damage per hit (impact weapons break poise, spec 04 section 3)
	enum class WeaponClass : uint8_t { Unarmed, Dagger, Sword, Axe, Mace, Staff, Polearm, Ranged, Magic };
	inline int poiseHit(WeaponClass w)
	{
		switch ( w )
		{
			case WeaponClass::Unarmed: return 6;
			case WeaponClass::Dagger: return 5;
			case WeaponClass::Sword: return 10;
			case WeaponClass::Axe: return 12;
			case WeaponClass::Mace: return 16;
			case WeaponClass::Staff: return 14;
			case WeaponClass::Polearm: return 12;
			case WeaponClass::Ranged: return 4;
			case WeaponClass::Magic: return 8;
		}
		return 8;
	}
	constexpr int kStaggerTicks = 30;          // 0.6 s; the table of gameplay/11 4.2 starts as one row
	constexpr int kPoiseRegenDelay = 2 * kTicksPerSecond;
	constexpr double kStaggerDamageMult = 1.5; // gameplay/11 4.2: a staggered target takes 1.5x

	struct PoiseState
	{
		int max = 24;
		double value = 24;
		int staggerLeft = 0;    // ticks; > 0 = staggered: cannot act, takes 1.5x
		int sinceHit = 1 << 20;
		int breaks = 0;
		void init(int m) { max = m; value = m; staggerLeft = 0; sinceHit = 1 << 20; breaks = 0; }
		bool staggered() const { return staggerLeft > 0; }
		// a hit drains poise; returns true when this hit breaks it (the stagger starts)
		bool hit(int poiseDamage)
		{
			sinceHit = 0;
			if ( staggered() ) return false;  // no re-stagger while staggered
			value -= poiseDamage;
			if ( value > 0 ) return false;
			value = 0;
			staggerLeft = kStaggerTicks;
			++breaks;
			return true;
		}
		void tick()
		{
			++sinceHit;
			if ( staggerLeft > 0 )
			{
				if ( --staggerLeft == 0 ) value = max; // poise refills when the stagger ends
				return;
			}
			if ( sinceHit >= kPoiseRegenDelay && value < max )
				value = std::min<double>(max, value + max * 0.10 / kTicksPerSecond);
		}
	};

	// ------------------------------------------------------------------ HP zones (D34)
	enum class Zone : uint8_t { Head, Torso, Arms, Legs };
	inline const char* zoneName(Zone z)
	{
		static const char* N[] = { "head", "torso", "arms", "legs" };
		return N[(int)z];
	}
	inline double zoneMultiplier(Zone z)
	{
		switch ( z )
		{
			case Zone::Head: return 2.5;
			case Zone::Torso: return 1.0;
			default: return 0.6;  // limbs
		}
	}
	// zone pools as a share of MAXHP (ASSUMED); the torso is the engine's HP itself
	inline int zonePool(Zone z, int maxhp)
	{
		switch ( z )
		{
			case Zone::Head: return std::max(1, maxhp * 30 / 100);
			case Zone::Arms: return std::max(1, maxhp * 35 / 100);
			case Zone::Legs: return std::max(1, maxhp * 35 / 100);
			default: return maxhp;
		}
	}
	// a player's strike by camera pitch (radians, positive = looking down in Barony); side picks
	// arms over torso in the middle band (a glancing strike)
	inline Zone zoneFromPitch(double pitch, bool glancing)
	{
		if ( pitch < -0.20 ) return Zone::Head;
		if ( pitch > 0.35 ) return Zone::Legs;
		return glancing ? Zone::Arms : Zone::Torso;
	}
	// a creature's strike on a player: zone weights by attacker size (0 small, 1 medium, 2 large);
	// r is a uniform integer in [0, 100)
	inline Zone rollZoneForSize(int size, int r)
	{
		// weights head / torso / arms / legs, percent
		static const int W[3][4] = { { 5, 35, 20, 40 }, { 12, 48, 25, 15 }, { 25, 50, 20, 5 } };
		const int* w = W[std::max(0, std::min(2, size))];
		int acc = 0;
		for ( int k = 0; k < 4; ++k )
		{
			acc += w[k];
			if ( r < acc ) return (Zone)k;
		}
		return Zone::Torso;
	}
	struct ZoneState
	{
		int hp[4] = { 1, 1, 1, 1 };
		int max[4] = { 1, 1, 1, 1 };
		void init(int maxhp)
		{
			for ( int k = 0; k < 4; ++k ) { max[k] = zonePool((Zone)k, maxhp); hp[k] = max[k]; }
		}
		bool disabled(Zone z) const { return z != Zone::Torso && hp[(int)z] <= 0; }
		// damage after the multiplier lands on the zone pool (torso: tracked as the engine's HP)
		void damage(Zone z, int amount)
		{
			if ( z == Zone::Torso ) return;
			hp[(int)z] = std::max(0, hp[(int)z] - amount);
		}
		void heal(int amount) // a heal restores every zone pool by the same amount
		{
			for ( int k = 0; k < 4; ++k ) hp[k] = std::min(max[k], hp[k] + amount);
		}
		bool canRun() const { return !disabled(Zone::Legs); }
		bool canAttack() const { return !disabled(Zone::Arms); }
	};
	// the damage a hit does: base x zone multiplier x stagger bonus, at least 1 when base > 0
	inline int hitDamage(int base, Zone z, bool staggered)
	{
		if ( base <= 0 ) return base;
		double d = base * zoneMultiplier(z) * (staggered ? kStaggerDamageMult : 1.0);
		return std::max(1, (int)(d + 0.5));
	}
	constexpr double kLegsDisabledSpeed = 0.5;

	// ------------------------------------------------------------------ downed (spec 04 section 5)
	constexpr int kDownedTicks = 20 * kTicksPerSecond;
	constexpr int kReviveTicks = 3 * kTicksPerSecond;
	constexpr double kDownedSpeed = 0.30;
	constexpr int kRevivePercent = 25;
	constexpr double kReviveRange = 1.5; // tiles

	struct DownedState
	{
		bool downed = false;
		int left = 0;           // ticks to bleed out
		int revive = 0;         // revive channel progress, ticks
		int downsThisFloor = 0;
		int downs = 0, revives = 0, bleedouts = 0;
		// a blow would bring HP to 0 or below. Returns true when the player goes down instead of dying:
		// co-op always; solo once per floor (spec 04 section 5)
		bool onLethal(bool coop)
		{
			if ( downed ) return true;   // already down: no HP loss while down
			if ( !coop && downsThisFloor >= 1 ) return false;
			downed = true;
			left = kDownedTicks;
			revive = 0;
			++downsThisFloor;
			++downs;
			return true;
		}
		// one tick. allyNear: an ally within kReviveRange this tick; hit: either took a hit this tick.
		// Returns +1 revived, -1 bled out (die now), 0 nothing.
		int tick(bool allyNear, bool hit)
		{
			if ( !downed ) return 0;
			if ( hit || !allyNear ) revive = 0;
			else if ( ++revive >= kReviveTicks )
			{
				downed = false; revive = 0; ++revives;
				return +1;
			}
			if ( --left <= 0 )
			{
				downed = false; ++bleedouts;
				return -1;
			}
			return 0;
		}
		int reviveHP(int maxhp) const { return std::max(1, maxhp * kRevivePercent / 100); }
		void newFloor() { downsThisFloor = 0; }
	};
} // namespace combat
} // namespace apx
