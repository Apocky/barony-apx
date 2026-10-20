// apx/diag_upload.cpp -- D91: OPT-IN upload of the diagnostics to the project's Supabase tables.
//
// PRIVACY RULES (enforced here, tested in tools/apx/diag_gate.py):
//  * Nothing is sent before the player says Yes. Consent lives in diag/consent.json {"consent":"yes|no"}; unset = no.
//  * What is sent: performance rows, errors, crashes, an anonymous random install id (never derived from the machine,
//    the user or the account). No names, files or account data: user-profile path segments are replaced with <user>,
//    the working directory and process id are not sent, crash reports carry no raw log lines (events only).
//  * Tests: APX_DIAG_UPLOAD=yes|no overrides consent for that process without writing it; APX_DIAG_INSTALL_ID gives a test
//    id; APX_DIAG_UPLOAD_DRYRUN=1 writes the request bodies to diag/upload-dryrun/ instead of sending them.
// Transport: WinHTTP (built into Windows), one background thread, a small spool in diag/upload-queue (40 files, 2 MB),
// retries with backoff, never on the frame thread.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <direct.h>
#include <io.h>
#endif
#include "diag.hpp"
#include "../main.hpp"
#include "../game.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#pragma comment(lib, "winhttp.lib")

namespace apxdiag
{
	std::string jsonEscape(const char* s); // diag.cpp
	namespace upload
	{
		static const wchar_t* kHost = L"nqfpeprkiclyazoefjvz.supabase.co";
		static const char* kKey = "sb_publishable_axc4Ricfo9K0VNcfFOJ4Wg_5mji79IZ"; // public by design: insert-only tables
		static const size_t kSessionCap = 16 * 1024, kBatchCap = 64 * 1024, kStackCap = 32 * 1024, kSpoolFiles = 40, kSpoolBytes = 2u << 20;

		struct Item { std::string table, body, spool; int tries = 0; int64_t notBeforeMs = 0; std::string markSent; };
		static std::mutex mu;
		static std::condition_variable cv;
		static std::deque<Item> q;
		static std::thread worker;
		static std::atomic<bool> stopping{ false }, running{ false };
		static std::vector<std::string> infoRows, batchRows;
		static size_t batchBytes = 0;
		static std::string installId, sessionId, version, sha, dir = "diag";
		static int consent = 0; // 0 unset, 1 yes, 2 no
		static bool envOverride = false, dryrun = false, sessionQueued = false;
		static int64_t lastBatchMs = 0, startMs = 0;
		static uint64_t sentOk = 0, sentFail = 0, dropped = 0, seq = 0;

		static int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

		static std::string hex(size_t n)
		{
			static std::random_device rd;
			std::string s;
			const char* h = "0123456789abcdef";
			for ( size_t i = 0; i < n; ++i ) { s += h[rd() & 15]; }
			return s;
		}
		static std::string uuid()
		{
			static std::random_device rd2;
			std::string a = hex(32);
			a[12] = '4';
			a[16] = "89ab"[rd2() & 3];
			return a.substr(0, 8) + "-" + a.substr(8, 4) + "-" + a.substr(12, 4) + "-" + a.substr(16, 4) + "-" + a.substr(20, 12);
		}

		// replace the user-profile segment of any path so no account name leaves the machine
		std::string scrub(const std::string& in)
		{
			// "Users" + one or more separators (a JSON-escaped backslash is two characters) + a name -> the name becomes <user>
			std::string out;
			for ( size_t i = 0; i < in.size(); )
			{
				if ( (in.compare(i, 5, "Users") == 0 || in.compare(i, 5, "users") == 0) && i + 5 < in.size() && (in[i + 5] == '\\' || in[i + 5] == '/') )
				{
					size_t j = i + 5;
					while ( j < in.size() && (in[j] == '\\' || in[j] == '/') ) { ++j; }
					out += in.substr(i, j - i) + "<user>";
					i = j;
					while ( i < in.size() && in[i] != '\\' && in[i] != '/' && in[i] != '"' && in[i] != ' ' ) { ++i; }
					continue;
				}
				out += in[i++];
			}
			return out;
		}

