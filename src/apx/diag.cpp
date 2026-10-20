// apx/diag.cpp -- D90 always-on local diagnostics. See diag.hpp for the row list and the privacy rule (local only).
#include "diag.hpp"
#include "../main.hpp"
#include "../game.hpp"
#include "../files.hpp"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifdef WINDOWS
#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>
#include <direct.h>
#include <io.h>
#include <GL/glew.h>
#pragma comment(lib, "dbghelp.lib")
#else
#include <GL/glew.h>
#include <sys/stat.h>
#endif

namespace apxdiag
{
	bool on = true;
	bool sampleTick = false;

	namespace
	{
		std::mutex gMu;
		FILE* gFile = nullptr;
		std::string gPath, gDir = "diag", gSessionStamp;
		int64_t gStartUs = 0;
		double gUsPerTick = 0;
		uint64_t gBytes = 0;
		const uint64_t kSoftCap = 12ull << 20, kHardCap = 16ull << 20;
		unsigned long gMainTid = 0;
		bool gCapNoted = false;

		// ring of the last events, for the crash report
		const int kRing = 256;
		std::array<std::string, kRing> gRing;
		int gRingN = 0;

		// frame / second accumulation (main thread only)
		int64_t gLastFrameUs = 0, gSecStartUs = 0;
		int64_t gFrameSys[S_N] = {}, gSecSys[S_N] = {};
		std::vector<float> gIv, gWork;
		int64_t gOverheadUs = 0, gScopes = 0;
		double gScopeCostUs = 0.05;
		float gMedianWork = 8.f;
		int gSpikesThisSec = 0;
		int gSecIndex = 0;
		bool gGlDone = false;
		uint64_t gErrTotal = 0, gWarnTotal = 0;
		uint32_t gFrames = 0;
		int gPlantCrashTick = -1, gPlantStallTick = -1, gPlantErrTick = -1, gPlantExitTick = -1;

		struct ErrKey { uint64_t count = 0; int level = 0; std::string sample; };
		std::map<std::string, ErrKey> gErr;
		int64_t gLastErrSumUs = 0;
		bool gCrashing = false;

		const char* kSysName[S_N] = { "logic", "ai_monster", "ai_player", "ent_other", "render", "ui", "swap", "limiter", "net", "steam",
			"load", "vision", "voxel", "tactics", "flow", "director", "other" };

		std::string jesc(const char* s)
		{
			std::string o;
			for ( ; s && *s; ++s )
			{
				unsigned char c = (unsigned char)*s;
				if ( c == '"' || c == '\\' ) { o += '\\'; o += (char)c; }
				else if ( c == '\n' || c == '\r' || c == '\t' ) { o += ' '; }
				else if ( c < 32 || c > 126 ) { o += '?'; }
				else { o += (char)c; }
			}
			return o;
		}

		double pct(std::vector<float>& v, double q)
		{
			if ( v.empty() ) { return 0; }
			std::sort(v.begin(), v.end());
			double k = (v.size() - 1) * q;
			size_t lo = (size_t)k, hi = std::min(lo + 1, v.size() - 1);
			return v[lo] + (v[hi] - v[lo]) * (k - lo);
		}

		// error / warning classifier. returns 0 none, 1 warning, 2 error
		int classify(const char* line)
		{
			std::string l;
			for ( const char* p = line; *p && l.size() < 400; ++p ) { l += (char)tolower((unsigned char)*p); }
			if ( l.find("no error") != std::string::npos ) { return 0; }
			if ( l.compare(0, 5, "[apx]") == 0 && l.find("fail") == std::string::npos && l.find("error") == std::string::npos ) { return 0; }
			static const char* errs[] = { "error", "could not", "couldn't", "unable to", "failed", "critical", "assert", "exception", "cannot",
				"not found", "invalid", "fatal", "corrupt" };
			for ( const char* e : errs ) { if ( l.find(e) != std::string::npos ) { return 2; } }
			static const char* warns[] = { "warning", "warn:", "deprecated", "missing", "skipping", "ignoring" };
			for ( const char* w : warns ) { if ( l.find(w) != std::string::npos ) { return 1; } }
			return 0;
		}

