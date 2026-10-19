// apx_combat.hpp -- F4c engine glue: poise and stagger, HP zones, co-op downed (spec 04 sections
// 2, 4, 5; D34). The rules are in apx_combat_units.hpp (offline-tested); this file keeps the
// per-entity state and wires it into Barony:
//   Entity::attack     apx::combat_glue::canAttack (arms disabled or downed: no attack) and
//                      onMeleeHit (zone, poise, stagger bonus) before the engine's modHP
//   Entity::modHP      interceptPlayerDamage (the downed state instead of a lethal blow)
//   actPlayer/actMonster  tick (poise regen, stagger end, downed timer and revive, self-test)
//   PlayerMovement_t::getSpeedFactor  speedScale (legs disabled 0.5, downed 0.3)
// Stagger is the engine's EFF_PARALYZED for kStaggerTicks: a staggered entity cannot act.
// State is server-side, keyed by entity uid; it is NOT sent over the network yet (remote clients
// do not see it; local splitscreen does). Knobs: APX_POISE=0, APX_ZONES=0, APX_DOWNED=0 each turn
// a feature off; APX_DOWNED_FORCE_COOP=1 treats a solo game as co-op; APX_F4C_SELFTEST=1 runs the
// scripted integration test on floor start and quits. Telemetry: "[apx-f4c]" lines in log.txt.
#pragma once

#include "main.hpp"
#include "apx/selftest_all.hpp"
#include "game.hpp"
#include "stat.hpp"
#include "entity.hpp"
#include "player.hpp"
#include "monster.hpp"
#include "apx_combat_units.hpp"
#include "apx/tiers.hpp"
#include "apx/poise_rules.hpp"
#include "apx_stamina.hpp"
#include "items.hpp"
#include <cstdlib>
#include <cmath>
#include <unordered_map>

namespace apx
{
namespace combat_glue
{
	using namespace apx::combat;

	inline bool envFlag(const char* name, bool dflt)
	{
		const char* v = std::getenv(name);
		if ( !v || !v[0] ) return dflt;
		return v[0] != '0';
	}
	inline bool poiseOn() { static const bool on = envFlag("APX_POISE", true); return on; }
	inline bool zonesOn() { static const bool on = envFlag("APX_ZONES", true); return on; }
	inline bool downedOn() { static const bool on = envFlag("APX_DOWNED", true); return on; }
	inline bool forceCoop() { static const bool on = envFlag("APX_DOWNED_FORCE_COOP", false); return on || selftestall::active(selftestall::F4C); } // the f4c stage runs solo as co-op, as its script does
	inline bool selftestOn() { static const bool on = envFlag("APX_F4C_SELFTEST", false); return on || selftestall::armed(); } // D77: also the "f4c" stage of -apxselftest=all
	// D75: a player's poise is physical (apx/poise_rules.hpp); 0 = F4c's flat pool (the control)
	inline bool physicalOn() { static const bool on = envFlag("APX_POISE_PHYSICAL", true); return on; }

	struct Rec
	{
		PoiseState poise;
		ZoneState zones;
		int maxhp = 0;
		Uint32 lastTick = 0;
		bool hitThisTick = false;
		int staggers = 0;
		// D75, players: the physical pool, and stagger locks timed the same way for either model
		apx::poise::State phys;
		bool physInit = false;
		double physMaxLogged = -1.0;
		bool wasStaggered = false;
		Uint32 lastStaggerEnd = 0;
		bool staggerEnded = false;
		long long locks = 0, staggeredTicks = 0;
	};
	inline std::unordered_map<Uint32, Rec>& table()
	{
		static std::unordered_map<Uint32, Rec> t;
		return t;
	}
	inline DownedState& downed(int playernum)
	{
		static DownedState d[MAXPLAYERS];
		return d[std::max(0, std::min(MAXPLAYERS - 1, playernum))];
	}
	inline int& selftestAlly() { static int a = 0; return a; } // self-test: a fake ally stands by

