// VERBATIM from Entity::attack (entity.cpp) as of the D93 C1 port; the core runs it for player melee. Do not edit without checking the legacy copy.
					bool skillIncreased = false;
					// skill increase
					// can raise skills up to skill level 20 on dummybots...
					bool doSkillIncrease = true;
					if ( monsterIsImmobileTurret(hit.entity, hitstats) )
					{
						if ( hitstats->type == DUMMYBOT && hitstats->HP > 0 )
						{
							doSkillIncrease = true; // can train on dummybots.
						}
						else
						{
							doSkillIncrease = false; // no skill for killing/hurting other turrets.
						}
					}
					else if ( hit.entity->behavior == &actMonster 
						&& (hit.entity->monsterAllyGetPlayerLeader() || (hitstats && achievementObserver.checkUidIsFromPlayer(hitstats->leader_uid) >= 0)) 
							&& behavior == &actPlayer )
					{
						doSkillIncrease = false; // no level up on allies
					}
					if ( hit.entity->behavior == &actPlayer && behavior == &actPlayer )
					{
						doSkillIncrease = false; // no skill for killing/hurting players
					}
					if ( hitstats->getEffectActive(EFF_STASIS) )
					{
						doSkillIncrease = false;
					}

					if ( doSkillIncrease && player >= 0 )
					{
						if ( isInvisible() )
						{
							players[player]->mechanics.updateSustainedSpellEvent(SPELL_INVISIBILITY, 10.0, 1.0, hit.entity);
						}
					}

					if ( doSkillIncrease
						&& ((weaponskill >= PRO_SWORD && weaponskill <= PRO_POLEARM) || weaponskill == PRO_UNARMED || (whip && weaponskill == PRO_RANGED)) )
					{
						if ( myStats->weapon &&
							(myStats->weapon->type == CRYSTAL_BATTLEAXE
								|| myStats->weapon->type == CRYSTAL_MACE
								|| myStats->weapon->type == CRYSTAL_SWORD
								|| myStats->weapon->type == CRYSTAL_SPEAR) )
						{
							int chance = 6;
							bool notify = true;
							if ( myStats->type == GOBLIN )
							{
								chance = 10;
								notify = true;
							}

							if ( local_rng.rand() % chance == 0 )
							{
								bool lowSkill = hitstats->type == DUMMYBOT;
								if ( !lowSkill || (lowSkill && myStats->getProficiency(weaponskill) < SKILL_LEVEL_BASIC) )
								{
									this->increaseSkill(weaponskill, notify);
									skillIncreased = true;
								}
							}
						}
						else if ( hitstats->HP <= 0 )
						{
							if ( player >= 0 && weaponskill == PRO_UNARMED 
								&& stats[player]->type == GOATMAN
								&& stats[player]->getEffectActive(EFF_DRUNK) )
							{
								steamStatisticUpdateClient(player, STEAM_STAT_BARFIGHT_CHAMP, STEAM_STAT_INT, 1);
							}
							int chance = 8;
							bool notify = true;
							if ( myStats->type == GOBLIN )
							{
								chance = 12;
								notify = true;
							}

							if ( flail && pose == MONSTER_POSE_FLAIL_SWING )
							{
								chance *= 4;
							}

							if ( local_rng.rand() % chance == 0 )
							{
								bool lowSkill = hitstats->type == DUMMYBOT;
								if ( !lowSkill || (lowSkill && myStats->getProficiency(weaponskill) < SKILL_LEVEL_BASIC) )
								{
									this->increaseSkill(weaponskill, notify);
									skillIncreased = true;
								}
							}
						}
						else
						{
							int chance = 10;
							bool notify = true;
							if ( myStats->type == GOBLIN && weaponskill != PRO_RANGED )
							{
								chance = 14;
								notify = true;
							}

							if ( flail && pose == MONSTER_POSE_FLAIL_SWING )
							{
								chance *= 4;
							}

							if ( local_rng.rand() % chance == 0 )
							{
								if ( hitstats->type != DUMMYBOT || (hitstats->type == DUMMYBOT && myStats->getProficiency(weaponskill) < SKILL_LEVEL_BASIC) )
								{
									this->increaseSkill(weaponskill, notify);
									skillIncreased = true;
								}
							}
						}
					}

					if ( skillIncreased && myStats->type == GOBLIN && weaponskill != PRO_RANGED )
					{
						// goblins level up all combat skills at once.
						int numIncreases = 0;
						if ( weaponskill != PRO_SWORD )
						{
							numIncreases += this->increaseSkill(PRO_SWORD, false) ? 1 : 0;
						}
						if ( weaponskill != PRO_MACE )
						{
							numIncreases += this->increaseSkill(PRO_MACE, false) ? 1 : 0;
						}
						if ( weaponskill != PRO_AXE )
						{
							numIncreases += this->increaseSkill(PRO_AXE, false) ? 1 : 0;
						}
						if ( weaponskill != PRO_POLEARM )
						{
							numIncreases += this->increaseSkill(PRO_POLEARM, false) ? 1 : 0;
						}
						if ( weaponskill != PRO_UNARMED )
						{
							numIncreases += this->increaseSkill(PRO_UNARMED, false) ? 1 : 0;
						}
						if ( player >= 0 && numIncreases > 0 )
						{
							Uint32 color = makeColorRGB(255, 255, 0);
							messagePlayerColor(player, MESSAGE_PROGRESSION, color, Language::get(3446));
						}
					}

					if ( hitstats && hitstats->getEffectActive(EFF_GUARD_BODY) )
					{
						thaumSpellArmorProc(hit.entity, *hitstats, false, this, EFF_GUARD_BODY);
					}
					if ( hitstats && hitstats->getEffectActive(EFF_DIVINE_GUARD) )
					{
						thaumSpellArmorProc(hit.entity, *hitstats, false, this, EFF_DIVINE_GUARD);
					}

					// write the obituary
					if ( hit.entity != this )
					{
						killedByMonsterObituary(hit.entity);
					}

					// damage weapon if applicable

					bool isWeakWeapon = false;
					bool artifactWeapon = false;
					bool degradeWeapon = false;

					if ( pose == PLAYER_POSE_GOLEM_SMASH )
					{
						if ( behavior == &actPlayer )
						{
							if ( damage > 0 && oldHP > hitstats->HP )
							{
								Sint32 damageTaken = oldHP - hitstats->HP;
								Compendium_t::Events_t::eventUpdate(skill[2], Compendium_t::CPDM_SPELL_DMG, SPELL_ITEM, damageTaken, false, SPELL_STRIKE);
								magicOnSpellCastEvent(this, this, hit.entity, SPELL_STRIKE, spell_t::SPELL_LEVEL_EVENT_DMG, damageTaken);
							}
						}
					}

					if ( weaponToBreak != nullptr && weaponType != WOODEN_SHIELD )
					{
						if ( behavior == &actPlayer )
						{
							Compendium_t::Events_t::eventUpdate(skill[2], Compendium_t::CPDM_ATTACKS,
								weaponType, 1);
							if ( pose == PLAYER_POSE_GOLEM_SMASH )
							{

							}
							else
							{
								Compendium_t::Events_t::eventUpdate(skill[2], Compendium_t::CPDM_DMG_MAX, weaponType, damage);
							}
						}

						if ( weaponType == ARTIFACT_AXE || weaponType == ARTIFACT_MACE || weaponType == ARTIFACT_SPEAR || weaponType == ARTIFACT_SWORD )
						{
							artifactWeapon = true;
						}
						else if ( weaponType == CRYSTAL_BATTLEAXE || weaponType == CRYSTAL_MACE || weaponType == CRYSTAL_SWORD || weaponType == CRYSTAL_SPEAR )
						{
							// crystal weapons degrade faster.
							isWeakWeapon = true;
						}

						if ( myStats->type == GOBLIN )
						{
							isWeakWeapon = false;
						}

						if ( !artifactWeapon )
						{
							// normal weapons chance to not degrade 75% chance on 0 dmg, else 98%
							int degradeOnZeroDMG = 4 + (myStats->type == GOBLIN ? 4 : 0);
							int degradeOnNormalDMG = 50 + (myStats->type == GOBLIN ? 20 : 0);

							if ( !myStats->weapon && (*weaponToBreak) )
							{
								// unarmed glove weapons, 87.5% to not degrade on 0 dmg, else 99%
								degradeOnZeroDMG = 8 + (myStats->type == GOBLIN ? 4 : 0);
								degradeOnNormalDMG = 100 + (myStats->type == GOBLIN ? 20 : 0);
							}
							else if ( isWeakWeapon )
							{
								// crystal weapons chance to not degrade 66% chance on 0 dmg, else 97.5%
								degradeOnZeroDMG = 3;
								degradeOnNormalDMG = 40;
							}

							if ( behavior == &actPlayer && ((weaponskill >= PRO_SWORD && weaponskill <= PRO_POLEARM) 
								|| weaponskill == PRO_UNARMED || weaponskill == PRO_RANGED) )
							{
								int skillLVL = myStats->getModifiedProficiency(weaponskill) / 20;
								degradeOnZeroDMG += skillLVL; // increase by 1-5
								degradeOnNormalDMG += (skillLVL * 10); // increase by 10-50
							}
							if ( myStats->weapon && myStats->weapon->type == TOOL_WHIP )
							{
								degradeOnZeroDMG = degradeOnNormalDMG; // don't degrade faster on 0 dmg.
							}
							if ( behavior == &actPlayer && (svFlags & SV_FLAG_HARDCORE) )
							{
								// double durability.
								degradeOnZeroDMG *= 2;
								degradeOnNormalDMG *= 2;
							}

							if ( flail && pose == MONSTER_POSE_FLAIL_SWING )
							{
								// double durability.
								degradeOnZeroDMG *= 2;
								degradeOnNormalDMG *= 2;
							}

							if	( (local_rng.rand() % degradeOnZeroDMG == 0 && damage == 0)
									|| (local_rng.rand() % degradeOnNormalDMG == 0 && damage > 0)
								)
							{
								degradeWeapon = true;
							}

							if ( player >= 0 )
							{
								if ( !players[player]->mechanics.itemDegradeRoll(*weaponToBreak, weaponskill) )
								{
									degradeWeapon = false;
								}
							}
							if ( behavior == &actPlayer && skillCapstoneUnlocked(skill[2], weaponskill) )
							{
								// don't degrade on capstone skill.
								degradeWeapon = false;
							}
							else if ( myStats->type == SHADOW || myStats->type == LICH_FIRE || myStats->type == LICH_ICE )
							{
								degradeWeapon = false; //certain monster's weapons don't degrade.
							}
							else if ( myStats->type == SKELETON && behavior == &actMonster && monsterAllySummonRank != 0 )
							{
								degradeWeapon = false;
							}
							else if ( isIllusion )
							{
								degradeWeapon = false;
							}
							else if ( myStats->weapon && myStats->weapon->type == TOOL_WHIP )
							{
								degradeWeapon = false;
							}
							else if ( myStats->weapon && myStats->weapon->type == RAPIER )
							{
								degradeWeapon = false;
							}
							else if ( myStats->weapon && myStats->weapon->type == SHILLELAGH_MACE )
							{
								degradeWeapon = false;
							}
							else if ( myStats->weapon && myStats->weapon->type == TOOL_PICKAXE && myStats->type == GNOME )
							{
								degradeWeapon = false;
							}
							else if ( flail	&& pose == MONSTER_POSE_FLAIL_SWING && local_rng.rand() % 4 > 0 )
							{
								degradeWeapon = false;
							}

							if ( degradeWeapon && !spellEffectPreserveItem(*weaponToBreak) )
							{
								if ( player >= 0 )
								{
									players[player]->mechanics.onItemDegrade(*weaponToBreak);
								}

								if ( (player >= 0 && players[player]->isLocalPlayer()) || player < 0 )
								{
									if ( (*weaponToBreak)->count > 1 )
									{
										newItem((*weaponToBreak)->type, (*weaponToBreak)->status, (*weaponToBreak)->beatitude,
											(*weaponToBreak)->count - 1, (*weaponToBreak)->appearance, (*weaponToBreak)->identified, &myStats->inventory);
									}
								}
								(*weaponToBreak)->count = 1;
								(*weaponToBreak)->status = static_cast<Status>((*weaponToBreak)->status - 1);
								if ( (*weaponToBreak)->status != BROKEN )
								{
									messagePlayer(player, MESSAGE_EQUIPMENT, Language::get(679));
									if ( behavior == &actPlayer )
									{
										Compendium_t::Events_t::eventUpdate(player, Compendium_t::CPDM_DEGRADED, (*weaponToBreak)->type, 1);
									}
								}
								else
								{
									playSoundEntity(this, 76, 64);
									messagePlayer(player, MESSAGE_EQUIPMENT, Language::get(680));
									if ( behavior == &actPlayer )
									{
										Compendium_t::Events_t::eventUpdate(player, Compendium_t::CPDM_BROKEN, (*weaponToBreak)->type, 1);
									}
								}
								if ( player > 0 && multiplayer == SERVER && !players[player]->isLocalPlayer() )
								{
									strcpy((char*)net_packet->data, "ARMR");
									if ( weaponToBreak == &myStats->weapon )
									{
										net_packet->data[4] = 5;
									}
									else
									{
										net_packet->data[4] = 2;
									}
									net_packet->data[5] = (*weaponToBreak)->status;
									SDLNet_Write16((int)(*weaponToBreak)->type, &net_packet->data[6]);
									net_packet->address.host = net_clients[player - 1].host;
									net_packet->address.port = net_clients[player - 1].port;
									net_packet->len = 8;
									sendPacketSafe(net_sock, -1, net_packet, player - 1);
								}
								if ( (*weaponToBreak)->status == BROKEN && behavior == &actMonster && playerhit >= 0 )
								{
									steamStatisticUpdateClient(playerhit, STEAM_STAT_TOUGH_AS_NAILS, STEAM_STAT_INT, 1);
								}
							}
						}
					}

					// damage opponent armor if applicable
					Item* armor = NULL;
					int armornum = 0;
					bool isWeakArmor = false;
					bool shieldIncreased = false;
					if ( damage > 0 || (damage == 0 && !(hitstats->shield && hitstats->defending) && parriedDamage == 0) )
					{
						if ( behavior == &actMonster )
						{
							int accuracy = myStats->monsterRangedAccuracy.getAccuracy(monsterTarget);
							myStats->monsterRangedAccuracy.incrementAccuracy();
						}

						// choose random piece of equipment to target
						armornum = hitstats->pickRandomEquippedItemToDegradeOnHit(&armor, true, false, false, true);
						if ( armor != NULL && armor->status > BROKEN )
						{
							switch ( armor->type )
							{
								case CRYSTAL_HELM:
								case CRYSTAL_SHIELD:
								case CRYSTAL_BREASTPIECE:
								case CRYSTAL_BOOTS:
								case CRYSTAL_GLOVES:
									isWeakArmor = true;
									break;
								default:
									isWeakArmor = false;
									break;
							}
						}

						int armorDegradeChance = 25;
						if ( isWeakArmor )
						{
							armorDegradeChance = 15;
							if ( weaponskill == PRO_MACE )
							{
								if ( !(flail && pose == MONSTER_POSE_FLAIL_SWING) )
								{
									armorDegradeChance = 5;
								}
							}
						}
						else
						{
							if ( weaponskill == PRO_MACE )
							{
								if ( !(flail && pose == MONSTER_POSE_FLAIL_SWING) )
								{
									armorDegradeChance = 10;
								}
							}
						}

						if ( hitstats->type == GOBLIN )
						{
							armorDegradeChance += 10;
						}

						if ( hit.entity->behavior == &actPlayer && armornum == 4 && hitstats->shield )
						{
							if ( skillCapstoneUnlocked(hit.entity->skill[2], PRO_SHIELD) )
							{
								armorDegradeChance = 100; // don't break.
							}
							else
							{
								if ( itemCategory(hitstats->shield) == ARMOR )
								{
									armorDegradeChance += 2 * (hitstats->getModifiedProficiency(PRO_SHIELD) / 10);
									armorDegradeChance += 10;
									if ( !players[hit.entity->skill[2]]->mechanics.itemDegradeRoll(hitstats->shield) )
									{
										armorDegradeChance = 100; // don't break.
									}
								}
								else
								{
									armorDegradeChance += (hitstats->getModifiedProficiency(PRO_SHIELD) / 10);
								}
							}
						}

						// crystal golem special attack increase chance for armor to break if hit. (25-33%)
						// special attack only degrades armor if primary target.
						if ( (pose == MONSTER_POSE_GOLEM_SMASH || pose == PLAYER_POSE_GOLEM_SMASH) && target == nullptr )
						{
							if ( isWeakArmor )
							{
								// 66% chance to be deselected from degrading.
								if ( local_rng.rand() % 3 > 0 )
								{
									armor = NULL;
									armornum = 0;
								}
							}
							else
							{
								// 75% chance to be deselected from degrading.
								if ( local_rng.rand() % 4 > 0 )
								{
									armor = NULL;
									armornum = 0;
								}
							}
						}
						else
						{
							if ( armorDegradeChance == 100 || (local_rng.rand() % armorDegradeChance > 0) )
							{
								armor = NULL;
								armornum = 0;
							}
						}
					}

					// if nothing chosen to degrade, check extra shield chances to degrade
					if ( hitstats->shield != NULL && hitstats->shield->status > BROKEN && armor == NULL
						&& !itemTypeIsQuiver(hitstats->shield->type) && itemCategory(hitstats->shield) != SPELLBOOK
						&& !itemTypeIsFoci(hitstats->shield->type)
						&& !itemTypeIsInstrument(hitstats->shield->type)
						&& hitstats->shield->type != TOOL_DUCK
						&& parriedDamage == 0
						&& hitstats->shield->type != TOOL_TINKERING_KIT
						&& hitstats->shield->type != TOOL_FRYING_PAN )
					{
						if ( hitstats->shield->type == TOOL_CRYSTALSHARD && hitstats->defending )
						{
							// shards degrade by 1 stage each hit.
							armor = hitstats->shield;
							armornum = 4;
						}
						else if ( hitstats->shield->type == MIRROR_SHIELD && hitstats->defending )
						{
							// mirror shield degrade by 1 stage each hit.
							armor = hitstats->shield;
							armornum = 4;
						}
						else
						{
							// if no armor piece was chosen to break, grant chance to improve shield skill.
							if ( itemCategory(hitstats->shield) == ARMOR
								|| (hitstats->defending) )
							{
								int roll = 20;
								int hitskill = hitstats->getProficiency(PRO_SHIELD) / 20;
								roll += hitskill * 5;
								if ( damage == 0 )
								{
									roll /= 2;
								}
								if ( myStats->type == BAT_SMALL )
								{
									if ( hitstats->getProficiency(PRO_SHIELD) >= SKILL_LEVEL_BASIC )
									{
										roll *= 4;
									}
								}
								if ( roll > 0 )
								{
									bool success = (local_rng.rand() % roll == 0);
									if ( !success && playerhit >= 0 && hitstats->defending )
									{
  										if ( players[playerhit]->mechanics.defendTicks != 0 )
										{
											if ( (::ticks - players[playerhit]->mechanics.defendTicks) < (TICKS_PER_SECOND / 3) )
											{
												// perfect block timing, roll again
												success = (local_rng.rand() % roll == 0);
											}
										}
									}

									if ( success )
									{
										bool increaseSkill = true;
										if ( hit.entity->behavior == &actPlayer && behavior == &actPlayer )
										{
											increaseSkill = false;
										}
										else if ( hit.entity->behavior == &actPlayer && this->monsterAllyGetPlayerLeader() )
										{
											increaseSkill = false;
										}
										else if ( hit.entity->behavior == &actPlayer
											&& !players[hit.entity->skill[2]]->mechanics.allowedRaiseBlockingAgainstEntity(*this) )
										{
											increaseSkill = false;
										}
										else if ( hitstats->getEffectActive(EFF_SHAPESHIFT) )
										{
											increaseSkill = false;
										}
										else if ( itemCategory(hitstats->shield) != ARMOR
											&& hitstats->getProficiency(PRO_SHIELD) >= SKILL_LEVEL_SKILLED )
										{
											increaseSkill = false; // non-shield offhands dont increase skill past 40.
										}
										if ( increaseSkill )
										{
											if ( hitstats->getEffectActive(EFF_FORCE_SHIELD) )
											{
												magicOnSpellCastEvent(hit.entity, hit.entity, this, SPELL_FORCE_SHIELD, spell_t::SpellOnCastEventTypes::SPELL_LEVEL_EVENT_DEFAULT, 1);
											}
											hit.entity->increaseSkill(PRO_SHIELD); // increase shield skill
											shieldIncreased = true;
											if ( hit.entity->behavior == &actPlayer )
											{
												players[hit.entity->skill[2]]->mechanics.enemyRaisedBlockingAgainst[this->getUID()]++;
											}
										}
									}
								}
							}

							// shield still has chance to degrade after raising skill.
							// crystal golem special attack increase chance for shield to break if defended. (33%)
							// special attack only degrades shields if primary target.
							int shieldDegradeChance = 10;
							if ( svFlags & SV_FLAG_HARDCORE )
							{
								shieldDegradeChance = 40;
							}
							if ( hitstats->type == GOBLIN )
							{
								shieldDegradeChance += 10;
							}
							if ( weaponskill == PRO_MACE )
							{
								shieldDegradeChance *= 0.75;
							}
							if ( hit.entity->behavior == &actPlayer )
							{
								if ( itemCategory(hitstats->shield) == ARMOR )
								{
									shieldDegradeChance += 2 * (hitstats->getModifiedProficiency(PRO_SHIELD) / 10); // 2x shield bonus offhand
									if ( !players[hit.entity->skill[2]]->mechanics.itemDegradeRoll(hitstats->shield) )
									{
										shieldDegradeChance = 100; // don't break.
									}
								}
								else
								{
									shieldDegradeChance += (hitstats->getModifiedProficiency(PRO_SHIELD) / 10);
								}
								if ( skillCapstoneUnlocked(hit.entity->skill[2], PRO_SHIELD) )
								{
									shieldDegradeChance = 100; // don't break.
								}
							}
							if ( shieldDegradeChance < 100 && armor == NULL &&
								(	(hitstats->defending && local_rng.rand() % shieldDegradeChance == 0)
									|| (hitstats->defending && pose == MONSTER_POSE_GOLEM_SMASH && target == nullptr && local_rng.rand() % 3 == 0)
								)
								)
							{
								armor = hitstats->shield;
								armornum = 4;
							}
						}
					}

					if ( armor != NULL && armor->status > BROKEN )
					{
						hit.entity->degradeArmor(*hitstats, *armor, armornum);
						if ( armor->status == BROKEN )
						{
							if ( player >= 0 && hit.entity->behavior == &actMonster )
							{
								players[player]->mechanics.incrementBreakableCounter(Player::PlayerMechanics_t::BreakableEvent::GBREAK_DEGRADE, hit.entity);
								steamStatisticUpdateClient(player, STEAM_STAT_UNSTOPPABLE_FORCE, STEAM_STAT_INT, 1);
								if ( armornum == 4 && hitstats->type == BUGBEAR 
									&& (hitstats->defending || hit.entity->monsterAttack == MONSTER_POSE_BUGBEAR_SHIELD) )
								{
									steamAchievementClient(player, "BARONY_ACH_BEAR_WITH_ME");
								}
							}
						}
					}

					bool statusInflicted = false;
					bool paralyzeStatusInflicted = false;
					bool slowStatusInflicted = false;
					bool bleedStatusInflicted = false;
					bool swordExtraDamageInflicted = false;
					bool knockbackInflicted = false;
					bool dyrnwynBurn = false;
					int shillelaghDamage = 0;
					int shillelaghDebuffs = 0;
					/*if ( thornsEffect < 0 )
					{
						hit.entity->modHP(thornsEffect);
					}*/

					// special weapon effects
					if ( myStats->weapon && !shapeshifted )
					{
						if ( myStats->weapon->type == ARTIFACT_SWORD )
						{
							real_t amount = 0.0;
							real_t percent = getArtifactWeaponEffectChance(myStats->weapon->type, *myStats, &amount);
							if ( dyrnwynSmite || (local_rng.rand() % 100 < static_cast<int>(percent)) )
							{
								if ( hit.entity->flags[BURNABLE] )
								{
									if ( hitstats )
									{
										hitstats->burningInflictedBy = static_cast<Sint32>(uid);
									}

									bool wasBurning = hit.entity->flags[BURNING];
									// Attempt to set the Entity on fire
									hit.entity->SetEntityOnFire(this);

									if ( !wasBurning && hit.entity->flags[BURNING] )
									{
										// 6 ticks maximum burning.
										hit.entity->char_fire = std::min(hit.entity->char_fire, static_cast<int>(TICKS_TO_PROCESS_FIRE * (6 + amount)));
										dyrnwynBurn = true;
									}

									// If a Player was hit, and they are now on fire, tell them what set them on fire
									if ( playerhit >= 0 && hit.entity->flags[BURNING] )
									{
										messagePlayer(playerhit, MESSAGE_COMBAT, Language::get(683)); // "Dyrnwyn sets you on fire!"
									}
								}
							}
						}
						else if ( myStats->weapon->type == ARTIFACT_AXE && parashuProc )
						{
							int duration = 100; // 2 seconds
							if ( hitstats->HP > 0 && hit.entity->setEffect(EFF_SLOW, true, duration, true) )
							{
								slowStatusInflicted = true;
								playSoundEntity(hit.entity, 396 + local_rng.rand() % 3, 64);
								spawnMagicEffectParticles(hit.entity->x, hit.entity->y, hit.entity->z, 171);
							}
						}
						if ( !shapeshifted && weaponskill == PRO_MACE && myStats->weapon && myStats->weapon->type == SHILLELAGH_MACE )
						{
							int numDebuffs = hitstats->numShillelaghDebuffsActive(hit.entity);
							if ( numDebuffs > 0 )
							{
								shillelaghDebuffs = numDebuffs;
								real_t scale = 0.25 + numDebuffs * 0.25;
								int bonusDamage = attackAfterReductions * scale * Entity::getDamageTableMultiplier(hit.entity, *hitstats, DAMAGE_TABLE_MAGIC);
								if ( bonusDamage > 0 )
								{
									real_t variance = 20;
									real_t baseSkillModifier = 50.0; // 40-60 base
									real_t skillModifier = baseSkillModifier - (variance / 2) + (myStats->getModifiedProficiency(PRO_MYSTICISM) / 2.0);
									skillModifier += (local_rng.rand() % (1 + static_cast<int>(variance)));
									skillModifier /= 100.0;
									skillModifier = std::min(skillModifier, 1.0);
									bonusDamage = bonusDamage - static_cast<int>((1.0 - skillModifier) * bonusDamage);
									//messagePlayer(0, MESSAGE_DEBUG, "Shill: %d", bonusDamage);
									shillelaghDamage = bonusDamage;

									Compendium_t::Events_t::eventUpdate(skill[2], Compendium_t::CPDM_SHILLELAGH_DEBUFFS_MAX, SHILLELAGH_MACE, numDebuffs);
									Compendium_t::Events_t::eventUpdate(skill[2], Compendium_t::CPDM_DMG_MAX, SHILLELAGH_MACE, damage + shillelaghDamage);

									Sint32 prevHP = hitstats->HP;
									hit.entity->modHP(-shillelaghDamage);
									Sint32 newHP = hitstats->HP;
									if ( newHP < prevHP )
									{
										//spawnDamageGib(hit.entity, shillelaghDamage, DamageGib::DMG_STRONGER, DamageGibDisplayType::DMG_GIB_NUMBER);
										spawnDamageGib(hit.entity, 225, DamageGib::DMG_STRONGER, DamageGibDisplayType::DMG_GIB_SPRITE);
										spawnDamageGib(hit.entity, 261, DamageGib::DMG_STRONGER, DamageGibDisplayType::DMG_GIB_SPRITE);
									}
								}
							}
						}
					}

					if ( (hitstats->getEffectActive(EFF_WEBBED) || hitstats->getEffectActive(EFF_MAGIC_GREASE) 
						|| pose == PLAYER_POSE_GOLEM_SMASH 
						|| hit.entity->myconidReboundOnHit(this)
						|| (myStats->type == GNOME && myStats->weapon && !shapeshifted && myStats->weapon->type == TOOL_PICKAXE))
						&& !hitstats->getEffectActive(EFF_KNOCKBACK) && hit.entity->setEffect(EFF_KNOCKBACK, true, 30, false) )
					{
						real_t baseMultiplier = 0.7;
						knockbackInflicted = true;
						if ( pose == PLAYER_POSE_GOLEM_SMASH )
						{
							baseMultiplier = 0.9;
						}
						else if ( (hitstats->getEffectActive(EFF_WEBBED) || hitstats->getEffectActive(EFF_MAGIC_GREASE)) )
						{
							baseMultiplier = 0.7;

							if ( hitstats->getEffectActive(EFF_MAGIC_GREASE) )
							{
								if ( behavior == &actPlayer )
								{
									achievementObserver.playerAchievements[skill[2]].skidRow++;
								}
							}
						}
						else if ( (myStats->type == GNOME && myStats->weapon && !shapeshifted && myStats->weapon->type == TOOL_PICKAXE) )
						{
							baseMultiplier = 0.7;
							if ( behavior == &actPlayer )
							{
								if ( charge < Stat::getMaxAttackCharge(myStats) / 2 )
								{
									baseMultiplier = 0.5;
								}
							}
						}
						else if ( hit.entity->myconidReboundOnHit(this) )
						{
							baseMultiplier = 0.2 + (hitstats->getEffectActive(EFF_GROWTH) - 1) * 0.25;
							knockbackInflicted = false; // hide message
							hit.entity->playerShakeGrowthHelmet();
						}
						real_t pushbackMultiplier = baseMultiplier;
						if ( !hit.entity->isMobile() )
						{
							pushbackMultiplier += 0.3;
						}
						real_t tangent = atan2(hit.entity->y - this->y, hit.entity->x - this->x);
						if ( hit.entity->behavior == &actMonster )
						{
							hit.entity->vel_x = cos(tangent) * pushbackMultiplier;
							hit.entity->vel_y = sin(tangent) * pushbackMultiplier;
							hit.entity->monsterKnockbackVelocity = 0.05;
							hit.entity->monsterKnockbackUID = this->getUID();
							hit.entity->monsterKnockbackTangentDir = tangent;
							hit.entity->lookAtEntity(*this);
							if ( !(backstab || flanking) )
							{
								if ( hit.entity->monsterAttack == 0 )
								{
									hit.entity->monsterHitTime = std::max(HITRATE - 12, hit.entity->monsterHitTime);
								}
							}
						}
						else if ( hit.entity->behavior == &actPlayer )
						{
							// normalize tangent
							while ( tangent < 0 )
							{
								tangent += 2 * PI;
							}
							while ( tangent > 2 * PI )
							{
								tangent -= 2 * PI;
							}
							if ( !players[hit.entity->skill[2]]->isLocalPlayer() )
							{
								hit.entity->monsterKnockbackVelocity = pushbackMultiplier;
								hit.entity->monsterKnockbackTangentDir = tangent;
								serverUpdateEntityFSkill(hit.entity, 11);
								serverUpdateEntityFSkill(hit.entity, 9);
							}
							else
							{
								hit.entity->monsterKnockbackVelocity = pushbackMultiplier;
								hit.entity->monsterKnockbackTangentDir = tangent;
							}
						}
					}

					// player weapon skills
					if ( damage > 0 && weaponskill == PRO_UNARMED && behavior == &actPlayer && (charge >= Stat::getMaxAttackCharge(myStats) - 3) )
					{
						int chance = 0;
						bool inflictParalyze = false;
						switch ( myStats->getModifiedProficiency(PRO_UNARMED) / 20 )
						{
							case 0:
								break;
							case 1:
								break;
							case 2:
								break;
							case 3:
								break;
							case 4:
								break;
							case 5:
								chance = 1;
								break;
							default:
								break;
						}
						if ( chance > 0 && backstab && !hitstats->getEffectActive(EFF_PARALYZED) && hitstats->HP > 0 )
						{
							int duration = 50;
							if ( hitstats->HP > 0 && hit.entity->setEffect(EFF_PARALYZED, true, duration, true) )
							{
								paralyzeStatusInflicted = true;
								playSoundEntity(hit.entity, 172, 64); //TODO: Paralyze spell sound.
								spawnMagicEffectParticles(hit.entity->x, hit.entity->y, hit.entity->z, 170);
								if ( behavior == &actPlayer ) // redundant; but if this code ever changes...
								{
									steamAchievementClient(skill[2], "BARONY_ACH_ONE_PUNCH_MAN");
									Compendium_t::Events_t::eventUpdateCodex(skill[2], Compendium_t::CPDM_SKILL_LEGENDARY_PROCS, "unarmed skill", 1);
								}
							}
							hit.entity->modHP(-5); // do extra damage.
						}
						if ( !knockbackInflicted && hasMeleeGloves
							&& hit.entity->setEffect(EFF_KNOCKBACK, true, 30, false) )
						{
							real_t baseMultiplier = 0.5;
							if ( myStats->gloves )
							{
								switch ( myStats->gloves->type )
								{
									case BRASS_KNUCKLES:
										baseMultiplier = 0.25;
										break;
									case IRON_KNUCKLES:
										baseMultiplier = 0.35;
										break;
									case SPIKED_GAUNTLETS:
										baseMultiplier = 0.5;
										break;
									default:
										break;
								}
							}
							real_t pushbackMultiplier = baseMultiplier + 0.1 * (myStats->getModifiedProficiency(PRO_UNARMED) / 20);
							/*if ( myStats->shield && hasMeleeGloves )
							{
								pushbackMultiplier /= 2;
							}*/
							if ( !hit.entity->isMobile() )
							{
								pushbackMultiplier += 0.3;
							}
							real_t tangent = atan2(hit.entity->y - this->y, hit.entity->x - this->x);
							if ( hit.entity->behavior == &actMonster )
							{
								hit.entity->vel_x = cos(tangent) * pushbackMultiplier;
								hit.entity->vel_y = sin(tangent) * pushbackMultiplier;
								hit.entity->monsterKnockbackVelocity = 0.05;
								hit.entity->monsterKnockbackUID = this->getUID();
								hit.entity->monsterKnockbackTangentDir = tangent;
								hit.entity->lookAtEntity(*this);
								if ( !(backstab || flanking) )
								{
									if ( hit.entity->monsterAttack == 0 )
									{
										hit.entity->monsterHitTime = std::max(HITRATE - 12, hit.entity->monsterHitTime);
									}
								}
							}
							else if ( hit.entity->behavior == &actPlayer )
							{
								// normalize tangent
								while ( tangent < 0 )
								{
									tangent += 2 * PI;
								}
								while ( tangent > 2 * PI )
								{
									tangent -= 2 * PI;
								}
								if ( !players[hit.entity->skill[2]]->isLocalPlayer() )
								{
									hit.entity->monsterKnockbackVelocity = pushbackMultiplier;
									hit.entity->monsterKnockbackTangentDir = tangent;
									serverUpdateEntityFSkill(hit.entity, 11);
									serverUpdateEntityFSkill(hit.entity, 9);
								}
								else
								{
									hit.entity->monsterKnockbackVelocity = pushbackMultiplier;
									hit.entity->monsterKnockbackTangentDir = tangent;
								}
							}
							knockbackInflicted = true;
						}
					}
					else if ( damage > 0 && behavior == &actPlayer && weaponskill >= PRO_SWORD && weaponskill <= PRO_POLEARM && (charge >= Stat::getMaxAttackCharge(myStats) - 3) )
					{
						// special weapon effects.
						int capstoneDamage = 5;
						if ( weaponskill == PRO_AXE )
						{
							capstoneDamage = 10;
						}
						int chance = 0;
						switch ( myStats->getModifiedProficiency(weaponskill) / 20 )
						{
							case 0:
							case 1:
							case 2:
							case 3:
							case 4:
								break;
							case 5:
								chance = 4;
								break;
							default:
								break;
						}

						if ( weaponskill == PRO_POLEARM )
						{
							// knockback.
							if ( chance > 0 )
							{
								if ( hit.entity->behavior == &actMonster && hit.entity->setEffect(EFF_KNOCKBACK, true, 20, false) )
								{
									real_t pushbackMultiplier = 0.3 + 0.075 * (myStats->getModifiedProficiency(PRO_POLEARM) / 20);
									if ( !hit.entity->isMobile() )
									{
										pushbackMultiplier += 0.3;
									}
									real_t tangent = atan2(hit.entity->y - this->y, hit.entity->x - this->x);
									hit.entity->vel_x = cos(tangent) * pushbackMultiplier;
									hit.entity->vel_y = sin(tangent) * pushbackMultiplier;
									hit.entity->monsterKnockbackVelocity = 0.05;
									hit.entity->monsterKnockbackUID = this->getUID();
									hit.entity->monsterKnockbackTangentDir = tangent;
									hit.entity->lookAtEntity(*this);
									if ( !(backstab || flanking) )
									{
										if ( hit.entity->monsterAttack == 0 )
										{
											hit.entity->monsterHitTime = std::max(HITRATE - 12, hit.entity->monsterHitTime);
										}
									}
									knockbackInflicted = true;
									if ( behavior == &actPlayer )
									{
										Compendium_t::Events_t::eventUpdateCodex(skill[2], Compendium_t::CPDM_SKILL_LEGENDARY_PROCS, "polearm skill", 1);
									}
								}
								hit.entity->modHP(-capstoneDamage); // do the damage
							}
						}
						else if ( weaponskill == PRO_MACE && hitstats->HP > 0 && !(flail && pose == MONSTER_POSE_FLAIL_SWING) )
						{
							// paralyze.
							if ( chance > 0 ) // chance based paralyze
							{
								if ( local_rng.rand() % chance == 0 && !hitstats->getEffectActive(EFF_PARALYZED) )
								{
									int duration = 75; // 1.5 seconds
									if ( hitstats->HP > 0 && hit.entity->setEffect(EFF_PARALYZED, true, duration, true) )
									{
										paralyzeStatusInflicted = true;
										playSoundEntity(hit.entity, 172, 64); //TODO: Paralyze spell sound.
										spawnMagicEffectParticles(hit.entity->x, hit.entity->y, hit.entity->z, 170);
										if ( behavior == &actPlayer )
										{
											Compendium_t::Events_t::eventUpdateCodex(skill[2], Compendium_t::CPDM_SKILL_LEGENDARY_PROCS, "mace skill", 1);
										}
									}
								}
								hit.entity->modHP(-capstoneDamage); // do the damage
							}
						}
						else if ( weaponskill == PRO_AXE && hitstats->HP > 0 )
						{
							// slow.
							if ( chance > 0 ) // always
							{
								int duration = 150; // 3 seconds
								bool wasSlowed = hitstats->getEffectActive(EFF_SLOW);
								if ( hitstats->HP > 0 && hit.entity->setEffect(EFF_SLOW, true, duration, true) && !slowStatusInflicted )
								{
									slowStatusInflicted = true;
									if ( !wasSlowed )
									{
										playSoundEntity(hit.entity, 396 + local_rng.rand() % 3, 64);
									}
									spawnMagicEffectParticles(hit.entity->x, hit.entity->y, hit.entity->z, 171);
									if ( behavior == &actPlayer )
									{
										Compendium_t::Events_t::eventUpdateCodex(skill[2], Compendium_t::CPDM_SKILL_LEGENDARY_PROCS, "axe skill", 1);
									}
								}
								hit.entity->modHP(-capstoneDamage); // do the damage
								// don't re-notify if already inflicted slow from Parashu.
							}
						}
						else if ( weaponskill == PRO_SWORD && hitstats->HP > 0 )
						{
							// bleed.
							if ( chance > 0 ) // always
							{
								spawnMagicEffectParticles(hit.entity->x, hit.entity->y, hit.entity->z, 643);
								playSoundEntity(hit.entity, 173, 128);
								if ( gibtype[hitstats->type] > 0 && gibtype[(int)hitstats->type] != 5 )
								{
									bleedStatusInflicted = true;
									for ( int gibs = 0; gibs < 10; ++gibs )
									{
										Entity* gib = spawnGib(hit.entity);
										serverSpawnGibForClient(gib);
									}
									hit.entity->modHP(-capstoneDamage); // do the damage
									if ( behavior == &actPlayer )
									{
										Compendium_t::Events_t::eventUpdateCodex(skill[2], Compendium_t::CPDM_SKILL_LEGENDARY_PROCS, "sword skill", 1);
									}
								}
								else
								{
									swordExtraDamageInflicted = true;
									int extraDamage = 5;
									hit.entity->modHP(-(extraDamage + capstoneDamage)); // do the damage
								}
							}
						}
					}

					bool playerPoisonedTarget = false;
					bool envenomWeapon = false;

					if ( damage > 0 && myStats->getEffectActive(EFF_ENVENOM_WEAPON) )
					{
						if ( local_rng.rand() % 2 == 0 )
						{
							int envenomDamage = std::min(
								getSpellDamageSecondaryFromID(SPELL_ENVENOM_WEAPON, this, myStats, this),
								getSpellDamageFromID(SPELL_ENVENOM_WEAPON, this, myStats, this));

							hit.entity->modHP(-envenomDamage); // do the damage
							for ( int tmp = 0; tmp < 3; ++tmp )
							{
								Entity* gib = spawnGib(hit.entity, 211);
								serverSpawnGibForClient(gib);
							}
							if ( !hitstats->getEffectActive(EFF_POISONED) )
							{
								envenomWeapon = true;
								hitstats->setEffectActive(EFF_POISONED, 1);

								int duration = TICKS_PER_SECOND * envenomDamage + 10;
								hitstats->EFFECTS_TIMERS[EFF_POISONED] = std::max(160, duration - hit.entity->getCON() * 20);
								hitstats->poisonKiller = getUID();
								if ( playerhit >= 0 )
								{
									messagePlayerMonsterEvent(playerhit, makeColorRGB(255, 0, 0), *myStats, Language::get(6531), Language::get(6532), MSG_COMBAT);
									serverUpdateEffects(playerhit);
								}

								if ( player >= 0 )
								{
									players[player]->mechanics.updateSustainedSpellEvent(SPELL_ENVENOM_WEAPON, 50.0, 1.0, hit.entity);
								}
							}
						}
					}

					// special monster effects
					if ( myStats->type == CRYSTALGOLEM && pose == MONSTER_POSE_GOLEM_SMASH )
					{
						if ( damage >= 150 && hit.entity->behavior == &actPlayer && playerhit >= 0 )
						{
							if ( hitstats && hitstats->HP > 0 )
							{
								steamAchievementClient(playerhit, "BARONY_ACH_SPONGE");
							}
						}
						if ( multiplayer != CLIENT )
						{
							createParticleRock(hit.entity);
							if ( multiplayer == SERVER )
							{
								serverSpawnMiscParticles(hit.entity, PARTICLE_EFFECT_ABILITY_ROCK, 78);
							}
							if ( target == nullptr )
							{
								// only play sound once on primary target.
								playSoundEntity(hit.entity, 181, 64);
							}
						}
					}
					else if ( myStats->type == BUGBEAR && pose == MONSTER_POSE_BUGBEAR_SHIELD )
					{
						if ( hit.entity->setEffect(EFF_KNOCKBACK, true, 30, false) )
						{
							real_t pushbackMultiplier = 0.9;
							knockbackInflicted = true;

							real_t tangent = atan2(hit.entity->y - this->y, hit.entity->x - this->x);
							if ( hit.entity->behavior == &actMonster )
							{
								hit.entity->vel_x = cos(tangent) * pushbackMultiplier;
								hit.entity->vel_y = sin(tangent) * pushbackMultiplier;
								hit.entity->monsterKnockbackVelocity = 0.01;
								hit.entity->monsterKnockbackUID = this->getUID();
								hit.entity->monsterKnockbackTangentDir = tangent;
								//hit.entity->lookAtEntity(*parent);
							}
							else if ( hit.entity->behavior == &actPlayer )
							{
								if ( !players[hit.entity->skill[2]]->isLocalPlayer() )
								{
									hit.entity->monsterKnockbackVelocity = pushbackMultiplier;
									hit.entity->monsterKnockbackTangentDir = tangent;
									serverUpdateEntityFSkill(hit.entity, 11);
									serverUpdateEntityFSkill(hit.entity, 9);
								}
								else
								{
									hit.entity->monsterKnockbackVelocity = pushbackMultiplier;
									hit.entity->monsterKnockbackTangentDir = tangent;
								}
							}
						}
					}
					else if ( myStats->type == BAT_SMALL )
					{
						if ( !hitstats->getEffectActive(EFF_BLEEDING) )
						{
							if ( !hitstats->defending && parriedDamage == 0 )
							{
								if ( hit.entity->setEffect(EFF_BLEEDING, true, 6 * TICKS_PER_SECOND, false) )
								{
									statusInflicted = true;
									messagePlayer(playerhit, MESSAGE_COMBAT, Language::get(701));
								}
							}
						}
					}
					else if ( myStats->type == MIMIC && local_rng.rand() % 4 == 0 )
					{
						Item* armor = nullptr;
						int armornum = 0;
						if ( behavior == &actPlayer 
							|| (hit.entity->behavior == &actMonster
								&& ((hit.entity->monsterAllySummonRank != 0 && hitstats->type == SKELETON)
									|| hit.entity->monsterIsTinkeringCreation()
									|| hitstats->type == MONSTER_ADORCISED_WEAPON
									|| hitstats->type == REVENANT_SKULL)) )
						{
							armor = nullptr;
						}
						else
						{
							if ( hitstats->defending && hitstats->shield && itemCategory(hitstats->shield) == ARMOR )
							{
								// try eat shield
								armornum = hitstats->pickRandomEquippedItem(&armor, true, false, true, true);
								if ( !armor )
								{
									armornum = hitstats->pickRandomEquippedItem(&armor, true, false, false, false);
								}
							}
							else
							{
								armornum = hitstats->pickRandomEquippedItem(&armor, true, false, false, false);
							}
						}
						if ( armor != nullptr )
						{
							bool mimicSentToVoid = false;
							if ( myStats->getEffectActive(EFF_MIMIC_VOID) )
							{
								int qty = 1;
								if ( itemTypeIsQuiver(armor->type) )
								{
									qty = armor->count;
								}

								Item* stolenArmor = newItem(armor->type, armor->status, armor->beatitude, qty, armor->appearance, armor->identified, nullptr);
								if ( Item* chestItem = Entity::addItemToVoidChestServer(-1, stolenArmor, true, nullptr) )
								{
									if ( chestItem != stolenArmor )
									{
										free(stolenArmor);
									}
									mimicSentToVoid = true;
								}
								else
								{
									free(stolenArmor);
								}
							}

							if ( armor )
							{
								int qty = 1;
								int startCount = armor->count;
								if ( itemTypeIsQuiver(armor->type) )
								{
									qty = armor->count;
									armor->count = 0;
								}
								else
								{
									armor->count--;
								}
								if ( hit.entity->behavior == &actPlayer && playerhit >= 0 )
								{
									steamStatisticUpdateClient(playerhit, STEAM_STAT_I_NEEDED_THAT, STEAM_STAT_INT, 1);
								}
								if ( !mimicSentToVoid )
								{
									messagePlayer(playerhit, MESSAGE_COMBAT, Language::get(6085), armor->getName());
									Item* stolenArmor = newItem(armor->type, armor->status, armor->beatitude, qty, armor->appearance, armor->identified, &myStats->inventory);
									stolenArmor->ownerUid = hit.entity->getUID();
									stolenArmor->isDroppable = armor->isDroppable;
									if ( myStats->getEffectActive(EFF_MIMIC_VOID) )
									{
										dropItemMonster(stolenArmor, this, myStats, stolenArmor->count);
									}
								}
								else
								{
									messagePlayer(playerhit, MESSAGE_COMBAT, Language::get(6567), armor->getName());
								}
								if ( playerhit > 0 && multiplayer == SERVER && !players[playerhit]->isLocalPlayer() )
								{
									strcpy((char*)net_packet->data, "STLA");
									net_packet->data[4] = armornum;
									SDLNet_Write32(static_cast<Uint32>(armor->type), &net_packet->data[5]);
									SDLNet_Write32(static_cast<Uint32>(armor->status), &net_packet->data[9]);
									SDLNet_Write32(static_cast<Uint32>(armor->beatitude), &net_packet->data[13]);
									SDLNet_Write32(static_cast<Uint32>(startCount), &net_packet->data[17]);
									SDLNet_Write32(static_cast<Uint32>(armor->appearance), &net_packet->data[21]);
									net_packet->data[25] = armor->identified;
									net_packet->address.host = net_clients[playerhit - 1].host;
									net_packet->address.port = net_clients[playerhit - 1].port;
									net_packet->len = 26;
									sendPacketSafe(net_sock, -1, net_packet, playerhit - 1);
								}

								if ( armor->count <= 0 )
								{
									Item** slot = itemSlot(hitstats, armor);
									if ( slot )
									{
										*slot = NULL;
									}
									if ( armor->node )
									{
										list_RemoveNode(armor->node);
									}
									else
									{
										free(armor);
									}
								}
							}
						}
					}
					else if ( (damage > 0 || hitstats->getEffectActive(EFF_PACIFY) 
						|| hitstats->getEffectActive(EFF_FEAR) 
						|| hitstats->getEffectActive(EFF_COWARDICE) ) && local_rng.rand() % 4 == 0 )
					{
						switch ( myStats->type )
						{
						case SCORPION:
							hitstats->setEffectActive(EFF_PARALYZED, 1);
							hitstats->EFFECTS_TIMERS[EFF_PARALYZED] = std::max(50, 150 - hit.entity->getCON() * 5);
							messagePlayer(playerhit, MESSAGE_COMBAT, Language::get(684));
							messagePlayer(playerhit, MESSAGE_COMBAT, Language::get(685));
							serverUpdateEffects(playerhit);
							break;
						case SCARAB:
							if ( myStats->getAttribute("SCARAB_GREATER_CURSE") != "" )
							{
								if ( hit.entity->setEffect(EFF_WEAKNESS, (Uint8)5, hitstats->EFFECTS_TIMERS[EFF_WEAKNESS] + 30 * TICKS_PER_SECOND,
									false, true, false) )
								{
									messagePlayer(playerhit, MESSAGE_COMBAT, Language::get(6964));
									spawnMagicEffectParticles(hit.entity->x, hit.entity->y, hit.entity->z, 2367);
								}
							}
							else
							{
								if ( hit.entity->setEffect(EFF_WEAKNESS, (Uint8)2, hitstats->EFFECTS_TIMERS[EFF_WEAKNESS] + 15 * TICKS_PER_SECOND,
									false, true, false) )
								{
									messagePlayer(playerhit, MESSAGE_COMBAT, Language::get(6963));
									spawnMagicEffectParticles(hit.entity->x, hit.entity->y, hit.entity->z, 2367);
								}
							}
							break;
						case SPIDER:
						{
							bool applyPoison = true;
							if ( behavior == &actPlayer )
							{
								if ( charge >= Stat::getMaxAttackCharge(myStats) - 3 ) // fully charged strike injects venom.
								{
									applyPoison = true;
								}
								else
								{
									applyPoison = false;
								}
							}
							if ( applyPoison )
							{
								playerPoisonedTarget = true;
								hitstats->setEffectActive(EFF_POISONED, 1);
								hitstats->EFFECTS_TIMERS[EFF_POISONED] = std::max(200, 600 - hit.entity->getCON() * 20);
								hitstats->poisonKiller = getUID();
								if ( arachnophobia_filter ) {
									messagePlayer(playerhit, MESSAGE_COMBAT, Language::get(4089));
								}
								else {
									messagePlayer(playerhit, MESSAGE_COMBAT, Language::get(686));
								}
								//messagePlayer(playerhit, MESSAGE_COMBAT, Language::get(687));
								serverUpdateEffects(playerhit);
								for ( int tmp = 0; tmp < 3; ++tmp )
								{
									Entity* gib = spawnGib(hit.entity, 211);
									serverSpawnGibForClient(gib);
								}
							}
							break;
						}
						case SUCCUBUS:
						{
							Item* armor = nullptr;
							int armornum = 0;
							if ( behavior == &actPlayer 
								|| (hit.entity->behavior == &actMonster
									&& ( (hit.entity->monsterAllySummonRank != 0 && hitstats->type == SKELETON)
									|| hit.entity->monsterIsTinkeringCreation())) )
							{
								armor = nullptr;
							}
							else
							{
								if ( !monsterAllyGetPlayerLeader() )
								{
									if ( currentlevel >= 10 )
									{
										armornum = hitstats->pickRandomEquippedItem(&armor, true, false, false, false);
									}
									else
									{
										if ( local_rng.rand() % 4 == 0 )
										{
											armornum = hitstats->pickRandomEquippedItem(&armor, true, false, false, false);
										}
									}
								}
								else
								{
									if ( local_rng.rand() % 8 == 0 )
									{
										armornum = hitstats->pickRandomEquippedItem(&armor, true, false, false, false);
									}
								}
							}
							if ( armor != nullptr )
							{
								int startCount = armor->count;
								int qty = 1;
								if ( itemTypeIsQuiver(armor->type) )
								{
									qty = armor->count;
									armor->count = 0;
								}
								else
								{
									armor->count--;
								}
								messagePlayer(playerhit, MESSAGE_COMBAT, Language::get(688), armor->getName());
								Item* stolenArmor = newItem(armor->type, armor->status, armor->beatitude, qty, armor->appearance, armor->identified, &myStats->inventory);
								stolenArmor->ownerUid = hit.entity->getUID();
								if ( playerhit > 0 && multiplayer == SERVER && !players[playerhit]->isLocalPlayer() )
								{
									strcpy((char*)net_packet->data, "STLA");
									net_packet->data[4] = armornum;
									SDLNet_Write32(static_cast<Uint32>(armor->type), &net_packet->data[5]);
									SDLNet_Write32(static_cast<Uint32>(armor->status), &net_packet->data[9]);
									SDLNet_Write32(static_cast<Uint32>(armor->beatitude), &net_packet->data[13]);
									SDLNet_Write32(static_cast<Uint32>(startCount), &net_packet->data[17]);
									SDLNet_Write32(static_cast<Uint32>(armor->appearance), &net_packet->data[21]);
									net_packet->data[25] = armor->identified;
									net_packet->address.host = net_clients[playerhit - 1].host;
									net_packet->address.port = net_clients[playerhit - 1].port;
									net_packet->len = 26;
									sendPacketSafe(net_sock, -1, net_packet, playerhit - 1);
								}

								if ( armor->count <= 0 )
								{
									Item** slot = itemSlot(hitstats, armor);
									if ( slot )
									{
										*slot = NULL;
									}
									if ( armor->node )
									{
										list_RemoveNode(armor->node);
									}
									else
									{
										free(armor);
									}
								}

								teleportRandom();

								// the succubus loses interest after this
								monsterState = 0;
								monsterTarget = 0;
							}
							break;
						}
						default:
							break;
						}

						if ( myStats->weapon && myStats->weapon->type == BRANCH_STAFF && !shapeshifted )
						{
							if ( !hitstats->getEffectActive(EFF_POISONED) )
							{
								envenomWeapon = true;
								hitstats->setEffectActive(EFF_POISONED, 1);
								hitstats->EFFECTS_TIMERS[EFF_POISONED] = std::max(200, 600 - hit.entity->getCON() * 20);
								hitstats->poisonKiller = getUID();
								if ( playerhit >= 0 )
								{
									messagePlayerMonsterEvent(playerhit, makeColorRGB(255, 0, 0), *myStats, Language::get(6531), Language::get(6532), MSG_COMBAT);
								}
								serverUpdateEffects(playerhit);
								for ( int tmp = 0; tmp < 3; ++tmp )
								{
									Entity* gib = spawnGib(hit.entity, 211);
									serverSpawnGibForClient(gib);
								}
							}
						}
					}
					else if ( damage == 0 && !hitstats->defending && parriedDamage == 0 )
					{
						// special chance effects when damage is 0.
						if ( local_rng.rand() % 20 == 0 )
						{
							switch ( myStats->type )
							{
								case SCORPION:
									hitstats->setEffectActive(EFF_PARALYZED, 1);
									hitstats->EFFECTS_TIMERS[EFF_PARALYZED] = std::max(50, 150 - hit.entity->getCON() * 5);
									messagePlayer(playerhit, MESSAGE_COMBAT, Language::get(684));
									messagePlayer(playerhit, MESSAGE_COMBAT, Language::get(685));
									serverUpdateEffects(playerhit);
									statusInflicted = true;
									break;
								case SCARAB:
									if ( myStats->getAttribute("SCARAB_GREATER_CURSE") != "" )
									{
										if ( hit.entity->setEffect(EFF_WEAKNESS, (Uint8)5, hitstats->EFFECTS_TIMERS[EFF_WEAKNESS] + 30 * TICKS_PER_SECOND,
											false, true, false) )
										{
											messagePlayer(playerhit, MESSAGE_COMBAT, Language::get(6964));
											spawnMagicEffectParticles(hit.entity->x, hit.entity->y, hit.entity->z, 2367);
										}
									}
									else
									{
										if ( hit.entity->setEffect(EFF_WEAKNESS, (Uint8)2, hitstats->EFFECTS_TIMERS[EFF_WEAKNESS] + 15 * TICKS_PER_SECOND,
											false, true, false) )
										{
											messagePlayer(playerhit, MESSAGE_COMBAT, Language::get(6963));
											spawnMagicEffectParticles(hit.entity->x, hit.entity->y, hit.entity->z, 2367);
										}
									}
									break;
								case SPIDER:
									if ( behavior != &actPlayer )
									{
										hitstats->setEffectActive(EFF_POISONED, 1);
										hitstats->EFFECTS_TIMERS[EFF_POISONED] = std::max(200, 300 - hit.entity->getCON() * 20);
										if (arachnophobia_filter) {
										    messagePlayer(playerhit, MESSAGE_COMBAT, Language::get(4089));
										} else {
										    messagePlayer(playerhit, MESSAGE_COMBAT, Language::get(686));
										}
										//messagePlayer(playerhit, MESSAGE_COMBAT, Language::get(687));
										serverUpdateEffects(playerhit);
										statusInflicted = true;
									}
									break;
								default:
									break;
							}
						}
					}

					if ( player >= 0 && hit.entity->behavior == &actMonster )
					{
						bool oldRhythmStatus = achievementStatusRhythmOfTheKnight[player];
						updateAchievementRhythmOfTheKnight(player, hit.entity, false);
						if ( !oldRhythmStatus && achievementStatusRhythmOfTheKnight[player] )
						{
							//messagePlayer(0, MESSAGE_DEBUG, "rhythm roll on atk");
							if ( local_rng.rand() % 10 < 8 )
							{
								bool increaseSkill = true;
								if ( this->behavior == &actPlayer )
								{
									if ( !players[this->skill[2]]->mechanics.allowedRaiseBlockingAgainstEntity(*hit.entity) )
									{
										increaseSkill = false;
									}
									if ( myStats->shield && itemCategory(myStats->shield) != ARMOR )
									{
										if ( myStats->getProficiency(PRO_SHIELD) >= SKILL_LEVEL_SKILLED )
										{
											increaseSkill = false;
										}
									}
									if ( increaseSkill )
									{
										players[this->skill[2]]->mechanics.enemyRaisedBlockingAgainst[hit.entity->getUID()]++;
									}
								}
								if ( increaseSkill )
								{
									if ( myStats->getEffectActive(EFF_FORCE_SHIELD) )
									{
										magicOnSpellCastEvent(this, this, hit.entity, SPELL_FORCE_SHIELD, spell_t::SpellOnCastEventTypes::SPELL_LEVEL_EVENT_DEFAULT, 1);
									}
									this->increaseSkill(PRO_SHIELD);
								}
							}
							achievementStatusRhythmOfTheKnight[player] = false;
							achievementRhythmOfTheKnightVec[player].clear(); // reset for the next one
						}
					}

