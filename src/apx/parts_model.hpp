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
	uint8_t maker, pad;
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

struct Material { std::string id; int density = 0, hardness = 0, tier = 1, valuePerKg = 0, meltC = 0, flammable = 0, element = 0; std::string salvageClass; };
struct Slot { std::string id; Role role = ROLE_OTHER; int volume = 0; };
struct OptSlot { int slot = 0, q = 0; };
struct Family { std::string id; std::vector<int> required; std::vector<OptSlot> optional; };
struct TypeForm { int family = -1; std::vector<int> materials; };
struct AffixPool { std::string key; int firstId = 0, count = 0; };
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

struct Counters { long long rolls = 0, parts_overflow = 0, ip_clamped = 0, ip_rerolls = 0, forms_unmapped = 0, forbidden = 0; };

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

// Salvage (05 section 5, card BF6-6): keepPart stays (the returned item holds it); every other
// part returns material grams; looted items return looted_return_pct, crafted 100; magic
// discharge and aura crystals for affixes; low integrity halves a part's yield.
struct SalvageResult
{
	std::vector<std::pair<int, int>> grams; // (material, grams) per returned part
	int massIn = 0, massOut = 0;            // grams of the non-kept parts, before and after the rules
	int auraCrystals = 0, affixesIn = 0;
	bool discharged = false, looted = false;
	ApxItemData kept{};                     // the item reduced to the kept part
};
SalvageResult salvage(const DB& db, const ApxItemData& it, int keepPart, uint64_t seed, bool symmetric = false);
int partMassGrams(const DB& db, const ApxPart& p);

} // namespace apxparts
