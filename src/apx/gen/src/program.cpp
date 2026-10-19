// The 2D interpreter for the TOML room programs (02 section 5; lattice/03 CARVE operator set):
// stamp (box, cylinder, disc), union, subtract, intersect, xor, dilate, erode, open, close, edge,
// noise threshold, components (ccl), repeat, assign. A 3D program is evaluated on its plan slice
// at y = 1 (the standing level): a box counts where from.y <= 1 < to.y, a cylinder by its section
// at y = 1. The [detail] block runs at FINE (the DETAIL pass) and is not part of CARVE: skipped.
#include "apxgen/apxgen.hpp"
#include <algorithm>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

namespace apx {

// ---------------------------------------------------------------- minimal TOML (the subset the kits use)
struct Val {
    enum T { Str, Int, Bool, Arr, Tab, Flt } t = Tab;
    std::string s;
    long long i = 0;
    double f = 0;
    std::vector<Val> a;
    std::vector<std::pair<std::string, Val>> kv;
    const Val* get(const std::string& k) const {
        for (auto& p : kv) if (p.first == k) return &p.second;
        return nullptr;
    }
    Val* getm(const std::string& k) {
        for (auto& p : kv) if (p.first == k) return &p.second;
        return nullptr;
    }
};

struct TomlParser {
    const std::string& s;
    size_t p = 0;
    std::string err;
    explicit TomlParser(const std::string& src) : s(src) {}
    void ws(bool nl) {
        for (;;) {
            while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || (nl && s[p] == '\n'))) ++p;
            if (p < s.size() && s[p] == '#') { while (p < s.size() && s[p] != '\n') ++p; continue; }
            break;
        }
    }
    bool fail(const std::string& m) { if (err.empty()) err = m + " at byte " + std::to_string(p); return false; }
    bool key(std::string& k) {
        ws(false);
        if (p < s.size() && s[p] == '"') { Val v; if (!str(v)) return false; k = v.s; return true; }
        size_t b = p;
        while (p < s.size() && (std::isalnum((unsigned char)s[p]) || s[p] == '_' || s[p] == '-')) ++p;
        if (p == b) return fail("expected key");
        k = s.substr(b, p - b);
        return true;
    }
    bool str(Val& v) {
        ++p;
        v.t = Val::Str;
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\' && p + 1 < s.size()) ++p;
            v.s += s[p++];
        }
        if (p >= s.size()) return fail("unterminated string");
        ++p;
        return true;
    }
    bool value(Val& v) {
        ws(true);
        if (p >= s.size()) return fail("expected value");
        char c = s[p];
        if (c == '"') return str(v);
        if (c == '[') {
            ++p; v.t = Val::Arr;
            for (;;) {
                ws(true);
                if (p < s.size() && s[p] == ']') { ++p; return true; }
                Val e;
                if (!value(e)) return false;
                v.a.push_back(std::move(e));
                ws(true);
                if (p < s.size() && s[p] == ',') { ++p; continue; }
                ws(true);
                if (p < s.size() && s[p] == ']') { ++p; return true; }
                return fail("expected , or ]");
            }
        }
        if (c == '{') {
            ++p; v.t = Val::Tab;
            for (;;) {
                ws(true);
                if (p < s.size() && s[p] == '}') { ++p; return true; }
                std::string k;
                if (!key(k)) return false;
                ws(false);
                if (p >= s.size() || s[p] != '=') return fail("expected =");
                ++p;
                Val e;
                if (!value(e)) return false;
                v.kv.push_back({k, std::move(e)});
                ws(true);
                if (p < s.size() && s[p] == ',') { ++p; continue; }
            }
        }
        if (s.compare(p, 4, "true") == 0) { p += 4; v.t = Val::Bool; v.i = 1; return true; }
        if (s.compare(p, 5, "false") == 0) { p += 5; v.t = Val::Bool; v.i = 0; return true; }
        if (c == '-' || c == '+' || std::isdigit((unsigned char)c)) {
            size_t b = p;
            ++p;
            while (p < s.size() && (std::isdigit((unsigned char)s[p]) || s[p] == '_' || s[p] == '.')) ++p;
            std::string n;
            for (size_t q = b; q < p; ++q) if (s[q] != '_') n += s[q];
            if (n.find('.') != std::string::npos) { v.t = Val::Flt; v.f = std::stod(n); v.i = (long long)v.f; return true; }
            v.t = Val::Int; v.i = std::stoll(n); v.f = (double)v.i;
            return true;
        }
        return fail("bad value");
    }
    bool doc(Val& root) {
        root.t = Val::Tab;
        Val* cur = &root;
        for (;;) {
            ws(true);
            if (p >= s.size()) return true;
            if (s[p] == '[') {
                bool arr = p + 1 < s.size() && s[p + 1] == '[';
                p += arr ? 2 : 1;
                std::string k;
                if (!key(k)) return false;
                p += arr ? 2 : 1;
                Val* slot = root.getm(k);
                if (!slot) { root.kv.push_back({k, Val{}}); slot = &root.kv.back().second; slot->t = arr ? Val::Arr : Val::Tab; }
                if (arr) { slot->a.push_back(Val{}); cur = &slot->a.back(); } else cur = slot;
                continue;
            }
            std::string k;
            if (!key(k)) return false;
            ws(false);
            if (p >= s.size() || s[p] != '=') return fail("expected =");
            ++p;
            Val v;
            if (!value(v)) return false;
            cur->kv.push_back({k, std::move(v)});
        }
    }
};