	inline bool isPlayer(Entity* e) { return e && e->behavior == &actPlayer; }
	inline Rec& rec(Entity* e, Stat* s)
	{
		Rec& r = table()[e->getUID()];
		if ( r.maxhp != s->MAXHP ) // first sight, or MAXHP changed (level up): pools follow
		{
			bool fresh = r.maxhp == 0;
			r.maxhp = s->MAXHP;
			r.zones.init(s->MAXHP);
			if ( fresh ) r.poise.init(poiseMax(tierFromStats(s->MAXHP, e->isBossMonster(), isPlayer(e))));
		}
		return r;
	}
	inline WeaponClass weaponClassOf(Stat* s)
	{
		if ( !s || !s->weapon ) return WeaponClass::Unarmed;
		switch ( getWeaponSkill(s->weapon) )
		{
			case PRO_SWORD: return WeaponClass::Sword;
			case PRO_MACE: return WeaponClass::Mace;
			case PRO_AXE: return WeaponClass::Axe;
			case PRO_POLEARM: return WeaponClass::Polearm;
			case PRO_RANGED: return WeaponClass::Ranged;
			case PRO_UNARMED: return WeaponClass::Unarmed;
			default: return WeaponClass::Dagger;
		}
	}
	inline int coopPlayers()
	{
		int n = 0;
		for ( int i = 0; i < MAXPLAYERS; ++i ) n += players[i] && players[i]->entity != nullptr;
		return n;
	}

	// ------------------------------------------------------------------ D75 physical poise (players)
	// An armour item's D66 material (apx/combat/impact_materials.inc order). Unlisted armour is soft cloth.
	inline int armourMaterial(int type)
	{
		using namespace apx::poise;
		switch ( type )
		{
			case IRON_HELM: case IRON_BREASTPIECE: case IRON_BOOTS: case IRON_BOOTS_WATERWALKING: case IRON_SHIELD: case IRON_KNUCKLES:
			case STEEL_HELM: case STEEL_BREASTPIECE: case STEEL_BOOTS: case STEEL_BOOTS_FEATHER: case STEEL_BOOTS_LEVITATION:
			case STEEL_SHIELD: case STEEL_SHIELD_RESISTANCE: case BRONZE_SHIELD: case MIRROR_SHIELD:
			case BLACKIRON_HELM: case BLACKIRON_BREASTPIECE: case BLACKIRON_BOOTS: case BLACKIRON_GAUNTLETS: case BLACKIRON_SHIELD:
			case SILVER_HELM: case SILVER_BREASTPIECE: case SILVER_BOOTS: case SILVER_GAUNTLETS: case SILVER_SHIELD:
			case ARTIFACT_HELM: case ARTIFACT_BREASTPIECE: case ARTIFACT_BOOTS: case ARTIFACT_GLOVES:
			case GAUNTLETS: case GAUNTLETS_STRENGTH: case SPIKED_GAUNTLETS: case BRASS_KNUCKLES: case CHAIN_BOOTS: case CHAIN_GLOVES:
			case HELM_MINING: case HAT_CROWNED_HELM: case MASK_STEEL_VISOR: case MASK_ARTIFACT_VISOR: case MASK_GOLDEN:
				return M_METAL;
			case CRYSTAL_HELM: case CRYSTAL_BREASTPIECE: case CRYSTAL_BOOTS: case CRYSTAL_GLOVES: case CRYSTAL_SHIELD: case MASK_CRYSTAL_VISOR:
				return M_STONE;
			case BONE_HELM: case BONE_BREASTPIECE: case BONE_BOOTS: case BONE_BRACERS: case BONE_SHIELD:
				return M_BONE;
			case LEATHER_HELM: case LEATHER_BREASTPIECE: case LEATHER_BOOTS: case LEATHER_BOOTS_SPEED: case BRACERS: case BRACERS_CONSTITUTION:
			case GLOVES: case GLOVES_DEXTERITY: case SUEDE_BOOTS: case SUEDE_GLOVES: case CLEAT_BOOTS: case BANDIT_BREASTPIECE:
			case MACHINIST_APRON: case HAT_BEAR_HOOD: case HAT_WOLF_HOOD: case HAT_STAG_HOOD:
				return M_HIDE;
			case WOODEN_SHIELD:
				return M_PLANT;
			default:
				return M_UNIFORM;
		}
	}
	struct PoiseInputs { double armour = 0.0; int con = 0, endurance = 0, pieces = 0; };
	inline PoiseInputs playerPoiseInputs(Entity* e, Stat* s)
	{
		PoiseInputs in;
		apx::poise::Piece p[8];
		Item* slots[7] = { s->helmet, s->breastplate, s->gloves, s->shoes, s->cloak, s->mask, s->shield };
		for ( Item* it : slots )
		{
			if ( !it || it->type < 0 || it->type >= NUMITEMS ) continue;
			if ( it == s->shield && itemCategory(it) != ARMOR ) continue; // a torch, lantern or tome in the off hand is no armour
			p[in.pieces++] = { items[it->type].weight, armourMaterial((int)it->type) };
		}
		in.armour = apx::poise::armourPoise(p, in.pieces);
		in.con = s->CON;
		const int pn = e->skill[2];
		in.endurance = (apx::enduranceEnabled() && pn >= 0 && pn < MAXPLAYERS) ? apx::stamina[pn].endurance : 0;
		return in;
	}
	inline double playerMaxPoise(Entity* e, Stat* s)
	{
		const PoiseInputs in = playerPoiseInputs(e, s);
		return apx::poise::maxPoise(in.con, in.armour, in.endurance);
	}
	// blocking or bracing (F4a stamina left) times the player's own heavy swing in flight
	inline double playerStanceMult(Entity* e, Stat* s)
	{
		const int pn = e->skill[2];
		const bool local = pn >= 0 && pn < MAXPLAYERS && apx::staminaEnabled();
		const bool staminaLeft = !local || apx::stamina[pn].value > 0.5;
		const bool shield = s->shield && itemCategory(s->shield) == ARMOR;
		const bool swinging = local && apx::stamina[pn].prevChop; // F4a: a melee swing in flight (local players)
		return apx::poise::guardMult(s->defending, shield, staminaLeft) * apx::poise::swingMult(swinging, weaponClassOf(s));
	}
	inline bool physicalPlayer(Entity* e) { return physicalOn() && isPlayer(e); }
	inline void ensurePhys(Entity* e, Stat* s, Rec& r)
	{
		if ( r.physInit ) return;
		r.physInit = true;
		r.phys.init(playerMaxPoise(e, s));
	}
	inline bool staggeredRec(Entity* e, Rec& r) { return physicalPlayer(e) ? r.phys.staggered() : r.poise.staggered(); }

