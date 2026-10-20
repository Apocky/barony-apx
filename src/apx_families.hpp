// apx_families.hpp -- F4d engine glue for the weapon-family moves (apx_moves.hpp): damage and armour shaping and the
// after-hit effects (push, knock-down, slam, shield aside/break, critical + bleed, throw, grapple), plus the
// APX_SELFTEST_FAMILIES scripted run. Included at the end of apx_stamina.hpp.
//
// Hooks in Entity::attack (entity.cpp): beforeHit just before the damage is applied, afterHit right after modHP,
// adjustAC where the enemy AC is read. Everything is data-driven from the move table and off with APX_MOVES=0.
#pragma once

#include "apx_stamina.hpp"
#include "apx/move/move.hpp"
#include <vector>

namespace apx
{
namespace fam
{
	using namespace apx::moves;

	// ---- counters (self-test summary) ----
	struct Counters { int hits = 0, pushes = 0, knockdowns = 0, slamVictims = 0, shieldAside = 0, shieldBreak = 0, crits = 0, bleeds = 0, throws = 0, grapples = 0, sneaks = 0; };
	inline Counters& counters() { static Counters c; return c; }

	// a creature small enough to be knocked down by a slam, a sweep or a throw
	inline bool isSmall(Entity* e) { return e && e->sizex <= 5 && e->sizey <= 5; }

	// "unaware" as the engine's backstab test reads it: the creature is not in the attack state when the blow lands.
	// beforeHit captures it before the hit alerts the creature; afterHit reuses the captured value.
	inline bool& lastUnaware() { static bool b = false; return b; }
	inline bool unaware(Entity* attacker, Entity* victim)
	{
		(void)attacker;
		return victim && victim->behavior == &actMonster && victim->monsterState != MONSTER_STATE_ATTACK;
	}

	// stagger / knock-down: the engine's paralysis for the given ticks (same device as F4c's stagger)
	inline void knockdown(Entity* target, int ticks)
	{
		if ( !target || ticks <= 0 ) { return; }
		Stat* ts = target->getStats();
		target->setEffect(EFF_PARALYZED, true, ticks, true, true);
		if ( ts && !ts->getEffectActive(EFF_PARALYZED) )
		{
			ts->setEffectActive(EFF_PARALYZED, 1);
			ts->EFFECTS_TIMERS[EFF_PARALYZED] = ticks;
		}
	}

	inline void push(Entity* attacker, Entity* victim, double force)
	{
		const double t = atan2(victim->y - attacker->y, victim->x - attacker->x);
		// apx D93 C2: a push is an apx::move impulse (force cap 7 in the move table); the force is the old velocity add x2 over 8 ticks
		apx::move::impulse(victim, (float)(cos(t) * force * 2.0), (float)(sin(t) * force * 2.0), 8);
		++counters().pushes;
	}

	// ---- self-test bookkeeping ----
	struct Pending { Uint32 uid; real_t x, y; Uint32 tick; int move; real_t ax, ay; bool checked3 = false, checked5 = false, checked25 = false; };
	inline std::vector<Pending>& pending() { static std::vector<Pending> v; return v; }
	inline std::vector<Uint32>& dummies() { static std::vector<Uint32> v; return v; }

	// AC the strike meets: the move's armour ignore, and a raised shield's active bonus ignored for aside/break moves
	inline int adjustAC(int player, Stat* vstats, int ac)
	{
		const MoveDef* d = apx::moveDefFor(player);
		if ( !d || !vstats ) { return ac; }
		if ( (d->effects & (EF_SHIELD_ASIDE | EF_SHIELD_BREAK)) && vstats->shield && vstats->defending )
		{
			vstats->defending = false;
			ac = AC(vstats); // the block's bonus does not apply to a hook or a cleave
			vstats->defending = true;
		}
		if ( d->armourIgnore > 0.0 ) { ac = static_cast<int>(ac * (1.0 - d->armourIgnore)); }
		return ac;
	}

	// critical and sneak damage, before the damage is applied
	inline int beforeHit(int player, Entity* attacker, Entity* victim, Stat* vstats, int damage, int previousState)
	{
		const MoveDef* d = apx::moveDefFor(player);
		if ( !d || !vstats || !victim ) { return damage; }
		int out = damage;
		// the engine alerts the target when the blow is found (before this point), so the state it had BEFORE the alert is what counts
		lastUnaware() = victim->behavior == &actMonster && previousState != MONSTER_STATE_ATTACK;
		const bool stag = vstats->getEffectActive(EFF_PARALYZED) != 0;
		if ( d->effects & EF_CRIT_BLEED )
		{
			if ( lastUnaware() || stag ) { out = static_cast<int>(damage * d->sneakMult); ++counters().crits; }
		}
		else if ( d->effects & EF_SNEAK )
		{
			if ( lastUnaware() ) { out = static_cast<int>(damage * d->sneakMult); ++counters().sneaks; }
		}
		return out;
	}

