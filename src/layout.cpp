// LAYOUT + CARVE on the tile grid (02 sections 3-5; port of bl-world layout_attempt): BSP cells,
// lock-region bands, BFS embedding of the mission, a room program per node chosen by its fits,
// A* corridors 1-3 tiles wide by route type under the region rules (no corridor bypasses a lock),
// loop quota, BFS connectivity repair, program islands joined, far-side walls made undiggable.
#include "apxgen/apxgen.hpp"
#include <algorithm>
#include <array>
#include <deque>
#include <queue>
#include <tuple>

namespace apx {

namespace {
using Rect = std::array<int, 4>; // x0 z0 x1 z1, half-open

void bsp(Rng& r, Rect c, const Params& P, std::vector<Rect>& out) {
    int w = c[2] - c[0], d = c[3] - c[1];
    if (w <= P.leaf_max && d <= P.leaf_max) { out.push_back(c); return; }
    if (w >= d) {
        int cut = c[0] + P.leaf_min + (int)r.next((uint64_t)(w - 2 * P.leaf_min + 1));
        bsp(r, {c[0], c[1], cut, c[3]}, P, out);
        bsp(r, {cut, c[1], c[2], c[3]}, P, out);
    } else {
        int cut = c[1] + P.leaf_min + (int)r.next((uint64_t)(d - 2 * P.leaf_min + 1));
        bsp(r, {c[0], c[1], c[2], cut}, P, out);
        bsp(r, {c[0], cut, c[2], c[3]}, P, out);
    }
}

constexpr uint8_t FREE = 255, ROOMBOX = 254;

struct Digger {
    int S;
    const Mission& m;
    const std::vector<uint32_t>& reg;
    const std::vector<uint32_t>& behind;
    const std::vector<RoomRow>& rooms;
    std::vector<uint32_t> tile_band;
    std::vector<int16_t> room_at;   // room whose box plus 1-tile margin holds the tile
    std::vector<uint8_t> occ;       // FREE, ROOMBOX, else corridor tag (lock region)
    std::vector<uint32_t> cmask;    // corridor behind mask per tile
    std::vector<int> carve_in_box;  // footprint tiles inside room boxes (door openings)
    int corridor_tiles = 0, narrowed = 0;
    int width_hist[4] = {0, 0, 0, 0};
    std::vector<uint32_t> g;
    std::vector<int> from;

    size_t id(int x, int z) const { return (size_t)z * S + x; }

    bool cell_ok(int x, int z, int a, int b, uint32_t tag, int pass) const {
        if (x < 1 || z < 1 || x >= S - 1 || z >= S - 1) return false;
        uint8_t o = occ[id(x, z)];
        uint32_t band = tile_band[id(x, z)];
        bool band_ok = pass == 2 || band == tag || (pass == 1 && (band == reg[a] || band == reg[b]));
        bool touch = false;
        if (o != ROOMBOX)
            for (int dz = -1; dz <= 1 && !touch; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = x + dx, nz = z + dz;
                    if (nx < 0 || nz < 0 || nx >= S || nz >= S) continue;
                    uint8_t on = occ[id(nx, nz)];
                    if (on != FREE && on != ROOMBOX && on != tag) { touch = true; break; }
                }
        bool occ_ok = o == FREE || o == ROOMBOX || o == tag;
        int k = room_at[id(x, z)];
        if (k >= 0 && k != a && k != b && (reg[k] != tag || m.nodes[k].kind == Kind::Lock)) return false;
        if (k >= 0 && (k == a || k == b)) return !touch && occ_ok;
        return band_ok && !touch && occ_ok;
    }

    bool foot_ok(int x, int z, int w, int a, int b, uint32_t tag, int pass) const {
        int lo = -(w - 1) / 2, hi = w / 2;
        for (int dz = lo; dz <= hi; ++dz)
            for (int dx = lo; dx <= hi; ++dx)
                if (!cell_ok(x + dx, z + dz, a, b, tag, pass)) return false;
        return true;
    }