		std::string normaliseKey(const char* line)
		{
			std::string k;
			bool inNum = false;
			for ( const char* p = line; *p && k.size() < 140; ++p )
			{
				char c = *p;
				if ( c == '\n' || c == '\r' ) { break; }
				if ( isdigit((unsigned char)c) ) { if ( !inNum ) { k += '#'; } inNum = true; continue; }
				inNum = false;
				k += c;
			}
			return k;
		}

		void ringPush(const std::string& s)
		{
			gRing[gRingN % kRing] = s.size() > 400 ? s.substr(0, 400) : s;
			++gRingN;
		}

		// caller holds gMu
		void writeRowLocked(const std::string& row, bool urgent)
		{
			ringPush(row);
			if ( !gFile ) { return; }
			if ( gBytes > kHardCap ) { return; }
			if ( gBytes > kSoftCap && !urgent )
			{
				if ( !gCapNoted )
				{
					gCapNoted = true;
					fputs("{\"t\":\"capped\",\"note\":\"12 MB session cap reached: only err/spike/crash rows from here\"}\n", gFile);
				}
				return;
			}
			fwrite(row.data(), 1, row.size(), gFile);
			fputc('\n', gFile);
			gBytes += row.size() + 1;
			if ( urgent ) { fflush(gFile); }
		}

		std::string head(const char* type)
		{
			char b[160];
			double ms = (nowUs() - gStartUs) / 1000.0;
			snprintf(b, sizeof(b), "{\"t\":\"%s\",\"ms\":%.1f,\"tick\":%u,\"lvl\":%d", type, ms, (unsigned)ticks, (int)currentlevel);
			return b;
		}

		void emit(const char* type, const std::string& kv, bool urgent)
		{
			std::string r = head(type);
			if ( !kv.empty() ) { r += ","; r += kv; }
			r += "}";
			{
				std::lock_guard<std::mutex> lk(gMu);
				writeRowLocked(r, urgent);
			}
			upload::noteRow(type, r);
		}

		void mkdirOne(const std::string& d)
		{
#ifdef WINDOWS
			_mkdir(d.c_str());
#else
			mkdir(d.c_str(), 0777);
#endif
		}

		void rotate(const char* prefix, const char* ext, int keep)
		{
#ifdef WINDOWS
			std::vector<std::string> names;
			_finddata_t fd;
			std::string pat = gDir + "/" + prefix + "*" + ext;
			intptr_t h = _findfirst(pat.c_str(), &fd);
			if ( h == -1 ) { return; }
			do { names.push_back(fd.name); } while ( _findnext(h, &fd) == 0 );
			_findclose(h);
			std::sort(names.begin(), names.end());
			for ( int i = 0; i + keep < (int)names.size(); ++i ) { remove((gDir + "/" + names[i]).c_str()); }
#endif
		}

		std::string hexTime()
		{
			time_t t = time(nullptr);
			char b[32];
			strftime(b, sizeof(b), "%Y%m%d-%H%M%S", localtime(&t));
			return b;
		}

