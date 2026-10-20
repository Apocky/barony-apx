// apx/combat/core.hpp -- D93 combat core (spec 04b section 6, phase C1): one state machine for every fighter, player
// control from apx::in, swept hits, and ONE damage entry point, applyHit(), which carries the impact rule, zones,
// armour and the re-hosted rule tables (tier D74, poise D75, stamina D68/D64, F4c zones).
//
// Data: data/apx/combat/moves.json -> moves_table.inc (tools/apx/gen_moves_inc.py).
// Knobs: APX_COMBAT=0 or APX_COMBAT_OLD=1 = CONTROL (Barony's player melee path and the old hook sites, as before D93).
// Telemetry: "[apx-combat]" log lines; APX_COMBAT_JSONL=<file> appends one JSON row per event.
#pragma once

#include "impact.hpp"
#include "../../apx_combat_units.hpp"

class Entity;
class Stat;

namespace apx
{
namespace combat
{
	// ---- fighters ------------------------------------------------------------------------------------------
	enum class State : unsigned char
	{
		Idle, Windup, Active, Recovery, Block, TimedBlock, Riposte, Chamber, Dodge, Feint, Kick, Bash, Stagger, Knockdown, Downed
	};
	const char* stateName(State s);

	enum Dir : int { DIR_THRUST = 0, DIR_LEFT, DIR_RIGHT, DIR_OVERHEAD, DIR_LOW, NUM_DIRS };
	const char* dirName(int d);
	enum Family : int { FAM_NONE = 0, FAM_SWORD, FAM_AXE, FAM_MACE, FAM_SPEAR, FAM_STAFF, FAM_DAGGER, FAM_UNARMED, FAM_SHIELD, FAM_COUNT };
	const char* familyName(int f);

	// after-hit effects (bit mask in a move row)
	enum Effect : unsigned
	{
		EF_NONE = 0, EF_SHIELD_ASIDE = 1u << 0, EF_SHIELD_BREAK = 1u << 1, EF_SLAM = 1u << 2, EF_PUSH = 1u << 3,
		EF_KNOCKDOWN_SMALL = 1u << 4, EF_CRIT_BLEED = 1u << 5, EF_THROW = 1u << 6, EF_SNEAK = 1u << 7, EF_DASH = 1u << 8, EF_GRAPPLE = 1u << 9,
	};

	// one row of data/apx/combat/moves.json (order = the generated table's)
	struct MoveRow
	{
		int id, family;
		const char* name;
		int kind;                       // Kind: IMPACT, SHEAR, PIERCE
		int windupMs, activeMs, recoverMs;
		float arcHalf;                  // radians, half angle of the swept sector; 0 = a line
		float reach;                    // multiple of STRIKERANGE
		int extra;                      // extra targets after the first
		float falloff1, falloff2, damage, staminaMult;
		int heavyPct;
		unsigned effects;
		float armourIgnore, force;
		int knockdownTicks;
		float radius, dash, sneakMult;
		bool charged;
		float momentum, area;           // kg m/s, cm^2: the impact rule's inputs
		int poise;                      // poise damage to the target
		int zone;                       // Zone or -1 (auto: from pitch)
		int sound;                      // whoosh sound base
	};
	const MoveRow* moveById(int id);
	int moveCount();
	const MoveRow& moveAt(int i);
	int moveFor(int family, int dir, bool charged);   // the table's pick; 0 when the family has no moves

	// ---- the single damage entry point ---------------------------------------------------------------------
	enum HitFlags : unsigned
	{
		HF_PRECOMPUTED = 1u << 0,   // Hit::damage is the engine's damage (creature melee, arrows, thrown until C3/C4): only the rule stages run
		HF_NO_ZONES = 1u << 1,      // skip zones and poise (arrows and thrown weapons: tier only)
		HF_EXTRA_TARGET = 1u << 2,  // not the first target of a swing (falloff)
		HF_CHARGED = 1u << 3,
		HF_DRY = 1u << 4,           // run the rule stages and return the damage; the caller deals it (the Barony hit sites, the self-tests)
	};
	enum Path : int { PATH_MELEE = 0, PATH_ARROW = 1, PATH_THROWN = 2 };

