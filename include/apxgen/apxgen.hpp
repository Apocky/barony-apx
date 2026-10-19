// apx-gen: the F2 generator core (specs/barony-fork/02 sections 3, 7, 8). C++17, no Barony dependency.
// Port of Apockalypsis crates/bl-world MISSION + LAYOUT onto a 2D tile grid, a 2D interpreter for
// the TOML room programs, and the hard critic checks (reach, locks after keys, dig-BFS gate).
// Integer only; every pass is a pure function of (seed, inputs).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace apx {

// ---------------------------------------------------------------- rng / hash
uint64_t mix(uint64_t x);                                   // splitmix64 finaliser
uint64_t hash_words(std::initializer_list<uint64_t> w);
uint64_t hash_str(const std::string& s);
struct Rng {
    uint64_t seed, n = 0;
    explicit Rng(uint64_t s) : seed(s) {}
    uint64_t next(uint64_t m) { ++n; return hash_words({seed, n}) % (m ? m : 1); }
};

// ---------------------------------------------------------------- MISSION
enum class Kind : uint8_t { Entrance, Room, Key, Lock, Boss, Treasure, Secret, Haven, Nexus, Stronghold, Exit };
const char* kind_name(Kind k);
bool kind_parse(const std::string& s, Kind& k);

struct Node { Kind kind; int id = 0; std::string name; };
struct Mission {
    std::vector<Node> nodes;
    std::vector<std::pair<int, int>> edges;   // (min, max)
    std::vector<int> neighbours(int v) const;
    int find(Kind k, int id = 0) const;
    int cycles() const;                        // cyclomatic number E - V + 1 (graph is connected)
};

// A grammar is rows of text (grammar/*.grammar); rows run in order. See grammar/default.grammar.
struct Grammar { std::vector<std::string> rows; std::string source; };
bool load_grammar(const std::string& path, Grammar& g, std::string& err);
bool mission_from_grammar(const Grammar& g, uint64_t seed, Mission& m, std::string& err);
std::vector<char> reach(const Mission& m, const std::vector<int>& closed);
std::string check_mission(const Mission& m);   // "" = ok
std::vector<uint32_t> regions(const Mission& m); // locks closed between node and entrance
std::vector<uint32_t> behind_masks(const Mission& m); // bit (id-1) set = node is behind lock id

// ---------------------------------------------------------------- room programs (2D)
struct Val;
struct Program {
    std::string id, path;
    int min[3] = {0, 0, 0}, max[3] = {0, 0, 0};
    std::vector<std::string> tags;
    struct Val* ops = nullptr;                 // parsed [[op]] array (owned by the library cache)
};
bool load_programs(const std::vector<std::string>& dirs, std::vector<Program>& out, std::string& err);
// Runs program p on a w x d plan (slice at y = 1, the standing level). floor[z*w+x] = 1 is walkable.
// Returns "" or an error. stats: op count executed.
std::string run_program(const Program& p, int w, int h, int d, uint64_t seed, std::vector<uint8_t>& floor, int* ops_run = nullptr);

// ---------------------------------------------------------------- LAYOUT / floor
enum Tile : uint8_t { ROCK = 0, FLOOR = 1, HARD = 2 };   // ROCK diggable, HARD undiggable

struct RoomRow { int node; std::string program; int x0, z0, x1, z1; int door_x, door_z; };
struct EntityRow { std::string kind; int id; int x, z; };

struct Params {
    int size = 128;              // tiles per side, up to 160
    int leaf_max = 24, leaf_min = 10;
    int loop_quota = 2;
    bool hard_walls = true;      // spec 02 section 8: walls of a lock's far side are undiggable
    int max_attempts = 8;        // sub-seeds tried before the floor is declared failed
};

struct Floor {
    int W = 0, H = 0;
    std::vector<uint8_t> tile;   // Tile
    std::vector<int16_t> owner;  // room node of a room-box tile, -1 corridor/none
    std::vector<uint32_t> behind;// behind-lock mask of the tile (floor or room box tiles)
    Mission mission;
    std::vector<uint32_t> reg;
    std::vector<RoomRow> rooms;
    std::vector<EntityRow> entities;
    int corridor_tiles = 0, loops = 0, repairs = 0, island_repairs = 0, narrowed = 0;
    int program_fallbacks = 0, components_before_repair = 0, cells = 0, attempt = 0;
    int width_hist[4] = {0, 0, 0, 0};
    uint64_t hash = 0;
    size_t idx(int x, int z) const { return (size_t)z * W + x; }
};

// One attempt at sub-seed `attempt`. "" = built; else why the layout dead-ended.
std::string build_floor(uint64_t seed, int attempt, const Grammar& g, const std::vector<Program>& progs, const Params& P, Floor& f);
uint64_t floor_hash(const Floor& f);

// ---------------------------------------------------------------- critic (02 section 7, 8)
struct Verdict {
    bool overlap_ok = true, reach_ok = true, connected = true, locks_ok = true, dig_ok = true, mission_ok = true;
    int floor_tiles = 0, reached_tiles = 0, components = 0;
    std::string why;
    bool pass() const { return overlap_ok && reach_ok && connected && locks_ok && dig_ok && mission_ok; }
};
Verdict critic(const Floor& f);

// Full generation per 02 section 7: build, critic, on failure the next sub-seed; failures logged.
struct Result { Floor floor; Verdict verdict; std::vector<std::string> log; bool ok = false; };
Result generate(uint64_t seed, const Grammar& g, const std::vector<Program>& progs, const Params& P);

} // namespace apx
