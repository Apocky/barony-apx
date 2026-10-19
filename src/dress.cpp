// DRESS: the micro pass (lattice/gen/31 G4 FIXTURES and G6 DRESSING; 32 section 3 function
// programs; gameplay/20 zones and density rules; 33 soft terms).
//
// Per room: ZONES (entry within 2 steps of an opening, hazard on or beside a hazard feature, edge
// along the walls, core the rest) -> ACTIVITIES of the room's function program, each a group with
// an ANCHOR fixture placed by salience (focal anchors at the deepest core tile, wall fixtures with a
// wall behind them), members in the anchor's eight neighbours, props on the anchor -> ITEMS by
// presence bucket (GUARANTEED 100, LIKELY 80, RARE 8, NEVER 0 per mille) -> VIGNETTES (story
// clutter clusters) -> WALL dressing per wall face -> CLUTTER clustered beside what is already
// there until free floor is at most 45 percent (rooms) and density is at least 3 per 10 m^2.
// Clearance (33 section 1): door bands and entity tiles stay free, every blocking fixture keeps an
// access tile on its use side, and a blocking fixture is accepted only when the room's walkable
// tiles stay one 4-connected piece.
#include "apxgen/apxgen.hpp"
#include <algorithm>
#include <deque>
#include <filesystem>
#include <map>
#include <set>
#include <cstdio>
#include <cstdlib>

namespace apx {

const char* const CONTENT_KINDS[9] = {"container", "station", "liquid", "hazard", "light", "den", "npc", "writing", "secret"};
int content_bit(const std::string& k) {
    for (int i = 0; i < 9; ++i) if (k == CONTENT_KINDS[i]) return i;
    return -1;
}
const char* zone_name(uint8_t z) {
    static const char* N[] = {"none", "entry", "core", "edge", "hazard"};
    return z < 5 ? N[z] : "?";
}
static uint8_t zone_parse(const std::string& s) {
    if (s == "entry") return Z_ENTRY;
    if (s == "core") return Z_CORE;
    if (s == "edge") return Z_EDGE;
    if (s == "hazard") return Z_HAZARD;
    return Z_NONE; // any
}

const FixtureDef* DressLib::fixture(const std::string& id) const { for (auto& f : fixtures) if (f.id == id) return &f; return nullptr; }
const PropDef* DressLib::prop(const std::string& id) const { for (auto& p : props) if (p.id == id) return &p; return nullptr; }
const FunctionProgram* DressLib::function(const std::string& id) const { for (auto& f : functions) if (f.id == id) return &f; return nullptr; }

bool load_dress(const std::string& root, DressLib& lib, std::string& err) {
    namespace fs = std::filesystem;
    auto files = [&](const std::string& dir) {
        std::vector<std::string> o;
        std::error_code ec;
        if (fs::is_directory(dir, ec))
            for (auto& e : fs::directory_iterator(dir)) if (e.path().extension() == ".toml") o.push_back(e.path().string());
        std::sort(o.begin(), o.end());
        return o;
    };
    for (auto& path : files(root + "/props")) {
        Val* doc = nullptr;
        if (!parse_toml_file(path, doc, err)) return false;
        if (fs::path(path).filename() == "props.toml") {
            for (auto* p : toml_arr(doc, "prop")) {
                PropDef d;
                d.id = toml_str(p, "id");
                d.content = toml_str(p, "content");
                d.place = toml_str(p, "place", "floor");
                d.variants = (int)toml_int(p, "variants", 2);
                if (d.id.empty()) { err = path + ": [[prop]] without id"; return false; }
                lib.props.push_back(d);
            }
            continue;
        }
        const Val* meta = toml_get(doc, "meta");
        FixtureDef f;
        f.id = toml_str(meta, "id");
        auto fp = toml_arr(meta, "footprint");
        if (f.id.empty() || fp.size() != 2) { err = path + ": meta.id and meta.footprint [2] required"; return false; }
        long long a = 1, b = 1;
        toml_expr(fp[0], {}, a); toml_expr(fp[1], {}, b);
        f.fw = (int)a; f.fd = (int)b;
        f.support = toml_str(meta, "support", "floor");
        f.blocks = toml_int(meta, "blocks", 1) != 0;
        f.content = toml_str(meta, "content");
        f.variants = (int)toml_int(meta, "variants", 1);
        f.doc = doc;
        lib.fixtures.push_back(f);
    }
    for (auto& path : files(root + "/functions")) {
        Val* doc = nullptr;
        if (!parse_toml_file(path, doc, err)) return false;
        FunctionProgram fn;
        fn.id = toml_str(toml_get(doc, "meta"), "id");
        fn.doc = doc;
        if (fn.id.empty()) { err = path + ": meta.id required"; return false; }
        // every id a program names must exist in the library
        auto known = [&](const std::string& id) { return lib.fixture(id) || lib.prop(id); };
        std::vector<std::string> ids = toml_strs(toml_get(doc, "meta"), "clutter");
        for (auto& n : toml_strs(toml_get(doc, "meta"), "never")) ids.push_back(n);
        for (auto* a2 : toml_arr(doc, "activity")) {
            ids.push_back(toml_str(a2, "anchor"));
            for (auto& s : toml_strs(a2, "around")) ids.push_back(s);
            for (auto& s : toml_strs(a2, "on")) ids.push_back(s);
        }
        for (auto* it : toml_arr(doc, "item")) ids.push_back(toml_str(it, "id"));
        for (auto* it : toml_arr(doc, "wall")) ids.push_back(toml_str(it, "id"));
        for (auto* v : toml_arr(doc, "vignette")) for (auto& s : toml_strs(v, "parts")) ids.push_back(s);
        for (auto& id : ids) if (!known(id)) { err = path + ": unknown fixture or prop '" + id + "'"; return false; }
        lib.functions.push_back(fn);
    }
    if (lib.fixtures.empty() || lib.functions.empty()) { err = "no fixtures or function programs under " + root; return false; }
    return true;
}

namespace {
enum Occ : uint8_t { O_FREE = 0, O_BLOCK = 1, O_ACCESS = 2, O_RESERVE = 3, O_PROP = 4 };
const int FX[4] = {0, 1, 0, -1}, FZ[4] = {1, 0, -1, 0};

int bucket_permille(const std::string& b) {
    if (b == "GUARANTEED") return 1000;
    if (b == "LIKELY") return 800;
    if (b == "RARE") return 80;
    return 0; // NEVER
}

struct RoomCtx {
    Floor& f;
    const DressLib& lib;
    const RoomRow& ro;
    RoomDress& rd;
    Rng rng;
    std::set<std::string> never;
    int W;
    std::vector<int> tiles;            // room floor tiles (global index)
    std::vector<uint8_t> in;           // global: 1 = room floor
    std::vector<uint8_t> occ;          // global occupancy (room tiles only meaningful)
    std::vector<int> walld;            // global: steps from a wall
    std::vector<int> openings;
    std::map<std::tuple<uint8_t, std::string, int>, int> clone; // (zone, id, variant) -> count
    std::vector<int> anchors;          // placed blocking fixture tiles (cluster targets)
    bool cap = false;                  // Clone Army cap: no 4th identical (id, variant) in a zone