		void writeSessionRows(int argc, char** argv)
		{
			std::string kv = "\"sha\":\"";
#ifdef APX_GIT_SHA
			kv += APX_GIT_SHA;
#else
			kv += "unknown";
#endif
			kv += "\",\"built\":\"" + std::string(__DATE__ " " __TIME__) + "\",\"pid\":" + std::to_string(
#ifdef WINDOWS
				GetCurrentProcessId()
#else
				0
#endif
				);
#ifdef STEAMWORKS
			kv += ",\"steamworks\":1";
#else
			kv += ",\"steamworks\":0";
#endif
#ifdef USE_EOS
			kv += ",\"eos\":1";
#else
			kv += ",\"eos\":0";
#endif
			char cwd[512] = {};
#ifdef WINDOWS
			_getcwd(cwd, sizeof(cwd));
#endif
			kv += ",\"cwd\":\"" + jesc(cwd) + "\"";
			const char* sid = getenv("SteamAppId");
			kv += ",\"steam_app_id\":\"" + jesc(sid ? sid : "") + "\"";
			emit("session", kv, true);

			std::string hw;
#ifdef WINDOWS
			int cpu[4] = {};
			char brand[49] = {};
			__cpuid(cpu, 0x80000000);
			if ( (unsigned)cpu[0] >= 0x80000004 )
			{
				for ( unsigned i = 0; i < 3; ++i ) { __cpuid(cpu, 0x80000002 + i); memcpy(brand + i * 16, cpu, 16); }
			}
			SYSTEM_INFO si;
			GetSystemInfo(&si);
			MEMORYSTATUSEX ms;
			ms.dwLength = sizeof(ms);
			GlobalMemoryStatusEx(&ms);
			typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
			OSVERSIONINFOW ov = {};
			ov.dwOSVersionInfoSize = sizeof(ov);
			if ( auto f = (RtlGetVersionFn)GetProcAddress(GetModuleHandleA("ntdll"), "RtlGetVersion") ) { f(&ov); }
			char b[512];
			snprintf(b, sizeof(b), "\"cpu\":\"%s\",\"cores\":%u,\"ram_mb\":%llu,\"ram_free_mb\":%llu,\"commit_free_mb\":%llu,\"os\":\"%lu.%lu.%lu\"",
				jesc(brand).c_str(), (unsigned)si.dwNumberOfProcessors, (unsigned long long)(ms.ullTotalPhys >> 20),
				(unsigned long long)(ms.ullAvailPhys >> 20), (unsigned long long)(ms.ullAvailPageFile >> 20), ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber);
			hw = b;
#endif
			emit("hw", hw, true);

			std::string kn = "\"argv\":\"";
			for ( int i = 1; i < argc; ++i ) { kn += jesc(argv[i]) + " "; }
			kn += "\",\"env\":{";
			bool first = true;
#ifdef WINDOWS
			for ( char** e = _environ; e && *e; ++e )
			{
				if ( !strncmp(*e, "APX", 3) || !strncmp(*e, "BARONY", 6) )
				{
					std::string kvp = *e;
					size_t eq = kvp.find('=');
					if ( eq == std::string::npos ) { continue; }
					if ( !first ) { kn += ","; }
					first = false;
					kn += "\"" + jesc(kvp.substr(0, eq).c_str()) + "\":\"" + jesc(kvp.substr(eq + 1).c_str()) + "\"";
				}
			}
#endif
			kn += "}";
			emit("knobs", kn, true);
		}

		void glAndSettingsRows()
		{
			gGlDone = true;
			std::string g;
			const char* v = (const char*)glGetString(GL_VENDOR);
			const char* r = (const char*)glGetString(GL_RENDERER);
			const char* ver = (const char*)glGetString(GL_VERSION);
			g = "\"gl_vendor\":\"" + jesc(v) + "\",\"gl_renderer\":\"" + jesc(r) + "\",\"gl_version\":\"" + jesc(ver) + "\"";
			const char* drv = SDL_GetCurrentVideoDriver();
			g += ",\"video_driver\":\"" + jesc(drv) + "\",\"displays\":" + std::to_string(SDL_GetNumVideoDisplays());
			SDL_DisplayMode dm;
			if ( SDL_GetCurrentDisplayMode(0, &dm) == 0 )
			{
				g += ",\"display0\":\"" + std::to_string(dm.w) + "x" + std::to_string(dm.h) + "@" + std::to_string(dm.refresh_rate) + "\"";
			}
			emit("gl", g, true);
			char s[512];
			snprintf(s, sizeof(s), "\"xres\":%d,\"yres\":%d,\"fullscreen\":%d,\"borderless\":%d,\"vsync\":%d,\"fps_limit\":%u,\"dlc1\":%d,\"dlc2\":%d,\"multiplayer\":%d,\"splitscreen\":%d,\"numplayers\":%d",
				(int)xres, (int)yres, (int)fullscreen, (int)borderless, (int)verticalSync, (unsigned)fpsLimit, (int)enabledDLCPack1, (int)enabledDLCPack2,
				(int)multiplayer, (int)splitscreen, (int)numplayers);
			emit("settings", s, true);
		}

