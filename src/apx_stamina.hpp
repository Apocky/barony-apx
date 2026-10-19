// apx_stamina.hpp -- BF4 stamina (D64, D68). Header-only; state is per LOCAL player.
//
// First slice of BF4-5: a stamina pool, the yellow HUD bar (ui/GameUI.cpp), block drain, and
// attack-while-blocking. It is NOT server-authoritative yet and is not sent over the network:
// each client tracks its own stamina and gates its own swings (BF4-1 adds the synced Stat fields).
//
// Knobs (environment, read once): APX_STAMINA=0 turns the whole feature off (vanilla swing gate,
// no drain, no cost, bar hidden); APX_BLOCKSWING=0 keeps the vanilla ban on swinging while blocking.
// Telemetry: every swing and every block prints one "[apx-stamina]" line through printlog().
#pragma once

#include "game.hpp"
#include <algorithm>
#include <cstdlib>

namespace apx
{
	// ASSUMED starting values (spec 04 section 3, card BF4-5); report measured fits, do not tune silently.
	static const real_t kStaminaBaseMax = 100.0;
	static const real_t kSwingCost = 8.0;             // stamina per melee swing
	static const real_t kSwingWhileBlockCostMult = 1.5; // D64
	static const real_t kSwingWhileBlockYield = 0.6;    // D64: shield bonus while the swing is out
	static const real_t kBlockDrainPerSecond = 2.0;     // D64: plus surplus/10 per absorbed hit (hit hook is a later step)
	static const real_t kRegenPerSecond = 12.0;
	static const Uint32 kRegenPauseTicks = 40;          // 0.8 s at 50 Hz after any spend

	inline bool envOn(const char* name)
	{
		const char* v = std::getenv(name);
		return !(v && v[0] == '0');
	}
	inline bool staminaEnabled()
	{
		static const bool on = envOn("APX_STAMINA");
		return on;
	}
	inline bool blockSwingEnabled()
	{
		static const bool on = staminaEnabled() && envOn("APX_BLOCKSWING");
		return on;
	}

	struct Stamina_t
	{
		real_t value = kStaminaBaseMax;
		real_t maxValue = kStaminaBaseMax;
		Uint32 lastSpendTick = 0;
		bool swingWhileBlock = false;   // true from a swing that started under a raised block until the swing ends
		bool prevChop = false;
		bool lastDefending = false;     // block state in the tick before the current chop began
		bool blocking = false;          // previous tick, for block start/end telemetry
		Uint32 blockStartTick = 0;
		real_t blockStartValue = 0.0;
		real_t blockDrained = 0.0;
		// counters for the run summary
		int swings = 0;
		int swingsWhileBlocking = 0;
		int clamped = 0;
		real_t spentSwing = 0.0;
		real_t spentBlock = 0.0;
	};

	inline Stamina_t stamina[MAXPLAYERS];

	inline Stamina_t& get(int player)
	{
		return stamina[std::max(0, std::min(player, MAXPLAYERS - 1))];
	}

	// Single writer of stamina. reason: 0 swing, 1 block drain.
	inline real_t spend(int player, real_t amount, int reason)
	{
		Stamina_t& s = get(player);
		const real_t before = s.value;
		s.value = std::max(0.0, s.value - amount);
		if ( s.value == 0.0 && before < amount )
		{
			++s.clamped; // asked for more than we had
		}
		const real_t actual = before - s.value;
		if ( amount > 0.0 )
		{
			s.lastSpendTick = ticks;
		}
		if ( reason == 0 ) { s.spentSwing += actual; }
		else { s.spentBlock += actual; s.blockDrained += actual; }
		return actual;
	}

	// May a melee swing start now? (APX_STAMINA off: always.)
	inline bool canSwing(int player)
	{
		return !staminaEnabled() || get(player).value >= 1.0;
	}

