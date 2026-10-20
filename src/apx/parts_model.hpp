// F6 parts-based gear: the pure model (card BF6, specs/barony-fork/05; data design 19-GEAR-PARTS).
// No Barony header here: parts_*.cpp are tested offline by tests/apx/parts_test.cpp.
// TRAP (card): Barony mallocs, frees and memcpys Item, so ApxItemData is plain old data (no
// pointer, no std member). All zero = a vanilla item (nParts 0).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

constexpr int APX_MAX_PARTS = 12;   // card: v1 cap; overflow is counted (parts_overflow)

#pragma pack(push, 1)
struct ApxPart
{
	uint8_t slot, material, quality, rarity /*0-5*/, integrity /*0-255 = 0.0-1.0*/, nAffix;
	uint16_t affix[4];
	uint8_t maker, model; // model: the component's shape id (0 = the default; C10 voxel builder, 05b 5.8)
};
struct ApxItemData
{
	uint32_t form, rollKey;
	uint16_t power;
	uint8_t nParts, flags; // flags: bits 0-2 sockets, 3-5 resonance element, 6 crafted, 7 looted
	ApxPart parts[APX_MAX_PARTS];
};
#pragma pack(pop)
static_assert(sizeof(ApxPart) == 16, "ApxPart is 16 bytes (card)");
static_assert(sizeof(ApxItemData) == 204, "ApxItemData is 204 bytes (card)");