	// effects after the damage landed (called once per struck target, main and extras)
	inline void afterHit(int player, Entity* attacker, Entity* victim, Stat* vstats, int damage, int pose)
	{
		const MoveDef* d = apx::moveDefFor(player);
		if ( !d || !vstats || !victim ) { return; }
		Counters& c = counters();
		++c.hits;
		const bool wasBlocking = vstats->shield && vstats->defending;
		const bool stagBefore = vstats->getEffectActive(EFF_PARALYZED) != 0;
		const bool wasUnaware = lastUnaware();
		if ( d->effects & EF_PUSH ) { push(attacker, victim, d->force); }
		if ( (d->effects & EF_THROW) && isSmall(victim) ) { knockdown(victim, d->knockdownTicks); ++c.throws; }
		else if ( d->effects & EF_THROW ) { /* too big to throw: pushed only */ }
		if ( (d->effects & EF_KNOCKDOWN_SMALL) && isSmall(victim) ) { knockdown(victim, d->knockdownTicks); ++c.knockdowns; }
		if ( d->effects & EF_GRAPPLE ) { knockdown(victim, d->knockdownTicks); ++c.grapples; }
		if ( (d->effects & EF_SHIELD_ASIDE) && wasBlocking ) { vstats->defending = false; ++c.shieldAside; }
		if ( (d->effects & EF_SHIELD_BREAK) && wasBlocking )
		{
			vstats->defending = false;
			victim->degradeArmor(*vstats, *vstats->shield, 4); // armour slot 4 = shield (entity.cpp:13787 idiom)
			++c.shieldBreak;
		}
		if ( (d->effects & EF_CRIT_BLEED) && (wasUnaware || stagBefore) ) { victim->setEffect(EFF_BLEEDING, true, 150, true); ++c.bleeds; }
		if ( d->effects & EF_SLAM )
		{
			// radial stagger and push around the impact; small creatures are knocked down
			for ( node_t* node = map.creatures ? map.creatures->first : nullptr; node; node = node->next )
			{
				Entity* e = (Entity*)node->element;
				if ( !e || e == victim || e == attacker || e->behavior != &actMonster ) { continue; }
				const double dx = e->x - victim->x, dy = e->y - victim->y;
				if ( dx * dx + dy * dy > d->radius * d->radius || e->monsterAllyGetPlayerLeader() ) { continue; }
				const double t = atan2(dy, dx);
				e->vel_x += cos(t) * d->force;
				e->vel_y += sin(t) * d->force;
				if ( isSmall(e) ) { knockdown(e, d->knockdownTicks); ++c.knockdowns; }
				++c.slamVictims;
				if ( selftestOn() ) { printlog("[apx-fam] slam victim uid=%u dist=%.1f small=%d", e->getUID(), sqrt(dx * dx + dy * dy), isSmall(e) ? 1 : 0); }
			}
		}
		if ( selftestOn() )
		{
			printlog("[apx-fam] hit move=%d name=%s uid=%u dmg=%d extra=%d unaware=%d blocking=%d staggered_before=%d size=%d,%d",
				d->id, d->name, victim->getUID(), damage, pose == MONSTER_POSE_SWEEP_ATTACK_NO_UPDATE ? 1 : 0,
				wasUnaware ? 1 : 0, wasBlocking ? 1 : 0, stagBefore ? 1 : 0, victim->sizex, victim->sizey);
			pending().push_back({ victim->getUID(), victim->x, victim->y, ticks, d->id, attacker->x, attacker->y });
		}
	}

	// ---- scripted run (APX_SELFTEST_FAMILIES) ----
	inline void equipFor(moves::Family f)
	{
		Stat* st = stats[0];
		ItemType wt = (ItemType)-1;
		switch ( f )
		{
			case F_SWORD: wt = STEEL_SWORD; break;
			case F_AXE: wt = IRON_AXE; break;
			case F_MACE: wt = IRON_MACE; break;
			case F_SPEAR: wt = IRON_SPEAR; break;
			case F_STAFF: wt = QUARTERSTAFF; break;
			case F_DAGGER: wt = RAPIER; break;
			default: break;
		}
		st->weapon = nullptr;
		st->shield = nullptr;
		if ( wt != (ItemType)-1 )
		{
			Item* w = newItem(wt, SERVICABLE, 0, 1, 0, true, &st->inventory);
			equipItem(w, &st->weapon, 0, false);
		}
		if ( f == F_SHIELD )
		{
			Item* sh = newItem(WOODEN_SHIELD, SERVICABLE, 0, 1, 0, true, &st->inventory);
			equipItem(sh, &st->shield, 0, false);
		}
		printlog("[apx-fam] equip family=%d weapon=%d shield=%d resolved_family=%d", (int)f, st->weapon ? (int)st->weapon->type : -1,
			st->shield ? (int)st->shield->type : -1, (int)apx::familyOf(st));
	}

