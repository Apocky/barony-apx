// H1a: the Hollow site (gameplay/20 sections 2-3). A surface pocket, four floors and the cave layer,
// linked by stairs, a shaft, a flooded sump and a collapsed level; per floor a faction with dens
// and patrol routes; the density rules Alive (rooms within 16 m of a den or patrol), Lit (rooms
// with an emissive) and Readable (writings with a text kind) measured by site_critic.
#include "apxgen/apxgen.hpp"
#include <algorithm>
#include <deque>
#include <map>
#include <set>

namespace apx {

namespace {
const int D4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

EntityRow* find_entity(Floor& f, const std::string& kind) {
    for (auto& e : f.entities) if (e.kind == kind) return &e;
    return nullptr;
}

// 64 x 64 surface pocket: open sky, a stream, a ruin with the way down, a copse, a camp
void build_surface(uint64_t seed, Floor& f, std::vector<std::string>& parts) {
    const int S = 64;
    f = Floor{};
    f.W = f.H = S;
    f.theme = "surface";
    f.tile.assign((size_t)S * S, FLOOR);
    f.feat.assign((size_t)S * S, F_SKY);
    f.owner.assign((size_t)S * S, -1);
    f.behind.assign((size_t)S * S, 0);
    f.zone.assign((size_t)S * S, Z_NONE);
    Rng r(hash_words({seed, 0x5A5F}));
    for (int z = 0; z < S; ++z)
        for (int x = 0; x < S; ++x)
            if (x == 0 || z == 0 || x == S - 1 || z == S - 1) { f.tile[f.idx(x, z)] = HARD; f.feat[f.idx(x, z)] = F_NONE; }
    // stream: north to south, a random walk of its x every 6 rows, 2 wide, shallow (walkable)
    int sx = 34 + (int)r.next(6);
    for (int z = 1; z < S - 1; ++z) {
        if (z % 6 == 0) sx = std::max(28, std::min(44, sx + (int)r.next(3) - 1));
        for (int x = sx; x < sx + 2; ++x) f.feat[f.idx(x, z)] = F_WATER;
        for (int x = sx - 1; x < sx + 3; x += 3) if (r.next(4) == 0) f.dress.push_back({"prop", "reeds", 0, x, z, 0, Z_NONE, -1, "surface:stream", ""});
    }
    parts.push_back("stream");
    // ruin: a broken rectangle of diggable stone, a door gap south and east, the way down inside
    const int rx0 = 6, rz0 = 6, rx1 = 6 + 14 + (int)r.next(4), rz1 = 6 + 10 + (int)r.next(4);
    for (int z = rz0; z <= rz1; ++z)
        for (int x = rx0; x <= rx1; ++x) {
            bool wall = x == rx0 || x == rx1 || z == rz0 || z == rz1;
            if (!wall) continue;
            bool gap = (z == rz1 && std::abs(x - (rx0 + rx1) / 2) <= 1) || (x == rx1 && std::abs(z - (rz0 + rz1) / 2) <= 1);
            bool broken = r.next(7) == 0; // fallen stretches of wall
            if (gap || broken) continue;
            f.tile[f.idx(x, z)] = ROCK;
            f.feat[f.idx(x, z)] = F_NONE;
        }
    f.dress.push_back({"fixture", "rubble", (int)r.next(4), rx0 + 2, rz0 + 2, 0, Z_NONE, -1, "surface:ruin", ""});
    f.dress.push_back({"fixture", "pillar", (int)r.next(3), rx1 - 2, rz0 + 2, 0, Z_NONE, -1, "surface:ruin", ""});
    f.dress.push_back({"prop", "ruin_stone", 0, rx0 + 3, rz1 - 2, 0, Z_NONE, -1, "surface:ruin", ""});
    f.dress.push_back({"wall", "plaque", 0, rx0 + 1, rz0 + 1, 2, Z_NONE, -1, "surface:ruin", "writing"});
    int stx = (rx0 + rx1) / 2, stz = (rz0 + rz1) / 2;
    for (int z = stz - 1; z <= stz + 1; ++z) for (int x = stx - 1; x <= stx + 1; ++x) f.feat[f.idx(x, z)] = F_NONE; // roofed stair well
    f.entities.push_back({"stairs_down", 0, stx, stz});
    parts.push_back("ruin");
    // copse: trees and bushes in a disc west of the stream
    int cx = 14, cz = 44 + (int)r.next(6), trees = 0;
    for (int z = cz - 7; z <= cz + 7; ++z)
        for (int x = cx - 7; x <= cx + 7; ++x) {
            if ((x - cx) * (x - cx) + (z - cz) * (z - cz) > 49 || x < 2 || z < 2 || x > S - 3 || z > S - 3) continue;
            uint64_t h = hash_words({seed, 0xC095E, (uint64_t)x, (uint64_t)z}) % 100;
            if (h < 18) { f.dress.push_back({"prop", "tree", (int)(h % 2), x, z, 0, Z_NONE, -1, "surface:copse", ""}); ++trees; }
            else if (h < 26) f.dress.push_back({"prop", h < 22 ? "bush" : "stump", 0, x, z, 0, Z_NONE, -1, "surface:copse", ""});
        }
    if (trees >= 6) parts.push_back("copse");
    // camp: tents, a fire, a crate, east of the stream
    int kx = 52, kz = 48 + (int)r.next(6);
    f.dress.push_back({"prop", "campfire", 0, kx, kz, 0, Z_NONE, -1, "surface:camp", "light"});
    f.dress.push_back({"prop", "tent", 0, kx - 3, kz - 2, 0, Z_NONE, -1, "surface:camp", "den"});
    f.dress.push_back({"prop", "tent", 1, kx + 3, kz - 2, 0, Z_NONE, -1, "surface:camp", "den"});
    f.dress.push_back({"fixture", "crate", (int)r.next(3), kx, kz + 3, 0, Z_NONE, -1, "surface:camp", "container"});
    f.dress.push_back({"fixture", "bench", (int)r.next(3), kx - 2, kz + 1, 1, Z_NONE, -1, "surface:camp", "station"});
    parts.push_back("camp");
    f.hash = floor_hash(f);
    f.dress_hash = dress_hash(f);
}

// The collapsed level: a dead-end pocket of 4 diggable tiles out of the exit room, walled in HARD
// so only the exit room reaches it; the way down sits at its far end. Returns the tip or -1.
int dig_collapse(Floor& f, int node, int LEN, int& tip_x, int& tip_z) {
    const RoomRow& ro = f.rooms[node];
    const int W = f.W;
    std::vector<std::pair<uint64_t, std::pair<int, int>>> cands; // (order, (tile, dir))
    for (int z = ro.z0; z < ro.z1; ++z)
        for (int x = ro.x0; x < ro.x1; ++x) {
            size_t t = f.idx(x, z);
            if (f.tile[t] != FLOOR || f.owner[t] != node) continue;
            for (int k = 0; k < 4; ++k) cands.push_back({hash_words({f.hash, t, (uint64_t)k}), {(int)t, k}});
        }
    std::sort(cands.begin(), cands.end());
    for (auto& c : cands) {
        int t = c.second.first, k = c.second.second, x = t % W, z = t / W;
        bool ok = true;
        std::vector<int> pocket;
        for (int j = 1; j <= LEN && ok; ++j) {
            int px = x + D4[k][0] * j, pz = z + D4[k][1] * j;
            if (px < 2 || pz < 2 || px >= W - 2 || pz >= f.H - 2) { ok = false; break; }
            size_t p = f.idx(px, pz);
            if (f.tile[p] == FLOOR || (f.owner[p] >= 0 && f.owner[p] != node) || feat_blocks(f.feat[p])) { ok = false; break; } // the exit box rim is fine
            // no floor of anything but the exit room next to the pocket
            for (int dz = -1; dz <= 1 && ok; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    size_t n = f.idx(px + dx, pz + dz);
                    if (f.tile[n] == FLOOR && f.owner[n] != node) { ok = false; break; }
                    if (f.tile[n] == FLOOR && j > 1) { ok = false; break; } // only the first tile touches the room
                }
            pocket.push_back((int)p);
        }
        if (!ok) continue;
        for (int p : pocket) {
            f.tile[p] = ROCK;
            int px = p % W, pz = p / W;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    size_t n = f.idx(px + dx, pz + dz);
                    if (f.tile[n] != FLOOR && std::find(pocket.begin(), pocket.end(), (int)n) == pocket.end()) f.tile[n] = HARD;
                }
        }
        tip_x = pocket.back() % W; tip_z = pocket.back() / W;
        return pocket.back();
    }
    return -1;
}

// tiles of the room box that are the room's floor
std::vector<int> room_tiles(const Floor& f, const RoomRow& ro) {
    std::vector<int> o;
    for (int z = ro.z0; z < ro.z1; ++z)
        for (int x = ro.x0; x < ro.x1; ++x) {
            size_t t = f.idx(x, z);
            if (f.tile[t] == FLOOR && f.owner[t] == ro.node) o.push_back((int)t);
        }
    return o;
}

void populate(Level& L, uint64_t seed, const SiteParams& SP) {
    Floor& f = L.r.floor;
    const int W = f.W;
    const Mission& m = f.mission;
    L.rooms = (int)f.rooms.size();
    // DENS: the stronghold (the faction's seat), rooms dressed with a den, then spread out
    size_t want = SP.thin ? 1 : 3 + f.rooms.size() / 12;
    std::vector<int> den_rooms;
    int sh = m.find(Kind::Stronghold);
    if (sh >= 0) den_rooms.push_back(sh);
    for (auto& d : f.dress)
        if (den_rooms.size() < want && d.content == "den" && d.room >= 0 && std::find(den_rooms.begin(), den_rooms.end(), d.room) == den_rooms.end() &&
            m.nodes[d.room].kind == Kind::Room)
            den_rooms.push_back(d.room);
    while (den_rooms.size() < want) { // farthest point from the dens so far, plain rooms only
        int best = -1;
        long long bd = -1;
        for (auto& ro : f.rooms) {
            if (m.nodes[ro.node].kind != Kind::Room || std::find(den_rooms.begin(), den_rooms.end(), ro.node) != den_rooms.end()) continue;
            long long dmin = 1LL << 40;
            for (int dr : den_rooms) {
                long long dx = ro.door_x - f.rooms[dr].door_x, dz = ro.door_z - f.rooms[dr].door_z;
                dmin = std::min(dmin, dx * dx + dz * dz);
            }
            if (den_rooms.empty()) dmin = (long long)(hash_words({seed, (uint64_t)ro.node}) % 1000);
            if (dmin > bd) { bd = dmin; best = ro.node; }
        }
        if (best < 0) break;
        den_rooms.push_back(best);
    }
    for (int dr : den_rooms) L.dens.push_back({dr, f.rooms[dr].door_x, f.rooms[dr].door_z});
    // PATROLS from dens, then ALIVE: rooms with a floor tile within alive_m walking steps of a den or
    // patrol tile. Not thinned: while under 70 percent of rooms are alive (cap 8 patrols, 3 extra
    // dens) the farthest dead room gets a patrol from a den of its lock region (patrols never cross
    // a lock), or a den of its own when its region has none.
    auto patrol = [&](const Den& d, int want_goal) {
        size_t s0 = f.idx(d.x, d.z);
        uint32_t mask = f.behind[s0];
        std::vector<int> prev(f.tile.size(), -2), dist(f.tile.size(), -1);
        std::deque<int> q{(int)s0};
        prev[s0] = -1; dist[s0] = 0;
        while (!q.empty()) {
            int c = q.front(); q.pop_front();
            for (auto& dd : D4) {
                size_t n = f.idx(c % W + dd[0], c / W + dd[1]);
                if (f.tile[n] != FLOOR || prev[n] != -2 || f.behind[n] != mask) continue;
                prev[n] = c; dist[n] = dist[c] + 1; q.push_back((int)n);
            }
        }
        int goal = -1, gd = -1;
        for (auto& ro : f.rooms) {
            size_t t = f.idx(ro.door_x, ro.door_z);
            if (ro.node == d.room || dist[t] < 0) continue;
            if (want_goal >= 0 && ro.node != want_goal) continue;
            bool taken = false;
            for (auto& p : L.patrols) taken |= p.room_b == ro.node && p.room_a == d.room;
            if (!taken && dist[t] > gd) { gd = dist[t]; goal = ro.node; }
        }
        if (goal < 0 || gd < 8) return false;
        Patrol p;
        p.room_a = d.room; p.room_b = goal;
        for (int c = (int)f.idx(f.rooms[goal].door_x, f.rooms[goal].door_z); c >= 0; c = prev[c]) p.tiles.push_back(c);
        std::reverse(p.tiles.begin(), p.tiles.end());
        L.patrols.push_back(std::move(p));
        return true;
    };
    int npat = SP.thin ? 0 : 3;
    for (int k = 0; k < npat && !L.dens.empty(); ++k) patrol(L.dens[k % L.dens.size()], -1);
    auto measure_alive = [&]() {
        std::vector<int> dist(f.tile.size(), -1);
        std::deque<int> q;
        for (auto& d : L.dens) { size_t t = f.idx(d.x, d.z); if (dist[t] < 0) { dist[t] = 0; q.push_back((int)t); } }
        for (auto& p : L.patrols) for (int t : p.tiles) if (dist[t] < 0) { dist[t] = 0; q.push_back(t); }
        while (!q.empty()) {
            int c = q.front(); q.pop_front();
            if (dist[c] >= SP.alive_m) continue;
            for (auto& dd : D4) {
                size_t n = f.idx(c % W + dd[0], c / W + dd[1]);
                if (f.tile[n] != FLOOR || dist[n] >= 0) continue;
                dist[n] = dist[c] + 1; q.push_back((int)n);
            }
        }
        L.room_alive.assign(f.rooms.size(), 0);
        for (size_t i = 0; i < f.rooms.size(); ++i)
            for (int t : room_tiles(f, f.rooms[i])) if (dist[t] >= 0) { L.room_alive[i] = 1; break; }
        L.alive = (int)std::count(L.room_alive.begin(), L.room_alive.end(), 1);
    };
    measure_alive();
    size_t den_cap = L.dens.size() + 3;
    std::set<int> tried;
    while (!SP.thin && L.alive * 10 < L.rooms * 7 && L.patrols.size() < 8) {
        // the dead room farthest from every den
        int dead = -1;
        long long bd = -1;
        for (size_t i = 0; i < f.rooms.size(); ++i) {
            if (L.room_alive[i] || tried.count((int)i)) continue;
            long long dmin = 1LL << 40;
            for (auto& d : L.dens) { long long dx = f.rooms[i].door_x - d.x, dz = f.rooms[i].door_z - d.z; dmin = std::min(dmin, dx * dx + dz * dz); }
            if (dmin > bd) { bd = dmin; dead = (int)i; }
        }
        if (dead < 0) break;
        tried.insert(dead);
        uint32_t mask = f.behind[f.idx(f.rooms[dead].door_x, f.rooms[dead].door_z)];
        bool done = false;
        for (auto& d : L.dens) if (f.behind[f.idx(d.x, d.z)] == mask && patrol(d, dead)) { done = true; break; }
        if (!done && L.dens.size() < den_cap && m.nodes[dead].kind == Kind::Room) { L.dens.push_back({dead, f.rooms[dead].door_x, f.rooms[dead].door_z}); done = true; }
        if (done) measure_alive();
    }
    // LIT: a light-content item in the room, lava beside its floor, or open sky
    L.room_lit.assign(f.rooms.size(), 0);
    for (auto& d : f.dress) if (d.content == "light" && d.room >= 0) L.room_lit[d.room] = 1;
    for (size_t i = 0; i < f.rooms.size(); ++i) {
        if (L.room_lit[i]) continue;
        for (int t : room_tiles(f, f.rooms[i])) {
            if (f.feat[t] == F_SKY) { L.room_lit[i] = 1; break; }
            bool lava = false;
            for (auto& dd : D4) lava |= f.feat[f.idx(t % W + dd[0], t / W + dd[1])] == F_LAVA;
            if (lava) { L.room_lit[i] = 1; break; }
        }
    }
    L.lit = (int)std::count(L.room_lit.begin(), L.room_lit.end(), 1);
    // READABLE (gameplay/20: "writings (books, plaques, graffiti) per floor, at least 12, each a
    // HISTORY row or a Form, reaction, map region or quest hint"). Readable kinds: book, scroll,
    // note, map_scroll (books), plaque, map_plaque (plaques), graffiti. Mosaics, chalk circles and
    // other inscribed trim are decoration, not counted. The text pass gives a text to 12-16 of the
    // candidates, spread one per room before any room gets a second (the map plaque first); the
    // other candidates are worn illegible (content "worn", no text). SP.all_legible = the control.
    static const char* TK[] = {"history", "map_region", "quest_hint", "form"};
    static const std::set<std::string> READABLE = {"book", "scroll", "note", "map_scroll", "plaque", "graffiti"};
    struct Cand { uint64_t order; int dress; int room; };
    std::vector<Cand> cands;
    for (size_t k = 0; k < f.dress.size(); ++k)
        if (f.dress[k].content == "writing" && READABLE.count(f.dress[k].id))
            cands.push_back({hash_words({seed, 0x7E47, (uint64_t)k}), (int)k, f.dress[k].room});
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.order < b.order; });
    size_t text_want = SP.all_legible ? cands.size() + 1 : (size_t)(SP.writings_lo + hash_words({seed, 0x12}) % (SP.writings_hi - SP.writings_lo + 1));
    for (auto& e : f.entities)
        if (e.kind == "map_plaque" && L.writings.size() < text_want) L.writings.push_back({-1, e.x, e.z, "map_plaque", "map_region", hash_words({seed, 0x9A9, (uint64_t)e.x})});
    std::vector<char> chosen(cands.size(), 0);
    for (int round = 0; L.writings.size() < text_want && round < 64; ++round) {
        std::set<int> used_rooms;
        bool any = false;
        for (size_t c = 0; c < cands.size() && L.writings.size() < text_want; ++c) {
            if (chosen[c] || used_rooms.count(cands[c].room)) continue;
            chosen[c] = 1; any = true;
            used_rooms.insert(cands[c].room);
            const DressRow& d = f.dress[cands[c].dress];
            uint64_t h = hash_words({seed, hash_str(d.id), (uint64_t)d.x, (uint64_t)d.z});
            L.writings.push_back({d.room, d.x, d.z, d.id, TK[h % 4], h});
        }
        if (!any) break;
    }
    for (size_t c = 0; c < cands.size(); ++c) if (!chosen[c]) f.dress[cands[c].dress].content = "worn";
    L.readable_candidates = (int)cands.size();

    // POPULATION (D72). The tier draw follows spec 04 section 2's shares (fodder 60, regular 30,
    // elite 8, champion 2 percent). SPAWNED packs go to dens, then patrols, while the floor's spawned
    // budget lasts; a pack's composition is drawn under the D72 caps. Everything else is EMERGENT:
    // groups of 1-3 in rooms, in room order from a seeded shuffle.
    {
        Rng pr(hash_words({seed, 0xD72}));
        auto draw = [&]() { int r = (int)pr.next(100); return r < 60 ? 0 : r < 90 ? 1 : r < 98 ? 2 : 3; };
        int total = (int)f.rooms.size() * SP.creatures_per_room_x10 / 10;
        int budget = total * (SP.break_packs ? 50 : SP.spawned_share_pct) / 100;
        std::vector<std::pair<std::string, int>> sites; // (source, room)
        for (auto& d : L.dens) sites.push_back({"den", d.room});
        for (auto& p : L.patrols) sites.push_back({"patrol", p.room_a});
        for (auto& [src, room] : sites) {
            if (budget < 2) break;
            Group g;
            g.spawned = true; g.source = src; g.room = room;
            if (SP.break_packs) {
                int n = 2 + (int)pr.next(7);
                for (int k = 0; k < n; ++k) g.tiers.push_back((int)pr.next(4));
            } else {
                // one draw decides the pack's kind: an elite-led pack (2-4, 1-2 elites, the rest
                // fodder or regular) 15 percent; a fodder swarm (4-6, fodder, at most one regular)
                // 35 percent; a small mixed pack (2-3, fodder and regular) the rest
                int kind = (int)pr.next(100);
                if (kind < 15) {
                    int n = 2 + (int)pr.next(SP.elite_pack_max - 1);
                    int e = 1 + (int)pr.next(std::min(SP.elites_per_pack_max, n - 1));
                    for (int k = 0; k < e; ++k) g.tiers.push_back(pr.next(10) == 0 ? 3 : 2);
                    while ((int)g.tiers.size() < n) g.tiers.push_back((int)pr.next(2));
                } else if (kind < 50) {
                    int n = 4 + (int)pr.next(3);
                    for (int k = 0; k < n; ++k) g.tiers.push_back(0);
                    if (pr.next(2)) g.tiers.back() = 1;
                } else {
                    int n = 2 + (int)pr.next(2);
                    for (int k = 0; k < n; ++k) g.tiers.push_back((int)pr.next(2));
                }
            }
            if ((int)g.tiers.size() > budget) g.tiers.resize(budget);
            budget -= (int)g.tiers.size();
            L.spawned_creatures += (int)g.tiers.size();
            L.groups.push_back(std::move(g));
        }
        int left = total - L.spawned_creatures;
        std::vector<int> order;
        for (auto& ro : f.rooms) if (m.nodes[ro.node].kind == Kind::Room) order.push_back(ro.node);
        for (size_t k = order.size(); k > 1; --k) std::swap(order[k - 1], order[pr.next(k)]);
        for (size_t k = 0; left > 0 && !order.empty(); ++k) {
            Group g;
            g.room = order[k % order.size()];
            g.source = "room";
            int n = std::min(left, 1 + (int)pr.next(SP.emergent_group_max));
            int elites = 0;
            for (int j = 0; j < n; ++j) {
                int t = draw();
                if (t >= 2 && elites >= SP.elites_per_pack_max && !SP.break_packs) t = 0; // D72: 1-2 elites in any group
                elites += t >= 2;
                g.tiers.push_back(t);
            }
            left -= n;
            L.groups.push_back(std::move(g));
        }
        L.creatures = total;
    }
}
} // namespace