    bool capped(uint8_t zone, const std::string& id, int variants) const {
        if (!cap) return false;
        for (int v = 0; v < variants; ++v) {
            auto it = clone.find({zone, id, v});
            if (it == clone.end() || it->second < 3) return false;
        }
        return true;
    }

    RoomCtx(Floor& f_, const DressLib& l, const RoomRow& r, RoomDress& d, uint64_t seed)
        : f(f_), lib(l), ro(r), rd(d), rng(hash_words({seed, 0xD7E5, (uint64_t)r.node})), W(f_.W) {}

    bool room(int x, int z) const { return x >= ro.x0 && z >= ro.z0 && x < ro.x1 && z < ro.z1 && in[(size_t)z * W + x]; }
    bool wall(int x, int z) const {
        if (x < 0 || z < 0 || x >= W || z >= f.H) return true;
        size_t t = (size_t)z * W + x;
        return f.tile[t] != FLOOR && !feat_blocks(f.feat[t]);
    }

    bool connected() const {
        int start = -1, total = 0;
        for (int t : tiles) if (occ[t] != O_BLOCK) { ++total; if (start < 0) start = t; }
        for (int t : openings) if (occ[t] == O_BLOCK) return false;
        if (start < 0) return false;
        std::vector<char> seen((size_t)(ro.x1 - ro.x0) * (ro.z1 - ro.z0), 0);
        auto li = [&](int t) { return (size_t)(t / W - ro.z0) * (ro.x1 - ro.x0) + (t % W - ro.x0); };
        std::deque<int> q{start};
        seen[li(start)] = 1;
        int n = 1;
        while (!q.empty()) {
            int c = q.front(); q.pop_front();
            for (int k = 0; k < 4; ++k) {
                int nx = c % W + FX[k], nz = c / W + FZ[k];
                if (!room(nx, nz)) continue;
                int t = nz * W + nx;
                if (occ[t] == O_BLOCK || seen[li(t)]) continue;
                seen[li(t)] = 1; ++n; q.push_back(t);
            }
        }
        return n == total;
    }

