// apx: the bench screen (U1 shell, D78 gear loop) -- Craft, Salvage/Dismantle and Formbook views
// built as rows for the U1 list widget, plus the vanilla stub provider of apx_bench_api.hpp.
// F6 registers the real provider (parts, Formbook, craft, salvage); fields it leaves null use the stub.
#include "apx_bench_api.hpp"
#include "apx_inventory.hpp"
#include "game.hpp"
#include "stat.hpp"
#include "items.hpp"
#include "player.hpp"
#include "entity.hpp"
#include "net.hpp"
#include "interface/interface.hpp"

namespace apx_bench
{
// ---------------------------------------------------------------- vanilla stub
static std::vector<PartRow> stubParts(const Item*) { return {}; }
static std::vector<FormRow> stubFormbook(int) { return {}; }
static int stubCap(int player)
{
	if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return 0; }
	return std::max(1, (int)statGetINT(stats[player], players[player]->entity) / 2);
}
static std::vector<SlotNeed> stubSlots(int, uint32_t, int) { return {}; }
static bool stubCraft(int, uint32_t, int, const std::vector<std::pair<int, uint32_t>>&, std::string& why) { why = "no Forms until F6"; return false; }
static bool stubPreview(int player, const Item* item, int, SalvagePreview& out)
{
	out = SalvagePreview{};
	if ( !item ) { return false; }
	bool ok = GenericGUI[player].isItemSalvageable(item, player);
	if ( ok ) { out.yield.push_back({ "Barony scrap (tinkering)", 0 }); }
	return ok;
}
static bool stubSalvage(int player, Item* item, int, std::string& why)
{
	if ( !item || !GenericGUI[player].isItemSalvageable(item, player) ) { why = "cannot be salvaged"; return false; }
	if ( !GenericGUI[player].tinkeringSalvageItem(item, false, player) ) { why = "salvage failed"; return false; }
	return true;
}
static bool stubBreak(int player, Item* item, std::string& why)
{
	if ( !item ) { why = "nothing to break"; return false; }
	if ( GenericGUI[player].isItemSalvageable(item, player) && GenericGUI[player].tinkeringSalvageItem(item, false, player) ) { return true; }
	if ( item->count > 1 ) { --item->count; }
	else if ( item->node ) { list_RemoveNode(item->node); }
	return true;
}

static std::vector<PouchRow> stubPouch(int) { return {}; }
static int stubComponentOf(const Item*, PartRow&) { return 0; }
static std::vector<SwapSlot> stubSwapSlots(int, const Item*, int) { return {}; }
static bool stubSwap(int, Item*, int, uint32_t, std::string& why) { why = "components arrive with F6"; return false; }
static std::vector<MakeOption> stubMakeOptions(int, int) { return {}; }
static bool stubMake(int, int, std::string& why) { why = "components arrive with F6"; return false; }
static long long stubPouchGrams(int) { return 0; }

static int stubRepairCost(int, const Item*, std::string& why, std::string&) { why = "no repair before F6"; return -1; }
static bool stubRepair(int, Item*, std::string& why) { why = "no repair before F6"; return false; }
static bool stubRepairPaid(int, Item*, std::string& why) { why = "no repair before F6"; return false; }
static int stubRepairGrams(const Item*, std::string& why, std::string&) { why = "no repair before F6"; return -1; }
static Provider makeStub()
{
	Provider p;
	p.name = "vanilla stub";
	p.parts = stubParts; p.formbook = stubFormbook; p.formInputCap = stubCap; p.craftSlots = stubSlots;
	p.craft = stubCraft; p.salvagePreview = stubPreview; p.salvage = stubSalvage; p.breakItem = stubBreak;
	p.pouch = stubPouch; p.pouchGrams = stubPouchGrams; p.drawBundle = nullptr;
	p.componentOf = stubComponentOf; p.swapSlots = stubSwapSlots; p.swapPart = stubSwap; p.makeOptions = stubMakeOptions; p.make = stubMake;
	p.repairCost = stubRepairCost; p.repair = stubRepair; p.repairPaid = stubRepairPaid; p.repairGrams = stubRepairGrams;
	return p;
}
static Provider g_stub = makeStub();
static Provider g_active = g_stub;