	struct Hit
	{
		Entity* attacker = nullptr;
		Entity* target = nullptr;
		int moveId = 0;             // the move that struck (0: none)
		int kind = IMPACT;          // Kind (used when moveId is 0)
		float momentum = 0.f;       // 0 = from the move row
		float area = 0.f;
		int zoneHint = -1;          // Zone, or -1 = auto
		int element = 0;            // 0 physical; elements arrive with apx::magic
		int poise = -1;             // -1 = the move's / weapon class's
		unsigned flags = 0;
		int damage = 0;             // HF_PRECOMPUTED only
		int extraIndex = 0;         // HF_EXTRA_TARGET: 0 = first extra target
		int path = PATH_MELEE;
		int dir = DIR_THRUST;
	};
	struct HitResult
	{
		bool landed = false;        // the blow reached the target (false: ignored, dead target)
		int damage = 0;             // HP taken
		bool killed = false, staggered = false, fracture = false, blocked = false, sneak = false;
		int zone = -1;
		float multiplier = 1.f;     // the impact-rule multiplier applied
		int hpBefore = 0, hpAfter = 0;
	};
	HitResult applyHit(const Hit& h);

	// Barony's melee rule code that the old Entity::attack ran inline (artifact multipliers, weapon procs, poison, paralyze, slow,
	// knockback, bleed, armour and weapon wear, shield and weapon skill gain, compendium and achievements), kept verbatim in
	// procs_*.inc and run by the core through Entity::apxMeleePre1/2 and apxMeleeAftermath (procs.cpp). Fields carry the locals between them.
	struct MeleeCtx
	{
		Entity* target = nullptr;
		int pose = 1, charge = 0, weaponskill = -1;
		int dmgType = 0;
		int damage = 0, oldHP = 0, myAttack = 0, enemyAC = 0, attackAfterReductions = 0;
		double weaponMultipliers = 1.0;
		bool backstab = false, flanking = false, extraTarget = false;
		int parriedDamage = 0;
		int thornsEffect = 0;
		bool dyrnwynSmite = false, zealSmite = false, gugnirProc = false, armorPierceProc = false, whipPierce = false, parashuProc = false;
		int previousMonsterState = -1;
		// out: what the aftermath inflicted (the hit messages read these)
		bool paralyzeStatusInflicted = false, slowStatusInflicted = false, bleedStatusInflicted = false, swordExtraDamageInflicted = false;
		bool knockbackInflicted = false, dyrnwynBurn = false, playerPoisonedTarget = false, envenomWeapon = false, skillIncreased = false;
		int shillelaghDamage = 0, shillelaghDebuffs = 0;
	};

	// ---- control and hooks ---------------------------------------------------------------------------------
	bool coreEnabled();                   // the core runs (APX_COMBAT_OLD=1 / APX_COMBAT=0 = control)
	bool ownsMelee(int player);           // this player's melee goes through the core now
	bool swinging(int player);            // Windup, Active or Recovery
	State stateOf(Entity* e);
	void tickPlayer(int player);          // once per sim tick (actPlayer)
	// first-person pose: writes the hud weapon's offsets while a move is out; false = leave Barony's pose
	bool poseHud(int player, double& moveX, double& moveY, double& moveZ, double& yaw, double& pitch, double& roll);
	bool objectPass();                    // Entity::attack is being used for non-creature targets (doors, colliders)
	// the Barony hit sites that still deal creature melee, arrows and thrown weapons call this (damage already computed)
	int onLegacyHit(Entity* attacker, Entity* target, int damage, int path);
	// self-test and QA
	int selftestStep();                   // -apxselftest=all "moves" stage; returns 0 running, 1 pass, -1 fail
	struct Counters
	{
		long long swings = 0, quick = 0, directional = 0, charged = 0, hits = 0, sweepTargets = 0, wallBlocked = 0, fizzled = 0, kills = 0;
		long long legacyHits = 0, objectPasses = 0, hitStops = 0;
		double staminaSpent = 0;
	};
	const Counters& counters();
	void perfDump();                      // p50/p99 of the core's per-tick cost (all ticks and swing ticks)
} // namespace combat
} // namespace apx
