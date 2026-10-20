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
		salvage_grams_out = 0, salvage_grams_paid = 0, components_given = 0, pouch_scrap_spent = 0, parts_repaired = 0, grafts = 0, graft_refused = 0, stripped = 0, aura_given = 0, infused = 0, infuse_refused = 0, assembled = 0, assemble_refused = 0, swapped = 0, swap_refused = 0, made = 0;
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
void apxPartsDecorateName(const Item* item, char* name, size_t size); // Item::getName: assembly names (05b section 4); APX_PARTS_NAMES=0 = vanilla
const char* apxPartsComponentName(const Item* item);
int apxPartsComponentGrams(const Item* item);
int apxPartsComponentValue(const Item* item);
struct ApxItemData;
Item* apxPartsGiveComponent(int player, const ApxItemData& data, bool identified = true); // into the player's inventory (itemPickup)
// Trade: loose items in no inventory (shop stock); the caller owns them. A bundle bought through
// itemPickup goes back into the buyer's pouch by itself (apxPartsTick), or call apxPartsLedgerGive.
Item* apxPartsMakeComponent(const ApxItemData& data, bool identified = true);
Item* apxPartsMakeBundle(int material, int grams);
// D85: Barony tinkering reads the pouch: cls 0 = metal, 1 = magic (salvage.json grams per unit)
int apxPartsPouchScrapUnits(int player, int cls);
bool apxPartsRepairPart(int player, Item* item, int part, std::string& why);
Item* apxPartsExtractGraft(int player, Item* item, int station, std::string& why); // C7: Rare+ assembly -> graft component (item destroyed)
int apxPartsSockets(const Item* item);                                             // C7: 0-4 = the assembly's rarity // 05b s5.8b, pouch grams of the part's material
bool apxPartsPouchTakeScrap(int player, int cls, int units);
// C6 aura crystals (05b 5.9): GEM_GLASS stand-in, appearance 0xA tag | affix id; same-affix crystals stack.
bool apxPartsIsAura(const Item* item);
int apxPartsAuraAffix(const Item* item);
const char* apxPartsAuraName(const Item* item);
Item* apxPartsMakeAura(int affix, int count, bool identified = true);       // loose (shop stock)
void apxPartsGivePendingAura(int player);                                  // tinkering station: after it grants the scrap
bool apxPartsInfuse(int player, Item* item, int part, uint32_t crystalUid, int station, std::string& why); // station 3 = crucible
// Trade lane (D73/D85): material ids index materials.json; Give/Take move grams in/out of the pouch
// (server side; logged); Take is false when the pouch is short. The D85 bundle is apxPartsIsBundle.
int apxPartsMaterialIndex(const char* id);
bool apxPartsLedgerGive(int player, int material, int grams);
bool apxPartsLedgerTake(int player, int material, int grams);
int apxPartsMaterialCount();
// C4/C5 (05b section 5 steps 3, 7, 8): bench operations on the local inventory; refusals say why and count.
#include <vector>
std::vector<int> apxPartsFamilySlots(int family, std::vector<int>* qgate);   // required slots first, then optional (with gates)
int apxPartsRequiredCount(int family);
std::vector<uint32_t> apxPartsComponentsForSlot(int player, int slot);       // component item uids for that slot
Item* apxPartsAssemble(int player, int family, int station, const std::vector<std::pair<int, uint32_t>>& slotUid, std::string& why);
bool apxPartsSwapAllowed(const Item* item, int part);                      // D87: looted = quick slots only
bool apxPartsIsQuickSlot(int slot);
int apxPartsStrip(int player, Item* item, std::string& why);                 // 05b 5.4: components given (-1 = refused)
std::vector<std::string> apxPartsFits(const Item* item);                     // "Blades Grip", ... for a loose component
bool apxPartsAssemblePreview(int player, int family, const std::vector<std::pair<int, uint32_t>>& slotUid, int& power, int& cap, std::string& why);
std::string apxPartsDisplayName(const Item* item);                           // "" = Barony's own
bool apxPartsSwap(int player, Item* item, int part, uint32_t componentUid, std::string& why); // old part -> component item
int apxPartsMakeCost(int slot, int material);                              // grams
Item* apxPartsMake(int player, int slot, int material, int station, uint32_t look, std::string& why); // spends pouch grams; look = BX1 model hash (0 = procedural)
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
bool apxPartsLedgerGive(int player, const char* material, int grams); // BV1: a creature's material drop goes into the player's ledger
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
// BP1-2 repair (field bench, anvil): one status step, paid from the player's salvage ledger in grams
// of the main part's material (props_rules repairCostGrams); -1 = cannot (why says)
int apxPartsRepairCost(int player, const Item* item, std::string& why, std::string& material);
bool apxPartsRepair(int player, Item* item, std::string& why);
bool apxPartsRepairPaid(int player, Item* item, std::string& why);
int apxPartsRepairGrams(const Item* item, std::string& why, std::string& material); // no ledger check (merchant price) // merchant: same rule, no ledger debit (gold charged by the shop)
const char* apxPartsFamilyName(uint32_t form);
int apxPartsPartMassGrams(const Item* item, int part);
const char* apxPartsMaterialName(int material);
const char* apxPartsSlotName(int slot);
