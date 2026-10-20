// apx/combat/core.cpp -- D93 combat core, phase C1 (spec 04b section 6). See core.hpp.
//
//   data        moves_table.inc (generated from data/apx/combat/moves.json)
//   control     tickPlayer(): reads apx::in::take(player) only; Attack starts the family's move, motion in the first
//               150 ms of the wind-up picks 1 of 5 directions, a tap fires the quick move, holding past 18 ticks charges
//               the family's charged move at 2x stamina
//   hits        each Active tick sweeps its slice of the arc at the move's reach from the attacker; bodies never block a
//               sweep, walls do; one hit per target per swing
//   damage      applyHit(): impact rule, armour, zones, poise, tier, stamina, effects; the single entry point
//   presenting  poseHud() poses the first-person weapon from the state; 40 ms hit stop; small camera kick
#include "core.hpp"

#include "../../main.hpp"
#include "../../game.hpp"
#include "../../stat.hpp"
#include "../../entity.hpp"
#include "../../player.hpp"
#include "../../monster.hpp"
#include "../../items.hpp"
#include "../../net.hpp"
#include "../../collision.hpp"
#include "../../engine/audio/sound.hpp"
#include "../../interface/interface.hpp"
#include "../../apx_stamina.hpp"
#include "../../apx_combat.hpp"
#include "../parts_engine.hpp"
#include "../tiers.hpp"
#include "../variants_glue.hpp"
#include "../../magic/magic.hpp"
#include "../selftest_all.hpp"
#include "../qadriver.hpp"
#include "../in/in.hpp"
#include "../move/move.hpp"
#include "../hostility.hpp"
#include <unordered_map>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace apx
{
namespace combat
{
	namespace
	{
#include "moves_table.inc"

		bool envZero(const char* n) { const char* v = std::getenv(n); return v && v[0] == '0'; }
		bool envOne(const char* n) { const char* v = std::getenv(n); return v && v[0] == '1'; }
		const bool gEnabled = !envZero("APX_COMBAT") && !envOne("APX_COMBAT_OLD") && !envOne("APX_VANILLA");

		constexpr int kTickMs = 20;
		inline int msToTicks(int ms) { return std::max(1, (ms + kTickMs - 1) / kTickMs); }
		constexpr int kMaxHoldTicks = 100;       // a charge held this long releases itself
		constexpr float kBeamHalfWidth = 4.f;    // units: a line move's half width beyond the body radius
		constexpr int kFizzleTicks = 12;
		constexpr double kHitBudgetMs = 0.08;   // after this much combat time in a tick, swept targets still waiting land on the next tick (a hit costs ~0.1 ms)
		constexpr int kMinTelegraphTicks = 15;  // 300 ms: a creature strike is never earlier than this after its wind-up began
		std::chrono::steady_clock::time_point gTickT0;
		double gTickLogBase = 0.0;
		double tickElapsedMs();
		constexpr int kTimedBlockTicks = 10;     // 200 ms (BL 6 ticks; fork 10): a block this young is free
		constexpr int kRiposteTicks = 13;        // 267 ms window after a timed block
		constexpr int kChamberTicks = 7;         // 133 ms: a counter pressed this soon after the block lands at once
		constexpr int kDodgeTicks = 8;           // 150 ms invulnerable (rounded to 20 ms ticks)
		constexpr int kDodgeCooldown = 20;
		constexpr double kDodgeCost = 15.0, kFeintCost = 10.0;
		constexpr int kKickCooldown = 25, kFeintRecover = 8;
		constexpr float kRiposteMult = 1.5f, kChamberMult = 2.0f;
		constexpr int kMaxHitsPerTick = 64;      // hits resolved in one sim tick (one hit costs ~0.12 ms in Barony's own stat code); swept targets left over follow on the next ticks, 20 ms each

		Counters gCount;
		bool gObjectPass = false;
		// the swing in flight / last finished, for the self-test and QA verdicts
		struct LastSwing { int swing = 0, moveId = 0, dir = 0, hits = 0; bool charged = false, done = false; double cost = 0, table = 0, before = 0, after = 0; };
		LastSwing gLast;
		std::vector<Uint32> gDummies; // the QA arena's creatures

		// ---------------------------------------------------------------- JSONL telemetry
		FILE* jsonl()
		{
			static FILE* f = nullptr;
			static bool tried = false;
			if ( !tried )
			{
				tried = true;
				const char* p = std::getenv("APX_COMBAT_JSONL");
				if ( p && p[0] ) { f = fopen(p, "a"); }
			}
			return f;
		}
		double gLogMs = 0.0;   // time spent writing telemetry: excluded from the core's measured cost
		double gLegacyMs = 0.0; // applyHit time at the Barony hit sites (creature melee, arrows, thrown) since the last player tick: counted in the per-tick cost
		struct LegacyTimer { std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now(); ~LegacyTimer() { gLegacyMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); } };
		// where the time goes (ms, calls), dumped in the perf row (sec_ms / sec_n):
		//  0 damage calc   5 getAttack  6 AC  7 damage table  8 AC effectiveness   (inside 0)
		//  1 rule stages (tier D74, zones F4c, poise D75; 9 = the tier part)
		//  2 modHP   3 consequences (10 effects and armour wear, 11 messages, 12 enemy bar, 13 sound, 14 alert)   4 sweep scan   15 object pass
		double gSecMs[16] = {};
		long long gSecN[16] = {};
		struct Sec
		{
			int i; std::chrono::steady_clock::time_point t0;
			explicit Sec(int idx) : i(idx), t0(std::chrono::steady_clock::now()) {}
			void end() { if ( i >= 0 ) { gSecMs[i] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); ++gSecN[i]; i = -1; } }
			~Sec() { end(); }
		};
		bool logOn() { static const bool on = envOne("APX_COMBAT_LOG") || jsonl() != nullptr; return on; }
		void row(const char* fmt, ...)
		{
			if ( !logOn() ) { return; } // counters only unless APX_COMBAT_LOG=1 or APX_COMBAT_JSONL is set
			const auto t0 = std::chrono::steady_clock::now();
			FILE* f = jsonl();
			char buf[640];
			va_list ap;
			va_start(ap, fmt);
			vsnprintf(buf, sizeof buf, fmt, ap);
			va_end(ap);
			printlog("[apx-combat] %s", buf);
			if ( f ) { fprintf(f, "%s\n", buf); fflush(f); }
			gLogMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		}

		// ---------------------------------------------------------------- perf ring
		struct Perf
		{
			std::vector<float> all, active;
			Perf() { all.reserve(1 << 16); active.reserve(1 << 14); }
		} gPerf;
		double pct(std::vector<float> v, double q)
		{
			if ( v.empty() ) { return 0.0; }
			std::sort(v.begin(), v.end());
			return v[std::min(v.size() - 1, (size_t)(q * v.size()))];
		}

		// ---------------------------------------------------------------- the fighter
		struct Fighter
		{
			State st = State::Idle;
			int fam = FAM_NONE;
			int moveId = 0, dir = DIR_THRUST;
			bool charged = false;
			int t = 0;                // ticks in the state
			int heldTicks = 0;
			bool released = false, dirLocked = false;
			bool needRelease = false; // an attempt failed: wait for the button to come up
			// C2 defence verbs (04b section 6): block, timed block, riposte, chamber, dodge, feint, kick
			bool blocking = false;    // Block held (and a shield up, or a staff braced)
			bool bracing = false;
			int blockT = -1;          // ticks since the block went up (0 on its first tick); -1 = down
			int riposteLeft = 0, chamberLeft = 0;  // ticks left of the riposte window (267 ms) and its first part, the chamber (133 ms)
			int dodgeLeft = 0, dodgeCool = 0, kickLeft = 0, kickCool = 0, feintLeft = 0;
			bool riposteSwing = false, chamberSwing = false;
			int probeAt = -1, probeFire = -1, probeMode = 0;  // probeMode 1: relative to a dodge press
			double dodgeX = 0, dodgeY = 0;
			  // QA: a hit on the player this many ticks after the block went up (/apxcombat_probe)
			float swDX = 0.f, swDY = 0.f;
			int activeTicks = 0, recoverTicks = 0, quickTicks = 0;
			int hitstop = 0;
			bool underBlock = false;
			std::vector<Uint32> hitUids;
			int hitCount = 0;
			bool objectChecked = false;
			// pose
			double rest[6] = { 0, 0, 0, 0, 0, 0 };
			bool restValid = false;
			double poseOut[6] = { 0, 0, 0, 0, 0, 0 };
			// telemetry of the swing in flight
			double staminaAtStart = 0.0, costPaid = 0.0;
			int swingId = 0;
			Uint32 startTick = 0;
		};
		Fighter gF[MAXPLAYERS];
		int gSwingSeq = 0;

		inline bool isCreature(Entity* e) { return e && (e->behavior == &actMonster || e->behavior == &actPlayer); }
		inline double wrapPi(double a) { while ( a > PI ) { a -= 2 * PI; } while ( a < -PI ) { a += 2 * PI; } return a; }

		Family famOf(Stat* st) { return (Family)apx::familyOf(st); }

		// the Barony class a move's family reports for stats and the damage table
		const char* kindName(int k) { static const char* n[] = { "impact", "shear", "pierce" }; return n[std::max(0, std::min(2, k))]; }

		void resetSwing(Fighter& f)
		{
			f.st = State::Idle; f.t = 0; f.heldTicks = 0; f.released = false; f.dirLocked = false; f.swDX = f.swDY = 0.f;
			f.moveId = 0; f.charged = false; f.hitUids.clear(); f.hitCount = 0; f.objectChecked = false; f.hitstop = 0; f.riposteSwing = f.chamberSwing = false;
			f.restValid = false;
		}
	} // namespace

	// ---------------------------------------------------------------------------------------------------------
	const char* stateName(State s)
	{
		static const char* n[] = { "Idle", "Windup", "Active", "Recovery", "Block", "TimedBlock", "Riposte", "Chamber", "Dodge", "Feint", "Kick", "Bash", "Stagger", "Knockdown", "Downed" };
		return n[(int)s];
	}
	const char* dirName(int d) { static const char* n[] = { "thrust", "left", "right", "overhead", "low" }; return n[std::max(0, std::min(NUM_DIRS - 1, d))]; }
	const char* familyName(int f) { static const char* n[] = { "none", "sword", "axe", "mace", "spear", "staff", "dagger", "unarmed", "shield" }; return n[std::max(0, std::min(FAM_COUNT - 1, f))]; }

	const MoveRow* moveById(int id)
	{
		for ( const MoveRow& m : kMoveRows ) { if ( m.id == id ) { return &m; } }
		return nullptr;
	}
	int moveCount() { return (int)(sizeof(kMoveRows) / sizeof(kMoveRows[0])); }
	const MoveRow& moveAt(int i) { return kMoveRows[i]; }
	int moveFor(int fam, int dir, bool charged)
	{
		if ( fam <= 0 || fam >= FAM_COUNT ) { return 0; }
		return kFamilyDirs[fam][charged ? 6 : 1 + std::max(0, std::min(NUM_DIRS - 1, dir))] ;
	}

	bool coreEnabled() { return gEnabled; }
	bool objectPass() { return gObjectPass; }
	const Counters& counters() { return gCount; }

	static int arenaAlive()
	{
		int n = 0;
		for ( Uint32 uid : gDummies ) { Entity* e = uidToEntity(uid); if ( e && e->getStats() && e->getStats()->HP > 0 ) { ++n; } }
		return n;
	}

	void perfDump()
	{
		if ( gPerf.all.empty() ) { return; }
		char secMsBuf[400] = "", secNBuf[300] = "";
		for ( int i = 0; i < 16; ++i ) { char t[32]; snprintf(t, sizeof t, "%s%.4f", i ? "," : "", gSecMs[i]); strcat(secMsBuf, t); snprintf(t, sizeof t, "%s%lld", i ? "," : "", gSecN[i]); strcat(secNBuf, t); }
		row("{\"ev\":\"perf\",\"ticks\":%zu,\"p50_ms\":%.4f,\"p99_ms\":%.4f,\"max_ms\":%.4f,\"swing_ticks\":%zu,\"swing_p50_ms\":%.4f,\"swing_p99_ms\":%.4f,\"swing_max_ms\":%.4f,\"creatures\":%d,\"stamina\":%.2f,\"stamina_max\":%.2f,\"last_spend\":%u,\"now\":%u,\"defending\":%d,\"stamina_ticks\":%llu,\"sec_ms\":[%s],\"sec_n\":[%s],\"arena\":%zu,\"arena_alive\":%d,\"swimming\":%d,\"inwater\":%d,\"z\":%.2f}",
			gPerf.all.size(), pct(gPerf.all, 0.5), pct(gPerf.all, 0.99), pct(gPerf.all, 1.0),
			gPerf.active.size(), pct(gPerf.active, 0.5), pct(gPerf.active, 0.99), pct(gPerf.active, 1.0),
			map.creatures ? (int)list_Size(map.creatures) : 0, apx::get(0).value, apx::get(0).maxValue, (unsigned)apx::get(0).lastSpendTick, (unsigned)ticks,
			stats[0] ? (int)stats[0]->defending : -1, apx::tickCalls,
			secMsBuf, secNBuf,
			gDummies.size(), arenaAlive(),
			(players[0] && players[0]->entity) ? (int)players[0]->movement.isPlayerSwimming() : -1, (players[0] && players[0]->entity) ? (int)players[0]->entity->skill[13] : -1,
			(players[0] && players[0]->entity) ? (double)players[0]->entity->z : 0.0);
		gPerf.all.clear();
		gPerf.active.clear();
	}

	bool ownsMelee(int p)
	{
		if ( !gEnabled || p < 0 || p >= MAXPLAYERS || !players[p] || !players[p]->entity || !stats[p] ) { return false; }
		if ( !players[p]->isLocalPlayer() ) { return false; }                  // co-op remote swings: C5
		if ( multiplayer == CLIENT ) { return false; }                          // co-op packets: C5
		Entity* me = players[p]->entity;
		if ( me->effectShapeshift != NOTHING || me->effectPolymorph != NOTHING ) { return false; }
		if ( cast_animation[p].active || cast_animation[p].active_spellbook || cast_animation[p].spellWaitingAttackInput() ) { return false; }
		return famOf(stats[p]) != FAM_NONE;
	}
	bool swinging(int p)
	{
		if ( p < 0 || p >= MAXPLAYERS ) { return false; }
		const State s = gF[p].st;
		return s == State::Windup || s == State::Active || s == State::Recovery;
	}
	State stateOf(Entity* e)
	{
		if ( !e ) { return State::Idle; }
		if ( e->behavior == &actPlayer )
		{
			const int p = e->skill[2];
			if ( p >= 0 && p < MAXPLAYERS )
			{
				const Fighter& f = gF[p];
				if ( f.st != State::Idle && f.st != State::Recovery ) { return f.st; }
				if ( f.dodgeLeft > 0 ) { return State::Dodge; }
				if ( f.kickLeft > 0 ) { return State::Kick; }
				if ( f.feintLeft > 0 ) { return State::Feint; }
				if ( f.st == State::Recovery ) { return f.st; }
				if ( f.blocking && f.blockT >= 0 && f.blockT <= kTimedBlockTicks ) { return State::TimedBlock; }
				if ( f.chamberLeft > 0 ) { return State::Chamber; }
				if ( f.riposteLeft > 0 ) { return State::Riposte; }
				if ( f.blocking ) { return State::Block; }
			}
		}
		if ( apx::combat_glue::staggered(e) ) { return State::Stagger; }
		Stat* s = e->getStats();
		if ( s && s->defending ) { return State::Block; }
		return State::Idle;
	}

	// =========================================================================================================
	// applyHit
	// =========================================================================================================
	namespace
	{
		combat::Zone zoneOf(const Hit& h, const MoveRow* mv)
		{
			int z = h.zoneHint;
			if ( z < 0 && mv ) { z = mv->zone; }
			if ( z >= 0 && z <= 3 ) { return (combat::Zone)z; }
			return combat_glue::strikeZone(h.attacker, h.target);
		}

		combat::WeaponClass wclassOf(const MoveRow* mv, Stat* as)
		{
			if ( mv )
			{
				switch ( mv->family )
				{
					case FAM_SWORD: return combat::WeaponClass::Sword;
					case FAM_AXE: return combat::WeaponClass::Axe;
					case FAM_MACE: return combat::WeaponClass::Mace;
					case FAM_SPEAR: return combat::WeaponClass::Polearm;
					case FAM_STAFF: return combat::WeaponClass::Staff;
					case FAM_DAGGER: return combat::WeaponClass::Dagger;
					default: return combat::WeaponClass::Unarmed;
				}
			}
			return combat_glue::weaponClassOf(as);
		}

		// the target is not in the attack state when the blow lands (the sneak rule reads this before the hit alerts it)
		bool unawareOf(Entity* victim, int previousState)
		{
			return victim && victim->behavior == &actMonster && previousState != MONSTER_STATE_ATTACK;
		}

		void afterEffects(Entity* attacker, Entity* victim, Stat* vstats, const MoveRow* mv, int damage, bool wasBlocking, bool stagBefore, bool wasUnaware)
		{
			if ( !mv || !vstats || !victim ) { return; }
			using namespace apx::fam;
			if ( mv->effects & EF_PUSH ) { push(attacker, victim, mv->force); }
			if ( (mv->effects & EF_THROW) && isSmall(victim) ) { knockdown(victim, mv->knockdownTicks); }
			if ( (mv->effects & EF_KNOCKDOWN_SMALL) && isSmall(victim) ) { knockdown(victim, mv->knockdownTicks); }
			if ( mv->effects & EF_GRAPPLE ) { knockdown(victim, mv->knockdownTicks); }
			if ( (mv->effects & EF_SHIELD_ASIDE) && wasBlocking ) { vstats->defending = false; }
			if ( (mv->effects & EF_SHIELD_BREAK) && wasBlocking )
			{
				vstats->defending = false;
				if ( vstats->shield ) { victim->degradeArmor(*vstats, *vstats->shield, 4); }
			}
			if ( (mv->effects & EF_CRIT_BLEED) && (wasUnaware || stagBefore) ) { victim->setEffect(EFF_BLEEDING, true, 150, true); }
			if ( mv->effects & EF_SLAM )
			{
				for ( node_t* node = map.creatures ? map.creatures->first : nullptr; node; node = node->next )
				{
					Entity* e = (Entity*)node->element;
					if ( !e || e == victim || e == attacker || e->behavior != &actMonster ) { continue; }
					const double dx = e->x - victim->x, dy = e->y - victim->y;
					if ( dx * dx + dy * dy > (double)mv->radius * mv->radius || e->monsterAllyGetPlayerLeader() ) { continue; }
					const double t = atan2(dy, dx);
					e->vel_x += cos(t) * mv->force;
					e->vel_y += sin(t) * mv->force;
					if ( isSmall(e) ) { knockdown(e, mv->knockdownTicks); }
				}
			}
			(void)damage;
		}

		// victim's armour wears under a blow (Barony's rule: a random worn piece, 1 in 25; mace 1 in 10)
		void wearArmour(Entity* victim, Stat* vstats, const MoveRow* mv)
		{
			if ( !victim || !vstats ) { return; }
			Item* armor = nullptr;
			const int armornum = vstats->pickRandomEquippedItemToDegradeOnHit(&armor, true, false, false, true);
			if ( !armor || armor->status <= BROKEN ) { return; }
			int chance = (mv && mv->family == FAM_MACE) ? 10 : 25;
			if ( vstats->type == GOBLIN ) { chance += 10; }
			if ( local_rng.rand() % chance == 0 ) { victim->degradeArmor(*vstats, *armor, armornum); }
		}

		// player melee damage before the rule stages: Barony's attack value, AC, the move's impact rule, skill variance
		int meleeDamage(Entity* me, Stat* ms, Entity* target, Stat* ts, const MoveRow* mv, const Hit& h, bool unaware, float& mult, bool& sneakOut, MeleeCtx& c)
		{
			const bool isPlayerAtk = me->behavior == &actPlayer;
			const int maxCharge = Stat::getMaxAttackCharge(ms);
			const int charge = (mv && mv->charged) ? maxCharge : 0;
			// weapon skill and the damage-table class
			int weaponskill = ms->weapon ? getWeaponSkill(ms->weapon) : PRO_UNARMED;
			if ( weaponskill < 0 ) { weaponskill = PRO_UNARMED; }
			DamageTableType dmgType = DAMAGE_TABLE_UNARMED;
			if ( weaponskill >= PRO_SWORD && weaponskill <= PRO_POLEARM ) { dmgType = (DamageTableType)(weaponskill - PRO_SWORD); }
			c.target = target; c.pose = 1; c.charge = charge; c.weaponskill = weaponskill; c.dmgType = (int)dmgType; c.extraTarget = (h.flags & HF_EXTRA_TARGET) != 0;
			c.previousMonsterState = target->monsterState;
			Sec t5(5);
			int myAttack = std::max(0, Entity::getAttack(me, ms, isPlayerAtk, charge) + me->getBonusAttackOnTarget(*ts));
			myAttack += apxPartsAttackOnTarget(me, ms, ts, myAttack);
			t5.end();
			Sec t7(7);
			real_t wm = Entity::getDamageTableMultiplier(target, *ts, dmgType); // class table (+ the class's impact rule when on) and Barony's bonuses
			// D93: the impact rule for THIS move (kind, momentum, contact area) in place of the class's reference move
			mult = 1.f;
			if ( mv && apx::combat::enabled() && dmgType != DAMAGE_TABLE_MAGIC )
			{
				const float cls = apx::combat::multiplierFor((int)ts->type, (int)dmgType, -1.f);
				combat::Move m{ (combat::Kind)(h.moveId ? mv->kind : h.kind), h.momentum > 0.f ? h.momentum : mv->momentum, h.area > 0.f ? h.area : mv->area };
				const float mine = apx::combat::multiplierForMove((int)ts->type, m, -1.f);
				if ( cls > 0.f && mine >= 0.f ) { mult = mine / cls; wm *= mult; apx::combat::countUse(); }
			}
			t7.end();
			c.weaponMultipliers = wm;
			me->apxMeleePre1(c); // Barony: thorns, whip pierce, effect multipliers, divine / artifact smites
			wm = c.weaponMultipliers;
			Sec t6(6);
			int enemyAC = AC(ts);
			if ( mv )
			{
				if ( (mv->effects & (EF_SHIELD_ASIDE | EF_SHIELD_BREAK)) && ts->shield && ts->defending )
				{
					ts->defending = false;
					enemyAC = AC(ts); // the block's bonus does not apply to a hook or a cleave
					ts->defending = true;
				}
				if ( mv->armourIgnore > 0.f ) { enemyAC = (int)(enemyAC * (1.0 - mv->armourIgnore)); }
			}
			t6.end();
			c.enemyAC = enemyAC;
			me->apxMeleePre2(c); // Barony: Gungnir, whip pierce, drum
			enemyAC = c.enemyAC;
			Sec t8b(8);
			int numBlessings = 0;
			const real_t eff = Entity::getACEffectiveness(target, ts, target->behavior == &actPlayer, me, ms, numBlessings);
			t8b.end();
			const int attackAfterReductions = (int)(std::max(0.0, (myAttack * eff - enemyAC)) + (1.0 - eff) * myAttack);
			c.myAttack = myAttack; c.attackAfterReductions = attackAfterReductions;
			int damage = (int)(attackAfterReductions * wm);
			if ( eff < 0.99 ) { damage = std::max(1, damage); } else { damage = std::max(0, damage); }
			if ( weaponskill == PRO_AXE ) { damage++; }
			// skill variance: Barony's modifier by proficiency (40-60 percent base)
			if ( (weaponskill >= PRO_SWORD && weaponskill < PRO_SHIELD) || weaponskill == PRO_UNARMED )
			{
				real_t variance = 20;
				real_t baseSkillModifier = 50.0;
				Entity::setMeleeDamageSkillModifiers(me, ms, weaponskill, baseSkillModifier, variance, nullptr);
				real_t skillModifier = baseSkillModifier - (variance / 2) + (ms->getModifiedProficiency(weaponskill) / 2.0);
				skillModifier += (local_rng.rand() % (1 + static_cast<int>(variance)));
				skillModifier /= 100.0;
				skillModifier = std::min(skillModifier, 1.0);
				damage = damage - static_cast<int>((1.0 - skillModifier) * damage);
			}
			// the move: damage multiple, falloff after the first target, sneak (x2, spec 04 s4) and the family's sneak/critical
			double d = damage;
			if ( mv )
			{
				d *= mv->damage * h.dmgMult;
				if ( h.flags & HF_EXTRA_TARGET ) { d *= (h.extraIndex <= 0 ? mv->falloff1 : mv->falloff2); }
			}
			sneakOut = false;
			if ( unaware && isPlayerAtk )
			{
				d *= 2.0;
				sneakOut = true;
				if ( mv && (mv->effects & EF_SNEAK) ) { d *= mv->sneakMult; }
			}
			if ( mv && (mv->effects & EF_CRIT_BLEED) && !sneakOut && ts->getEffectActive(EFF_PARALYZED) ) { d *= mv->sneakMult; sneakOut = true; }
			damage = (int)d;
			if ( ms->weapon && ms->weapon->type == ARTIFACT_AXE ) // Parashu sometimes multiplies the blow
			{
				real_t amount = 0.0;
				const real_t percent = getArtifactWeaponEffectChance(ms->weapon->type, *ms, &amount);
				if ( local_rng.rand() % 100 < static_cast<int>(percent) ) { damage = (int)(damage * amount); c.parashuProc = true; }
			}
			if ( damage <= 0 && eff < 0.99 ) { damage = 1; }
			return damage;
		}
	} // namespace

	static bool defendHit(Entity* attacker, Entity* target, int& damage);

	HitResult applyHit(const Hit& h)
	{
		HitResult r;
		Entity* me = h.attacker;
		Entity* target = h.target;
		if ( !target ) { return r; }
		Stat* ts = target->getStats();
		Stat* ms = me ? me->getStats() : nullptr;
		if ( !ts || ts->HP <= 0 ) { return r; }
		r.landed = true;
		r.hpBefore = ts->HP;
		const MoveRow* mv = h.moveId ? moveById(h.moveId) : nullptr;
		const bool stagBefore = ts->getEffectActive(EFF_PARALYZED) != 0;
		const bool wasBlocking = ts->shield && ts->defending;
		const int previousState = target->monsterState;
		const bool unaware = unawareOf(target, previousState);

		int damage = h.damage;
		bool sneak = false;
		MeleeCtx mctx;
		if ( !(h.flags & HF_PRECOMPUTED) )
		{
			if ( !me || !ms ) { r.landed = false; return r; }
			Sec sec(0);
			damage = meleeDamage(me, ms, target, ts, mv, h, unaware, r.multiplier, sneak, mctx);
		}
		r.sneak = sneak;
		if ( !(h.flags & HF_PRECOMPUTED) && target->behavior == &actPlayer )
		{
			int d = damage;
			if ( defendHit(me, target, d) ) { r.damage = 0; r.hpAfter = ts->HP; r.blocked = true; return r; } // dodged or timed block: the blow is spent
		}

		Sec secRules(1);
		// rule stage: D74 tier damage (a hostile creature's hit on a player or follower; the knob lives in tiers.cpp)
		Sec t9(9);
		if ( me ) { damage = apx::tierdmg::onCreatureHit(me, target, damage, (apx::tierdmg::Path)h.path); }
		t9.end();
		if ( me && h.path == PATH_MELEE && (h.flags & HF_PRECOMPUTED) ) { damage = apx::vglue::scaleDamage(me, damage); } // BV1: a variant creature's damage multiple
		// rule stage: F4c zones and D75 poise (armour and stance are read inside), staggered x1.5
		if ( me ) { apx::hostility::onHit(me, target, damage); } // provocation and the hostility gate's counters (was inside onMeleeHit, which the core bypasses)
		const combat::Zone zone = zoneOf(h, mv);
		if ( !(h.flags & HF_NO_ZONES) )
		{
			const int poise = h.poise >= 0 ? h.poise : (mv ? mv->poise : -1);
			damage = combat_glue::applyHit(me, ms, target, ts, damage, zone, wclassOf(mv, ms), poise);
		}
		r.zone = (int)zone;
		r.staggered = ts->getEffectActive(EFF_PARALYZED) != 0 && !stagBefore;
		secRules.end();
		if ( h.flags & HF_DRY ) { r.damage = damage; r.hpAfter = ts->HP; return r; }

		const Sint32 oldHP = ts->HP;
		Sec secHP(2);
		target->modHP(-damage);
		secHP.end();
		r.damage = std::max(0, oldHP - ts->HP);
		r.hpAfter = ts->HP;
		r.blocked = wasBlocking && damage == 0;
		r.killed = ts->HP <= 0 && oldHP > 0;
		if ( (h.flags & HF_PRECOMPUTED) ) { return r; } // creature melee, arrows, thrown: the engine does the rest of its own bookkeeping

		// ---- the swing's consequences (player melee) ----
		Sec secCons(3);
		{ Sec t10(10); if ( mv ) { afterEffects(me, target, ts, mv, damage, wasBlocking, stagBefore, unaware); }
		if ( me->behavior == &actPlayer || me->behavior == &actMonster )
		{
			// Barony's own rule code for the rest of the blow, kept verbatim (procs.cpp): skill gain, armour and shield wear, weapon wear,
			// obituary, compendium, and the weapon's procs (poison, paralyze, slow, knockback, bleed, artifact effects)
			mctx.damage = damage; mctx.oldHP = oldHP; mctx.backstab = sneak && unaware;
			me->apxMeleeAftermath(mctx);
		} }
		if ( me->behavior == &actPlayer )
		{
			const int player = me->skill[2];
			Uint32 color = makeColorRGB(0, 255, 0);
			if ( ts->HP > 0 )
			{
				if ( damage == 0 ) { messagePlayerMonsterEvent(player, 0xFFFFFFFF, *ts, Language::get(691), Language::get(ts->sex ? 695 : 696), MSG_COMBAT_BASIC); }
				else if ( sneak && unaware ) { messagePlayerMonsterEvent(player, color, *ts, Language::get(2543), Language::get(2544), MSG_COMBAT); }
				else { Sec t11(11); messagePlayerMonsterEvent(player, color, *ts, Language::get(690), Language::get(694), MSG_COMBAT_BASIC); }
			}
			else
			{
				messagePlayerMonsterEvent(player, color, *ts, Language::get(692), Language::get(697), MSG_COMBAT);
			}
			if ( ts->HP > 0 )
			{
				if ( mctx.playerPoisonedTarget ) { messagePlayerMonsterEvent(player, color, *ts, Language::get(3478), Language::get(3479), MSG_COMBAT); }
				if ( mctx.paralyzeStatusInflicted ) { messagePlayerMonsterEvent(player, color, *ts, Language::get(3206), Language::get(3205), MSG_COMBAT); }
				else if ( mctx.slowStatusInflicted ) { messagePlayerMonsterEvent(player, color, *ts, Language::get(394), Language::get(393), MSG_COMBAT); }
				else if ( mctx.swordExtraDamageInflicted ) { messagePlayerMonsterEvent(player, color, *ts, Language::get(3211), Language::get(3210), MSG_COMBAT); }
				else if ( mctx.knockbackInflicted ) { messagePlayerMonsterEvent(player, color, *ts, Language::get(3215), Language::get(3214), MSG_COMBAT); }
				else if ( mctx.envenomWeapon ) { messagePlayerMonsterEvent(player, color, *ts, Language::get(6533), Language::get(6534), MSG_COMBAT); }
			}
			// the HUD enemy bar
			DamageGib gib = DMG_DEFAULT;
			if ( r.multiplier >= 1.15f || (mv && mv->charged) ) { gib = (mv && mv->charged) ? DMG_STRONGEST : DMG_STRONGER; }
			else if ( r.multiplier > 0.f && r.multiplier <= 0.85f ) { gib = DMG_WEAKER; }
			Sec t12(12);
			updateEnemyBar(me, target, ts->name[0] ? ts->name : getMonsterLocalizedName(ts->type).c_str(), ts->HP, ts->MAXHP, false, gib);
		}
		if ( me->behavior == &actMonster && target->behavior == &actPlayer ) { cameravars[target->skill[2]].shakey += 8; cameravars[target->skill[2]].shakex += 0.1; }
		{ Sec t13(13); playSoundEntity(target, 28, 64); } // hit
		if ( r.killed )
		{
			me->awardXP(target, true, true);
			++gCount.kills;
		}
		else if ( target->behavior == &actMonster )
		{
			const bool friendly = !(svFlags & SV_FLAG_FRIENDLYFIRE) && me->checkFriend(target) && me->friendlyFireProtection(target);
			if ( !friendly && target->monsterAlertBeforeHit(me) && target->monsterState != MONSTER_STATE_ATTACK && target->monsterSpecialState == 0 )
			{
				Sec t14(14);
				target->monsterAcquireAttackTarget(*me, MONSTER_STATE_PATH, true);
			}
		}
		return r;
	}

	// C2: what the defender's state does to a creature's melee blow (dodge, timed block, block). Returns true when the blow is spent.
	static bool defendHit(Entity* attacker, Entity* target, int& damage)
	{
		if ( !target || target->behavior != &actPlayer || damage <= 0 ) { return false; }
		const int p = target->skill[2];
		if ( p < 0 || p >= MAXPLAYERS || !gEnabled ) { return false; }
		Fighter& f = gF[p];
		if ( f.dodgeLeft > 0 )
		{
			++gCount.dodged;
			row("{\"ev\":\"dodged\",\"tick\":%u,\"player\":%d,\"damage_in\":%d,\"dodge_left\":%d}", (unsigned)ticks, p, damage, f.dodgeLeft);
			damage = 0;
			return true;
		}
		if ( f.blocking && stats[p] && (stats[p]->shield || f.bracing) )
		{
			if ( f.blockT >= 0 && f.blockT <= kTimedBlockTicks )
			{
				++gCount.timedBlocks;
				f.riposteLeft = kRiposteTicks; f.chamberLeft = kChamberTicks;
				if ( attacker && attacker->getStats() ) { apx::fam::knockdown(attacker, 25); } // the attacker is staggered
				row("{\"ev\":\"timed_block\",\"tick\":%u,\"player\":%d,\"since_press_ticks\":%d,\"since_press_ms\":%d,\"damage_in\":%d,\"stamina\":%.2f}", (unsigned)ticks, p, f.blockT, f.blockT * kTickMs, damage, apx::get(p).value);
				damage = 0;
				return true;
			}
			++gCount.blocks;
			// the surplus the block absorbed comes off stamina (G11 rule, D64): the AC the raised guard added, measured on the target, is what it took off the blow
			double absorbed = 0.0;
			if ( stats[p]->shield && stats[p]->defending )
			{
				const int acUp = AC(stats[p]);
				stats[p]->defending = false;
				const int acDown = AC(stats[p]);
				stats[p]->defending = true;
				absorbed = std::max(0, acUp - acDown);
			}
			const double cost = absorbed / 10.0;
			apx::spend(p, cost, 1);
			row("{\"ev\":\"block\",\"tick\":%u,\"player\":%d,\"brace\":%d,\"since_press_ticks\":%d,\"damage_in\":%d,\"absorbed\":%.1f,\"stamina_cost\":%.2f,\"stamina\":%.2f}", (unsigned)ticks, p, f.bracing ? 1 : 0, f.blockT, damage, absorbed, cost, apx::get(p).value);
		}
		return false;
	}

	int onLegacyHit(Entity* attacker, Entity* target, int damage, int path)
	{
		if ( path == PATH_MELEE && gEnabled && defendHit(attacker, target, damage) ) { ++gCount.legacyHits; return 0; }
		LegacyTimer legacyTimer;
		++gCount.legacyHits;
		if ( !gEnabled )
		{
			// CONTROL: the hooks as they were before D93, at the hit site
			damage = apx::tierdmg::onCreatureHit(attacker, target, damage, (apx::tierdmg::Path)path);
			if ( path == PATH_MELEE && attacker && target && target->getStats() )
			{
				damage = apx::vglue::scaleDamage(attacker, damage);
				damage = combat_glue::onMeleeHit(attacker, attacker->getStats(), target, target->getStats(), damage);
			}
			return damage;
		}
		// the same stages through the one entry point; the site's own modHP deals what comes back
		Hit h;
		h.attacker = attacker; h.target = target; h.path = path; h.damage = damage;
		h.flags = HF_PRECOMPUTED | HF_DRY | (path == PATH_MELEE ? 0u : HF_NO_ZONES);
		if ( !target || !target->getStats() ) { return damage; }
		const HitResult r = applyHit(h);
		return r.landed ? r.damage : damage;
	}

	// =========================================================================================================
	// player control, the sweep
	// =========================================================================================================
	namespace { void qaOnce(int p); void clearDummies(); Uint32 gCreatureTick = 0xFFFFFFFFu; double tickElapsedMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gTickT0).count() - (gLogMs - gTickLogBase); } }
	namespace
	{
		int pickDir(float dx, float dy)
		{
			const float thr = 6.f; // pixels of look motion in the 150 ms window; below it = no direction (the family default)
			const float ax = std::fabs(dx), ay = std::fabs(dy);
			if ( ax < thr && ay < thr ) { return DIR_THRUST; }
			if ( ax >= ay ) { return dx < 0 ? DIR_LEFT : DIR_RIGHT; }
			return dy < 0 ? DIR_OVERHEAD : DIR_LOW;
		}

		// the sweep's centre offset (radians from the facing) at fraction u of the active time
		double sweepCentre(const MoveRow& mv, int dir, double u)
		{
			const double a = mv.arcHalf;
			if ( a <= 0.0 ) { return 0.0; }
			// the blade travels across the view: a right sweep goes from the left edge to the right edge
			const double s = (dir == DIR_LEFT) ? -1.0 : 1.0;
			return s * (-a + 2.0 * a * u);
		}

		bool wallBetween(Entity* me, double ang, double dist, double slack)
		{
			const hit_t saved = hit;
			const real_t got = lineTrace(me, me->x, me->y, ang, dist, 0, false);
			hit = saved;
			return got < dist - slack - 1.0;
		}

		// returns true when swept targets are left over for the next tick (the per-tick hit budget)
		bool sweepStep(int p, Fighter& f, const MoveRow& mv, double u0, double u1)
		{
			Sec scan(4);
			Entity* me = players[p]->entity;
			const double yaw = me->yaw;
			const double reachW = STRIKERANGE * (double)mv.reach;
			const bool sector = mv.arcHalf > 0.0 && !(mv.kind == PIERCE && mv.arcHalf < 0.15);
			double lo = 0, hi = 0;
			if ( sector )
			{
				const double c0 = sweepCentre(mv, f.dir, 0.0), c1 = sweepCentre(mv, f.dir, u1); // everything swept since the swing began (a deferred hit is not lost as the blade moves on)
				(void)u0;
				lo = std::min(c0, c1); hi = std::max(c0, c1);
			}
			struct Cand { Entity* e; double d; };
			static std::vector<Cand> cands; // capacity persists: no allocation per tick
			cands.clear();
			const bool ffOn = (svFlags & SV_FLAG_FRIENDLYFIRE) != 0;
			const int maxTargets = 1 + mv.extra;
			for ( node_t* node = map.creatures ? map.creatures->first : nullptr; node; node = node->next )
			{
				Entity* e = (Entity*)node->element;
				if ( e == me || !isCreature(e) ) { continue; }
				Stat* es = e->getStats();
				if ( !es || es->HP <= 0 ) { continue; }
				if ( std::find(f.hitUids.begin(), f.hitUids.end(), e->getUID()) != f.hitUids.end() ) { continue; }
				const double dx = e->x - me->x, dy = e->y - me->y;
				const double d = std::sqrt(dx * dx + dy * dy);
				const double r = std::max(e->sizex, e->sizey);
				if ( d > reachW + r ) { continue; }
				const double rel = wrapPi(std::atan2(dy, dx) - yaw);
				bool in = false;
				if ( sector )
				{
					const double db = std::atan2(r, std::max(d, 1.0));
					in = (rel >= lo - db) && (rel <= hi + db);
				}
				else
				{
					const double fwd = d * std::cos(rel), lat = std::fabs(d * std::sin(rel));
					in = fwd > -r * 0.5 && lat <= r + kBeamHalfWidth;
				}
				if ( !in ) { continue; }
				if ( !ffOn && me->checkFriend(e) && me->friendlyFireProtection(e) ) { continue; }
				cands.push_back({ e, d });
			}
			scan.end();
			if ( cands.empty() ) { return false; }
			int hitsThisTick = 0;
			bool deferred = false;
			std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
			for ( const Cand& c : cands )
			{
				if ( f.hitCount >= maxTargets ) { break; }
				if ( hitsThisTick >= kMaxHitsPerTick || (hitsThisTick > 0 && tickElapsedMs() > kHitBudgetMs) ) { deferred = true; break; }
				const double ang = std::atan2(c.e->y - me->y, c.e->x - me->x);
				const double r = std::max(c.e->sizex, c.e->sizey);
				if ( wallBetween(me, ang, c.d, r) ) { ++gCount.wallBlocked; continue; } // walls block a sweep, bodies never
				Hit h;
				h.attacker = me; h.target = c.e; h.moveId = mv.id; h.dir = f.dir;
				h.flags = (f.hitCount > 0 ? HF_EXTRA_TARGET : 0u) | (mv.charged ? HF_CHARGED : 0u);
				h.extraIndex = f.hitCount > 0 ? f.hitCount - 1 : 0;
				h.dmgMult = f.riposteSwing ? (f.chamberSwing ? kChamberMult : kRiposteMult) : 1.f;
				if ( f.dir == DIR_LOW && !mv.charged ) { h.zoneHint = (int)combat::Zone::Legs; }
				else if ( f.dir == DIR_OVERHEAD && !mv.charged ) { h.zoneHint = (int)combat::Zone::Head; }
				Stat* es = c.e->getStats();
				const std::string tname = es ? getMonsterLocalizedName(es->type) : std::string("?");
				const int hpBefore = es ? es->HP : 0;
				const HitResult res = applyHit(h);
				if ( !res.landed ) { continue; }
				f.hitUids.push_back(c.e->getUID());
				++f.hitCount; ++hitsThisTick;
				++gCount.hits; ++gCount.sweepTargets;
				if ( f.hitstop == 0 ) { f.hitstop = msToTicks(kHitStopMs); ++gCount.hitStops; }
				cameravars[p].shakey += 4;
				row("{\"ev\":\"hit\",\"tick\":%u,\"swing\":%d,\"player\":%d,\"fam\":\"%s\",\"move\":%d,\"name\":\"%s\",\"dir\":\"%s\",\"charged\":%d,\"target\":%u,\"type\":%d,\"tname\":\"%s\","
					"\"dist\":%.1f,\"order\":%d,\"dmg\":%d,\"hp\":%d,\"zone\":%d,\"mult\":%.3f,\"sneak\":%d,\"staggered\":%d,\"killed\":%d,\"hitstop\":%d,\"poisoned\":%d,\"wstatus\":%d,\"prof\":%d,\"riposte\":%d,\"hp_after\":%d}",
					(unsigned)ticks, f.swingId, p, familyName(mv.family), mv.id, mv.name, dirName(f.dir), mv.charged ? 1 : 0, c.e->getUID(), es ? (int)es->type : -1, tname.c_str(),
					c.d, f.hitCount - 1, res.damage, hpBefore, res.zone, res.multiplier, res.sneak ? 1 : 0, res.staggered ? 1 : 0, res.killed ? 1 : 0, f.hitstop,
					es ? (int)es->getEffectActive(EFF_POISONED) : -1, stats[p]->weapon ? (int)stats[p]->weapon->status : -1, stats[p]->getProficiency(stats[p]->weapon ? std::max(0, (int)getWeaponSkill(stats[p]->weapon)) : PRO_UNARMED), f.riposteSwing ? 1 : 0, es ? es->HP : -1);
			}
			return deferred;
		}

		// non-creature targets on the line (doors, colliders, boulders, furniture) still use Barony's object code
		void objectStrike(int p, Fighter& f, const MoveRow& mv)
		{
			Entity* me = players[p]->entity;
			// cheap pre-filter (the full line trace walks every entity of the floor, ~0.1 ms): is a solid non-creature entity in front and in reach?
			bool near = false;
			const double reachW = STRIKERANGE * (double)mv.reach;
			for ( node_t* node = map.entities ? map.entities->first : nullptr; node && !near; node = node->next )
			{
				Entity* e = (Entity*)node->element;
				if ( !e || e == me || isCreature(e) || e->flags[PASSABLE] || e->flags[INVISIBLE] || e->flags[SPRITE] ) { continue; }
				const double dx = e->x - me->x, dy = e->y - me->y;
				const double r = std::max(e->sizex, e->sizey);
				if ( dx * dx + dy * dy > (reachW + r + 4.0) * (reachW + r + 4.0) ) { continue; }
				if ( std::fabs(wrapPi(std::atan2(dy, dx) - me->yaw)) < 0.9 ) { near = true; }
			}
			if ( !near ) { return; }
			const hit_t saved = hit;
			const real_t got = lineTrace(me, me->x, me->y, me->yaw, STRIKERANGE * mv.reach, LINETRACE_ATK_CHECK_FRIENDLYFIRE, false);
			(void)got;
			Entity* obj = hit.entity;
			hit = saved;
			if ( !obj || isCreature(obj) ) { return; }
			gObjectPass = true;
			me->attack(1, 0, nullptr);
			gObjectPass = false;
			++gCount.objectPasses;
			row("{\"ev\":\"object\",\"tick\":%u,\"swing\":%d,\"uid\":%u}", (unsigned)ticks, f.swingId, obj->getUID());
		}

		void startActive(int p, Fighter& f, const MoveRow& mv)
		{
			Entity* me = players[p]->entity;
			f.st = State::Active; f.t = 0;
			f.activeTicks = std::max(3, msToTicks(mv.activeMs));
			f.recoverTicks = msToTicks(mv.recoverMs);
			f.hitUids.clear(); f.hitCount = 0; f.objectChecked = false;
			// the body animation other players see, and the whoosh
			me->skill[10] = 0;
			me->skill[9] = 1;
			if ( multiplayer == SERVER ) { serverUpdateEntitySkill(me, 9); serverUpdateEntitySkill(me, 10); }
			playSoundEntity(me, mv.sound + local_rng.rand() % 5, 128);
			if ( mv.effects & EF_DASH ) { apx::move::impulse(me, (float)std::cos(me->yaw) * mv.dash * 2.5f, (float)std::sin(me->yaw) * mv.dash * 2.5f, 6); }
			cameravars[p].shakey += 3;
		}

		void controlTick(int p, Fighter& f, const in::TickRecord& rec)
		{
			Entity* me = players[p]->entity;
			Stat* ms = stats[p];
			apx::Stamina_t& sta = apx::get(p);
			const bool held = rec.isHeld(in::B_Attack);
			if ( f.hitstop > 0 && f.st == State::Active )
			{
				// 40 ms hit stop: the swing's clock and the pose hold; swept targets still waiting for their hit (per-tick budget) land meanwhile
				--f.hitstop;
				if ( const MoveRow* hm = moveById(f.moveId) ) { sweepStep(p, f, *hm, 0.0, std::min(1.0, (double)f.t / f.activeTicks)); }
				return;
			}

			if ( f.st == State::Idle )
			{
				if ( f.needRelease ) { if ( !held ) { f.needRelease = false; } return; }
				const bool press = rec.pressCount[in::B_Attack] > 0;
				if ( !(press || held) ) { return; }
				if ( !apx::canSwing(p) ) { f.needRelease = true; ++gCount.fizzled; row("{\"ev\":\"fizzle\",\"tick\":%u,\"player\":%d,\"stamina\":%.2f}", (unsigned)ticks, p, sta.value); return; }
				f.fam = famOf(ms);
				if ( f.fam == FAM_NONE ) { return; }
				f.st = State::Windup; f.t = 0; f.heldTicks = 0; f.released = false; f.dirLocked = false; f.swDX = f.swDY = 0.f; f.charged = false;
				f.underBlock = apx::blockSwingEnabled() && ms->defending;
				f.riposteSwing = f.riposteLeft > 0; f.chamberSwing = f.chamberLeft > 0;
				if ( f.riposteSwing ) { f.riposteLeft = 0; f.chamberLeft = 0; row("{\"ev\":\"riposte_swing\",\"tick\":%u,\"player\":%d,\"chamber\":%d}", (unsigned)ticks, p, f.chamberSwing ? 1 : 0); }
				f.staminaAtStart = sta.value;
				f.swingId = ++gSwingSeq;
				f.startTick = (unsigned)ticks;
				f.dir = DIR_THRUST;
			}

			if ( f.st == State::Windup )
			{
				++f.t;
				if ( held ) { ++f.heldTicks; }
				if ( rec.releaseCount[in::B_Attack] > 0 || !held ) { f.released = true; }
				const int dirTicks = msToTicks(kDirectionWindowMs);
				if ( !f.dirLocked )
				{
					f.swDX += rec.swingDX; f.swDY += rec.swingDY;
					if ( f.t >= dirTicks ) { f.dirLocked = true; f.dir = pickDir(f.swDX, f.swDY); }
				}
				if ( held && f.heldTicks >= kMaxHoldTicks ) { f.released = true; }
				f.charged = f.heldTicks >= kChargeHoldTicks;
				const int chMove = moveFor(f.fam, f.dir, true);
				const bool canCharge = chMove != 0;
				const int quickMoveId = moveFor(f.fam, f.dir, false);
				const MoveRow* qm = moveById(quickMoveId);
				f.quickTicks = qm ? msToTicks(qm->windupMs) : 12;
				if ( f.riposteSwing ) { f.quickTicks = f.chamberSwing ? 1 : std::max(2, f.quickTicks / 2); } // a riposte skips most of the draw; a chamber none
				bool commit = false;
				if ( f.riposteSwing && f.t >= f.quickTicks ) { commit = true; f.charged = false; }
				else if ( f.released && f.dirLocked )
				{
					if ( f.charged && canCharge ) { commit = true; }
					else if ( f.t >= f.quickTicks ) { commit = true; }
				}
				if ( !commit ) { return; }
				const bool useCharged = f.charged && canCharge;
				f.moveId = useCharged ? chMove : quickMoveId;
				const MoveRow* mv = moveById(f.moveId);
				if ( !mv ) { resetSwing(f); return; }
				// stamina: the move's multiple of one swing, 1.5x under a block (D64); the charged move is 2x (D67)
				const double cost = apx::kSwingCost * mv->staminaMult * (f.underBlock ? apx::kSwingWhileBlockCostMult : 1.0);
				if ( sta.value < 1.0 )
				{
					++gCount.fizzled; f.needRelease = held;
					row("{\"ev\":\"fizzle\",\"tick\":%u,\"player\":%d,\"stamina\":%.2f,\"move\":%d}", (unsigned)ticks, p, sta.value, mv->id);
					f.st = State::Recovery; f.t = 0; f.recoverTicks = kFizzleTicks;
					return;
				}
				const double before = sta.value;
				f.costPaid = apx::spend(p, cost, 0);
				sta.swingWhileBlock = f.underBlock;
				++sta.swings;
				if ( f.underBlock ) { ++sta.swingsWhileBlocking; }
				sta.move = mv->id;
				gCount.staminaSpent += f.costPaid;
				++gCount.swings;
				gLast.swing = f.swingId; gLast.moveId = mv->id; gLast.dir = f.dir; gLast.hits = 0; gLast.charged = useCharged; gLast.done = false;
				gLast.cost = f.costPaid; gLast.table = cost; gLast.before = before; gLast.after = sta.value;
				if ( useCharged ) { ++gCount.charged; } else { ++gCount.quick; if ( f.dir != DIR_THRUST ) { ++gCount.directional; } }
				row("{\"ev\":\"swing\",\"tick\":%u,\"swing\":%d,\"player\":%d,\"fam\":\"%s\",\"move\":%d,\"name\":\"%s\",\"kind\":\"%s\",\"dir\":\"%s\",\"charged\":%d,\"held_ticks\":%d,\"windup_ticks\":%d,"
					"\"cost_table\":%.2f,\"cost_paid\":%.2f,\"stamina_before\":%.2f,\"stamina_after\":%.2f,\"under_block\":%d,\"dx\":%.1f,\"dy\":%.1f}",
					(unsigned)ticks, f.swingId, p, familyName(mv->family), mv->id, mv->name, kindName(mv->kind), dirName(f.dir), useCharged ? 1 : 0, f.heldTicks, f.t,
					cost, f.costPaid, before, sta.value, f.underBlock ? 1 : 0, f.swDX, f.swDY);
				startActive(p, f, *mv);
				// fall into the first Active tick below
			}

			if ( f.st == State::Active )
			{
				const MoveRow* mv = moveById(f.moveId);
				if ( !mv ) { resetSwing(f); return; }
				if ( !f.objectChecked && f.t >= 2 ) { f.objectChecked = true; Sec t15(15); objectStrike(p, f, *mv); } // not on the tick of the first creature hit: it is the costliest one
				const double u0 = (double)f.t / f.activeTicks, u1 = (double)(f.t + 1) / f.activeTicks;
				const bool deferred = sweepStep(p, f, *mv, u0, std::min(1.0, u1));
				++f.t;
				if ( f.t >= f.activeTicks && !(deferred && f.t < f.activeTicks + 6) ) { f.st = State::Recovery; f.t = 0; gLast.hits = f.hitCount; gLast.done = true; }
				return;
			}

			if ( f.st == State::Recovery )
			{
				++f.t;
				if ( f.t >= f.recoverTicks )
				{
					sta.swingWhileBlock = false;
					sta.move = 0;
					f.st = State::Idle; f.t = 0; f.restValid = false; f.riposteSwing = f.chamberSwing = false;
					if ( !held ) { f.needRelease = false; }
				}
			}
		}
	} // namespace


	// =========================================================================================================
	// C2: block, timed block, riposte, chamber, dodge, feint, kick (04b section 6). Read from apx::in only.
	// =========================================================================================================
	bool blockOwned(int p)
	{
		if ( !gEnabled || p < 0 || p >= MAXPLAYERS || !players[p] || !players[p]->entity || !stats[p] ) { return false; }
		if ( !players[p]->isLocalPlayer() || multiplayer == CLIENT ) { return false; }
		Entity* me = players[p]->entity;
		return me->effectShapeshift == NOTHING && me->effectPolymorph == NOTHING;
	}
	bool bracing(int p) { return p >= 0 && p < MAXPLAYERS && gF[p].bracing && blockWanted(p); }
	bool blockWanted(int p)
	{
		if ( p < 0 || p >= MAXPLAYERS ) { return false; }
		const Fighter& f = gF[p];
		return f.blocking && (f.st == State::Idle || f.underBlock) && f.dodgeLeft == 0;
	}

	namespace
	{
		void defenseTick(int p, Fighter& f, const in::TickRecord& rec, Entity* me)
		{
			Stat* ms = stats[p];
			apx::Stamina_t& sta = apx::get(p);
			if ( f.riposteLeft > 0 ) { --f.riposteLeft; }
			if ( f.chamberLeft > 0 ) { --f.chamberLeft; }
			if ( f.dodgeLeft > 0 && --f.dodgeLeft == 0 )
			{
				row("{\"ev\":\"dodge_end\",\"tick\":%u,\"player\":%d,\"moved\":%.2f}", (unsigned)ticks, p, std::sqrt((me->x - f.dodgeX) * (me->x - f.dodgeX) + (me->y - f.dodgeY) * (me->y - f.dodgeY)));
			}
			if ( f.dodgeCool > 0 ) { --f.dodgeCool; }
			if ( f.kickLeft > 0 ) { --f.kickLeft; }
			if ( f.kickCool > 0 ) { --f.kickCool; }
			if ( f.feintLeft > 0 ) { --f.feintLeft; }
			const bool bHeld = rec.isHeld(in::B_Block), bPress = rec.pressCount[in::B_Block] > 0;
			bool feinted = false;
			// feint: Block pressed during a wind-up cancels the swing for 10 stamina
			if ( bPress && f.st == State::Windup && sta.value >= kFeintCost )
			{
				apx::spend(p, kFeintCost, 0);
				++gCount.feints; feinted = true;
				row("{\"ev\":\"feint\",\"tick\":%u,\"player\":%d,\"windup_ticks\":%d,\"cost\":%.1f,\"stamina\":%.2f}", (unsigned)ticks, p, f.t, kFeintCost, sta.value);
				apx::get(p).swingWhileBlock = false;
				resetSwing(f);
				f.st = State::Recovery; f.t = 0; f.recoverTicks = kFeintRecover; f.feintLeft = kFeintRecover;
				f.needRelease = rec.isHeld(in::B_Attack);
			}
			// block: a shield up while Block is held; the first 200 ms are the timed window
			const bool shield = ms->shield && !itemTypeIsQuiver(ms->shield->type);
			// a staff blocks without a shield (bracing, D64): cheaper, poise x1.5, no armour bonus
			const bool brace = !shield && famOf(ms) == FAM_STAFF;
			f.bracing = bHeld && brace && !feinted;
			f.blocking = bHeld && (shield || brace) && !feinted;
			if ( f.blocking && blockWanted(p) ) { f.blockT = f.blockT < 0 ? 0 : f.blockT + 1; }
			else { f.blockT = -1; }
			if ( f.blockT == 0 && f.probeMode == 0 && f.probeAt >= 0 && f.probeFire < 0 ) { f.probeFire = (int)ticks + f.probeAt; }
			if ( f.probeFire >= 0 && (int)ticks == f.probeFire )
			{
				Entity* att = nullptr;
				for ( Uint32 uid : gDummies ) { Entity* e = uidToEntity(uid); if ( e && e->getStats() && e->getStats()->HP > 0 ) { att = e; break; } }
				const int since = f.blockT;
				const int dmg = 10;
				const int before = ms->HP;
				const int out = onLegacyHit(att, me, dmg, PATH_MELEE);
				if ( out > 0 ) { me->modHP(-out); }
				row("{\"ev\":\"probe\",\"tick\":%u,\"since_press_ticks\":%d,\"since_press_ms\":%d,\"damage_in\":%d,\"damage_out\":%d,\"hp_before\":%d,\"hp_after\":%d,\"free\":%d}", (unsigned)ticks, since, since * kTickMs, dmg, out, before, ms->HP, out == 0 ? 1 : 0);
				f.probeAt = -1; f.probeFire = -1;
			}
			// dodge: 15 stamina, 150 ms invulnerable, an impulse through apx::move
			if ( rec.pressCount[in::B_Dodge] > 0 && f.dodgeCool == 0 && f.dodgeLeft == 0 && f.st != State::Active && sta.value >= kDodgeCost )
			{
				apx::spend(p, kDodgeCost, 0);
				++gCount.dodges;
				const double yaw = me->yaw;
				double dx = std::cos(yaw) * rec.moveY + std::cos(yaw + PI / 2) * rec.moveX, dy = std::sin(yaw) * rec.moveY + std::sin(yaw + PI / 2) * rec.moveX;
				const double l = std::sqrt(dx * dx + dy * dy);
				if ( l < 0.1 ) { dx = -std::cos(yaw); dy = -std::sin(yaw); } else { dx /= l; dy /= l; } // no direction held: a step back
				const double x0 = me->x, y0 = me->y;
				apx::move::impulse(me, (float)(dx * 3.0), (float)(dy * 3.0), kDodgeTicks);
				f.dodgeLeft = kDodgeTicks; f.dodgeCool = kDodgeCooldown; f.dodgeX = x0; f.dodgeY = y0;
				if ( f.probeMode == 1 && f.probeAt >= 0 ) { f.probeFire = (int)ticks + f.probeAt; }
				if ( f.st == State::Windup ) { apx::get(p).swingWhileBlock = false; resetSwing(f); }
				row("{\"ev\":\"dodge\",\"tick\":%u,\"player\":%d,\"cost\":%.1f,\"invulnerable_ms\":%d,\"dir\":[%.2f,%.2f],\"stamina\":%.2f,\"x\":%.2f,\"y\":%.2f}", (unsigned)ticks, p, kDodgeCost, kDodgeTicks * kTickMs, dx, dy, sta.value, x0, y0);
			}
			// kick: a cheap shove, no shield needed
			if ( rec.pressCount[in::B_Kick] > 0 && f.kickCool == 0 && (f.st == State::Idle || f.st == State::Recovery) )
			{
				const MoveRow* kv = moveById(80);
				const double cost = kv ? apx::kSwingCost * kv->staminaMult : 5.0;
				if ( kv && sta.value >= cost )
				{
					apx::spend(p, cost, 0);
					f.kickLeft = msToTicks(kv->activeMs + kv->recoverMs); f.kickCool = kKickCooldown; ++gCount.kicks;
					Entity* best = nullptr; double bd = 1e9;
					for ( node_t* node = map.creatures ? map.creatures->first : nullptr; node; node = node->next )
					{
						Entity* e = (Entity*)node->element;
						if ( e == me || !isCreature(e) || !e->getStats() || e->getStats()->HP <= 0 ) { continue; }
						const double dx = e->x - me->x, dy = e->y - me->y, d = std::sqrt(dx * dx + dy * dy);
						if ( d > STRIKERANGE * kv->reach + std::max(e->sizex, e->sizey) || std::fabs(wrapPi(std::atan2(dy, dx) - me->yaw)) > 0.6 ) { continue; }
						if ( (svFlags & SV_FLAG_FRIENDLYFIRE) == 0 && me->checkFriend(e) && me->friendlyFireProtection(e) ) { continue; }
						if ( d < bd ) { bd = d; best = e; }
					}
					int dmg = -1;
					if ( best )
					{
						Hit h; h.attacker = me; h.target = best; h.moveId = 80; h.zoneHint = (int)combat::Zone::Legs;
						dmg = applyHit(h).damage;
					}
					row("{\"ev\":\"kick\",\"tick\":%u,\"player\":%d,\"cost\":%.2f,\"hit\":%d,\"dmg\":%d,\"stamina\":%.2f}", (unsigned)ticks, p, cost, best ? 1 : 0, dmg, sta.value);
				}
			}
		}
	} // namespace


	// =========================================================================================================
	// C3: creature melee on the core. The monster AI (actMonster) still chooses when and whom; its wind-up animation is the telegraph
	// (limbs raise, a whoosh sounds); the strike call that used to run inside Entity::attack comes here, waits until the telegraph has
	// lasted 300 ms, and resolves through the same applyHit as a player's blow, against what apx::hostility says is a foe.
	// =========================================================================================================
	namespace
	{
		struct WindupRec { Uint32 tick; int pose; };
		struct PendingStrike { Uint32 uid, due, windup; int pose, charge; };
		std::unordered_map<Uint32, WindupRec> gWind;
		std::vector<PendingStrike> gPending;
		Uint32 gFoeUid = 0;

		bool creatureOwned(Entity* e)
		{
			if ( !gEnabled || !e || e->behavior != &actMonster ) { return false; }
			Stat* s = e->getStats();
			if ( !s || e->isBossMonster() || s->type == MIMIC || s->type == MINIMIMIC || s->type == SHOPKEEPER ) { return false; }
			return e->monsterAttack >= MONSTER_POSE_MELEE_WINDUP1 && e->monsterAttack <= MONSTER_POSE_MELEE_WINDUP3;
		}

		void executeStrike(const PendingStrike& ps)
		{
			Entity* e = uidToEntity(ps.uid);
			if ( !e || !e->getStats() || e->getStats()->HP <= 0 || !e->isMobile() ) { return; }
			// the strike animation starts now (Entity::attack did this at the strike call): the weapon arm swings as the blow resolves
			{
				Stat* es = e->getStats();
				e->monsterAttack = (es->weapon != nullptr || es->type == CRYSTALGOLEM || es->type == COCKATRICE || es->type == EARTH_ELEMENTAL) ? ps.pose : 1;
				e->monsterAttackTime = 0;
			}
			Entity* target = uidToEntity(e->monsterTarget);
			const int delayMs = (int)(((Uint32)ticks - ps.windup) * kTickMs);
			if ( !target || !isCreature(target) || !target->getStats() || target->getStats()->HP <= 0 )
			{
				// no creature to strike: a door, a barrier or air: Barony's object code (creatures masked out)
				gObjectPass = true; e->attack(ps.pose, ps.charge, nullptr); gObjectPass = false;
				return;
			}
			apx::hostility::Reason why;
			const bool foe = apx::hostility::hostile(e, target, &why);
			const double dx = target->x - e->x, dy = target->y - e->y, d = std::sqrt(dx * dx + dy * dy);
			const double r = std::max(target->sizex, target->sizey);
			const bool inReach = d <= STRIKERANGE + r + 2.0 && std::fabs(wrapPi(std::atan2(dy, dx) - e->yaw)) < 0.9;
			// line of sight as Barony's own strike did: a trace along the facing that must reach the target first (walls, doors and bodies in the way stop it)
			bool los = false;
			if ( foe && inReach )
			{
				const hit_t savedHit = hit;
				lineTrace(e, e->x, e->y, std::atan2(dy, dx), STRIKERANGE + r + 4.0, LINETRACE_ATK_CHECK_FRIENDLYFIRE, false);
				los = hit.entity == target;
				hit = savedHit;
			}
			if ( !foe || !inReach || !los )
			{
				++gCount.creatureMisses;
				row("{\"ev\":\"creature_miss\",\"tick\":%u,\"attacker\":%u,\"type\":%d,\"foe\":%d,\"reason\":\"%s\",\"in_reach\":%d,\"dist\":%.1f}", (unsigned)ticks, e->getUID(), (int)e->getStats()->type, foe ? 1 : 0, why.cls, inReach ? 1 : 0, d);
				return;
			}
			Hit h;
			h.attacker = e; h.target = target;
			const int hpBefore = target->getStats()->HP;
			const HitResult res = applyHit(h);
			++gCount.creatureStrikes;
			row("{\"ev\":\"creature_strike\",\"tick\":%u,\"attacker\":%u,\"type\":%d,\"target\":%u,\"target_is_player\":%d,\"telegraph_ms\":%d,\"dmg\":%d,\"blocked\":%d,\"hp_before\":%d,\"hp_after\":%d,\"zone\":%d,\"why\":\"%s\"}",
				(unsigned)ticks, e->getUID(), (int)e->getStats()->type, target->getUID(), target->behavior == &actPlayer ? 1 : 0, delayMs, res.damage, res.blocked ? 1 : 0, hpBefore, res.hpAfter, res.zone, why.cls);
		}
	} // namespace

	void onCreatureWindup(Entity* e, int pose)
	{
		if ( !gEnabled || !e || e->behavior != &actMonster || pose < MONSTER_POSE_MELEE_WINDUP1 || pose > MONSTER_POSE_MELEE_WINDUP3 ) { return; }
		Stat* s = e->getStats();
		if ( !s || e->isBossMonster() ) { return; }
		gWind[e->getUID()] = { (Uint32)ticks, pose };
		playSoundEntity(e, 23 + local_rng.rand() % 5, 140); // the telegraph sound: the whoosh of the raised weapon, loud enough to react to
		row("{\"ev\":\"telegraph\",\"tick\":%u,\"attacker\":%u,\"type\":%d,\"pose\":%d}", (unsigned)ticks, e->getUID(), (int)s->type, pose);
	}

	bool creatureStrike(Entity* e, int pose, int charge)
	{
		if ( !creatureOwned(e) ) { return false; }
		for ( const PendingStrike& q : gPending ) { if ( q.uid == e->getUID() ) { return true; } } // the animation calls again every tick until it is advanced
		Uint32 windup = (Uint32)ticks, due = (Uint32)ticks;
		auto it = gWind.find(e->getUID());
		if ( it != gWind.end() ) { windup = it->second.tick; due = std::max<Uint32>(due, windup + kMinTelegraphTicks); gWind.erase(it); }
		gPending.push_back({ e->getUID(), due, windup, pose, charge });
		return true;
	}

	void creatureTick()
	{
		if ( gFoeUid && ticks % 50 == 0 && players[0] && players[0]->entity )
		{
			Entity* f = uidToEntity(gFoeUid);
			if ( f ) { row("{\"ev\":\"foe_state\",\"tick\":%u,\"state\":%d,\"target\":%u,\"dist\":%.1f,\"hp\":%d,\"attack\":%d,\"mobile\":%d}", (unsigned)ticks, (int)f->monsterState, f->monsterTarget, std::hypot(f->x - players[0]->entity->x, f->y - players[0]->entity->y), f->getStats() ? f->getStats()->HP : -1, (int)f->monsterAttack, (int)f->isMobile()); }
		}
		if ( gPending.empty() ) { return; }
		for ( size_t i = 0; i < gPending.size(); )
		{
			if ( gPending[i].due > (Uint32)ticks ) { ++i; continue; }
			if ( i > 0 && tickElapsedMs() > kHitBudgetMs ) { break; } // the rest next tick
			const PendingStrike ps = gPending[i];
			gPending.erase(gPending.begin() + i);
			executeStrike(ps);
		}
		if ( gWind.size() > 4096 ) { gWind.clear(); }
	}

	static ConsoleCommand ccmd_apxcombat_foe("/apxcombat_foe", "apx: <goblin|skeleton|rat|troll> -- a creature two tiles ahead that hunts the player (the C3 telegraph gate)", [](int argc, const char** argv) {
		if ( !players[0] || !players[0]->entity ) { return; }
		Entity* me = players[0]->entity;
		Monster t = GOBLIN;
		if ( argc > 1 ) { if ( !strcmp(argv[1], "skeleton") ) { t = SKELETON; } else if ( !strcmp(argv[1], "rat") ) { t = RAT; } else if ( !strcmp(argv[1], "troll") ) { t = TROLL; } }
		clearDummies(); // the test cluster would wall the creature off
		Entity* m = nullptr;
		for ( int i = 0; i < 16 && !m; ++i ) { const double a = me->yaw + (i % 8) * (PI / 4), d = 26.0 + 8.0 * (i / 8); m = summonMonster(t, me->x + cos(a) * d, me->y + sin(a) * d, false); }
		if ( !m || !m->getStats() ) { row("{\"ev\":\"foe\",\"made\":0}"); return; }
		gFoeUid = m->getUID();
		m->getStats()->HP = m->getStats()->MAXHP = 9999;
		m->monsterAcquireAttackTarget(*me, MONSTER_STATE_PATH, false);
		row("{\"ev\":\"foe\",\"made\":1,\"type\":%d,\"uid\":%u}", (int)t, m->getUID());
	});
	static ConsoleCommand ccmd_apxcombat_probe("/apxcombat_probe", "apx: <ticks> -- on the next block press a creature hit lands that many ticks later; logs whether it was free (the timed-block gate)", [](int argc, const char** argv) {
		if ( argc > 1 ) { gF[0].probeAt = std::max(0, std::atoi(argv[1])); gF[0].probeFire = -1; gF[0].probeMode = (argc > 2 && !strcmp(argv[2], "dodge")) ? 1 : 0; }
	});
	static ConsoleCommand ccmd_apxcombat_envenom("/apxcombat_envenom", "apx: envenom the wielded weapon for 10 minutes (the poison proc probe)", [](int, const char**) {
		if ( !players[0] || !players[0]->entity || !stats[0] ) { return; }
		players[0]->entity->setEffect(EFF_ENVENOM_WEAPON, true, 600 * TICKS_PER_SECOND, false);
		row("{\"ev\":\"envenom\",\"active\":%d}", (int)stats[0]->getEffectActive(EFF_ENVENOM_WEAPON));
	});
	static ConsoleCommand ccmd_apxcombat_hp("/apxcombat_hp", "apx: <n> -- set the player HP and max HP (QA)", [](int argc, const char** argv) {
		if ( argc > 1 && stats[0] ) { stats[0]->MAXHP = stats[0]->HP = std::atoi(argv[1]); }
	});

	void tickPlayer(int p)
	{
		if ( p < 0 || p >= MAXPLAYERS || !players[p] ) { return; }
		const auto t0 = std::chrono::steady_clock::now();
		const double log0 = gLogMs;
		gTickT0 = t0; gTickLogBase = gLogMs;
		if ( gCreatureTick != (Uint32)ticks ) { gCreatureTick = (Uint32)ticks; creatureTick(); }
		const in::TickRecord& rec = in::take(p); // always take: nothing accumulates while the core is idle
		Fighter& f = gF[p];
		apx::Stamina_t& sta = apx::get(p);
		if ( !players[p]->entity || !stats[p] ) { resetSwing(f); return; }
		Entity* me = players[p]->entity;
		if ( gEnabled && players[p]->isLocalPlayer() ) { apx::tick(p, 0, false, stats[p]->defending); } // stamina: regen, block drain, net (was the HUD's)
		if ( blockOwned(p) && stats[p]->HP > 0 && !players[p]->movement.isPlayerSwimming() && me->isMobile() ) { defenseTick(p, f, rec, me); } else { f.blocking = false; f.blockT = -1; f.dodgeLeft = 0; }
		const bool own = ownsMelee(p) && stats[p]->HP > 0;
		if ( !own )
		{
			if ( f.st != State::Idle ) { sta.swingWhileBlock = false; resetSwing(f); }
			sta.prevChop = false;
			return;
		}
		// stagger, paralysis or downed: the swing is lost
		// vanilla: the HUD weapon returns before its swing check while the player swims (water, lava): no attacking then
		const bool swimming = players[p]->movement.isPlayerSwimming() || me->skill[13] != 0;
		if ( swimming || !me->isMobile() || !apx::combat_glue::canAttack(me) )
		{
			if ( f.st == State::Windup || f.st == State::Active ) { row("{\"ev\":\"cancel\",\"tick\":%u,\"player\":%d,\"state\":\"%s\",\"mobile\":%d,\"can_attack\":%d,\"skill9\":%d,\"paralyzed\":%d,\"hp\":%d}", (unsigned)ticks, p, stateName(f.st), (int)me->isMobile(), (int)apx::combat_glue::canAttack(me), (int)me->skill[9], (int)stats[p]->getEffectActive(EFF_PARALYZED), (int)stats[p]->HP); }
			if ( f.st != State::Idle ) { sta.swingWhileBlock = false; resetSwing(f); }
			f.needRelease = rec.isHeld(in::B_Attack);
			sta.prevChop = false;
			return;
		}
		qaOnce(p);
		controlTick(p, f, rec);
		sta.prevChop = swinging(p);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() - (gLogMs - log0) + gLegacyMs;
		gLegacyMs = 0.0;
		gPerf.all.push_back((float)ms);
		if ( f.st == State::Active ) { gPerf.active.push_back((float)ms); }
		if ( gPerf.all.size() >= 2000 ) { perfDump(); }
	}

	// =========================================================================================================
	// first-person pose
	// =========================================================================================================
	namespace
	{
		struct Pose { double v[6]; }; // x, y, z, yaw, pitch, roll (hud weapon offsets)
		inline double ease(double t) { t = std::max(0.0, std::min(1.0, t)); return t * t * (3.0 - 2.0 * t); }
		inline double easeOut(double t) { t = std::max(0.0, std::min(1.0, t)); return 1.0 - (1.0 - t) * (1.0 - t); }

		// wind-up and strike keyframes per direction (Barony's chop: X -1 Y -2 pitch -PI/4 back, X 4 pitch 3PI/4 through)
		void keyframes(int dir, int fam, Pose& W, Pose& S)
		{
			switch ( dir )
			{
				case DIR_OVERHEAD: W = { { -1, -2, -6, 0, -0.785, 0 } }; S = { { 4, -1, 0, 0, 2.356, 0 } }; break;
				case DIR_LEFT:     W = { { 0, 3, -5, -0.5, -0.2, 0.6 } }; S = { { 3, -5, -3, 0.7, 0.0, -0.6 } }; break;
				case DIR_RIGHT:    W = { { 0, -5, -5, 0.5, -0.2, -0.6 } }; S = { { 3, 3, -3, -0.7, 0.0, 0.6 } }; break;
				case DIR_LOW:      W = { { -1, 1, -8, 0, 0.7, 0 } }; S = { { 5, 0, -2, 0, -0.8, 0 } }; break;
				default:           W = { { -3, -2, -5, 0, -0.1, 0 } }; S = { { 7, -1, -3, 0, 0.35, 0 } }; break; // thrust
			}
			if ( fam == FAM_MACE || fam == FAM_AXE ) { W.v[2] -= 1; S.v[0] += 0; }
		}
	} // namespace

	bool poseHud(int p, double& mx, double& my, double& mz, double& yaw, double& pitch, double& roll)
	{
		if ( p < 0 || p >= MAXPLAYERS ) { return false; }
		Fighter& f = gF[p];
		if ( f.st != State::Windup && f.st != State::Active && f.st != State::Recovery ) { f.restValid = false; return false; }
		if ( !f.restValid )
		{
			f.rest[0] = mx; f.rest[1] = my; f.rest[2] = mz; f.rest[3] = yaw; f.rest[4] = pitch; f.rest[5] = roll;
			f.restValid = true;
		}
		const MoveRow* mv = f.moveId ? moveById(f.moveId) : nullptr;
		int dir = f.dir;
		int fam = f.fam;
		bool charged = f.charged;
		if ( mv ) { charged = mv->charged; }
		Pose W, S;
		keyframes(charged ? DIR_OVERHEAD : dir, fam, W, S);
		if ( charged && fam != FAM_MACE && fam != FAM_AXE && fam != FAM_STAFF ) { keyframes(DIR_THRUST, fam, W, S); W.v[0] -= 2; S.v[0] += 1; }
		if ( f.st == State::Windup && !f.dirLocked ) { keyframes(DIR_THRUST, fam, W, S); } // the direction is still being chosen: a neutral draw-back
		double out[6];
		if ( f.st == State::Windup )
		{
			const int wt = std::max(1, f.quickTicks > 0 ? f.quickTicks : 12);
			const double k = easeOut((double)f.t / wt);
			for ( int i = 0; i < 6; ++i ) { out[i] = f.rest[i] + (W.v[i] - f.rest[i]) * k; }
			if ( f.charged ) // a held charge trembles
			{
				const double tr = 0.25 * std::sin((double)ticks * 1.7);
				out[1] += tr; out[2] += 0.3 * std::sin((double)ticks * 2.3);
			}
		}
		else if ( f.st == State::Active )
		{
			const double k = ease(((double)f.t + 0.5) / std::max(1, f.activeTicks));
			for ( int i = 0; i < 6; ++i ) { out[i] = W.v[i] + (S.v[i] - W.v[i]) * k; }
		}
		else
		{
			const double k = ease((double)f.t / std::max(1, f.recoverTicks));
			for ( int i = 0; i < 6; ++i ) { out[i] = S.v[i] + (f.rest[i] - S.v[i]) * k; }
		}
		mx = out[0]; my = out[1]; mz = out[2]; yaw = out[3]; pitch = out[4]; roll = out[5];
		for ( int i = 0; i < 6; ++i ) { f.poseOut[i] = out[i]; }
		return true;
	}

	// =========================================================================================================
	// arena (QA and self-test): a weapon family in hand, creatures around the player
	// =========================================================================================================
	namespace
	{
		void equipFamily(int p, int fam)
		{
			Stat* st = stats[p];
			ItemType wt = (ItemType)-1;
			switch ( fam )
			{
				case FAM_SWORD: wt = STEEL_SWORD; break;
				case FAM_AXE: wt = IRON_AXE; break;
				case FAM_MACE: wt = IRON_MACE; break;
				case FAM_SPEAR: wt = IRON_SPEAR; break;
				case FAM_STAFF: wt = QUARTERSTAFF; break;
				case FAM_DAGGER: wt = RAPIER; break;
				default: break;
			}
			st->weapon = nullptr;
			st->shield = nullptr;
			if ( wt != (ItemType)-1 )
			{
				Item* w = newItem(wt, SERVICABLE, 0, 1, 0, true, &st->inventory);
				equipItem(w, &st->weapon, p, false);
			}
			if ( fam == FAM_SHIELD )
			{
				Item* sh = newItem(WOODEN_SHIELD, SERVICABLE, 0, 1, 0, true, &st->inventory);
				equipItem(sh, &st->shield, p, false);
			}
			row("{\"ev\":\"equip\",\"fam\":\"%s\",\"resolved\":\"%s\",\"weapon\":%d}", familyName(fam), familyName(famOf(st)), st->weapon ? (int)st->weapon->type : -1);
		}

		// QA: the floor's own monsters would wander in and stagger the player mid-swing; the arena is calm (the crowd the gate asks for fights on purpose)
		void calmFloor()
		{
			int cleared = 0;
			for ( node_t* node = map.creatures ? map.creatures->first : nullptr; node; node = node->next )
			{
				Entity* e = (Entity*)node->element;
				if ( !e || e->behavior != &actMonster || !e->getStats() || e->getStats()->HP <= 0 ) { continue; }
				if ( e->monsterAllyGetPlayerLeader() ) { continue; }
				e->getStats()->HP = 0;
				++cleared;
			}
			row("{\"ev\":\"calm\",\"cleared\":%d}", cleared);
		}

		void clearDummies()
		{
			for ( Uint32 uid : gDummies ) { Entity* e = uidToEntity(uid); if ( e && e->getStats() ) { e->getStats()->HP = 0; } }
			gDummies.clear();
		}

		// five in an arc at 0.6 reach and two behind them in the line (lines, run-throughs), as the F4d script placed them
		void spawnTestCluster(int p)
		{
			Entity* me = players[p]->entity;
			struct P { double ang, dist; };
			const P pos[7] = { { -0.7, 14.4 }, { -0.35, 14.4 }, { 0.0, 14.4 }, { 0.35, 14.4 }, { 0.7, 14.4 }, { 0.0, 22.0 }, { 0.0, 29.0 } };
			for ( const P& q : pos )
			{
				Entity* d = summonMonster(DUMMYBOT, me->x + cos(me->yaw + q.ang) * q.dist, me->y + sin(me->yaw + q.ang) * q.dist, true);
				if ( !d || !d->getStats() ) { continue; }
				d->getStats()->HP = d->getStats()->MAXHP = 9999;
				gDummies.push_back(d->getUID());
			}
		}

		// n creatures on rings around the player (perf and crowd QA): rats, goblins, skeletons
		int spawnCrowd(int p, int n, bool still)
		{
			Entity* me = players[p]->entity;
			const Monster kinds[3] = { RAT, GOBLIN, SKELETON };
			int made = 0;
			for ( int i = 0; i < n * 3 && made < n; ++i )
			{
				const double ring = 20.0 + 14.0 * (i / 12);
				const double ang = (i % 12) * (2 * PI / 12) + 0.17 * (i / 12);
				Entity* d = summonMonster(still ? DUMMYBOT : kinds[made % 3], me->x + cos(ang) * ring, me->y + sin(ang) * ring, false);
				if ( !d || !d->getStats() ) { continue; }
				d->getStats()->HP = d->getStats()->MAXHP = 9999;
				d->monsterState = MONSTER_STATE_WAIT; // stand and do not fight: the gate measures the core, not the AI
				gDummies.push_back(d->getUID());
				++made;
			}
			return made;
		}

		void qaOnce(int p)
		{
			static bool done = false;
			if ( done || p != 0 ) { return; }
			const char* qa = std::getenv("APX_COMBAT_QA");
			if ( !qa || !qa[0] ) { done = true; return; }
			if ( ticks < 60 ) { return; }
			done = true;
			std::string s = qa;
			int crowd = 0;
			const size_t colon = s.find(':');
			bool crowdStill = false;
			if ( colon != std::string::npos ) { crowd = std::atoi(s.c_str() + colon + 1); crowdStill = s.find(":still") != std::string::npos; s = s.substr(0, colon); }
			int fam = FAM_SWORD;
			for ( int i = 1; i < FAM_COUNT; ++i ) { if ( s == familyName(i) ) { fam = i; } }
			stats[p]->HP = stats[p]->MAXHP = std::max(stats[p]->MAXHP, 999);
			equipFamily(p, fam);
			calmFloor();
			spawnTestCluster(p);
			const int made = crowd > 0 ? spawnCrowd(p, crowd, crowdStill) : 0;
			row("{\"ev\":\"qa_setup\",\"fam\":\"%s\",\"cluster\":%zu,\"crowd_asked\":%d,\"crowd_made\":%d}", familyName(fam), gDummies.size(), crowd, made);
		}
	} // namespace

	static ConsoleCommand ccmd_apxcombat_arena("/apxcombat_arena", "apx: <family|none> [crowd n] -- a weapon family in hand, a test cluster and a crowd around you", [](int argc, const char** argv) {
		if ( !players[0] || !players[0]->entity ) { return; }
		int fam = FAM_SWORD;
		if ( argc > 1 ) { for ( int i = 1; i < FAM_COUNT; ++i ) { if ( !strcmp(argv[1], familyName(i)) ) { fam = i; } } }
		equipFamily(0, fam);
		clearDummies();
		calmFloor();
		spawnTestCluster(0);
		if ( argc > 2 ) { stats[0]->MAXHP = std::max(stats[0]->MAXHP, 9999); stats[0]->HP = stats[0]->MAXHP; } // a crowd that fights back must not end the measurement
		const int made = argc > 2 ? spawnCrowd(0, std::atoi(argv[2]), argc > 3 && !strcmp(argv[3], "still")) : 0;
		row("{\"ev\":\"arena\",\"fam\":\"%s\",\"cluster\":%zu,\"crowd\":%d}", familyName(fam), gDummies.size(), made);
	});
	static ConsoleCommand ccmd_apxcombat_perf("/apxcombat_perf", "apx: write the combat core's per-tick cost (p50/p99) to the log", [](int, const char**) { perfDump(); });

	// =========================================================================================================
	// self-test: the "moves" stage of -apxselftest=all, through the core with real latching (synthetic SDL events into apx::in)
	// =========================================================================================================
	namespace
	{
		void fakeButton(bool down)
		{
			in::Raw e; e.type = down ? in::R_MouseDown : in::R_MouseUp; e.code = 1; e.tsMs = SDL_GetTicks();
			in::event(e);
		}
		void fakeMotion(int dx, int dy)
		{
			in::Raw e; e.type = in::R_MouseMove; e.dx = (float)dx; e.dy = (float)dy; e.tsMs = SDL_GetTicks();
			in::event(e);
		}
		struct SelfResult { int fam, moveId, dir; bool charged; int hits; double cost, table; bool ok; };
		std::vector<SelfResult> gSelfResults;
		Uint32 gSelfT = 0;
		bool gSelfDone = false;
		int gSelfHP = 0, gSelfMaxHP = 0;
		constexpr Uint32 kBlock = 700;
	}

	int selftestStep()
	{
		if ( gSelfDone ) { return 1; }
		if ( !players[0] || !players[0]->entity ) { return 0; }
		Entity* me = players[0]->entity;
		if ( gSelfT == 0 ) { gSelfHP = stats[0]->HP; gSelfMaxHP = stats[0]->MAXHP; }
		++gSelfT;
		const Uint32 t = gSelfT;
		const int famCount = FAM_COUNT - 1;
		const int block = (int)((t - 1) / kBlock);
		const Uint32 o = (t - 1) % kBlock;
		if ( block >= famCount )
		{
			int bad = 0;
			for ( const SelfResult& r : gSelfResults ) { if ( !r.ok ) { ++bad; } }
			const bool pass = bad == 0 && (int)gSelfResults.size() == famCount * 5;
			char buf[256];
			snprintf(buf, sizeof buf, "\"swings\":%d,\"failed\":%d,\"expected\":%d,\"core\":1", (int)gSelfResults.size(), bad, famCount * 5);
			row("{\"ev\":\"selftest_moves\",\"pass\":%d,%s}", pass ? 1 : 0, buf);
			gSelfDone = true;
			clearDummies();
			stats[0]->MAXHP = gSelfMaxHP; stats[0]->HP = gSelfHP; // the test character comes back as it was
			selftestall::done(selftestall::MOVES, pass, buf);
			return pass ? 1 : -1;
		}
		const int fam = block + 1;
		apx::Stamina_t& sta = apx::get(0);
		stats[0]->HP = stats[0]->MAXHP = std::max(stats[0]->MAXHP, 9999);
		static double homeX = 0, homeY = 0, homeYaw = 0;
		if ( gSelfT == 1 ) { homeX = me->x; homeY = me->y; homeYaw = me->yaw; }
		if ( o == 0 ) { clearDummies(); equipFamily(0, fam); me->x = homeX; me->y = homeY; me->yaw = homeYaw; me->vel_x = me->vel_y = 0; } // dashes and pushes are impulses now: every family starts from the same spot
		if ( o == 20 ) { spawnTestCluster(0); }
		// the swings: default, left, overhead, low (mouse motion in the window), charged (held 26 ticks)
		struct S { Uint32 at; int dx, dy; int hold; };
		static const S script[5] = { { 60, 0, 0, 3 }, { 170, -9, 0, 3 }, { 280, 0, -9, 3 }, { 390, 0, 9, 3 }, { 520, 0, 0, 26 } };
		for ( int k = 0; k < 5; ++k )
		{
			const S& s = script[k];
			if ( o == s.at - 1 ) { me->yaw = homeYaw; sta.value = sta.maxValue; sta.lastSpendTick = 0; gLast = LastSwing(); }
			if ( o == s.at ) { fakeButton(true); }
			if ( o == s.at + 1 && (s.dx || s.dy) ) { fakeMotion(s.dx, s.dy); }
			if ( o == s.at + (Uint32)s.hold ) { fakeButton(false); }
			if ( o == s.at + 100 )
			{
				const MoveRow* mv = moveById(gLast.moveId);
				const int want = moveFor(fam, k == 0 ? DIR_THRUST : (k == 1 ? DIR_LEFT : (k == 2 ? DIR_OVERHEAD : (k == 3 ? DIR_LOW : DIR_THRUST))), k == 4);
				bool ok = mv && gLast.moveId == want && gLast.done && gLast.hits >= 1 && std::fabs(gLast.cost - gLast.table) < 0.01 && std::fabs(gLast.table - apx::kSwingCost * mv->staminaMult) < 0.01;
				if ( mv && mv->extra > 0 ) { ok = ok && gLast.hits >= 2; }
				if ( mv && mv->extra == 0 ) { ok = ok && gLast.hits == 1; }
				gSelfResults.push_back({ fam, gLast.moveId, gLast.dir, gLast.charged, gLast.hits, gLast.cost, gLast.table, ok });
				row("{\"ev\":\"selftest_swing\",\"fam\":\"%s\",\"k\":%d,\"want_move\":%d,\"move\":%d,\"name\":\"%s\",\"dir\":\"%s\",\"charged\":%d,\"hits\":%d,\"extra_allowed\":%d,\"cost\":%.2f,\"table\":%.2f,\"ok\":%d}",
					familyName(fam), k, want, gLast.moveId, mv ? mv->name : "?", dirName(gLast.dir), gLast.charged ? 1 : 0, gLast.hits, mv ? mv->extra : -1, gLast.cost, gLast.table, ok ? 1 : 0);
			}
		}
		return 0;
	}
} // namespace combat
} // namespace apx
