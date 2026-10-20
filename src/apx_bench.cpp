// apx: the bench screen (U1 shell, D78 gear loop) -- Craft, Salvage/Dismantle and Formbook views
// built as rows for the U1 list widget, plus the vanilla stub provider of apx_bench_api.hpp.
// F6 registers the real provider (parts, Formbook, craft, salvage); fields it leaves null use the stub.
#include "apx_bench_api.hpp"
#include <map>
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
static std::string stubDisplayName(const Item*) { return ""; }
static bool stubAssemblePreview(int, uint32_t, int, const std::vector<std::pair<int, uint32_t>>&, int& power, int& cap, std::string& why) { power = 0; cap = 0; why = "assembly arrives with F6"; return false; }
static std::vector<RepairOption> stubRepairOptions(int, const Item*, int) { return {}; }
static bool stubRepairPart(int, Item*, int, std::string& why) { why = "per-component repair arrives with F6"; return false; }
static long long stubPouchGrams(int) { return 0; }
static std::vector<InfuseOption> stubInfuseOptions(int, const Item*, int) { return {}; }
static bool stubInfuse(int, Item*, int, uint32_t, std::string& why) { why = "infusing arrives with F6 (C6)"; return false; }

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
	p.displayName = stubDisplayName; p.assemblePreview = stubAssemblePreview; p.repairOptions = stubRepairOptions; p.repairPart = stubRepairPart; p.infuseOptions = stubInfuseOptions; p.infuse = stubInfuse;
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
	if ( !g_active.repairPart ) { g_active.repairPart = g_stub.repairPart; }
	if ( !g_active.infuseOptions ) { g_active.infuseOptions = g_stub.infuseOptions; }
	if ( !g_active.infuse ) { g_active.infuse = g_stub.infuse; }
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
	if ( !g_active.displayName ) { g_active.displayName = g_stub.displayName; }
	if ( !g_active.assemblePreview ) { g_active.assemblePreview = g_stub.assemblePreview; }
	if ( !g_active.repairOptions ) { g_active.repairOptions = g_stub.repairOptions; }
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

const int BENCH_TABS = apx_logic::BENCH_TAB_COUNT; // Assemble, Swap, Make, Repair, Salvage, Forms, Materials (05b section 7)
const char* benchTabName(int tab) { static const char* n[] = { "Asm", "Swap", "Make", "Rep", "Salv", "Forms", "Mats" }; return tab >= 0 && tab < BENCH_TABS ? n[tab] : "?"; }

// per player: the component chosen to keep in Salvage, and the component chosen per slot in Assemble
struct BenchPick { Uint32 salvageItem = 0; int keep = -1; uint32_t asmForm = 0; std::map<int, uint32_t> asmPick; };
static BenchPick g_pick[MAXPLAYERS];
int benchKeep(int player) { return g_pick[player].keep; }

static std::string grams(long long g) { return std::to_string(g) + " g"; }
static std::string costText(const std::vector<PouchRow>& cost)
{
	std::string s;
	for ( auto& c : cost ) { s += (s.empty() ? "" : ", ") + c.name + " " + grams(c.grams); }
	return s;
}
static std::string componentLabel(const Provider& pv, uint32_t uid, int* rarity)
{
	Item* c = uidToItem(uid);
	if ( !c ) { return "?"; }
	PartRow pr;
	if ( pv.componentOf(c, pr) ) { if ( rarity ) { *rarity = pr.rarity; } }
	std::string n = pv.displayName(c);
	return n.empty() ? std::string(c->getName()) : n;
}