	// The zone a strike lands on: a player's by camera pitch, a creature's on a player by its size,
	// creature on creature by medium-size weights.
	inline Zone strikeZone(Entity* attacker, Entity* target)
	{
		if ( isPlayer(attacker) ) return zoneFromPitch(attacker->pitch, local_rng.rand() % 4 == 0);
		int size = 1;
		if ( attacker ) size = attacker->sizex <= 3 ? 0 : attacker->sizex >= 6 ? 2 : 1;
		(void)target;
		return rollZoneForSize(size, (int)(local_rng.rand() % 100));
	}

	// Stagger: the engine's paralysis for kStaggerTicks (cannot act); counted.
	inline void startStagger(Entity* target, Rec& r)
	{
		target->setEffect(EFF_PARALYZED, true, kStaggerTicks, true, true);
		Stat* ts = target->getStats();
		if ( ts && !ts->getEffectActive(EFF_PARALYZED) )
		{
			// the engine refuses the paralysis SPELL to some creatures (bosses): a stagger is not that
			// spell, so the engine's paralysed state is set directly for the stagger's ticks
			ts->setEffectActive(EFF_PARALYZED, 1);
			ts->EFFECTS_TIMERS[EFF_PARALYZED] = kStaggerTicks;
			printlog("[apx-f4c] stagger forced past paralysis immunity uid=%u type=%d", target->getUID(), (int)ts->type);
		}
		++r.staggers;
	}