		void secondRow(int64_t now)
		{
			++gSecIndex;
			bool skip = gBytes > kSoftCap && (gSecIndex % 10) != 0;
			std::vector<float> iv = gIv, wk = gWork;
			char b[300];
			double ivp50 = pct(iv, 0.5), ivp99 = pct(iv, 0.99), ivmax = iv.empty() ? 0 : iv.back();
			double wp50 = pct(wk, 0.5), wp99 = pct(wk, 0.99), wmax = wk.empty() ? 0 : wk.back();
			gMedianWork = (float)std::max(1.0, wp50);
			double overUs = (double)gOverheadUs + gScopes * 2 * gScopeCostUs;
			double wallUs = (double)(now - gSecStartUs);
			snprintf(b, sizeof(b), "\"frames\":%u,\"iv_p50\":%.2f,\"iv_p99\":%.2f,\"iv_max\":%.2f,\"work_p50\":%.2f,\"work_p99\":%.2f,\"work_max\":%.2f,\"overhead_us\":%.0f,\"overhead_pct\":%.4f,\"err_total\":%llu,\"warn_total\":%llu",
				gFrames, ivp50, ivp99, ivmax, wp50, wp99, wmax, overUs, wallUs > 0 ? 100.0 * overUs / wallUs : 0.0,
				(unsigned long long)gErrTotal, (unsigned long long)gWarnTotal);
			std::string kv = b;
			kv += ",\"sys_ms\":{";
			bool first = true;
			for ( int i = 0; i < S_N; ++i )
			{
				if ( gSecSys[i] <= 0 ) { continue; }
				char c[64];
				snprintf(c, sizeof(c), "%s\"%s\":%.2f", first ? "" : ",", kSysName[i], gSecSys[i] / 1000.0);
				kv += c;
				first = false;
			}
			kv += "}";
			if ( !skip ) { emit("sec", kv, false); }
			if ( gFile ) { std::lock_guard<std::mutex> lk(gMu); fflush(gFile); }
			gIv.clear(); gWork.clear();
			memset(gSecSys, 0, sizeof(gSecSys));
			gOverheadUs = 0; gScopes = 0; gFrames = 0; gSpikesThisSec = 0;
			gSecStartUs = now;
			if ( now - gLastErrSumUs > 60 * 1000000LL )
			{
				gLastErrSumUs = now;
				std::vector<std::pair<uint64_t, std::string>> top;
				{
					std::lock_guard<std::mutex> lk(gMu);
					for ( auto& e : gErr ) { if ( e.second.count > 0 ) { top.push_back({ e.second.count, e.first }); } }
				}
				if ( !top.empty() )
				{
					std::sort(top.rbegin(), top.rend());
					std::string k = "\"top\":[";
					for ( size_t i = 0; i < top.size() && i < 10; ++i )
					{
						k += std::string(i ? "," : "") + "{\"n\":" + std::to_string(top[i].first) + ",\"msg\":\"" + jesc(top[i].second.c_str()) + "\"}";
					}
					k += "]";
					emit("errsum", k, false);
				}
			}
		}
	}

	std::string jsonEscape(const char* s) { return jesc(s); }
	void localRow(const char* type, const std::string& kv)
	{
		// written to the session file only: never put into the upload buffers (the upload log must not upload itself)
		if ( !on ) { return; }
		std::string r = head(type);
		if ( !kv.empty() ) { r += ","; r += kv; }
		r += "}";
		std::lock_guard<std::mutex> lk(gMu);
		writeRowLocked(r, true);
	}
	void (*consentPromptHook)() = nullptr;