	// ---- APX_SELFTEST=1: scripted block/swing sequence for measurement (no human input needed) ----
	// phase A t 250..750 block held (drain); B swings at 800,950,1100; C block 1300..1700 with swings at 1350,1500,1650.
	inline bool& shotRequest() { static bool b = false; return b; }
	inline bool consumeShot() { bool r = shotRequest(); shotRequest() = false; return r; }
	inline Uint32& selftestClock() { static Uint32 t = 0; return t; }
	inline bool selftestOn() { static const bool on = std::getenv("APX_SELFTEST") != nullptr; return on; }
	inline bool forceDefend()
	{
		if ( !selftestOn() ) return false;
		const Uint32 t = selftestClock();
		return (t >= 250 && t < 750) || (t >= 1300 && t < 1700);
	}
	inline bool forceSwing()
	{
		if ( !selftestOn() ) return false;
		const Uint32 t = selftestClock();
		static const Uint32 at[] = { 800, 950, 1100, 1350, 1500, 1650 };
		for ( Uint32 a : at ) { if ( t >= a && t < a + 4 ) return true; }
		return false;
	}
	inline void selftestAdvance(int player)
	{
		if ( !selftestOn() || player != 0 ) return;
		Uint32& t = selftestClock();
		++t;
		if ( t == 1400 ) { shotRequest() = true; } // mid block-plus-swing phase: HUD screenshot through the game's own back buffer
		if ( t == 1800 )
		{
			const Stamina_t& s = get(0);
			printlog("[apx-stamina] selftest_done swings=%d swingsWhileBlocking=%d spentSwing=%.2f spentBlock=%.3f clamped=%d value=%.2f",
				s.swings, s.swingsWhileBlocking, s.spentSwing, s.spentBlock, s.clamped, s.value);
		}
	}

	// Called once per game tick per local player from actHudWeapon.
	// chop = HUDWEAPON_CHOP; melee = the equipped weapon is a melee weapon (or fists); defending = stats->defending.
	inline void tick(int player, int chop, bool melee, bool defending)
	{
		if ( !staminaEnabled() )
		{
			return;
		}
		selftestAdvance(player);
		Stamina_t& s = get(player);
		const real_t perTick = 1.0 / TICKS_PER_SECOND;

		// swing start: chop leaves 0 for a melee weapon
		const bool chopping = (chop != 0) && melee;
		if ( chopping && !s.prevChop )
		{
			const bool underBlock = s.lastDefending && blockSwingEnabled();
			const real_t cost = kSwingCost * (underBlock ? kSwingWhileBlockCostMult : 1.0);
			const real_t before = s.value;
			const real_t actual = spend(player, cost, 0);
			s.swingWhileBlock = underBlock;
			++s.swings;
			if ( underBlock ) { ++s.swingsWhileBlocking; }
			printlog("[apx-stamina] swing p=%d blocking=%d cost=%.2f spent=%.2f before=%.2f after=%.2f tick=%u",
				player, underBlock ? 1 : 0, cost, actual, before, s.value, ticks);
		}
		if ( !chopping )
		{
			s.swingWhileBlock = false;
			s.lastDefending = defending; // remember the block state while no swing is out
		}
		s.prevChop = chopping;

		// block drain (block held, the shield is actually up)
		if ( defending )
		{
			if ( !s.blocking )
			{
				s.blocking = true;
				s.blockStartTick = ticks;
				s.blockStartValue = s.value;
				s.blockDrained = 0.0;
			}
			spend(player, kBlockDrainPerSecond * perTick, 1);
		}
		else if ( s.blocking )
		{
			s.blocking = false;
			const Uint32 held = ticks - s.blockStartTick;
			printlog("[apx-stamina] block_end p=%d held_ticks=%u drained=%.3f per_second=%.3f start=%.2f end=%.2f",
				player, held, s.blockDrained, held ? s.blockDrained * TICKS_PER_SECOND / held : 0.0,
				s.blockStartValue, s.value);
		}

		// regen, paused after any spend
		if ( s.value < s.maxValue && ticks - s.lastSpendTick >= kRegenPauseTicks )
		{
			s.value = std::min(s.maxValue, s.value + kRegenPerSecond * perTick);
		}
	}

	// Reset on a new level or new character.
	inline void reset(int player)
	{
		stamina[std::max(0, std::min(player, MAXPLAYERS - 1))] = Stamina_t();
	}
}