    int pick_variant(uint8_t zone, const std::string& id, int variants) {
        int best = 0, bc = 1 << 30;
        uint64_t off = rng.next((uint64_t)std::max(1, variants));
        for (int k = 0; k < variants; ++k) {
            int v = (int)((k + off) % variants);
            auto it = clone.find({zone, id, v});
            int c = it == clone.end() ? 0 : it->second;
            if (c < bc) { bc = c; best = v; }
        }
        return best;
    }

    void emit(const std::string& cls, const std::string& id, int x, int z, int rot, const std::string& source, const std::string& content, int variants) {
        size_t t = (size_t)z * W + x;
        uint8_t zn = f.zone[t];
        DressRow r;
        r.cls = cls; r.id = id; r.x = x; r.z = z; r.rot = rot; r.zone = zn; r.room = ro.node; r.source = source; r.content = content;
        r.variant = pick_variant(zn, id, variants);
        clone[{zn, id, r.variant}]++;
        if (never.count(id)) rd.never_hits++;
        int b = content_bit(content);
        if (b >= 0) rd.kinds |= 1 << b;
        rd.items++;
        if (cls == "fixture") rd.fixtures++;
        f.dress.push_back(r);
    }

    // footprint tiles of a fixture at (x, z) facing rot
    std::vector<int> foot(const FixtureDef& fx, int x, int z, int rot) const {
        std::vector<int> o;
        int rx = FZ[rot], rz = -FX[rot]; // right-hand axis
        for (int a = 0; a < fx.fw; ++a)
            for (int b = 0; b < fx.fd; ++b) {
                int px = x + a * rx + b * FX[rot], pz = z + a * rz + b * FZ[rot];
                if (!room(px, pz)) return {};
                o.push_back(pz * W + px);
            }
        return o;
    }

    // Places fixture `fx` (zone z or any); mode: 0 focal (deepest), 1 wall (shallowest), 2 near
    // `target`, 3 any. Returns the anchor tile or -1. A failed clearance test tries the next spot.
    int place_fixture(const FixtureDef& fx, uint8_t zone, int mode, int target, const std::string& source, bool relax_ok) {
        struct Cand { long long score; int x, z, rot; };
        // pass 0 the asked zone; 1 any zone but hazard; 2 any zone (a last resort, still counted)
        // pass 3 (GUARANTEED only): a wall fixture may stand free when no wall tile is left
        for (int pass = 0; pass < 4; ++pass) {
            if (pass >= 1 && !relax_ok) break;
            if (pass == 3 && fx.support != "wall") break;
            std::vector<Cand> cs;
            for (int t : tiles) {
                int x = t % W, z = t / W;
                uint8_t zt = f.zone[t];
                if (pass == 0 && zone != Z_NONE && zt != zone) continue;
                if (pass == 1 && zt == Z_HAZARD && zone != Z_HAZARD) continue;
                if (pass == 2 && (zone == Z_NONE || zt != Z_HAZARD)) continue; // only what pass 1 left out
                if (pass == 3 && zt == Z_HAZARD) continue;
                if (occ[t] != O_FREE) continue;
                if (capped(zt, fx.id, fx.variants)) continue;
                for (int rot = 0; rot < 4; ++rot) {
                    auto ft = foot(fx, x, z, rot);
                    if (ft.empty()) continue;
                    bool ok = true;
                    for (int u : ft) if (occ[u] != O_FREE) { ok = false; break; }
                    if (!ok) continue;
                    if (fx.support == "wall" && pass < 3) {
                        for (int u : ft) if (!wall(u % W - FX[rot], u / W - FZ[rot])) { ok = false; break; }
                        if (!ok) continue;
                    }
                    if (fx.blocks) {
                        int ax = x + fx.fd * FX[rot], az = z + fx.fd * FZ[rot];
                        if (!room(ax, az) || occ[(size_t)az * W + ax] == O_BLOCK) continue;
                    }
                    long long s;
                    if (mode == 0) s = -walld[t] * 16;
                    else if (mode == 1) s = walld[t] * 16;
                    else if (mode == 2) s = std::max(std::abs(x - target % W), std::abs(z - target / W)) * 16;
                    else s = 0;
                    // floor fixtures face the room: prefer the rotation whose front is deeper
                    int ax = x + FX[rot], az = z + FZ[rot];
                    if (room(ax, az)) s -= std::min(4, walld[(size_t)az * W + ax]) * 2;
                    s = s * 1024 + (long long)(hash_words({rng.seed, (uint64_t)t, (uint64_t)rot}) % 1024);
                    cs.push_back({s, x, z, rot});
                }
            }
            std::sort(cs.begin(), cs.end(), [](const Cand& a, const Cand& b) { return a.score < b.score; });
            int tries = 0;
            for (auto& c : cs) {
                if (++tries > 48) break;
                auto ft = foot(fx, c.x, c.z, c.rot);
                std::vector<std::pair<int, uint8_t>> undo;
                for (int u : ft) { undo.push_back({u, occ[u]}); occ[u] = fx.blocks ? O_BLOCK : O_PROP; }
                if (fx.blocks) {
                    int a = (c.z + fx.fd * FZ[c.rot]) * W + c.x + fx.fd * FX[c.rot];
                    if (occ[a] == O_FREE) { undo.push_back({a, occ[a]}); occ[a] = O_ACCESS; }
                    if (!connected()) { for (auto it = undo.rbegin(); it != undo.rend(); ++it) occ[it->first] = it->second; rd.blocked_rejects++; continue; }
                }
                if (pass >= 1) rd.zone_relaxed++;
                emit("fixture", fx.id, c.x, c.z, c.rot, source, fx.content, fx.variants);
                if (fx.blocks) anchors.push_back(c.z * W + c.x);
                return c.z * W + c.x;
            }
        }
        return -1;
    }

