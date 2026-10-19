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
    std::string theme = "any";                 // crypt mine sewer temple fortress cave, or any
    std::string district;                      // macro structure the program belongs to ("" = any)
    std::string special;                       // haven boss vault nexus stronghold ("" = ordinary)
    std::string function;                      // function program id for DRESS ("" = by theme)
    struct Val* ops = nullptr;                 // parsed [[op]] array (owned by the library cache)
};
// Tile features a program emits through masks of the same name (02 section 4 structures).
enum Feat : uint8_t { F_NONE = 0, F_WATER = 1, F_DEEP = 2, F_LAVA = 3, F_CHASM = 4, F_SKY = 5, F_BRIDGE = 6, F_HAZARD = 7 };
inline bool feat_blocks(uint8_t f) { return f == F_DEEP || f == F_LAVA || f == F_CHASM; }
const char* feat_name(uint8_t f);
// The TOML subset parser, shared by room programs, function programs and prop grammars.
bool parse_toml(const std::string& src, Val*& out, std::string& err); // out owned by the library cache
bool parse_toml_file(const std::string& path, Val*& out, std::string& err);
const Val* toml_get(const Val* v, const std::string& k);
std::string toml_str(const Val* v, const std::string& k, const std::string& dflt = "");
long long toml_int(const Val* v, const std::string& k, long long dflt = 0);
double toml_num(const Val* v, const std::string& k, double dflt = 0);
std::vector<const Val*> toml_arr(const Val* v, const std::string& k);
std::vector<std::string> toml_strs(const Val* v, const std::string& k);
bool toml_expr(const Val* v, const std::vector<std::pair<std::string, long long>>& vars, long long& out); // int or "w/2+1"
bool load_programs(const std::vector<std::string>& dirs, std::vector<Program>& out, std::string& err);
// Runs program p on a w x d plan (slice at y = 1, the standing level). floor[z*w+x] = 1 is walkable.
// Returns "" or an error. stats: op count executed.
// feat (optional) receives a Feat per tile: deep/lava/chasm tiles are taken out of the floor,
// bridge tiles over them put back in, water/sky/hazard tag floor tiles.
std::string run_program(const Program& p, int w, int h, int d, uint64_t seed, std::vector<uint8_t>& floor, int* ops_run = nullptr,
                        std::vector<uint8_t>* feat = nullptr);

// ---------------------------------------------------------------- LAYOUT / floor
enum Tile : uint8_t { ROCK = 0, FLOOR = 1, HARD = 2 };   // ROCK diggable, HARD undiggable

struct RoomRow { int node; std::string program; int x0, z0, x1, z1; int door_x, door_z; std::string district; };
struct EntityRow { std::string kind; int id; int x, z; };

struct Params {
    int size = 128;              // tiles per side, up to 160
    int leaf_max = 24, leaf_min = 10;
    int loop_quota = 2;
    bool hard_walls = true;      // spec 02 section 8: walls of a lock's far side are undiggable
    int max_attempts = 8;        // sub-seeds tried before the floor is declared failed
    std::string theme;           // "" = by seed from the six themes
    bool special_cells = true;   // false = control: special nodes keep whatever cell BFS gave them
    bool band_order_x = false;   // true = the F2 band order (cells by x then z): thin column bands
    int boss_min = 12;
    bool boss_largest = false;   // true = F3.1 rule: the boss takes the largest cell of its band (far moves)
    bool band_steal = true;
    int edge_reorders = 4;       // corridor-pass restarts with the failed edge first (0 = control)      // false = control: a special that fits no cell of its band dead-ends           // the boss cell holds at least this many tiles a side inside its margin
    // macro structure grammar (kits/districts.toml): theme -> districts; one district per lock region
    std::vector<std::pair<std::string, std::vector<std::string>>> districts;
};
extern const char* const THEMES[6];
bool load_districts(const std::string& path, Params& P, std::string& err);