bool build_site(uint64_t seed, const std::vector<Grammar>& g, const std::vector<Program>& progs, const DressLib& lib0,
                const Params& P, const SiteParams& SP, Site& s, std::string& err) {
    s = Site{};
    s.seed = seed;
    DressLib lib = lib0;
    if (SP.thin) lib.wall_scale = 0.0;
    build_surface(seed, s.surface, s.surface_parts);
    // floors: fortress (garrison), temple (cult), crypt or sewer (undead and vermin), mine (goblins);
    // then the cave layer (beasts) with the lava vent
    const char* F3 = hash_words({seed, 0xF3}) % 2 ? "sewer" : "crypt";
    struct Def { const char* name; const char* theme; const char* faction; int grammar; bool vent; };
    const Def defs[5] = {{"floor1", "fortress", "garrison", 0, false}, {"floor2", "temple", "cult", 0, false}, {"floor3", F3, "undead", 0, false},
                         {"floor4", "mine", "goblin", 0, false}, {"caves", "cave", "beasts", 1, true}};
    for (int i = 0; i < 5; ++i) {
        Params Pi = P;
        Pi.theme = defs[i].theme;
        Pi.vent = defs[i].vent;
        Level L;
        L.name = defs[i].name;
        L.faction = defs[i].faction;
        L.r = generate(hash_words({seed, 0x51E, (uint64_t)i}), g[defs[i].grammar], progs, Pi);
        if (!L.r.ok) { err = L.name + ": " + (L.r.log.empty() ? L.r.verdict.why : L.r.log.back()); return false; }
        s.levels.push_back(std::move(L));
    }
    // LINKS: stairs, shaft, flooded sump, collapsed level, stairs
    auto& Fl = s.levels;
    auto ent = [&](Floor& f, const char* k) { EntityRow* e = find_entity(f, k); return e; };
    {
        EntityRow* a = ent(s.surface, "stairs_down"); EntityRow* b = ent(Fl[0].r.floor, "stairs_up");
        if (!a || !b) { err = "stairs link endpoints missing"; return false; }
        s.links.push_back({"stairs", 0, 1, a->x, a->z, b->x, b->z, 0});
    }
    {
        EntityRow* a = ent(Fl[0].r.floor, "stairs_down"); EntityRow* b = ent(Fl[1].r.floor, "stairs_up");
        if (!a || !b) { err = "shaft link endpoints missing"; return false; }
        a->kind = "shaft_down"; b->kind = "shaft_landing";
        s.links.push_back({"shaft", 1, 2, a->x, a->z, b->x, b->z, 0});
    }
    {
        Floor &fa = Fl[1].r.floor, &fb = Fl[2].r.floor;
        EntityRow* a = ent(fa, "stairs_down"); EntityRow* b = ent(fb, "stairs_up");
        if (!a || !b) { err = "sump link endpoints missing"; return false; }
        a->kind = "sump_down"; b->kind = "sump_up";
        // the sump floods the floor round both ends (shallow water, walkable)
        auto flood = [](Floor& f, int x, int z) {
            for (int dz = -2; dz <= 2; ++dz)
                for (int dx = -2; dx <= 2; ++dx) {
                    size_t t = f.idx(x + dx, z + dz);
                    if (f.tile[t] == FLOOR && !feat_blocks(f.feat[t])) f.feat[t] = F_WATER;
                }
        };
        flood(fa, a->x, a->z);
        flood(fb, b->x, b->z);
        s.links.push_back({"sump", 2, 3, a->x, a->z, b->x, b->z, 0});
    }
    {
        Floor &fa = Fl[2].r.floor, &fb = Fl[3].r.floor;
        int ex = fa.mission.find(Kind::Exit);
        int tx = 0, tz = 0, from_room = -1;
        // from the exit room, else from another room of the exit's lock region (the deepest: the
        // pocket's HARD shell keeps every lock's dig gate)
        std::vector<int> starts;
        if (ex >= 0) {
            starts.push_back(ex);
            uint32_t mask = fa.behind[fa.idx(fa.rooms[ex].door_x, fa.rooms[ex].door_z)];
            for (auto& ro : fa.rooms)
                if (ro.node != ex && fa.behind[fa.idx(ro.door_x, ro.door_z)] == mask && fa.mission.nodes[ro.node].kind != Kind::Lock) starts.push_back(ro.node);
        }
        int dig_len = 0;
        for (int len = 4; len >= 2 && from_room < 0; --len) // the longest pocket that fits, at least 2 tiles of rock
            for (int st : starts) if (dig_collapse(fa, st, len, tx, tz) >= 0) { from_room = st; dig_len = len; break; }
        if (from_room < 0) { err = "floor3: no room for the collapsed passage (" + std::to_string(starts.size()) + " start rooms, exit box " + (ex >= 0 ? std::to_string(fa.rooms[ex].x0) + "," + std::to_string(fa.rooms[ex].z0) + "-" + std::to_string(fa.rooms[ex].x1) + "," + std::to_string(fa.rooms[ex].z1) : std::string("-")) + ")"; return false; }
        auto& es = fa.entities;
        es.erase(std::remove_if(es.begin(), es.end(), [](const EntityRow& e) { return e.kind == "stairs_down"; }), es.end());
        es.push_back({"collapse_down", 0, tx, tz});
        EntityRow* b = ent(fb, "stairs_up");
        if (!b) { err = "collapse link endpoint missing"; return false; }
        b->kind = "collapse_landing";
        s.links.push_back({"collapse", 3, 4, tx, tz, b->x, b->z, dig_len, from_room});
        fa.hash = floor_hash(fa);
        Fl[2].r.verdict = critic(fa); // the pocket changed tiles: the hard gates are checked again
    }
    {
        EntityRow* a = ent(Fl[3].r.floor, "stairs_down"); EntityRow* b = ent(Fl[4].r.floor, "stairs_up");
        if (!a || !b) { err = "cave stairs endpoints missing"; return false; }
        s.links.push_back({"stairs", 4, 5, a->x, a->z, b->x, b->z, 0});
    }
    // DRESS and POPULATION per level
    for (size_t i = 0; i < Fl.size(); ++i) {
        Floor& f = Fl[i].r.floor;
        f.hash = floor_hash(f);
        dress_floor(f, progs, lib, hash_words({seed, 0xD7E5, (uint64_t)i}));
        populate(Fl[i], hash_words({seed, 0x909, (uint64_t)i}), SP);
    }
    // the site hash: every level's floor and dress hash, the links, dens, patrols and writings
    uint64_t h = hash_words({seed, s.surface.hash, s.surface.dress_hash});
    for (auto& L : Fl) {
        h = hash_words({h, L.r.floor.hash, L.r.floor.dress_hash, (uint64_t)L.dens.size(), (uint64_t)L.writings.size()});
        for (auto& p : L.patrols) h = hash_words({h, (uint64_t)p.room_a, (uint64_t)p.room_b, (uint64_t)p.tiles.size()});
    }
    for (auto& l : s.links) h = hash_words({h, hash_str(l.kind), (uint64_t)l.fx, (uint64_t)l.fz, (uint64_t)l.tx, (uint64_t)l.tz});
    s.hash = h;
    return true;
}

