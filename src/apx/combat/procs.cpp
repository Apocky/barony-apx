// apx/combat/procs.cpp -- the melee rule code of the old Entity::attack, moved into the combat core (D93 C1: no feature lost).
// The four .inc files are VERBATIM slices of Entity::attack; this file only gives them the locals they expect and carries
// their results back in MeleeCtx. Entity::attack keeps its own copy for the APX_COMBAT_OLD=1 control until that path is deleted.
#include "../../main.hpp"
#include "../../game.hpp"
#include "../../stat.hpp"
#include "../../entity.hpp"
#include "../../items.hpp"
#include "../../monster.hpp"
#include "../../engine/audio/sound.hpp"
#include "../../magic/magic.hpp"
#include "../../interface/interface.hpp"
#include "../../net.hpp"
#include "../../collision.hpp"
#include "../../paths.hpp"
#include "../../shops.hpp"
#ifdef STEAMWORKS
#include <steam/steam_api.h>
#endif
#include "../../player.hpp"
#include "../../scores.hpp"
#include "../../menu.hpp"
#include "../../mod_tools.hpp"
#include "../parts_engine.hpp"
#include "../../apx_stamina.hpp"
#include "core.hpp"

using apx::combat::MeleeCtx;

// before the AC step: thorns, whip pierce, effect multipliers, divine and artifact smites
void Entity::apxMeleePre1(MeleeCtx& c)
{
	Stat* myStats = getStats();
	Entity* hitEntity = c.target;
	Stat* hitstats = hitEntity->getStats();
	const hit_t savedHit = hit;
	hit.entity = hitEntity;
	const int player = behavior == &actPlayer ? skill[2] : -1;
	const bool shapeshifted = false, whip = false;
	const int weaponskill = c.weaponskill;
	DamageTableType dmgType = static_cast<DamageTableType>(c.dmgType);
	real_t weaponMultipliers = c.weaponMultipliers;
	(void)player;
#include "procs_pre_mult.inc"
	c.weaponMultipliers = weaponMultipliers;
	c.thornsEffect = thornsEffect;
	c.dyrnwynSmite = dyrnwynSmite; c.zealSmite = zealSmite; c.gugnirProc = gugnirProc; c.armorPierceProc = armorPierceProc; c.whipPierce = whipPierce;
	hit = savedHit;
}

// after enemyAC is known: Gungnir, whip pierce, drum
void Entity::apxMeleePre2(MeleeCtx& c)
{
	Stat* myStats = getStats();
	Entity* hitEntity = c.target;
	const hit_t savedHit = hit;
	hit.entity = hitEntity;
	const bool shapeshifted = false;
	const int weaponskill = c.weaponskill;
	const int charge = c.charge;
	int enemyAC = c.enemyAC;
	bool gugnirProc = c.gugnirProc, armorPierceProc = c.armorPierceProc, whipPierce = c.whipPierce;
#include "procs_pre_ac.inc"
	c.enemyAC = enemyAC;
	c.gugnirProc = gugnirProc; c.armorPierceProc = armorPierceProc;
	hit = savedHit;
}

// after modHP: skill gain, armour and shield wear, weapon wear, obituary, compendium, weapon procs (poison, paralyze, slow, knockback, bleed, artifacts)
void Entity::apxMeleeAftermath(MeleeCtx& c)
{
	Stat* myStats = getStats();
	Entity* hitEntity = c.target;
	Stat* hitstats = hitEntity->getStats();
	const hit_t savedHit = hit;
	hit.entity = hitEntity;
	const int player = behavior == &actPlayer ? skill[2] : -1;
	const int playerhit = hitEntity->behavior == &actPlayer ? hitEntity->skill[2] : -1;
	const bool shapeshifted = false, whip = false, flail = false, isIllusion = false;
	Entity* const target = nullptr;
	int pose = c.pose, charge = c.charge, weaponskill = c.weaponskill;
	int damage = c.damage;
	const int oldHP = c.oldHP;
	const int myAttack = c.myAttack, attackAfterReductions = c.attackAfterReductions, enemyAC = c.enemyAC;
	(void)enemyAC; (void)myAttack;
	real_t weaponMultipliers = c.weaponMultipliers;
	bool backstab = c.backstab, flanking = c.flanking;
	bool dyrnwynSmite = c.dyrnwynSmite, zealSmite = c.zealSmite, gugnirProc = c.gugnirProc, armorPierceProc = c.armorPierceProc, parashuProc = c.parashuProc;
	int parriedDamage = c.parriedDamage;
	int thornsEffect = c.thornsEffect;
	const int previousMonsterState = c.previousMonsterState;
	const int olddamage = damage;
	const int chargeMult = (c.charge >= Stat::getMaxAttackCharge(myStats)) ? 2 : 1;
	(void)olddamage; (void)flanking; (void)backstab; (void)gugnirProc; (void)armorPierceProc; (void)dyrnwynSmite; (void)zealSmite; (void)parashuProc; (void)previousMonsterState; (void)chargeMult;
#include "procs_wear_select.inc"
#include "procs_aftermath.inc"
	c.damage = damage;
	c.paralyzeStatusInflicted = paralyzeStatusInflicted; c.slowStatusInflicted = slowStatusInflicted; c.bleedStatusInflicted = bleedStatusInflicted;
	c.swordExtraDamageInflicted = swordExtraDamageInflicted; c.knockbackInflicted = knockbackInflicted; c.dyrnwynBurn = dyrnwynBurn;
	c.playerPoisonedTarget = playerPoisonedTarget; c.envenomWeapon = envenomWeapon; c.skillIncreased = skillIncreased;
	c.shillelaghDamage = shillelaghDamage; c.shillelaghDebuffs = shillelaghDebuffs;
	hit = savedHit;
}