// ---------------------------------------------------------------- integer expressions: w h d i, + - * / ( )
struct Expr {
    const std::string& s;
    size_t p = 0;
    const std::map<std::string, long long>& vars;
    bool ok = true;
    void sp() { while (p < s.size() && s[p] == ' ') ++p; }
    long long atom() {
        sp();
        if (p < s.size() && s[p] == '(') { ++p; long long v = sum(); sp(); if (p < s.size() && s[p] == ')') ++p; else ok = false; return v; }
        if (p < s.size() && s[p] == '-') { ++p; return -atom(); }
        if (p < s.size() && std::isdigit((unsigned char)s[p])) { long long v = 0; while (p < s.size() && std::isdigit((unsigned char)s[p])) v = v * 10 + (s[p++] - '0'); return v; }
        std::string n;
        while (p < s.size() && (std::isalpha((unsigned char)s[p]) || s[p] == '_')) n += s[p++];
        auto it = vars.find(n);
        if (n.empty() || it == vars.end()) { ok = false; return 0; }
        return it->second;
    }
    long long prod() {
        long long v = atom();
        for (;;) {
            sp();
            if (p < s.size() && s[p] == '*') { ++p; v *= atom(); }
            else if (p < s.size() && s[p] == '/') { ++p; long long d = atom(); v = d ? v / d : (ok = false, 0); }
            else return v;
        }
    }
    long long sum() {
        long long v = prod();
        for (;;) {
            sp();
            if (p < s.size() && s[p] == '+') { ++p; v += prod(); }
            else if (p < s.size() && s[p] == '-') { ++p; v -= prod(); }
            else return v;
        }
    }
};

static bool num(const Val* v, const std::map<std::string, long long>& vars, long long& out) {
    if (!v) return false;
    if (v->t == Val::Int) { out = v->i; return true; }
    if (v->t != Val::Str) return false;
    Expr e{v->s, 0, vars};
    out = e.sum();
    e.sp();
    return e.ok && e.p == v->s.size();
}

// ---------------------------------------------------------------- loading
static std::vector<std::unique_ptr<Val>> g_docs; // owned parsed programs