const Provider& stubProvider() { return g_stub; }
const Provider& provider() { return g_active; }
void setProvider(const Provider& p)
{
	g_active = p;
	if ( !g_active.name ) { g_active.name = "unnamed"; }
	if ( !g_active.parts ) { g_active.parts = g_stub.parts; }
	if ( !g_active.formbook ) { g_active.formbook = g_stub.formbook; }
	if ( !g_active.formInputCap ) { g_active.formInputCap = g_stub.formInputCap; }
	if ( !g_active.craftSlots ) { g_active.craftSlots = g_stub.craftSlots; }
	if ( !g_active.repairCost ) { g_active.repairCost = g_stub.repairCost; }
	if ( !g_active.repair ) { g_active.repair = g_stub.repair; }
	if ( !g_active.repairPaid ) { g_active.repairPaid = g_stub.repairPaid; }
	if ( !g_active.repairGrams ) { g_active.repairGrams = g_stub.repairGrams; }
	if ( !g_active.craft ) { g_active.craft = g_stub.craft; }
	if ( !g_active.salvagePreview ) { g_active.salvagePreview = g_stub.salvagePreview; }
	if ( !g_active.salvage ) { g_active.salvage = g_stub.salvage; }
	if ( !g_active.breakItem ) { g_active.breakItem = g_stub.breakItem; }
	if ( !g_active.pouch ) { g_active.pouch = g_stub.pouch; }
	if ( !g_active.pouchGrams ) { g_active.pouchGrams = g_stub.pouchGrams; }
	// drawBundle stays null until the provider has it (the Materials view hides the action)
	if ( !g_active.componentOf ) { g_active.componentOf = g_stub.componentOf; }
	if ( !g_active.swapSlots ) { g_active.swapSlots = g_stub.swapSlots; }
	if ( !g_active.swapPart ) { g_active.swapPart = g_stub.swapPart; }
	if ( !g_active.makeOptions ) { g_active.makeOptions = g_stub.makeOptions; }
	if ( !g_active.make ) { g_active.make = g_stub.make; }
	printlog("[apx] bench provider: %s", g_active.name);
}
long long pouchUnits(int player)
{
	long long g = g_active.pouchGrams ? g_active.pouchGrams(player) : 0;
	return g > 0 ? g / 100 : 0; // whole units, rounded down as F6 counts them (apxPartsPouchWeight)
}
} // namespace apx_bench

