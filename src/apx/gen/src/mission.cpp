// MISSION (02 section 3; port of bl-world mission/check_mission/regions): a Dormans-style cyclic
// graph built by running grammar rows in order. Rows:
//   add  NAME kind[:id]                 a node
//   link A B                            an edge
//   cycle A B P la lb                   two room paths A..B of la and lb rooms (Pa1.., Pb1..): a cycle
//   hang NAME kind[:id] C1 C2 ...       a leaf hung on one of the candidates (seeded pick)
//   grow MIN MAX                        subdivide edges that touch no lock with rooms up to a seeded
//                                       total in [MIN, MAX]
#include "apxgen/apxgen.hpp"
#include <algorithm>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>

namespace apx {

uint64_t mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
uint64_t hash_words(std::initializer_list<uint64_t> w) {
    uint64_t h = 0x243F6A8885A308D3ull;
    for (uint64_t v : w) h = mix(h ^ mix(v));
    return h;
}
uint64_t hash_str(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

static const char* KN[] = {"entrance", "room", "key", "lock", "boss", "treasure", "secret", "haven", "nexus", "stronghold", "exit"};
const char* kind_name(Kind k) { return KN[(int)k]; }
bool kind_parse(const std::string& s, Kind& k) {
    for (int i = 0; i < 11; ++i) if (s == KN[i]) { k = (Kind)i; return true; }
    return false;
}

std::vector<int> Mission::neighbours(int v) const {
    std::vector<int> o;
    for (auto& e : edges) { if (e.first == v) o.push_back(e.second); else if (e.second == v) o.push_back(e.first); }
    return o;
}
int Mission::find(Kind k, int id) const {
    for (size_t i = 0; i < nodes.size(); ++i) if (nodes[i].kind == k && nodes[i].id == id) return (int)i;
    return -1;
}
int Mission::cycles() const { return (int)edges.size() - (int)nodes.size() + 1; }

bool load_grammar(const std::string& path, Grammar& g, std::string& err) {
    std::ifstream in(path);
    if (!in) { err = "cannot open grammar " + path; return false; }
    g.rows.clear(); g.source = path;
    std::string line;
    while (std::getline(in, line)) {
        auto h = line.find('#');
        if (h != std::string::npos) line = line.substr(0, h);
        std::istringstream ss(line);
        std::string w, out;
        while (ss >> w) out += (out.empty() ? "" : " ") + w;
        if (!out.empty()) g.rows.push_back(out);
    }
    return true;
}

static bool is_lock(const Mission& m, int v) { return m.nodes[v].kind == Kind::Lock; }

bool mission_from_grammar(const Grammar& g, uint64_t seed, Mission& m, std::string& err, int room_cap) {
    Rng r(hash_words({seed, 0x4D15}));
    m = Mission{};
    std::map<std::string, int> name;
    auto add = [&](const std::string& n, Kind k, int id) {
        m.nodes.push_back({k, id, n});
        name[n] = (int)m.nodes.size() - 1;
        return (int)m.nodes.size() - 1;
    };
    auto link = [&](int a, int b) { m.edges.push_back({std::min(a, b), std::max(a, b)}); };
    auto kindid = [&](const std::string& s, Kind& k, int& id) {
        auto c = s.find(':');
        id = c == std::string::npos ? 0 : std::atoi(s.c_str() + c + 1);
        return kind_parse(s.substr(0, c), k);
    };
    auto node = [&](const std::string& n, int& v) {
        auto it = name.find(n);
        if (it == name.end()) return false;
        v = it->second;
        return true;
    };
    for (size_t ri = 0; ri < g.rows.size(); ++ri) {
        std::istringstream ss(g.rows[ri]);
        std::vector<std::string> t;
        std::string w;
        while (ss >> w) t.push_back(w);
        auto bad = [&](const std::string& why) { err = "grammar row " + std::to_string(ri + 1) + " '" + g.rows[ri] + "': " + why; return false; };
        Kind k; int id, a, b;
        if (t[0] == "add" && t.size() == 3) {
            if (!kindid(t[2], k, id)) return bad("unknown kind");
            if (name.count(t[1])) return bad("duplicate name");
            add(t[1], k, id);
        } else if (t[0] == "link" && t.size() == 3) {
            if (!node(t[1], a) || !node(t[2], b)) return bad("unknown node");
            link(a, b);
        } else if (t[0] == "cycle" && t.size() == 6) {
            if (!node(t[1], a) || !node(t[2], b)) return bad("unknown node");
            for (int side = 0; side < 2; ++side) {
                int len = std::atoi(t[4 + side].c_str()), prev = a;
                for (int i = 1; i <= len; ++i) {
                    int n = add(t[3] + (side ? "b" : "a") + std::to_string(i), Kind::Room, 0);
                    link(prev, n);
                    prev = n;
                }
                link(prev, b);
            }
        } else if (t[0] == "hang" && t.size() >= 4) {
            if (!kindid(t[2], k, id)) return bad("unknown kind");
            std::vector<int> cand;
            for (size_t i = 3; i < t.size(); ++i) { if (!node(t[i], a)) return bad("unknown node " + t[i]); cand.push_back(a); }
            int on = cand[r.next(cand.size())];
            link(on, add(t[1], k, id));
        } else if (t[0] == "grow" && t.size() == 3) {
            int lo = std::atoi(t[1].c_str()), hi = std::atoi(t[2].c_str());
            int target = lo + (int)r.next((uint64_t)(hi - lo + 1));
            if (room_cap >= 0 && target > room_cap) {
                if (room_cap < lo) return bad("room cap " + std::to_string(room_cap) + " under the grow minimum " + std::to_string(lo));
                target = room_cap; // a small floor: the mission grows to what its cells hold
            }
            int serial = 0;
            while ((int)m.nodes.size() < target) {
                std::vector<int> cand;
                for (size_t i = 0; i < m.edges.size(); ++i)
                    if (!is_lock(m, m.edges[i].first) && !is_lock(m, m.edges[i].second)) cand.push_back((int)i);
                if (cand.empty()) return bad("no edge to grow");
                int e = cand[r.next(cand.size())];
                auto [ea, eb] = m.edges[e];
                int n = add("g" + std::to_string(++serial), Kind::Room, 0);
                m.edges[e] = {std::min(ea, n), std::max(ea, n)};
                link(n, eb);
            }
        } else {
            return bad("unknown row");
        }
    }
    if (m.nodes.empty() || m.nodes[0].kind != Kind::Entrance) { err = "first node must be the entrance"; return false; }
    return true;
}

std::vector<char> reach(const Mission& m, const std::vector<int>& closed) {
    std::vector<char> seen(m.nodes.size(), 0);
    std::deque<int> q{0};
    seen[0] = 1;
    while (!q.empty()) {
        int v = q.front(); q.pop_front();
        for (int n : m.neighbours(v))
            if (!seen[n] && std::find(closed.begin(), closed.end(), n) == closed.end()) { seen[n] = 1; q.push_back(n); }
    }
    return seen;
}

// Every key reachable with its lock closed, every lock gates the boss, solvable in key order, the
// graph has at least 2 independent cycles, and the required special rooms exist.
std::string check_mission(const Mission& m) {
    int boss = m.find(Kind::Boss), exit = m.find(Kind::Exit);
    if (boss < 0) return "no boss";
    if (exit < 0) return "no exit";
    for (Kind k : {Kind::Treasure, Kind::Secret, Kind::Haven, Kind::Nexus, Kind::Stronghold}) {
        bool any = false;
        for (auto& n : m.nodes) any |= n.kind == k;
        if (!any) return std::string("no ") + kind_name(k);
    }
    std::vector<int> locks;
    for (size_t i = 0; i < m.nodes.size(); ++i) {
        if (m.nodes[i].kind != Kind::Lock) continue;
        int id = m.nodes[i].id, key = m.find(Kind::Key, id);
        if (key < 0) return "lock " + std::to_string(id) + " without key";
        auto r = reach(m, {(int)i});
        if (!r[key]) return "key " + std::to_string(id) + " behind its own lock";
        if (r[boss]) return "lock " + std::to_string(id) + " does not gate the boss";
        locks.push_back((int)i);
    }
    std::vector<int> closed = locks;
    for (;;) {
        auto r = reach(m, closed);
        size_t before = closed.size();
        closed.erase(std::remove_if(closed.begin(), closed.end(), [&](int l) { return r[m.find(Kind::Key, m.nodes[l].id)] != 0; }), closed.end());
        if (closed.size() == before) break;
    }
    auto r = reach(m, closed);
    for (char v : r) if (!v) return "not solvable in key order";
    if (m.cycles() < 2) return "fewer than 2 cycles (" + std::to_string(m.cycles()) + ")";
    return "";
}

std::vector<uint32_t> regions(const Mission& m) {
    std::vector<uint32_t> reg(m.nodes.size(), 0);
    for (size_t l = 0; l < m.nodes.size(); ++l) {
        if (m.nodes[l].kind != Kind::Lock) continue;
        auto r = reach(m, {(int)l});
        for (size_t i = 0; i < r.size(); ++i) if (!r[i]) reg[i] += 1;
    }
    return reg;
}

std::vector<uint32_t> behind_masks(const Mission& m) {
    std::vector<uint32_t> b(m.nodes.size(), 0);
    for (size_t l = 0; l < m.nodes.size(); ++l) {
        if (m.nodes[l].kind != Kind::Lock || m.nodes[l].id < 1 || m.nodes[l].id > 31) continue;
        auto r = reach(m, {(int)l});
        for (size_t i = 0; i < r.size(); ++i) if (!r[i]) b[i] |= 1u << (m.nodes[l].id - 1);
    }
    return b;
}

} // namespace apx