bool parse_toml(const std::string& src, Val*& out, std::string& err) {
    auto doc = std::make_unique<Val>();
    TomlParser tp(src);
    if (!tp.doc(*doc)) { err = tp.err; return false; }
    out = doc.get();
    g_docs.push_back(std::move(doc));
    return true;
}
bool parse_toml_file(const std::string& path, Val*& out, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << in.rdbuf();
    if (!parse_toml(ss.str(), out, err)) { err = path + ": " + err; return false; }
    return true;
}
const Val* toml_get(const Val* v, const std::string& k) { return v ? v->get(k) : nullptr; }
std::string toml_str(const Val* v, const std::string& k, const std::string& dflt) {
    const Val* x = toml_get(v, k);
    return x && x->t == Val::Str ? x->s : dflt;
}
long long toml_int(const Val* v, const std::string& k, long long dflt) {
    const Val* x = toml_get(v, k);
    return x && (x->t == Val::Int || x->t == Val::Bool || x->t == Val::Flt) ? x->i : dflt;
}
double toml_num(const Val* v, const std::string& k, double dflt) {
    const Val* x = toml_get(v, k);
    return x && (x->t == Val::Int || x->t == Val::Flt) ? x->f : dflt;
}
std::vector<const Val*> toml_arr(const Val* v, const std::string& k) {
    std::vector<const Val*> o;
    const Val* x = k.empty() ? v : toml_get(v, k);
    if (x && x->t == Val::Arr) for (auto& e : x->a) o.push_back(&e);
    return o;
}
std::vector<std::string> toml_strs(const Val* v, const std::string& k) {
    std::vector<std::string> o;
    for (auto* e : toml_arr(v, k)) if (e->t == Val::Str) o.push_back(e->s);
    return o;
}
const char* feat_name(uint8_t f) {
    static const char* N[] = {"none", "water", "deep", "lava", "chasm", "sky", "bridge", "hazard"};
    return f < 8 ? N[f] : "?";
}

bool load_programs(const std::vector<std::string>& dirs, std::vector<Program>& out, std::string& err) {
    namespace fs = std::filesystem;
    std::vector<fs::path> files;
    for (auto& d : dirs) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) { err = "no kit directory " + d; return false; }
        for (auto& e : fs::directory_iterator(d)) if (e.path().extension() == ".toml") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return a.filename().string() < b.filename().string(); });
    for (auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string src = ss.str();
        auto doc = std::make_unique<Val>();
        TomlParser tp(src);
        if (!tp.doc(*doc)) { err = f.string() + ": " + tp.err; return false; }
        const Val* meta = doc->get("meta");
        const Val* fits = meta ? meta->get("fits") : nullptr;
        const Val *mn = fits ? fits->get("min") : nullptr, *mx = fits ? fits->get("max") : nullptr;
        if (!meta || !meta->get("id") || !mn || !mx || mn->a.size() != 3 || mx->a.size() != 3) { err = f.string() + ": meta.id / meta.fits min,max [3] required"; return false; }
        Program p;
        p.id = meta->get("id")->s;
        p.path = f.string();
        for (int k = 0; k < 3; ++k) { p.min[k] = (int)mn->a[k].i; p.max[k] = (int)mx->a[k].i; }
        if (auto t = meta->get("tags")) for (auto& x : t->a) p.tags.push_back(x.s);
        static const char* THEMES[] = {"crypt", "mine", "sewer", "temple", "fortress", "cave"};
        p.theme = toml_str(meta, "theme", "");
        if (p.theme.empty()) {
            p.theme = "any";
            for (auto& t : p.tags) for (auto* th : THEMES) if (t == th) p.theme = th;
        }
        p.district = toml_str(meta, "district");
        p.special = toml_str(meta, "special");
        p.function = toml_str(meta, "function");
        Val* ops = doc->getm("op");
        if (!ops || ops->t != Val::Arr || ops->a.empty()) { err = f.string() + ": no [[op]] rows"; return false; }
        p.ops = ops;
        for (auto& q : out) if (q.id == p.id) { err = "duplicate program id " + p.id; return false; }
        out.push_back(p);
        g_docs.push_back(std::move(doc));
    }
    if (out.empty()) { err = "no room programs found"; return false; }
    return true;
}