	// The melee hit hook. damage is the engine's damage for this hit; returns the damage to apply.
	inline int applyHit(Entity* attacker, Stat* atkStats, Entity* target, Stat* tStats, int damage, Zone z, WeaponClass w)
	{
		if ( !target || !tStats ) return damage;
		Rec& r = rec(target, tStats);
		r.hitThisTick = true;
		if ( physicalPlayer(target) ) ensurePhys(target, tStats, r);
		bool wasStaggered = poiseOn() && staggeredRec(target, r);
		int out = damage;
		if ( zonesOn() ) out = hitDamage(damage, z, wasStaggered);
		else if ( wasStaggered ) out = hitDamage(damage, Zone::Torso, true);
		if ( zonesOn() && out > 0 ) r.zones.damage(z, out);
		// apx D74: a hostile creature's poise hit on a player or follower by its tier (fodder a third; apx/tiers.hpp)
		const int poiseIn = apx::tierdmg::poiseForHit(attacker, target, poiseHit(w));
		if ( poiseOn() && physicalPlayer(target) )
		{
			// apx D75: physical poise; the stance and a heavy swing in flight divide the blow's poise damage
			if ( r.phys.hit(poiseIn / playerStanceMult(target, tStats), kStaggerTicks) ) startStagger(target, r);
		}
		else if ( poiseOn() && r.poise.hit(poiseIn) ) startStagger(target, r);
		const bool phys = physicalPlayer(target);
		printlog("[apx-f4c] hit uid=%u type=%d zone=%s base=%d dmg=%d poise=%.0f/%.0f%s staggered=%d legs=%d/%d arms=%d/%d", target->getUID(),
			(int)tStats->type, zoneName(z), damage, out, phys ? r.phys.value : r.poise.value, phys ? r.phys.max : (double)r.poise.max,
			phys ? " physical" : "", (int)staggeredRec(target, r),
			r.zones.hp[(int)Zone::Legs], r.zones.max[(int)Zone::Legs], r.zones.hp[(int)Zone::Arms], r.zones.max[(int)Zone::Arms]);
		(void)atkStats;
		return out;
	}
	inline int onMeleeHit(Entity* attacker, Stat* atkStats, Entity* target, Stat* tStats, int damage)
	{
		if ( !poiseOn() && !zonesOn() ) return damage;
		return applyHit(attacker, atkStats, target, tStats, damage, strikeZone(attacker, target), weaponClassOf(atkStats));
	}

	// in a poise stagger now (the engine's boss un-paralysis skips these)
	inline bool staggered(Entity* e)
	{
		if ( !e || !poiseOn() ) return false;
		auto it = table().find(e->getUID());
		return it != table().end() && staggeredRec(e, it->second);
	}

	// arms disabled or downed: no attack (a staggered entity is already paralysed by the engine)
	inline bool canAttack(Entity* e)
	{
		if ( !e ) return true;
		Stat* s = e->getStats();
		if ( !s ) return true;
		if ( isPlayer(e) && downedOn() && downed(e->skill[2]).downed ) return false;
		if ( !zonesOn() ) return true;
		auto it = table().find(e->getUID());
		return it == table().end() || it->second.zones.canAttack();
	}

	inline real_t speedScale(int playernum)
	{
		real_t k = 1.0;
		if ( downedOn() && downed(playernum).downed ) k = std::min<real_t>(k, kDownedSpeed);
		if ( zonesOn() && players[playernum] && players[playernum]->entity )
		{
			auto it = table().find(players[playernum]->entity->getUID());
			if ( it != table().end() && !it->second.zones.canRun() ) k = std::min<real_t>(k, kLegsDisabledSpeed);
		}
		return k;
	}

	// a player's HP change: a lethal blow downs instead (co-op, or solo once per floor)
	inline int interceptPlayerDamage(Entity* e, Stat* s, int amount)
	{
		if ( !downedOn() || amount >= 0 || !isPlayer(e) || !s ) return amount;
		DownedState& d = downed(e->skill[2]);
		if ( d.downed ) return 0; // no HP loss while down
		if ( s->HP + amount > 0 ) return amount;
		if ( !d.onLethal(coopPlayers() > 1 || forceCoop()) ) return amount;
		printlog("[apx-f4c] downed player=%d hp=%d blow=%d coop=%d", e->skill[2], s->HP, amount, coopPlayers());
		return 1 - s->HP; // hold at 1 HP
	}

	inline void selftestStep(Entity* my, Stat* s);