    // a floor prop: a free tile in the zone (or near target); never on a door band or access tile
    int place_prop(const PropDef& p, uint8_t zone, int target, const std::string& source) {
        int best = -1;
        long long bs = 0;
        for (int t : tiles) {
            if (occ[t] != O_FREE) continue;
            if (zone != Z_NONE && f.zone[t] != zone) continue;
            if (capped(f.zone[t], p.id, p.variants)) continue;
            long long s = target >= 0 ? std::max(std::abs(t % W - target % W), std::abs(t / W - target / W)) * 1024 : 0;
            s += (long long)(hash_words({rng.seed, (uint64_t)t, hash_str(p.id), (uint64_t)rd.items}) % 1024);
            if (best < 0 || s < bs) { best = t; bs = s; }
        }
        if (best < 0) return -1;
        if (target >= 0 && std::max(std::abs(best % W - target % W), std::abs(best / W - target / W)) > 2) return -1; // cluster, don't scatter
        occ[best] = O_PROP;
        emit("prop", p.id, best % W, best / W, 0, source, p.content, p.variants);
        return best;
    }

    int place_any(const std::string& id, uint8_t zone, int mode, int target, const std::string& source, bool relax) {
        if (auto* fx = lib.fixture(id)) return place_fixture(*fx, zone, mode, target, source, relax);
        if (auto* p = lib.prop(id)) {
            if (p->place == "on" && target >= 0) {
                if (capped(f.zone[target], p->id, p->variants)) return -1;
                emit("prop", p->id, target % W, target / W, 0, source, p->content, p->variants); return target; }
            if (p->place == "wall") return -1;
            int at = place_prop(*p, zone, target, source);
            if (at < 0 && relax && zone != Z_NONE) { at = place_prop(*p, Z_NONE, target, source); rd.zone_relaxed += at >= 0; }
            return at;
        }
        return -1;
    }

