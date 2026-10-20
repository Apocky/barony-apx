// F6 data loader: a small hand JSON parser (no new dependency, card BF6-1) and the DB lookups.
#include <cctype>
#include "parts_model.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

namespace apxparts {
namespace {

struct J
{
	enum T { NUL, NUM, STR, ARR, OBJ, BOOL } t = NUL;
	double n = 0;
	std::string s;
	std::vector<J> a;
	std::vector<std::pair<std::string, J>> o;
	const J* get(const std::string& k) const { for ( auto& p : o ) { if ( p.first == k ) { return &p.second; } } return nullptr; }
	std::string str(const std::string& k, const std::string& d = "") const { const J* v = get(k); return v && v->t == STR ? v->s : d; }
	int num(const std::string& k, int d = 0) const { const J* v = get(k); return v && v->t == NUM ? (int)v->n : d; }
	const std::vector<J>& arr(const std::string& k) const { static const std::vector<J> e; const J* v = get(k); return v && v->t == ARR ? v->a : e; }
};

struct Parser
{
	const std::string& src;
	size_t i = 0;
	std::string err;
	explicit Parser(const std::string& s) : src(s) {}
	void ws() { while ( i < src.size() && (src[i] == ' ' || src[i] == '\n' || src[i] == '\r' || src[i] == '\t') ) { ++i; } }
	bool fail(const std::string& m) { if ( err.empty() ) { err = m + " at byte " + std::to_string(i); } return false; }
	bool string(std::string& out)
	{
		if ( src[i] != '"' ) { return fail("expected string"); }
		++i;
		while ( i < src.size() && src[i] != '"' )
		{
			if ( src[i] == '\\' && i + 1 < src.size() ) { ++i; out += src[i] == 'n' ? '\n' : src[i]; ++i; continue; }
			out += src[i++];
		}
		if ( i >= src.size() ) { return fail("unterminated string"); }
		++i;
		return true;
	}
	bool value(J& v, int depth = 0)
	{
		if ( depth > 32 ) { return fail("too deep"); }
		ws();
		if ( i >= src.size() ) { return fail("unexpected end"); }
		char c = src[i];
		if ( c == '{' )
		{
			v.t = J::OBJ; ++i; ws();
			if ( i < src.size() && src[i] == '}' ) { ++i; return true; }
			for ( ;; )
			{
				ws();
				std::string k;
				if ( !string(k) ) { return false; }
				ws();
				if ( i >= src.size() || src[i] != ':' ) { return fail("expected :"); }
				++i;
				J e;
				if ( !value(e, depth + 1) ) { return false; }
				v.o.push_back({ k, std::move(e) });
				ws();
				if ( i < src.size() && src[i] == ',' ) { ++i; continue; }
				if ( i < src.size() && src[i] == '}' ) { ++i; return true; }
				return fail("expected , or }");
			}
		}
		if ( c == '[' )
		{
			v.t = J::ARR; ++i; ws();
			if ( i < src.size() && src[i] == ']' ) { ++i; return true; }
			for ( ;; )
			{
				J e;
				if ( !value(e, depth + 1) ) { return false; }
				v.a.push_back(std::move(e));
				ws();
				if ( i < src.size() && src[i] == ',' ) { ++i; continue; }
				if ( i < src.size() && src[i] == ']' ) { ++i; return true; }
				return fail("expected , or ]");
			}
		}
		if ( c == '"' ) { v.t = J::STR; return string(v.s); }
		if ( !src.compare(i, 4, "true") ) { v.t = J::BOOL; v.n = 1; i += 4; return true; }
		if ( !src.compare(i, 5, "false") ) { v.t = J::BOOL; i += 5; return true; }
		if ( !src.compare(i, 4, "null") ) { i += 4; return true; }
		char* end = nullptr;
		v.n = strtod(src.c_str() + i, &end);
		if ( end == src.c_str() + i ) { return fail("bad value"); }
		v.t = J::NUM;
		i = end - src.c_str();
		return true;
	}
};

std::string readJson(const std::string& path, J& out)
{
	std::ifstream in(path, std::ios::binary);
	if ( !in ) { return "cannot open " + path; }
	std::stringstream ss;
	ss << in.rdbuf();
	std::string src = ss.str();
	Parser p(src);
	if ( !p.value(out) ) { return path + ": " + p.err; }
	return "";
}

Role roleOf(const std::string& r)
{
	return r == "strike" ? ROLE_STRIKE : r == "structural" ? ROLE_STRUCTURAL : r == "balance" ? ROLE_BALANCE : r == "grip" ? ROLE_GRIP : ROLE_OTHER;
}

} // namespace

int DB::material(const std::string& id) const { for ( size_t k = 0; k < materials.size(); ++k ) { if ( materials[k].id == id ) { return (int)k; } } return -1; }
int DB::slot(const std::string& id) const { for ( size_t k = 0; k < slots.size(); ++k ) { if ( slots[k].id == id ) { return (int)k; } } return -1; }
int DB::family(const std::string& id) const { for ( size_t k = 0; k < families.size(); ++k ) { if ( families[k].id == id ) { return (int)k; } } return -1; }
const TypeForm* DB::typeForm(const std::string& n) const
{
	auto it = std::lower_bound(typeForms.begin(), typeForms.end(), n, [](const std::pair<std::string, TypeForm>& a, const std::string& b) { return a.first < b; });
	return it != typeForms.end() && it->first == n ? &it->second : nullptr;
}
const AffixPool* DB::pool(int fam, int sl) const
{
	const std::string a = (fam >= 0 ? families[fam].id : std::string("?")) + "/" + (sl >= 0 ? slots[sl].id : std::string("?"));
	const std::string b = sl >= 0 ? slots[sl].id : std::string("?");
	const AffixPool* star = nullptr;
	for ( auto& p : pools ) { if ( p.key == a ) { return &p; } }
	for ( auto& p : pools ) { if ( p.key == b ) { return &p; } if ( p.key == "*" ) { star = &p; } }
	return star;
}

std::string loadDB(const std::string& dir, DB& db)
{
	db = DB();
	J m, f, fm, af, sv;
	std::string e;
	if ( !(e = readJson(dir + "/materials.json", m)).empty() ) { return e; }
	if ( !(e = readJson(dir + "/forms.json", f)).empty() ) { return e; }
	if ( !(e = readJson(dir + "/form_map.json", fm)).empty() ) { return e; }
	if ( !(e = readJson(dir + "/affixes.json", af)).empty() ) { return e; }
	if ( !(e = readJson(dir + "/salvage.json", sv)).empty() ) { return e; }
	for ( auto& r : m.arr("materials") )
	{
		Material x;
		x.id = r.str("id"); x.density = r.num("density"); x.hardness = r.num("hardness"); x.tier = r.num("tier", 1);
		x.valuePerKg = r.num("value_per_kg"); x.meltC = r.num("melt_c"); x.flammable = r.num("flammable"); x.element = r.num("element");
		x.salvageClass = r.str("salvage", "none");
		{ const auto& t = r.arr("tint"); for ( size_t k = 0; k < 3 && k < t.size(); ++k ) { x.tint[k] = (float)t[k].n; } }
		if ( x.id.empty() || db.material(x.id) >= 0 ) { return "materials.json: missing or duplicate id " + x.id; }
		db.materials.push_back(x);
	}
	if ( db.materials.size() > 255 ) { return "materials.json: over 255 materials (part.material is a byte)"; }
	for ( auto& r : f.arr("slots") )
	{
		Slot s; s.id = r.str("id"); s.role = roleOf(r.str("role")); s.volume = r.num("volume"); s.quick = r.num("quick") != 0;
		if ( s.id.empty() || db.slot(s.id) >= 0 ) { return "forms.json: missing or duplicate slot " + s.id; }
		db.slots.push_back(s);
	}
	for ( auto& r : f.arr("families") )
	{
		Family x; x.id = r.str("id");
		for ( auto& s : r.arr("required") ) { int k = db.slot(s.s); if ( k < 0 ) { return "forms.json: unknown slot " + s.s; } x.required.push_back(k); }
		for ( auto& o : r.arr("optional") ) { int k = db.slot(o.str("slot")); if ( k < 0 ) { return "forms.json: unknown slot " + o.str("slot"); } x.optional.push_back({ k, o.num("q") }); }
		if ( x.id.empty() || db.family(x.id) >= 0 ) { return "forms.json: missing or duplicate family " + x.id; }
		db.families.push_back(x);
	}
	db.plainFamily = db.family("Plain");
	db.fleshMaterial = db.material("flesh");
	if ( db.plainFamily < 0 ) { return "forms.json: no Plain family (the fallback)"; }
	for ( auto& p : fm.o )
	{
		TypeForm tf;
		tf.family = db.family(p.second.str("family"));
		if ( tf.family < 0 ) { return "form_map.json: unknown family for " + p.first; }
		for ( auto& mm : p.second.arr("materials") ) { int k = db.material(mm.s); if ( k < 0 ) { return "form_map.json: unknown material " + mm.s; } tf.materials.push_back(k); }
		if ( tf.materials.empty() ) { tf.materials.push_back(0); }
		db.typeForms.push_back({ p.first, tf });
	}
	std::sort(db.typeForms.begin(), db.typeForms.end(), [](const std::pair<std::string, TypeForm>& a, const std::pair<std::string, TypeForm>& b) { return a.first < b.first; });
	for ( auto& p : af.arr("pools") )
	{
		AffixPool ap; ap.key = p.str("id"); ap.firstId = (int)db.affixPct.size();
		for ( auto& a : p.arr("affixes") )
		{
			db.affixPct.push_back({ a.str("name"), a.num("pct") });
			AffixInfo ai;
			ai.name = a.str("name"); ai.pct = a.num("pct"); ai.stat = a.str("stat", ai.name.c_str());
			ai.weight = std::max(1, std::min(100, a.num("weight", 1)));
			// content-lane keys (2026-10-19): "pos", "word" ("Keen" / "of Embers"), "stat", "weight" 1-100
			const std::string pos = a.str("pos", a.str("position"));
			ai.label = a.str("word", a.str("label"));
			if ( pos.empty() || ai.label.empty() ) { ++db.affixDefaults; }
			if ( pos == "prefix" ) { ai.position = AFFIX_PREFIX; }
			else if ( pos.empty() || pos == "suffix" ) { ai.position = AFFIX_SUFFIX; }
			else { return "affixes.json: affix " + ai.name + " position must be prefix or suffix, not " + pos; }
			if ( ai.label.empty() )
			{
				bool up = true;
				for ( char ch : ai.name ) { ai.label += up ? (char)toupper((unsigned char)ch) : ch; up = ch == ' '; }
				if ( ai.position == AFFIX_SUFFIX ) { ai.label = "of " + ai.label; }
			}
			db.affixes.push_back(ai);
		}
		ap.count = (int)db.affixPct.size() - ap.firstId;
		if ( ap.count < 4 ) { return "affixes.json: pool " + ap.key + " has under 4 affixes (Legendary takes 4)"; }
		db.pools.push_back(ap);
	}
	SalvageParams& s = db.salvage;
	s.lootedPct = sv.num("looted_return_pct", 60); s.craftedPct = sv.num("crafted_return_pct", 100);
	s.dischargePct = sv.num("discharge_pct", 20); s.auraPerAffixPct = sv.num("aura_per_affix_pct", 25);
	s.lowIntegrityByte = sv.num("low_integrity_byte", 77); s.lowIntegrityFactorPct = sv.num("low_integrity_factor_pct", 50);
	s.auraItem = sv.str("aura_item", "GEM_ROCK");
	for ( auto& r : sv.arr("stand_in") ) { s.standIns.push_back({ r.str("class"), r.str("item"), r.num("grams") }); }
	return "";
}

} // namespace apxparts
