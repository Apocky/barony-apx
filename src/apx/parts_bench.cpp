// F6 -> BU1 bench shell adapter (API agreed with the U1 lane 2026-10-19: src/apx_bench_api.hpp on
// apx/u1b). Compiles to a no-op until that header is in the tree, so each branch builds alone.
// keepSlot = part index (0..nParts-1) or -1; massUnits = Barony weight units (1 unit = 100 g).
#if defined(__has_include)
#if __has_include("../apx_bench_api.hpp")
#define APX_HAVE_BENCH_API 1
#endif
#endif

#include "parts_engine.hpp"

#ifdef APX_HAVE_BENCH_API
#include "../main.hpp"
#include "../items.hpp"
#include "../apx_bench_api.hpp"
#include "parts_model.hpp"

namespace {

std::vector<apx_bench::PartRow> benchParts(const Item* item)
{
	std::vector<apx_bench::PartRow> rows;
	if ( !item ) { return rows; }
	for ( int k = 0; k < item->apx.nParts; ++k )
	{
		const ApxPart& p = item->apx.parts[k];
		apx_bench::PartRow r;
		r.slot = p.slot;
		r.slotName = apxPartsSlotName(p.slot);
		r.material = apxPartsMaterialName(p.material);
		r.quality = p.quality;
		r.rarity = p.rarity;
		r.massUnits = (int32_t)(apxPartsPartMassGrams(item, k) / 100);
		r.integrity = p.integrity / 255.f;
		rows.push_back(r);
	}
	return rows;
}

std::vector<apx_bench::FormRow> benchFormbook(int player)
{
	std::vector<apx_bench::FormRow> rows;
	for ( uint32_t f = 0; f < 64; ++f )
	{
		const int s = apxPartsFormSuccesses(player, f);
		if ( s > 0 ) { rows.push_back({ f, apxPartsFamilyName(f), apxPartsFamilyName(f), s }); }
	}
	return rows;
}

std::vector<apx_bench::SlotNeed> benchCraftSlots(int, uint32_t, int) { return {}; } // crafting: next slice
bool benchCraft(int, uint32_t, int, const std::vector<std::pair<int, uint32_t>>&, std::string& why) { why = "crafting: next slice"; return false; }

bool benchSalvagePreview(int, const Item* item, int keepSlot, apx_bench::SalvagePreview& out)
{
	if ( !item || item->apx.nParts == 0 ) { return false; }
	out.parts = benchParts(item);
	out.looted = (item->apx.flags & apxparts::FLAG_LOOTED) && !(item->apx.flags & apxparts::FLAG_CRAFTED);
	const int pct = out.looted ? 60 : 100; // the preview mirrors salvage.json's defaults; salvage itself reads the file
	out.yield.clear();
	for ( int k = 0; k < item->apx.nParts; ++k )
	{
		if ( k == keepSlot ) { continue; }
		const int g = apxPartsPartMassGrams(item, k) * pct / 100;
		out.yield.push_back({ apxPartsMaterialName(item->apx.parts[k].material), (int32_t)(g / 100) });
	}
	out.dischargePct = out.looted ? 20 : 0;
	out.auraPct = 25;
	return true;
}

bool benchSalvage(int player, Item* item, int keepSlot, std::string& why) { return apxPartsSalvageItem(player, item, keepSlot, why); }
bool benchBreak(int player, Item* item, std::string& why) { return apxPartsBreak(player, item, why); }

} // namespace

void apxPartsBenchRegister()
{
	static bool done = false;
	if ( done || !apxPartsEnabled() ) { return; }
	done = true;
	apx_bench::Provider p = apx_bench::provider();
	p.parts = &benchParts;
	p.formbook = &benchFormbook;
	p.formInputCap = &apxPartsFormInputCap;
	p.craftSlots = &benchCraftSlots;
	p.craft = &benchCraft;
	p.salvagePreview = &benchSalvagePreview;
	p.salvage = &benchSalvage;
	p.breakItem = &benchBreak;
	apx_bench::setProvider(p);
	printlog("apxparts: bench provider registered (parts, formbook, salvage, break; craft next slice)\n");
}
#else
void apxPartsBenchRegister() {} // apx_bench_api.hpp not in this tree yet (BU1 apx/u1b)
#endif
