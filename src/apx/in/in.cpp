// apx/in/in.cpp -- see in.hpp. Pure core: no SDL, no Barony headers.
#include "in.hpp"

#include <rapidjson/document.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace apx
{
namespace in
{
	namespace
	{
		const char* const kButtonNames[B_COUNT] = {
			"Attack", "Block", "Dodge", "Kick", "Use", "Cast", "Sneak", "Sprint", "Inventory", "Map", "Menu",
			"Hotbar1", "Hotbar2", "Hotbar3", "Hotbar4", "Hotbar5", "Hotbar6", "Hotbar7", "Hotbar8", "Hotbar9", "Hotbar10",
			"HotbarNext", "HotbarPrev", "Chat", "Console", "UiAccept", "UiBack", "UiNext", "UiPrev" };
		const char* const kAxisNames[X_COUNT] = { "MoveX", "MoveY", "LookX", "LookY" };
		const char* const kPadButtons[] = { "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
			"leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright" };
		const char* const kPadAxes[] = { "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger" };

		// Source ids: one flat space per device set.
		constexpr int SRC_KEY0 = 0, SRC_KEYN = 512;
		constexpr int SRC_MOUSE0 = 512;                 // + button 1..8
		constexpr int SRC_WHEELUP = 530, SRC_WHEELDN = 531;
		constexpr int SRC_PADBTN0 = 540;                // + button 0..31
		constexpr int SRC_PADAX0 = 580;                 // + axis*2 + (negative ? 1 : 0)
		constexpr int SRC_N = 600;

		Dev devOfSrc(int s)
		{
			if ( s < SRC_MOUSE0 ) { return DEV_KEY; }
			if ( s < SRC_PADBTN0 ) { return DEV_MOUSE; }
			return DEV_PAD;
		}

		struct Bind { int src; int button; int axis; float scale; };
		struct Analog { int padAxis; int axis; float scale; };
		struct Settings { float mouseSens = 1.f; bool invertY = false; float padDead = 0.2f; float padCurve = 1.5f; float padLookScale = 24.f; };

		struct Player
		{
			bool srcDown[SRC_N] = {};
			bool srcBlocked[SRC_N] = {};
			int btnHolders[B_COUNT] = {};
			uint64_t held = 0;
			uint16_t pc[B_COUNT] = {}, rc[B_COUNT] = {};
			uint32_t pms[B_COUNT] = {}, rms[B_COUNT] = {};
			float padAxis[8] = {};
			float lookDX = 0.f, lookDY = 0.f, swingDX = 0.f, swingDY = 0.f;
			Owner own[DEV_COUNT] = { OWN_VANILLA, OWN_VANILLA, OWN_VANILLA };
			TickRecord last;
		};

		// ticks: sim ticks that ran while the legacy state was 1; a release waits until it is >= 1 (old consumers sample once per tick)
		struct Cs { bool held = false, needSet = false, needClear = false, tap = false, phys = false, blocked = false; int seen = 0; int ticks = 0; int sym = 0; };

		struct State
		{
			bool loaded = false, old = false;
			Env env;
			Settings set;
			std::vector<Bind> bySrc[SRC_N];
			std::vector<Analog> analogs;
			bool mouseLook[2] = { false, false };
			float mouseLookScale[2] = { 1.f, 1.f };
			Player pl[kMaxPlayers];
			uint32_t tick = 0, nowMs = 0;
			Counters cnt;
			std::unordered_map<int, Cs> ckey;
			Cs cmouse[8];
			int wheelUp = 0, wheelDn = 0;
			// replay and log
			FILE* log = nullptr; int logRows = 0;
			std::vector<TickRecord> rows[kMaxPlayers];
			size_t rowIdx[kMaxPlayers] = {};
			bool replay = false;
			bool replayLevelKey[SRC_N] = {};
			std::vector<std::string> notes;
		};
		State S;

		int kbPlayer() { return S.env.keyboardPlayer ? S.env.keyboardPlayer() : 0; }
		bool inGame(int p) { return S.env.inGame ? S.env.inGame(p) : true; }

		int padIndex(const char* const* tab, int n, const std::string& name)
		{
			for ( int i = 0; i < n; ++i ) { if ( name == tab[i] ) { return i; } }
			return -1;
		}

		void note(const std::string& s) { S.notes.push_back(s); }

		// "key:W", "key:W:+1", "mouse:1", "mouse:x", "wheel:up", "padbtn:a", "padaxis:leftx[:-1]"
		bool addToken(int button, int axis, const std::string& tok)
		{
			std::vector<std::string> part; std::stringstream ss(tok); std::string t;
			while ( std::getline(ss, t, ':') ) { part.push_back(t); }
			if ( part.size() < 2 ) { note("bad token '" + tok + "'"); return false; }
			float scale = 1.f;
			if ( part.size() >= 3 ) { scale = (float)atof(part[2].c_str()); }
			const std::string& kind = part[0];
			int src = -1;
			if ( kind == "key" )
			{
				int sc = S.env.scancodeFromName ? S.env.scancodeFromName(part[1].c_str()) : 0;
				if ( sc <= 0 || sc >= SRC_KEYN ) { note("unknown key '" + part[1] + "' in " + tok); return false; }
				src = SRC_KEY0 + sc;
			}
			else if ( kind == "mouse" )
			{
				if ( part[1] == "x" || part[1] == "y" )
				{
					if ( axis != X_LookX && axis != X_LookY ) { note("mouse motion on a non-look action: " + tok); return false; }
					const int a = axis - X_LookX; S.mouseLook[a] = true; S.mouseLookScale[a] = scale; return true;
				}
				const int b = atoi(part[1].c_str());
				if ( b < 1 || b > 8 ) { note("bad mouse button in " + tok); return false; }
				src = SRC_MOUSE0 + b;
			}
			else if ( kind == "wheel" ) { src = part[1] == "up" ? SRC_WHEELUP : SRC_WHEELDN; }
			else if ( kind == "padbtn" )
			{
				const int b = padIndex(kPadButtons, 15, part[1]);
				if ( b < 0 ) { note("unknown pad button in " + tok); return false; }
				src = SRC_PADBTN0 + b;
			}
			else if ( kind == "padaxis" )
			{
				const int a = padIndex(kPadAxes, 6, part[1]);
				if ( a < 0 ) { note("unknown pad axis in " + tok); return false; }
				if ( axis >= 0 ) { S.analogs.push_back({ a, axis, scale }); return true; }
				src = SRC_PADAX0 + a * 2 + (scale < 0.f ? 1 : 0); // button threshold on that side of the axis
			}
			else { note("unknown token kind in " + tok); return false; }
			S.bySrc[src].push_back({ src, button, axis, scale });
			return true;
		}

		bool readFile(const std::string& path, std::string* out)
		{
			std::ifstream f(path, std::ios::binary);
			if ( !f ) { return false; }
			std::stringstream ss; ss << f.rdbuf(); *out = ss.str(); return true;
		}

		bool loadBindings(const std::string& path, bool user, std::string* err)
		{
			std::string txt;
			if ( !readFile(path, &txt) ) { if ( !user ) { *err = "cannot read " + path; } return user; }
			rapidjson::Document d; d.Parse(txt.c_str());
			if ( d.HasParseError() || !d.IsObject() ) { *err = "bad json " + path; return false; }
			if ( d.HasMember("settings") && d["settings"].IsObject() )
			{
				const auto& s = d["settings"];
				if ( s.HasMember("mouseSensitivity") && s["mouseSensitivity"].IsNumber() ) { S.set.mouseSens = (float)s["mouseSensitivity"].GetDouble(); }
				if ( s.HasMember("invertLookY") && s["invertLookY"].IsBool() ) { S.set.invertY = s["invertLookY"].GetBool(); }
				if ( s.HasMember("padDeadzone") && s["padDeadzone"].IsNumber() ) { S.set.padDead = (float)s["padDeadzone"].GetDouble(); }
				if ( s.HasMember("padLookCurve") && s["padLookCurve"].IsNumber() ) { S.set.padCurve = (float)s["padLookCurve"].GetDouble(); }
				if ( s.HasMember("padLookScale") && s["padLookScale"].IsNumber() ) { S.set.padLookScale = (float)s["padLookScale"].GetDouble(); }
			}
			if ( !d.HasMember("bindings") || !d["bindings"].IsObject() ) { *err = "no bindings object in " + path; return false; }
			for ( auto it = d["bindings"].MemberBegin(); it != d["bindings"].MemberEnd(); ++it )
			{
				const std::string name = it->name.GetString();
				int button = buttonId(name.c_str()), axis = axisId(name.c_str());
				if ( button < 0 && axis < 0 ) { note("bindings: unknown action '" + name + "'"); continue; }
				if ( user ) // a user entry replaces the defaults of that action
				{
					for ( auto& v : S.bySrc ) { v.erase(std::remove_if(v.begin(), v.end(), [&](const Bind& b) { return b.button == button && b.axis == axis; }), v.end()); }
					S.analogs.erase(std::remove_if(S.analogs.begin(), S.analogs.end(), [&](const Analog& a) { return a.axis == axis; }), S.analogs.end());
					if ( axis >= X_LookX ) { S.mouseLook[axis - X_LookX] = false; }
				}
				if ( !it->value.IsArray() ) { note("bindings: '" + name + "' is not an array"); continue; }
				for ( auto& tk : it->value.GetArray() ) { if ( tk.IsString() ) { addToken(button, axis, tk.GetString()); } }
			}
			return true;
		}

		bool loadActions(const std::string& path, std::string* err)
		{
			std::string txt;
			if ( !readFile(path, &txt) ) { *err = "cannot read " + path; return false; }
			rapidjson::Document d; d.Parse(txt.c_str());
			if ( d.HasParseError() || !d.IsObject() || !d.HasMember("actions") || !d["actions"].IsArray() ) { *err = "bad json " + path; return false; }
			bool seenB[B_COUNT] = {}; bool seenA[X_COUNT] = {};
			for ( auto& a : d["actions"].GetArray() )
			{
				if ( !a.IsObject() || !a.HasMember("name") ) { continue; }
				const std::string n = a["name"].GetString();
				const int b = buttonId(n.c_str()), x = axisId(n.c_str());
				if ( b >= 0 ) { seenB[b] = true; } else if ( x >= 0 ) { seenA[x] = true; } else { *err = "actions.json: unknown action " + n; return false; }
			}
			for ( int i = 0; i < B_COUNT; ++i ) { if ( !seenB[i] ) { *err = std::string("actions.json misses ") + kButtonNames[i]; return false; } }
			for ( int i = 0; i < X_COUNT; ++i ) { if ( !seenA[i] ) { *err = std::string("actions.json misses ") + kAxisNames[i]; return false; } }
			return true;
		}

		// ---- action side
		void pressButton(Player& P, int b, uint32_t ts)
		{
			if ( P.btnHolders[b]++ == 0 )
			{
				P.held |= (1ull << b);
				if ( P.pc[b] == 0xFFFF ) { S.cnt.saturated++; } else { P.pc[b]++; }
				P.pms[b] = ts; S.cnt.pressesLatched++;
			}
		}
		void releaseButton(Player& P, int b, uint32_t ts)
		{
			if ( P.btnHolders[b] <= 0 ) { return; }
			if ( --P.btnHolders[b] == 0 )
			{
				P.held &= ~(1ull << b);
				if ( P.rc[b] < 0xFFFF ) { P.rc[b]++; } else { S.cnt.saturated++; }
				P.rms[b] = ts; S.cnt.releasesLatched++;
			}
		}
		float axisKeys(const Player& P, int axis)
		{
			float v = 0.f;
			for ( int s = 0; s < SRC_MOUSE0; ++s )
			{
				if ( !P.srcDown[s] || P.srcBlocked[s] ) { continue; }
				for ( const Bind& b : S.bySrc[s] ) { if ( b.axis == axis ) { v += b.scale; } }
			}
			return std::max(-1.f, std::min(1.f, v));
		}
		void applySrc(Player& P, int s, bool down, uint32_t ts)
		{
			for ( const Bind& b : S.bySrc[s] )
			{
				if ( b.button < 0 ) { continue; }
				if ( down ) { pressButton(P, b.button, ts); } else { releaseButton(P, b.button, ts); }
			}
		}
		void lookAdd(Player& P, float dx, float dy)
		{
			P.lookDX += dx; P.lookDY += dy;
			if ( (P.held & (1ull << B_Attack)) || P.pc[B_Attack] ) { P.swingDX += dx; P.swingDY += dy; }
		}
		float curve(float v, float dead, float c)
		{
			const float a = std::fabs(v);
			if ( a <= dead ) { return 0.f; }
			const float n = std::min(1.f, (a - dead) / (1.f - dead));
			return (v < 0.f ? -1.f : 1.f) * std::pow(n, c);
		}

		// ---- compat side
		void cPress(Cs& c, bool apxuiOwns, bool repeat)
		{
			c.phys = true;
			if ( apxuiOwns ) { c.blocked = true; return; }
			if ( c.blocked ) { return; }
			if ( repeat ) { c.needSet = true; return; }
			c.held = true; c.needSet = true; c.needClear = false; c.tap = false; c.ticks = 0;
		}
		void cRelease(Cs& c)
		{
			const bool wasBlocked = c.blocked;
			c.phys = false; c.blocked = false;
			if ( wasBlocked ) { return; }
			if ( !c.held && !c.needSet ) { return; }
			c.held = false;
			if ( c.needSet || c.ticks == 0 ) { c.tap = true; c.seen = 0; } // no sim tick has sampled the 1 yet: keep it visible until one has
			else { c.needClear = true; }
		}
		void cDrop(Cs& c) // owner switch to APXUI: the vanilla side gets a release
		{
			if ( c.phys ) { c.blocked = true; }
			if ( c.held || c.needSet || c.tap ) { c.needClear = true; }
			c.held = false; c.needSet = false; c.tap = false;
		}

		Owner computeOwner(int p, Dev d)
		{
			if ( !inGame(p) ) { return OWN_VANILLA; }
			if ( d == DEV_MOUSE )
			{
				if ( S.env.apxuiMouse && S.env.apxuiMouse(p) ) { return OWN_APXUI; }
				if ( S.env.vanillaMouse && S.env.vanillaMouse(p) ) { return OWN_VANILLA; }
				return OWN_GAME;
			}
			if ( d == DEV_KEY )
			{
				if ( S.env.apxuiKeyboard && S.env.apxuiKeyboard(p) ) { return OWN_APXUI; }
				if ( S.env.vanillaKeyboard && S.env.vanillaKeyboard(p) ) { return OWN_VANILLA; }
				return OWN_GAME;
			}
			return OWN_GAME;
		}

		void switchOwner(int p, Dev d, Owner from, Owner to, uint32_t now)
		{
			Player& P = S.pl[p];
			S.cnt.ownerSwitches++;
			if ( from == OWN_GAME && to != OWN_GAME )
			{
				const int lo = d == DEV_KEY ? SRC_KEY0 : d == DEV_MOUSE ? SRC_MOUSE0 : SRC_PADBTN0;
				const int hi = d == DEV_KEY ? SRC_KEYN : d == DEV_MOUSE ? SRC_PADBTN0 : SRC_N;
				for ( int s = lo; s < hi; ++s )
				{
					if ( P.srcDown[s] && !P.srcBlocked[s] ) { applySrc(P, s, false, now); P.srcBlocked[s] = true; S.cnt.switchReleases++; }
				}
				if ( d == DEV_PAD ) { for ( float& a : P.padAxis ) { a = 0.f; } }
			}
			else if ( to == OWN_GAME )
			{
				const int lo = d == DEV_KEY ? SRC_KEY0 : d == DEV_MOUSE ? SRC_MOUSE0 : SRC_PADBTN0;
				const int hi = d == DEV_KEY ? SRC_KEYN : d == DEV_MOUSE ? SRC_PADBTN0 : SRC_N;
				for ( int s = lo; s < hi; ++s ) { if ( P.srcDown[s] && P.srcBlocked[s] ) { S.cnt.heldIgnored++; } }
			}
			if ( p == kbPlayer() && d != DEV_PAD )
			{
				if ( to == OWN_APXUI ) { if ( d == DEV_MOUSE ) { for ( auto& c : S.cmouse ) { cDrop(c); } } else { for ( auto& kv : S.ckey ) { cDrop(kv.second); } } }
			}
			P.own[d] = to;
		}

		void sourceEvent(int p, int s, bool down, uint32_t ts, bool repeat)
		{
			Player& P = S.pl[p];
			const Dev d = devOfSrc(s);
			if ( repeat ) { return; }
			if ( down )
			{
				if ( P.srcDown[s] ) { return; } // a duplicate press (injection on top of a held source)
				P.srcDown[s] = true;
				if ( P.own[d] != OWN_GAME )
				{
					P.srcBlocked[s] = true;
					for ( const Bind& b : S.bySrc[s] ) { if ( b.button >= 0 && inGame(p) ) { S.cnt.gatedPresses++; } }
					return;
				}
				P.srcBlocked[s] = false;
				applySrc(P, s, true, ts);
			}
			else
			{
				if ( !P.srcDown[s] ) { return; }
				P.srcDown[s] = false;
				const bool blocked = P.srcBlocked[s]; P.srcBlocked[s] = false;
				if ( !blocked ) { applySrc(P, s, false, ts); }
			}
		}

		void writeRowTo(FILE* f, const std::string& row) { fputs(row.c_str(), f); fputc('\n', f); }

		// ---- replay: translate a record into the legacy state for this tick
		void replayCompat(CompatSink& sink, int p, const TickRecord& r)
		{
			// first key or mouse source of every button, and the axis key sources
			std::vector<std::pair<int, bool>> want; // src, level
			for ( int s = 0; s < SRC_PADBTN0; ++s )
			{
				if ( s >= SRC_WHEELUP && s <= SRC_WHEELDN ) { continue; }
				for ( const Bind& b : S.bySrc[s] )
				{
					if ( b.button >= 0 )
					{
						const bool lv = r.isHeld(b.button) || r.pressCount[b.button] > 0;
						want.push_back({ s, lv });
					}
					else if ( b.axis == X_MoveX || b.axis == X_MoveY )
					{
						const float v = b.axis == X_MoveX ? r.moveX : r.moveY;
						want.push_back({ s, (b.scale > 0 ? v > 0.5f : v < -0.5f) });
					}
				}
			}
			bool level[SRC_N] = {};
			for ( auto& w : want ) { level[w.first] = level[w.first] || w.second; }
			for ( int s = 0; s < SRC_PADBTN0; ++s )
			{
				if ( s >= SRC_WHEELUP && s <= SRC_WHEELDN ) { continue; }
				bool relevant = false; for ( auto& w : want ) { if ( w.first == s ) { relevant = true; break; } }
				if ( !relevant ) { continue; }
				if ( level[s] != S.replayLevelKey[s] )
				{
					S.replayLevelKey[s] = level[s];
					if ( s < SRC_KEYN ) { const int sym = S.env.symFromScancode ? S.env.symFromScancode(s) : s; sink.setKey(sym, level[s]); }
					else { sink.setMouse(s - SRC_MOUSE0, level[s]); }
				}
			}
			for ( int b = B_HotbarNext; b <= B_HotbarPrev; ++b )
			{
				if ( r.pressCount[b] ) { sink.pulseWheel(b == B_HotbarPrev); }
			}
			if ( r.lookDX != 0.f || r.lookDY != 0.f ) { sink.addMouseRel(r.lookDX, r.lookDY); }
			(void)p;
		}

		std::string fnum(double v, bool dbl)
		{
			char b[48]; snprintf(b, sizeof(b), dbl ? "%.17g" : "%.9g", v); return b;
		}
	} // namespace

	// ---- names
	const char* buttonName(int b) { return (b >= 0 && b < B_COUNT) ? kButtonNames[b] : nullptr; }
	int buttonId(const char* n) { for ( int i = 0; i < B_COUNT; ++i ) { if ( !strcmp(n, kButtonNames[i]) ) { return i; } } return -1; }
	int axisId(const char* n) { for ( int i = 0; i < X_COUNT; ++i ) { if ( !strcmp(n, kAxisNames[i]) ) { return i; } } return -1; }

	uint64_t Counters::lost() const { return pressesLatched - pressesDelivered - pending + saturated; }

	// ---- lifecycle
	void reset() { bool old = S.old; S.~State(); new (&S) State(); S.old = old; }
	bool oldPath() { return S.old; }
	void setOldPath(bool v) { S.old = v; }
	bool loaded() { return S.loaded; }

	bool init(const char* dataDir, const char* userDir, const Env& env, std::string* err)
	{
		const bool old = S.old; FILE* lg = S.log;
		S.~State(); new (&S) State(); S.old = old; S.log = lg;
		S.env = env;
		std::string e, dd = dataDir ? dataDir : "";
		if ( !dd.empty() && dd.back() != '/' && dd.back() != '\\' ) { dd += "/"; }
		if ( !loadActions(dd + "actions.json", &e) || !loadBindings(dd + "bindings.json", false, &e) )
		{
			if ( err ) { *err = e; }
			return false;
		}
		if ( userDir && userDir[0] )
		{
			std::string ud = userDir; if ( ud.back() != '/' && ud.back() != '\\' ) { ud += "/"; }
			std::string e2;
			if ( !loadBindings(ud + "bindings.user.json", true, &e2) ) { note("user bindings: " + e2); }
		}
		S.loaded = true;
		if ( err )
		{
			std::string all; for ( auto& n : S.notes ) { all += n + "; "; }
			*err = all; // non-fatal notes, empty when clean
		}
		return true;
	}

	// ---- per frame
	void beginFrame(uint32_t nowMs)
	{
		if ( !S.loaded || S.old ) { return; }
		S.nowMs = nowMs;
		for ( int p = 0; p < kMaxPlayers; ++p )
		{
			for ( int d = 0; d < DEV_COUNT; ++d )
			{
				const Owner o = computeOwner(p, (Dev)d);
				if ( o != S.pl[p].own[d] ) { switchOwner(p, (Dev)d, S.pl[p].own[d], o, nowMs); }
			}
		}
	}

	void event(const Raw& e)
	{
		if ( !S.loaded || S.old ) { return; }
		S.cnt.events++;
		if ( S.replay ) { return; } // a replay is driven by the file only
		const uint32_t ts = e.tsMs ? e.tsMs : S.nowMs;
		const int kp = kbPlayer();
		switch ( e.type )
		{
		case R_KeyDown: case R_KeyUp:
		{
			const bool down = e.type == R_KeyDown;
			if ( e.code > 0 && e.code < SRC_KEYN ) { sourceEvent(kp, SRC_KEY0 + e.code, down, ts, e.repeat); }
			Cs& c = S.ckey[e.sym]; c.sym = e.sym;
			if ( down ) { cPress(c, S.pl[kp].own[DEV_KEY] == OWN_APXUI, e.repeat); } else { cRelease(c); }
			break;
		}
		case R_MouseDown: case R_MouseUp:
		{
			const bool down = e.type == R_MouseDown;
			if ( e.code >= 1 && e.code <= 8 )
			{
				sourceEvent(kp, SRC_MOUSE0 + e.code, down, ts, false);
				if ( e.code < 8 ) { Cs& c = S.cmouse[e.code]; if ( down ) { cPress(c, S.pl[kp].own[DEV_MOUSE] == OWN_APXUI, false); } else { cRelease(c); } }
			}
			break;
		}
		case R_Wheel:
		{
			if ( e.value == 0.f ) { break; }
			const bool up = e.value > 0.f;
			Player& P = S.pl[kp];
			if ( P.own[DEV_MOUSE] == OWN_GAME ) { const int s = up ? SRC_WHEELUP : SRC_WHEELDN; applySrc(P, s, true, ts); applySrc(P, s, false, ts); }
			else if ( inGame(kp) ) { for ( const Bind& b : S.bySrc[up ? SRC_WHEELUP : SRC_WHEELDN] ) { if ( b.button >= 0 ) { S.cnt.gatedPresses++; } } }
			if ( P.own[DEV_MOUSE] != OWN_APXUI ) { if ( up ) { S.wheelUp++; } else { S.wheelDn++; } }
			break;
		}
		case R_MouseMove:
		{
			Player& P = S.pl[kp];
			if ( P.own[DEV_MOUSE] != OWN_GAME ) { break; }
			float dx = S.mouseLook[0] ? e.dx * S.set.mouseSens * S.mouseLookScale[0] : 0.f;
			float dy = S.mouseLook[1] ? e.dy * S.set.mouseSens * S.mouseLookScale[1] : 0.f;
			if ( S.set.invertY ) { dy = -dy; }
			lookAdd(P, dx, dy);
			break;
		}
		case R_PadDown: case R_PadUp:
		{
			const int p = S.env.padPlayer ? S.env.padPlayer(e.pad) : -1;
			if ( p < 0 || p >= kMaxPlayers || e.code < 0 || e.code >= 32 ) { break; }
			sourceEvent(p, SRC_PADBTN0 + e.code, e.type == R_PadDown, ts, false);
			break;
		}
		case R_PadAxis:
		{
			const int p = S.env.padPlayer ? S.env.padPlayer(e.pad) : -1;
			if ( p < 0 || p >= kMaxPlayers || e.code < 0 || e.code >= 6 ) { break; }
			Player& P = S.pl[p];
			if ( P.own[DEV_PAD] != OWN_GAME ) { break; }
			P.padAxis[e.code] = e.value;
			// threshold sources (triggers as buttons)
			for ( int side = 0; side < 2; ++side )
			{
				const int s = SRC_PADAX0 + e.code * 2 + side;
				if ( S.bySrc[s].empty() ) { continue; }
				const bool on = side == 0 ? e.value > 0.5f : e.value < -0.5f;
				if ( on != P.srcDown[s] ) { sourceEvent(p, s, on, ts, false); }
			}
			break;
		}
		}
	}

	void endFrame(CompatSink& sink)
	{
		if ( !S.loaded || S.old ) { return; }
		for ( auto& kv : S.ckey )
		{
			Cs& c = kv.second;
			if ( c.needSet ) { sink.setKey(c.sym, true); c.needSet = false; }
			if ( c.needClear ) { sink.setKey(c.sym, false); c.needClear = false; }
		}
		for ( int b = 1; b < 8; ++b )
		{
			Cs& c = S.cmouse[b];
			if ( c.needSet ) { sink.setMouse(b, true); c.needSet = false; }
			if ( c.needClear ) { sink.setMouse(b, false); c.needClear = false; }
		}
		if ( S.wheelUp > 0 ) { sink.pulseWheel(true); S.wheelUp = 0; }
		if ( S.wheelDn > 0 ) { sink.pulseWheel(false); S.wheelDn = 0; }
	}

	// ---- per tick
	TickRecord takeNow(int p)
	{
		TickRecord r;
		if ( p < 0 || p >= kMaxPlayers ) { return r; }
		Player& P = S.pl[p];
		r.tick = S.tick;
		r.held = P.held;
		for ( int b = 0; b < B_COUNT; ++b )
		{
			r.pressCount[b] = P.pc[b]; r.releaseCount[b] = P.rc[b]; r.lastPressMs[b] = P.pms[b]; r.lastReleaseMs[b] = P.rms[b];
			S.cnt.pressesDelivered += P.pc[b]; S.cnt.releasesDelivered += P.rc[b]; S.cnt.takenPress[b] += P.pc[b];
			P.pc[b] = 0; P.rc[b] = 0;
		}
		float mx = axisKeys(P, X_MoveX), my = axisKeys(P, X_MoveY), lx = 0.f, ly = 0.f;
		if ( P.own[DEV_PAD] == OWN_GAME )
		{
			for ( const Analog& a : S.analogs )
			{
				const float v = P.padAxis[a.padAxis] * a.scale;
				if ( a.axis == X_MoveX ) { mx += curve(v, S.set.padDead, 1.f); }
				else if ( a.axis == X_MoveY ) { my += curve(v, S.set.padDead, 1.f); }
				else if ( a.axis == X_LookX ) { lx += curve(v, S.set.padDead, S.set.padCurve) * S.set.padLookScale; }
				else if ( a.axis == X_LookY ) { ly += curve(v, S.set.padDead, S.set.padCurve) * S.set.padLookScale; }
			}
		}
		r.moveX = std::max(-1.f, std::min(1.f, mx)); r.moveY = std::max(-1.f, std::min(1.f, my));
		if ( S.set.invertY ) { ly = -ly; }
		if ( lx != 0.f || ly != 0.f ) { lookAdd(P, lx, ly); }
		r.lookDX = P.lookDX; r.lookDY = P.lookDY; r.swingDX = P.swingDX; r.swingDY = P.swingDY;
		P.lookDX = P.lookDY = P.swingDX = P.swingDY = 0.f;
		S.cnt.takes++;
		P.last = r;
		return r;
	}

	const TickRecord& take(int p)
	{
		static TickRecord none;
		if ( p < 0 || p >= kMaxPlayers ) { return none; }
		return S.pl[p].last;
	}

	void tickAll(CompatSink& sink)
	{
		if ( !S.loaded || S.old ) { return; }
		// compat: taps that were visible for one tick end now
		for ( auto& kv : S.ckey )
		{
			Cs& c = kv.second;
			if ( c.tap ) { if ( c.seen >= 1 ) { sink.setKey(c.sym, false); c.tap = false; } else { c.seen++; } }
			else if ( c.held ) { c.ticks = 1; }
		}
		for ( int b = 1; b < 8; ++b )
		{
			Cs& c = S.cmouse[b];
			if ( c.tap ) { if ( c.seen >= 1 ) { sink.setMouse(b, false); c.tap = false; } else { c.seen++; } }
			else if ( c.held ) { c.ticks = 1; }
		}
		for ( int p = 0; p < kMaxPlayers; ++p )
		{
			if ( !inGame(p) ) { S.pl[p].last = TickRecord(); S.pl[p].last.tick = S.tick; continue; }
			TickRecord r;
			if ( S.replay )
			{
				if ( S.rowIdx[p] < S.rows[p].size() ) { r = S.rows[p][S.rowIdx[p]++]; }
				else { r.tick = S.tick; }
				S.pl[p].last = r;
				replayCompat(sink, p, r);
				S.cnt.takes++;
			}
			else { r = takeNow(p); }
			if ( S.log )
			{
				double x = 0, y = 0, yaw = 0; bool have = false;
				if ( S.env.playerPos ) { have = S.env.playerPos(p, &x, &y, &yaw); }
				writeRowTo(S.log, recordRow(p, r, have, x, y, yaw));
				if ( ++S.logRows % 50 == 0 ) { fflush(S.log); }
			}
		}
		++S.tick;
	}

	bool uiOwnsMouse(int p) { return p >= 0 && p < kMaxPlayers && S.pl[p].own[DEV_MOUSE] != OWN_GAME; }
	bool uiOwnsKeyboard(int p) { return p >= 0 && p < kMaxPlayers && S.pl[p].own[DEV_KEY] != OWN_GAME; }
	Owner mouseOwner(int p) { return (p >= 0 && p < kMaxPlayers) ? S.pl[p].own[DEV_MOUSE] : OWN_VANILLA; }
	Owner keyboardOwner(int p) { return (p >= 0 && p < kMaxPlayers) ? S.pl[p].own[DEV_KEY] : OWN_VANILLA; }
	uint32_t tickCounter() { return S.tick; }

	bool resolveAction(const char* action, Raw* down, Raw* up)
	{
		const int b = buttonId(action);
		if ( b < 0 ) { return false; }
		for ( int s = 0; s < SRC_PADBTN0; ++s )
		{
			if ( s >= SRC_WHEELUP && s <= SRC_WHEELDN ) { continue; }
			for ( const Bind& bd : S.bySrc[s] )
			{
				if ( bd.button != b ) { continue; }
				Raw d; d.type = s < SRC_KEYN ? R_KeyDown : R_MouseDown; d.code = s < SRC_KEYN ? s : s - SRC_MOUSE0;
				d.sym = (s < SRC_KEYN && S.env.symFromScancode) ? S.env.symFromScancode(s) : 0;
				Raw u = d; u.type = s < SRC_KEYN ? R_KeyUp : R_MouseUp;
				if ( down ) { *down = d; }
				if ( up ) { *up = u; }
				return true;
			}
		}
		return false;
	}

	const Counters& counters()
	{
		static Counters c;
		c = S.cnt;
		uint64_t pend = 0;
		for ( auto& P : S.pl ) { for ( int b = 0; b < B_COUNT; ++b ) { pend += P.pc[b]; } }
		c.pending = pend;
		return c;
	}

	std::string countersJson()
	{
		const Counters& c = counters();
		char b[512];
		snprintf(b, sizeof(b), "\"events\":%llu,\"press\":%llu,\"press_taken\":%llu,\"rel\":%llu,\"rel_taken\":%llu,\"gated\":%llu,\"switches\":%llu,\"switch_rel\":%llu,\"held_ignored\":%llu,\"lost\":%llu,\"takes\":%llu,\"old\":%d",
			(unsigned long long)c.events, (unsigned long long)c.pressesLatched, (unsigned long long)c.pressesDelivered,
			(unsigned long long)c.releasesLatched, (unsigned long long)c.releasesDelivered, (unsigned long long)c.gatedPresses,
			(unsigned long long)c.ownerSwitches, (unsigned long long)c.switchReleases, (unsigned long long)c.heldIgnored,
			(unsigned long long)c.lost(), (unsigned long long)c.takes, S.old ? 1 : 0);
		return b;
	}

	// ---- log and replay
	std::string recordRow(int p, const TickRecord& r, bool have, double x, double y, double yaw)
	{
		std::string s = "{\"k\":" + std::to_string(r.tick) + ",\"p\":" + std::to_string(p) + ",\"h\":" + std::to_string((unsigned long long)r.held);
		auto arr = [&](const char* key, const uint16_t* a)
		{
			bool any = false; std::string t;
			for ( int b = 0; b < B_COUNT; ++b ) { if ( a[b] ) { t += std::string(any ? "," : "") + "\"" + kButtonNames[b] + "\":" + std::to_string(a[b]); any = true; } }
			if ( any ) { s += std::string(",\"") + key + "\":{" + t + "}"; }
		};
		auto tms = [&](const char* key, const uint32_t* a, const uint16_t* cnt)
		{
			bool any = false; std::string t;
			for ( int b = 0; b < B_COUNT; ++b ) { if ( cnt[b] ) { t += std::string(any ? "," : "") + "\"" + kButtonNames[b] + "\":" + std::to_string(a[b]); any = true; } }
			if ( any ) { s += std::string(",\"") + key + "\":{" + t + "}"; }
		};
		arr("pc", r.pressCount); arr("rc", r.releaseCount); tms("pt", r.lastPressMs, r.pressCount); tms("rt", r.lastReleaseMs, r.releaseCount);
		auto f = [&](const char* k, float v) { if ( v != 0.f ) { s += std::string(",\"") + k + "\":" + fnum(v, false); } };
		f("mx", r.moveX); f("my", r.moveY); f("lx", r.lookDX); f("ly", r.lookDY); f("sx", r.swingDX); f("sy", r.swingDY);
		if ( have ) { s += ",\"pos\":[" + fnum(x, true) + "," + fnum(y, true) + "," + fnum(yaw, true) + "]"; }
		s += "}";
		return s;
	}

	bool parseRow(const std::string& line, int* player, TickRecord* out)
	{
		rapidjson::Document d; d.Parse(line.c_str());
		if ( d.HasParseError() || !d.IsObject() || !d.HasMember("k") || !d.HasMember("p") ) { return false; }
		TickRecord r;
		r.tick = d["k"].GetUint(); *player = d["p"].GetInt();
		if ( d.HasMember("h") ) { r.held = d["h"].GetUint64(); }
		auto rd = [&](const char* key, uint16_t* a)
		{
			if ( !d.HasMember(key) || !d[key].IsObject() ) { return; }
			for ( auto it = d[key].MemberBegin(); it != d[key].MemberEnd(); ++it ) { const int b = buttonId(it->name.GetString()); if ( b >= 0 ) { a[b] = (uint16_t)it->value.GetUint(); } }
		};
		auto rt = [&](const char* key, uint32_t* a)
		{
			if ( !d.HasMember(key) || !d[key].IsObject() ) { return; }
			for ( auto it = d[key].MemberBegin(); it != d[key].MemberEnd(); ++it ) { const int b = buttonId(it->name.GetString()); if ( b >= 0 ) { a[b] = it->value.GetUint(); } }
		};
		rd("pc", r.pressCount); rd("rc", r.releaseCount); rt("pt", r.lastPressMs); rt("rt", r.lastReleaseMs);
		auto f = [&](const char* k, float* v) { if ( d.HasMember(k) && d[k].IsNumber() ) { *v = (float)d[k].GetDouble(); } };
		f("mx", &r.moveX); f("my", &r.moveY); f("lx", &r.lookDX); f("ly", &r.lookDY); f("sx", &r.swingDX); f("sy", &r.swingDY);
		*out = r;
		return true;
	}

	bool logOpen(const char* path) { logClose(); S.log = fopen(path, "wb"); S.logRows = 0; return S.log != nullptr; }
	void logClose() { if ( S.log ) { fclose(S.log); S.log = nullptr; } }
	bool logEnabled() { return S.log != nullptr; }

	bool replayOpen(const char* path, std::string* err)
	{
		std::ifstream f(path);
		if ( !f ) { if ( err ) { *err = std::string("cannot read ") + path; } return false; }
		for ( auto& v : S.rows ) { v.clear(); }
		std::string line; size_t n = 0;
		while ( std::getline(f, line) )
		{
			if ( line.empty() ) { continue; }
			int p = 0; TickRecord r;
			if ( !parseRow(line, &p, &r) || p < 0 || p >= kMaxPlayers ) { if ( err ) { *err = "bad row " + std::to_string(n + 1); } return false; }
			S.rows[p].push_back(r); ++n;
		}
		for ( auto& i : S.rowIdx ) { i = 0; }
		S.replay = n > 0;
		return n > 0;
	}
	bool replaying() { return S.replay; }
	bool replayDone()
	{
		if ( !S.replay ) { return false; }
		for ( int p = 0; p < kMaxPlayers; ++p ) { if ( S.rowIdx[p] < S.rows[p].size() ) { return false; } }
		return true;
	}
} // namespace in
} // namespace apx
