// F6 parts-based gear, Barony side (card BF6-2/3/4/6). See parts_engine.hpp.
#include "../main.hpp"
#include "../game.hpp"
#include "../entity.hpp"
#include "../items.hpp"
#include "../net.hpp"
#include "../player.hpp"
#include "../scores.hpp"
#include "../interface/interface.hpp"
#include "parts_engine.hpp"
#include "props_rules.hpp"
#include "parts_model.hpp"
#include "../apx_bench_api.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <set>
#include <unordered_map>

#ifndef APX_PARTS_DATA_DIR
#define APX_PARTS_DATA_DIR "data/apx"
#endif

ApxPartsCounters apxPartsCounters;

namespace {

const char* const TYPE_NAMES[] = {
#include "parts_typenames.inc"
};
static_assert(sizeof(TYPE_NAMES) / sizeof(TYPE_NAMES[0]) == NUMITEMS, "parts_typenames.inc is stale: run tools/apx/gen_forms.py");

apxparts::DB& db()
{
	static apxparts::DB d;
	static bool loaded = false;
	if ( !loaded )
	{
		loaded = true;
		const char* envDir = getenv("APX_PARTS_DATA");
		std::string chosen = envDir && *envDir ? std::string(envDir) : std::string();
		if ( chosen.empty() ) // release layout first: <exe dir>/data/apx; then the source copy (developer builds)
		{
			char* bp = SDL_GetBasePath();
			const std::string rel = (bp ? std::string(bp) : std::string("./")) + "data/apx";
			if ( bp ) { SDL_free(bp); }
			FILE* f = fopen((rel + "/SCHEMA.md").c_str(), "rb");
			if ( f ) { fclose(f); chosen = rel; } else { chosen = APX_PARTS_DATA_DIR; }
		}
		const char* dir = chosen.c_str();
		const std::string e = apxparts::loadDB(dir, d);
		if ( !e.empty() ) { printlog("apxparts: FAILED data load: %s (parts off)\n", e.c_str()); d.families.clear(); }
		else { printlog("apxparts: data %s: %d materials, %d families, %d types, %d affixes\n", dir && *dir ? dir : APX_PARTS_DATA_DIR,
			(int)d.materials.size(), (int)d.families.size(), (int)d.typeForms.size(), (int)d.affixPct.size()); }
	}
	return d;
}

uint32_t floorIndex = 0;
std::unordered_map<Uint32, ApxItemData> floorParts; // floor item entity uid -> parts (server truth)

bool bindable(const Item* item)
{
	if ( !item || item->type < 0 || item->type >= NUMITEMS ) { return false; }
	const Category c = itemCategory(item);
	return c == WEAPON || c == ARMOR || c == TOOL;
}

} // namespace

bool apxPartsEnabled()
{
	static int on = -1;
	if ( on < 0 ) { const char* v = getenv("APX_PARTS"); on = (v && *v == '0') ? 0 : 1; }
	return on == 1 && !db().families.empty();
}

void apxPartsLevelStart()
{
	floorIndex = 0;
	apxPartsBenchRegister();
	floorParts.clear();
}

void apxPartsBind(Item* item, int source, int depthOverride)
{
	if ( !apxPartsEnabled() || !bindable(item) || item->apx.nParts > 0 ) { return; }
	const apxparts::DB& d = db();
	const apxparts::TypeForm* tf = d.typeForm(TYPE_NAMES[item->type]);
	apxparts::Counters c;
	apxparts::TypeForm plain;
	plain.family = d.plainFamily;
	plain.materials.push_back(0);
	const bool capOn = !(getenv("APX_IP_CAP") && *getenv("APX_IP_CAP") == '0');
	apxparts::roll(d, tf ? *tf : plain, (uint32_t)mapseed, depthOverride > 0 ? depthOverride : currentlevel, source, floorIndex++, item->apx, c, capOn);
	if ( !tf ) { ++c.forms_unmapped; }
	if ( source == apxparts::SRC_CHEST || source == apxparts::SRC_MONSTER || source == apxparts::SRC_FLOOR ) { item->apx.flags |= apxparts::FLAG_LOOTED; }
	apxPartsCounters.bound += item->apx.nParts > 0;
	apxPartsCounters.parts_overflow += c.parts_overflow;
	apxPartsCounters.ip_clamped += c.ip_clamped;
	apxPartsCounters.ip_rerolls += c.ip_rerolls;
	apxPartsCounters.forms_unmapped += c.forms_unmapped;
}

void apxPartsBindInventory(void* listPtr, int source)
{
	list_t* list = (list_t*)listPtr;
	if ( !list || !apxPartsEnabled() || multiplayer == CLIENT ) { return; }
	for ( node_t* n = list->first; n; n = n->next ) { apxPartsBind((Item*)n->element, source); }
}

void apxPartsToFloor(Entity* entity, const Item* item)
{
	if ( !entity || !item || !apxPartsEnabled() || multiplayer == CLIENT ) { return; }
	if ( apxPartsIsComponent(item) ) { printlog("apxparts: component on floor \"%s\" blob %016llx\n", apxPartsComponentName(item), (unsigned long long)apxparts::fnv1a64(&item->apx, sizeof(item->apx))); }
	if ( apxPartsIsBundle(item) ) { printlog("apxparts: bundle on floor %s %d g appearance %08x\n", apxPartsMaterialName(apxPartsBundleMaterial(item)), apxPartsBundleGrams(item), item->appearance); }
	if ( item->apx.nParts > 0 ) { floorParts[entity->getUID()] = item->apx; }
	else { floorParts.erase(entity->getUID()); }
}

void apxPartsBindFloorEntity(Entity* entity)
{
	if ( !entity || !apxPartsEnabled() || multiplayer == CLIENT ) { return; }
	Item tmp;
	memset(&tmp, 0, sizeof(tmp));
	tmp.type = (ItemType)entity->skill[10];
	tmp.count = 1;
	apxPartsBind(&tmp, apxparts::SRC_FLOOR);
	if ( tmp.apx.nParts > 0 ) { floorParts[entity->getUID()] = tmp.apx; }
}

void apxPartsFromFloor(const Entity* entity, Item* item)
{
	if ( !entity || !item || !apxPartsEnabled() ) { return; }
	auto it = floorParts.find(entity->getUID());
	if ( it != floorParts.end() ) { item->apx = it->second; }
}

int apxPartsNetAppend(const Item* item, uint8_t* packet, int len)
{
	if ( !item || !apxPartsEnabled() || item->apx.nParts == 0 ) { return len; }
	if ( getenv("APX_NET_STRIP") && *getenv("APX_NET_STRIP") == '1' ) { return len; } // control: the blob is not sent
	if ( len + apxparts::PACK_MAX > NET_PACKET_SIZE ) { ++apxPartsCounters.net_blob_dropped; return len; }
	++apxPartsCounters.net_blob_sent;
	const int n = len + apxparts::pack(item->apx, packet + len);
	printlog("apxparts: net sent type %d len %d blob %016llx\n", (int)item->type, n,
		(unsigned long long)apxparts::fnv1a64(&item->apx, sizeof(item->apx)));
	return n;
}

void apxPartsNetRead(Item* item, const uint8_t* packet, int len, int at)
{
	if ( !item ) { return; }
	if ( len <= at ) { printlog("apxparts: net recv type %d len %d blob none\n", (int)item->type, len); return; }
	if ( !apxparts::unpack(packet + at, len - at, item->apx) ) { ++apxPartsCounters.net_blob_dropped; memset(&item->apx, 0, sizeof(item->apx)); return; }
	printlog("apxparts: net recv type %d len %d blob %016llx\n", (int)item->type, len,
		(unsigned long long)apxparts::fnv1a64(&item->apx, sizeof(item->apx)));
}