namespace apxparts {

enum Source : uint8_t { SRC_CHEST = 0, SRC_MONSTER = 1, SRC_FLOOR = 2, SRC_SHOP = 3, SRC_CRAFT = 4 };
enum Role : uint8_t { ROLE_STRIKE = 0, ROLE_STRUCTURAL, ROLE_BALANCE, ROLE_GRIP, ROLE_OTHER };
constexpr uint8_t FLAG_CRAFTED = 1 << 6, FLAG_LOOTED = 1 << 7;
// D86 component: one part as its own item (form = COMPONENT_FORM, nParts 1). It rides every path an
// item with parts already rides (save sidecar, packet blob, floor map) and never stacks.
constexpr uint32_t COMPONENT_FORM = 0xC0000000u;
inline bool isComponent(const ApxItemData& d) { return d.form == COMPONENT_FORM && d.nParts == 1; }

struct Material { std::string id; int density = 0, hardness = 0, tier = 1, valuePerKg = 0, meltC = 0, flammable = 0, element = 0; std::string salvageClass; float tint[3] = { 0, 0, 0 }; };
struct Slot { std::string id; Role role = ROLE_OTHER; int volume = 0; bool quick = false; }; // quick: swaps on looted assemblies (05b 5.3)
struct OptSlot { int slot = 0, q = 0; };
struct Family { std::string id; std::vector<int> required; std::vector<OptSlot> optional; };
struct TypeForm { int family = -1; std::vector<int> materials; };
struct AffixPool { std::string key; int firstId = 0, count = 0; };
// D86 affix entry: position (prefix "Keen ..." or suffix "... of Embers"), the label used in the name,
// the named stat or effect, and a rarity weight. Missing fields default to suffix / Capitalised name /
// name / 1 (counted in DB::affixDefaults so the data lane can see what is still unset).
enum AffixPos : uint8_t { AFFIX_PREFIX = 0, AFFIX_SUFFIX = 1 };
struct AffixInfo { std::string name, label, stat; int pct = 0, weight = 1; AffixPos position = AFFIX_SUFFIX; };
struct StandIn { std::string cls, item; int grams = 0; };
struct SalvageParams { int lootedPct = 60, craftedPct = 100, dischargePct = 20, auraPerAffixPct = 25, lowIntegrityByte = 77, lowIntegrityFactorPct = 50; std::string auraItem; std::vector<StandIn> standIns; };

struct DB
{
	std::vector<Material> materials;
	std::vector<Slot> slots;
	std::vector<Family> families;
	std::vector<std::pair<std::string, TypeForm>> typeForms; // vanilla ItemType name -> form (sorted by name)
	std::vector<AffixPool> pools;
	std::vector<std::pair<std::string, int>> affixPct;      // global affix id -> (name, pct)
	std::vector<AffixInfo> affixes;                         // global affix id -> D86 entry (same order)
	int affixDefaults = 0;                                  // entries with no position/label in the data
	SalvageParams salvage;
	int plainFamily = -1, fleshMaterial = -1;
	int material(const std::string& id) const;
	int slot(const std::string& id) const;
	int family(const std::string& id) const;
	const TypeForm* typeForm(const std::string& itemTypeName) const;
	const AffixPool* pool(int family, int slot) const;
};
// Loads data/apx/{materials,forms,form_map,affixes,salvage}.json. "" = ok, else the error.
std::string loadDB(const std::string& dataDir, DB& db);

struct Counters { long long rolls = 0, rolls_rerolled = 0, parts_overflow = 0, ip_clamped = 0, ip_rerolls = 0, forms_unmapped = 0, forbidden = 0; };

uint64_t fnv1a64(const void* data, size_t n, uint64_t h = 1469598103934665603ULL);
uint64_t rollKey(uint32_t realm, uint8_t depth, uint8_t source, uint32_t index);
inline int ipCap(int depth) { return 250 + 90 * depth; }
int itemPower(const DB& db, const ApxItemData& it);
// The seeded roll (card BF6 roll order). capOn = false is the S2 control (APX_IP_CAP=0).
void roll(const DB& db, const TypeForm& tf, uint32_t realm, int depth, int source, uint32_t index, ApxItemData& out,
	Counters& c, bool capOn = true);
// Items with parts never stack (card BF6-2).
inline bool mayStack(const ApxItemData& a, const ApxItemData& b) { return a.nParts == 0 && b.nParts == 0; }

// Save format v1 (ASCII, card BF6-3) and the packet blob (card BF6-4: 1 + 8 + 2 + 1 + 1 + 16 n <= 205 bytes).
std::string toString(const ApxItemData& it);
bool fromString(const std::string& s, ApxItemData& it);
int pack(const ApxItemData& it, uint8_t* out);
bool unpack(const uint8_t* in, int len, ApxItemData& it);
constexpr int PACK_MAX = 1 + 8 + 2 + 1 + 1 + 16 * APX_MAX_PARTS;

// Salvage (05 section 5, card BF6-6, D86): a crafted item returns EVERY part as a component item (no
// grams, no aura: the affixes stay on the components). Any other item returns the chosen part
// (keepPart, -1 = none) as a component item and every other part as material grams: looted items at
// looted_return_pct, the rest at crafted_return_pct; magic discharge (looted, 20 %) strips the kept
// component's affixes; aura crystals for the affixes of the parts that became material; low integrity
// halves a part's yield. A component item itself salvages to its material at crafted_return_pct.
struct SalvageResult
{
	std::vector<std::pair<int, int>> grams; // (material, grams) per part returned as material
	int massIn = 0, massOut = 0;            // grams of the parts returned as material, before and after the rules
	int auraCrystals = 0, affixesIn = 0;
	bool discharged = false, looted = false, crafted = false;
	std::vector<ApxItemData> components;    // the component items handed over (D86)
	int loosePart = -1;                     // quick salvage (keepPart -1, looted): the part that dropped loose, or -1
};
// 05b section 5 step 5, QUICK SALVAGE (no choice): the highest-rarity part (lowest index on a tie) drops
// loose as a component with the chance by its rarity: Common 0, Uncommon 10, Rare 25, Epic 50,
// Legendary and Unique 100 percent. ASSUMED: one roll, for that one part.
constexpr int LOOSE_PCT[6] = { 0, 10, 25, 50, 100, 100 };
// keepAsMaterial = the LS44 control (APX_BREAK_KEEP_MATERIAL=1): the chosen part is melted like the rest.
SalvageResult salvage(const DB& db, const ApxItemData& it, int keepPart, uint64_t seed, bool symmetric = false, bool keepAsMaterial = false);
int partMassGrams(const DB& db, const ApxPart& p);
// ---- C4 ASSEMBLE / SWAP and C5 MAKE (05b section 5 steps 3, 7, 8)
// Assemble a known Form (family) from components: every required slot exactly once; an optional slot
// opens when the lowest quality among the required components reaches its gate; no slot twice; IP
// must not exceed ipCapValue (the caller passes ipCap(depth); a rejection says so). Crafted flag set.
// ASSUMED until makers exist (C9): the maker and foreign-75-percent rules are not applied (maker = 0).
bool assemble(const DB& db, int family, const std::vector<ApxPart>& parts, uint32_t rollKey, int ipCapValue, ApxItemData& out, std::string& why);
// Swap rule (D87): on a LOOTED assembly only quick slots swap; on any other every slot swaps.
bool swapAllowed(const DB& db, const ApxItemData& it, int partIndex);
// Swap part partIndex for component c (same slot); the old part is returned in `old`.
bool swapPart(const DB& db, ApxItemData& it, int partIndex, const ApxPart& c, int ipCapValue, ApxPart& old, std::string& why);
// MAKE: grams of material for one component of this slot (= its mass), and the made part: quality
// 0-100 = 30 x material tier / 9 + station (0-30) + 20 shaping (any valid build, 05b 5.8) + tool (0-20),
// stored on the 1-10 part scale (/10, at least 1); rarity Common, Uncommon from Fine (50), Rare at
// Masterwork (70+) (D87); affix slots empty; integrity full; model = modelId.
int makeCostGrams(const DB& db, int slot, int material);
ApxPart makePart(const DB& db, int slot, int material, int stationPts, int toolPts, uint8_t modelId, int* quality100 = nullptr);

// One part as a component item's data (D86).
ApxItemData componentOf(const DB& db, const ApxPart& p, uint32_t rollKey, uint8_t flags);
// "Keen Steel Blade of Embers": prefixes, Material, Slot, "of" suffixes joined by "and".
std::string componentName(const DB& db, const ApxPart& p);

} // namespace apxparts
