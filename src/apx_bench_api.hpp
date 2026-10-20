// apx: the bench screen's contract between U1 (the shell: frames, input, mass bars) and F6 (items with
// parts, salvage, the Formbook). U1 owns this header and ships a vanilla stub provider (empty Formbook,
// salvage = Barony's tinkering salvage, break = destroy). F6 calls apx_bench::setProvider() at init, so
// each branch builds without the other. Mass is in Barony weight units (U1 shows 1 unit = 0.1 kg,
// ASSUMED). Stations: apx_logic::Station (NONE 0, FORGE 1, ANVIL 2, CRUCIBLE 3, WORKBENCH 4, ALEMBIC 5).
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class Item;

namespace apx_bench
{
	struct PartRow  { int slot = 0; std::string slotName, material; int quality = 0, rarity = 0; /* 0 common .. 4 legendary, 5 unique */ int32_t massUnits = 0; float integrity = 1.f;
		std::string name; std::vector<std::string> affixes; };   // D86: a component has a name, rarity 0 common .. 4 legendary, affixes
	struct FormRow  { uint32_t form = 0; std::string name, family; int successes = 0; };        // known >= 1, mastered at 5
	struct SlotNeed { int slot = 0; std::string name; bool required = true; int qgate = 0; std::vector<uint32_t> candidateUids; };
	struct YieldRow { std::string material; int32_t massUnits = 0; };
	struct PouchRow { int material = -1; std::string name; long long grams = 0; };   // D85: the material pouch (F6 ledger)
	struct SwapSlot { int partIndex = 0; std::string name; std::vector<uint32_t> candidateUids; };          // D86 Swap
	struct MakeOption { int id = 0; std::string name; std::vector<PouchRow> cost; bool affordable = false; }; // D86 Make
	struct SalvagePreview { std::vector<PartRow> parts; std::vector<YieldRow> yield; int dischargePct = 0, auraPct = 0; bool looted = false; };

	struct Provider
	{
		const char* name = "vanilla stub";
		std::vector<PartRow>  (*parts)(const Item* item) = nullptr;
		std::vector<FormRow>  (*formbook)(int player) = nullptr;
		int                   (*formInputCap)(int player) = nullptr;
		std::vector<SlotNeed> (*craftSlots)(int player, uint32_t form, int station) = nullptr;
		bool (*craft)(int player, uint32_t form, int station, const std::vector<std::pair<int, uint32_t>>& slotUid, std::string& why) = nullptr;
		bool (*salvagePreview)(int player, const Item* item, int keepSlot, SalvagePreview& out) = nullptr;
		bool (*salvage)(int player, Item* item, int keepSlot, std::string& why) = nullptr;
		bool (*breakItem)(int player, Item* item, std::string& why) = nullptr;   // teaches the Form (BU1 "Break")
		// D85 hybrid pouch: salvaged materials held as grams (counted against carry mass, 100 g = 1 unit)
		std::vector<PouchRow> (*pouch)(int player) = nullptr;          // per material, grams > 0 only
		long long (*pouchGrams)(int player) = nullptr;                 // the total
		bool (*drawBundle)(int player, int material, long long grams, std::string& why) = nullptr; // a real item; null = no button
		// D86 components: first-class items; Assemble = craftSlots/craft with component candidates
		int (*componentOf)(const Item* item, PartRow& out) = nullptr;                          // 1 when the item is a component
		std::vector<SwapSlot> (*swapSlots)(int player, const Item* item, int station) = nullptr;
		bool (*swapPart)(int player, Item* item, int partIndex, uint32_t componentUid, std::string& why) = nullptr; // the old part returns as a component
		std::vector<MakeOption> (*makeOptions)(int player, int station) = nullptr;
		bool (*make)(int player, int optionId, std::string& why) = nullptr;                       // spends pouch grams, adds a component item
		// BP1-2: Repair at a FIELD bench or an ANVIL: the cost in grams (-1 = cannot, why says), then repair
		int  (*repairCost)(int player, const Item* item, std::string& why, std::string& material) = nullptr;
		bool (*repair)(int player, Item* item, std::string& why) = nullptr;
		// the same checks and one-step rule, the ledger untouched: a merchant service charges gold first (trade lane)
		bool (*repairPaid)(int player, Item* item, std::string& why) = nullptr;
		// grams for one condition step with NO ledger check (a merchant's price); -1 + why when it cannot be repaired
		int  (*repairGrams)(const Item* item, std::string& why, std::string& material) = nullptr;
	};
	long long pouchUnits(int player);      // the pouch in Barony weight units (rounded), 0 without a pouch
	void setProvider(const Provider& p);   // F6: register at init; fields left null fall back to the stub
	const Provider& provider();            // always complete (stub fills the gaps)
	const Provider& stubProvider();
}
