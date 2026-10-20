// VERBATIM from Entity::attack (entity.cpp) as of the D93 C1 port; the core runs it for player melee.
				int thornsEffect = 0;
				if ( hitstats->mask && hitstats->mask->type == MASK_MOUTHKNIFE )
				{
					if ( hitstats->mask->beatitude >= 0 || shouldInvertEquipmentBeatitude(hitstats) )
					{
						thornsEffect = (1 + abs(hitstats->mask->beatitude)) * 2;
					}
					else
					{
						thornsEffect = -2 * (1 + abs(hitstats->mask->beatitude));
					}
				}
				if ( hitstats->getEffectActive(EFF_ENSEMBLE_HORN) )
				{
					thornsEffect += static_cast<int>(hitstats->getEnsembleEffectBonus(Stat::ENSEMBLE_HORN_TIER));
				}

				bool dyrnwynSmite = false;
				bool zealSmite = false;
				bool gugnirProc = false;
				bool armorPierceProc = false;
				bool whipPierce = false;
				if ( whip )
				{
					int statChance = std::min(std::max(this->getPER(), 0), 50); // 0 to 50 value.
					if ( behavior == &actMonster )
					{
						statChance = std::min(std::max(this->getPER() / 2, 0), 50);
					}
					int chance = local_rng.rand() % 100;
					if ( chance < statChance )
					{
						whipPierce = true;
						armorPierceProc = true;
						if ( this->behavior == &actMonster )
						{
							weaponMultipliers += std::min(25, std::max(0, statGetPER(myStats, this))) / 100.0;
						}
						else
						{
							weaponMultipliers += std::max(0, statGetPER(myStats, this)) / 100.0;
						}
					}
				}

				Entity::modifyDamageMultipliersFromEffects(hit.entity, this, weaponMultipliers, dmgType);

				if ( (hitstats->getEffectActive(EFF_DIVINE_FIRE) & 0xF) ||
						(
							(!shapeshifted && weaponskill == PRO_SWORD && myStats->weapon && myStats->weapon->type == ARTIFACT_SWORD)
							|| (/*myStats->weapon &&*/ myStats->getEffectActive(EFF_DIVINE_ZEAL))
							|| (/*myStats->weapon &&*/ myStats->getEffectActive(EFF_FOCI_LIGHT_JUSTICE)))
					)
				{
					bool particle = false;

					bool effect = false;
					if ( (hitstats->getEffectActive(EFF_DIVINE_FIRE) & 0xF) )
					{
						particle = true;
						effect = true;
					}

					if ( hit.entity->isSmiteWeakMonster() )
					{
						// smite these creatures
						if ( weaponskill == PRO_SWORD && myStats->weapon && myStats->weapon->type == ARTIFACT_SWORD
							&& !shapeshifted )
						{
							real_t amount = 0.0;
							real_t percent = getArtifactWeaponEffectChance(myStats->weapon->type, *myStats, &amount);
							if ( local_rng.rand() % 100 < static_cast<int>(percent) )
							{
								weaponMultipliers += amount;
								dyrnwynSmite = true;
								particle = true;
								effect = true;
								//playSoundEntity(hit.entity, 249, 64);
							}
						}
						if ( !dyrnwynSmite )
						{
							if ( myStats->getEffectActive(EFF_DIVINE_ZEAL) )
							{
								zealSmite = true;
								particle = true;
								effect = true;
								real_t bonus = getSpellDamageFromID(SPELL_DIVINE_ZEAL, this, myStats, this) / 100.0;
								bonus = std::min(bonus, getSpellDamageSecondaryFromID(SPELL_DIVINE_ZEAL, nullptr, nullptr, nullptr) / 100.0);
								weaponMultipliers += bonus;

								if ( behavior == &actPlayer )
								{
									players[skill[2]]->mechanics.updateSustainedSpellEvent(SPELL_DIVINE_ZEAL, bonus * 100.0, 1.0, hit.entity);
								}
							}
							if ( myStats->getEffectActive(EFF_FOCI_LIGHT_JUSTICE) )
							{
								effect = true;
								zealSmite = true;
								particle = true;
								weaponMultipliers += getSpellEffectDurationSecondaryFromID(SPELL_FOCI_LIGHT_JUSTICE, nullptr, nullptr, nullptr) / 100.0;
								int tier = std::max(0, (myStats->getEffectActive(EFF_FOCI_LIGHT_JUSTICE) - 1));
								weaponMultipliers += tier * getSpellDamageSecondaryFromID(SPELL_FOCI_LIGHT_JUSTICE, nullptr, nullptr, nullptr) / 100.0;
							}
						}
					}

					if ( effect )
					{
						if ( myStats->type == SALAMANDER
							&& myStats->getEffectActive(EFF_SALAMANDER_HEART) == 2 )
						{
							if ( myStats->EFFECTS_TIMERS[EFF_SALAMANDER_HEART] > 0 )
							{
								int prevDuration = myStats->EFFECTS_TIMERS[EFF_SALAMANDER_HEART];
								myStats->EFFECTS_TIMERS[EFF_SALAMANDER_HEART] += TICKS_PER_SECOND / 2;
								if ( prevDuration < 5 * TICKS_PER_SECOND && myStats->EFFECTS_TIMERS[EFF_SALAMANDER_HEART] >= 5 * TICKS_PER_SECOND )
								{
									if ( this->behavior == &actPlayer )
									{
										serverUpdateEffects(skill[2]); // update timer
									}
								}
							}
						}
					}

					if ( particle )
					{
						spawnMagicEffectParticles(hit.entity->x, hit.entity->y, hit.entity->z, 981);
					}
				}
