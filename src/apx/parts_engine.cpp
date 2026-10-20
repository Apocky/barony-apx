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
#include "parts_model.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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

long long apxPartsLedgerGrams(int player)
{
	long long g = 0;
	if ( player >= 0 && player < MAXPLAYERS ) { for ( int m = 0; m < MAX_MATS; ++m ) { g += ledger[player][m]; } }
	return g;
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
	printlog("apxparts: salvage-keep player %d type %d parts %d keep %d -> metal %d magic %d aura %d (mass %d -> %d g; ledger %lld g)\n", player, (int)item->type,
		(int)item->apx.nParts, keepPart, metal, magic, r.auraCrystals, r.massIn, r.massOut, apxPartsLedgerGrams(player));
	if ( keepPart >= 0 ) { item->apx = r.kept; } // the item now holds only the kept part
	else { Item* consumed = item; consumeItem(consumed, player); }
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
			auto items = rollProbeItems(20, &stats[0]->inventory);
			int station = 0, bench = 0, keptOne = 0;
			for ( size_t k = 0; k < items.size(); ++k )
			{
				Item* it = items[k];
				if ( k < 10 ) { station += GenericGUI[0].tinkeringSalvageItem(it, false, 0) ? 1 : 0; }
				else
				{
					std::string why;
					if ( apxPartsSalvageItem(0, it, 0, why) ) { ++bench; keptOne += it->apx.nParts == 1; }
				}
			}
			const int metal = countType(0, TOOL_METAL_SCRAP) - metal0, magic = countType(0, TOOL_MAGIC_SCRAP) - magic0, gems = countType(0, GEM_ROCK) - gem0;
			const long long salvaged = apxPartsCounters.salvaged - salv0, aura = apxPartsCounters.aura_stand_in - aura0;
			const long long gramsOut = apxPartsCounters.salvage_grams_out - out0, gramsPaid = apxPartsCounters.salvage_grams_paid - paid0, ledgerGain = apxPartsLedgerGrams(0) - ledger0;
			const bool conserved = gramsOut == gramsPaid + ledgerGain; // no salvaged gram is lost
			salvageOk = !on || (station == 10 && bench == 10 && keptOne == 10 && salvaged == 20 && gems == aura && metal + magic > 0 && conserved && gramsOut > 0);
			snprintf(buf, sizeof(buf), "{\"probe\":\"salvage\",\"parts_on\":%d,\"station_ok\":%d,\"bench_ok\":%d,\"bench_kept_one_part\":%d,\"parts_salvaged\":%lld,"
				"\"metal_scrap\":%d,\"magic_scrap\":%d,\"aura_gems\":%d,\"aura_counted\":%lld,\"grams_out\":%lld,\"grams_paid\":%lld,\"ledger_gain\":%lld,\"conserved\":%d,\"pass\":%s}",
				on ? 1 : 0, station, bench, keptOne, salvaged, metal, magic, gems, aura, gramsOut, gramsPaid, ledgerGain, conserved ? 1 : 0, salvageOk ? "true" : "false");
			probeRow(buf);
		}
		if ( probes.find("net") == std::string::npos ) { exit(pass && salvageOk ? 0 : 3); }
		if ( probes.find("net") == std::string::npos ) { exit(pass ? 0 : 3); }
		return;
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
		snprintf(buf, sizeof(buf), "{\"probe\":\"net\",\"role\":\"host\",\"parts_on\":%d,\"given\":%d,\"with_parts\":%d,\"net_blob_sent\":%lld,\"net_blob_dropped\":%lld}",
			on ? 1 : 0, (int)rolled.size(), withParts, apxPartsCounters.net_blob_sent, apxPartsCounters.net_blob_dropped);
		probeRow(buf);
	}
	// client side of the net probe: once the host's 40 items are in, break 3 (FBOK), drop 5 (DROP blobs), equip 1 (EQUI blob)
	if ( state <= 2 && probes.find("netclient") != std::string::npos && multiplayer == CLIENT )
	{
		std::vector<Item*> withParts;
		for ( node_t* nd = stats[clientnum]->inventory.first; nd; nd = nd->next )
		{
			Item* it = (Item*)nd->element;
			if ( it && it->apx.nParts > 0 ) { withParts.push_back(it); }
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
		snprintf(buf, sizeof(buf), "{\"probe\":\"netclient\",\"role\":\"client\",\"received_with_parts\":%d,\"broke\":%d,\"dropped\":%d,\"equipped\":%d,\"formbook\":\"%016llx\"}",
			(int)withParts.size(), broke, dropped, equipped, (unsigned long long)apxPartsFormbookHash(clientnum));
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
	++apxPartsCounters.salvaged;
	apxPartsCounters.aura_stand_in += r.auraCrystals;
	printlog("apxparts: salvage type %d parts %d mass %d g -> %d g (looted %d, discharged %d) metal %d magic %d aura %d ledger %lld g\n",
		(int)item->type, (int)item->apx.nParts, r.massIn, r.massOut, (int)r.looted, (int)r.discharged, mu, gu, r.auraCrystals, apxPartsLedgerGrams(player));
	return true;
}
