// The prop grammar (kits/props/<id>.toml) and Barony's slab .vox writer/reader.
// A grammar names dims [x, y, z] (z up), a 6-bit palette, and [[part]] rows run in order:
//   box     from/to [3] half-open, color         fill
//   clear   from/to [3]                          empty (255)
//   paint   from/to [3], color                   recolour filled voxels only
//   cyl     center [2] (x, y), radius, z [2], color   vertical cylinder
//   sphere  center [3], radius, color
//   chunks  count, size [2], from/to [3], color, seed   seeded boxes (rubble, ore)
//   repeat  from, to, step, parts [inline rows]  binds i
// Expressions use w d h (dims), v (variant) and i, as the room programs do.
#include "apxgen/apxgen.hpp"
#include <cstdio>
#include <fstream>

namespace apx {

namespace {
struct VCtx {
    Vox& v;
    std::vector<std::pair<std::string, long long>> vars;
    uint64_t seed;
    std::string err;
    void set(long long x, long long y, long long z, uint8_t c) {
        if (x < 0 || y < 0 || z < 0 || x >= v.sx || y >= v.sy || z >= v.sz) return;
        // grammar z is up; Barony's model index z runs the other way (written flipped)
        size_t i = (size_t)(v.sz - 1 - z) + (size_t)y * v.sz + (size_t)x * v.sy * v.sz;
        v.data[i] = c;
    }
    uint8_t get(long long x, long long y, long long z) const {
        size_t i = (size_t)(v.sz - 1 - z) + (size_t)y * v.sz + (size_t)x * v.sy * v.sz;
        return v.data[i];
    }
    bool e(const Val* x, long long& o) { return x && toml_expr(x, vars, o); }
    bool vec(const Val* row, const char* k, int n, long long* o) {
        auto a = toml_arr(row, k);
        if ((int)a.size() != n) return false;
        for (int i = 0; i < n; ++i) if (!e(a[i], o[i])) return false;
        return true;
    }
    bool run(const std::vector<const Val*>& parts) {
        for (auto* p : parts) {
            std::string op = toml_str(p, "op");
            long long c = 0;
            if (toml_get(p, "color") && !e(toml_get(p, "color"), c)) return fail(op, "bad color");
            uint8_t col = (uint8_t)c;
            long long a[3], b[3];
            if (op == "box" || op == "clear" || op == "paint") {
                if (!vec(p, "from", 3, a) || !vec(p, "to", 3, b)) return fail(op, "from/to [3]");
                for (long long x = a[0]; x < b[0]; ++x)
                    for (long long y = a[1]; y < b[1]; ++y)
                        for (long long z = a[2]; z < b[2]; ++z) {
                            if (x < 0 || y < 0 || z < 0 || x >= v.sx || y >= v.sy || z >= v.sz) continue;
                            if (op == "box") set(x, y, z, col);
                            else if (op == "clear") set(x, y, z, 255);
                            else if (get(x, y, z) != 255) set(x, y, z, col);
                        }
            } else if (op == "cyl") {
                long long ce[2], zz[2], r;
                if (!vec(p, "center", 2, ce) || !vec(p, "z", 2, zz) || !e(toml_get(p, "radius"), r)) return fail(op, "center [2], z [2], radius");
                // centre on a half-voxel grid: (2x+1 - 2cx)^2 + ... <= (2r)^2, so even widths stay symmetric
                for (long long x = 0; x < v.sx; ++x)
                    for (long long y = 0; y < v.sy; ++y) {
                        long long dx = 2 * x + 1 - 2 * ce[0], dy = 2 * y + 1 - 2 * ce[1];
                        if (dx * dx + dy * dy > 4 * r * r) continue;
                        for (long long z = zz[0]; z < zz[1]; ++z) set(x, y, z, col);
                    }
            } else if (op == "sphere") {
                long long ce[3], r;
                if (!vec(p, "center", 3, ce) || !e(toml_get(p, "radius"), r)) return fail(op, "center [3], radius");
                for (long long x = 0; x < v.sx; ++x)
                    for (long long y = 0; y < v.sy; ++y)
                        for (long long z = 0; z < v.sz; ++z) {
                            long long dx = 2 * x + 1 - 2 * ce[0], dy = 2 * y + 1 - 2 * ce[1], dz = 2 * z + 1 - 2 * ce[2];
                            if (dx * dx + dy * dy + dz * dz <= 4 * r * r) set(x, y, z, col);
                        }
            } else if (op == "chunks") {
                long long n, sz[2];
                if (!e(toml_get(p, "count"), n) || !vec(p, "size", 2, sz) || !vec(p, "from", 3, a) || !vec(p, "to", 3, b)) return fail(op, "count, size [2], from/to [3]");
                Rng r(hash_words({seed, hash_str(toml_str(p, "seed", "chunks"))}));
                for (long long k = 0; k < n; ++k) {
                    long long s = sz[0] + (long long)r.next((uint64_t)std::max(1LL, sz[1] - sz[0] + 1));
                    long long x0 = a[0] + (long long)r.next((uint64_t)std::max(1LL, b[0] - a[0] - s + 1));
                    long long y0 = a[1] + (long long)r.next((uint64_t)std::max(1LL, b[1] - a[1] - s + 1));
                    long long z0 = a[2] + (long long)r.next((uint64_t)std::max(1LL, b[2] - a[2] - s + 1));
                    for (long long x = x0; x < x0 + s; ++x)
                        for (long long y = y0; y < y0 + s; ++y)
                            for (long long z = z0; z < z0 + std::max(1LL, s - 1); ++z) set(x, y, z, (uint8_t)(col + (k % 2)));
                }
            } else if (op == "repeat") {
                long long f0, t0, st = 1;
                if (!e(toml_get(p, "from"), f0) || !e(toml_get(p, "to"), t0) || (toml_get(p, "step") && !e(toml_get(p, "step"), st)) || st < 1) return fail(op, "from, to, step");
                size_t slot = 0;
                while (slot < vars.size() && vars[slot].first != "i") ++slot;
                long long saved = vars[slot].second;
                for (long long i = f0; i < t0; i += st) {
                    vars[slot].second = i;
                    if (!run(toml_arr(p, "parts"))) return false;
                }
                vars[slot].second = saved;
            } else return fail(op, "unknown op");
        }
        return true;
    }
    bool fail(const std::string& op, const std::string& m) { if (err.empty()) err = "part " + op + ": " + m; return false; }
};
} // namespace

bool build_vox(const FixtureDef& fx, int variant, Vox& out, std::string& err) {
    const Val* meta = toml_get(fx.doc, "meta");
    out = Vox{};
    std::vector<std::pair<std::string, long long>> vars{{"v", variant}, {"i", 0}};
    auto dims = toml_arr(meta, "dims");
    long long d[3];
    if (dims.size() != 3) { err = fx.id + ": meta.dims [3] required"; return false; }
    for (int k = 0; k < 3; ++k) if (!toml_expr(dims[k], vars, d[k]) || d[k] < 1 || d[k] > 128) { err = fx.id + ": bad dims"; return false; }
    out.sx = (int)d[0]; out.sy = (int)d[1]; out.sz = (int)d[2];
    out.data.assign((size_t)out.sx * out.sy * out.sz, 255);
    auto pal = toml_arr(meta, "palette");
    for (size_t i = 0; i < pal.size() && i < 256; ++i) {
        auto rgb = toml_arr(pal[i], "");
        for (int k = 0; k < 3 && k < (int)rgb.size(); ++k) {
            long long c = 0;
            toml_expr(rgb[k], {}, c);
            out.pal[i * 3 + k] = (uint8_t)std::max(0LL, std::min(63LL, c));
        }
    }
    vars.push_back({"w", d[0]}); vars.push_back({"d", d[1]}); vars.push_back({"h", d[2]});
    VCtx c{out, vars, hash_words({hash_str(fx.id), (uint64_t)variant}), ""};
    if (!c.run(toml_arr(fx.doc, "part"))) { err = fx.id + " v" + std::to_string(variant) + ": " + c.err; return false; }
    if (out.count() == 0) { err = fx.id + ": no voxels"; return false; }
    return true;
}

bool write_vox(const std::string& path, const Vox& v) {
    std::ofstream o(path, std::ios::binary);
    if (!o) return false;
    int32_t h[3] = {v.sx, v.sy, v.sz};
    o.write((const char*)h, sizeof h);
    o.write((const char*)v.data.data(), (std::streamsize)v.data.size());
    o.write((const char*)v.pal, 768);
    return (bool)o;
}

bool read_vox(const std::string& path, Vox& v, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open " + path; return false; }
    int32_t h[3];
    in.read((char*)h, sizeof h);
    if (!in || h[0] == 542658390) { err = path + ": not a slab .vox"; return false; }
    v = Vox{};
    v.sx = h[0]; v.sy = h[1]; v.sz = h[2];
    if (v.sx < 1 || v.sy < 1 || v.sz < 1 || (long long)v.sx * v.sy * v.sz > (1 << 24)) { err = path + ": bad dims"; return false; }
    v.data.resize((size_t)v.sx * v.sy * v.sz);
    in.read((char*)v.data.data(), (std::streamsize)v.data.size());
    in.read((char*)v.pal, 768);
    if (!in) { err = path + ": short file"; return false; }
    if (in.peek() != EOF) { err = path + ": trailing bytes"; return false; }
    return true;
}

} // namespace apx
