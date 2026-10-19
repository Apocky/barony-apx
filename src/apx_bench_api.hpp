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
	struct PartRow  { int slot = 0; std::string slotName, material; int quality = 0, rarity = 0; int32_t massUnits = 0; float integrity = 1.f; };
	struct FormRow  { uint32_t form = 0; std::string name, family; int successes = 0; };        // known >= 1, mastered at 5
	struct SlotNeed { int slot = 0; std::string name; bool required = true; int qgate = 0; std::vector<uint32_t> candidateUids; };
	struct YieldRow { std::string material; int32_t massUnits = 0; };
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
	};
	void setProvider(const Provider& p);   // F6: register at init; fields left null fall back to the stub
	const Provider& provider();            // always complete (stub fills the gaps)
	const Provider& stubProvider();
}