	inline bool victimRat() { static const bool on = std::getenv("APX_SELFTEST_VICTIM") && std::getenv("APX_SELFTEST_VICTIM")[0] == 'r'; return on; }
	inline void clearDummies()
	{
		for ( Uint32 uid : dummies() )
		{
			Entity* e = uidToEntity(uid);
			if ( e && e->getStats() ) { e->getStats()->HP = 0; }
		}
		dummies().clear();
	}

	inline void spawnDummies()
	{
		Entity* me = players[0] ? players[0]->entity : nullptr;
		if ( !me ) { return; }
		struct P { real_t ang, dist; };
		// five in an arc at 0.6 reach, two in the line behind (for lines and run-throughs)
		const P pos[7] = { { -0.7, 14.4 }, { -0.35, 14.4 }, { 0.0, 14.4 }, { 0.35, 14.4 }, { 0.7, 14.4 }, { 0.0, 22.0 }, { 0.0, 29.0 } };
		for ( const P& p : pos )
		{
			Entity* d = summonMonster(victimRat() ? RAT : DUMMYBOT, me->x + cos(me->yaw + p.ang) * p.dist, me->y + sin(me->yaw + p.ang) * p.dist, true);
			if ( !d || !d->getStats() ) { continue; }
			d->getStats()->HP = d->getStats()->MAXHP = 9999;
			Item* sh = newItem(WOODEN_SHIELD, SERVICABLE, 0, 1, 0, true, nullptr); // every dummy holds a shield (aside / break moves)
			d->getStats()->shield = sh;
			dummies().push_back(d->getUID());
		}
		printlog("[apx-fam] spawned %d dummies", (int)dummies().size());
	}

	inline void setAwareness(bool aware)
	{
		Entity* me = players[0] ? players[0]->entity : nullptr;
		if ( !me ) { return; }
		for ( Uint32 uid : dummies() )
		{
			Entity* e = uidToEntity(uid);
			if ( !e || !e->getStats() ) { continue; }
			e->monsterTarget = aware ? me->getUID() : 0;
			e->monsterState = aware ? MONSTER_STATE_ATTACK : MONSTER_STATE_WAIT;
			e->getStats()->defending = true; // every victim blocks
		}
	}

	inline void samplePending()
	{
		auto& v = pending();
		for ( Pending& p : v )
		{
			Entity* e = uidToEntity(p.uid);
			if ( !e ) { p.checked5 = p.checked25 = true; continue; }
			if ( !p.checked3 && ticks - p.tick >= 3 )
			{
				p.checked3 = true;
				printlog("[apx-fam] vel3 uid=%u move=%d v=%.2f", p.uid, p.move, sqrt(e->vel_x * e->vel_x + e->vel_y * e->vel_y));
			}
			if ( !p.checked5 && ticks - p.tick >= 5 )
			{
				p.checked5 = true;
				Stat* es = e->getStats();
				printlog("[apx-fam] after5 uid=%u move=%d stagger=%d bleed=%d", p.uid, p.move, es && es->getEffectActive(EFF_PARALYZED) ? 1 : 0, es && es->getEffectActive(EFF_BLEEDING) ? 1 : 0);
			}
			if ( !p.checked25 && ticks - p.tick >= 25 )
			{
				p.checked25 = true;
				const real_t ux = p.x - p.ax, uy = p.y - p.ay, ul = sqrt(ux * ux + uy * uy) + 1e-9;
				const real_t proj = ((e->x - p.x) * ux + (e->y - p.y) * uy) / ul; // along the player -> victim axis: > 0 = pushed away
				printlog("[apx-fam] after25 uid=%u move=%d disp=%.2f proj=%.2f", p.uid, p.move, sqrt((e->x - p.x) * (e->x - p.x) + (e->y - p.y) * (e->y - p.y)), proj);
			}
		}
		v.erase(std::remove_if(v.begin(), v.end(), [](const Pending& p) { return p.checked5 && p.checked25; }), v.end());
	}

	inline void advance(Uint32 t)
	{
		static int lastAware = -1;
		samplePending();
		for ( const FamBlock& b : famBlocks() )
		{
			if ( t == b.start ) { clearDummies(); equipFor(b.fam); }
			if ( t == b.start + 20 ) { spawnDummies(); }
		}
		for ( const Win& w : famScript() )
		{
			if ( t == w.start - 3 ) { setAwareness(w.aware != 0); lastAware = w.aware; }
			if ( t >= w.start && t < w.start + w.len + 30 ) { setAwareness(w.aware != 0); } // through the release, ~10-15 ticks after the press
		}
		if ( t == famEnd() )
		{
			Counters& c = counters();
			printlog("[apx-fam] selftest_done_families hits=%d pushes=%d knockdowns=%d slamVictims=%d shieldAside=%d shieldBreak=%d crits=%d bleeds=%d throws=%d grapples=%d sneaks=%d",
				c.hits, c.pushes, c.knockdowns, c.slamVictims, c.shieldAside, c.shieldBreak, c.crits, c.bleeds, c.throws, c.grapples, c.sneaks);
		}
		(void)lastAware;
	}
}

inline void famSelftestAdvance(Uint32 t) { fam::advance(t); }
}