// DRESS (lattice/gen/31 G4 FIXTURES + G6 DRESSING, 32 section 3 function programs)
enum Zone : uint8_t { Z_NONE = 0, Z_ENTRY = 1, Z_CORE = 2, Z_EDGE = 3, Z_HAZARD = 4 };
const char* zone_name(uint8_t z);
struct DressRow {
    std::string cls;      // fixture, prop, wall
    std::string id;       // table, candle, sconce ...
    int variant = 0, x = 0, z = 0, rot = 0; // rot 0..3 = facing +z, +x, -z, -x
    uint8_t zone = Z_NONE;
    int room = -1;
    std::string source;   // activity:<id>, item:<id>, vignette:<id>, wall, clutter
    std::string content;  // container station liquid hazard light den npc writing secret, or ""
};
struct RoomDress {
    int node = -1;
    std::string function, program;
    bool hall = false;
    int floor_tiles = 0, items = 0, fixtures = 0, wall_faces = 0, wall_dressed = 0, used_tiles = 0;
    int kinds = 0;        // bit per content kind
    int clone_groups = 0; // (zone, id, variant) groups over 3, fixtures and floor props
    int clone_wall = 0;   // the same over wall trim (reported apart: trim repeats like tilework)
    int guaranteed_missed = 0, never_hits = 0, zone_relaxed = 0, blocked_rejects = 0;
    double neg_space = 0, density = 0;
    std::vector<std::string> present; // function program entries that appeared
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
    std::string theme;
    std::vector<std::string> region_district; // district per lock region
    std::vector<uint8_t> feat;   // Feat per tile
    int special_rooms = 0, themed_rooms = 0, district_rooms = 0;
    int special_swaps = 0, special_merges = 0, band_steals = 0, edge_reorders = 0, special_missed = 0; // missed: a special node on a non-special program
    unsigned special_kinds = 0;  // bit per special kind placed: haven boss vault nexus stronghold
    int boss_w = 0, boss_d = 0;
    std::vector<uint8_t> zone;   // Zone per tile (room floor only)
    std::vector<DressRow> dress;
    std::vector<RoomDress> room_dress;
    uint64_t hash = 0, dress_hash = 0;
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

// ---------------------------------------------------------------- DRESS library and pass
struct FixtureDef {        // kits/props/<id>.toml: a prop grammar that also drives placement
    std::string id, support = "floor", content;
    int fw = 1, fd = 1, variants = 1;
    bool blocks = true;
    const Val* doc = nullptr;
};
struct PropDef { std::string id, content, place = "floor"; int variants = 2; };
struct FunctionProgram { std::string id; const Val* doc = nullptr; };
struct DressLib {
    std::vector<FixtureDef> fixtures;
    std::vector<PropDef> props;
    std::vector<FunctionProgram> functions;
    double m2_per_tile = 1.0;   // ASSUMED: one tile = 1 m^2 for the density rule
    bool clone_cap = true;      // false = the control trial: Clone Army must come back
    const FixtureDef* fixture(const std::string& id) const;
    const PropDef* prop(const std::string& id) const;
    const FunctionProgram* function(const std::string& id) const;
};
extern const char* const CONTENT_KINDS[9];
int content_bit(const std::string& k);   // -1 if not one of the nine
bool load_dress(const std::string& kits_root, DressLib& lib, std::string& err);
// Dresses every room of a floor that passed the critic; deterministic in (seed, floor). Never
// changes tiles: fixtures are entity rows, and a blocking fixture is only accepted when the room's
// walkable tiles stay one 4-connected piece (the clearance rule), so the hard gates are untouched.
void dress_floor(Floor& f, const std::vector<Program>& progs, const DressLib& lib, uint64_t seed);
uint64_t dress_hash(const Floor& f);
// Independent re-check from the emitted rows only: in every room, the floor tiles not under a
// blocking fixture are one 4-connected piece, every opening tile is free, and no two blocking
// fixtures share a tile. Returns the number of violating rooms.
int dress_check(const Floor& f, const DressLib& lib);

// ---------------------------------------------------------------- Barony .vox (slab format)
// int32 sizex, sizey, sizez; sizex*sizey*sizez palette bytes (255 = empty) at index
// z + y*sizez + x*sizey*sizez; 768 palette bytes in 0..63 (Barony shifts them left by 2).
// Measured in barony-apx/src/files.cpp loadVoxel and the mesher (index formula).
struct Vox {
    int sx = 0, sy = 0, sz = 0;
    std::vector<uint8_t> data;
    uint8_t pal[768] = {0};
    int count() const { int n = 0; for (uint8_t b : data) n += b != 255; return n; }
};
// The prop grammar: builds variant v of a fixture. Grammar z is up; Barony's model z is written
// flipped (z index 0 = top), ASSUMED from Barony's down-positive z; see README.
bool build_vox(const FixtureDef& fx, int variant, Vox& out, std::string& err);
bool write_vox(const std::string& path, const Vox& v);
bool read_vox(const std::string& path, Vox& v, std::string& err);

} // namespace apx