    double used_frac() const {
        int u = 0;
        for (int t : tiles) u += occ[t] != O_FREE;
        return tiles.empty() ? 1.0 : (double)u / tiles.size();
    }
};
} // namespace

void dress_floor(Floor& f, const std::vector<Program>& progs, const DressLib& lib, uint64_t seed) {
    f.dress.clear();
    f.room_dress.clear();
    f.zone.assign(f.tile.size(), Z_NONE);
    const int W = f.W;
    std::map<std::string, const Program*> by_id;
    for (auto& p : progs) by_id[p.id] = &p;
    std::vector<uint8_t> occ(f.tile.size(), O_FREE), in(f.tile.size(), 0);
    std::vector<int> walld(f.tile.size(), 0);
    for (auto& ro : f.rooms) {
        std::string pid = ro.program.substr(0, ro.program.find('!'));
        const Program* pg = by_id.count(pid) ? by_id[pid] : nullptr;
        RoomDress rd;
        rd.node = ro.node;
        rd.program = ro.program;
        rd.function = pg && !pg->function.empty() ? pg->function : pg && pg->theme != "any" ? pg->theme : "generic";
        if (!lib.function(rd.function)) rd.function = "generic";
        static const char* HALL[] = {"hall", "courtyard", "cave", "gallery", "corridor", "arena", "hub", "rotunda", "junction", "cistern", "chasm"};
        if (pg) for (auto& t : pg->tags) for (auto* h : HALL) if (t == h) rd.hall = true;
        const FunctionProgram* fn = lib.function(rd.function);
        RoomCtx R(f, lib, ro, rd, seed);
        for (auto& n : toml_strs(toml_get(fn->doc, "meta"), "never")) R.never.insert(n);

        // room floor tiles, openings, zones
        for (int z = ro.z0; z < ro.z1; ++z)
            for (int x = ro.x0; x < ro.x1; ++x) {
                size_t t = f.idx(x, z);
                if (f.tile[t] == FLOOR && f.owner[t] == ro.node) { R.tiles.push_back((int)t); in[t] = 1; }
            }
        R.in.swap(in);
        R.occ.swap(occ);
        R.walld.swap(walld);
        rd.floor_tiles = (int)R.tiles.size();
        for (int t : R.tiles) {
            int x = t % W, z = t / W;
            for (int k = 0; k < 4; ++k) {
                int nx = x + FX[k], nz = z + FZ[k];
                size_t n = f.idx(nx, nz);
                if (f.tile[n] == FLOOR && f.owner[n] != ro.node) { R.openings.push_back(t); break; }
            }
        }
        if (R.openings.empty()) R.openings.push_back((int)f.idx(ro.door_x, ro.door_z));
        // BFS steps from the openings (entry) and from the walls (salience)
        std::map<int, int> dop;
        {
            std::deque<int> q;
            for (int t : R.openings) { dop[t] = 0; q.push_back(t); }
            while (!q.empty()) {
                int c = q.front(); q.pop_front();
                for (int k = 0; k < 4; ++k) {
                    int nx = c % W + FX[k], nz = c / W + FZ[k];
                    if (!R.room(nx, nz)) continue;
                    int t = nz * W + nx;
                    if (dop.count(t)) continue;
                    dop[t] = dop[c] + 1; q.push_back(t);
                }
            }
            std::deque<int> q2;
            for (int t : R.tiles) {
                int x = t % W, z = t / W;
                bool edge = false;
                for (int dz = -1; dz <= 1 && !edge; ++dz)
                    for (int dx = -1; dx <= 1; ++dx) if (!R.room(x + dx, z + dz) && R.wall(x + dx, z + dz)) { edge = true; break; }
                R.walld[t] = edge ? 1 : 0;
                if (edge) q2.push_back(t);
            }
            while (!q2.empty()) {
                int c = q2.front(); q2.pop_front();
                for (int k = 0; k < 4; ++k) {
                    int nx = c % W + FX[k], nz = c / W + FZ[k];
                    if (!R.room(nx, nz)) continue;
                    int t = nz * W + nx;
                    if (R.walld[t]) continue;
                    R.walld[t] = R.walld[c] + 1; q2.push_back(t);
                }
            }
            for (int t : R.tiles) if (!R.walld[t]) R.walld[t] = 9; // pockets bounded by features only
        }
        bool any_hazard = false;
        for (int t : R.tiles) {
            int x = t % W, z = t / W;
            bool hz = f.feat[t] == F_HAZARD || f.feat[t] == F_WATER || f.feat[t] == F_BRIDGE;
            for (int k = 0; k < 4 && !hz; ++k) { size_t n = f.idx(x + FX[k], z + FZ[k]); hz = feat_blocks(f.feat[n]); }
            auto it = dop.find(t);
            uint8_t zn = hz ? Z_HAZARD : (it != dop.end() && it->second <= 2) ? Z_ENTRY : R.walld[t] <= 1 ? Z_EDGE : Z_CORE;
            f.zone[t] = zn;
            any_hazard |= zn == Z_HAZARD;
        }
        // content kinds the room already holds from its structure and its mission entity
        for (int t : R.tiles) {
            int x = t % W, z = t / W;
            for (int k = 0; k < 4; ++k) {
                uint8_t ft = f.feat[f.idx(x + FX[k], z + FZ[k])];
                if (ft == F_WATER || ft == F_DEEP) rd.kinds |= 1 << 2;
                if (ft == F_LAVA || ft == F_CHASM || ft == F_HAZARD) rd.kinds |= 1 << 3;
            }
            if (f.feat[t] == F_WATER) rd.kinds |= 1 << 2;
            if (f.feat[t] == F_HAZARD) rd.kinds |= 1 << 3;
        }
        // reserve door bands (the opening and its neighbours) and the mission entity's tile
        for (int t : R.openings) {
            R.occ[t] = O_RESERVE;
            for (int k = 0; k < 4; ++k) { int nx = t % W + FX[k], nz = t / W + FZ[k]; if (R.room(nx, nz)) R.occ[(size_t)nz * W + nx] = O_RESERVE; }
        }
        for (auto& e : f.entities) {
            if (e.x < ro.x0 || e.z < ro.z0 || e.x >= ro.x1 || e.z >= ro.z1) continue;
            size_t t = f.idx(e.x, e.z);
            if (R.room(e.x, e.z)) R.occ[t] = O_RESERVE;
            static const std::map<std::string, std::string> EK = {{"chest", "container"}, {"key", "container"}, {"secret", "secret"}, {"merchant", "npc"},
                                                                  {"map_plaque", "writing"}, {"faction_seat", "npc"}, {"boss", "den"}, {"lock_door", "writing"}};
            auto it = EK.find(e.kind);
            if (it != EK.end()) rd.kinds |= 1 << content_bit(it->second);
        }

        auto roll = [&](const std::string& bucket) { return (long long)R.rng.next(1000) < bucket_permille(bucket); };
        auto present = [&](const std::string& s) { rd.present.push_back(s); };
        // ACTIVITIES
        for (auto* a : toml_arr(fn->doc, "activity")) {
            std::string id = toml_str(a, "id"), bucket = toml_str(a, "bucket", "LIKELY");
            if (!roll(bucket)) continue;
            uint8_t zn = zone_parse(toml_str(a, "zone"));
            if (zn == Z_HAZARD && !any_hazard) { if (bucket == "GUARANTEED") rd.guaranteed_missed++; continue; }
            std::string anchor = toml_str(a, "anchor");
            const FixtureDef* fx = lib.fixture(anchor);
            int mode = toml_int(a, "focal", 0) ? 0 : (fx && fx->support == "wall") ? 1 : 3;
            int at = R.place_any(anchor, zn, mode, -1, "activity:" + id, true);
            if (at < 0) { if (bucket == "GUARANTEED") rd.guaranteed_missed++; continue; }
            present("activity:" + id);
            for (auto& m : toml_strs(a, "around")) R.place_any(m, Z_NONE, 2, at, "activity:" + id, false);
            for (auto& m : toml_strs(a, "on")) R.place_any(m, Z_NONE, 2, at, "activity:" + id, false);
        }
        // ITEMS by bucket
        for (auto* it : toml_arr(fn->doc, "item")) {
            std::string id = toml_str(it, "id"), bucket = toml_str(it, "bucket", "LIKELY");
            if (!roll(bucket)) continue;
            uint8_t zn = zone_parse(toml_str(it, "zone"));
            // a hazard item in a room without a hazard zone makes its own: it goes to the core and
            // its tile becomes hazard (a trap or spikes is the hazard), counted as zone_relaxed
            bool own_hazard = zn == Z_HAZARD && !any_hazard;
            if (own_hazard) zn = Z_CORE;
            int lo = (int)toml_int(it, "min", 1), hi = (int)toml_int(it, "max", lo);
            int n = lo + (int)R.rng.next((uint64_t)(hi - lo + 1));
            int got = 0, first = -1;
            for (int k = 0; k < n; ++k) {
                const FixtureDef* fx = lib.fixture(id);
                int at = R.place_any(id, zn, first >= 0 ? 2 : (fx && fx->support == "wall") ? 1 : 3, first, "item:" + id, true);
                if (at >= 0) {
                    ++got;
                    if (first < 0) first = at;
                    if (own_hazard) { f.zone[at] = Z_HAZARD; f.dress.back().zone = Z_HAZARD; rd.zone_relaxed++; }
                }
            }
            if (got) present("item:" + id);
            else if (bucket == "GUARANTEED") rd.guaranteed_missed++;
        }
        // VIGNETTES: story clutter, the first part sets the spot, the rest cluster within 2 tiles
        for (auto* v : toml_arr(fn->doc, "vignette")) {
            std::string id = toml_str(v, "id"), bucket = toml_str(v, "bucket", "RARE");
            if (!roll(bucket)) continue;
            auto parts = toml_strs(v, "parts");
            if (parts.empty()) continue;
            uint8_t zn = zone_parse(toml_str(v, "zone"));
            int at = R.place_any(parts[0], zn, 3, -1, "vignette:" + id, true);
            if (at < 0) continue;
            int placed = 1;
            for (size_t k = 1; k < parts.size(); ++k) placed += R.place_any(parts[k], Z_NONE, 2, at, "vignette:" + id, false) >= 0;
            if (placed * 2 >= (int)parts.size()) present("vignette:" + id);
        }
        // from here on (wall trim and clutter) the Clone Army cap holds
        R.cap = lib.clone_cap;
        // WALL dressing: one item per wall face at most, by share; wall fixtures already on a face count
        {
            auto walls = toml_arr(fn->doc, "wall");
            std::set<std::pair<int, int>> taken;
            for (auto& d : f.dress)
                if (d.room == ro.node && d.cls == "fixture") {
                    const FixtureDef* fx = lib.fixture(d.id);
                    if (fx && fx->support == "wall") taken.insert({(int)f.idx(d.x, d.z), (d.rot + 2) % 4});
                }
            for (int t : R.tiles) {
                int x = t % W, z = t / W;
                for (int k = 0; k < 4; ++k) {
                    if (!R.wall(x + FX[k], z + FZ[k]) || R.room(x + FX[k], z + FZ[k])) continue;
                    rd.wall_faces++;
                    if (taken.count({t, k})) { rd.wall_dressed++; continue; }
                    uint64_t hv = hash_words({R.rng.seed, (uint64_t)t, (uint64_t)k}) % 1000;
                    long long acc = 0;
                    // the face's pick by share; a pick at the Clone Army cap passes to the next entry
                    for (size_t wi = 0; wi < walls.size(); ++wi) {
                        acc += (long long)(toml_num(walls[wi], "share", 0) * lib.wall_scale * 1000);
                        if ((long long)hv >= acc) continue;
                        for (size_t j = 0; j < walls.size(); ++j) {
                            const PropDef* p = lib.prop(toml_str(walls[(wi + j) % walls.size()], "id"));
                            if (!p || R.capped(f.zone[t], p->id, p->variants)) continue;
                            R.emit("wall", p->id, x, z, (k + 2) % 4, "wall", p->content, p->variants);
                            rd.wall_dressed++;
                            break;
                        }
                        break;
                    }
                }
            }
        }
        // LIGHT: a room with no emissive gets one by lit_permille: a wall light on a free wall face
        // (any wall_scale), else a floor light; the rest stay dark on purpose (Lit, gameplay/20)
        {
            bool lit = false;
            for (auto& d : f.dress) lit |= d.room == ro.node && d.content == "light";
            for (int t : R.tiles) lit |= f.feat[t] == F_SKY;
            auto lights = toml_strs(toml_get(fn->doc, "meta"), "light");
            long long pm = toml_int(toml_get(fn->doc, "meta"), "lit_permille", 0);
            if (!lit && !lights.empty() && (long long)R.rng.next(1000) < pm) {
                bool done = false;
                for (auto& id : lights) {
                    const PropDef* p = lib.prop(id);
                    if (done || !p) continue;
                    if (p->place == "wall") {
                        if (lib.wall_scale <= 0) continue;
                        for (int t : R.tiles) {
                            int x = t % W, z = t / W;
                            for (int k = 0; k < 4 && !done; ++k)
                                if (R.wall(x + FX[k], z + FZ[k]) && !R.room(x + FX[k], z + FZ[k])) { R.emit("wall", p->id, x, z, (k + 2) % 4, "light", p->content, p->variants); done = true; }
                            if (done) break;
                        }
                    } else done = R.place_any(id, Z_NONE, 3, -1, "light", true) >= 0;
                }
            }
        }
        // CLUTTER: clustered beside placed fixtures until free floor <= 45 percent (rooms; halls keep
        // the void and stop at density 3 per 10 m^2), never below 30 percent free
        {
            auto clutter = toml_strs(toml_get(fn->doc, "meta"), "clutter");
            auto density = [&]() { return rd.floor_tiles ? rd.items * 10.0 / (rd.floor_tiles * lib.m2_per_tile) : 0; };
            int fails = 0;
            for (int guard = 0; guard < 400 && !clutter.empty() && fails < 24; ++guard) {
                bool want = rd.hall ? density() < 3.0 : (1.0 - R.used_frac() > 0.45 || density() < 3.0);
                if (!want) break;
                if (R.used_frac() >= 0.70 - 1e-9) break;
                // a fourth identical item in a zone takes another variant; with every variant at 3 the
                // next clutter kind is tried; a pick lands beside a placed fixture (cluster, don't scatter)
                size_t k0 = R.rng.next(clutter.size());
                int target = R.anchors.empty() ? -1 : R.anchors[R.rng.next(R.anchors.size())];
                int at = -1;
                for (size_t j = 0; j < clutter.size() && at < 0; ++j)
                    at = R.place_any(clutter[(k0 + j) % clutter.size()], Z_NONE, target >= 0 ? 2 : 3, target, "clutter", false);
                if (at < 0) ++fails; else fails = 0;
            }
        }
        // the room's soft measurements
        rd.used_tiles = 0;
        for (int t : R.tiles) rd.used_tiles += R.occ[t] != O_FREE;
        rd.neg_space = rd.floor_tiles ? 100.0 * (rd.floor_tiles - rd.used_tiles) / rd.floor_tiles : 0;
        rd.density = rd.floor_tiles ? rd.items * 10.0 / (rd.floor_tiles * lib.m2_per_tile) : 0;
        for (auto& c : R.clone) {
            if (c.second <= 3) continue;
            const PropDef* pp = lib.prop(std::get<1>(c.first));
            if (pp && pp->place == "wall") rd.clone_wall++; else rd.clone_groups++;
        }
        // hand the scratch arrays back cleared for the next room
        for (int t : R.tiles) { R.occ[t] = O_FREE; R.in[t] = 0; R.walld[t] = 0; }
        for (int t : R.openings) R.occ[t] = O_FREE;
        occ.swap(R.occ);
        in.swap(R.in);
        walld.swap(R.walld);
        f.room_dress.push_back(rd);
    }
    f.dress_hash = dress_hash(f);
}

int dress_check(const Floor& f, const DressLib& lib) {
    const int W = f.W;
    int bad = 0;
    std::vector<uint8_t> block(f.tile.size(), 0);
    bool overlap = false;
    for (auto& d : f.dress) {
        if (d.cls != "fixture") continue;
        const FixtureDef* fx = lib.fixture(d.id);
        if (!fx || !fx->blocks) continue;
        int rx = FZ[d.rot], rz = -FX[d.rot];
        for (int a = 0; a < fx->fw; ++a)
            for (int b = 0; b < fx->fd; ++b) {
                size_t t = f.idx(d.x + a * rx + b * FX[d.rot], d.z + a * rz + b * FZ[d.rot]);
                if (block[t]) overlap = true;
                block[t] = 1;
            }
    }
    for (auto& ro : f.rooms) {
        std::vector<int> tiles;
        int start = -1;
        bool fail = overlap;
        for (int z = ro.z0; z < ro.z1; ++z)
            for (int x = ro.x0; x < ro.x1; ++x) {
                size_t t = f.idx(x, z);
                if (f.tile[t] != FLOOR || f.owner[t] != ro.node) { if (block[t]) fail = true; continue; } // a fixture off the floor
                if (block[t]) {
                    for (int k = 0; k < 4; ++k) { size_t n = f.idx(x + FX[k], z + FZ[k]); if (f.tile[n] == FLOOR && f.owner[n] != ro.node) fail = true; } // on an opening
                    continue;
                }
                tiles.push_back((int)t);
                if (start < 0) start = (int)t;
            }
        if (start >= 0) {
            std::set<int> seen{start};
            std::deque<int> q{start};
            while (!q.empty()) {
                int c = q.front(); q.pop_front();
                for (int k = 0; k < 4; ++k) {
                    int nx = c % W + FX[k], nz = c / W + FZ[k];
                    if (nx < ro.x0 || nz < ro.z0 || nx >= ro.x1 || nz >= ro.z1) continue;
                    int t = nz * W + nx;
                    if (f.tile[t] != FLOOR || f.owner[t] != ro.node || block[t] || seen.count(t)) continue;
                    seen.insert(t); q.push_back(t);
                }
            }
            if (seen.size() != tiles.size()) fail = true;
        }
        bad += fail;
    }
    return bad;
}

uint64_t dress_hash(const Floor& f) {
    uint64_t h = 1469598103934665603ull;
    auto byte = [&](uint8_t b) { h ^= b; h *= 1099511628211ull; };
    auto word = [&](uint64_t v) { for (int i = 0; i < 8; ++i) byte((uint8_t)(v >> (8 * i))); };
    for (auto& d : f.dress) {
        for (char c : d.id) byte((uint8_t)c);
        word((uint64_t)d.variant); word((uint64_t)d.x); word((uint64_t)d.z); word((uint64_t)d.rot); byte(d.zone);
    }
    return h;
}

} // namespace apx