std::string apxPartsSaveString(const Item* item)
{
	if ( !item || !apxPartsEnabled() || item->apx.nParts == 0 ) { return ""; }
	if ( getenv("APX_SAVE_STRIP") && *getenv("APX_SAVE_STRIP") == '1' ) { return ""; } // control: parts not written
	return apxparts::toString(item->apx);
}

void apxPartsLoadString(Item* item, const std::string& s)
{
	if ( !item || s.empty() ) { return; }
	if ( !apxparts::fromString(s, item->apx) ) { ++apxPartsCounters.save_blob_dropped; memset(&item->apx, 0, sizeof(item->apx)); }
}

namespace {
std::unordered_map<uint64_t, std::string> sidecar; // (player << 32 | index) -> parts string
constexpr int MAX_FORMS = 64;
uint8_t formbook[MAXPLAYERS][MAX_FORMS] = {};
uint8_t sidecarFormbook[MAXPLAYERS][MAX_FORMS] = {}; // read with a save; applied by loadGame only
constexpr int MAX_MATS = 256;
uint32_t ledger[MAXPLAYERS][MAX_MATS] = {};         // salvaged material grams not yet paid out as items
uint32_t sidecarLedger[MAXPLAYERS][MAX_MATS] = {};
bool sidecarHasFormbook[MAXPLAYERS] = {};
}

void apxPartsFormbookFromSave(int player)
{
	if ( player < 0 || player >= MAXPLAYERS ) { return; }
	memcpy(formbook[player], sidecarFormbook[player], MAX_FORMS); // a save without a book clears it
	memcpy(ledger[player], sidecarLedger[player], sizeof(ledger[player]));
	printlog("apxparts: formbook loaded player %d hash %016llx\n", player, (unsigned long long)apxparts::fnv1a64(formbook[player], MAX_FORMS));
}

void apxPartsSidecarSave(const char* savePath)
{
	if ( !savePath || !apxPartsEnabled() ) { return; }
	const std::string path = std::string(savePath) + ".apxparts";
	if ( getenv("APX_SAVE_STRIP") && *getenv("APX_SAVE_STRIP") == '1' ) { remove(path.c_str()); return; } // control
	FILE* fp = fopen(path.c_str(), "wb");
	if ( !fp ) { printlog("apxparts: cannot write %s\n", path.c_str()); return; }
	int rows = 0;
	for ( int c = 0; c < MAXPLAYERS; ++c )
	{
		if ( !stats[c] ) { continue; }
		int index = 0;
		for ( node_t* n = stats[c]->inventory.first; n; n = n->next, ++index )
		{
			const std::string s = apxPartsSaveString((const Item*)n->element);
			if ( !s.empty() ) { fprintf(fp, "%d %d %s\n", c, index, s.c_str()); ++rows; }
		}
		for ( int f = 0; f < MAX_FORMS; ++f ) { if ( formbook[c][f] ) { fprintf(fp, "fb %d %d %d\n", c, f, formbook[c][f]); ++rows; } }
		for ( int m = 0; m < MAX_MATS; ++m ) { if ( ledger[c][m] ) { fprintf(fp, "ml %d %d %u\n", c, m, ledger[c][m]); ++rows; } }
	}
	fclose(fp);
	printlog("apxparts: sidecar %s rows %d\n", path.c_str(), rows);
}

void apxPartsSidecarLoad(const char* savePath)
{
	sidecar.clear();
	memset(sidecarFormbook, 0, sizeof(sidecarFormbook));
	memset(sidecarLedger, 0, sizeof(sidecarLedger));
	if ( !savePath ) { return; }
	FILE* fp = fopen((std::string(savePath) + ".apxparts").c_str(), "rb");
	if ( !fp ) { return; }
	char line[4096];
	while ( fgets(line, sizeof(line), fp) )
	{
		int c = -1, index = -1, used = 0;
		int f = -1, succ = 0;
		if ( line[0] == 'm' && line[1] == 'l' )
		{
			unsigned g = 0;
			if ( sscanf(line, "ml %d %d %u", &c, &f, &g) == 3 && c >= 0 && c < MAXPLAYERS && f >= 0 && f < MAX_MATS ) { sidecarLedger[c][f] = g; }
			else { ++apxPartsCounters.save_blob_dropped; }
			continue;
		}
		if ( line[0] == 'f' && line[1] == 'b' )
		{
			if ( sscanf(line, "fb %d %d %d", &c, &f, &succ) == 3 && c >= 0 && c < MAXPLAYERS && f >= 0 && f < MAX_FORMS && succ >= 0 && succ <= 5 ) { sidecarFormbook[c][f] = (uint8_t)succ; }
			else { ++apxPartsCounters.save_blob_dropped; }
			continue;
		}
		if ( sscanf(line, "%d %d %n", &c, &index, &used) < 2 || c < 0 || c >= MAXPLAYERS || index < 0 ) { ++apxPartsCounters.save_blob_dropped; continue; }
		std::string s = line + used;
		while ( !s.empty() && (s.back() == '\n' || s.back() == '\r') ) { s.pop_back(); }
		sidecar[((uint64_t)c << 32) | (uint32_t)index] = s;
	}
	fclose(fp);
}

const std::string& apxPartsSidecarGet(int player, int inventoryIndex)
{
	static const std::string none;
	auto it = sidecar.find(((uint64_t)player << 32) | (uint32_t)inventoryIndex);
	return it == sidecar.end() ? none : it->second;
}

// ---------------------------------------------------------------- palette tint (D79 stand-in)
void apxPartsApplyTint(Entity* entity, const Item* item)
{
	static std::unordered_map<Uint32, bool> tinted; // entities whose lightBonus F6 set
	if ( !entity ) { return; }
	const bool vis = !(getenv("APX_PARTS_VIS") && *getenv("APX_PARTS_VIS") == '0');
	if ( vis && item && item->apx.nParts > 0 && apxPartsEnabled() && item->apx.parts[0].material < db().materials.size() )
	{
		const float* t = db().materials[item->apx.parts[0].material].tint;
		entity->lightBonus = vec4(t[0], t[1], t[2], 0.f);
		tinted[entity->getUID()] = true;
		return;
	}
	auto it = tinted.find(entity->getUID());
	if ( it != tinted.end() ) { entity->lightBonus = vec4(0.f, 0.f, 0.f, 0.f); tinted.erase(it); } // back to the vanilla look
}

// ---------------------------------------------------------------- Formbook
namespace {
void sendFormbook(int player)
{
	if ( multiplayer != CLIENT || player != clientnum ) { return; }
	int n = 0;
	strcpy((char*)net_packet->data, "FBOK");
	net_packet->data[4] = (Uint8)player;
	for ( int f = 0; f < MAX_FORMS; ++f )
	{
		if ( formbook[player][f] ) { net_packet->data[6 + 2 * n] = (Uint8)f; net_packet->data[7 + 2 * n] = formbook[player][f]; ++n; }
	}
	net_packet->data[5] = (Uint8)n;
	net_packet->address.host = net_server.host;
	net_packet->address.port = net_server.port;
	net_packet->len = 6 + 2 * n;
	sendPacketSafe(net_sock, -1, net_packet, 0);
	printlog("apxparts: formbook sent player %d forms %d hash %016llx\n", player, n, (unsigned long long)apxPartsFormbookHash(player));
}
} // namespace

const char* apxPartsFamilyName(uint32_t form) { return form < db().families.size() ? db().families[form].id.c_str() : "?"; }
const char* apxPartsMaterialName(int m) { return m >= 0 && m < (int)db().materials.size() ? db().materials[m].id.c_str() : "?"; }
const char* apxPartsSlotName(int s) { return s >= 0 && s < (int)db().slots.size() ? db().slots[s].id.c_str() : "?"; }
int apxPartsPartMassGrams(const Item* item, int part) { return item && part >= 0 && part < item->apx.nParts ? apxparts::partMassGrams(db(), item->apx.parts[part]) : 0; }

int apxPartsFormSuccesses(int player, uint32_t form)
{
	return player >= 0 && player < MAXPLAYERS && form < MAX_FORMS ? formbook[player][form] : 0;
}