	// once per tick per entity, from actPlayer and actMonster
	inline void tick(Entity* e)
	{
		if ( !e ) return;
		Stat* s = e->getStats();
		if ( !s ) return;
		Rec& r = rec(e, s);
		if ( r.lastTick != ::ticks )
		{
			r.lastTick = ::ticks;
			r.poise.tick();
			if ( physicalPlayer(e) )
			{
				// D75: the pool follows gear, CON and Endurance; it recovers slowly while the player attacks or moves
				ensurePhys(e, s, r);
				const double m = playerMaxPoise(e, s);
				r.phys.setMax(m);
				if ( std::fabs(m - r.physMaxLogged) >= 1.0 )
				{
					const PoiseInputs in = playerPoiseInputs(e, s);
					printlog("[apx-poise] player %d max %.1f (body %.1f: CON %d; armour %.1f from %d pieces; Endurance %d) value %.1f",
						e->skill[2], m, apx::poise::table().base + apx::poise::table().perCON * in.con, in.con, in.armour, in.pieces,
						in.endurance, r.phys.value);
					r.physMaxLogged = m;
				}
				const int pn = e->skill[2];
				const bool swinging = pn >= 0 && pn < MAXPLAYERS && apx::staminaEnabled() && apx::stamina[pn].prevChop;
				const bool moving = std::hypot(e->vel_x, e->vel_y) > 0.05;
				r.phys.tick(swinging || moving);
			}
			if ( isPlayer(e) && poiseOn() )
			{
				// stagger locks, timed the same way for either model: a stagger starting within lockGapS of the last end
				const bool now = staggeredRec(e, r);
				if ( now ) ++r.staggeredTicks;
				if ( now && !r.wasStaggered && r.staggerEnded
					&& ::ticks - r.lastStaggerEnd < (Uint32)(apx::poise::table().lockGapS * kTicksPerSecond) ) ++r.locks;
				if ( !now && r.wasStaggered ) { r.lastStaggerEnd = ::ticks; r.staggerEnded = true; }
				r.wasStaggered = now;
			}
		}
		if ( isPlayer(e) )
		{
			int pn = e->skill[2];
			static int lastLevel[MAXPLAYERS];
			static bool lastInit = false;
			if ( !lastInit ) { for ( int& l : lastLevel ) l = -1; lastInit = true; }
			if ( lastLevel[pn] != currentlevel ) { lastLevel[pn] = currentlevel; downed(pn).newFloor(); }
			DownedState& d = downed(pn);
			if ( downedOn() && d.downed )
			{
				bool ally = selftestAlly() > 0;
				for ( int i = 0; i < MAXPLAYERS && !ally; ++i )
				{
					if ( i == pn || !players[i] || !players[i]->entity ) continue;
					Entity* o = players[i]->entity;
					ally = std::hypot(o->x - e->x, o->y - e->y) <= kReviveRange * 16.0 && !downed(i).downed;
				}
				int res = d.tick(ally, r.hitThisTick);
				if ( res > 0 )
				{
					e->setHP(d.reviveHP(s->MAXHP));
					printlog("[apx-f4c] revived player=%d hp=%d", pn, s->HP);
				}
				else if ( res < 0 )
				{
					printlog("[apx-f4c] bled out player=%d", pn);
					e->setHP(0);
				}
			}
			if ( selftestOn() && pn == 0 ) selftestStep(e, s);
		}
		r.hitThisTick = false;
	}

	// ------------------------------------------------------------------ the integration self-test
	// Spawns a fodder, regular, elite and champion beside player 0, drives applyHit (the same
	// function the melee path uses) and the real modHP, checks the engine reacts, prints one
	// "[apx-f4c] check" row per check and a summary, then quits.
	struct Selftest
	{
		int phase = 0;
		Uint32 t0 = 0, mark = 0;
		Entity* mon[4] = { nullptr, nullptr, nullptr, nullptr };
		int pass = 0, fail = 0;
		int downedAt = -1;
		double bx = 0, by = 0;
		int bossHeld = 0;
		void check(bool ok, const char* what, double got, double want)
		{
			(ok ? pass : fail)++;
			printlog("[apx-f4c] check %s %s got=%.3f want=%.3f", ok ? "PASS" : "FAIL", what, got, want);
		}
	};
	inline Selftest& st() { static Selftest t; return t; }

