// The critic's hard checks at generation time (02 section 7) and the dig-BFS gate (section 8):
//   overlap    room boxes are disjoint
//   connected  the floor tiles form one 4-connected component (connectivity 100 percent)
//   reach      with every lock open, every room's door and the exit are reached from the entrance
//   locks      solvable in key order on tiles; with lock k's room closed, key k is reached and no
//              tile behind lock k is (walking, 8-connected: diagonal steps count, the strict case)
//   dig        the same with every ROCK tile passable (digging): no tile behind lock k is reached
//   mission    the graph checks (check_mission) hold
#include "apxgen/apxgen.hpp"
#include <deque>

namespace apx {

namespace {
// BFS from the entrance door; `closed` lock ids (bit id-1) block their lock room's whole box.
std::vector<char> flood(const Floor& f, uint32_t closed, bool dig, bool diag) {
    const int S = f.W;
    std::vector<char> seen(f.tile.size(), 0);
    auto blocked = [&](size_t t) {
        if (f.tile[t] == HARD) return true;
        if (f.tile[t] == ROCK && !dig) return true;
        int o = f.owner[t];
        if (o >= 0 && f.mission.nodes[o].kind == Kind::Lock) {
            int id = f.mission.nodes[o].id;
            if (id >= 1 && id <= 31 && (closed >> (id - 1)) & 1) return true;
        }
        return false;
    };
    size_t s = f.idx(f.rooms[0].door_x, f.rooms[0].door_z);
    std::deque<size_t> q{s};
    seen[s] = 1;
    while (!q.empty()) {
        size_t c = q.front(); q.pop_front();
        int x = (int)(c % S), z = (int)(c / S);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                if ((dx == 0 && dz == 0) || (!diag && dx != 0 && dz != 0)) continue;
                int nx = x + dx, nz = z + dz;
                if (nx < 0 || nz < 0 || nx >= S || nz >= f.H) continue;
                size_t n = f.idx(nx, nz);
                if (seen[n] || blocked(n)) continue;
                seen[n] = 1;
                q.push_back(n);
            }
    }
    return seen;
}
} // namespace

Verdict critic(const Floor& f) {
    Verdict v;
    auto why = [&](const std::string& s) { if (v.why.empty()) v.why = s; };
    const Mission& m = f.mission;
    std::string mc = check_mission(m);
    if (!mc.empty()) { v.mission_ok = false; why("mission: " + mc); }
    for (size_t a = 0; a < f.rooms.size(); ++a)
        for (size_t b = a + 1; b < f.rooms.size(); ++b) {
            auto &p = f.rooms[a], &q = f.rooms[b];
            if (p.x0 < q.x1 && q.x0 < p.x1 && p.z0 < q.z1 && q.z0 < p.z1) { v.overlap_ok = false; why("rooms overlap"); }
        }
    // connectivity: 4-connected components of the floor
    {
        const int S = f.W;
        std::vector<char> seen(f.tile.size(), 0);
        for (size_t s = 0; s < f.tile.size(); ++s) {
            if (f.tile[s] != FLOOR) continue;
            ++v.floor_tiles;
            if (seen[s]) continue;
            ++v.components;
            std::deque<size_t> q{s};
            seen[s] = 1;
            while (!q.empty()) {
                size_t c = q.front(); q.pop_front();
                int x = (int)(c % S), z = (int)(c / S);
                const int D[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (auto& d : D) {
                    int nx = x + d[0], nz = z + d[1];
                    if (nx < 0 || nz < 0 || nx >= S || nz >= f.H) continue;
                    size_t n = f.idx(nx, nz);
                    if (f.tile[n] == FLOOR && !seen[n]) { seen[n] = 1; q.push_back(n); }
                }
            }
        }
        if (v.components != 1) { v.connected = false; why("floor has " + std::to_string(v.components) + " components"); }
    }
    // reach with every lock open, 4-connected walking
    {
        auto r = flood(f, 0, false, false);
        for (size_t t = 0; t < r.size(); ++t) v.reached_tiles += r[t] && f.tile[t] == FLOOR;
        for (auto& ro : f.rooms)
            if (!r[f.idx(ro.door_x, ro.door_z)]) { v.reach_ok = false; why("room " + m.nodes[ro.node].name + " unreachable"); }
    }
    // locks: solvable in key order, then each lock gates its far side and its key is before it
    uint32_t all = 0;
    for (auto& n : m.nodes) if (n.kind == Kind::Lock && n.id >= 1 && n.id <= 31) all |= 1u << (n.id - 1);
    {
        uint32_t closed = all;
        for (;;) {
            auto r = flood(f, closed, false, false);
            uint32_t before = closed;
            for (auto& ro : f.rooms) {
                const Node& n = m.nodes[ro.node];
                if (n.kind == Kind::Key && n.id >= 1 && n.id <= 31 && r[f.idx(ro.door_x, ro.door_z)]) closed &= ~(1u << (n.id - 1));
            }
            if (closed == before) break;
        }
        auto r = flood(f, closed, false, false);
        int ex = m.find(Kind::Exit);
        if (closed || ex < 0 || !r[f.idx(f.rooms[ex].door_x, f.rooms[ex].door_z)]) { v.locks_ok = false; why("not solvable in key order on tiles"); }
    }
    for (auto& n : m.nodes) {
        if (n.kind != Kind::Lock || n.id < 1 || n.id > 31) continue;
        uint32_t bit = 1u << (n.id - 1);
        int key = m.find(Kind::Key, n.id);
        auto walk = flood(f, bit, false, true);
        if (key < 0 || !walk[f.idx(f.rooms[key].door_x, f.rooms[key].door_z)]) { v.locks_ok = false; why("key " + std::to_string(n.id) + " not reachable before lock " + std::to_string(n.id)); }
        for (size_t t = 0; t < walk.size(); ++t)
            if (walk[t] && f.tile[t] == FLOOR && (f.behind[t] & bit)) { v.locks_ok = false; why("walking bypasses lock " + std::to_string(n.id)); break; }
        auto dig = flood(f, bit, true, true);
        for (size_t t = 0; t < dig.size(); ++t)
            if (dig[t] && f.tile[t] == FLOOR && (f.behind[t] & bit)) {
                v.dig_ok = false;
                why("digging bypasses lock " + std::to_string(n.id) + " at " + std::to_string(t % f.W) + "," + std::to_string(t / f.W));
                break;
            }
    }
    return v;
}

Result generate(uint64_t seed, const Grammar& g, const std::vector<Program>& progs, const Params& P) {
    Result res;
    for (int a = 0; a < P.max_attempts; ++a) {
        std::string e = build_floor(seed, a, g, progs, P, res.floor);
        if (!e.empty()) { res.log.push_back("attempt " + std::to_string(a) + " layout: " + e); continue; }
        res.verdict = critic(res.floor);
        if (res.verdict.pass()) { res.ok = true; return res; }
        res.log.push_back("attempt " + std::to_string(a) + " critic: " + res.verdict.why);
    }
    return res;
}

} // namespace apx