int apxPartsFormInputCap(int player)
{
	if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return 1; }
	return std::max(1, (int)statGetINT(stats[player], players[player] ? players[player]->entity : nullptr) / 2);
}

uint64_t apxPartsFormbookHash(int player)
{
	return player >= 0 && player < MAXPLAYERS ? apxparts::fnv1a64(formbook[player], MAX_FORMS) : 0;
}

bool apxPartsBreak(int player, Item* item, std::string& why)
{
	if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { why = "no player"; return false; }
	if ( !item || item->apx.nParts == 0 ) { why = "a vanilla item has no Form to learn"; return false; }
	if ( apxparts::isComponent(item->apx) ) { why = "a component has no Form: assemble it into an item first"; return false; }
	const uint32_t form = item->apx.form;
	if ( form >= MAX_FORMS ) { why = "form out of range"; return false; }
	if ( formbook[player][form] == 0 )
	{
		int known = 0;
		for ( int f = 0; f < MAX_FORMS; ++f ) { known += formbook[player][f] > 0; }
		if ( known >= apxPartsFormInputCap(player) ) { why = "no free Form slot (INT / 2)"; return false; }
	}
	if ( formbook[player][form] < 5 ) { ++formbook[player][form]; }
	printlog("apxparts: break player %d form %s successes %d\n", player, apxPartsFamilyName(form), formbook[player][form]);
	Item* consumed = item;
	consumeItem(consumed, player);
	sendFormbook(player);
	return true;
}

void apxPartsFormbookRecv(const uint8_t* d, int len)
{
	if ( len < 6 ) { return; }
	const int player = std::min<int>(d[4], MAXPLAYERS - 1), n = d[5];
	if ( len != 6 + 2 * n ) { ++apxPartsCounters.net_blob_dropped; return; }
	memset(formbook[player], 0, MAX_FORMS);
	for ( int k = 0; k < n; ++k ) { if ( d[6 + 2 * k] < MAX_FORMS ) { formbook[player][d[6 + 2 * k]] = (uint8_t)std::min<int>(5, d[7 + 2 * k]); } }
	printlog("apxparts: formbook recv player %d forms %d hash %016llx\n", player, n, (unsigned long long)apxPartsFormbookHash(player));
}

// Salvage settlement: every returned gram goes into the player's material ledger (no mass is lost to
// rounding or to materials without a Barony stand-in); whole stand-in units (metal scrap, magic scrap,
// salvage.json grams each) are paid out of it. The ledger is saved in the sidecar ("ml" rows).
namespace {
void settle(int player, const apxparts::SalvageResult& r, int& metalUnits, int& magicUnits)
{
	const apxparts::DB& d = db();
	metalUnits = magicUnits = 0;
	if ( player < 0 || player >= MAXPLAYERS ) { return; }
	for ( auto& mg : r.grams ) { if ( mg.first >= 0 && mg.first < MAX_MATS ) { ledger[player][mg.first] += (uint32_t)std::max(0, mg.second); } }
	apxPartsCounters.salvage_grams_out += r.massOut;
	int per[2] = { 400, 200 };
	for ( auto& s : d.salvage.standIns ) { if ( s.cls == "metal" ) { per[0] = std::max(1, s.grams); } else if ( s.cls == "magic" ) { per[1] = std::max(1, s.grams); } }
	for ( int k = 0; k < 2; ++k )
	{
		const char* cls = k == 0 ? "metal" : "magic";
		long long have = 0;
		for ( size_t m = 0; m < d.materials.size() && m < (size_t)MAX_MATS; ++m ) { if ( d.materials[m].salvageClass == cls ) { have += ledger[player][m]; } }
		const int units = (int)(have / per[k]);
		long long owe = (long long)units * per[k];
		for ( size_t m = 0; m < d.materials.size() && m < (size_t)MAX_MATS && owe > 0; ++m )
		{
			if ( d.materials[m].salvageClass != cls ) { continue; }
			const long long take = std::min<long long>(owe, ledger[player][m]);
			ledger[player][m] -= (uint32_t)take;
			owe -= take;
		}
		(k == 0 ? metalUnits : magicUnits) = units;
		apxPartsCounters.salvage_grams_paid += (long long)units * per[k];
	}
}
}

// ---------------------------------------------------------------- D85 bundles
namespace {
constexpr Uint32 BUNDLE_TAG = 0xBu << 28;
std::set<Uint32> drawnBundles[MAXPLAYERS]; // uids of bundles this player drew (or loaded): they stay items
bool pendingAbsorb[MAXPLAYERS] = {};
}

bool apxPartsIsBundle(const Item* item) { return item && item->type == GEM_GLASS && (item->appearance & 0xF0000000u) == BUNDLE_TAG; }
int apxPartsBundleMaterial(const Item* item) { return apxPartsIsBundle(item) ? (int)((item->appearance >> 20) & 0xFF) : -1; }
int apxPartsBundleGrams(const Item* item) { return apxPartsIsBundle(item) ? (int)(item->appearance & 0xFFFFF) : 0; }
int apxPartsLedgerMaterialGrams(int player, int m) { return player >= 0 && player < MAXPLAYERS && m >= 0 && m < MAX_MATS ? (int)ledger[player][m] : 0; }

int apxPartsBundleValue(const Item* item)
{
	const int m = apxPartsBundleMaterial(item);
	return m >= 0 && m < (int)db().materials.size() ? (int)((long long)apxPartsBundleGrams(item) * db().materials[m].valuePerKg / 1000) : 0;
}

const char* apxPartsBundleName(const Item* item)
{
	static char buf[96];
	snprintf(buf, sizeof(buf), "bundle of %s (%.1f kg)", apxPartsMaterialName(apxPartsBundleMaterial(item)), apxPartsBundleGrams(item) / 1000.0);
	return buf;
}

Item* apxPartsDrawBundle(int player, int material, int grams, std::string& why)
{
	if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { why = "no player"; return nullptr; }
	if ( material < 0 || material >= MAX_MATS || material >= (int)db().materials.size() ) { why = "no such material"; return nullptr; }
	if ( grams <= 0 || grams > 0xFFFFF ) { why = "grams out of range (1 .. 1,048,575)"; return nullptr; }
	if ( (int)ledger[player][material] < grams ) { why = "not that much in the pouch"; return nullptr; }
	ledger[player][material] -= (uint32_t)grams;
	Item* b = newItem(GEM_GLASS, SERVICABLE, 0, 1, BUNDLE_TAG | ((Uint32)material << 20) | (Uint32)grams, true, &stats[player]->inventory);
	drawnBundles[player].insert(b->uid);
	printlog("apxparts: bundle drawn player %d %s %d g appearance %08x\n", player, apxPartsMaterialName(material), grams, b->appearance);
	return b;
}

void apxPartsBundlePickedUp(int player, const Item* item)
{
	if ( !apxPartsIsBundle(item) || player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->isLocalPlayer() ) { return; }
	pendingAbsorb[player] = true; // absorbed next frame, after the caller has finished with the item
}

void apxPartsBundlesRegister(int player)
{
	if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return; }
	drawnBundles[player].clear();
	for ( node_t* n = stats[player]->inventory.first; n; n = n->next ) { const Item* it = (const Item*)n->element; if ( apxPartsIsBundle(it) ) { drawnBundles[player].insert(it->uid); } }
}

