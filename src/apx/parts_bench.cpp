// F6 -> BU1 bench shell adapter (API agreed with the U1 lane 2026-10-19: src/apx_bench_api.hpp on
// apx/u1b). Compiles to a no-op until that header is in the tree, so each branch builds alone.
// keepSlot = part index (0..nParts-1) or -1; massUnits = Barony weight units (1 unit = 100 g).
#if defined(__has_include)
#if __has_include("../apx_bench_api.hpp")
#define APX_HAVE_BENCH_API 1
#endif
#endif

#include <algorithm>
#include "parts_engine.hpp"

#ifdef APX_HAVE_BENCH_API
#include "../main.hpp"
#include "../items.hpp"
#include "../stat.hpp"
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
		r.affixesKnown = item->identified; // 05b s4: "unknown" until identified
		r.name = r.affixesKnown || !apxPartsIsComponent(item) ? apxPartsPartName(item, k) : apxPartsComponentName(item);
		if ( !r.affixesKnown ) { r.name = std::string(apxPartsMaterialName(p.material)) + " " + apxPartsSlotName(p.slot); }
		for ( int a = 0; a < apxPartsAffixCount(item, k); ++a ) { r.affixes.push_back(r.affixesKnown ? apxPartsAffixWord(item, k, a) : "unknown"); }
		r.maker = p.maker ? "maker " + std::to_string((int)p.maker) : ""; // C9: makers' names arrive with their tendencies
		if ( apxPartsIsComponent(item) ) { r.fits = apxPartsFits(item); }
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

// C4 Assemble = craftSlots/craft with component candidates (05b 5.7)
std::vector<apx_bench::SlotNeed> benchCraftSlots(int player, uint32_t form, int)
{
	std::vector<apx_bench::SlotNeed> rows;
	std::vector<int> gates;
	const std::vector<int> slots = apxPartsFamilySlots((int)form, &gates);
	const int req = apxPartsRequiredCount((int)form);
	for ( size_t k = 0; k < slots.size(); ++k )
	{
		apx_bench::SlotNeed n;
		n.slot = slots[k]; n.name = apxPartsSlotName(slots[k]); n.required = (int)k < req; n.qgate = gates[k];
		n.candidateUids = apxPartsComponentsForSlot(player, slots[k]);
		rows.push_back(n);
	}
	return rows;
}
bool benchCraft(int player, uint32_t form, int station, const std::vector<std::pair<int, uint32_t>>& slotUid, std::string& why)
{
	return apxPartsAssemble(player, (int)form, station, slotUid, why) != nullptr;
}

// C4 Swap (05b 5.3, D87)
std::vector<apx_bench::SwapSlot> benchSwapSlots(int player, const Item* item, int)
{
	std::vector<apx_bench::SwapSlot> rows;
	if ( !item ) { return rows; }
	for ( int k = 0; k < item->apx.nParts; ++k )
	{
		if ( !apxPartsSwapAllowed(item, k) ) { continue; }
		apx_bench::SwapSlot s;
		s.partIndex = k; s.name = apxPartsSlotName(item->apx.parts[k].slot);
		s.quick = apxPartsIsQuickSlot(item->apx.parts[k].slot);
		s.candidateUids = apxPartsComponentsForSlot(player, item->apx.parts[k].slot);
		rows.push_back(s);
	}
	return rows;
}
bool benchSwapPart(int player, Item* item, int part, uint32_t uid, std::string& why) { return apxPartsSwap(player, item, part, uid, why); }

