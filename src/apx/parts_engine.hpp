// F6 parts-based gear, Barony side (card BF6-2/3/4/6). APX_PARTS=0 skips every path below.
#pragma once
#include <cstdint>
#include <string>

class Item;
class Entity;

struct ApxPartsCounters
{
	long long bound = 0, parts_overflow = 0, ip_clamped = 0, ip_rerolls = 0, forms_unmapped = 0, model_fallback = 0,
		net_blob_sent = 0, net_blob_dropped = 0, net_blob_mismatch = 0, save_blob_dropped = 0, salvaged = 0, aura_stand_in = 0,
		salvage_grams_out = 0, salvage_grams_paid = 0, components_given = 0;
};
extern ApxPartsCounters apxPartsCounters;

bool apxPartsEnabled();                                  // env APX_PARTS (default 1)
void apxPartsLevelStart();                               // per-floor roll index back to 0 (end of assignActions)
void apxPartsBind(Item* item, int source, int depthOverride = 0); // roll the parts of a loot item (weapon, armour, tool); no-op when bound; depthOverride: selftest forced roll
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
bool apxPartsSalvageValue(int player, const Item* item, int* metal, int* magic, int* auraCrystals);
long long apxPartsLedgerGrams(int player); // the material pouch (D85): salvaged grams held for the player
int apxPartsPouchWeight(int player);
// D85 bundles: material drawn out of the pouch as one real item (stand-in type GEM_GLASS; Item::appearance
// = 0xB tag in bits 28-31, material id bits 20-27, grams bits 0-19, up to 1,048,575 g). Barony's own
// appearance paths (floor entities, ITEM/DROP/CITM packets, saves) carry it unchanged. A local player's
// pickup of a bundle returns it to the pouch on the next frame; bundles the player drew stay items.
bool apxPartsIsBundle(const Item* item);
int apxPartsBundleMaterial(const Item* item);
int apxPartsBundleGrams(const Item* item);
Item* apxPartsDrawBundle(int player, int material, int grams, std::string& why); // debits the pouch
void apxPartsBundlePickedUp(int player, const Item* item);   // itemPickup: a bundle arrived for player
void apxPartsBundlesRegister(int player);                    // loadGame: bundles in the inventory stay items
void apxPartsTick();                                         // game loop: absorb picked-up bundles
const char* apxPartsBundleName(const Item* item);            // "bundle of iron (0.6 kg)"
int apxPartsLedgerMaterialGrams(int player, int material);
int apxPartsBundleValue(const Item* item);                    // gold: grams x material value_per_kg / 1000
void apxPartsLedgerCredit(int player, int material, int grams); // selftest seed for the pouch (logged)
// D86 components: one part as its own item (stand-in type GEM_GLASS, apx.form = COMPONENT_FORM, nParts 1;
// appearance 0xC tag | slot << 8 | material, cosmetic only). Saves, packets and floor items carry it like
// any item with parts; it never stacks. Name from its affixes ("Keen Steel Blade of Embers").
bool apxPartsIsComponent(const Item* item);
const char* apxPartsComponentName(const Item* item);
int apxPartsComponentGrams(const Item* item);
int apxPartsComponentValue(const Item* item);
struct ApxItemData;
Item* apxPartsGiveComponent(int player, const ApxItemData& data); // into the player's inventory (itemPickup)
// Trade lane (D73/D85): material ids index materials.json; Give/Take move grams in/out of the pouch
// (server side; logged); Take is false when the pouch is short. The D85 bundle is apxPartsIsBundle.
int apxPartsMaterialIndex(const char* id);
bool apxPartsLedgerGive(int player, int material, int grams);
bool apxPartsLedgerTake(int player, int material, int grams);
int apxPartsMaterialCount();
// bench rows (U1 provider): one part's component name and its affix words
const char* apxPartsPartName(const Item* item, int part);
int apxPartsAffixCount(const Item* item, int part);
const char* apxPartsAffixWord(const Item* item, int part, int a);
// Palette tint (D79 stand-in until the procedural voxel tool): the held weapon takes its main part's
// material tint (materials.json "tint") through Entity::lightBonus; APX_PARTS_VIS=0 = vanilla look.
void apxPartsApplyTint(Entity* entity, const Item* item);
void apxPartsBenchRegister();                            // F6 provider into the BU1 bench shell (parts_bench.cpp)
const char* apxPartsShotRequest();                       // the probe's framebuffer shot path, once (game.cpp frame end)
void apxPartsProbeTick();                                // APX_BF6_PROBE, once floor 1 is up (game loop)

// Formbook (card BF6-6; 05 section 5): per player, Form id (= apx.form, the family) -> successes 0-5;
// known at 1, mastered at 5. Breaking an item with parts teaches its Form and consumes the item.
// A new Form needs a free input slot: known Forms < max(1, INT / 2) (Wit stand-in; ASSUMED reading).
int apxPartsFormSuccesses(int player, uint32_t form);
int apxPartsFormInputCap(int player);
bool apxPartsBreak(int player, Item* item, std::string& why);     // consumes the item when it returns true
void apxPartsFormbookRecv(const uint8_t* packet, int len);        // server: an owner client's FBOK
void apxPartsFormbookFromSave(int player);                        // loadGame: the book read with the save
uint64_t apxPartsFormbookHash(int player);
// Salvage with the keep-one-part choice (the BU1 bench calls this; the vanilla station keeps none):
// keepPart = part index (0..nParts-1) or -1. Scrap and aura crystals go to the player; the item keeps
// only the kept part, or is consumed when keepPart is -1.
bool apxPartsSalvageItem(int player, Item* item, int keepPart, std::string& why);
const char* apxPartsFamilyName(uint32_t form);
int apxPartsPartMassGrams(const Item* item, int part);
const char* apxPartsMaterialName(int material);
const char* apxPartsSlotName(int slot);
