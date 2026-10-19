// apxgen: runs N seeds, prints one JSONL row per floor and a summary row; exit 0 only when the W2
// gates hold (connectivity 100 percent, locks after keys, dig gate) on every seed.
//   apxgen [--seeds N] [--first S] [--size T] [--grammar F] [--kits D1;D2] [--no-hard-walls]
//          [--dump SEED out.txt] [--quiet]
#include "apxgen/apxgen.hpp"
#include <algorithm>
#include <bitset>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace apx;

static std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o;
}

int main(int argc, char** argv) {
    long long seeds = 1000, first = 1, dump_seed = -1;
    std::string grammar = APXGEN_ROOT "/grammar/default.grammar", dump_path;
    std::string kits = APXGEN_ROOT "/../Apockalypsis/kits/rooms;" APXGEN_ROOT "/kits/rooms";
    Params P;
    bool quiet = false;
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

    if (dump_seed >= 0) {
        Result r = generate((uint64_t)dump_seed, g, progs, P);
        std::ofstream o(dump_path);
        const Floor& f = r.floor;
        o << "seed " << dump_seed << " ok " << r.ok << " hash " << std::hex << f.hash << std::dec << " why " << r.verdict.why << "\n";
        std::vector<std::string> rows(f.H, std::string(f.W, ' '));
        for (int z = 0; z < f.H; ++z)
            for (int x = 0; x < f.W; ++x) {
                uint8_t t = f.tile[f.idx(x, z)];
                rows[z][x] = t == FLOOR ? (f.behind[f.idx(x, z)] ? (char)('0' + std::min(9, (int)std::bitset<32>(f.behind[f.idx(x, z)]).count())) : '.') : t == HARD ? '#' : ' ';
            }
        for (auto& e : f.entities) rows[e.z][e.x] = e.kind[0] == 's' && e.kind == "stairs_up" ? '<' : e.kind == "stairs_down" ? '>' : (char)std::toupper((unsigned char)e.kind[0]);
        for (auto& s : rows) o << s << "\n";
        for (auto& ro : f.rooms) o << "room " << ro.node << " " << f.mission.nodes[ro.node].name << " " << kind_name(f.mission.nodes[ro.node].kind) << " " << ro.program << " [" << ro.x0 << "," << ro.z0 << ")-(" << ro.x1 << "," << ro.z1 << ") door " << ro.door_x << "," << ro.door_z << "\n";
        for (auto& e : f.entities) o << "entity " << e.kind << " " << e.id << " " << e.x << "," << e.z << "\n";
        for (auto& l : r.log) o << "log " << l << "\n";
        std::printf("dumped seed %lld to %s (ok=%d)\n", dump_seed, dump_path.c_str(), (int)r.ok);
        return r.ok ? 0 : 1;
    }

    long long ok = 0, conn = 0, locks = 0, dig = 0, reach = 0, mission = 0, retried = 0, nondet = 0;
    std::vector<double> ms;
    double rooms_sum = 0, open_sum = 0;
    for (long long s = first; s < first + seeds; ++s) {
        auto t0 = std::chrono::steady_clock::now();
        Result r = generate((uint64_t)s, g, progs, P);
        double dt = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ms.push_back(dt);
        const Floor& f = r.floor;
        const Verdict& v = r.verdict;
        // W5 spot check: the first 50 seeds are generated twice and must hash the same
        if (s < first + 50) { Result r2 = generate((uint64_t)s, g, progs, P); if (r2.floor.hash != f.hash) ++nondet; }
        ok += r.ok; conn += r.ok && v.connected; locks += r.ok && v.locks_ok; dig += r.ok && v.dig_ok; reach += r.ok && v.reach_ok; mission += r.ok && v.mission_ok;
        retried += f.attempt > 0;
        double open_pct = f.W ? 100.0 * v.floor_tiles / (f.W * f.H) : 0;
        rooms_sum += f.rooms.size();
        open_sum += open_pct;
        if (!quiet) {
            std::printf("{\"seed\":%lld,\"ok\":%s,\"ms\":%.3f,\"attempt\":%d,\"size\":%d,\"nodes\":%zu,\"edges\":%zu,\"cycles\":%d,\"rooms\":%zu,\"cells\":%d,"
                        "\"floor_tiles\":%d,\"open_pct\":%.1f,\"corridor_tiles\":%d,\"width_1_2_3\":[%d,%d,%d],\"narrowed\":%d,\"loops\":%d,\"components_before_repair\":%d,"
                        "\"repairs\":%d,\"island_repairs\":%d,\"program_fallbacks\":%d,\"components\":%d,\"connected\":%s,\"reach\":%s,\"locks\":%s,\"dig\":%s,\"mission\":%s,"
                        "\"hash\":\"%016llx\",\"why\":\"%s\",\"log\":%zu}\n",
                        s, r.ok ? "true" : "false", dt, f.attempt, f.W, f.mission.nodes.size(), f.mission.edges.size(), f.mission.cycles(), f.rooms.size(), f.cells,
                        v.floor_tiles, open_pct, f.corridor_tiles, f.width_hist[1], f.width_hist[2], f.width_hist[3], f.narrowed, f.loops, f.components_before_repair,
                        f.repairs, f.island_repairs, f.program_fallbacks, v.components, v.connected ? "true" : "false", v.reach_ok ? "true" : "false",
                        v.locks_ok ? "true" : "false", v.dig_ok ? "true" : "false", v.mission_ok ? "true" : "false", (unsigned long long)f.hash,
                        esc(r.ok ? "" : (r.log.empty() ? v.why : r.log.back())).c_str(), r.log.size());
        }
    }
    std::vector<double> sm = ms;
    std::sort(sm.begin(), sm.end());
    double mean = 0;
    for (double x : ms) mean += x;
    mean /= std::max<size_t>(1, ms.size());
    auto pct = [&](double p) { return sm.empty() ? 0.0 : sm[std::min(sm.size() - 1, (size_t)(p * sm.size()))]; };
    bool gate = ok == seeds && conn == seeds && locks == seeds && dig == seeds && nondet == 0;
    std::printf("{\"summary\":true,\"grammar\":\"%s\",\"programs\":%zu,\"hard_walls\":%s,\"size\":%d,\"seeds\":%lld,\"pass\":%lld,\"connected\":%lld,\"reach\":%lld,"
                "\"locks_after_keys\":%lld,\"dig_gate\":%lld,\"mission\":%lld,\"retried\":%lld,\"nondeterministic\":%lld,\"rooms_mean\":%.1f,\"open_pct_mean\":%.1f,"
                "\"ms_mean\":%.3f,\"ms_p50\":%.3f,\"ms_p99\":%.3f,\"ms_max\":%.3f,\"gate\":\"%s\"}\n",
                esc(grammar).c_str(), progs.size(), P.hard_walls ? "true" : "false", P.size, seeds, ok, conn, reach, locks, dig, mission, retried, nondet,
                rooms_sum / std::max<long long>(1, seeds), open_sum / std::max<long long>(1, seeds), mean, pct(0.5), pct(0.99), sm.empty() ? 0 : sm.back(), gate ? "PASS" : "FAIL");
    return gate ? 0 : 1;
}