void apxPartsTick()
{
	for ( int p = 0; p < MAXPLAYERS; ++p )
	{
		if ( !pendingAbsorb[p] || !stats[p] ) { continue; }
		pendingAbsorb[p] = false;
		node_t* next = nullptr;
		for ( node_t* n = stats[p]->inventory.first; n; n = next )
		{
			next = n->next;
			Item* it = (Item*)n->element;
			if ( !apxPartsIsBundle(it) || drawnBundles[p].count(it->uid) ) { continue; }
			const int m = apxPartsBundleMaterial(it), g = apxPartsBundleGrams(it); // bundles never stack
			if ( m >= 0 && m < MAX_MATS ) { ledger[p][m] += (uint32_t)g; }
			printlog("apxparts: bundle absorbed player %d %s %d g appearance %08x pouch %lld g\n", p, apxPartsMaterialName(m), g, it->appearance, apxPartsLedgerGrams(p));
			Item* gone = it;
			consumeItem(gone, p);
		}
	}
}

// ---------------------------------------------------------------- D86 components
namespace {
constexpr Uint32 COMPONENT_TAG = 0xCu << 28;
}

bool apxPartsIsComponent(const Item* item) { return item && item->type == GEM_GLASS && apxparts::isComponent(item->apx); }

const char* apxPartsComponentName(const Item* item)
{
	static char buf[160];
	snprintf(buf, sizeof(buf), "%s", apxPartsIsComponent(item) ? apxparts::componentName(db(), item->apx.parts[0]).c_str() : "component");
	return buf;
}

int apxPartsComponentGrams(const Item* item) { return apxPartsIsComponent(item) ? apxparts::partMassGrams(db(), item->apx.parts[0]) : 0; }

int apxPartsComponentValue(const Item* item) // ASSUMED: material value x (1 + rarity)
{
	if ( !apxPartsIsComponent(item) ) { return 0; }
	const ApxPart& p = item->apx.parts[0];
	const int v = p.material < db().materials.size() ? db().materials[p.material].valuePerKg : 0;
	return std::max(1, (int)((long long)apxPartsComponentGrams(item) * v / 1000 * (1 + p.rarity)));
}

Item* apxPartsGiveComponent(int player, const ApxItemData& data)
{
	if ( player < 0 || player >= MAXPLAYERS || !stats[player] || !apxparts::isComponent(data) ) { return nullptr; }
	const ApxPart& p = data.parts[0];
	Item* c = newItem(GEM_GLASS, SERVICABLE, 0, 1, COMPONENT_TAG | ((Uint32)p.slot << 8) | (Uint32)p.material, true, nullptr);
	c->apx = data;
	Item* got = itemPickup(player, c); // itemPickup copies apx (the picked-up copy keeps its parts)
	free(c);
	++apxPartsCounters.components_given;
	printlog("apxparts: component given player %d \"%s\" slot %s material %s rarity %d affixes %d blob %016llx\n", player,
		apxparts::componentName(db(), p).c_str(), apxPartsSlotName(p.slot), apxPartsMaterialName(p.material), (int)p.rarity, (int)p.nAffix,
		(unsigned long long)apxparts::fnv1a64(&data, sizeof(data)));
	return got;
}

namespace {
void giveComponents(int player, const apxparts::SalvageResult& r)
{
	for ( auto& c : r.components ) { apxPartsGiveComponent(player, c); }
}
}

void apxPartsLedgerCredit(int player, int material, int grams) // selftest seed for the pouch, logged
{
	if ( player < 0 || player >= MAXPLAYERS || material < 0 || material >= MAX_MATS || grams <= 0 ) { return; }
	ledger[player][material] += (uint32_t)grams;
	printlog("apxparts: pouch test credit player %d %s %d g\n", player, apxPartsMaterialName(material), grams);
}

int apxPartsPouchWeight(int player)
{
	return apxPartsEnabled() ? (int)(apxPartsLedgerGrams(player) / 100) : 0;
}

long long apxPartsLedgerGrams(int player)
{
	long long g = 0;
	if ( player >= 0 && player < MAXPLAYERS ) { for ( int m = 0; m < MAX_MATS; ++m ) { g += ledger[player][m]; } }
	return g;
}

int apxPartsRepairCost(int player, const Item* item, std::string& why, std::string& material)
{
	if ( !item || item->apx.nParts == 0 ) { why = "a vanilla item has no parts to mend (use a repair scroll)"; return -1; }
	if ( player < 0 || player >= MAXPLAYERS ) { why = "no player"; return -1; }
	const int g = apx::props::repairCostGrams(apxPartsPartMassGrams(item, 0), (int)item->status);
	if ( g < 0 ) { why = "already in excellent condition"; return -1; }
	const int mat = item->apx.parts[0].material;
	material = apxPartsMaterialName(mat);
	if ( mat < 0 || mat >= MAX_MATS || (long long)ledger[player][mat] < g )
	{
		why = "need " + std::to_string(g) + " g of " + material + " (salvage some first; have " + std::to_string(mat >= 0 && mat < MAX_MATS ? ledger[player][mat] : 0) + ")";
		return -1;
	}
	return g;
}

bool apxPartsRepair(int player, Item* item, std::string& why)
{
	std::string mat;
	const int g = apxPartsRepairCost(player, item, why, mat);
	if ( g < 0 ) { return false; }
	ledger[player][item->apx.parts[0].material] -= (uint32_t)g;
	item->status = (Status)std::min((int)EXCELLENT, (int)item->status + 1);
	for ( int k = 0; k < item->apx.nParts; ++k ) { item->apx.parts[k].integrity = (uint8_t)std::min(255, item->apx.parts[k].integrity + 64); }
	printlog("apxparts: repair player %d %s -> status %d, %d g of %s", player, item->getName(), (int)item->status, g, mat.c_str());
	return true;
}

bool apxPartsSalvageItem(int player, Item* item, int keepPart, std::string& why)
{
	if ( !item || item->apx.nParts == 0 ) { why = "a vanilla item: the vanilla station path"; return false; }
	if ( keepPart >= item->apx.nParts ) { why = "no such part"; return false; }
	const apxparts::DB& d = db();
	uint8_t key[12];
	memcpy(key, &mapseed, 4);
	const uint32_t depth = (uint32_t)currentlevel;
	memcpy(key + 4, &depth, 4);
	memcpy(key + 8, &item->apx.rollKey, 4);
	const uint64_t seed = apxparts::fnv1a64("salvage", 7, apxparts::fnv1a64(key, sizeof(key)));
	const bool symmetric = getenv("APX_SALVAGE_SYMMETRIC") && *getenv("APX_SALVAGE_SYMMETRIC") == '1';
	const apxparts::SalvageResult r = apxparts::salvage(d, item->apx, keepPart, seed, symmetric);
	int metal = 0, magic = 0;
	settle(player, r, metal, magic);
	auto give = [&](ItemType t, int count) {
		if ( count <= 0 ) { return; }
		Item* it = newItem(t, DECREPIT, 0, (Sint16)count, 0, true, nullptr);
		itemPickup(player, it);
		free(it);
	};
	give(TOOL_METAL_SCRAP, metal);
	give(TOOL_MAGIC_SCRAP, magic);
	give(GEM_ROCK, r.auraCrystals);
	++apxPartsCounters.salvaged;
	apxPartsCounters.aura_stand_in += r.auraCrystals;
	printlog("apxparts: salvage-keep player %d type %d parts %d keep %d crafted %d -> components %d metal %d magic %d aura %d (mass %d -> %d g; ledger %lld g)\n", player, (int)item->type,
		(int)item->apx.nParts, keepPart, (int)r.crafted, (int)r.components.size(), metal, magic, r.auraCrystals, r.massIn, r.massOut, apxPartsLedgerGrams(player));
	Item* consumed = item; // D86: no husk; the chosen part (or every part, crafted) is its own item now
	consumeItem(consumed, player);
	giveComponents(player, r);
	return true;
}