	int64_t nowUs()
	{
#ifdef WINDOWS
		LARGE_INTEGER c;
		QueryPerformanceCounter(&c);
		return (int64_t)(c.QuadPart * gUsPerTick);
#else
		return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
	}

	void addUs(int sys, int64_t us)
	{
#ifdef WINDOWS
		if ( GetCurrentThreadId() != gMainTid ) { return; }
#endif
		++gScopes;
		gFrameSys[sys] += us;
	}

	void event(const char* type, const char* kv)
	{
		if ( !on ) { return; }
		int64_t t0 = nowUs();
		emit("ev", std::string("\"e\":\"") + jesc(type) + "\"" + (kv && *kv ? std::string(",") + kv : std::string()), false);
		gOverheadUs += nowUs() - t0;
	}

	void eventf(const char* type, const char* fmt, ...)
	{
		if ( !on ) { return; }
		char b[600];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(b, sizeof(b), fmt, ap);
		va_end(ap);
		event(type, b);
	}

	void logLine(const char* line)
	{
		if ( !on || !line ) { return; }
		int64_t t0 = nowUs();
		if ( !strncmp(line, "[apx] features", 14) )
		{
			emit("knobs_features", "\"line\":\"" + jesc(line) + "\"", true);
		}
		int lv = classify(line);
		if ( lv )
		{
			std::string key = normaliseKey(line);
			bool writeIt = false;
			{
				std::lock_guard<std::mutex> lk(gMu);
				if ( gErr.size() < 2000 || gErr.count(key) )
				{
					ErrKey& e = gErr[key];
					++e.count;
					e.level = lv;
					if ( e.sample.empty() ) { e.sample = line; }
					writeIt = e.count <= 3;
				}
				if ( lv == 2 ) { ++gErrTotal; } else { ++gWarnTotal; }
			}
			if ( writeIt )
			{
				std::string kv = std::string("\"level\":\"") + (lv == 2 ? "error" : "warning") + "\",\"msg\":\"" + jesc(line) + "\",\"tick\":" + std::to_string(ticks);
				emit("err", kv, true);
			}
		}
		else
		{
			// not an error: keep it in the crash ring so the last log lines are in a crash report
			std::lock_guard<std::mutex> lk(gMu);
			ringPush(std::string("log: ") + line);
		}
		gOverheadUs += nowUs() - t0;
	}

	void frame()
	{
		if ( !on ) { return; }
		int64_t t0 = nowUs();
		sampleTick = false;
		if ( !gGlDone ) { glAndSettingsRows(); }
		if ( gLastFrameUs )
		{
			float iv = (t0 - gLastFrameUs) / 1000.f;
			float limiter = gFrameSys[S_LIMITER] / 1000.f;
			float work = std::max(0.f, iv - limiter);
			gIv.push_back(iv);
			gWork.push_back(work);
			++gFrames;
			for ( int i = 0; i < S_N; ++i ) { gSecSys[i] += gFrameSys[i]; }
			float thresh = std::max(33.f, 4.f * gMedianWork);
			if ( work > thresh && gSpikesThisSec < 20 )
			{
				++gSpikesThisSec;
				std::string kv = "\"work_ms\":" + std::to_string(work) + ",\"iv_ms\":" + std::to_string(iv) + ",\"threshold_ms\":" + std::to_string(thresh) + ",\"sys_ms\":{";
				bool first = true;
				for ( int i = 0; i < S_N; ++i )
				{
					if ( gFrameSys[i] < 500 ) { continue; }
					char c[64];
					snprintf(c, sizeof(c), "%s\"%s\":%.2f", first ? "" : ",", kSysName[i], gFrameSys[i] / 1000.0);
					kv += c;
					first = false;
				}
				kv += "},\"loading\":" + std::to_string((int)loading) + ",\"paused\":" + std::to_string((int)gamePaused);
				emit("spike", kv, true);
			}
			memset(gFrameSys, 0, sizeof(gFrameSys));
		}
		gLastFrameUs = t0;
		if ( !gSecStartUs ) { gSecStartUs = t0; }
		if ( t0 - gSecStartUs >= 1000000 )
		{
			secondRow(t0);
			upload::tick();
			static int promptCheck = 0;
			if ( consentPromptHook && upload::state() == 0 && (++promptCheck % 3) == 0 ) { consentPromptHook(); }
		}
		plantedChecks(ticks);
		gOverheadUs += nowUs() - t0;
	}