	inline void selftestStep(Entity* my, Stat* s)
	{
		if ( selftestall::armed() && !selftestall::active(selftestall::F4C) ) { return; } // D77: wait for the "f4c" stage
		Selftest& T = st();
		if ( T.phase == 0 ) { T.t0 = ::ticks; T.phase = 1; return; }
		Uint32 dt = ::ticks - T.t0;
		if ( T.phase == 1 && dt >= 100 ) // spawn: rat (fodder), goblin (regular), troll (elite), minotaur (champion)
		{
			const Monster kinds[4] = { RAT, GOBLIN, TROLL, MINOTAUR };
			for ( int k = 0; k < 4; ++k )
			{
				T.mon[k] = summonMonster(kinds[k], (long)my->x + 32 + 24 * k, (long)my->y, true);
				Stat* ms = T.mon[k] ? T.mon[k]->getStats() : nullptr;
				printlog("[apx-f4c] spawned k=%d type=%d maxhp=%d tier=%s", k, (int)kinds[k], ms ? ms->MAXHP : -1,
					ms ? tierName(tierFromStats(ms->MAXHP, T.mon[k]->isBossMonster(), false)) : "none");
			}
			T.phase = 2;
			return;
		}
		if ( T.phase == 2 ) // poise: sword hits of 1 damage until each staggers
		{
			for ( int k = 0; k < 4; ++k )
			{
				Entity* m = T.mon[k];
				Stat* ms = m ? m->getStats() : nullptr;
				if ( !ms ) { T.check(false, "monster spawned", 0, 1); continue; }
				Tier tier = tierFromStats(ms->MAXHP, m->isBossMonster(), false);
				int hits = 0;
				while ( hits < 60 && !ms->getEffectActive(EFF_PARALYZED) )
				{
					int d = applyHit(my, s, m, ms, 1, Zone::Torso, WeaponClass::Sword);
					m->modHP(-d);
					++hits;
				}
				char what[96];
				snprintf(what, sizeof what, "hits_to_stagger_%s", tierName(tier));
				int want = (poiseMax(tier) + poiseHit(WeaponClass::Sword) - 1) / poiseHit(WeaponClass::Sword);
				T.check(hits == want && ms->getEffectActive(EFF_PARALYZED), what, hits, want);
			}
			T.mark = ::ticks;
			if ( T.mon[3] ) { T.bx = T.mon[3]->x; T.by = T.mon[3]->y; }
			T.phase = 3;
			return;
		}
		if ( T.phase == 3 && ::ticks - T.mark == (Uint32)kStaggerTicks / 2 ) // mid-stagger: paralysed, takes 1.5x
		{
			Stat* bs = T.mon[3] ? T.mon[3]->getStats() : nullptr; // the boss-tier creature: immune to the spell, not to stagger
			T.check(bs && bs->getEffectActive(EFF_PARALYZED), "boss_tier_paralysed_mid_stagger", bs ? bs->getEffectActive(EFF_PARALYZED) : -1, 1);

			Entity* m = T.mon[1];
			Stat* ms = m ? m->getStats() : nullptr;
			T.check(ms && ms->getEffectActive(EFF_PARALYZED), "regular_paralysed_mid_stagger", ms ? ms->getEffectActive(EFF_PARALYZED) : 0, 1);
			if ( ms ) { int d = applyHit(my, s, m, ms, 10, Zone::Torso, WeaponClass::Sword); T.check(d == 15, "staggered_takes_1.5x", d, 15); }
			T.phase = 4;
			return;
		}
		if ( T.phase >= 3 && T.phase <= 4 && ::ticks - T.mark < (Uint32)kStaggerTicks - 1 && T.mon[3] && T.mon[3]->getStats() && T.mon[3]->getStats()->getEffectActive(EFF_PARALYZED) )
			++T.bossHeld; // the boss-tier creature held paralysed, tick by tick
		if ( T.phase == 4 && ::ticks - T.mark >= (Uint32)kStaggerTicks + 5 ) // stagger over
		{
			T.check(T.bossHeld >= kStaggerTicks - 3, "boss_tier_held_whole_stagger_ticks", T.bossHeld, kStaggerTicks - 1);
			Stat* ms = T.mon[1] ? T.mon[1]->getStats() : nullptr;
			T.check(ms && !ms->getEffectActive(EFF_PARALYZED), "stagger_ends", ms ? ms->getEffectActive(EFF_PARALYZED) : -1, 0);
			for ( auto* m : T.mon ) if ( m ) m->setHP(0); // clear the arena
			// zones on the player: legs, then arms (the creature-side hits use the same function)
			Rec& r = rec(my, s);
			s->HP = s->MAXHP;
			real_t before = speedScale(0);
			for ( int guard = 0; guard < 100 && r.zones.canRun(); ++guard ) { int d = applyHit(nullptr, nullptr, my, s, 4, Zone::Legs, WeaponClass::Unarmed); my->modHP(-d); s->HP = std::max(s->HP, 2); }
			T.check(speedScale(0) == kLegsDisabledSpeed && before == 1.0, "legs_disabled_speed_scale", speedScale(0), kLegsDisabledSpeed);
			for ( int guard = 0; guard < 100 && r.zones.canAttack(); ++guard ) { int d = applyHit(nullptr, nullptr, my, s, 4, Zone::Arms, WeaponClass::Unarmed); my->modHP(-d); s->HP = std::max(s->HP, 2); }
			T.check(!canAttack(my), "arms_disabled_no_attack", canAttack(my), 0);
			T.check(r.zones.hp[(int)Zone::Legs] == 0 && my->getStats() != nullptr, "zone_disabled_not_removed", r.zones.hp[(int)Zone::Legs], 0);
			r.zones.init(s->MAXHP);
			s->HP = s->MAXHP;
			T.check(canAttack(my) && speedScale(0) == 1.0, "zones_restored_by_heal", speedScale(0), 1.0);
			// downed: a lethal blow through the real modHP
			my->modHP(-1000);
			T.check(s->HP == 1 && downed(0).downed, "lethal_blow_downs_hp1", s->HP, 1);
			T.check(!canAttack(my) && speedScale(0) == kDownedSpeed, "downed_crawl_no_attack", speedScale(0), kDownedSpeed);
			my->modHP(-50);
			T.check(s->HP == 1, "no_hp_loss_while_down", s->HP, 1);
			selftestAlly() = 1;
			T.mark = ::ticks;
			T.phase = 5;
			return;
		}
		if ( T.phase == 5 && ::ticks - T.mark >= (Uint32)kReviveTicks + 2 )
		{
			T.check(!downed(0).downed && s->HP == downed(0).reviveHP(s->MAXHP), "ally_revive_3s_25pct", s->HP, downed(0).reviveHP(s->MAXHP));
			selftestAlly() = 0;
			downed(0).newFloor();
			my->modHP(-1000);
			T.check(downed(0).downed, "downed_again", downed(0).downed, 1);
			T.mark = ::ticks;
			T.phase = 6;
			return;
		}
		if ( T.phase == 6 && (s->HP <= 0 || ::ticks - T.mark >= (Uint32)kDownedTicks + 50) )
		{
			Uint32 took = ::ticks - T.mark;
			T.check(s->HP <= 0 && took >= (Uint32)kDownedTicks - 2 && took <= (Uint32)kDownedTicks + 2, "bleed_out_20s", took, kDownedTicks);
			printlog("[apx-f4c] summary pass=%d fail=%d poise=%d zones=%d downed=%d coop_forced=%d", T.pass, T.fail, (int)poiseOn(), (int)zonesOn(), (int)downedOn(), (int)forceCoop());
			T.phase = 7;
			if ( selftestall::armed() )
			{
				char buf[160];
				snprintf(buf, sizeof buf, "\"checks_pass\":%d,\"checks_fail\":%d,\"poise\":%d,\"zones\":%d,\"downed\":%d", T.pass, T.fail,
					(int)poiseOn(), (int)zonesOn(), (int)downedOn());
				selftestall::done(selftestall::F4C, T.fail == 0 && T.pass > 0, buf);
			}
			else { mainloop = 0; }
		}
	}
} // namespace combat_glue
} // namespace apx