// ---------------------------------------------------------------- the interpreter
namespace {
using Mask = std::vector<uint8_t>;
struct Ctx {
    int w, d, h;
    uint64_t seed;
    std::map<std::string, Mask> m;
    Mask solid;
    bool assigned = false;
    int ops = 0;
    std::string err;
    Mask& get(const std::string& n) { auto& x = m[n]; if (x.empty()) x.assign((size_t)w * d, 0); return x; }
    Mask expr(const std::string& e) {
        bool neg = !e.empty() && e[0] == '!';
        Mask x = get(neg ? e.substr(1) : e);
        if (neg) for (auto& c : x) c = !c;
        return x;
    }
};

Mask morph(const Ctx& c, const Mask& in, int k, bool dilate) {
    Mask out(in.size(), 0);
    for (int z = 0; z < c.d; ++z)
        for (int x = 0; x < c.w; ++x) {
            bool any = false, all = true;
            for (int dz = -k; dz <= k; ++dz)
                for (int dx = -k; dx <= k; ++dx) {
                    int nx = x + dx, nz = z + dz;
                    bool v = nx >= 0 && nz >= 0 && nx < c.w && nz < c.d && in[(size_t)nz * c.w + nx];
                    any |= v; all &= v;
                }
            out[(size_t)z * c.w + x] = dilate ? any : all;
        }
    return out;
}

Mask components(const Ctx& c, const Mask& in, long long min_size, bool largest) {
    std::vector<int> lab(in.size(), -1);
    std::vector<std::vector<int>> comps;
    for (int s = 0; s < (int)in.size(); ++s) {
        if (!in[s] || lab[s] >= 0) continue;
        std::vector<int> cc{s};
        lab[s] = (int)comps.size();
        for (size_t k = 0; k < cc.size(); ++k) {
            int x = cc[k] % c.w, z = cc[k] / c.w;
            const int D[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (auto& dd : D) {
                int nx = x + dd[0], nz = z + dd[1];
                if (nx < 0 || nz < 0 || nx >= c.w || nz >= c.d) continue;
                int n = nz * c.w + nx;
                if (in[n] && lab[n] < 0) { lab[n] = lab[s]; cc.push_back(n); }
            }
        }
        comps.push_back(std::move(cc));
    }
    size_t best = 0;
    for (size_t i = 1; i < comps.size(); ++i) if (comps[i].size() > comps[best].size()) best = i;
    Mask out(in.size(), 0);
    for (size_t i = 0; i < comps.size(); ++i) {
        if (largest && i != best) continue;
        if ((long long)comps[i].size() < min_size) continue;
        for (int t : comps[i]) out[t] = 1;
    }
    return out;
}

// value noise on a lattice of `scale` cells, integer bilinear, values 0..1023
Mask noise(const Ctx& c, uint64_t salt, long long scale, long long thr) {
    if (scale < 1) scale = 1;
    auto lat = [&](long long gx, long long gz) { return (long long)(hash_words({c.seed, salt, (uint64_t)gx, (uint64_t)gz}) % 1024); };
    Mask out((size_t)c.w * c.d, 0);
    for (int z = 0; z < c.d; ++z)
        for (int x = 0; x < c.w; ++x) {
            long long gx = x / scale, gz = z / scale, fx = x % scale, fz = z % scale;
            long long a = lat(gx, gz), b = lat(gx + 1, gz), e = lat(gx, gz + 1), f = lat(gx + 1, gz + 1);
            long long top = a * (scale - fx) + b * fx, bot = e * (scale - fx) + f * fx;
            long long v = (top * (scale - fz) + bot * fz) / (scale * scale);
            out[(size_t)z * c.w + x] = v < thr;
        }
    return out;
}

std::string sval(const Val& op, const char* k, const std::string& dflt = "") {
    const Val* v = op.get(k);
    return v && v->t == Val::Str ? v->s : dflt;
}

bool run_ops(Ctx& c, const Val& ops, std::map<std::string, long long>& vars) {
    for (auto& op : ops.a) {
        ++c.ops;
        std::string kind = sval(op, "kind");
        auto bad = [&](const std::string& m) { c.err = "op " + std::to_string(c.ops) + " (" + kind + "): " + m; return false; };
        if (kind == "stamp") {
            std::string shape = sval(op, "shape", "box");
            Mask& dst = c.get(sval(op, "mask"));
            if (shape == "box") {
                const Val *f = op.get("from"), *t = op.get("to");
                if (!f || !t || f->a.size() != 3 || t->a.size() != 3) return bad("box needs from/to [3]");
                long long a[3], b[3];
                for (int k = 0; k < 3; ++k) if (!num(&f->a[k], vars, a[k]) || !num(&t->a[k], vars, b[k])) return bad("bad coordinate");
                if (!(a[1] <= 1 && 1 < b[1])) continue; // the box misses the plan slice
                for (long long z = std::max(0LL, a[2]); z < std::min<long long>(c.d, b[2]); ++z)
                    for (long long x = std::max(0LL, a[0]); x < std::min<long long>(c.w, b[0]); ++x) dst[(size_t)z * c.w + x] = 1;
            } else if (shape == "cylinder" || shape == "disc") {
                std::string axis = shape == "disc" ? "y" : sval(op, "axis", "y");
                const Val* ce = op.get("center");
                long long r, p0, p1;
                if (!ce || ce->a.size() != 2 || !num(&ce->a[0], vars, p0) || !num(&ce->a[1], vars, p1) || !num(op.get("radius"), vars, r)) return bad("cylinder needs center [2] and radius");
                for (int z = 0; z < c.d; ++z)
                    for (int x = 0; x < c.w; ++x) {
                        long long u, v; // the section offsets in the plane normal to the axis
                        if (axis == "z") { u = x - p0; v = 1 - p1; }
                        else if (axis == "x") { u = 1 - p0; v = z - p1; }
                        else { u = x - p0; v = z - p1; }
                        if (u * u + v * v <= r * r) dst[(size_t)z * c.w + x] = 1;
                    }
            } else return bad("unknown shape " + shape);
        } else if (kind == "union" || kind == "subtract" || kind == "intersect" || kind == "xor") {
            Mask src = c.expr(sval(op, "src"));
            Mask& dst = c.get(sval(op, "dst"));
            for (size_t i = 0; i < dst.size(); ++i) {
                if (kind == "union") dst[i] = dst[i] | src[i];
                else if (kind == "subtract") dst[i] = dst[i] & !src[i];
                else if (kind == "intersect") dst[i] = dst[i] & src[i];
                else dst[i] = dst[i] ^ src[i];
            }
        } else if (kind == "dilate" || kind == "erode" || kind == "open" || kind == "close" || kind == "edge") {
            std::string out = sval(op, "mask"), src = sval(op, "src", out);
            long long k = 1;
            if (op.get("k") && !num(op.get("k"), vars, k)) return bad("bad k");
            Mask s = c.expr(src), r;
            if (kind == "dilate") r = morph(c, s, (int)k, true);
            else if (kind == "erode") r = morph(c, s, (int)k, false);
            else if (kind == "open") r = morph(c, morph(c, s, (int)k, false), (int)k, true);
            else if (kind == "close") r = morph(c, morph(c, s, (int)k, true), (int)k, false);
            else { Mask e = morph(c, s, 1, false); r = s; for (size_t i = 0; i < r.size(); ++i) r[i] = s[i] && !e[i]; }
            c.get(out) = r;
        } else if (kind == "noise") {
            long long sc = 4, thr = 512;
            if (op.get("scale_cells") && !num(op.get("scale_cells"), vars, sc)) return bad("bad scale_cells");
            if (op.get("threshold_1024") && !num(op.get("threshold_1024"), vars, thr)) return bad("bad threshold_1024");
            c.get(sval(op, "mask")) = noise(c, hash_str(sval(op, "seed", "noise")), sc, thr);
        } else if (kind == "components" || kind == "ccl") {
            std::string out = sval(op, "mask"), src = sval(op, "src", out);
            long long mn = 0;
            if (op.get("min") && !num(op.get("min"), vars, mn)) return bad("bad min");
            c.get(out) = components(c, c.expr(src), mn, sval(op, "keep") == "largest");
        } else if (kind == "repeat") {
            std::string along = sval(op, "along", "z");
            long long step = 1, from = 0, to = along == "x" ? c.w : c.d;
            if ((op.get("step") && !num(op.get("step"), vars, step)) || (op.get("from") && !num(op.get("from"), vars, from)) || (op.get("to") && !num(op.get("to"), vars, to))) return bad("bad repeat bounds");
            const Val* inner = op.get("ops");
            if (!inner || inner->t != Val::Arr || step < 1) return bad("repeat needs ops and step >= 1");
            long long saved = vars.count("i") ? vars["i"] : 0;
            for (long long i = from; i < to; i += step) { vars["i"] = i; if (!run_ops(c, *inner, vars)) return false; }
            vars["i"] = saved;
        } else if (kind == "assign") {
            Mask x = c.expr(sval(op, "mask"));
            if (c.solid.empty()) c.solid.assign(x.size(), 0);
            for (size_t i = 0; i < x.size(); ++i) c.solid[i] |= x[i];
            c.assigned = true;
        } else {
            return bad("unknown op kind");
        }
    }
    return true;
}
} // namespace

bool toml_expr(const Val* v, const std::vector<std::pair<std::string, long long>>& vars, long long& out) {
    std::map<std::string, long long> m(vars.begin(), vars.end());
    return num(v, m, out);
}

std::string run_program(const Program& p, int w, int h, int d, uint64_t seed, std::vector<uint8_t>& floor, int* ops_run, std::vector<uint8_t>* feat) {
    Ctx c{w, d, h, seed, {}, {}, false, 0, ""};
    std::map<std::string, long long> vars{{"w", w}, {"h", h}, {"d", d}, {"i", 0}};
    if (!run_ops(c, *p.ops, vars)) return p.id + ": " + c.err;
    if (ops_run) *ops_run = c.ops;
    floor.assign((size_t)w * d, 0);
    const Mask* fl = c.assigned ? nullptr : &c.get("void");
    int n = 0;
    for (int z = 1; z < d - 1; ++z)
        for (int x = 1; x < w - 1; ++x) {
            size_t i = (size_t)z * w + x;
            floor[i] = c.assigned ? !c.solid[i] : (*fl)[i];
            n += floor[i];
        }
    if (feat) {
        feat->assign((size_t)w * d, F_NONE);
        struct FM { const char* name; uint8_t f; };
        // order matters: floor tags, then blockers out of the floor, then bridges back in
        static const FM ORDER[] = {{"hazard", F_HAZARD}, {"water", F_WATER}, {"sky", F_SKY}, {"deep", F_DEEP}, {"lava", F_LAVA}, {"chasm", F_CHASM}, {"bridge", F_BRIDGE}};
        for (auto& fm : ORDER) {
            if (!c.m.count(fm.name)) continue;
            const Mask& mk = c.m[fm.name];
            for (int z = 1; z < d - 1; ++z)
                for (int x = 1; x < w - 1; ++x) {
                    size_t i = (size_t)z * w + x;
                    if (!mk[i]) continue;
                    if (feat_blocks(fm.f)) { if (floor[i]) --n; floor[i] = 0; (*feat)[i] = fm.f; }
                    else if (fm.f == F_BRIDGE) { if (feat_blocks((*feat)[i])) { ++n; floor[i] = 1; (*feat)[i] = F_BRIDGE; } }
                    else if (floor[i]) (*feat)[i] = fm.f;
                }
        }
    }
    if (n == 0) return p.id + ": no floor tile at " + std::to_string(w) + "x" + std::to_string(d);
    return "";
}

} // namespace apx