// ---------------------------------------------------------------- probe (card rule 5; one batched launch)
namespace {
struct ProbeItem { int type, status, beatitude, count; Uint32 appearance; bool identified; ApxItemData apx; };
std::vector<ProbeItem> dumpInventory(int p)
{
	std::vector<ProbeItem> v;
	if ( !stats[p] ) { return v; }
	for ( node_t* n = stats[p]->inventory.first; n; n = n->next )
	{
		const Item* it = (const Item*)n->element;
		if ( it ) { v.push_back({ (int)it->type, (int)it->status, it->beatitude, it->count, it->appearance, it->identified, it->apx }); }
	}
	return v;
}
bool sameItem(const ProbeItem& a, const ProbeItem& b)
{
	return a.type == b.type && a.status == b.status && a.beatitude == b.beatitude && a.count == b.count && a.appearance == b.appearance
		&& a.identified == b.identified && !memcmp(&a.apx, &b.apx, sizeof(a.apx));
}
void probeRow(const std::string& json)
{
	printlog("apxprobe: %s\n", json.c_str());
	if ( const char* out = getenv("APX_BF6_OUT") )
	{
		if ( FILE* fp = fopen(out, "ab") ) { fprintf(fp, "%s\n", json.c_str()); fclose(fp); }
	}
}
std::vector<Item*> rollProbeItems(int count, list_t* inventory, const char* family = nullptr, int depth = 0)
{
	std::vector<Item*> items;
	for ( int t = 0, k = 0; t < NUMITEMS && k < count; ++t )
	{
		Item probe;
		memset(&probe, 0, sizeof(probe));
		probe.type = (ItemType)t;
		if ( !bindable(&probe) ) { continue; }
		if ( family )
		{
			const apxparts::TypeForm* tf = db().typeForm(TYPE_NAMES[t]);
			if ( !tf || db().families[tf->family].id != family ) { continue; }
		}
		Item* it = newItem((ItemType)t, SERVICABLE, 0, 1, (Uint32)(1000 + k), true, inventory);
		apxPartsBind(it, apxparts::SRC_CHEST, depth);
		items.push_back(it);
		++k;
		if ( family ) { --t; } // the same type again: a family can have few types
	}
	return items;
}
int countType(int p, ItemType t)
{
	int n = 0;
	if ( stats[p] ) { for ( node_t* nd = stats[p]->inventory.first; nd; nd = nd->next ) { const Item* it = (const Item*)nd->element; if ( it && it->type == t ) { n += it->count; } } }
	return n;
}
} // namespace

namespace { std::string shotRequest; }
const char* apxPartsShotRequest()
{
	static std::string handed;
	if ( shotRequest.empty() ) { return nullptr; }
	handed = shotRequest;
	shotRequest.clear();
	return handed.c_str();
}

namespace { bool bundlePass = false, bundleDrawOk = false; long long bundlePouchBeforePickup = 0; char bundleRow[512] = ""; }