    bool astar(int a, int b, int w, uint32_t tag, int pass, std::vector<int>& path) {
        auto& ra = rooms[a];
        auto& rb = rooms[b];
        int sx = ra.door_x, sz = ra.door_z, tx = rb.door_x, tz = rb.door_z;
        std::fill(g.begin(), g.end(), UINT32_MAX);
        std::fill(from.begin(), from.end(), -1);
        using Q = std::tuple<uint32_t, uint32_t, int>;
        std::priority_queue<Q, std::vector<Q>, std::greater<Q>> open;
        auto h = [&](int x, int z) { return (uint32_t)(std::abs(x - tx) + std::abs(z - tz)); };
        g[id(sx, sz)] = 0;
        open.push({h(sx, sz), 0, (int)id(sx, sz)});
        int goal = (int)id(tx, tz);
        while (!open.empty()) {
            auto [f, gc, c] = open.top();
            open.pop();
            if (c == goal) {
                path.clear();
                for (int k = c; k >= 0; k = from[k]) path.push_back(k);
                return true;
            }
            if (gc > g[c]) continue;
            int x = c % S, z = c / S;
            const int D[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (auto& dd : D) {
                int nx = x + dd[0], nz = z + dd[1];
                if (nx < 1 || nz < 1 || nx >= S - 1 || nz >= S - 1) continue;
                int n = (int)id(nx, nz);
                if (n != goal && !foot_ok(nx, nz, w, a, b, tag, pass)) continue;
                uint32_t ng = gc + 1;
                if (ng < g[n]) { g[n] = ng; from[n] = c; open.push({ng + h(nx, nz), ng, n}); }
            }
        }
        return false;
    }

    // Requested width first, narrower only when the wide route has no path (counted: narrowed).
    bool dig(int a, int b, uint32_t tag, int want) {
        std::vector<int> path;
        for (int w = want; w >= 1; --w)
            for (int pass = 0; pass < 3; ++pass) {
                if (!astar(a, b, w, tag, pass, path)) continue;
                uint32_t mk = behind[a] & behind[b];
                int lo = -(w - 1) / 2, hi = w / 2;
                for (int c : path) {
                    int x = c % S, z = c / S;
                    for (int dz = lo; dz <= hi; ++dz)
                        for (int dx = lo; dx <= hi; ++dx) {
                            int px = x + dx, pz = z + dz;
                            if (px < 1 || pz < 1 || px >= S - 1 || pz >= S - 1) continue;
                            size_t t = id(px, pz);
                            if (occ[t] == FREE) { occ[t] = (uint8_t)tag; cmask[t] = mk; ++corridor_tiles; }
                            else if (occ[t] == ROOMBOX) carve_in_box.push_back((int)t);
                        }
                }
                if (w < want) ++narrowed;
                ++width_hist[w];
                return true;
            }
        return false;
    }
};
} // namespace

std::string build_floor(uint64_t seed, int attempt, const Grammar& gram, const std::vector<Program>& progs, const Params& P, Floor& f) {
    f = Floor{};
    const int S = std::min(160, std::max(48, P.size));
    std::string err;
    if (!mission_from_grammar(gram, seed, f.mission, err)) return err;
    const Mission& m = f.mission;
    const int N = (int)m.nodes.size();
    f.reg = regions(m);
    std::vector<uint32_t> behind = behind_masks(m);
    f.attempt = attempt;
    f.W = f.H = S;
    Rng r(hash_words({seed, 0x1A70, (uint64_t)attempt}));

    std::vector<Rect> cells;
    bsp(r, {1, 1, S - 1, S - 1}, P, cells);
    f.cells = (int)cells.size();
    if ((int)cells.size() < N) return std::to_string(cells.size()) + " BSP cells for " + std::to_string(N) + " nodes";

    // bands: cells by centre (x, then z) split into contiguous runs, one per lock region
    auto centre = [&](int c) { return std::pair<int, int>{(cells[c][0] + cells[c][2]) / 2, (cells[c][1] + cells[c][3]) / 2}; };
    int nreg = (int)*std::max_element(f.reg.begin(), f.reg.end()) + 1;
    std::vector<int> order(cells.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return std::make_tuple(centre(a).first, centre(a).second, a) < std::make_tuple(centre(b).first, centre(b).second, b); });
    std::vector<int> counts(nreg, 0);
    for (auto x : f.reg) counts[x]++;
    int spare = (int)cells.size() - N;
    std::vector<uint32_t> band_of_cell(cells.size(), 0);
    size_t pos = 0;
    for (int k = 0; k < nreg; ++k) {
        size_t take = counts[k] + (size_t)spare * counts[k] / N + (k + 1 == nreg ? cells.size() : 0);
        for (size_t j = pos; j < std::min(order.size(), pos + take); ++j) band_of_cell[order[j]] = k;
        pos += take;
    }
    // embed: BFS from the entrance; each node takes the free cell of its band nearest its parent
    std::vector<int> cell_of(N, -1);
    std::vector<char> used(cells.size(), 0), seen(N, 0);
    cell_of[0] = order[0]; used[order[0]] = 1; seen[0] = 1;
    std::deque<int> q{0};
    while (!q.empty()) {
        int v = q.front(); q.pop_front();
        auto pc = centre(cell_of[v]);
        for (int n : m.neighbours(v)) {
            if (seen[n]) continue;
            seen[n] = 1;
            int best = -1;
            long long bd = 0;
            for (int c = 0; c < (int)cells.size(); ++c) {
                if (used[c] || band_of_cell[c] != f.reg[n]) continue;
                auto cc = centre(c);
                long long dd = (long long)(cc.first - pc.first) * (cc.first - pc.first) + (long long)(cc.second - pc.second) * (cc.second - pc.second);
                if (best < 0 || dd < bd) { best = c; bd = dd; }
            }
            if (best < 0) return "out of cells in band " + std::to_string(f.reg[n]);
            cell_of[n] = best; used[best] = 1;
            q.push_back(n);
        }
    }
    for (int v = 0; v < N; ++v) if (cell_of[v] < 0) return "mission node " + m.nodes[v].name + " not connected";

