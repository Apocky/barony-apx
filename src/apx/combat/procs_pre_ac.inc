// VERBATIM from Entity::attack (entity.cpp) as of the D93 C1 port; the core runs it for player melee. Do not edit without checking the legacy copy.
					if ( weaponskill == PRO_POLEARM && myStats->weapon && myStats->weapon->type == ARTIFACT_SPEAR && !shapeshifted )
					{
						real_t amount = 0.f;
						real_t percent = getArtifactWeaponEffectChance(ARTIFACT_SPEAR, *myStats, &amount);
						if ( (local_rng.rand() % 100 < static_cast<int>(percent)) )
						{
							enemyAC *= amount;
							gugnirProc = true;
						}
					}
					else if ( whipPierce )
					{
						enemyAC *= 0.5;
						armorPierceProc = true;
					}
					else if ( myStats->getEffectActive(EFF_ENSEMBLE_DRUM) )
					{
						real_t amount = (100.0 - myStats->getEnsembleEffectBonus(Stat::ENSEMBLE_DRUM_TIER)) / 100.0;
						if ( (charge >= Stat::getMaxAttackCharge(myStats) - 3) )
						{
							if ( /*(local_rng.rand() % 100 < 25)*/true )
							{
								enemyAC *= amount;
								armorPierceProc = true;
							}
						}
					}