std::string benchDisplayName(const Item* item) { return apxPartsDisplayName(item); }
bool benchAssemblePreview(int player, uint32_t form, int, const std::vector<std::pair<int, uint32_t>>& slotUid, int& power, int& cap, std::string& why)
{
	return apxPartsAssemblePreview(player, (int)form, slotUid, power, cap, why);
}
// 05b s5.8b per-component repair: one row per worn part; cost = grams of its own material for the lost
// integrity (part mass x lost fraction, ASSUMED until BP1's per-part rule), from the pouch
std::vector<apx_bench::RepairOption> benchRepairOptions(int player, const Item* item, int)
{
	std::vector<apx_bench::RepairOption> rows;
	if ( !item ) { return rows; }
	for ( int k = 0; k < item->apx.nParts; ++k )
	{
		const ApxPart& p = item->apx.parts[k];
		if ( p.integrity >= 255 ) { continue; }
		apx_bench::RepairOption o;
		o.partIndex = k;
		o.name = apxPartsPartName(item, k);
		o.integrity = p.integrity / 255.f;
		const long long g = std::max(1LL, (long long)apxPartsPartMassGrams(item, k) * (255 - p.integrity) / 255);
		o.cost.push_back({ p.material, apxPartsMaterialName(p.material), g });
		o.affordable = apxPartsLedgerMaterialGrams(player, p.material) >= g;
		rows.push_back(o);
	}
	return rows;
}
bool benchRepairPart(int player, Item* item, int part, std::string& why) { return apxPartsRepairPart(player, item, part, why); }
// C6 infuse (05b 5.9): every aura crystal x every part with a free slot; ok/why from a dry run on a copy
int benchInfuseStation = 3; // the station of the last infuseOptions call (infuse carries none)
std::vector<apx_bench::InfuseOption> benchInfuseOptions(int player, const Item* item, int station)
{
	benchInfuseStation = station;
	std::vector<apx_bench::InfuseOption> rows;
	if ( !item || item->apx.nParts == 0 || player < 0 || player >= MAXPLAYERS || !stats[player] ) { return rows; }
	for ( node_t* n = stats[player]->inventory.first; n; n = n->next )
	{
		const Item* c = (const Item*)n->element;
		if ( !apxPartsIsAura(c) ) { continue; }
		for ( int k = 0; k < item->apx.nParts; ++k )
		{
			apx_bench::InfuseOption o;
			o.partIndex = k; o.crystalUid = c->uid; o.affix = apxPartsAuraName(c);
			o.ok = apxPartsInfuseCheck(item, k, apxPartsAuraAffix(c), station, o.why);
			rows.push_back(o);
		}
	}
	return rows;
}
bool benchInfuse(int player, Item* item, int part, uint32_t crystalUid, std::string& why) { return apxPartsInfuse(player, item, part, crystalUid, benchInfuseStation, why); }
int benchStation = 4; // the station of the last makeOptions call (make carries no station); ASSUMED workbench
// C5 Make (05b 5.8): option id = slot << 8 | material; slots of the known Forms x materials in the pouch
std::vector<apx_bench::MakeOption> benchMakeOptions(int player, int station)
{
	benchStation = station;
	std::vector<apx_bench::MakeOption> rows;
	std::vector<int> slots;
	for ( uint32_t f = 0; f < 64; ++f )
	{
		if ( apxPartsFormSuccesses(player, f) < 1 ) { continue; }
		for ( int s : apxPartsFamilySlots((int)f, nullptr) ) { if ( std::find(slots.begin(), slots.end(), s) == slots.end() ) { slots.push_back(s); } }
	}
	for ( int s : slots )
	{
		for ( int m = 0; m < apxPartsMaterialCount(); ++m )
		{
			const int have = apxPartsLedgerMaterialGrams(player, m);
			if ( have <= 0 ) { continue; }
			apx_bench::MakeOption o;
			o.id = s << 8 | m;
			o.name = std::string(apxPartsMaterialName(m)) + " " + apxPartsSlotName(s);
			const int cost = apxPartsMakeCost(s, m);
			o.cost.push_back({ m, apxPartsMaterialName(m), (long long)cost });
			o.affordable = have >= cost;
			rows.push_back(o);
		}
	}
	return rows;
}
bool benchMake(int player, int id, std::string& why) { return apxPartsMake(player, id >> 8, id & 0xFF, benchStation, 0, why) != nullptr; }

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

std::vector<apx_bench::PouchRow> benchPouch(int player)
{
	std::vector<apx_bench::PouchRow> rows;
	for ( int m = 0; m < apxPartsMaterialCount(); ++m )
	{
		const int g = apxPartsLedgerMaterialGrams(player, m);
		if ( g > 0 ) { rows.push_back({ m, apxPartsMaterialName(m), (long long)g }); }
	}
	return rows;
}
bool benchDrawBundle(int player, int material, long long grams, std::string& why)
{
	if ( grams <= 0 || grams > 0xFFFFF ) { why = "grams out of range (1 .. 1,048,575)"; return false; }
	return apxPartsDrawBundle(player, material, (int)grams, why) != nullptr;
}
int benchComponentOf(const Item* item, apx_bench::PartRow& out)
{
	if ( !apxPartsIsComponent(item) ) { return 0; }
	out = benchParts(item)[0];
	return 1;
}

} // namespace

void apxPartsBenchRegister()
{
	static bool done = false;
	if ( done || !apxPartsEnabled() ) { return; }
	done = true;
	apx_bench::Provider p = apx_bench::provider();
	p.name = "F6 parts";
	p.parts = &benchParts;
	p.formbook = &benchFormbook;
	p.formInputCap = &apxPartsFormInputCap;
	p.craftSlots = &benchCraftSlots;
	p.craft = &benchCraft;
	p.salvagePreview = &benchSalvagePreview;
	p.salvage = &benchSalvage;
	p.breakItem = &benchBreak;
	p.pouchGrams = &apxPartsLedgerGrams; // D85: the one carry-weight rule (actplayer.cpp, U1) reads the real ledger
	p.pouch = &benchPouch;
	p.drawBundle = &benchDrawBundle;
	p.componentOf = &benchComponentOf;
	p.repairCost = &apxPartsRepairCost;
	p.repair = &apxPartsRepair;
	p.repairPaid = &apxPartsRepairPaid;
	p.repairGrams = &apxPartsRepairGrams;
	p.swapSlots = &benchSwapSlots;
	p.swapPart = &benchSwapPart;
	p.makeOptions = &benchMakeOptions;
	p.make = &benchMake;
	p.displayName = &benchDisplayName;
	p.assemblePreview = &benchAssemblePreview;
	p.repairOptions = &benchRepairOptions;
	p.repairPart = &benchRepairPart;
	p.infuseOptions = &benchInfuseOptions;
	p.infuse = &benchInfuse;
	apx_bench::setProvider(p);
	printlog("apxparts: bench provider registered (parts, formbook, salvage, break; craft next slice)\n");
}
#else
void apxPartsBenchRegister() {} // apx_bench_api.hpp not in this tree yet (BU1 apx/u1b)
#endif
