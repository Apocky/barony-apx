// apx/shot.cpp -- BF1-5: GL back-buffer screenshot hook.
//
// The in-game takeScreenshot() runs AFTER GO_SwapBuffers, so it reads the buffer the swap just made
// undefined. PrintWindow of a fullscreen OpenGL window returns black. This hook runs INSIDE GO_SwapBuffers,
// after the frame is resolved to framebuffer 0 (non-HDR: main_framebuffer.draw; HDR: the camera end pass
// already wrote it) and before SDL_GL_SwapWindow, so one hook point covers both modes.
//
// Triggers (any): env APX_SHOT_AT_TICK=<n[,n..]>  | file <out>/shot.req containing a file name (deleted when
// served)  | console `/apxshot <name>`  | the in-game Screenshot key (name empty -> "Screenshot <time>.png").
// Output: <out>/shots/<name>.png and <name>.json (tick, floor, seed, floor_hash, w, h, mean_luma, std_luma, hdr).
#include "telemetry.hpp"
#include "../main.hpp"
#include "../game.hpp"
#include "../entity.hpp"
#include "../draw.hpp"
#include "../files.hpp"
#include "../savepng.hpp"
#include <SDL.h>
#include <GL/glew.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>


namespace apx
{
	uint64_t lastFloorHash = 0;
	static std::string pendingName;
	static bool pendingFlag = false;
	static std::vector<Uint32> atTicks;
	static bool atInit = false;
	static size_t atNext = 0;

	void shotRequest(const char* name)
	{
		pendingName = name ? name : "";
		pendingFlag = true;
	}

	static void initAt()
	{
		atInit = true;
		const char* e = getenv("APX_SHOT_AT_TICK");
		if ( !e ) { return; }
		std::string s = e;
		size_t i = 0;
		while ( i < s.size() )
		{
			size_t j = s.find(',', i);
			if ( j == std::string::npos ) { j = s.size(); }
			atTicks.push_back((Uint32)strtoul(s.substr(i, j - i).c_str(), nullptr, 10));
			i = j + 1;
		}
	}
}

static bool apxCapture(SDL_Window* win, std::string name)
{
	using namespace apx;
	if ( name.empty() )
	{
		char tb[32];
		getTimeAndDateFormatted(getTime(), tb, sizeof(tb));
		name = std::string("Screenshot ") + tb;
	}
	for ( char& c : name ) { if ( c == '/' || c == '\\' || c == ':' ) { c = '_'; } }

	int w = 0, h = 0;
	SDL_GL_GetDrawableSize(win, &w, &h);
	if ( w <= 0 || h <= 0 ) { ++shotsFailed; return false; }
	std::vector<Uint8> px((size_t)w * h * 4);
	GLint prevBind = 0, prevRead = 0;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevBind);
	glGetIntegerv(GL_READ_BUFFER, &prevRead);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glReadBuffer(GL_BACK);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
	GLenum err = glGetError();
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevBind);
	glReadBuffer((GLenum)prevRead);
	if ( err != GL_NO_ERROR ) { ++shotsFailed; printlog("[apx] shot GL error %u", (unsigned)err); return false; }

	// flip vertically, drop alpha, measure
	std::vector<Uint8> rgb((size_t)w * h * 3);
	double sum = 0, sum2 = 0;
	for ( int y = 0; y < h; ++y )
	{
		const Uint8* src = &px[(size_t)(h - 1 - y) * w * 4];
		Uint8* dst = &rgb[(size_t)y * w * 3];
		for ( int x = 0; x < w; ++x )
		{
			dst[x * 3] = src[x * 4]; dst[x * 3 + 1] = src[x * 4 + 1]; dst[x * 3 + 2] = src[x * 4 + 2];
			double l = 0.2126 * src[x * 4] + 0.7152 * src[x * 4 + 1] + 0.0722 * src[x * 4 + 2];
			sum += l; sum2 += l * l;
		}
	}
	double n = (double)w * h, mean = sum / n, sd = sqrt(std::max(0.0, sum2 / n - mean * mean));
	std::string dir = enabled ? outDir() + "/shots" : std::string(outputdir);
	if ( enabled ) { std::string d = dir; for ( char& c : d ) { if ( c == '/' ) { c = '\\'; } } system(("if not exist \"" + d + "\" mkdir \"" + d + "\"").c_str()); }
	std::string path = dir + "/" + name + ".png";
	SDL_Surface* s = SDL_CreateRGBSurfaceFrom(rgb.data(), w, h, 24, w * 3, 0x0000ff, 0x00ff00, 0xff0000, 0);
	bool ok = s && SDL_SavePNG(s, path.c_str()) == 0;
	if ( s ) { SDL_FreeSurface(s); }
	if ( !ok ) { ++shotsFailed; printlog("[apx] shot save failed: %s", path.c_str()); return false; }
	if ( enabled )
	{
		ApxRow r;
		commonFields(r);
		r.str("file", name + ".png").i("w", w).i("h", h).d("mean_luma", mean).d("std_luma", sd).i("hdr", hdrEnabled ? 1 : 0)
			.hex64("floor_hash", lastFloorHash).i("shots_failed", (long long)shotsFailed);
		std::string js = r.done();
		FILE* f = fopen((dir + "/" + name + ".json").c_str(), "wb");
		if ( f ) { fwrite(js.data(), 1, js.size(), f); fclose(f); }
		ApxRow r2;
		commonFields(r2);
		r2.str("file", name + ".png").d("mean_luma", mean).d("std_luma", sd);
		write("shots.jsonl", r2);
	}
	printlog("[apx] shot %s (%dx%d luma %.1f sd %.1f hdr %d)", path.c_str(), w, h, mean, sd, hdrEnabled ? 1 : 0);
	return true;
}

void apxShotMaybe(SDL_Window* win)
{
	using namespace apx;
	if ( !enabled && !pendingFlag ) { return; }
	if ( !atInit ) { initAt(); }
	std::string name;
	bool want = false;
	if ( pendingFlag ) { want = true; name = pendingName; pendingFlag = false; }
	if ( !want && atNext < atTicks.size() && ticks >= atTicks[atNext] )
	{
		char b[64];
		snprintf(b, sizeof(b), "tick%u", atTicks[atNext]);
		name = b;
		++atNext;
		want = true;
	}
	if ( !want && enabled && (ticks % 10) == 0 )
	{
		std::string req = outDir() + "/shot.req";
		if ( FILE* f = fopen(req.c_str(), "rb") )
		{
			char buf[256] = {};
			size_t n = fread(buf, 1, sizeof(buf) - 1, f);
			fclose(f);
			while ( n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ') ) { buf[--n] = 0; }
			name = buf;
			remove(req.c_str());
			want = true;
		}
	}
	if ( !want ) { return; }
	static const bool twice = getenv("APX_SHOT_TWICE") && getenv("APX_SHOT_TWICE")[0] == '1';
	if ( name.empty() )
	{
		char tb[32];
		getTimeAndDateFormatted(getTime(), tb, sizeof(tb));
		name = std::string("Screenshot ") + tb;
	}
	for ( char& c : name ) { if ( c == '/' || c == '\\' || c == ':' ) { c = '_'; } }
	if ( twice ) { apxCapture(win, name + "_a"); apxCapture(win, name + "_b"); }
	else { apxCapture(win, name); }
}
