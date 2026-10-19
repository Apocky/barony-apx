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
#include <cstdio>
#include <cstdlib>

namespace apx {

const char* const THEMES[6] = {"crypt", "mine", "sewer", "temple", "fortress", "cave"};

bool load_districts(const std::string& path, Params& P, std::string& err) {
    Val* doc = nullptr;
    if (!parse_toml_file(path, doc, err)) return false;
    for (auto* t : toml_arr(doc, "theme")) {
        std::string id = toml_str(t, "id");
        auto ds = toml_strs(t, "districts");
        if (id.empty() || ds.empty()) { err = path + ": [[theme]] needs id and districts"; return false; }
        P.districts.push_back({id, ds});
    }
    return true;
}

static const char* special_of(Kind k) {
    switch (k) {
        case Kind::Haven: return "haven";
        case Kind::Boss: return "boss";
        case Kind::Treasure: return "vault";
        case Kind::Nexus: return "nexus";
        case Kind::Stronghold: return "stronghold";
        default: return "";
    }
}

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
    std::vector<uint8_t> featg;     // room programs' features on the grid: crossing a blocker costs feat_cost
    uint32_t feat_cost = 24;
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
                uint32_t pen = 0; // go round lava, chasms, deep water where a way exists: every footprint tile counts
                if (feat_cost) {
                    int lo = -(w - 1) / 2, hi = w / 2;
                    for (int dz = lo; dz <= hi; ++dz)
                        for (int dx = lo; dx <= hi; ++dx) {
                            int px = nx + dx, pz = nz + dz;
                            if (px >= 0 && pz >= 0 && px < S && pz < S && feat_blocks(featg[id(px, pz)])) pen += feat_cost;
                        }
                }
                uint32_t ng = gc + 1 + pen;
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
    // too few cells for the mission (small floors): split again with a smaller largest leaf, down
    // to 2 * leaf_min - 1 (the smallest a cut can still halve); counted in f.leaf_shrinks
    for (int lm = P.leaf_max - 2; (int)cells.size() < N + 2 && P.adaptive_leaf && lm >= 2 * P.leaf_min - 1; lm -= 2) {
        Params Q = P;
        Q.leaf_max = lm;
        cells.clear();
        Rng r2(hash_words({seed, 0x1EAF, (uint64_t)attempt, (uint64_t)lm}));
        bsp(r2, {1, 1, S - 1, S - 1}, Q, cells);
        ++f.leaf_shrinks;
    }
    f.cells = (int)cells.size();
    if ((int)cells.size() < N) return std::to_string(cells.size()) + " BSP cells for " + std::to_string(N) + " nodes";