		static std::string readFile(const std::string& p)
		{
			FILE* f = fopen(p.c_str(), "rb");
			if ( !f ) { return ""; }
			std::string s;
			char b[4096];
			size_t n;
			while ( (n = fread(b, 1, sizeof(b), f)) > 0 ) { s.append(b, n); }
			fclose(f);
			return s;
		}
		static void writeFile(const std::string& p, const std::string& s)
		{
			if ( FILE* f = fopen(p.c_str(), "wb") ) { fwrite(s.data(), 1, s.size(), f); fclose(f); }
		}

		static void saveConsent()
		{
			if ( envOverride ) { return; }
			writeFile(dir + "/consent.json", std::string("{\"v\":1,\"consent\":\"") + (consent == 1 ? "yes" : (consent == 2 ? "no" : "unset")) + "\",\"install_id\":\"" + installId + "\"}\n");
		}

		// ---------------------------------------------------------------- transport (worker thread)
		static int post(const std::string& table, const std::string& body)
		{
#ifdef WINDOWS
			if ( dryrun )
			{
				_mkdir((dir + "/upload-dryrun").c_str());
				writeFile(dir + "/upload-dryrun/" + std::to_string(++seq) + "-" + table + ".json", body);
				return 201;
			}
			int status = 0;
			HINTERNET s = WinHttpOpen(L"barony-apx-diag/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
			if ( !s ) { return -1; }
			WinHttpSetTimeouts(s, 8000, 8000, 10000, 15000);
			HINTERNET c = WinHttpConnect(s, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
			std::wstring path = L"/rest/v1/" + std::wstring(table.begin(), table.end());
			HINTERNET r = c ? WinHttpOpenRequest(c, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
			if ( r )
			{
				std::string h = std::string("apikey: ") + kKey + "\r\nAuthorization: Bearer " + kKey + "\r\nContent-Type: application/json\r\nPrefer: return=minimal\r\n";
				std::wstring wh(h.begin(), h.end());
				if ( WinHttpSendRequest(r, wh.c_str(), (DWORD)-1L, (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) && WinHttpReceiveResponse(r, nullptr) )
				{
					DWORD code = 0, sz = sizeof(code);
					if ( WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX) ) { status = (int)code; }
				}
				else { status = -2; }
				WinHttpCloseHandle(r);
			}
			else { status = -3; }
			if ( c ) { WinHttpCloseHandle(c); }
			WinHttpCloseHandle(s);
			return status;
#else
			return -1;
#endif
		}

		static void spoolWrite(Item& it)
		{
#ifdef WINDOWS
			_mkdir((dir + "/upload-queue").c_str());
			// cap: file count and bytes; drop the oldest
			std::vector<std::pair<std::string, long>> files;
			_finddata_t fd;
			intptr_t h = _findfirst((dir + "/upload-queue/*.json").c_str(), &fd);
			size_t total = 0;
			if ( h != -1 )
			{
				do { files.push_back({ fd.name, (long)fd.size }); total += fd.size; } while ( _findnext(h, &fd) == 0 );
				_findclose(h);
			}
			std::sort(files.begin(), files.end());
			while ( !files.empty() && (files.size() >= kSpoolFiles || total + it.body.size() > kSpoolBytes) )
			{
				remove((dir + "/upload-queue/" + files.front().first).c_str());
				total -= files.front().second;
				files.erase(files.begin());
				++dropped;
			}
			char nm[64];
			snprintf(nm, sizeof(nm), "%013lld-%llu-", (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(), (unsigned long long)++seq);
			it.spool = dir + "/upload-queue/" + nm + it.table + ".json";
			writeFile(it.spool, it.body);
#endif
		}

		static void workerMain()
		{
			running = true;
			while ( !stopping )
			{
				Item it;
				{
					std::unique_lock<std::mutex> lk(mu);
					cv.wait_for(lk, std::chrono::seconds(2), [] { return stopping.load() || !q.empty(); });
					if ( stopping && q.empty() ) { break; }
					if ( q.empty() ) { continue; }
					if ( q.front().notBeforeMs > nowMs() ) { cv.wait_for(lk, std::chrono::milliseconds(500)); continue; }
					it = q.front();
					q.pop_front();
				}
				int st = post(it.table, it.body);
				localRow("upload", "\"table\":\"" + it.table + "\",\"status\":" + std::to_string(st) + ",\"bytes\":" + std::to_string(it.body.size()) + ",\"tries\":" + std::to_string(it.tries) + (dryrun ? ",\"dryrun\":1" : ""));
				if ( st == 200 || st == 201 || st == 204 )
				{
					++sentOk;
					if ( !it.spool.empty() ) { remove(it.spool.c_str()); }
					if ( !it.markSent.empty() ) { writeFile(it.markSent, "sent\n"); }
				}
				else if ( st >= 400 && st < 500 && st != 408 && st != 429 )
				{
					// the server refused this body for good (schema, size): do not retry forever
					++sentFail;
					++dropped;
					if ( !it.spool.empty() ) { remove(it.spool.c_str()); }
					fprintf(stderr, "[apx] diag upload refused (%d) table %s\n", st, it.table.c_str());
				}
				else
				{
					++sentFail;
					if ( it.spool.empty() ) { spoolWrite(it); }
					if ( ++it.tries <= 6 && !stopping )
					{
						it.notBeforeMs = nowMs() + std::min<int64_t>(15 * 60 * 1000, 30000LL << (it.tries - 1));
						std::lock_guard<std::mutex> lk(mu);
						q.push_back(it);
					} // else it stays in the spool and is retried at the next launch
				}
			}
			running = false;
		}

		static void enqueue(const std::string& table, std::string body, const std::string& markSent = "")
		{
			if ( !running && !stopping ) { /* worker started in start() */ }
			Item it;
			it.table = table;
			it.body = std::move(body);
			it.markSent = markSent;
			{
				std::lock_guard<std::mutex> lk(mu);
				if ( q.size() > 200 ) { ++dropped; return; }
				q.push_back(std::move(it));
			}
			cv.notify_one();
		}

		// ---------------------------------------------------------------- bodies
		static std::string header(bool withSha)
		{
			std::string h = "\"install_id\":\"" + installId + "\",\"session_id\":\"" + sessionId + "\",\"version\":\"" + jsonEscape(version.c_str()) + "\"";
			if ( withSha ) { h += ",\"build_sha\":\"" + jsonEscape(sha.c_str()) + "\""; }
			return h;
		}

		// drop everything that could name the player's machine or account from one info row
		static std::string cleanInfoRow(const std::string& row)
		{
			std::string r = scrub(row);
			// the working directory and process id are not sent
			auto cut = [&](const char* key) {
				size_t p = r.find(key);
				if ( p == std::string::npos ) { return; }
				size_t e = r.find(',', p);
				if ( e == std::string::npos ) { return; }
				r.erase(p, e - p + 1);
			};
			cut("\"cwd\":");
			cut("\"pid\":");
			return r;
		}

		static void queueSession()
		{
			if ( sessionQueued || consent != 1 ) { return; }
			std::string info = "[";
			bool first = true;
			for ( auto& r : infoRows ) { info += (first ? "" : ",") + cleanInfoRow(r); first = false; }
			info += "]";
			if ( info.size() > kSessionCap ) { info.resize(kSessionCap - 2); info += "\"]"; } // never over the table's cap
			sessionQueued = true;
			enqueue("labyrinth_sessions", "{" + header(true) + ",\"info\":" + info + "}");
		}

		static void queueBatch(bool force)
		{
			if ( consent != 1 || batchRows.empty() ) { return; }
			if ( !force && nowMs() - lastBatchMs < 300000 ) { return; }
			lastBatchMs = nowMs();
			std::string b = "[";
			for ( size_t i = 0; i < batchRows.size(); ++i ) { b += (i ? "," : "") + scrub(batchRows[i]); }
			b += "]";
			batchRows.clear();
			batchBytes = 0;
			enqueue("labyrinth_events", "{" + header(false) + ",\"batch\":" + b + "}");
		}

		static void queueOldCrashes()
		{
#ifdef WINDOWS
			_finddata_t fd;
			intptr_t h = _findfirst((dir + "/crash-*.txt").c_str(), &fd);
			if ( h == -1 ) { return; }
			std::vector<std::string> names;
			do { names.push_back(fd.name); } while ( _findnext(h, &fd) == 0 );
			_findclose(h);
			for ( auto& n : names )
			{
				std::string path = dir + "/" + n, mark = path + ".sent";
				if ( _access(mark.c_str(), 0) == 0 ) { continue; }
				std::string t = readFile(path);
				if ( t.empty() ) { continue; }
				auto field = [&](const char* k) { size_t p = t.find(k); if ( p == std::string::npos ) { return std::string(); } p += strlen(k); size_t e = t.find('\n', p); return t.substr(p, e - p); };
				std::string csha = field("sha: "), cver = field("version: "), csid = field("session_id: ");
				size_t sp = t.find("stack:\n"), ep = t.find("last events:\n");
				std::string stack = scrub(t.substr(0, ep == std::string::npos ? t.size() : ep)); // header + reason + stack
				if ( stack.size() > kStackCap ) { stack.resize(kStackCap); }
				std::string evs = "[";
				if ( ep != std::string::npos )
				{
					size_t i = ep + 13;
					bool first = true;
					size_t bytes = 0;
					while ( i < t.size() )
					{
						size_t e = t.find('\n', i);
						if ( e == std::string::npos ) { e = t.size(); }
						std::string line = t.substr(i, e - i);
						i = e + 1;
						while ( !line.empty() && line[0] == ' ' ) { line.erase(0, 1); }
						if ( line.empty() || line.compare(0, 4, "log:") == 0 ) { continue; } // raw log lines never leave the machine
						std::string el = "\"" + jsonEscape(scrub(line).c_str()) + "\"";
						if ( bytes + el.size() > kBatchCap - 2048 ) { break; }
						evs += (first ? "" : ",") + el;
						bytes += el.size() + 1;
						first = false;
					}
				}
				evs += "]";
				(void)sp;
				std::string hdr = "\"install_id\":\"" + installId + "\",\"session_id\":\"" + (csid.empty() ? sessionId : csid) + "\",\"version\":\"" + jsonEscape((cver.empty() ? version : cver).c_str())
					+ "\",\"build_sha\":\"" + jsonEscape((csha.empty() ? sha : csha).c_str()) + "\"";
				enqueue("labyrinth_crashes", "{" + hdr + ",\"stack\":\"" + jsonEscape(stack.c_str()) + "\",\"last_events\":" + evs + "}", mark);
			}
#endif
		}

		static void startWorker()
		{
			if ( running || worker.joinable() ) { return; }
			stopping = false;
			worker = std::thread(workerMain);
		}

		static void loadSpool()
		{
#ifdef WINDOWS
			_finddata_t fd;
			intptr_t h = _findfirst((dir + "/upload-queue/*.json").c_str(), &fd);
			if ( h == -1 ) { return; }
			std::vector<std::string> names;
			do { names.push_back(fd.name); } while ( _findnext(h, &fd) == 0 );
			_findclose(h);
			std::sort(names.begin(), names.end());
			for ( auto& n : names )
			{
				size_t d = n.rfind('-');
				if ( d == std::string::npos ) { continue; }
				Item it;
				it.table = n.substr(d + 1, n.size() - d - 1 - 5);
				it.spool = dir + "/upload-queue/" + n;
				it.body = readFile(it.spool);
				if ( it.body.empty() ) { remove(it.spool.c_str()); continue; }
				std::lock_guard<std::mutex> lk(mu);
				q.push_back(std::move(it));
			}
#endif
		}

		// ---------------------------------------------------------------- public (see diag.hpp)
		void init()
		{
			std::string cj = readFile(dir + "/consent.json");
			auto val = [&](const char* k) { size_t p = cj.find(k); if ( p == std::string::npos ) { return std::string(); } p += strlen(k); size_t e = cj.find('"', p); return cj.substr(p, e - p); };
			std::string c = val("\"consent\":\"");
			installId = val("\"install_id\":\"");
			consent = c == "yes" ? 1 : (c == "no" ? 2 : 0);
			if ( installId.empty() ) { installId = uuid(); saveConsent(); }
			if ( const char* e = getenv("APX_DIAG_UPLOAD") )
			{
				envOverride = true;
				consent = (e[0] == 'y' || e[0] == 'Y' || e[0] == '1') ? 1 : 2;
			}
			if ( const char* id = getenv("APX_DIAG_INSTALL_ID") ) { installId = id; }
			dryrun = getenv("APX_DIAG_UPLOAD_DRYRUN") != nullptr;
			sessionId = uuid();
			if ( const char* v = getenv("APX_DIAG_VERSION") ) { version = v; }
			else { version = scrub(readFile("apx_version.txt")); while ( !version.empty() && (version.back() == '\n' || version.back() == '\r') ) { version.pop_back(); } if ( version.empty() ) { version = "dev"; } }
#ifdef APX_GIT_SHA
			sha = APX_GIT_SHA;
#else
			sha = "unknown";
#endif
			startMs = lastBatchMs = nowMs();
			if ( consent == 1 )
			{
				loadSpool();
				startWorker();
				queueOldCrashes();
			}
		}

		const char* sessionIdStr() { return sessionId.c_str(); }
		const char* versionStr() { return version.c_str(); }
		const char* shaStr() { return sha.c_str(); }
		int state() { return consent; } // 0 unset, 1 yes, 2 no
		const char* installIdStr() { return installId.c_str(); }

		void setConsent(bool yes)
		{
			consent = yes ? 1 : 2;
			saveConsent();
			if ( yes )
			{
				loadSpool();
				startWorker();
				queueOldCrashes();
				queueSession();
			}
			else
			{
				std::lock_guard<std::mutex> lk(mu);
				q.clear();
				batchRows.clear();
			}
		}

		void noteRow(const char* type, const std::string& row)
		{
			static const char* infoTypes[] = { "session", "hw", "gl", "settings", "knobs", "knobs_features" };
			for ( const char* t : infoTypes )
			{
				if ( !strcmp(type, t) )
				{
					std::lock_guard<std::mutex> lk(mu);
					infoRows.push_back(row);
					return;
				}
			}
			if ( consent != 1 ) { return; }
			if ( !strcmp(type, "sec") || !strcmp(type, "spike") || !strcmp(type, "ev") || !strcmp(type, "err") || !strcmp(type, "errsum") )
			{
				std::lock_guard<std::mutex> lk(mu);
				batchRows.push_back(row);
				batchBytes += row.size() + 1;
				// keep the newest rows that fit the table's 64 KB cap
				while ( batchBytes > kBatchCap - 4096 && !batchRows.empty() ) { batchBytes -= batchRows.front().size() + 1; batchRows.erase(batchRows.begin()); }
			}
		}

		void tick()
		{
			if ( consent != 1 ) { return; }
			if ( !sessionQueued && infoRows.size() >= 5 ) { queueSession(); } // after hw/gl/settings/knobs exist
			queueBatch(false);
		}

		void shutdownFlush()
		{
			if ( consent != 1 ) { return; }
			queueBatch(true);
			cv.notify_all();
			// give the worker a few seconds to drain, then stop it (the rest stays in the spool for the next launch)
			int64_t t0 = nowMs();
			while ( nowMs() - t0 < 4000 )
			{
				{
					std::lock_guard<std::mutex> lk(mu);
					if ( q.empty() ) { break; }
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
			stopping = true;
			cv.notify_all();
			if ( worker.joinable() ) { worker.join(); }
			// anything unsent is spooled for the next launch
			std::lock_guard<std::mutex> lk(mu);
			for ( auto& it : q ) { if ( it.spool.empty() ) { spoolWrite(it); } }
			q.clear();
		}

		void stats(uint64_t& ok, uint64_t& fail, uint64_t& drop, size_t& queued)
		{
			std::lock_guard<std::mutex> lk(mu);
			ok = sentOk; fail = sentFail; drop = dropped; queued = q.size();
		}
	}
}
