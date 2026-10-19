// apxgen: runs N seeds, prints one JSONL row per floor and a summary row; exit 0 only when the W2
// gates hold (connectivity 100 percent, locks after keys, dig gate) on every seed and the DRESS
// pass keeps its own hard rules (no GUARANTEED miss, no NEVER item, determinism).
//   apxgen [--seeds N] [--first S] [--size T] [--grammar F] [--kits D1;D2] [--no-hard-walls]
//          [--theme T] [--no-dress] [--no-clone-cap] [--strict-soft] [--presence out.json] [--dump SEED out.txt] [--vox DIR] [--quiet]
#include "apxgen/apxgen.hpp"
#include <algorithm>
#include <bitset>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace apx;

static std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o;
}

// per-floor DRESS measurements (gameplay/20 section 3, lattice/gen/33 soft terms)
struct DressSum {
    int clone_wall = 0, rooms = 0, items = 0, fixtures = 0, wall_items = 0, floor_tiles = 0, filled = 0, three = 0, clone = 0, gmiss = 0, never = 0;
    int rooms_dense = 0, nrooms = 0, nrooms_band = 0, wall_faces = 0, wall_dressed = 0, relaxed = 0, rejects = 0;
    double ns_rooms = 0, ns_halls = 0, density = 0, density_nowall = 0;
    int nhalls = 0;
};
static DressSum sum_dress(const Floor& f) {
    DressSum s;
    for (auto& rd : f.room_dress) {
        ++s.rooms;
        s.items += rd.items; s.fixtures += rd.fixtures; s.floor_tiles += rd.floor_tiles;
        int kinds = (int)std::bitset<9>(rd.kinds).count();
        s.filled += kinds >= 1; s.three += kinds >= 3;
        s.clone += rd.clone_groups; s.clone_wall += rd.clone_wall; s.gmiss += rd.guaranteed_missed; s.never += rd.never_hits;
        s.rooms_dense += rd.density >= 3.0;
        s.wall_faces += rd.wall_faces; s.wall_dressed += rd.wall_dressed;
        s.relaxed += rd.zone_relaxed; s.rejects += rd.blocked_rejects;
        if (rd.hall) { ++s.nhalls; s.ns_halls += rd.neg_space; }
        else { ++s.nrooms; s.ns_rooms += rd.neg_space; s.nrooms_band += rd.neg_space >= 30.0 - 1e-9 && rd.neg_space <= 45.0 + 1e-9; }
    }
    for (auto& d : f.dress) s.wall_items += d.cls == "wall";
    s.density = s.floor_tiles ? s.items * 10.0 / s.floor_tiles : 0;
    s.density_nowall = s.floor_tiles ? (s.items - s.wall_items) * 10.0 / s.floor_tiles : 0;
    if (s.nrooms) s.ns_rooms /= s.nrooms;
    if (s.nhalls) s.ns_halls /= s.nhalls;
    return s;
}

static char fixture_glyph(const std::string& id) {
    static const std::map<std::string, char> G = {{"table", 'T'}, {"bench", 'b'}, {"shelf", 'S'}, {"barrel", 'o'}, {"crate", 'c'}, {"brazier", '*'},
                                                  {"altar", 'A'}, {"bed", 'B'}, {"rack", 'R'}, {"pillar", 'I'}, {"statue", '&'}, {"rubble", '%'}};
    auto it = G.find(id);
    return it == G.end() ? 'x' : it->second;
}