    // bands: cells by centre (x, then z) split into contiguous runs, one per lock region
    auto centre = [&](int c) { return std::pair<int, int>{(cells[c][0] + cells[c][2]) / 2, (cells[c][1] + cells[c][3]) / 2}; };
    int nreg = (int)*std::max_element(f.reg.begin(), f.reg.end()) + 1;
    std::vector<int> order(cells.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
    // cells in Hilbert-curve order of their centres, so a run of cells (a lock region's band) is a
    // compact blob, not a one-cell-wide column (P.band_order_x = the old x-then-z order, the control)
    auto hilbert = [](int x, int z) {
        long long d = 0;
        for (int s = 128; s > 0; s /= 2) {
            int rx = (x & s) > 0, rz = (z & s) > 0;
            d += (long long)s * s * ((3 * rx) ^ rz);
            if (rz == 0) { if (rx == 1) { x = 255 - x; z = 255 - z; } std::swap(x, z); }
        }
        return d;
    };
    if (P.band_order_x)
        std::sort(order.begin(), order.end(), [&](int a, int b) { return std::make_tuple(centre(a).first, centre(a).second, a) < std::make_tuple(centre(b).first, centre(b).second, b); });
    else
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return std::make_tuple(hilbert(centre(a).first, centre(a).second), a) < std::make_tuple(hilbert(centre(b).first, centre(b).second), b); });
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

    // special nodes (HAVEN, BOSS, VAULT, NEXUS, STRONGHOLD) get a cell their special program fits:
    // a swap inside the node's band (its lock region, so the region rules hold); the boss takes the
    // largest cell of its band and needs at least P.boss_min tiles a side inside the cell margin
    if (P.special_cells) {
        auto inner = [&](int c) { return std::pair<int, int>{cells[c][2] - cells[c][0] - 4, cells[c][3] - cells[c][1] - 4}; };
        std::vector<int> node_at(cells.size(), -1);
        for (int v = 0; v < N; ++v) node_at[cell_of[v]] = v;
        std::vector<char> fixed(N, 0);
        // the boss first (it wants the largest), then the rest in node order
        std::vector<int> order_sp;
        for (int v = 0; v < N; ++v) if (m.nodes[v].kind == Kind::Boss) order_sp.push_back(v);
        for (int v = 0; v < N; ++v) if (*special_of(m.nodes[v].kind) && m.nodes[v].kind != Kind::Boss) order_sp.push_back(v);
        for (int v : order_sp) {
            const std::string sp = special_of(m.nodes[v].kind);
            int nw = 1 << 30, nd = 1 << 30;
            for (auto& p : progs) if (p.special == sp) { nw = std::min(nw, p.min[0]); nd = std::min(nd, p.min[2]); }
            if (nw == (1 << 30)) continue; // no special program of this kind in the kits
            bool boss = m.nodes[v].kind == Kind::Boss;
            if (boss) { nw = std::max(nw, P.boss_min); nd = std::max(nd, P.boss_min); }
            int best = -1;
            long long ba = -1;
            for (int c = 0; c < (int)cells.size(); ++c) {
                if (band_of_cell[c] != f.reg[v]) continue;
                int u = node_at[c];
                if (u >= 0 && u != v && (fixed[u] || *special_of(m.nodes[u].kind))) continue;
                auto [cw, cd] = inner(c);
                if (cw < nw || cd < nd) continue;
                long long area = (long long)cw * cd;
                // the current cell if it fits; else the nearest fitting cell (a corridor from the
                // node's neighbours, embedded near the old cell, can still reach it); ties by area,
                // the boss preferring the larger, the rest the smaller. P.boss_largest = the old rule.
                long long dx = (cells[c][0] + cells[c][2]) / 2 - (cells[cell_of[v]][0] + cells[cell_of[v]][2]) / 2;
                long long dz = (cells[c][1] + cells[c][3]) / 2 - (cells[cell_of[v]][1] + cells[cell_of[v]][3]) / 2;
                long long near = -(dx * dx + dz * dz) * 4096 + (boss ? area : -area);
                long long score = (boss && P.boss_largest) ? area : (c == cell_of[v] ? (1LL << 50) : near);
                if (best < 0 || score > ba) { best = c; ba = score; }
            }
            if (best < 0) {
                // no single cell fits: grow a rectangle of up to 4 cells of the band, each step
                // adding a cell that shares a whole edge with the rectangle so far (the union stays
                // a rectangle, no other cell inside it). Non-special occupants move to spare cells
                // of the band (v's old cell counts as spare). The union goes to v; the others retire.
                auto shares = [](const Rect& R, const Rect& B) {
                    return (R[1] == B[1] && R[3] == B[3] && (R[2] == B[0] || B[2] == R[0])) ||
                           (R[0] == B[0] && R[2] == B[2] && (R[3] == B[1] || B[3] == R[1]));
                };
                auto movable = [&](int c) { int u = node_at[c]; return u < 0 || u == v || (!fixed[u] && !*special_of(m.nodes[u].kind)); };
                long long bestA = -1;
                std::vector<int> bestS;
                for (int a = 0; a < (int)cells.size(); ++a) {
                    if (band_of_cell[a] != f.reg[v] || !movable(a)) continue;
                    std::vector<int> S{a};
                    Rect R = cells[a];
                    for (int step = 0; step < 3; ++step) {
                        if (R[2] - R[0] - 4 >= nw && R[3] - R[1] - 4 >= nd) break;
                        int pick = -1;
                        long long ps = -1;
                        for (int b = 0; b < (int)cells.size(); ++b) {
                            if (band_of_cell[b] != f.reg[v] || std::find(S.begin(), S.end(), b) != S.end() || !movable(b) || !shares(R, cells[b])) continue;
                            Rect U{std::min(R[0], cells[b][0]), std::min(R[1], cells[b][1]), std::max(R[2], cells[b][2]), std::max(R[3], cells[b][3])};
                            long long s = std::min(U[2] - U[0], U[3] - U[1]); // grow the short side first
                            if (s > ps) { ps = s; pick = b; }
                        }
                        if (pick < 0) break;
                        Rect B = cells[pick];
                        R = {std::min(R[0], B[0]), std::min(R[1], B[1]), std::max(R[2], B[2]), std::max(R[3], B[3])};
                        S.push_back(pick);
                    }
                    int cw = R[2] - R[0] - 4, cd = R[3] - R[1] - 4;
                    if (cw < nw || cd < nd) continue;
                    int movers = 0, spares = 0;
                    for (int c : S) movers += node_at[c] >= 0 && node_at[c] != v;
                    for (int c = 0; c < (int)cells.size(); ++c)
                        if (band_of_cell[c] == f.reg[v] && std::find(S.begin(), S.end(), c) == S.end() && (node_at[c] < 0 || c == cell_of[v])) ++spares;
                    if (spares < movers) continue;
                    long long score = (long long)cw * cd - 1000LL * (long long)S.size(); // fewest cells, then largest
                    if (bestS.empty() || score > bestA) { bestA = score; bestS = S; }
                }
                if (bestS.empty() && P.band_steal) {
                    // the band has no room for this special even merged: take the nearest fitting
                    // cell of another band into this one (its occupant, if any, moves to a spare cell
                    // of its own band). Bands only steer corridors; the critic still checks locks.
                    long long cx = 0, cz = 0, n = 0;
                    for (int c = 0; c < (int)cells.size(); ++c)
                        if (band_of_cell[c] == f.reg[v]) { cx += (cells[c][0] + cells[c][2]) / 2; cz += (cells[c][1] + cells[c][3]) / 2; ++n; }
                    if (n) { cx /= n; cz /= n; }
                    int pick = -1, home = -1;
                    long long pd = 0;
                    for (int c = 0; c < (int)cells.size(); ++c) {
                        uint32_t bc = band_of_cell[c];
                        if (bc == f.reg[v] || bc == UINT32_MAX) continue;
                        auto [cw, cd] = inner(c);
                        if (cw < nw || cd < nd) continue;
                        int u = node_at[c], h = -1;
                        if (u >= 0) {
                            if (fixed[u] || *special_of(m.nodes[u].kind)) continue;
                            for (int c2 = 0; c2 < (int)cells.size(); ++c2) if (c2 != c && band_of_cell[c2] == bc && node_at[c2] < 0) { h = c2; break; }
                            if (h < 0) continue;
                        }
                        long long dx = (cells[c][0] + cells[c][2]) / 2 - cx, dz = (cells[c][1] + cells[c][3]) / 2 - cz;
                        long long d2 = dx * dx + dz * dz;
                        // a cell touching the band (a shared boundary run) first: a corridor can reach it
                        bool touch = false;
                        for (int c2 = 0; c2 < (int)cells.size() && !touch; ++c2) {
                            if (band_of_cell[c2] != f.reg[v]) continue;
                            const Rect &A = cells[c], &B = cells[c2];
                            bool xs = (A[2] == B[0] || B[2] == A[0]) && std::min(A[3], B[3]) - std::max(A[1], B[1]) >= 4;
                            bool zs = (A[3] == B[1] || B[3] == A[1]) && std::min(A[2], B[2]) - std::max(A[0], B[0]) >= 4;
                            touch = xs || zs;
                        }
                        if (!touch) d2 += 1LL << 40;
                        if (pick < 0 || d2 < pd) { pick = c; pd = d2; home = h; }
                    }
                    if (pick >= 0) {
                        int u = node_at[pick];
                        if (u >= 0) { cell_of[u] = home; node_at[home] = u; }
                        node_at[cell_of[v]] = -1;
                        band_of_cell[pick] = f.reg[v];
                        cell_of[v] = pick; node_at[pick] = v;
                        for (int c = 0; c < (int)cells.size(); ++c) used[c] = node_at[c] >= 0 || band_of_cell[c] == UINT32_MAX;
                        ++f.band_steals;
                        fixed[v] = 1;
                        continue;
                    }
                }
                if (bestS.empty()) return "no cell in band " + std::to_string(f.reg[v]) + " fits special " + sp;
                std::vector<int> movers;
                node_at[cell_of[v]] = -1;
                Rect R = cells[bestS[0]];
                for (int c : bestS) {
                    int u = node_at[c];
                    if (u >= 0 && u != v) movers.push_back(u);
                    node_at[c] = -1;
                    R = {std::min(R[0], cells[c][0]), std::min(R[1], cells[c][1]), std::max(R[2], cells[c][2]), std::max(R[3], cells[c][3])};
                }
                int ma = bestS[0];
                cells[ma] = R;
                for (size_t k = 1; k < bestS.size(); ++k) { cells[bestS[k]] = {0, 0, 0, 0}; band_of_cell[bestS[k]] = UINT32_MAX; } // retired
                cell_of[v] = ma; node_at[ma] = v;
                for (int u : movers) {
                    int home = -1;
                    for (int c = 0; c < (int)cells.size(); ++c)
                        if (band_of_cell[c] == f.reg[v] && node_at[c] < 0) { home = c; break; }
                    if (home < 0) return "merge left node " + m.nodes[u].name + " without a cell";
                    cell_of[u] = home; node_at[home] = u;
                }
                for (int c = 0; c < (int)cells.size(); ++c) used[c] = node_at[c] >= 0 || band_of_cell[c] == UINT32_MAX;
                f.special_merges += (int)bestS.size() - 1;
                fixed[v] = 1;
                continue;
            }
            int old = cell_of[v], u = node_at[best];
            if (best != old) {
                cell_of[v] = best; node_at[best] = v; node_at[old] = u;
                if (u >= 0) cell_of[u] = old; else used[old] = 0, used[best] = 1;
                ++f.special_swaps;
            }
            fixed[v] = 1;
        }
    }

    // theme of the floor (by seed, not by attempt) and a district per lock region from the
    // structure grammar (kits/districts.toml): region 0 is the approach, deeper regions the buildings
    f.theme = P.theme.empty() ? THEMES[hash_words({seed, 0x7E3E}) % 6] : P.theme;
    {
        const std::vector<std::string>* ds = nullptr;
        for (auto& t : P.districts) if (t.first == f.theme) ds = &t.second;
        for (int k = 0; k < nreg; ++k)
            f.region_district.push_back(ds ? (*ds)[(k == 0 ? 0 : hash_words({seed, 0xD157, (uint64_t)k}) % ds->size())] : std::string());
    }
    // rooms: a special program for a special node (HAVEN, BOSS, VAULT, NEXUS, STRONGHOLD) when one
    // fits; else a program of the floor's theme and the region's district, else of the theme, else
    // any that fits (bosses take the largest); sized inside the cell
    std::vector<std::vector<uint8_t>> room_floor(N), room_feat(N);
    for (int node = 0; node < N; ++node) {
        Rect cell = cells[cell_of[node]];
        int cw = cell[2] - cell[0] - 4, cd = cell[3] - cell[1] - 4;
        const std::string sp0 = special_of(m.nodes[node].kind);
        // the cave layer's lava vent (gameplay/20 section 2): the first plain room a vent program fits
        const bool vent = sp0.empty() && P.vent && f.vent_rooms == 0 && m.nodes[node].kind == Kind::Room;
        const std::string sp = vent ? "vent" : sp0;
        const std::string& dist = f.region_district[f.reg[node]];
        std::vector<const Program*> fits, special, themed, district;
        for (auto& p : progs) {
            if (p.min[0] > cw || p.min[2] > cd) continue;
            if (!p.special.empty()) { if (p.special == sp) special.push_back(&p); continue; }
            fits.push_back(&p);
            if (p.theme == f.theme) { themed.push_back(&p); if (!dist.empty() && p.district == dist) district.push_back(&p); }
        }
        if (fits.empty() && special.empty()) return "no program fits cell " + std::to_string(cw) + "x" + std::to_string(cd);
        const std::vector<const Program*>& pool = !special.empty() ? special : !district.empty() ? district : !themed.empty() ? themed : fits;
        if (vent) f.vent_rooms += !special.empty();
        else f.special_rooms += !special.empty();
        if (!sp0.empty()) {
            if (special.empty()) ++f.special_missed;
            else {
                static const char* SK[] = {"haven", "boss", "vault", "nexus", "stronghold"};
                for (int k = 0; k < 5; ++k) if (sp == SK[k]) f.special_kinds |= 1u << k;
            }
        }
        f.district_rooms += special.empty() && !district.empty();
        f.themed_rooms += special.empty() && !themed.empty();
        const Program* p = pool[0];
        if (m.nodes[node].kind == Kind::Boss) {
            for (auto* q2 : pool) if (q2->max[0] * q2->max[2] > p->max[0] * p->max[2]) p = q2;
        } else p = pool[r.next(pool.size())];
        int w = std::max(p->min[0], std::min(cw, p->max[0]));
        int d = std::max(p->min[2], std::min(cd, p->max[2]));
        int h = std::max(p->min[1], std::min(p->max[1], 6));
        int x0 = cell[0] + 2 + (cw - w) / 2, z0 = cell[1] + 2 + (cd - d) / 2;
        if (m.nodes[node].kind == Kind::Boss) { f.boss_w = w; f.boss_d = d; }
        RoomRow row{node, p->id, x0, z0, x0 + w, z0 + d, 0, 0, dist};
        std::string e = run_program(*p, w, h, d, hash_words({seed, 0xC4A7, (uint64_t)node}), room_floor[node], nullptr, &room_feat[node]);
        if (!e.empty()) {
            // a program that cannot run at this size: a plain box, counted and named in the log
            ++f.program_fallbacks;
            row.program = p->id + "!fallback";
            room_floor[node].assign((size_t)w * d, 0);
            room_feat[node].assign((size_t)w * d, F_NONE);
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
    D.featg.assign((size_t)S * S, F_NONE);
    D.feat_cost = P.corridor_feat_cost;
    for (auto& ro : f.rooms)
        for (int z = ro.z0; z < ro.z1; ++z)
            for (int x = ro.x0; x < ro.x1; ++x) D.featg[D.id(x, z)] = room_feat[ro.node][(size_t)(z - ro.z0) * (ro.x1 - ro.x0) + (x - ro.x0)];
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
    // edges in mission order; a failed edge restarts the corridor pass with that edge first
    // (earlier corridors of other regions can wall a room in: a corridor may not touch another
    // region's corridor). P.edge_reorders restarts at most, counted in f.edge_reorders.
    std::vector<int> eorder(m.edges.size());
    for (size_t k = 0; k < eorder.size(); ++k) eorder[k] = (int)k;
    const std::vector<uint8_t> occ0 = D.occ;
    std::string efail;
    for (int round = 0;; ++round) {
        efail.clear();
        int failed = -1;
        for (int k : eorder) {
            int a = m.edges[k].first, b = m.edges[k].second;
            int w = (on_path[a] && on_path[b] && (par[a] == b || par[b] == a)) ? 3 : (deg[a] == 1 || deg[b] == 1) ? 1 : 2;
            if (!D.dig(a, b, std::min(f.reg[a], f.reg[b]), w)) {
                failed = k;
                efail = "no corridor for edge " + m.nodes[a].name + "-" + m.nodes[b].name + " (" + std::to_string(f.rooms[a].door_x) + "," + std::to_string(f.rooms[a].door_z) +
                        " r" + std::to_string(f.reg[a]) + " -> " + std::to_string(f.rooms[b].door_x) + "," + std::to_string(f.rooms[b].door_z) + " r" + std::to_string(f.reg[b]) +
                        ", after " + std::to_string(round) + " reorders)";
                break;
            }
        }
        if (failed < 0) break;
        if (round >= P.edge_reorders) return efail;
        // restart: the failed edge moves to the front
        eorder.erase(std::find(eorder.begin(), eorder.end(), failed));
        eorder.insert(eorder.begin(), failed);
        D.occ = occ0;
        std::fill(D.cmask.begin(), D.cmask.end(), 0u);
        D.carve_in_box.clear();
        D.corridor_tiles = 0; D.narrowed = 0;
        for (int& x : D.width_hist) x = 0;
        ++f.edge_reorders;
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
    f.feat.assign((size_t)S * S, F_NONE);
    for (auto& ro : f.rooms) {
        int w = ro.x1 - ro.x0;
        for (int z = ro.z0; z < ro.z1; ++z)
            for (int x = ro.x0; x < ro.x1; ++x) {
                size_t t = f.idx(x, z);
                f.owner[t] = (int16_t)ro.node;
                f.behind[t] = behind[ro.node];
                size_t li = (size_t)(z - ro.z0) * w + (x - ro.x0);
                if (room_floor[ro.node][li]) f.tile[t] = FLOOR;
                f.feat[t] = room_feat[ro.node][li];
            }
    }
    for (int t = 0; t < S * S; ++t)
        if (D.occ[t] != FREE && D.occ[t] != ROOMBOX) { f.tile[t] = FLOOR; f.behind[t] = D.cmask[t]; }
    for (int t : D.carve_in_box) { f.tile[t] = FLOOR; if (feat_blocks(f.feat[t])) f.feat[t] = F_BRIDGE; }
    // program islands: a room's floor pieces not joined to its door piece get a path inside the box
    for (auto& ro : f.rooms) {
        for (int guard = 0; guard < 64; ++guard) {
            std::vector<int> lab((size_t)S * S, -1);
            std::deque<int> qq{(int)f.idx(ro.door_x, ro.door_z)};
            lab[qq[0]] = 0;
            // the whole box: a corridor opening carved into a box corner has no interior neighbour
            auto inside = [&](int x, int z) { return x >= ro.x0 && z >= ro.z0 && x < ro.x1 && z < ro.z1; };
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
            // pass 0 keeps off lava, chasms and deep water; pass 1 may cross them (they become bridge)
            std::vector<int> prev;
            int hit = -1;
            for (int pass = P.corridor_feat_cost ? 0 : 1; pass < 2 && hit < 0; ++pass) {
                prev.assign((size_t)S * S, -2);
                std::deque<int> q2{island};
                prev[island] = -1;
                while (!q2.empty() && hit < 0) {
                    int c = q2.front(); q2.pop_front();
                    int x = c % S, z = c / S;
                    const int DD[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    for (auto& d2 : DD) {
                        int nx = x + d2[0], nz = z + d2[1];
                        if (!inside(nx, nz)) continue;
                        int n = nz * S + nx;
                        if (prev[n] != -2) continue;
                        if (pass == 0 && feat_blocks(f.feat[n])) continue;
                        prev[n] = c;
                        if (lab[n] == 0) { hit = n; break; }
                        q2.push_back(n);
                    }
                }
            }
            if (hit < 0) return "room " + std::to_string(ro.node) + " (" + ro.program + " " + std::to_string(ro.x1 - ro.x0) + "x" + std::to_string(ro.z1 - ro.z0) + ") island cannot join its door";
            for (int c = hit; c >= 0; c = prev[c]) { f.tile[c] = FLOOR; if (feat_blocks(f.feat[c])) f.feat[c] = F_BRIDGE; }
            ++f.island_repairs;
        }
    }
    // 02 section 8: walls of a lock's far side are undiggable; the map border is undiggable
    for (int z = 0; z < S; ++z)
        for (int x = 0; x < S; ++x) {
            size_t t = f.idx(x, z);
            if (x == 0 || z == 0 || x == S - 1 || z == S - 1) { f.tile[t] = HARD; continue; }
            if (f.tile[t] == ROCK && feat_blocks(f.feat[t])) { f.tile[t] = HARD; continue; } // a chasm, lava or deep water is not dug
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
    for (uint8_t t : f.feat) byte(t);
    auto ents = f.entities;
    std::sort(ents.begin(), ents.end(), [](const EntityRow& a, const EntityRow& b) { return std::tie(a.kind, a.id, a.x, a.z) < std::tie(b.kind, b.id, b.x, b.z); });
    for (auto& e : ents) { str(e.kind); word((uint64_t)e.id); word((uint64_t)e.x); word((uint64_t)e.z); }
    for (auto& r : f.rooms) { word((uint64_t)r.node); str(r.program); word((uint64_t)r.x0); word((uint64_t)r.z0); word((uint64_t)r.x1); word((uint64_t)r.z1); }
    return h;
}

} // namespace apx
