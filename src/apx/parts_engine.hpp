// F6 parts-based gear, Barony side (card BF6-2/3/4/6). APX_PARTS=0 skips every path below.
#pragma once
#include <cstdint>
#include <string>

class Item;
class Entity;

struct ApxPartsCounters
{
	long long bound = 0, parts_overflow = 0, ip_clamped = 0, ip_rerolls = 0, forms_unmapped = 0, model_fallback = 0,
		net_blob_sent = 0, net_blob_dropped = 0, net_blob_mismatch = 0, save_blob_dropped = 0, salvaged = 0, aura_stand_in = 0;
};
extern ApxPartsCounters apxPartsCounters;

bool apxPartsEnabled();                                  // env APX_PARTS (default 1)
void apxPartsLevelStart();                               // per-floor roll index back to 0 (end of assignActions)
void apxPartsBind(Item* item, int source);               // roll the parts of a loot item (weapon, armour, tool); no-op when bound
void apxPartsBindInventory(void* list, int source);      // every item of a list_t (chest or monster inventory)
void apxPartsToFloor(Entity* entity, const Item* item);  // a dropped item's parts ride with its floor entity (server map)
void apxPartsBindFloorEntity(Entity* entity);            // floor items placed by level generation
void apxPartsFromFloor(const Entity* entity, Item* item);// pickup: the entity's parts into the new item
int apxPartsNetAppend(const Item* item, uint8_t* packet, int len); // returns the new packet length
void apxPartsNetRead(Item* item, const uint8_t* packet, int len, int at); // blob after byte `at` when len > at
std::string apxPartsSaveString(const Item* item);
void apxPartsLoadString(Item* item, const std::string& s);
// Saves: Barony's save (JSON or binary, no field names in binary) stays byte-identical; the parts go
// to a sidecar <save>.apxparts (lines "player index v1;..."), keyed like populateFromSession's
// inventory order, which loadGame's restore loop repeats.
void apxPartsSidecarSave(const char* savePath);
void apxPartsSidecarLoad(const char* savePath);
const std::string& apxPartsSidecarGet(int player, int inventoryIndex);
// Tinkering station (interface.cpp tinkeringSalvageItem): for an item with parts the metal and magic
// scrap come from the parts salvage rules (05 section 5); aura crystals are returned for the caller to
// give (GEM_ROCK stand-in). v1 has no keep-one-part choice (that needs the BU1 bench UI): every part
// returns. False = a vanilla item, the caller keeps its vanilla numbers.
bool apxPartsSalvageValue(const Item* item, int* metal, int* magic, int* auraCrystals);
void apxPartsProbeTick();                                // APX_BF6_PROBE, once floor 1 is up (game loop)