	// ------------------------------------------------------------------ crash handling
#ifdef WINDOWS
	namespace
	{
		std::string symbolise(DWORD64 addr)
		{
			char buf[sizeof(SYMBOL_INFO) + 256] = {};
			SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
			sym->SizeOfStruct = sizeof(SYMBOL_INFO);
			sym->MaxNameLen = 255;
			DWORD64 disp = 0;
			std::string out;
			char hex[32];
			snprintf(hex, sizeof(hex), "0x%llx", (unsigned long long)addr);
			out = hex;
			HMODULE mod = nullptr;
			if ( GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod) && mod )
			{
				char name[MAX_PATH] = {};
				GetModuleFileNameA(mod, name, MAX_PATH);
				const char* base = strrchr(name, '\\');
				snprintf(hex, sizeof(hex), "+0x%llx", (unsigned long long)(addr - (DWORD64)mod));
				out += std::string(" ") + (base ? base + 1 : name) + hex;
			}
			if ( SymFromAddr(GetCurrentProcess(), addr, &disp, sym) )
			{
				out += std::string(" ") + sym->Name;
				IMAGEHLP_LINE64 ln = {};
				ln.SizeOfStruct = sizeof(ln);
				DWORD ld = 0;
				if ( SymGetLineFromAddr64(GetCurrentProcess(), addr, &ld, &ln) )
				{
					const char* f = strrchr(ln.FileName, '\\');
					out += std::string(" (") + (f ? f + 1 : ln.FileName) + ":" + std::to_string(ln.LineNumber) + ")";
				}
			}
			return out;
		}
	}