static int vox_gate(const DressLib& lib, const std::string& dir) {
    std::filesystem::create_directories(dir);
    int bad = 0, files = 0;
    for (auto& fx : lib.fixtures) {
        for (int v = 0; v < fx.variants; ++v) {
            Vox a, b;
            std::string err;
            if (!build_vox(fx, v, a, err)) { std::printf("{\"vox\":\"%s\",\"variant\":%d,\"error\":\"%s\"}\n", fx.id.c_str(), v, esc(err).c_str()); ++bad; continue; }
            std::string path = dir + "/" + fx.id + (v ? "_v" + std::to_string(v) : std::string()) + ".vox";
            if (!write_vox(path, a)) { std::printf("{\"vox\":\"%s\",\"error\":\"write failed\"}\n", path.c_str()); ++bad; continue; }
            bool ok = read_vox(path, b, err);
            bool same = ok && a.sx == b.sx && a.sy == b.sy && a.sz == b.sz && a.count() == b.count() && a.data == b.data && std::memcmp(a.pal, b.pal, 768) == 0;
            auto bytes = std::filesystem::file_size(path);
            bool size_ok = bytes == (uintmax_t)(12 + a.data.size() + 768);
            std::printf("{\"vox\":\"%s\",\"variant\":%d,\"dims\":[%d,%d,%d],\"voxels\":%d,\"read_dims\":[%d,%d,%d],\"read_voxels\":%d,\"bytes\":%llu,\"roundtrip\":%s}\n",
                        fx.id.c_str(), v, a.sx, a.sy, a.sz, a.count(), b.sx, b.sy, b.sz, ok ? b.count() : -1, (unsigned long long)bytes,
                        same && size_ok ? "true" : "false");
            bad += !(same && size_ok);
            ++files;
        }
    }
    std::printf("{\"vox_summary\":true,\"fixtures\":%zu,\"files\":%d,\"failed\":%d,\"gate\":\"%s\"}\n", lib.fixtures.size(), files, bad, bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}

int main(int argc, char** argv) {
    long long seeds = 1000, first = 1, dump_seed = -1;
    std::string grammar = APXGEN_ROOT "/grammar/default.grammar", dump_path, vox_dir, presence_path;
    std::string kits = APXGEN_ROOT "/../Apockalypsis/kits/rooms;" APXGEN_ROOT "/kits/rooms";
    Params P;
    bool quiet = false, dress = true, clone_cap = true, strict_soft = false; // strict-soft: Clone Army 0 is a gate
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto nxt = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--seeds") seeds = std::stoll(nxt());
        else if (a == "--first") first = std::stoll(nxt());
        else if (a == "--size") P.size = std::stoi(nxt());
        else if (a == "--grammar") grammar = nxt();
        else if (a == "--kits") kits = nxt();
        else if (a == "--no-hard-walls") P.hard_walls = false;
        else if (a == "--quiet") quiet = true;
        else if (a == "--theme") P.theme = nxt();
        else if (a == "--no-dress") dress = false;
        else if (a == "--no-clone-cap") clone_cap = false;
        else if (a == "--strict-soft") strict_soft = true;
        else if (a == "--no-special-cells") P.special_cells = false;
        else if (a == "--band-order-x") P.band_order_x = true;
        else if (a == "--presence") presence_path = nxt();
        else if (a == "--vox") vox_dir = nxt();
        else if (a == "--dump") { dump_seed = std::stoll(nxt()); dump_path = nxt(); }
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    Grammar g;
    std::string err;
    if (!load_grammar(grammar, g, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    std::vector<std::string> dirs;
    { std::stringstream ss(kits); std::string d; while (std::getline(ss, d, ';')) if (!d.empty()) dirs.push_back(d); }
    std::vector<Program> progs;
    if (!load_programs(dirs, progs, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    if (!load_districts(APXGEN_ROOT "/kits/districts.toml", P, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    DressLib lib;
    if (!load_dress(APXGEN_ROOT "/kits", lib, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    lib.clone_cap = clone_cap;
    if (!vox_dir.empty()) return vox_gate(lib, vox_dir);

    if (dump_seed >= 0) {
        Result r = generate((uint64_t)dump_seed, g, progs, P);
        if (r.ok && dress) dress_floor(r.floor, progs, lib, (uint64_t)dump_seed);
        std::ofstream o(dump_path);
        const Floor& f = r.floor;
        DressSum ds = sum_dress(f);
        o << "seed " << dump_seed << " ok " << r.ok << " theme " << f.theme << " hash " << std::hex << f.hash << " dress_hash " << f.dress_hash << std::dec
          << " why " << r.verdict.why << "\n";
        o << "summary rooms " << ds.rooms << " items " << ds.items << " fixtures " << ds.fixtures << " wall_items " << ds.wall_items
          << " density_per_10m2 " << ds.density << " clone_groups " << ds.clone << " clone_wall_groups " << ds.clone_wall << " neg_space_rooms " << ds.ns_rooms << " neg_space_halls " << ds.ns_halls
          << " filled " << ds.filled << "/" << ds.rooms << " three_kinds " << ds.three << "/" << ds.rooms << "\n";
        o << "legend floor . | behind-lock 1-9 | HARD # | water ~ deep = lava ^ chasm : sky \" bridge + hazard ! (floor tag) | stairs < > key k lock L boss @ chest $ secret ? merchant m plaque p seat f\n";
        o << "legend fixtures table T bench b shelf S barrel o crate c brazier * altar A bed B rack R pillar I statue & rubble % | floor prop , | wall items counted, not drawn\n";
        if (f.tile.size() != (size_t)f.W * f.H) { // the last attempt dead-ended in LAYOUT: no grid to draw
            for (auto& l : r.log) o << "log " << l << "\n";
            std::printf("seed %lld failed in layout; log written to %s\n", dump_seed, dump_path.c_str());
            return 1;
        }
        std::vector<std::string> rows(f.H, std::string(f.W, ' '));
        for (int z = 0; z < f.H; ++z)
            for (int x = 0; x < f.W; ++x) {
                size_t t = f.idx(x, z);
                uint8_t ti = f.tile[t];
                char c = ti == FLOOR ? (f.behind[t] ? (char)('0' + std::min(9, (int)std::bitset<32>(f.behind[t]).count())) : '.') : ti == HARD ? '#' : ' ';
                static const char FC[] = {0, '~', '=', '^', ':', '"', '+', 0};
                if (!f.feat.empty() && f.feat[t] && FC[f.feat[t]]) c = FC[f.feat[t]];
                rows[z][x] = c;
            }
        for (auto& d : f.dress) {
            if (d.cls == "fixture") {
                const FixtureDef* fx = lib.fixture(d.id);
                const int FX[4] = {0, 1, 0, -1}, FZ[4] = {1, 0, -1, 0};
                int rx = FZ[d.rot], rz = -FX[d.rot];
                for (int a = 0; a < (fx ? fx->fw : 1); ++a)
                    for (int b = 0; b < (fx ? fx->fd : 1); ++b) rows[d.z + a * rz + b * FZ[d.rot]][d.x + a * rx + b * FX[d.rot]] = fixture_glyph(d.id);
            } else if (d.cls == "prop" && rows[d.z][d.x] != 'T' && !std::isalpha((unsigned char)rows[d.z][d.x])) rows[d.z][d.x] = ',';
        }
        static const std::map<std::string, char> EG = {{"stairs_up", '<'}, {"stairs_down", '>'}, {"key", 'k'}, {"lock_door", 'L'}, {"boss", '@'}, {"chest", '$'},
                                                       {"secret", '?'}, {"merchant", 'm'}, {"map_plaque", 'p'}, {"faction_seat", 'f'}};
        for (auto& e : f.entities) rows[e.z][e.x] = EG.count(e.kind) ? EG.at(e.kind) : 'E';
        for (auto& s : rows) o << s << "\n";
        for (auto& ro : f.rooms) o << "room " << ro.node << " " << f.mission.nodes[ro.node].name << " " << kind_name(f.mission.nodes[ro.node].kind) << " " << ro.program
                                   << " district " << (ro.district.empty() ? "-" : ro.district) << " [" << ro.x0 << "," << ro.z0 << ")-(" << ro.x1 << "," << ro.z1 << ") door " << ro.door_x << "," << ro.door_z << "\n";
        for (auto& rd : f.room_dress)
            o << "roomdress " << rd.node << " " << rd.function << " hall " << rd.hall << " floor " << rd.floor_tiles << " items " << rd.items << " density " << rd.density
              << " neg_space " << rd.neg_space << " kinds " << std::bitset<9>(rd.kinds).count() << " clone " << rd.clone_groups << " wall " << rd.wall_dressed << "/" << rd.wall_faces
              << " gmiss " << rd.guaranteed_missed << "\n";
        for (auto& e : f.entities) o << "entity " << e.kind << " " << e.id << " " << e.x << "," << e.z << "\n";
        for (auto& d : f.dress) o << "dress " << d.cls << " " << d.id << " " << d.variant << " " << d.x << " " << d.z << " " << d.rot << " " << zone_name(d.zone) << " " << d.room << " " << d.source << " " << (d.content.empty() ? "-" : d.content) << "\n";
        for (int z = 0; z < f.H; ++z) {
            o << "zone ";
            for (int x = 0; x < f.W; ++x) o << (char)('0' + (f.zone.empty() ? 0 : f.zone[f.idx(x, z)]));
            o << "\n";
        }
        for (auto& l : r.log) o << "log " << l << "\n";
        std::printf("dumped seed %lld (%s) to %s (ok=%d)\n", dump_seed, f.theme.c_str(), dump_path.c_str(), (int)r.ok);
        return r.ok ? 0 : 1;
    }

    long long ok = 0, conn = 0, locks = 0, dig = 0, reach = 0, mission = 0, retried = 0, nondet = 0, fallbacks = 0;
    long long all5 = 0, sp_missed = 0, sp_swaps = 0, sp_merges = 0, boss_small = 0; int boss_min_side = 1 << 30;
    std::map<std::string, std::pair<double, std::pair<long long, long long>>> ns_by_fn; // function -> (sum ns, (rooms, in band))
    long long d_clearance = 0, d_clone_wall = 0, d_gmiss = 0, d_never = 0, d_clone = 0, d_rooms = 0, d_filled = 0, d_three = 0, d_floor_dense = 0, d_floor_filled = 0, d_floor_three = 0;
    long long d_nrooms = 0, d_band = 0, d_wall_faces = 0, d_wall_dressed = 0, d_clone_floors = 0, d_special = 0, d_themed = 0, d_district = 0;
    double d_density = 0, d_ns_rooms = 0, d_ns_halls = 0, d_dense_min = 1e9;
    std::map<std::string, std::pair<long long, long long>> pres; // function|entry -> (rooms with it, rooms of the function)
    std::map<std::string, long long> fn_rooms;
    std::vector<double> ms, dms;
    double rooms_sum = 0, open_sum = 0;
    for (long long s = first; s < first + seeds; ++s) {
        auto t0 = std::chrono::steady_clock::now();
        Result r = generate((uint64_t)s, g, progs, P);
        double dt = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ms.push_back(dt);
        Floor& f = r.floor;
        const Verdict& v = r.verdict;
        double ddt = 0;
        if (r.ok && dress) {
            auto t1 = std::chrono::steady_clock::now();
            dress_floor(f, progs, lib, (uint64_t)s);
            ddt = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
            dms.push_back(ddt);
        }
        // W5 spot check: the first 50 seeds are generated (and dressed) twice and must hash the same
        if (s < first + 50) {
            Result r2 = generate((uint64_t)s, g, progs, P);
            if (r2.ok && dress) dress_floor(r2.floor, progs, lib, (uint64_t)s);
            if (r2.floor.hash != f.hash || r2.floor.dress_hash != f.dress_hash) ++nondet;
        }
        ok += r.ok; conn += r.ok && v.connected; locks += r.ok && v.locks_ok; dig += r.ok && v.dig_ok; reach += r.ok && v.reach_ok; mission += r.ok && v.mission_ok;
        retried += f.attempt > 0;
        fallbacks += f.program_fallbacks;
        if (r.ok) {
            all5 += f.special_kinds == 31u && f.special_missed == 0;
            sp_missed += f.special_missed; sp_swaps += f.special_swaps; sp_merges += f.special_merges;
            boss_min_side = std::min(boss_min_side, std::min(f.boss_w, f.boss_d));
            boss_small += std::min(f.boss_w, f.boss_d) < P.boss_min;
        }
        d_special += f.special_rooms; d_themed += f.themed_rooms; d_district += f.district_rooms;
        double open_pct = f.W ? 100.0 * v.floor_tiles / (f.W * f.H) : 0;
        rooms_sum += f.rooms.size();
        open_sum += open_pct;
        DressSum ds = sum_dress(f);
        if (r.ok && dress) {
            d_clearance += dress_check(f, lib);
            d_clone_wall += ds.clone_wall; d_gmiss += ds.gmiss; d_never += ds.never; d_clone += ds.clone; d_clone_floors += ds.clone > 0;
            d_rooms += ds.rooms; d_filled += ds.filled; d_three += ds.three;
            d_floor_dense += ds.density >= 3.0; d_floor_filled += ds.filled == ds.rooms; d_floor_three += ds.three * 10 >= ds.rooms * 7;
            d_density += ds.density; d_dense_min = std::min(d_dense_min, ds.density);
            d_ns_rooms += ds.ns_rooms * ds.nrooms; d_nrooms += ds.nrooms; d_band += ds.nrooms_band; d_ns_halls += ds.ns_halls * ds.nhalls;
            d_wall_faces += ds.wall_faces; d_wall_dressed += ds.wall_dressed;
            for (auto& rd : f.room_dress) {
                fn_rooms[rd.function]++;
                auto& nb = ns_by_fn[rd.function + (rd.hall ? "/hall" : "/room")];
                nb.first += rd.neg_space; nb.second.first++; nb.second.second += rd.neg_space >= 30.0 - 1e-9 && rd.neg_space <= 45.0 + 1e-9;
                std::set<std::string> seen(rd.present.begin(), rd.present.end());
                for (auto& e : seen) pres[rd.function + "|" + e].first++;
            }
        }
        if (!quiet) {
            std::printf("{\"seed\":%lld,\"ok\":%s,\"ms\":%.3f,\"dress_ms\":%.3f,\"attempt\":%d,\"theme\":\"%s\",\"size\":%d,\"nodes\":%zu,\"edges\":%zu,\"cycles\":%d,\"rooms\":%zu,\"cells\":%d,"
                        "\"special_rooms\":%d,\"district_rooms\":%d,\"themed_rooms\":%d,\"program_fallbacks\":%d,"
                        "\"floor_tiles\":%d,\"open_pct\":%.1f,\"corridor_tiles\":%d,\"width_1_2_3\":[%d,%d,%d],\"narrowed\":%d,\"loops\":%d,\"components_before_repair\":%d,"
                        "\"repairs\":%d,\"island_repairs\":%d,\"components\":%d,\"connected\":%s,\"reach\":%s,\"locks\":%s,\"dig\":%s,\"mission\":%s,"
                        "\"items\":%d,\"fixtures\":%d,\"wall_items\":%d,\"density_per_10m2\":%.2f,\"density_no_wall\":%.2f,\"rooms_dense\":%d,\"filled\":%d,\"three_kinds\":%d,"
                        "\"clone_army_groups\":%d,\"clone_army_wall_groups\":%d,\"neg_space_rooms\":%.1f,\"rooms_in_30_45\":%d,\"non_hall_rooms\":%d,\"neg_space_halls\":%.1f,\"wall_cover_pct\":%.1f,"
                        "\"guaranteed_missed\":%d,\"never_hits\":%d,\"zone_relaxed\":%d,\"clearance_rejects\":%d,"
                        "\"hash\":\"%016llx\",\"dress_hash\":\"%016llx\",\"why\":\"%s\",\"log\":%zu}\n",
                        s, r.ok ? "true" : "false", dt, ddt, f.attempt, f.theme.c_str(), f.W, f.mission.nodes.size(), f.mission.edges.size(), f.mission.cycles(), f.rooms.size(), f.cells,
                        f.special_rooms, f.district_rooms, f.themed_rooms, f.program_fallbacks,
                        v.floor_tiles, open_pct, f.corridor_tiles, f.width_hist[1], f.width_hist[2], f.width_hist[3], f.narrowed, f.loops, f.components_before_repair,
                        f.repairs, f.island_repairs, v.components, v.connected ? "true" : "false", v.reach_ok ? "true" : "false",
                        v.locks_ok ? "true" : "false", v.dig_ok ? "true" : "false", v.mission_ok ? "true" : "false",
                        ds.items, ds.fixtures, ds.wall_items, ds.density, ds.density_nowall, ds.rooms_dense, ds.filled, ds.three,
                        ds.clone, ds.clone_wall, ds.ns_rooms, ds.nrooms_band, ds.nrooms, ds.ns_halls, ds.wall_faces ? 100.0 * ds.wall_dressed / ds.wall_faces : 0.0,
                        ds.gmiss, ds.never, ds.relaxed, ds.rejects,
                        (unsigned long long)f.hash, (unsigned long long)f.dress_hash,
                        esc(r.ok ? "" : (r.log.empty() ? v.why : r.log.back())).c_str(), r.log.size());
        }
    }
    // presence buckets across seeds (33: GUARANTEED 100, LIKELY over 60, RARE under 15, NEVER 0 percent)
    long long pres_bad = 0, pres_rows = 0;
    std::string pres_json = "[";
    for (auto& fnp : lib.functions) {
        auto rooms_of = fn_rooms.count(fnp.id) ? fn_rooms[fnp.id] : 0;
        auto row = [&](const std::string& entry, const std::string& bucket) {
            long long n = pres.count(fnp.id + "|" + entry) ? pres[fnp.id + "|" + entry].first : 0;
            double pct = rooms_of ? 100.0 * n / rooms_of : 0;
            // GUARANTEED is gated at 100; the probabilistic buckets are reported against their band
            bool okb = bucket == "GUARANTEED" ? (rooms_of == 0 || n == rooms_of) : bucket == "LIKELY" ? pct > 60 : bucket == "RARE" ? pct < 15 : n == 0;
            if (rooms_of) { ++pres_rows; pres_bad += !okb; }
            char buf[512];
            std::snprintf(buf, sizeof buf, "%s{\"function\":\"%s\",\"entry\":\"%s\",\"bucket\":\"%s\",\"rooms\":%lld,\"present\":%lld,\"pct\":%.1f,\"in_band\":%s}",
                          pres_json.size() > 1 ? "," : "", fnp.id.c_str(), entry.c_str(), bucket.c_str(), rooms_of, n, pct, okb ? "true" : "false");
            pres_json += buf;
        };
        for (auto* a : toml_arr(fnp.doc, "activity")) row("activity:" + toml_str(a, "id"), toml_str(a, "bucket", "LIKELY"));
        for (auto* a : toml_arr(fnp.doc, "item")) row("item:" + toml_str(a, "id"), toml_str(a, "bucket", "LIKELY"));
        for (auto* a : toml_arr(fnp.doc, "vignette")) row("vignette:" + toml_str(a, "id"), toml_str(a, "bucket", "RARE"));
    }
    pres_json += "]";
    if (!presence_path.empty()) { std::ofstream po(presence_path); po << pres_json << "\n"; }

    std::vector<double> sm = ms, sd = dms;
    std::sort(sm.begin(), sm.end());
    std::sort(sd.begin(), sd.end());
    double mean = 0, dmean = 0;
    for (double x : ms) mean += x;
    for (double x : dms) dmean += x;
    mean /= std::max<size_t>(1, ms.size());
    dmean /= std::max<size_t>(1, dms.size());
    auto pct = [&](const std::vector<double>& v, double p) { return v.empty() ? 0.0 : v[std::min(v.size() - 1, (size_t)(p * v.size()))]; };
    bool gate = all5 == ok && ok == seeds && conn == seeds && locks == seeds && dig == seeds && nondet == 0 && (!dress || (d_gmiss == 0 && d_never == 0 && d_clearance == 0 && (!strict_soft || d_clone == 0)));
    long long dfl = std::max<long long>(1, ok);
    std::printf("{\"summary\":true,\"grammar\":\"%s\",\"programs\":%zu,\"hard_walls\":%s,\"size\":%d,\"seeds\":%lld,\"pass\":%lld,\"connected\":%lld,\"reach\":%lld,"
                "\"locks_after_keys\":%lld,\"dig_gate\":%lld,\"mission\":%lld,\"retried\":%lld,\"nondeterministic\":%lld,\"program_fallbacks\":%lld,\"rooms_mean\":%.1f,\"open_pct_mean\":%.1f,"
                "\"floors_all_5_specials\":%lld,\"special_nodes_missed\":%lld,\"special_swaps\":%lld,\"special_merges\":%lld,\"boss_min_side\":%d,\"floors_boss_under_min\":%lld,\"special_rooms\":%lld,\"district_rooms\":%lld,\"themed_rooms\":%lld,"
                "\"dress\":%s,\"density_mean\":%.2f,\"density_min\":%.2f,\"floors_density_ge3\":%lld,\"rooms_filled_pct\":%.1f,\"rooms_three_kinds_pct\":%.1f,"
                "\"floors_all_filled\":%lld,\"floors_three_kinds_ge70\":%lld,\"clone_army_groups\":%lld,\"floors_with_clone_army\":%lld,\"clone_army_wall_groups\":%lld,"
                "\"neg_space_rooms_mean\":%.1f,\"rooms_in_30_45_pct\":%.1f,\"neg_space_halls_mean\":%.1f,\"wall_cover_pct\":%.1f,"
                "\"clone_cap\":%s,\"clearance_violations\":%lld,\"guaranteed_missed\":%lld,\"never_hits\":%lld,\"presence_rows\":%lld,\"presence_out_of_band\":%lld,"
                "\"ms_mean\":%.3f,\"ms_p50\":%.3f,\"ms_p99\":%.3f,\"ms_max\":%.3f,\"dress_ms_mean\":%.3f,\"dress_ms_p99\":%.3f,\"gate\":\"%s\"}\n",
                esc(grammar).c_str(), progs.size(), P.hard_walls ? "true" : "false", P.size, seeds, ok, conn, reach, locks, dig, mission, retried, nondet, fallbacks,
                rooms_sum / std::max<long long>(1, seeds), open_sum / std::max<long long>(1, seeds), all5, sp_missed, sp_swaps, sp_merges, boss_min_side, boss_small, d_special, d_district, d_themed,
                dress ? "true" : "false", d_density / dfl, d_dense_min > 1e8 ? 0.0 : d_dense_min, d_floor_dense, d_rooms ? 100.0 * d_filled / d_rooms : 0.0,
                d_rooms ? 100.0 * d_three / d_rooms : 0.0, d_floor_filled, d_floor_three, d_clone, d_clone_floors, d_clone_wall,
                d_nrooms ? d_ns_rooms / d_nrooms : 0.0, d_nrooms ? 100.0 * d_band / d_nrooms : 0.0, (d_rooms - d_nrooms) ? d_ns_halls / (d_rooms - d_nrooms) : 0.0,
                d_wall_faces ? 100.0 * d_wall_dressed / d_wall_faces : 0.0, clone_cap ? "true" : "false", d_clearance, d_gmiss, d_never, pres_rows, pres_bad,
                mean, pct(sm, 0.5), pct(sm, 0.99), sm.empty() ? 0 : sm.back(), dmean, pct(sd, 0.99), gate ? "PASS" : "FAIL");
    if (!quiet || !presence_path.empty()) {
        for (auto& [k, v] : ns_by_fn)
            std::printf("{\"neg_space_by_type\":\"%s\",\"rooms\":%lld,\"mean\":%.1f,\"in_30_45_pct\":%.1f}\n", k.c_str(), v.second.first,
                        v.first / std::max<long long>(1, v.second.first), 100.0 * v.second.second / std::max<long long>(1, v.second.first));
    }
    return gate ? 0 : 1;
}
