// VERBATIM from Entity::attack (entity.cpp) as of the D93 C1 port; the core runs it for player melee. Do not edit without checking the legacy copy.
					Item** weaponToBreak = nullptr;
					ItemType weaponType = static_cast<ItemType>(WOODEN_SHIELD);
					bool hasMeleeGloves = false;
					if ( myStats->gloves && !shapeshifted )
					{
						switch ( myStats->gloves->type )
						{
						case BRASS_KNUCKLES:
						case IRON_KNUCKLES:
						case SPIKED_GAUNTLETS:
							hasMeleeGloves = true;
							break;
						default:
							break;
						}
					}
					if ( myStats->weapon && !shapeshifted )
					{
						weaponToBreak = &myStats->weapon;
					}
					else if ( hasMeleeGloves )
					{
						weaponToBreak = &myStats->gloves;
					}
					if ( weaponToBreak != nullptr && !shapeshifted )
					{
						weaponType = (*weaponToBreak)->type;
					}