#endif

	long crash(_EXCEPTION_POINTERS* e, const char* why)
	{
#ifdef WINDOWS
		if ( !on || gCrashing ) { return 0; }
		gCrashing = true;
		std::string path = gDir + "/crash-" + gSessionStamp + "-" + std::to_string(GetCurrentProcessId()) + ".txt";
		FILE* f = fopen(path.c_str(), "wb");
		std::string stackRow;
		std::string text = std::string("barony-apx crash report (a local file; it is only uploaded if the player opted in)\nwhy: ") + why + "\n";
		text += std::string("sha: ") + upload::shaStr() + "\nversion: " + upload::versionStr() + "\nsession_id: " + upload::sessionIdStr() + "\n";
		char b[256];
		if ( e && e->ExceptionRecord )
		{
			snprintf(b, sizeof(b), "exception code: 0x%08lx at 0x%llx\n", e->ExceptionRecord->ExceptionCode, (unsigned long long)e->ExceptionRecord->ExceptionAddress);
			text += b;
		}
		snprintf(b, sizeof(b), "tick %u level %d loading %d paused %d\n", (unsigned)ticks, (int)currentlevel, (int)loading, (int)gamePaused);
		text += b;
		// symbols: the .pdb ships beside the exe
		char exe[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, exe, MAX_PATH);
		std::string exeDir = exe;
		size_t sl = exeDir.find_last_of('\\');
		if ( sl != std::string::npos ) { exeDir.resize(sl); }
		SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
		SymInitialize(GetCurrentProcess(), exeDir.c_str(), TRUE);
		text += "stack:\n";
		if ( e && e->ContextRecord )
		{
			CONTEXT ctx = *e->ContextRecord;
			STACKFRAME64 sf = {};
			sf.AddrPC.Offset = ctx.Rip; sf.AddrPC.Mode = AddrModeFlat;
			sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
			sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
			stackRow = "\"stack\":[";
			for ( int i = 0; i < 40; ++i )
			{
				if ( !StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &sf, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ) { break; }
				if ( sf.AddrPC.Offset == 0 ) { break; }
				std::string s = symbolise(sf.AddrPC.Offset);
				text += "  #" + std::to_string(i) + " " + s + "\n";
				stackRow += std::string(i ? "," : "") + "\"" + jesc(s.c_str()) + "\"";
			}
			stackRow += "]";
		}
		text += "last events:\n";
		std::string evs;
		{
			// the lock may be held by the crashing thread: do not take it, a torn string is better than a deadlock
			int n = std::min(gRingN, kRing);
			for ( int i = n; i > 0; --i )
			{
				const std::string& s = gRing[(gRingN - i) % kRing];
				text += "  " + s + "\n";
			}
		}
		if ( f ) { fwrite(text.data(), 1, text.size(), f); fclose(f); }
		if ( gFile )
		{
			std::string r = head("crash") + ",\"why\":\"" + jesc(why) + "\",\"file\":\"" + jesc(path.c_str()) + "\"" + (stackRow.empty() ? "" : "," + stackRow) + "}";
			fwrite(r.data(), 1, r.size(), gFile);
			fputc('\n', gFile);
			fflush(gFile);
		}
		rotate("crash-", ".txt", 10);
		fprintf(stderr, "[apx] crash report written: %s\n", path.c_str());
		gCrashing = false;
#endif
		return 0;
	}

	namespace
	{
#ifdef WINDOWS
		void fatalNonSeh(const char* why)
		{
			CONTEXT ctx = {};
			RtlCaptureContext(&ctx);
			EXCEPTION_RECORD rec = {};
			rec.ExceptionCode = 0xE0000001;
			rec.ExceptionAddress = (PVOID)ctx.Rip;
			EXCEPTION_POINTERS ep = { &rec, &ctx };
			crash(&ep, why);
		}
		void onTerminate() { fatalNonSeh("std::terminate"); abort(); }
		void onAbort(int) { fatalNonSeh("SIGABRT"); _exit(3); }
		void onInvalidParam(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) { fatalNonSeh("invalid CRT parameter"); _exit(3); }
		void onPurecall() { fatalNonSeh("pure virtual call"); _exit(3); }
#endif
	}

	__declspec(noinline) void apxDiagPlantedCrash()
	{
		volatile int* p = nullptr;
		*p = 1234; // deliberate: the gate needs a dump and a symbolised stack naming this function
	}

	void plantedCrash() { apxDiagPlantedCrash(); }

	void plantedChecks(uint32_t t)
	{
		if ( gPlantStallTick >= 0 && (int)t >= gPlantStallTick )
		{
			gPlantStallTick = -1;
			APX_SCOPE(S_TACTICS);
			SDL_Delay(500); // planted stall inside a named scope (well above any load-time threshold): the spike row must name tactics
		}
		if ( gPlantErrTick >= 0 && (int)t >= gPlantErrTick )
		{
			gPlantErrTick = -1;
			printlog("Error: planted diagnostic error %d (APX_DIAG_PLANT=error)", 42);
			printlog("warning: planted diagnostic warning");
		}
		if ( gPlantExitTick >= 0 && (int)t >= gPlantExitTick )
		{
			gPlantExitTick = -1;
			mainloop = 0; // a clean quit through the normal shutdown path (the exit-batch test)
		}
		if ( gPlantCrashTick >= 0 && (int)t >= gPlantCrashTick )
		{
			gPlantCrashTick = -1;
			plantedCrash();
		}
	}

	void init(int argc, char** argv)
	{
		const char* d = getenv("APX_DIAG");
		for ( int i = 1; i < argc; ++i )
		{
			if ( !strcmp(argv[i], "-apxdiagselftest") ) { int f = selfTest(); fprintf(stderr, "[apx] diag selftest: %s (%d failures)\n", f ? "FAIL" : "PASS", f); exit(f ? 1 : 0); }
		}
		if ( d && d[0] == '0' ) { on = false; return; }
#ifdef WINDOWS
		LARGE_INTEGER fq;
		QueryPerformanceFrequency(&fq);
		gUsPerTick = 1e6 / (double)fq.QuadPart;
		gMainTid = GetCurrentThreadId();
#endif
		gStartUs = nowUs();
		// calibrate the cost of one timestamp so the scoped timers' own overhead is reported honestly
		{
			int64_t a = nowUs();
			for ( int i = 0; i < 20000; ++i ) { volatile int64_t x = nowUs(); (void)x; }
			gScopeCostUs = std::max(0.01, (nowUs() - a) / 20000.0);
		}
		mkdirOne(gDir);
		gSessionStamp = hexTime();
		rotate("session-", ".jsonl", 9);
#ifdef WINDOWS
		gPath = gDir + "/session-" + gSessionStamp + "-" + std::to_string(GetCurrentProcessId()) + ".jsonl";
		gFile = fopen(gPath.c_str(), "wb");
		SetErrorMode(SEM_FAILCRITICALERRORS);
		std::set_terminate(onTerminate);
		signal(SIGABRT, onAbort);
		_set_invalid_parameter_handler(onInvalidParam);
		_set_purecall_handler(onPurecall);
		_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
		const char* pl = getenv("APX_DIAG_PLANT");
		if ( pl )
		{
			// "stall:<tick>", "error:<tick>", "crash:<tick>"
			const char* c = strchr(pl, ':');
			int tk = c ? atoi(c + 1) : 200;
			if ( !strncmp(pl, "stall", 5) ) { gPlantStallTick = tk; }
			else if ( !strncmp(pl, "error", 5) ) { gPlantErrTick = tk; }
			else if ( !strncmp(pl, "crash", 5) ) { gPlantCrashTick = tk; }
			else if ( !strncmp(pl, "exit", 4) ) { gPlantExitTick = tk; }
		}
		upload::init(); // before the session rows, so they are kept for the (opt-in) session upload
		writeSessionRows(argc, argv);
	}

	void shutdown()
	{
		if ( !on ) { return; }
		emit("exit", "\"clean\":1", true);
		upload::shutdownFlush();
		std::lock_guard<std::mutex> lk(gMu);
		if ( gFile ) { fclose(gFile); gFile = nullptr; }
	}

	// ------------------------------------------------------------------ pure-logic self test (no game needed)
	int selfTest()
	{
		int fail = 0;
		auto check = [&](bool ok, const char* what) { if ( !ok ) { ++fail; fprintf(stderr, "[apx] diag selftest FAIL: %s\n", what); } };
		std::vector<float> v;
		for ( int i = 1; i <= 100; ++i ) { v.push_back((float)i); }
		check(fabs(pct(v, 0.5) - 50.5) < 1e-3, "p50 of 1..100");
		check(fabs(pct(v, 0.99) - 99.01) < 1e-2, "p99 of 1..100");
		std::vector<float> e;
		check(pct(e, 0.5) == 0, "empty percentile");
		check(jesc("a\"b\\c\n\x01") == "a\\\"b\\\\c ?", "json escape");
		check(classify("PhysFS: successfully initialized, last error: no error") == 0, "no error is not an error");
		check(classify("[JSON]: Error: Could not locate json file x.json") == 2, "error line");
		check(classify("warning: something odd") == 1, "warning line");
		check(classify("loading image 'a.png'...") == 0, "plain line");
		check(classify("[apx] features apxgen=1 impact=1") == 0, "apx features line is not an error");
		check(normaliseKey("Could not open 'models/12/limbs.txt' at 400") == "Could not open '#models/#/limbs.txt' at #" || normaliseKey("Could not open 'models/12/limbs.txt' at 400") == "Could not open 'models/#/limbs.txt' at #", "key normalisation");
		return fail;
	}
}