    // rooms: a program whose fits accepts the cell (bosses take the largest), sized inside the cell
    std::vector<std::vector<uint8_t>> room_floor(N);
    for (int node = 0; node < N; ++node) {
        Rect cell = cells[cell_of[node]];
        int cw = cell[2] - cell[0] - 4, cd = cell[3] - cell[1] - 4;
        std::vector<const Program*> fits;
        for (auto& p : progs) if (p.min[0] <= cw && p.min[2] <= cd) fits.push_back(&p);
        if (fits.empty()) return "no program fits cell " + std::to_string(cw) + "x" + std::to_string(cd);
        const Program* p = fits[0];
        if (m.nodes[node].kind == Kind::Boss) {
            for (auto* q2 : fits) if (q2->max[0] * q2->max[2] > p->max[0] * p->max[2]) p = q2;
        } else p = fits[r.next(fits.size())];
        int w = std::max(p->min[0], std::min(cw, p->max[0]));
        int d = std::max(p->min[2], std::min(cd, p->max[2]));
        int h = std::max(p->min[1], std::min(p->max[1], 6));
        int x0 = cell[0] + 2 + (cw - w) / 2, z0 = cell[1] + 2 + (cd - d) / 2;
        RoomRow row{node, p->id, x0, z0, x0 + w, z0 + d, 0, 0};
        std::string e = run_program(*p, w, h, d, hash_words({seed, 0xC4A7, (uint64_t)node}), room_floor[node]);
        if (!e.empty()) {
            // a program that cannot run at this size: a plain box, counted and named in the log
            ++f.program_fallbacks;
            row.program = p->id + "!fallback";
            room_floor[node].assign((size_t)w * d, 0);
            for (int z = 1; z < d - 1; ++z) for (int x = 1; x < w - 1; ++x) room_floor[node][(size_t)z * w + x] = 1;
        }
        // door: the floor tile nearest the box centre
        long long bd = -1;
        for (int z = 0; z < d; ++z)
            for (int x = 0; x < w; ++x) {
                if (!room_floor[node][(size_t)z * w + x]) continue;
                long long dd = (long long)(2 * x - w) * (2 * x - w) + (long long)(2 * z - d) * (2 * z - d);
                if (bd < 0 || dd < bd) { bd = dd; row.door_x = x0 + x; row.door_z = z0 + z; }
            }
        f.rooms.push_back(row);
    }