void apxPartsProbeTick()
{
	static int state = -1;
	static Uint32 start = 0;
	static std::string probes;
	if ( state == -1 )
	{
		const char* p = getenv("APX_BF6_PROBE");
		probes = p ? p : "";
		state = probes.empty() ? 99 : 0;
	}
	const bool netOnly = probes.find("save") == std::string::npos; // the net probe runs in a LAN game on the start map (level 0)
	if ( state == 99 || intro || (!netOnly && currentlevel < 1) || !players[0] || !players[0]->entity || !stats[0] ) { return; }
	if ( state == 0 ) { start = ticks; state = 1; return; }
	if ( ticks - start < TICKS_PER_SECOND * 3 ) { return; } // let the floor settle
	const bool on = apxPartsEnabled();
	char buf[1024];
	// visual: the same sword with an iron main part, then gold; a framebuffer shot of each (palette tint)
	if ( probes.find("visual") != std::string::npos && multiplayer != CLIENT )
	{
		static Uint32 t0 = 0;
		static int step = 0;
		static std::string outDir;
		if ( outDir.empty() ) { const char* o = getenv("APX_BF6_OUT"); outDir = o ? std::string(o) : std::string("probe.jsonl"); outDir = outDir.substr(0, outDir.find_last_of("/\\") + 1); }
		auto equipMaterial = [&](const char* mat) {
			Item* it = newItem(IRON_SWORD, SERVICABLE, 0, 1, 7, true, &stats[0]->inventory);
			apxPartsBind(it, apxparts::SRC_CHEST);
			const int m = db().material(mat);
			if ( it->apx.nParts > 0 && m >= 0 ) { it->apx.parts[0].material = (uint8_t)m; }
			useItem(it, 0);
			return it->apx.nParts;
		};
		if ( step == 0 )
		{
			// the hand weapon is hidden in water (actHudWeapon): stand on the nearest dry floor tile first
			Entity* pe = players[0]->entity;
			const int px = (int)(pe->x / 16), py = (int)(pe->y / 16);
			int best = -1, bx = px, by = py;
			for ( int y = 1; y < (int)map.height - 1; ++y )
			{
				for ( int x = 1; x < (int)map.width - 1; ++x )
				{
					const Sint32* t = &map.tiles[y * MAPLAYERS + x * MAPLAYERS * map.height];
					if ( t[1] || !t[0] || (swimmingtiles && swimmingtiles[t[0]]) || (lavatiles && lavatiles[t[0]]) ) { continue; }
					const int dd = (x - px) * (x - px) + (y - py) * (y - py);
					if ( best < 0 || dd < best ) { best = dd; bx = x; by = y; }
				}
			}
			pe->x = bx * 16 + 8;
			pe->y = by * 16 + 8;
			equipMaterial("iron"); t0 = ticks; step = 1; return;
		}
		if ( step == 1 && ticks - t0 > TICKS_PER_SECOND * 2 ) { shotRequest = outDir + "visual-a.png"; t0 = ticks; step = 2; return; }
		if ( step == 2 && ticks - t0 > TICKS_PER_SECOND ) { equipMaterial("gold"); t0 = ticks; step = 3; return; }
		if ( step == 3 && ticks - t0 > TICKS_PER_SECOND * 2 ) { shotRequest = outDir + "visual-b.png"; t0 = ticks; step = 4; return; }
		if ( step == 4 && ticks - t0 > TICKS_PER_SECOND )
		{
			const bool vis = !(getenv("APX_PARTS_VIS") && *getenv("APX_PARTS_VIS") == '0');
			const Entity* hw = players[0]->hud.weapon;
			const Item* wpn = stats[0]->weapon;
			snprintf(buf, sizeof(buf), "{\"probe\":\"visual\",\"parts_on\":%d,\"vis\":%d,\"a\":\"iron\",\"b\":\"gold\",\"shots\":[\"visual-a.png\",\"visual-b.png\"],"
				"\"hud_weapon\":%d,\"hud_invisible\":%d,\"hud_sprite\":%d,\"hud_light\":[%.2f,%.2f,%.2f],\"weapon_parts\":%d,\"weapon_material\":\"%s\"}",
				on ? 1 : 0, vis ? 1 : 0, hw ? 1 : 0, hw ? (int)hw->flags[INVISIBLE] : -1, hw ? (int)hw->sprite : -1,
				hw ? hw->lightBonus.x : -1.f, hw ? hw->lightBonus.y : -1.f, hw ? hw->lightBonus.z : -1.f,
				wpn ? (int)wpn->apx.nParts : -1, wpn && wpn->apx.nParts ? apxPartsMaterialName(wpn->apx.parts[0].material) : "-");
			probeRow(buf);
			exit(0);
		}
		return;
	}
	if ( state == 1 && probes.find("save") != std::string::npos && multiplayer != CLIENT )
	{
		state = 2;
		auto rolled = rollProbeItems(40, &stats[0]->inventory);
		// forced deep rolls (depth 40, quality 10): a Tools item has exactly 6 parts, a Hafts item 8
		for ( auto* it : rollProbeItems(1, &stats[0]->inventory, "Tools", 40) ) { rolled.push_back(it); }
		for ( auto* it : rollProbeItems(1, &stats[0]->inventory, "Hafts", 40) ) { rolled.push_back(it); }
		int withParts = 0, sixParts = 0, maxParts = 0;
		for ( auto* it : rolled ) { withParts += it->apx.nParts > 0; sixParts += it->apx.nParts == 6; maxParts = std::max<int>(maxParts, it->apx.nParts); }
		// Formbook: break 5 Blades items -> Blades mastered (5 successes); the book must survive save/load
		int broke = 0;
		uint32_t bladesForm = 0;
		for ( auto* it : rollProbeItems(5, &stats[0]->inventory, "Blades") )
		{
			std::string why;
			bladesForm = it->apx.form;
			broke += apxPartsBreak(0, it, why) ? 1 : 0;
		}
		const int bladesSucc = apxPartsFormSuccesses(0, bladesForm);
		const uint64_t bookBefore = apxPartsFormbookHash(0);
		const auto before = dumpInventory(0);
		const int saved = saveGame(0);
		const SaveGameInfo info = getSaveGameInfo(multiplayer == SINGLE, 0);
		const long long droppedBefore = apxPartsCounters.save_blob_dropped;
		const int loaded = info.game_version == -1 ? 1 : loadGame(0, info);
		const auto after = dumpInventory(0);
		int equal = 0, partsKept = 0, partsBefore = 0;
		for ( size_t k = 0; k < before.size() && k < after.size(); ++k )
		{
			equal += sameItem(before[k], after[k]);
			partsBefore += before[k].apx.nParts > 0;
			partsKept += before[k].apx.nParts > 0 && !memcmp(&before[k].apx, &after[k].apx, sizeof(ApxItemData));
		}
		const bool strip = getenv("APX_SAVE_STRIP") && *getenv("APX_SAVE_STRIP") == '1';
		const uint64_t bookAfter = apxPartsFormbookHash(0);
		const bool bookOk = !on || strip || (broke == 5 && bladesSucc == 5 && bookAfter == bookBefore);
		const bool pass = saved == 0 && loaded == 0 && before.size() == after.size() && (on && !strip ? equal == (int)before.size() && withParts == (int)rolled.size() && sixParts >= 1 : true)
			&& apxPartsCounters.save_blob_dropped == droppedBefore && bookOk;
		snprintf(buf, sizeof(buf), "{\"probe\":\"save\",\"parts_on\":%d,\"save_strip\":%d,\"rolled\":%d,\"with_parts\":%d,\"six_parts\":%d,\"max_parts\":%d,"
			"\"inventory_before\":%d,\"inventory_after\":%d,\"items_equal\":%d,\"parts_items\":%d,\"parts_kept\":%d,\"saved\":%d,\"loaded\":%d,"
			"\"broke\":%d,\"blades_successes\":%d,\"formbook_before\":\"%016llx\",\"formbook_after\":\"%016llx\",\"formbook_equal\":%d,"
			"\"save_blob_dropped\":%lld,\"parts_overflow\":%lld,\"ip_clamped\":%lld,\"ip_rerolls\":%lld,\"forms_unmapped\":%lld,\"pass\":%s}",
			on ? 1 : 0, strip ? 1 : 0, (int)rolled.size(), withParts, sixParts, maxParts, (int)before.size(), (int)after.size(), equal, partsBefore, partsKept,
			saved, loaded, broke, bladesSucc, (unsigned long long)bookBefore, (unsigned long long)bookAfter, bookAfter == bookBefore ? 1 : 0,
			apxPartsCounters.save_blob_dropped, apxPartsCounters.parts_overflow, apxPartsCounters.ip_clamped,
			apxPartsCounters.ip_rerolls, apxPartsCounters.forms_unmapped, pass ? "true" : "false");
		probeRow(buf);
		// salvage, live: 10 items through Barony's tinkering station (keeps none), 10 through the bench path (keeps part 0)
		bool salvageOk = true;
		if ( probes.find("salvage") != std::string::npos )
		{
			const int metal0 = countType(0, TOOL_METAL_SCRAP), magic0 = countType(0, TOOL_MAGIC_SCRAP), gem0 = countType(0, GEM_ROCK);
			const long long salv0 = apxPartsCounters.salvaged, aura0 = apxPartsCounters.aura_stand_in;
			const long long out0 = apxPartsCounters.salvage_grams_out, paid0 = apxPartsCounters.salvage_grams_paid, ledger0 = apxPartsLedgerGrams(0);
			auto countInv = [&]() { int n = 0; for ( node_t* nd = stats[0]->inventory.first; nd; nd = nd->next ) { ++n; } return n; };
			auto items = rollProbeItems(20, &stats[0]->inventory);
			const int invBefore = countInv();
			int station = 0, bench = 0, keptOne = 0, expectComps = 0;
			auto countComps = [&]() { int n = 0; for ( node_t* nd = stats[0]->inventory.first; nd; nd = nd->next ) { n += apxPartsIsComponent((const Item*)nd->element); } return n; };
			const int comps0 = countComps();
			const long long given0 = apxPartsCounters.components_given;
			for ( size_t k = 0; k < items.size(); ++k )
			{
				Item* it = items[k];
				if ( k < 10 ) { expectComps += (it->apx.flags & apxparts::FLAG_CRAFTED) ? it->apx.nParts : 0; station += GenericGUI[0].tinkeringSalvageItem(it, false, 0) ? 1 : 0; }
				else
				{
					std::string why;
					const ApxPart want = it->apx.parts[0];
					const bool crafted = (it->apx.flags & apxparts::FLAG_CRAFTED) != 0;
					const int n = it->apx.nParts;
					const long long g0 = apxPartsCounters.components_given;
					if ( apxPartsSalvageItem(0, it, 0, why) ) // D86: the item is gone; part 0 is its own component item
					{
						++bench;
						expectComps += crafted ? n : 1;
						const Item* last = nullptr;
						for ( node_t* nd = stats[0]->inventory.first; nd; nd = nd->next ) { const Item* c = (const Item*)nd->element; if ( apxPartsIsComponent(c) ) { last = c; } }
						keptOne += apxPartsCounters.components_given - g0 == (crafted ? n : 1) && last && last->apx.parts[0].slot == want.slot && last->apx.parts[0].material == want.material;
					}
				}
			}
			const int compsGained = countComps() - comps0;
			const long long compsGiven = apxPartsCounters.components_given - given0;
			// a crafted item through the bench: every part comes back as a component
			int craftedParts = 0, craftedComps = 0;
			{
				auto cr = rollProbeItems(1, &stats[0]->inventory, "Blades");
				if ( !cr.empty() )
				{
					cr[0]->apx.flags = (uint8_t)((cr[0]->apx.flags & ~apxparts::FLAG_LOOTED) | apxparts::FLAG_CRAFTED);
					craftedParts = cr[0]->apx.nParts;
					const int c0 = countComps();
					std::string why;
					apxPartsSalvageItem(0, cr[0], -1, why);
					craftedComps = countComps() - c0;
				}
			}
			char compName[160] = "";
			for ( node_t* nd = stats[0]->inventory.first; nd; nd = nd->next ) { const Item* c = (const Item*)nd->element; if ( apxPartsIsComponent(c) && c->apx.parts[0].nAffix > 0 ) { snprintf(compName, sizeof(compName), "%s", c->getName()); } }
			std::vector<ApxItemData> compsBefore;
			for ( node_t* nd = stats[0]->inventory.first; nd; nd = nd->next ) { const Item* c = (const Item*)nd->element; if ( apxPartsIsComponent(c) ) { compsBefore.push_back(c->apx); } }
			const int metal = countType(0, TOOL_METAL_SCRAP) - metal0, magic = countType(0, TOOL_MAGIC_SCRAP) - magic0, gems = countType(0, GEM_ROCK) - gem0;
			const long long salvaged = apxPartsCounters.salvaged - salv0, aura = apxPartsCounters.aura_stand_in - aura0;
			const long long gramsOut = apxPartsCounters.salvage_grams_out - out0, gramsPaid = apxPartsCounters.salvage_grams_paid - paid0, ledgerGain = apxPartsLedgerGrams(0) - ledger0;
			const bool conserved = gramsOut == gramsPaid + ledgerGain; // no salvaged gram is lost
			// D85 pouch: Barony's carry weight = items + gold + pouch; the pouch survives save/load
			int itemWeight = 0;
			for ( node_t* nd = stats[0]->inventory.first; nd; nd = nd->next ) { const Item* it = (const Item*)nd->element; if ( it && it->type >= 0 && it->type < NUMITEMS ) { itemWeight += it->getWeight(); } }
			const int charWeight = players[0]->movement.getCharacterWeight();
			const int pouchUnits = (int)apx_bench::pouchUnits(0); // U1 owns the carry rule (actplayer.cpp)
			const bool weightOk = !on || charWeight == itemWeight + stats[0]->getGoldWeight() + pouchUnits;
			const long long pouchBefore = apxPartsLedgerGrams(0);
			const int saved2 = saveGame(0);
			const SaveGameInfo info2 = getSaveGameInfo(multiplayer == SINGLE, 0);
			const int loaded2 = info2.game_version == -1 ? 1 : loadGame(0, info2);
			const long long pouchAfter = apxPartsLedgerGrams(0);
			const bool pouchOk = !on || (saved2 == 0 && loaded2 == 0 && pouchAfter == pouchBefore && pouchBefore > 0);
			int compsKept = 0; // D86: every component item survives save/load byte-equal
			{
				size_t k = 0;
				for ( node_t* nd = stats[0]->inventory.first; nd; nd = nd->next )
				{
					const Item* c = (const Item*)nd->element;
					if ( apxPartsIsComponent(c) ) { compsKept += k < compsBefore.size() && !memcmp(&compsBefore[k], &c->apx, sizeof(ApxItemData)); ++k; }
				}
			}
			const bool compsOk = !on || (compsGained == expectComps && compsGiven == expectComps && expectComps >= 10 && craftedParts >= 2 && craftedComps == craftedParts
				&& compsKept == (int)compsBefore.size() && !compsBefore.empty() && compName[0]);
			const int invAfter = countInv(); // 20 items consumed (+1 crafted); components are new items; scrap/gem stacks may appear
			const bool invOk = invAfter >= invBefore - 21 + expectComps + craftedComps;
			salvageOk = !on || (station == 10 && bench == 10 && keptOne == 10 && salvaged == 21 && gems == aura && metal + magic > 0 && conserved && gramsOut > 0 && invOk && weightOk && pouchOk && compsOk);
			snprintf(buf, sizeof(buf), "{\"probe\":\"components\",\"expected\":%d,\"gained\":%d,\"given\":%lld,\"crafted_parts\":%d,\"crafted_components\":%d,\"kept_after_load\":%d,\"before_load\":%d,\"name\":\"%s\",\"pass\":%s}",
				expectComps, compsGained, compsGiven, craftedParts, craftedComps, compsKept, (int)compsBefore.size(), compName, compsOk ? "true" : "false");
			probeRow(buf);
			snprintf(buf, sizeof(buf), "{\"probe\":\"salvage\",\"parts_on\":%d,\"station_ok\":%d,\"bench_ok\":%d,\"bench_kept_one_part\":%d,\"parts_salvaged\":%lld,"
				"\"metal_scrap\":%d,\"magic_scrap\":%d,\"aura_gems\":%d,\"aura_counted\":%lld,\"grams_out\":%lld,\"grams_paid\":%lld,\"ledger_gain\":%lld,\"conserved\":%d,\"inventory_before\":%d,\"inventory_after\":%d,\"pouch_units\":%d,\"carry_weight\":%d,\"item_weight\":%d,\"weight_ok\":%d,\"pouch_before\":%lld,\"pouch_after\":%lld,\"pouch_saved\":%d,\"pass\":%s}",
				on ? 1 : 0, station, bench, keptOne, salvaged, metal, magic, gems, aura, gramsOut, gramsPaid, ledgerGain, conserved ? 1 : 0, invBefore, invAfter, pouchUnits, charWeight, itemWeight, weightOk ? 1 : 0, pouchBefore, pouchAfter, pouchOk ? 1 : 0, salvageOk ? "true" : "false");
			probeRow(buf);
		}
		if ( probes.find("bundle") != std::string::npos && on )
		{
			// D85: draw two bundles from the pouch, save/load (they stay items), then pick a fresh one up
			bundlePass = pass && salvageOk;
			int mats[2] = { -1, -1 };
			for ( int m = 0; m < (int)db().materials.size(); ++m )
			{
				const int g = apxPartsLedgerMaterialGrams(0, m);
				if ( mats[0] < 0 || g > apxPartsLedgerMaterialGrams(0, mats[0]) ) { mats[1] = mats[0]; mats[0] = m; }
				else if ( mats[1] < 0 || g > apxPartsLedgerMaterialGrams(0, mats[1]) ) { mats[1] = m; }
			}
			if ( mats[1] < 0 || apxPartsLedgerMaterialGrams(0, mats[1]) < 2 ) { apxPartsLedgerCredit(0, db().material("oak"), 400); mats[1] = db().material("oak"); }
			const long long pouch0 = apxPartsLedgerGrams(0);
			std::string why;
			Uint32 app[2] = { 0, 0 };
			int drawnGrams = 0;
			for ( int k = 0; k < 2; ++k )
			{
				const int g = std::max(1, apxPartsLedgerMaterialGrams(0, mats[k]) / 2);
				Item* b = apxPartsDrawBundle(0, mats[k], g, why);
				if ( b ) { app[k] = b->appearance; drawnGrams += g; }
			}
			const long long pouch1 = apxPartsLedgerGrams(0);
			char nm[2][96] = { "", "" };
			for ( node_t* nd = stats[0]->inventory.first; nd; nd = nd->next )
			{
				const Item* it = (const Item*)nd->element;
				for ( int k = 0; k < 2; ++k ) { if ( apxPartsIsBundle(it) && it->appearance == app[k] ) { snprintf(nm[k], sizeof(nm[k]), "%s", it->getName()); } }
			}
			const int saved3 = saveGame(0);
			const SaveGameInfo info3 = getSaveGameInfo(multiplayer == SINGLE, 0);
			const int loaded3 = info3.game_version == -1 ? 1 : loadGame(0, info3);
			int kept = 0;
			for ( node_t* nd = stats[0]->inventory.first; nd; nd = nd->next ) { const Item* it = (const Item*)nd->element; kept += apxPartsIsBundle(it) && (it->appearance == app[0] || it->appearance == app[1]); }
			bundleDrawOk = app[0] && app[1] && pouch0 - pouch1 == drawnGrams && saved3 == 0 && loaded3 == 0 && kept == 2;
			// a bundle from outside (a partner's, a chest's) picked up: back to the pouch next frame
			bundlePouchBeforePickup = apxPartsLedgerGrams(0);
			Item* incoming = newItem(GEM_GLASS, SERVICABLE, 0, 1, (0xBu << 28) | ((Uint32)db().material("iron") << 20) | 250u, true, nullptr);
			itemPickup(0, incoming);
			free(incoming);
			snprintf(bundleRow, sizeof(bundleRow), "\"drawn\":[\"%08x\",\"%08x\"],\"names\":[\"%s\",\"%s\"],\"drawn_grams\":%d,\"pouch_drop\":%lld,"
				"\"kept_after_load\":%d,\"draw_ok\":%d", app[0], app[1], nm[0], nm[1], drawnGrams, pouch0 - pouch1, kept, bundleDrawOk ? 1 : 0);
			state = 20;
			return;
		}
		if ( probes.find("net") == std::string::npos ) { exit(pass && salvageOk ? 0 : 3); }
		return;
	}
	if ( state == 20 ) // the frame after the pickup: apxPartsTick has absorbed it
	{
		const long long gain = apxPartsLedgerGrams(0) - bundlePouchBeforePickup;
		int bundlesLeft = 0;
		for ( node_t* nd = stats[0]->inventory.first; nd; nd = nd->next ) { bundlesLeft += apxPartsIsBundle((const Item*)nd->element); }
		const bool absorbOk = gain == 250 && bundlesLeft == 2;
		snprintf(buf, sizeof(buf), "{\"probe\":\"bundle\",%s,\"pickup_gain\":%lld,\"bundles_in_inventory\":%d,\"absorb_ok\":%d,\"pass\":%s}",
			bundleRow, gain, bundlesLeft, absorbOk ? 1 : 0, bundleDrawOk && absorbOk ? "true" : "false");
		probeRow(buf);
		exit(bundlePass && bundleDrawOk && absorbOk ? 0 : 3);
	}
	if ( state <= 2 && probes.find("net") != std::string::npos && multiplayer == SERVER )
	{
		bool clientIn = false;
		for ( int c = 1; c < MAXPLAYERS; ++c ) { clientIn = clientIn || (!client_disconnected[c] && players[c] && players[c]->entity); }
		if ( !clientIn ) { return; }
		state = 3;
		auto rolled = rollProbeItems(40, nullptr);
		int withParts = 0;
		for ( auto* it : rolled )
		{
			withParts += it->apx.nParts > 0;
			itemPickup(1, it); // the real server -> client pickup path: ITEM packet with the parts blob
			free(it);
		}
		Uint32 sentBundle = 0;
		if ( on )
		{
			// D85: a bundle from the host's pouch to the client (ITEM packet); the client's pouch must absorb it
			std::string why;
			apxPartsLedgerCredit(0, db().material("iron"), 1000);
			if ( Item* b = apxPartsDrawBundle(0, db().material("iron"), 600, why) )
			{
				sentBundle = b->appearance;
				itemPickup(1, b);
				Item* gone = b;
				consumeItem(gone, 0);
			}
		}
		unsigned long long sentComp = 0;
		if ( on )
		{
			// D86: a component item to the client (ITEM packet + blob); the client must hold it byte-equal
			auto src = rollProbeItems(1, nullptr, "Blades", 30);
			if ( !src.empty() )
			{
				int best = 0;
				for ( int k = 1; k < src[0]->apx.nParts; ++k ) { if ( src[0]->apx.parts[k].nAffix > src[0]->apx.parts[best].nAffix ) { best = k; } }
				const ApxItemData c = apxparts::componentOf(db(), src[0]->apx.parts[best], src[0]->apx.rollKey, apxparts::FLAG_LOOTED);
				sentComp = (unsigned long long)apxparts::fnv1a64(&c, sizeof(c));
				apxPartsGiveComponent(1, c);
				free(src[0]);
			}
		}
		snprintf(buf, sizeof(buf), "{\"probe\":\"net\",\"role\":\"host\",\"parts_on\":%d,\"given\":%d,\"with_parts\":%d,\"net_blob_sent\":%lld,\"net_blob_dropped\":%lld,\"bundle_sent\":\"%08x\",\"component_sent\":\"%016llx\"}",
			on ? 1 : 0, (int)rolled.size(), withParts, apxPartsCounters.net_blob_sent, apxPartsCounters.net_blob_dropped, sentBundle, sentComp);
		probeRow(buf);
	}
	// client side of the net probe: once the host's 40 items are in, break 3 (FBOK), drop 5 (DROP blobs), equip 1 (EQUI blob)
	if ( state <= 2 && probes.find("netclient") != std::string::npos && multiplayer == CLIENT )
	{
		std::vector<Item*> withParts;
		for ( node_t* nd = stats[clientnum]->inventory.first; nd; nd = nd->next )
		{
			Item* it = (Item*)nd->element;
			if ( it && it->apx.nParts > 0 && !apxPartsIsComponent(it) ) { withParts.push_back(it); }
		}
		if ( withParts.size() < 40 ) { return; }
		if ( ticks - start < TICKS_PER_SECOND * 6 ) { return; }
		state = 3;
		// break 3 items of one Form (a new Form needs a free INT/2 slot; one Form keeps it to one slot)
		int broke = 0;
		const uint32_t form = withParts[0]->apx.form;
		std::vector<Item*> sameForm, rest;
		for ( auto* it : withParts ) { (it->apx.form == form && sameForm.size() < 3 ? sameForm : rest).push_back(it); }
		for ( auto* it : sameForm ) { std::string why; broke += apxPartsBreak(clientnum, it, why) ? 1 : 0; }
		int dropped = 0, equipped = 0;
		for ( auto* it : rest )
		{
			if ( dropped < 5 && itemCategory(it) != WEAPON ) { dropItem(it, clientnum); ++dropped; continue; }
			if ( !equipped && itemCategory(it) == WEAPON ) { useItem(it, clientnum); ++equipped; }
			if ( dropped >= 5 && equipped ) { break; }
		}
		Uint32 droppedBundle = 0;
		{
			std::string why;
			apxPartsLedgerCredit(clientnum, db().material("oak"), 500);
			if ( Item* b = apxPartsDrawBundle(clientnum, db().material("oak"), 450, why) ) { droppedBundle = b->appearance; dropItem(b, clientnum); }
		}
		unsigned long long recvComp = 0, droppedComp = 0;
		char recvName[160] = "";
		for ( node_t* nd = stats[clientnum]->inventory.first; nd; nd = nd->next )
		{
			Item* c = (Item*)nd->element;
			if ( apxPartsIsComponent(c) ) { recvComp = (unsigned long long)apxparts::fnv1a64(&c->apx, sizeof(c->apx)); snprintf(recvName, sizeof(recvName), "%s", c->getName()); droppedComp = recvComp; dropItem(c, clientnum); break; }
		}
		snprintf(buf, sizeof(buf), "{\"probe\":\"netclient\",\"role\":\"client\",\"received_with_parts\":%d,\"broke\":%d,\"dropped\":%d,\"equipped\":%d,\"formbook\":\"%016llx\",\"bundle_dropped\":\"%08x\",\"component_received\":\"%016llx\",\"component_name\":\"%s\",\"component_dropped\":\"%016llx\"}",
			(int)withParts.size(), broke, dropped, equipped, (unsigned long long)apxPartsFormbookHash(clientnum), droppedBundle, recvComp, recvName, droppedComp);
		probeRow(buf);
	}
}