// rows of one bench view. selectedOwn: the item selected in the player's list
std::vector<Entry> benchEntries(int player, int tab, int station, Item* selectedOwn, uint32_t selectedForm, std::string& head)
{
	const Provider& pv = provider();
	auto& pk = g_pick[player];
	std::vector<Entry> e;
	head.clear();
	for ( int t = 0; t < BENCH_TABS; ++t ) { head += (t ? " " : "") + (t == tab ? std::string("[") + benchTabName(t) + "]" : std::string(benchTabName(t))); }
	if ( tab == 0 ) // Assemble: a known Form, a component per slot, item power against the depth cap
	{
		auto fb = pv.formbook(player);
		e.push_back(header(std::string(apx_logic::stationName(station)) + ": assemble a known Form"));
		if ( fb.empty() ) { e.push_back(line("No Forms known yet. Break an item to learn its Form.")); }
		for ( auto& f : fb )
		{
			if ( f.successes <= 0 ) { continue; }
			// the row carries form + 1: Form id 0 (Blades) is a real Form, 0 means none chosen
			e.push_back(line((f.form + 1 == selectedForm ? "[*] " : "    ") + f.name + "  " + std::to_string(f.successes) + "/5", 0, f.form + 1, 1));
		}
		if ( selectedForm )
		{
			const uint32_t form = selectedForm - 1;
			if ( pk.asmForm != selectedForm ) { pk.asmForm = selectedForm; pk.asmPick.clear(); }
			auto slots = pv.craftSlots(player, form, station);
			std::vector<std::pair<int, uint32_t>> picks;
			bool allRequired = true;
			for ( auto& s : slots )
			{
				e.push_back(header(s.name + (s.required ? "" : " (optional)")));
				if ( s.candidateUids.empty() ) { e.push_back(line("  no fitting component carried")); }
				auto chosen = pk.asmPick.find(s.slot);
				for ( uint32_t uid : s.candidateUids )
				{
					int rar = -1;
					bool on = chosen != pk.asmPick.end() && chosen->second == uid;
					Item* c = uidToItem(uid);
					Entry x = line((on ? "[x] " : "[ ] ") + componentLabel(pv, uid, &rar), c ? c->getWeight() : 0, uid, 10);
					x.row.slot = s.slot; x.row.rarity = rar;
					e.push_back(x);
				}
				if ( chosen != pk.asmPick.end() ) { picks.push_back({ s.slot, chosen->second }); }
				else if ( s.required ) { allRequired = false; }
			}
			int power = 0, cap = 0;
			std::string why;
			const bool hasPreview = pv.assemblePreview != stubProvider().assemblePreview;
			bool ok = allRequired && (hasPreview ? pv.assemblePreview(player, form, station, picks, power, cap, why) : true);
			e.push_back(header(hasPreview ? "Item power " + std::to_string(power) + " / cap " + std::to_string(cap) : "Item power: shown when the provider has a preview"));
			e.push_back(line(ok ? ">> Assemble" : (allRequired ? "   " + why : "   fill every required slot"), 0, selectedForm, ok ? 11 : 0));
		}
	}
	else if ( tab == 1 ) // Swap: a part of the selected item for a carried component (looted items: quick slots only)
	{
		if ( !selectedOwn ) { e.push_back(line("Select an item in your pack.")); return e; }
		e.push_back(header(std::string(selectedOwn->getName()) + ": swap a part"));
		auto slots = pv.swapSlots(player, selectedOwn, station);
		if ( slots.empty() ) { e.push_back(line("No part can be swapped here (looted: quick slots only).")); }
		for ( auto& s : slots )
		{
			e.push_back(header(s.name + (s.quick ? " (quick slot)" : "")));
			if ( s.candidateUids.empty() ) { e.push_back(line("  no fitting component carried")); }
			for ( uint32_t uid : s.candidateUids )
			{
				int rar = -1;
				Item* c = uidToItem(uid);
				Entry x = line(">> " + componentLabel(pv, uid, &rar), c ? c->getWeight() : 0, uid, 7);
				x.row.slot = s.partIndex; x.row.rarity = rar;
				e.push_back(x);
			}
		}
	}
	else if ( tab == 2 ) // Make: a component from pouch materials
	{
		auto opts = pv.makeOptions(player, station);
		e.push_back(header("Make a component (pouch " + grams(pv.pouchGrams(player)) + ")"));
		if ( opts.empty() ) { e.push_back(line("Nothing to make here yet.")); }
		for ( auto& o : opts )
		{
			Entry x = line((o.affordable ? ">> " : "   ") + o.name + "  (" + costText(o.cost) + ")", 0, (uint32_t)(o.id + 1), 8);
			if ( !o.affordable ) { x.row.status = 1; }
			e.push_back(x);
		}
	}
	else if ( tab == apx_logic::TAB_REPAIR ) // Repair: a component's integrity back, for pouch material of its kind
	{
		if ( !selectedOwn ) { e.push_back(line("Select an item in your pack.")); return e; }
		e.push_back(header(std::string(selectedOwn->getName()) + ": repair"));
		auto opts = pv.repairOptions(player, selectedOwn, station);
		// BP1-2 (props lane): one condition step of the whole item at a FIELD bench or an ANVIL, paid from the
		// pouch. Always offered there: the item's condition and its components' integrity are separate repairs
		// (F6's per-component rows follow), so filling one never hides the other
		if ( station == apx_logic::FIELD || station == apx_logic::ANVIL )
		{
			std::string why, mat;
			int g = pv.repairCost(player, selectedOwn, why, mat);
			e.push_back(line(g >= 0 ? ">> Repair one step: " + std::to_string(g) + " g of " + mat : "   Cannot repair: " + why, 0, 0, g >= 0 ? apx_logic::ROW_REPAIR_STEP : apx_logic::ROW_INFO));
		}
		else if ( opts.empty() ) { e.push_back(line("Repair here: a field bench or an anvil.")); }
		if ( !opts.empty() ) { e.push_back(header("Components")); }
		for ( auto& o : opts )
		{
			int pct = (int)(o.integrity * 100.f + 0.5f);
			Entry x = line((o.affordable ? ">> " : "   ") + o.name + "  " + std::to_string(pct) + "%  (" + costText(o.cost) + ")", 0, (uint32_t)(o.partIndex + 1), 12);
			if ( !o.affordable ) { x.row.status = 1; }
			e.push_back(x);
		}
	}
	else if ( tab == 4 ) // Salvage: choose the component to keep, preview the return, then break
	{
		if ( !selectedOwn ) { e.push_back(line("Select an item in your pack.")); return e; }
		if ( pk.salvageItem != selectedOwn->uid ) { pk.salvageItem = selectedOwn->uid; pk.keep = -1; }
		SalvagePreview pr;
		bool ok = pv.salvagePreview(player, selectedOwn, pk.keep, pr);
		e.push_back(header(std::string(apx_logic::stationName(station)) + ": break"));
		e.push_back(header(std::string(selectedOwn->getName()) + (pr.looted ? " (looted)" : " (crafted)")));
		e.push_back(line("Item", selectedOwn->getWeight()));
		if ( !pr.parts.empty() )
		{
			e.push_back(header(pr.looted ? "Keep one component" : "Every component returns"));
			for ( size_t k = 0; k < pr.parts.size(); ++k )
			{
				const auto& p = pr.parts[k];
				std::string nm = p.name.empty() ? p.material : p.name;
				Entry x = line(((int)k == pk.keep ? "[x] " : "[ ] ") + p.slotName + ": " + nm, p.massUnits, (uint32_t)(k + 1), 3); // uid = part index + 1
				x.row.rarity = p.rarity;
				e.push_back(x);
			}
		}
		e.push_back(header(ok ? "Returns to the pouch" : "Cannot be salvaged"));
		long long ym = 0;
		for ( auto& y : pr.yield ) { e.push_back(line(y.material, y.massUnits)); ym += y.massUnits; }
		if ( ok && !pr.yield.empty() ) { e.push_back(line("Total", (int32_t)ym)); }
		if ( pr.dischargePct || pr.auraPct )
		{
			e.push_back(line("Discharge " + std::to_string(pr.dischargePct) + "%, aura crystal " + std::to_string(pr.auraPct) + "% per affix"));
		}
		if ( ok )
		{
			if ( !pr.looted || pr.parts.empty() ) { e.push_back(line(">> Break", 0, 0, 4)); }
			else if ( pk.keep >= 0 ) { e.push_back(line(">> Break, keeping " + pr.parts[pk.keep].slotName, 0, (uint32_t)(pk.keep + 1), 4)); }
			else { e.push_back(line("   choose the component to keep")); }
			if ( pr.looted && !pr.parts.empty() ) { e.push_back(line(">> Quick salvage (no choice)", 0, 0, 9)); }
		}
	}
	else if ( tab == 6 ) // Materials: the pouch (D85), grams per material, and Draw out when the provider has it
	{
		auto pouch = pv.pouch(player);
		long long total = pv.pouchGrams(player);
		e.push_back(header("Material pouch: " + std::to_string(total / 1000) + "." + std::to_string((total % 1000) / 100) + " kg"));
		if ( pouch.empty() ) { e.push_back(line(total > 0 ? "Held as one store (" + grams(total) + ")." : "Empty. Salvaging fills it.")); }
		for ( auto& p : pouch ) { e.push_back(line(p.name + "  " + grams(p.grams), (int32_t)((p.grams + 50) / 100), (uint32_t)(p.material + 1), 6)); }
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
	auto& pk = g_pick[player];
	if ( row.header ) { return false; }
	if ( tab == 0 && (row.row.kind == 1 || row.row.kind == 11) && station == apx_logic::FIELD ) { msg = "No assembling at a field bench."; return false; } // apx BP1
	if ( tab == 0 && row.row.kind == 1 ) { selectedForm = row.row.uid; return true; }   // pick the Form
	if ( tab == 0 && row.row.kind == 10 )                                               // pick or unpick a component for a slot
	{
		auto it = pk.asmPick.find(row.row.slot);
		if ( it != pk.asmPick.end() && it->second == row.row.uid ) { pk.asmPick.erase(it); } else { pk.asmPick[row.row.slot] = row.row.uid; }
		return true;
	}
	if ( tab == 0 && row.row.kind == 11 && selectedForm )
	{
		std::vector<std::pair<int, uint32_t>> picks(pk.asmPick.begin(), pk.asmPick.end());
		std::string why;
		bool ok = pv.craft(player, selectedForm - 1, station, picks, why);
		msg = ok ? "Assembled." : "Cannot assemble: " + why;
		if ( ok ) { pk.asmPick.clear(); }
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
	if ( tab == apx_logic::TAB_REPAIR && row.row.kind == apx_logic::ROW_REPAIR_STEP && selectedOwn )
	{
		std::string why, name = selectedOwn->getName();
		bool ok = pv.repair(player, selectedOwn, why);
		msg = ok ? "Repaired the " + name + "." : "Cannot repair: " + why;
		return ok;
	}
	if ( tab == 3 && row.row.kind == 12 && selectedOwn )
	{
		std::string why;
		bool ok = pv.repairPart(player, selectedOwn, (int)row.row.uid - 1, why);
		msg = ok ? "Repaired." : "Cannot repair: " + why;
		return ok;
	}
	if ( tab == 6 && row.row.kind == 5 && pv.drawBundle )
	{
		std::string why;
		bool ok = pv.drawBundle(player, (int)row.row.uid - 1, 1000, why);
		msg = ok ? "You draw out a bundle." : "Cannot draw out: " + why;
		return ok;
	}
	if ( tab == 4 && selectedOwn && row.row.kind == 3 ) { pk.salvageItem = selectedOwn->uid; pk.keep = (int)row.row.uid - 1; return true; } // choose the keep
	if ( tab == 4 && selectedOwn && (row.row.kind == 4 || row.row.kind == 9) )
	{
		int keep = row.row.kind == 4 ? (int)row.row.uid - 1 : -1; // the part index; -1: quick salvage / crafted (all return)
		std::string why, name = selectedOwn->getName();
		bool ok = pv.salvage(player, selectedOwn, keep, why);
		msg = ok ? "Salvaged the " + name + "." : "Cannot salvage: " + why;
		if ( ok ) { pk.keep = -1; pk.salvageItem = 0; }
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
