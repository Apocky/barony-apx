// apx_stamina.hpp -- BF4: stamina (D64, D68), Endurance (D64), sword move set and charged lunge (D65, D67).
// Header-only. Stamina state is per LOCAL player, then synced to the host and relayed to the other clients
// (packet 'APXS', see sendState/onPacket). The host reads a remote player's swing-while-blocking flag and
// sword move from those packets and from the 'ATAK' packet's 16th byte.
//
// Knobs (environment, read once, default ON, "0" turns OFF and restores vanilla):
//   APX_STAMINA   whole feature (pool, bar, swing cost, drain, Endurance, moves)
//   APX_BLOCKSWING swing while blocking (vanilla ban kept when 0)
//   APX_ENDURANCE max stamina and regen stay flat when 0
//   APX_MOVES     sword move set (thrust / slash / vertical) when 0 the sword swings as vanilla
//   APX_CHARGE2   charged lunge, 18-tick wind-up, 2x stamina; when 0 the vanilla 30-tick charge
// Telemetry: "[apx-stamina]", "[apx-move]", "[apx-hit]", "[apx-net]" lines through printlog().
#pragma once

#include "game.hpp"
#include "net.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace apx
{
	// ASSUMED starting values (spec 04 sections 3, 3.1; card BF4-5, BF4b-4). Report measured fits, do not tune silently.
	static const real_t kStaminaBaseMax = 100.0;
	static const real_t kEnduranceMaxPerLevel = 5.0;    // m in max = base + Vigour*k + Endurance*m (Vigour does not exist yet)
	static const real_t kRegenPerSecond = 12.0;
	static const real_t kRegenPerEndurance = 0.05;      // regen x (1 + this*Endurance + kRegenPerDex*DEX); DEX stands in for Grace
	static const real_t kRegenPerDex = 0.01;
	static const Uint32 kRegenPauseTicks = 40;          // 0.8 s at 50 Hz after any spend
	static const real_t kSwingCost = 8.0;
	static const real_t kSwingWhileBlockCostMult = 1.5; // D64
	static const real_t kSwingWhileBlockYield = 0.6;    // D64
	static const real_t kBlockDrainPerSecond = 2.0;     // D64 (plus surplus/10 per absorbed hit: later step)
	static const int kEnduranceThresholds[7] = { 20, 50, 100, 170, 260, 370, 500 }; // G14: stamina spent in combat
	static const Uint32 kCombatWindowTicks = 500;       // a spend counts as combat if an enemy was near in the last 10 s
	static const real_t kCombatRadius = 160.0;          // 10 tiles
	// sword
	static const int kChargedWindupTicks = 18;          // D67 (vanilla 30)
	static const real_t kChargedCostMult = 2.0;         // D67
	static const real_t kThrustReach = 1.3;
	static const real_t kLungeReach = 1.4;
	static const real_t kLungeArmourIgnore = 0.25;
	static const real_t kLungeImpulse = 1.8;
	static const real_t kVerticalDamage = 1.25;
	static const real_t kVerticalCost = 1.2;
	static const real_t kSlashArc = PI / 4;             // half angle: 90 degrees total
	static const int kSlashExtraTargets = 2;            // 2-3 targets total
	static const real_t kSlashFalloff[2] = { 0.7, 0.5 };
	static const real_t kYawThreshold = 0.06;           // rad of mouse yaw between press and release that picks the slash
	static const real_t kPitchThreshold = 0.04;         // rad of pitch change that picks the vertical slash

	enum Move : int { MOVE_NONE = 0, SWORD_THRUST = 1, SWORD_SLASH = 2, SWORD_VERTICAL = 3, SWORD_LUNGE = 4 };

	inline bool envOn(const char* name)
	{
		const char* v = std::getenv(name);
		return !(v && v[0] == '0');
	}
	inline bool staminaEnabled() { static const bool on = envOn("APX_STAMINA"); return on; }
	inline bool blockSwingEnabled() { static const bool on = staminaEnabled() && envOn("APX_BLOCKSWING"); return on; }
	inline bool enduranceEnabled() { static const bool on = staminaEnabled() && envOn("APX_ENDURANCE"); return on; }
	inline bool movesEnabled() { static const bool on = staminaEnabled() && envOn("APX_MOVES"); return on; }
	inline bool charge2Enabled() { static const bool on = staminaEnabled() && envOn("APX_CHARGE2"); return on; }

	struct Stamina_t
	{
		real_t value = kStaminaBaseMax;
		real_t maxValue = kStaminaBaseMax;
		Uint32 lastSpendTick = 0;
		bool swingWhileBlock = false;
		bool prevChop = false;
		bool lastDefending = false;
		bool blocking = false;
		Uint32 blockStartTick = 0;
		real_t blockStartValue = 0.0;
		real_t blockDrained = 0.0;
		// Endurance (use tag)
		int endurance = 0;
		real_t combatSpent = 0.0;
		bool inCombat = false;
		Uint32 combatSeenTick = 0;
		// sword
		int move = MOVE_NONE;           // move of the swing in flight / last released
		real_t startYaw = 0.0, startPitch = 0.0;
		int sweepExtras = 0;            // extra targets already hit by the current slash
		// counters
		int swings = 0, swingsWhileBlocking = 0, clamped = 0;
		real_t spentSwing = 0.0, spentBlock = 0.0;
		// net
		Uint32 lastNetTick = 0;
		real_t lastNetValue = -1.0;
	};

	inline Stamina_t stamina[MAXPLAYERS];
	inline int pendingMove[MAXPLAYERS] = {};   // host: move announced by a remote client's 'ATAK'
	inline real_t extraScale = 1.0;            // set by the sweep timer around each extra-target attack

	inline Stamina_t& get(int player)
	{
		return stamina[std::max(0, std::min(player, MAXPLAYERS - 1))];
	}

	inline bool isSword(const Stat* st)
	{
		return st && st->weapon && getWeaponSkill(st->weapon) == PRO_SWORD;
	}

	inline real_t maxStaminaFor(const Stamina_t& s)
	{
		return kStaminaBaseMax + (enduranceEnabled() ? kEnduranceMaxPerLevel * s.endurance : 0.0);
	}

	// Move of the swing in flight for a player (host: remote players announce theirs in 'ATAK').
	inline int moveFor(int player)
	{
		if ( !movesEnabled() || player < 0 ) { return MOVE_NONE; }
		if ( multiplayer == SERVER && player > 0 && players[player] && !players[player]->isLocalPlayer() )
		{
			return pendingMove[player];
		}
		return get(player).move;
	}

	inline real_t reachMult(int player)
	{
		const int m = moveFor(player);
		return m == SWORD_THRUST ? kThrustReach : (m == SWORD_LUNGE ? kLungeReach : 1.0);
	}

	// Damage shaping used by Entity::attack for the sword moves.
	inline int adjustDamage(int player, int damage, int pose)
	{
		const int m = moveFor(player);
		if ( m == SWORD_VERTICAL ) { return static_cast<int>(damage * kVerticalDamage); }
		if ( m == SWORD_SLASH && pose == MONSTER_POSE_SWEEP_ATTACK_NO_UPDATE ) { return static_cast<int>(damage * extraScale); }
		return damage;
	}

	// Half-angle of the vanilla sweep timer's arc (actmagic.cpp): ours for the sword slash, PI/3 for everything else.
	inline real_t sweepArc(Entity* parent)
	{
		if ( parent && parent->behavior == &actPlayer && moveFor(parent->skill[2]) == SWORD_SLASH ) { return kSlashArc; }
		return PI / 3;
	}

	// Wind-up length for this stat: players with a sword use 18 ticks when the charged attack is on.
	inline int windupFor(Stat* st, int vanilla)
	{
		if ( !charge2Enabled() || !st ) { return vanilla; }
		for ( int i = 0; i < MAXPLAYERS; ++i )
		{
			if ( stats[i] == st ) { return isSword(st) ? std::min(vanilla, kChargedWindupTicks) : vanilla; }
		}
		return vanilla;
	}

	// ---- stamina core -------------------------------------------------------------------------------
	inline void grantEnduranceUse(int player, real_t amount)
	{
		Stamina_t& s = get(player);
		if ( !enduranceEnabled() || !s.inCombat ) { return; }
		s.combatSpent += amount;
		while ( s.endurance < 7 && s.combatSpent >= kEnduranceThresholds[s.endurance] )
		{
			++s.endurance;
			s.maxValue = maxStaminaFor(s);
			printlog("[apx-stamina] endurance p=%d level=%d combat_spent=%.2f max=%.1f", player, s.endurance, s.combatSpent, s.maxValue);
		}
	}

	// Single writer of stamina. reason: 0 swing, 1 block drain.
	inline real_t spend(int player, real_t amount, int reason)
	{
		Stamina_t& s = get(player);
		const real_t before = s.value;
		s.value = std::max(0.0, s.value - amount);
		if ( s.value == 0.0 && before < amount ) { ++s.clamped; }
		const real_t actual = before - s.value;
		if ( amount > 0.0 ) { s.lastSpendTick = ticks; }
		if ( reason == 0 ) { s.spentSwing += actual; }
		else { s.spentBlock += actual; s.blockDrained += actual; }
		grantEnduranceUse(player, actual);
		return actual;
	}

	inline bool canSwing(int player)
	{
		return !staminaEnabled() || get(player).value >= 1.0;
	}

	// An enemy monster within kCombatRadius of the player (checked every 10 ticks).
	inline void updateCombat(int player)
	{
		Stamina_t& s = get(player);
		Entity* me = players[player] ? players[player]->entity : nullptr;
		if ( me && map.creatures && ticks % 10 == 0 )
		{
			for ( node_t* node = map.creatures->first; node; node = node->next )
			{
				Entity* e = (Entity*)node->element;
				if ( !e || e->behavior != &actMonster ) { continue; }
				const real_t dx = e->x - me->x, dy = e->y - me->y;
				if ( dx * dx + dy * dy < kCombatRadius * kCombatRadius && me->checkEnemy(e) )
				{
					s.combatSeenTick = ticks;
					break;
				}
			}
		}
		s.inCombat = (s.combatSeenTick != 0 && ticks - s.combatSeenTick < kCombatWindowTicks);
	}

	// ---- network ------------------------------------------------------------------------------------
	// 'APXS' : [4]=player, [5..6]=value*10 (u16), [7..8]=max*10 (u16), [9]=flags (bit0 swingWhileBlock), [10]=endurance, [11]=move
	inline void writeState(int player, Uint8* d)
	{
		const Stamina_t& s = get(player);
		d[4] = (Uint8)player;
		SDLNet_Write16((Uint16)std::min(6553.0, s.value * 10.0), &d[5]);
		SDLNet_Write16((Uint16)std::min(6553.0, s.maxValue * 10.0), &d[7]);
		d[9] = s.swingWhileBlock ? 1 : 0;
		d[10] = (Uint8)s.endurance;
		d[11] = (Uint8)s.move;
	}

	inline void readState(const Uint8* d)
	{
		const int p = std::min((int)d[4], MAXPLAYERS - 1);
		Stamina_t& s = get(p);
		s.value = SDLNet_Read16(&d[5]) / 10.0;
		s.maxValue = SDLNet_Read16(&d[7]) / 10.0;
		s.swingWhileBlock = (d[9] & 1) != 0;
		s.endurance = d[10];
		s.move = d[11];
	}

	inline int packetsSent = 0, packetsReceived = 0;

	// Called every 10 ticks for the local player.
	inline void sendState(int player)
	{
		if ( multiplayer == SINGLE || !net_packet || !net_packet->data ) { return; }
		Stamina_t& s = get(player);
		strcpy((char*)net_packet->data, "APXS");
		writeState(player, net_packet->data);
		net_packet->len = 12;
		if ( multiplayer == CLIENT )
		{
			net_packet->address.host = net_server.host;
			net_packet->address.port = net_server.port;
			sendPacketSafe(net_sock, -1, net_packet, 0);
			++packetsSent;
		}
		else if ( multiplayer == SERVER )
		{
			for ( int c = 1; c < MAXPLAYERS; ++c )
			{
				if ( client_disconnected[c] || (players[c] && players[c]->isLocalPlayer()) ) { continue; }
				net_packet->address.host = net_clients[c - 1].host;
				net_packet->address.port = net_clients[c - 1].port;
				sendPacketSafe(net_sock, -1, net_packet, c - 1);
				++packetsSent;
			}
		}
		s.lastNetTick = ticks;
		s.lastNetValue = s.value;
	}

	// Host: a client reported its state (store, then relay to the other clients). Client: the host relayed someone's.
	inline void onPacket()
	{
		const int p = std::min((int)net_packet->data[4], MAXPLAYERS - 1);
		if ( net_packet->len < 12 || p == clientnum ) { return; } // never overwrite our own state
		if ( multiplayer == SERVER && (p <= 0 || client_disconnected[p]) ) { return; }
		const bool wasFlag = get(p).swingWhileBlock;
		const int wasEnd = get(p).endurance;
		readState(net_packet->data);
		++packetsReceived;
		if ( selftestOn() && (wasFlag != get(p).swingWhileBlock || packetsReceived % 40 == 1) )
		{
			printlog("[apx-net] recv #%d from p=%d value=%.1f max=%.1f swingWhileBlock=%d endurance=%d(was %d) move=%d tick=%u",
				packetsReceived, p, get(p).value, get(p).maxValue, get(p).swingWhileBlock ? 1 : 0, get(p).endurance, wasEnd, get(p).move, ticks);
		}
		if ( multiplayer == SERVER )
		{
			for ( int c = 1; c < MAXPLAYERS; ++c )
			{
				if ( c == p || client_disconnected[c] || (players[c] && players[c]->isLocalPlayer()) ) { continue; }
				strcpy((char*)net_packet->data, "APXS");
				writeState(p, net_packet->data);
				net_packet->len = 12;
				net_packet->address.host = net_clients[c - 1].host;
				net_packet->address.port = net_clients[c - 1].port;
				sendPacketSafe(net_sock, -1, net_packet, c - 1);
			}
		}
	}

	// ---- selftest (APX_SELFTEST=1): scripted block / swing / move sequence on the local player 0 -------------
	inline bool& shotRequest() { static bool b = false; return b; }
	inline bool consumeShot() { bool r = shotRequest(); shotRequest() = false; return r; }
	inline Uint32& selftestClock() { static Uint32 t = 0; return t; }
	inline bool selftestOn() { static const bool on = std::getenv("APX_SELFTEST") != nullptr; return on; }
	inline bool selftestMoves() { static const bool on = selftestOn() && std::getenv("APX_SELFTEST_MOVES") != nullptr; return on; }
	inline bool selftestEndurance() { static const bool on = selftestOn() && std::getenv("APX_SELFTEST_ENDURANCE") != nullptr; return on; }
	struct Win { Uint32 start, len; int move; };
	// APX_SELFTEST_NET=host|client : two processes on this PC (lobby autopilot from the F2 branch). The client runs a sword
	// script against dummies the host spawned around it; the host logs what arrives ('APXS' state, 'ATAK' move byte).
	inline int netRole() { static const int r = []() { const char* v = std::getenv("APX_SELFTEST_NET"); return v ? (v[0] == 'h' ? 1 : 2) : 0; }(); return r; }
	inline const std::vector<Win>& netScript()
	{
		static std::vector<Win> v;
		if ( v.empty() )
		{
			Uint32 t = 600;
			for ( int i = 0; i < 8; ++i ) { v.push_back({ t, 4, SWORD_SLASH }); t += 70; }
			for ( int i = 0; i < 6; ++i ) { v.push_back({ t, 4, SWORD_VERTICAL }); t += 70; }
			v.push_back({ 2050, 4, SWORD_SLASH }); v.push_back({ 2200, 4, SWORD_SLASH }); v.push_back({ 2350, 4, SWORD_SLASH });
		}
		return v;
	}
	inline bool forceDefend()
	{
		if ( selftestOn() && netRole() == 2 ) { const Uint32 t = selftestClock(); return t >= 2000 && t < 2400; }
		if ( !selftestOn() || selftestMoves() || selftestEndurance() ) return false;
		const Uint32 t = selftestClock();
		return (t >= 250 && t < 750) || (t >= 1300 && t < 1700);
	}
	// move script (APX_SELFTEST_MOVES): far dummy beyond vanilla reach, then a cluster of five
	inline const std::vector<Win>& moveScript(Uint32* spawnCluster = nullptr, Uint32* end = nullptr)
	{
		static std::vector<Win> v; static Uint32 sc = 0, en = 0;
		if ( v.empty() )
		{
			Uint32 t = 300;
			for ( int i = 0; i < 6; ++i ) { v.push_back({ t, 4, MOVE_NONE }); t += 70; }
			for ( int i = 0; i < 6; ++i ) { v.push_back({ t, 4, SWORD_THRUST }); t += 70; }
			for ( int i = 0; i < 6; ++i ) { v.push_back({ t, 50, SWORD_LUNGE }); t += 120; }
			sc = t + 30; t = sc + 60;
			const int order[4] = { MOVE_NONE, SWORD_SLASH, SWORD_VERTICAL, SWORD_THRUST };
			for ( int m : order ) { for ( int i = 0; i < 10; ++i ) { v.push_back({ t, 4, m }); t += 70; } }
			en = t + 100;
		}
		if ( spawnCluster ) { *spawnCluster = sc; }
		if ( end ) { *end = en; }
		return v;
	}
	// swing windows {start, length}: stamina script, move script, or Endurance script
	inline bool forceSwing()
	{
		if ( !selftestOn() ) return false;
		const Uint32 t = selftestClock();
		static const Uint32 stam[][2] = { {800,4}, {950,4}, {1100,4}, {1350,4}, {1500,4}, {1650,4} };
		if ( netRole() == 2 ) { for ( auto& w : netScript() ) { if ( t >= w.start && t < w.start + w.len ) return true; } return false; }
		if ( netRole() == 1 ) { return false; }
		if ( selftestEndurance() ) { return t >= 100 && t < 4700 && (t - 100) % 70 < 4; }
		if ( selftestMoves() ) { for ( auto& w : moveScript() ) { if ( t >= w.start && t < w.start + w.len ) return true; } return false; }
		for ( auto& w : stam ) { if ( t >= w[0] && t < w[0] + w[1] ) return true; }
		return false;
	}
	inline int forcedMove()
	{
		const Uint32 t = selftestClock();
		int m = MOVE_NONE;
		if ( netRole() == 2 ) { for ( auto& w : netScript() ) { if ( t >= w.start ) { m = w.move; } } return m; }
		for ( auto& w : moveScript() ) { if ( t >= w.start ) { m = w.move; } }
		return m;
	}
	inline Entity*& farDummy() { static Entity* e = nullptr; return e; }
	inline void selftestSpawn(int phase)
	{
		Entity* me = players[0] ? players[0]->entity : nullptr;
		if ( !me ) return;
		auto put = [&](real_t ang, real_t dist)
		{
			Entity* d = summonMonster(DUMMYBOT, me->x + cos(me->yaw + ang) * dist, me->y + sin(me->yaw + ang) * dist, true);
			if ( d && d->getStats() ) { d->getStats()->HP = d->getStats()->MAXHP = 9999; }
			return d;
		};
		if ( phase == 0 ) { farDummy() = put(0.0, 33.0); } // beyond vanilla reach (24 + body), inside thrust reach (31 + body)
		if ( phase == 1 ) { const real_t a[5] = { -0.7, -0.35, 0.0, 0.35, 0.7 }; for ( real_t x : a ) put(x, STRIKERANGE * 0.6); }
		printlog("[apx-move] selftest_spawn phase=%d yaw=%.3f pos=%.1f,%.1f", phase, me->yaw, me->x, me->y);
	}
	inline void selftestAdvance(int player)
	{
		if ( !selftestOn() ) return;
		if ( netRole() != 0 )
		{
			if ( player != clientnum ) return;
			Uint32& t = selftestClock();
			++t;
			if ( netRole() == 2 && t == 5 )
			{
				Item* w = newItem(STEEL_SWORD, SERVICABLE, 0, 1, 0, true, &stats[player]->inventory);
				equipItem(w, &stats[player]->weapon, player, false);
				printlog("[apx-net] client equip sword ok=%d clientnum=%d", stats[player]->weapon ? 1 : 0, clientnum);
			}
			if ( netRole() == 1 && t == 100 && players[1] && players[1]->entity )
			{
				Entity* c = players[1]->entity;
				const real_t a[5] = { -0.7, -0.35, 0.0, 0.35, 0.7 };
				for ( real_t x : a )
				{
					Entity* d = summonMonster(DUMMYBOT, c->x + cos(c->yaw + x) * STRIKERANGE * 0.6, c->y + sin(c->yaw + x) * STRIKERANGE * 0.6, true);
					if ( d && d->getStats() ) { d->getStats()->HP = d->getStats()->MAXHP = 9999; }
				}
				printlog("[apx-net] host spawned cluster around client yaw=%.3f pos=%.1f,%.1f", c->yaw, c->x, c->y);
			}
			if ( t == 3200 )
			{
				const Stamina_t& r = get(netRole() == 1 ? 1 : 0);
				printlog("[apx-net] summary role=%d sent=%d received=%d remote(p=%d) value=%.1f max=%.1f endurance=%d move=%d",
					netRole(), packetsSent, packetsReceived, netRole() == 1 ? 1 : 0, r.value, r.maxValue, r.endurance, r.move);
				printlog("[apx-net] selftest_done_net");
			}
			return;
		}
		if ( player != 0 ) return;
		Uint32& t = selftestClock();
		++t;
		if ( selftestEndurance() )
		{
			if ( t == 50 ) { selftestSpawn(0); }
			if ( t == 4800 )
			{
				const Stamina_t& s = get(0);
				printlog("[apx-stamina] selftest_done_endurance level=%d combat_spent=%.2f max=%.1f value=%.2f swings=%d clamped=%d",
					s.endurance, s.combatSpent, s.maxValue, s.value, s.swings, s.clamped);
			}
			return;
		}
		if ( t == 5 && selftestMoves() )
		{
			Item* w = newItem(STEEL_SWORD, SERVICABLE, 0, 1, 0, true, &stats[0]->inventory);
			equipItem(w, &stats[0]->weapon, 0, false);
			printlog("[apx-move] selftest_equip sword ok=%d", stats[0]->weapon ? 1 : 0);
		}
		if ( selftestMoves() )
		{
			Uint32 sc = 0, en = 0;
			moveScript(&sc, &en);
			if ( t == 100 ) { selftestSpawn(0); }
			if ( t == sc )
			{
				if ( farDummy() && farDummy()->getStats() ) { farDummy()->getStats()->HP = 0; }
				selftestSpawn(1);
			}
			for ( auto& w : moveScript() ) { if ( w.move == SWORD_LUNGE && (t == w.start - 2 || t == w.start + 100) ) { printlog("[apx-move] selftest_pos t=%u pos=%.2f,%.2f", t, players[0]->entity->x, players[0]->entity->y); } }
			if ( t == en ) { printlog("[apx-move] selftest_done"); }
			return;
		}
		if ( t == 1400 ) { shotRequest() = true; }
		if ( t == 1800 )
		{
			const Stamina_t& s = get(0);
			printlog("[apx-stamina] selftest_done swings=%d swingsWhileBlocking=%d spentSwing=%.2f spentBlock=%.3f clamped=%d value=%.2f",
				s.swings, s.swingsWhileBlocking, s.spentSwing, s.spentBlock, s.clamped, s.value);
		}
	}

	// ---- per-tick local update, called from actHudWeapon --------------------------------------------------
	inline void tick(int player, int chop, bool melee, bool defending)
	{
		if ( !staminaEnabled() ) { return; }
		selftestAdvance(player);
		Stamina_t& s = get(player);
		const real_t perTick = 1.0 / TICKS_PER_SECOND;
		updateCombat(player);
		s.maxValue = maxStaminaFor(s);

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
			if ( players[player] && players[player]->entity )
			{
				s.startYaw = players[player]->entity->yaw;
				s.startPitch = players[player]->entity->pitch;
			}
			printlog("[apx-stamina] swing p=%d blocking=%d cost=%.2f spent=%.2f before=%.2f after=%.2f tick=%u",
				player, underBlock ? 1 : 0, cost, actual, before, s.value, ticks);
		}
		if ( !chopping )
		{
			s.swingWhileBlock = false;
			s.lastDefending = defending;
			s.move = MOVE_NONE;
		}
		s.prevChop = chopping;

		// block drain
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
				player, held, s.blockDrained, held ? s.blockDrained * TICKS_PER_SECOND / held : 0.0, s.blockStartValue, s.value);
		}

		// regen, paused after any spend; scales with Endurance and DEX (stands in for Grace)
		if ( s.value < s.maxValue && ticks - s.lastSpendTick >= kRegenPauseTicks )
		{
			const real_t dex = stats[player] ? std::max(0, stats[player]->DEX) : 0;
			const real_t rate = kRegenPerSecond * (enduranceEnabled() ? 1.0 + kRegenPerEndurance * s.endurance + kRegenPerDex * dex : 1.0);
			s.value = std::min(s.maxValue, s.value + rate * perTick);
		}

		// net: every 10 ticks
		if ( multiplayer != SINGLE && (ticks - s.lastNetTick >= 10) )
		{
			sendState(player);
		}
	}

	// Called at the melee release (generic branch of actHudWeapon): picks the sword move, charges the lunge.
	inline void onMeleeRelease(int player, int charge)
	{
		if ( !movesEnabled() ) { return; }
		Stamina_t& s = get(player);
		Stat* st = stats[player];
		Entity* me = players[player] ? players[player]->entity : nullptr;
		if ( !isSword(st) || !me ) { s.move = MOVE_NONE; return; }
		const int maxCharge = Stat::getMaxAttackCharge(st);
		const bool charged = charge2Enabled() && charge >= maxCharge;
		real_t dyaw = me->yaw - s.startYaw;
		while ( dyaw > PI ) { dyaw -= 2 * PI; }
		while ( dyaw < -PI ) { dyaw += 2 * PI; }
		const real_t dpitch = me->pitch - s.startPitch;
		int move = SWORD_THRUST; // default when the mouse did not move: sword thrusts
		if ( std::fabs(dyaw) > kYawThreshold && std::fabs(dyaw) >= std::fabs(dpitch) ) { move = SWORD_SLASH; }
		else if ( std::fabs(dpitch) > kPitchThreshold ) { move = SWORD_VERTICAL; }
		if ( charged ) { move = SWORD_LUNGE; }
		if ( selftestMoves() ) { move = forcedMove(); }
		s.move = move;
		s.sweepExtras = 0;
		real_t extra = 0.0;
		if ( charged ) { extra += kSwingCost * (kChargedCostMult - 1.0) * (s.swingWhileBlock ? kSwingWhileBlockCostMult : 1.0); }
		if ( move == SWORD_VERTICAL ) { extra += kSwingCost * (kVerticalCost - 1.0); }
		if ( extra > 0.0 ) { spend(player, extra, 0); }
		if ( move == SWORD_LUNGE )
		{
			// short dash along the facing; the player movement code's clipMove stops it at walls
			me->vel_x += cos(me->yaw) * kLungeImpulse;
			me->vel_y += sin(me->yaw) * kLungeImpulse;
		}
		printlog("[apx-move] release p=%d move=%d charged=%d charge=%d max=%d dyaw=%.3f dpitch=%.3f extra_cost=%.2f value=%.2f pos=%.2f,%.2f tick=%u",
			player, move, charged ? 1 : 0, charge, maxCharge, dyaw, dpitch, extra, s.value, me->x, me->y, ticks);
	}

	// Reset on a new character (called from initClass).
	inline void reset(int player)
	{
		stamina[std::max(0, std::min(player, MAXPLAYERS - 1))] = Stamina_t();
	}
}
