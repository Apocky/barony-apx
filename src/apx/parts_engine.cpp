// F6 parts-based gear, Barony side (card BF6-2/3/4/6). See parts_engine.hpp.
#include "../main.hpp"
#include "../game.hpp"
#include "../entity.hpp"
#include "../items.hpp"
#include "../net.hpp"
#include "../player.hpp"
#include "../scores.hpp"
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
		const char* dir = getenv("APX_PARTS_DATA");
		const std::string e = apxparts::loadDB(dir && *dir ? dir : APX_PARTS_DATA_DIR, d);
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
	floorParts.clear();
}

void apxPartsBind(Item* item, int source)
{
	if ( !apxPartsEnabled() || !bindable(item) || item->apx.nParts > 0 ) { return; }
	const apxparts::DB& d = db();
	const apxparts::TypeForm* tf = d.typeForm(TYPE_NAMES[item->type]);
	apxparts::Counters c;
	apxparts::TypeForm plain;
	plain.family = d.plainFamily;
	plain.materials.push_back(0);
	const bool capOn = !(getenv("APX_IP_CAP") && *getenv("APX_IP_CAP") == '0');
	apxparts::roll(d, tf ? *tf : plain, (uint32_t)mapseed, currentlevel, source, floorIndex++, item->apx, c, capOn);
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
	}
	fclose(fp);
	printlog("apxparts: sidecar %s rows %d\n", path.c_str(), rows);
}

void apxPartsSidecarLoad(const char* savePath)
{
	sidecar.clear();
	if ( !savePath ) { return; }
	FILE* fp = fopen((std::string(savePath) + ".apxparts").c_str(), "rb");
	if ( !fp ) { return; }
	char line[4096];
	while ( fgets(line, sizeof(line), fp) )
	{
		int c = -1, index = -1, used = 0;
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
std::vector<Item*> rollProbeItems(int count, list_t* inventory)
{
	std::vector<Item*> items;
	for ( int t = 0, k = 0; t < NUMITEMS && k < count; ++t )
	{
		Item probe;
		memset(&probe, 0, sizeof(probe));
		probe.type = (ItemType)t;
		if ( !bindable(&probe) ) { continue; }
		Item* it = newItem((ItemType)t, SERVICABLE, 0, 1, (Uint32)(1000 + k), true, inventory);
		apxPartsBind(it, apxparts::SRC_CHEST);
		items.push_back(it);
		++k;
	}
	return items;
}
} // namespace

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
	if ( state == 1 && probes.find("save") != std::string::npos && multiplayer != CLIENT )
	{
		state = 2;
		auto rolled = rollProbeItems(40, &stats[0]->inventory);
		int withParts = 0, sixParts = 0, maxParts = 0;
		for ( auto* it : rolled ) { withParts += it->apx.nParts > 0; sixParts += it->apx.nParts == 6; maxParts = std::max<int>(maxParts, it->apx.nParts); }
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
		const bool pass = saved == 0 && loaded == 0 && before.size() == after.size() && (on && !strip ? equal == (int)before.size() && withParts == (int)rolled.size() : true)
			&& apxPartsCounters.save_blob_dropped == droppedBefore;
		snprintf(buf, sizeof(buf), "{\"probe\":\"save\",\"parts_on\":%d,\"save_strip\":%d,\"rolled\":%d,\"with_parts\":%d,\"six_parts\":%d,\"max_parts\":%d,"
			"\"inventory_before\":%d,\"inventory_after\":%d,\"items_equal\":%d,\"parts_items\":%d,\"parts_kept\":%d,\"saved\":%d,\"loaded\":%d,"
			"\"save_blob_dropped\":%lld,\"parts_overflow\":%lld,\"ip_clamped\":%lld,\"ip_rerolls\":%lld,\"forms_unmapped\":%lld,\"pass\":%s}",
			on ? 1 : 0, strip ? 1 : 0, (int)rolled.size(), withParts, sixParts, maxParts, (int)before.size(), (int)after.size(), equal, partsBefore, partsKept,
			saved, loaded, apxPartsCounters.save_blob_dropped, apxPartsCounters.parts_overflow, apxPartsCounters.ip_clamped,
			apxPartsCounters.ip_rerolls, apxPartsCounters.forms_unmapped, pass ? "true" : "false");
		probeRow(buf);
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
}

bool apxPartsSalvageValue(const Item* item, int* metal, int* magic, int* auraCrystals)
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
	long long g[2] = { 0, 0 }; // metal, magic grams
	for ( auto& mg : r.grams )
	{
		const std::string& cls = d.materials[mg.first].salvageClass;
		if ( cls == "metal" ) { g[0] += mg.second; } else if ( cls == "magic" ) { g[1] += mg.second; }
	}
	int per[2] = { 400, 200 };
	for ( auto& s : d.salvage.standIns ) { if ( s.cls == "metal" ) { per[0] = std::max(1, s.grams); } else if ( s.cls == "magic" ) { per[1] = std::max(1, s.grams); } }
	if ( metal ) { *metal = (int)(g[0] / per[0]); }
	if ( magic ) { *magic = (int)(g[1] / per[1]); }
	if ( auraCrystals ) { *auraCrystals = r.auraCrystals; }
	++apxPartsCounters.salvaged;
	apxPartsCounters.aura_stand_in += r.auraCrystals;
	printlog("apxparts: salvage type %d parts %d mass %d g -> %d g (looted %d, discharged %d) metal %lld g magic %lld g aura %d\n",
		(int)item->type, (int)item->apx.nParts, r.massIn, r.massOut, (int)r.looted, (int)r.discharged, g[0], g[1], r.auraCrystals);
	return true;
}