namespace apx_inv
{
using apx_logic::Entry;
using namespace apx_bench;

static Entry header(const std::string& t) { Entry e; e.header = true; e.row.name = t; return e; }
static Entry line(const std::string& t, int32_t mass = 0, uint32_t id = 0, int kind = 0)
{
	Entry e; e.row.name = t; e.row.mass = mass; e.row.uid = id; e.row.kind = kind; e.row.type = 0; e.row.status = 4;
	if ( mass == 0 && kind == 0 ) { e.row.price = -2; } // an information row: no mass column
	return e;
}

const int BENCH_TABS = 6; // Assemble, Swap, Make, Salvage, Forms, Materials (D86)
const char* benchTabName(int tab) { static const char* n[] = { "Asm", "Swap", "Make", "Salv", "Forms", "Mats" }; return tab >= 0 && tab < BENCH_TABS ? n[tab] : "?"; }

// rows of one bench view. selectedOwn: the item selected in the player's list (salvage target)
std::vector<Entry> benchEntries(int player, int tab, int station, Item* selectedOwn, uint32_t selectedForm, std::string& head)
{
	const Provider& pv = provider();
	std::vector<Entry> e;
	head.clear();
	for ( int t = 0; t < BENCH_TABS; ++t ) { head += (t ? " " : "") + (t == tab ? std::string("[") + benchTabName(t) + "]" : std::string(benchTabName(t))); }
	if ( tab == 0 ) // Craft: Forms this station can make, then the chosen Form's slots
	{
		auto fb = pv.formbook(player);
		std::string ops;
		for ( auto& o : apx_logic::stationOps(station) ) { ops += (ops.empty() ? "" : ", ") + o; }
		e.push_back(header(std::string(apx_logic::stationName(station)) + ": " + ops));
		e.push_back(header("Forms"));
		if ( fb.empty() ) { e.push_back(line("No Forms known yet. Break an item to learn its Form.")); }
		for ( auto& f : fb ) { if ( f.successes > 0 ) { e.push_back(line(f.name + "  " + std::to_string(f.successes) + "/5", 0, f.form, 1)); } }
		if ( selectedForm )
		{
			e.push_back(header("Slots"));
			for ( auto& s : pv.craftSlots(player, selectedForm, station) )
			{
				long long m = 0;
				for ( uint32_t uid : s.candidateUids ) { if ( Item* it = uidToItem(uid) ) { m += it->getWeight(); } }
				e.push_back(line(s.name + (s.required ? "" : " (optional)") + "  " + std::to_string(s.candidateUids.size()) + " fit", (int32_t)m, 0, 2));
			}
		}
		for ( auto& op : apx_logic::stationOps(station) ) { (void)op; }
	}
	else if ( tab == 1 ) // Swap: replace a part of the item selected in our list with a carried component
	{
		if ( !selectedOwn ) { e.push_back(line("Select an item in your pack.")); return e; }
		e.push_back(header(std::string(selectedOwn->getName()) + ": swap a part"));
		auto slots = pv.swapSlots(player, selectedOwn, station);
		if ( slots.empty() ) { e.push_back(line("No part can be swapped here.")); }
		for ( auto& s : slots )
		{
			e.push_back(header(s.name));
			if ( s.candidateUids.empty() ) { e.push_back(line("  no fitting component carried")); }
			for ( uint32_t uid : s.candidateUids )
			{
				Item* c = uidToItem(uid);
				Entry x = line(c ? std::string(">> ") + c->getName() : "?", c ? c->getWeight() : 0, uid, 7);
				x.row.slot = s.partIndex;
				PartRow pr;
				if ( c && pv.componentOf(c, pr) ) { x.row.rarity = pr.rarity; }
				e.push_back(x);
			}
		}
	}
	else if ( tab == 2 ) // Make: a component from pouch materials
	{
		auto opts = pv.makeOptions(player, station);
		e.push_back(header("Make a component (pouch " + std::to_string(pv.pouchGrams(player)) + " g)"));
		if ( opts.empty() ) { e.push_back(line("Nothing to make here yet.")); }
		for ( auto& o : opts )
		{
			std::string cost;
			for ( auto& c : o.cost ) { cost += (cost.empty() ? "" : ", ") + c.name + " " + std::to_string(c.grams) + " g"; }
			Entry x = line((o.affordable ? ">> " : "   ") + o.name + "  (" + cost + ")", 0, (uint32_t)(o.id + 1), 8);
			if ( !o.affordable ) { x.row.status = 1; }
			e.push_back(x);
		}
	}
	else if ( tab == 3 ) // Salvage / dismantle the item selected in our list
	{
		if ( !selectedOwn ) { e.push_back(line("Select an item in your pack.")); return e; }
		SalvagePreview pr;
		bool ok = pv.salvagePreview(player, selectedOwn, -1, pr);
		e.push_back(header(std::string(apx_logic::stationName(station)) + ": dismantle"));
		e.push_back(header(std::string(selectedOwn->getName()) + (pr.looted ? " (looted)" : "")));
		e.push_back(line("Item", selectedOwn->getWeight()));
		if ( !pr.parts.empty() )
		{
			e.push_back(header("Parts (pick one to keep)"));
			for ( size_t k = 0; k < pr.parts.size(); ++k )
			{
				const auto& p = pr.parts[k];
				e.push_back(line(p.slotName + ": " + p.material, p.massUnits, (uint32_t)(k + 1), 3)); // uid = part index + 1
			}
		}
		e.push_back(header(ok ? "Returns" : "Cannot be salvaged"));
		long long ym = 0;
		for ( auto& y : pr.yield ) { e.push_back(line(y.material, y.massUnits)); ym += y.massUnits; }
		if ( ok && !pr.yield.empty() ) { e.push_back(line("Total returned", (int32_t)ym)); }
		if ( pr.dischargePct || pr.auraPct )
		{
			e.push_back(line("Discharge " + std::to_string(pr.dischargePct) + "%, aura crystal " + std::to_string(pr.auraPct) + "% per affix"));
		}
		e.push_back(line(">> Salvage", 0, 0xFFFFFFFFu, 4));
		if ( station == apx_logic::FIELD || station == apx_logic::ANVIL ) // BP1-2: repair one condition step
		{
			std::string why, mat;
			int g = pv.repairCost(player, selectedOwn, why, mat);
			e.push_back(header("Repair"));
			e.push_back(line(g >= 0 ? ">> Repair one step: " + std::to_string(g) + " g of " + mat : "Cannot repair: " + why, 0, 0xFFFFFFFEu, g >= 0 ? 9 : 0)); // kind 9: repair
		}
	}
	else if ( tab == 5 ) // Materials: the pouch (D85), grams per material, and Draw out when the provider has it
	{
		auto pouch = pv.pouch(player);
		long long total = pv.pouchGrams(player);
		e.push_back(header("Material pouch: " + std::to_string(total / 1000) + "." + std::to_string((total % 1000) / 100) + " kg"));
		if ( pouch.empty() ) { e.push_back(line("Empty. Salvaging fills it.")); }
		for ( auto& p : pouch ) { e.push_back(line(p.name + "  " + std::to_string(p.grams) + " g", (int32_t)((p.grams + 50) / 100), (uint32_t)(p.material + 1), 6)); }
		if ( pv.drawBundle )
		{
			e.push_back(header("Draw out a 1 kg bundle"));
			for ( auto& p : pouch ) { if ( p.grams >= 1000 ) { e.push_back(line(">> " + p.name, 0, (uint32_t)(p.material + 1), 5)); } }
		}
	}
	else // Formbook
	{
		auto fb = pv.formbook(player);
		e.push_back(header("Formbook  (input cap " + std::to_string(pv.formInputCap(player)) + ")"));
		if ( fb.empty() ) { e.push_back(line("Empty. Breaking an item teaches its Form.")); }
		for ( auto& f : fb )
		{
			std::string bar(std::min(5, std::max(0, f.successes)), '#');
			bar += std::string(5 - bar.size(), '-');
			e.push_back(line(f.name + " " + bar + (f.successes >= 5 ? " mastered" : f.successes >= 1 ? " known" : ""), 0, f.form, 1));
		}
	}
	return e;
}

// Enter / confirm on a bench row
bool benchActivate(int player, int tab, int station, Item* selectedOwn, const Entry& row, uint32_t& selectedForm, std::string& msg)
{
	const Provider& pv = provider();
	if ( row.header ) { return false; }
	if ( tab == 0 && row.row.kind == 1 && station == apx_logic::FIELD ) { msg = "No crafting at a field bench."; return false; } // apx BP1
	if ( tab == 0 && row.row.kind == 1 )
	{
		if ( selectedForm != row.row.uid ) { selectedForm = row.row.uid; return true; } // first press picks the Form
		std::vector<std::pair<int, uint32_t>> pick;
		for ( auto& s : pv.craftSlots(player, selectedForm, station) )
		{
			if ( !s.candidateUids.empty() ) { pick.push_back({ s.slot, s.candidateUids.front() }); }
		}
		std::string why;
		bool ok = pv.craft(player, selectedForm, station, pick, why);
		msg = ok ? "Crafted." : "Cannot craft: " + why;
		return ok;
	}
	if ( tab == 3 && selectedOwn && row.row.kind == 9 ) // BP1-2 repair (U1f: Salvage is tab 3)
	{
		std::string why, name = selectedOwn->getName();
		bool ok = pv.repair(player, selectedOwn, why);
		msg = ok ? "Repaired the " + name + "." : "Cannot repair: " + why;
		return ok;
	}
	if ( tab == 1 && row.row.kind == 7 && selectedOwn )
	{
		std::string why, name = selectedOwn->getName();
		bool ok = pv.swapPart(player, selectedOwn, row.row.slot, row.row.uid, why);
		msg = ok ? "Swapped a part of the " + name + "." : "Cannot swap: " + why;
		return ok;
	}
	if ( tab == 2 && row.row.kind == 8 )
	{
		std::string why;
		bool ok = pv.make(player, (int)row.row.uid - 1, why);
		msg = ok ? "Made a component." : "Cannot make: " + why;
		return ok;
	}
	if ( tab == 5 && row.row.kind == 5 && pv.drawBundle )
	{
		std::string why;
		bool ok = pv.drawBundle(player, (int)row.row.uid - 1, 1000, why);
		msg = ok ? "You draw out a bundle." : "Cannot draw out: " + why;
		return ok;
	}
	if ( tab == 3 && selectedOwn && (row.row.kind == 3 || row.row.kind == 4) )
	{
		int keep = row.row.kind == 3 ? (int)row.row.uid - 1 : -1; // the part index, -1 keeps nothing
		std::string why, name = selectedOwn->getName();
		bool ok = pv.salvage(player, selectedOwn, keep, why);
		msg = ok ? "Salvaged the " + name + "." : "Cannot salvage: " + why;
		return ok;
	}
	return false;
}

void breakItem(int player, Item* item)
{
	if ( !item || !stats[player] ) { return; }
	if ( players[player]->paperDoll.enabled && players[player]->paperDoll.isItemOnDoll(*item) )
	{
		messagePlayer(player, MESSAGE_INVENTORY, "Unequip the %s before breaking it.", item->getName());
		return;
	}
	std::string name = item->getName(), why;
	if ( provider().breakItem(player, item, why) )
	{
		messagePlayer(player, MESSAGE_INVENTORY, "You break the %s.%s", name.c_str(), provider().formbook == stubProvider().formbook ? " (Forms are learned when F6 lands.)" : "");
	}
	else
	{
		messagePlayer(player, MESSAGE_INVENTORY, "You cannot break the %s: %s", name.c_str(), why.c_str());
	}
}
} // namespace apx_inv