SiteVerdict site_critic(const Site& s, const SiteParams& SP) {
    SiteVerdict v;
    auto fail = [&](const std::string& m) { v.fails.push_back(m); };
    // surface: four parts, one walkable piece, the way down on it
    for (const char* p : {"ruin", "stream", "copse", "camp"})
        if (std::find(s.surface_parts.begin(), s.surface_parts.end(), p) == s.surface_parts.end()) fail(std::string("surface: no ") + p);
    {
        const Floor& f = s.surface;
        int comps = 0;
        std::vector<char> seen(f.tile.size(), 0);
        for (size_t t = 0; t < f.tile.size(); ++t) {
            if (f.tile[t] != FLOOR || seen[t]) continue;
            ++comps;
            std::deque<size_t> q{t};
            seen[t] = 1;
            while (!q.empty()) {
                size_t c = q.front(); q.pop_front();
                for (auto& d : D4) {
                    size_t n = f.idx((int)(c % f.W) + d[0], (int)(c / f.W) + d[1]);
                    if (f.tile[n] == FLOOR && !seen[n]) { seen[n] = 1; q.push_back(n); }
                }
            }
        }
        if (comps != 1) fail("surface: " + std::to_string(comps) + " walkable pieces");
    }
    if (s.levels.size() != 5) { fail("levels: " + std::to_string(s.levels.size())); return v; }
    std::set<std::string> factions;
    for (int i = 0; i < 4; ++i) {
        const Level& L = s.levels[i];
        const Floor& f = L.r.floor;
        const Mission& m = f.mission;
        const std::string n = L.name + ": ";
        if (!L.r.verdict.pass()) fail(n + "hard gate " + L.r.verdict.why);
        int keys = 0, locks = 0, secrets = 0, bosses = 0;
        for (auto& nd : m.nodes) {
            keys += nd.kind == Kind::Key; locks += nd.kind == Kind::Lock; secrets += nd.kind == Kind::Secret; bosses += nd.kind == Kind::Boss;
        }
        if (f.rooms.size() < 24 || f.rooms.size() > 40) fail(n + std::to_string(f.rooms.size()) + " rooms");
        if (keys != 3 || locks != 3) fail(n + "keys/locks " + std::to_string(keys) + "/" + std::to_string(locks));
        if (secrets != 2) fail(n + std::to_string(secrets) + " secrets");
        if (bosses != 2) fail(n + std::to_string(bosses) + " bosses");
        if (m.cycles() < 2 || f.loops < 2) fail(n + "cycles " + std::to_string(m.cycles()) + " loops " + std::to_string(f.loops));
        if (f.special_kinds != 31u || f.special_missed) fail(n + "special rooms incomplete");
        factions.insert(L.faction);
        if (L.dens.size() < 2) fail(n + std::to_string(L.dens.size()) + " dens");
        if (L.patrols.size() < 2) fail(n + std::to_string(L.patrols.size()) + " patrols");
        if (L.alive * 10 < L.rooms * 6) fail(n + "alive " + std::to_string(L.alive) + "/" + std::to_string(L.rooms));
        if (L.lit * 10 < L.rooms * 8) fail(n + "lit " + std::to_string(L.lit) + "/" + std::to_string(L.rooms));
        if ((int)L.writings.size() < SP.gate_writings_lo || (int)L.writings.size() > SP.gate_writings_hi)
            fail(n + std::to_string(L.writings.size()) + " writings (band " + std::to_string(SP.gate_writings_lo) + "-" + std::to_string(SP.gate_writings_hi) + ")");
        for (auto& w : L.writings) if (w.kind.empty()) { fail(n + "a writing without a text kind"); break; }
    }
    if (factions.size() != 4) fail("factions on the floors: " + std::to_string(factions.size()));
    // D72 pack rules on every level (floors and caves)
    for (const Level& L : s.levels) {
        const std::string n = L.name + ": ";
        int counted = 0;
        for (const Group& g : L.groups) {
            int elites = 0, fodder = 0, size = (int)g.tiers.size();
            for (int t : g.tiers) { elites += t >= 2; fodder += t == 0; }
            counted += size;
            if (elites > 0 && size > SP.elite_pack_max) { fail(n + "elite pack of " + std::to_string(size)); break; }
            if (elites > SP.elites_per_pack_max) { fail(n + std::to_string(elites) + " elites in one pack"); break; }
            if (size >= SP.large_pack && (elites > 0 || fodder * 4 < size * 3)) { fail(n + "large pack not mostly fodder (" + std::to_string(fodder) + "/" + std::to_string(size) + ")"); break; }
            if (!g.spawned && size > SP.emergent_group_max) { fail(n + "emergent group of " + std::to_string(size)); break; }
        }
        if (L.creatures && L.spawned_creatures * 100 > L.creatures * SP.spawned_share_pct) fail(n + "spawned share " + std::to_string(L.spawned_creatures) + "/" + std::to_string(L.creatures));
        if (counted != L.creatures) fail(n + "creature count " + std::to_string(counted) + " != " + std::to_string(L.creatures));
    }
    {
        const Level& C = s.levels[4];
        if (!C.r.verdict.pass()) fail("caves: hard gate " + C.r.verdict.why);
        if (C.r.floor.vent_rooms != 1) fail("caves: no lava vent room");
        bool lava = false;
        for (uint8_t ft : C.r.floor.feat) lava |= ft == F_LAVA;
        if (!lava) fail("caves: no lava");
    }
    // links: all four kinds, in order down the site, and the collapse is reached only by digging
    std::set<std::string> kinds;
    for (size_t i = 0; i < s.links.size(); ++i) {
        kinds.insert(s.links[i].kind);
        if (s.links[i].from != (int)i || s.links[i].to != (int)i + 1) fail("link " + std::to_string(i) + " out of order");
    }
    for (const char* k : {"stairs", "shaft", "sump", "collapse"}) if (!kinds.count(k)) fail(std::string("no ") + k + " link");
    for (auto& l : s.links) {
        if (l.kind != "collapse") continue;
        const Floor& f = s.levels[2].r.floor;
        size_t tip = f.idx(l.fx, l.fz);
        if (f.tile[tip] != ROCK) fail("collapse: the way down is not behind diggable rock");
        // dig from the exit room through ROCK only: must reach the tip; walking cannot
        int ex = l.room;
        if (ex < 0) { fail("collapse: no start room"); continue; }
        std::vector<char> seen(f.tile.size(), 0);
        std::deque<size_t> q{f.idx(f.rooms[ex].door_x, f.rooms[ex].door_z)};
        seen[q.front()] = 1;
        while (!q.empty()) {
            size_t c = q.front(); q.pop_front();
            for (auto& d : D4) {
                size_t n = f.idx((int)(c % f.W) + d[0], (int)(c / f.W) + d[1]);
                if (seen[n] || f.tile[n] == HARD) continue;
                if (f.tile[n] == ROCK && f.owner[c] < 0 && f.tile[c] == FLOOR) continue; // dig only out of the exit room or the pocket
                if (f.tile[n] == FLOOR && f.owner[n] != ex) continue;
                seen[n] = 1; q.push_back(n);
            }
        }
        if (!seen[tip]) fail("collapse: the way down cannot be dug to from its room");
    }
    return v;
}

} // namespace apx