bool apxPartsSalvageValue(int player, const Item* item, int* metal, int* magic, int* auraCrystals)
{
	if ( !item || !apxPartsEnabled() || item->apx.nParts == 0 ) { return false; }
	const apxparts::DB& d = db();
	uint8_t key[12];
	memcpy(key, &mapseed, 4);
	const uint32_t depth = (uint32_t)currentlevel;
	memcpy(key + 4, &depth, 4);
	memcpy(key + 8, &item->apx.rollKey, 4);
	const uint64_t seed = apxparts::fnv1a64("salvage", 7, apxparts::fnv1a64(key, sizeof(key)));
	const bool symmetric = getenv("APX_SALVAGE_SYMMETRIC") && *getenv("APX_SALVAGE_SYMMETRIC") == '1';
	const apxparts::SalvageResult r = apxparts::salvage(d, item->apx, -1, seed, symmetric);
	int mu = 0, gu = 0;
	settle(player, r, mu, gu);
	if ( metal ) { *metal = mu; }
	if ( magic ) { *magic = gu; }
	if ( auraCrystals ) { *auraCrystals = r.auraCrystals; }
	giveComponents(player, r); // D86: a crafted item's parts come back as component items
	++apxPartsCounters.salvaged;
	apxPartsCounters.aura_stand_in += r.auraCrystals;
	printlog("apxparts: salvage type %d parts %d mass %d g -> %d g (looted %d, discharged %d) metal %d magic %d aura %d ledger %lld g\n",
		(int)item->type, (int)item->apx.nParts, r.massIn, r.massOut, (int)r.looted, (int)r.discharged, mu, gu, r.auraCrystals, apxPartsLedgerGrams(player));
	return true;
}