    Digger D{S, m, f.reg, behind, f.rooms};
    D.tile_band.assign((size_t)S * S, 0);
    D.room_at.assign((size_t)S * S, -1);
    D.occ.assign((size_t)S * S, FREE);
    D.cmask.assign((size_t)S * S, 0);
    D.g.assign((size_t)S * S, 0);
    D.from.assign((size_t)S * S, -1);
    for (size_t c = 0; c < cells.size(); ++c)
        for (int z = cells[c][1]; z < cells[c][3]; ++z)
            for (int x = cells[c][0]; x < cells[c][2]; ++x) D.tile_band[D.id(x, z)] = band_of_cell[c];
    for (auto& ro : f.rooms) {
        for (int z = ro.z0 - 1; z < ro.z1 + 1; ++z)
            for (int x = ro.x0 - 1; x < ro.x1 + 1; ++x) D.room_at[D.id(x, z)] = (int16_t)ro.node;
        for (int z = ro.z0; z < ro.z1; ++z)
            for (int x = ro.x0; x < ro.x1; ++x) D.occ[D.id(x, z)] = ROOMBOX;
    }

    // route type -> width: the critical path entrance..exit 3, leaves 1, the rest 2
    std::vector<int> par(N, -1), deg(N, 0);
    std::vector<char> on_path(N, 0);
    for (auto& e : m.edges) { deg[e.first]++; deg[e.second]++; }
    {
        std::vector<char> s2(N, 0);
        std::deque<int> qq{0};
        s2[0] = 1;
        while (!qq.empty()) { int v = qq.front(); qq.pop_front(); for (int n : m.neighbours(v)) if (!s2[n]) { s2[n] = 1; par[n] = v; qq.push_back(n); } }
        int ex = m.find(Kind::Exit);
        for (int v = ex; v >= 0; v = par[v]) on_path[v] = 1;
    }
    for (auto& e : m.edges) {
        int a = e.first, b = e.second;
        int w = (on_path[a] && on_path[b] && (par[a] == b || par[b] == a)) ? 3 : (deg[a] == 1 || deg[b] == 1) ? 1 : 2;
        if (!D.dig(a, b, std::min(f.reg[a], f.reg[b]), w)) return "no corridor for edge " + m.nodes[a].name + "-" + m.nodes[b].name;
    }
    // loop quota: the closest same-region pairs that are not graph neighbours (no lock rooms)
    std::vector<std::tuple<long long, int, int>> pairs;
    for (int a = 0; a < N; ++a)
        for (int b = a + 1; b < N; ++b) {
            if (f.reg[a] != f.reg[b] || m.nodes[a].kind == Kind::Lock || m.nodes[b].kind == Kind::Lock) continue;
            if (std::find(m.edges.begin(), m.edges.end(), std::pair<int, int>{a, b}) != m.edges.end()) continue;
            long long dx = f.rooms[a].door_x - f.rooms[b].door_x, dz = f.rooms[a].door_z - f.rooms[b].door_z;
            pairs.push_back({dx * dx + dz * dz, a, b});
        }
    std::sort(pairs.begin(), pairs.end());
    for (auto& [dd, a, b] : pairs) {
        if (f.loops >= P.loop_quota) break;
        if (D.dig(a, b, f.reg[a], 2)) ++f.loops;
    }
    // BFS connectivity over boxes and corridors; repair by joining the two largest components
    auto comps = [&]() {
        std::vector<int> lab((size_t)S * S, -1);
        std::vector<std::vector<int>> out;
        for (int s = 0; s < S * S; ++s) {
            if (D.occ[s] == FREE || lab[s] >= 0) continue;
            std::vector<int> cc{s};
            lab[s] = (int)out.size();
            for (size_t k = 0; k < cc.size(); ++k) {
                int x = cc[k] % S, z = cc[k] / S;
                const int DD[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (auto& d2 : DD) {
                    int nx = x + d2[0], nz = z + d2[1];
                    if (nx < 0 || nz < 0 || nx >= S || nz >= S) continue;
                    int n = nz * S + nx;
                    if (D.occ[n] != FREE && lab[n] < 0) { lab[n] = lab[s]; cc.push_back(n); }
                }
            }
            out.push_back(std::move(cc));
        }
        return std::make_pair(out, lab);
    };
    f.components_before_repair = (int)comps().first.size();
    for (;;) {
        auto [cs, lab] = comps();
        if (cs.size() <= 1) break;
        std::vector<int> ord(cs.size());
        for (size_t i = 0; i < ord.size(); ++i) ord[i] = (int)i;
        std::stable_sort(ord.begin(), ord.end(), [&](int a, int b) { return cs[a].size() > cs[b].size(); });
        int ra = -1, rb = -1;
        for (auto& ro : f.rooms) {
            int l = lab[D.id(ro.x0, ro.z0)];
            if (l == ord[0] && ra < 0) ra = ro.node;
            if (l == ord[1] && rb < 0) rb = ro.node;
        }
        if (ra < 0 || rb < 0) return "component without room";
        if (!D.dig(ra, rb, std::min(f.reg[ra], f.reg[rb]), 1)) return "repair corridor failed";
        if (++f.repairs > 16) return "repair did not converge";
    }

    // CARVE: rock, room floors from their programs, corridors, door openings
    f.tile.assign((size_t)S * S, ROCK);
    f.owner.assign((size_t)S * S, -1);
    f.behind.assign((size_t)S * S, 0);
    for (auto& ro : f.rooms) {
        int w = ro.x1 - ro.x0;
        for (int z = ro.z0; z < ro.z1; ++z)
            for (int x = ro.x0; x < ro.x1; ++x) {
                size_t t = f.idx(x, z);
                f.owner[t] = (int16_t)ro.node;
                f.behind[t] = behind[ro.node];
                if (room_floor[ro.node][(size_t)(z - ro.z0) * w + (x - ro.x0)]) f.tile[t] = FLOOR;
            }
    }
    for (int t = 0; t < S * S; ++t)
        if (D.occ[t] != FREE && D.occ[t] != ROOMBOX) { f.tile[t] = FLOOR; f.behind[t] = D.cmask[t]; }
    for (int t : D.carve_in_box) f.tile[t] = FLOOR;
    // program islands: a room's floor pieces not joined to its door piece get a path inside the box
    for (auto& ro : f.rooms) {
        for (int guard = 0; guard < 64; ++guard) {
            std::vector<int> lab((size_t)S * S, -1);
            std::deque<int> qq{(int)f.idx(ro.door_x, ro.door_z)};
            lab[qq[0]] = 0;
            auto inside = [&](int x, int z) { return x > ro.x0 && z > ro.z0 && x < ro.x1 - 1 && z < ro.z1 - 1; };
            while (!qq.empty()) {
                int c = qq.front(); qq.pop_front();
                int x = c % S, z = c / S;
                const int DD[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (auto& d2 : DD) {
                    int nx = x + d2[0], nz = z + d2[1];
                    if (nx < ro.x0 || nz < ro.z0 || nx >= ro.x1 || nz >= ro.z1) continue;
                    int n = nz * S + nx;
                    if (f.tile[n] == FLOOR && lab[n] < 0) { lab[n] = 0; qq.push_back(n); }
                }
            }
            int island = -1;
            for (int z = ro.z0; z < ro.z1 && island < 0; ++z)
                for (int x = ro.x0; x < ro.x1; ++x) if (f.tile[f.idx(x, z)] == FLOOR && lab[f.idx(x, z)] < 0) { island = (int)f.idx(x, z); break; }
            if (island < 0) break;
            // BFS through the box interior from the island to the door piece, carve the path
            std::vector<int> prev((size_t)S * S, -2);
            std::deque<int> q2{island};
            prev[island] = -1;
            int hit = -1;
            while (!q2.empty() && hit < 0) {
                int c = q2.front(); q2.pop_front();
                int x = c % S, z = c / S;
                const int DD[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (auto& d2 : DD) {
                    int nx = x + d2[0], nz = z + d2[1];
                    if (!inside(nx, nz)) continue;
                    int n = nz * S + nx;
                    if (prev[n] != -2) continue;
                    prev[n] = c;
                    if (lab[n] == 0) { hit = n; break; }
                    q2.push_back(n);
                }
            }
            if (hit < 0) return "room " + std::to_string(ro.node) + " island cannot join its door";
            for (int c = hit; c >= 0; c = prev[c]) f.tile[c] = FLOOR;
            ++f.island_repairs;
        }
    }
    // 02 section 8: walls of a lock's far side are undiggable; the map border is undiggable
    for (int z = 0; z < S; ++z)
        for (int x = 0; x < S; ++x) {
            size_t t = f.idx(x, z);
            if (x == 0 || z == 0 || x == S - 1 || z == S - 1) { f.tile[t] = HARD; continue; }
            if (!P.hard_walls || f.tile[t] != ROCK) continue;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    size_t n = f.idx(x + dx, z + dz);
                    if (f.tile[n] == FLOOR && f.behind[n]) f.tile[t] = HARD;
                }
        }

    f.corridor_tiles = D.corridor_tiles;
    f.narrowed = D.narrowed;
    for (int i = 0; i < 4; ++i) f.width_hist[i] = D.width_hist[i];
    static const char* ENT[] = {"stairs_up", "", "key", "lock_door", "boss", "chest", "secret", "merchant", "map_plaque", "faction_seat", "stairs_down"};
    for (auto& ro : f.rooms) {
        const Node& n = m.nodes[ro.node];
        if (n.kind == Kind::Room) continue;
        f.entities.push_back({ENT[(int)n.kind], n.id, ro.door_x, ro.door_z});
    }
    f.hash = floor_hash(f);
    return "";
}

uint64_t floor_hash(const Floor& f) {
    uint64_t h = 1469598103934665603ull;
    auto byte = [&](uint8_t b) { h ^= b; h *= 1099511628211ull; };
    auto word = [&](uint64_t v) { for (int i = 0; i < 8; ++i) byte((uint8_t)(v >> (8 * i))); };
    auto str = [&](const std::string& s) { word(s.size()); for (char c : s) byte((uint8_t)c); };
    word((uint64_t)f.W); word((uint64_t)f.H);
    for (uint8_t t : f.tile) byte(t);
    auto ents = f.entities;
    std::sort(ents.begin(), ents.end(), [](const EntityRow& a, const EntityRow& b) { return std::tie(a.kind, a.id, a.x, a.z) < std::tie(b.kind, b.id, b.x, b.z); });
    for (auto& e : ents) { str(e.kind); word((uint64_t)e.id); word((uint64_t)e.x); word((uint64_t)e.z); }
    for (auto& r : f.rooms) { word((uint64_t)r.node); str(r.program); word((uint64_t)r.x0); word((uint64_t)r.z0); word((uint64_t)r.x1); word((uint64_t)r.z1); }
    return h;
}

} // namespace apx
