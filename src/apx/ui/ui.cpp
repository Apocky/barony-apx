// apx/ui/ui.cpp -- see ui.hpp (D92 P0).
#include "ui.hpp"
#include "inventory.hpp"
#include "../headless.hpp"
#include "../../main.hpp"
#include "../../game.hpp"
#include "../../draw.hpp"
#include "../../mod_tools.hpp"
#include "../../items.hpp"
#include "../../player.hpp"
#include "../../interface/interface.hpp"
#include "../../ui/Image.hpp"

#include <physfs.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <unordered_map>
#ifdef WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#endif

extern bool ImGui_ImplSDL2_Passive; // imgui_impl_sdl.cpp (apx): never capture, warp or poll the OS mouse

namespace apx
{
namespace ui
{
	namespace
	{
		bool g_ready = false, g_failed = false, g_begun = false;
		float g_mx = -1.f, g_my = -1.f;          // the pointer in xres/yres coordinates (from SDL motion events)
		std::vector<IdRect> g_cur, g_prev;       // this frame's rects, last complete frame's
		ImFont* g_body = nullptr; ImFont* g_small = nullptr; ImFont* g_head = nullptr;
		int g_scale = 1;
		double g_frameMs = 0.0, g_frameCpuMs = 0.0;
		std::vector<double> g_cpu;               // ring: this thread's CPU ms (QueryThreadCycleTime), immune to preemption by other processes

		// thread CPU time in ms: cycles counted only while this thread runs, converted at the TSC rate
		// (calibrated against the performance counter over the run so far)
		double threadCpuMs()
		{
#ifdef WINDOWS
			static Uint64 tsc0 = __rdtsc(), qpc0 = SDL_GetPerformanceCounter();
			ULONG64 cyc = 0;
			QueryThreadCycleTime(GetCurrentThread(), &cyc);
			const double secs = (double)(SDL_GetPerformanceCounter() - qpc0) / (double)SDL_GetPerformanceFrequency();
			const double rate = secs > 0.5 ? (double)(__rdtsc() - tsc0) / secs : 3.0e9; // cycles per second
			return (double)cyc * 1000.0 / rate;
#else
			return std::chrono::duration<double, std::milli>(clk::now().time_since_epoch()).count();
#endif
		}
		std::vector<double> g_cost, g_costBuild; // rings of CPU ms while open: total, and the build part (frame())
		size_t g_costPos = 0;
		GateStats g_gate;
		bool g_textStarted = false;              // we turned SDL text input on for our field (Barony keeps it off)

		using clk = std::chrono::steady_clock;
		double msSince(clk::time_point t) { return std::chrono::duration<double, std::milli>(clk::now() - t).count(); }

		ImFont* loadFont(const char* path, float px)
		{
			PHYSFS_File* f = PHYSFS_openRead(path);
			if ( !f ) { printlog("[apx-ui] font %s not found in the data dirs", path); return nullptr; }
			const PHYSFS_sint64 len = PHYSFS_fileLength(f);
			if ( len <= 0 ) { PHYSFS_close(f); return nullptr; }
			void* buf = IM_ALLOC((size_t)len);
			const PHYSFS_sint64 got = PHYSFS_readBytes(f, buf, (PHYSFS_uint64)len);
			PHYSFS_close(f);
			if ( got != len ) { IM_FREE(buf); return nullptr; }
			ImFontConfig cfg;
			cfg.OversampleH = 1; cfg.OversampleV = 1; cfg.PixelSnapH = true; // crisp pixel glyphs
			cfg.FontDataOwnedByAtlas = true;
			snprintf(cfg.Name, sizeof cfg.Name, "%s %d", path, (int)px);
			// Latin-1 plus General Punctuation: Barony's item text uses bullets and dashes from U+2000..U+206F
			static const ImWchar ranges[] = { 0x0001, 0x00FF, 0x2000, 0x206F, 0 }; // 0x1E is Barony's bullet glyph
			return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(buf, (int)len, px, &cfg, ranges);
		}

		void buildFonts()
		{
			ImGuiIO& io = ImGui::GetIO();
			io.Fonts->Clear();
			const float s = (float)g_scale;
			g_body = loadFont("fonts/pixel_maz_multiline.ttf", 16.f * s);
			g_small = loadFont("fonts/pixelmix.ttf", 8.f * s);
			g_head = loadFont("fonts/pixel_maz.ttf", 32.f * s);
			if ( !g_body ) { g_body = io.Fonts->AddFontDefault(); }
			if ( !g_small ) { g_small = g_body; }
			if ( !g_head ) { g_head = g_body; }
			io.FontDefault = g_body;
			ImGui_ImplOpenGL3_DestroyFontsTexture();
			ImGui_ImplOpenGL3_CreateFontsTexture();
			// nearest-neighbour on the atlas: pixel fonts stay sharp (section 3)
			const GLuint tex = (GLuint)(intptr_t)io.Fonts->TexID;
			glBindTexture(GL_TEXTURE_2D, tex);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glBindTexture(GL_TEXTURE_2D, 0);
			printlog("[apx-ui] fonts built at scale %d (body %s, head %s)", g_scale, g_body ? "pixel_maz_multiline" : "-", g_head != g_body ? "pixel_maz" : "-");
		}

		void applyStyle()
		{
			ImGuiStyle& st = ImGui::GetStyle();
			st = ImGuiStyle();
			st.WindowRounding = st.ChildRounding = st.FrameRounding = st.PopupRounding = 0.f;
			st.ScrollbarRounding = st.GrabRounding = st.TabRounding = 0.f;
			st.WindowBorderSize = 2.f; st.ChildBorderSize = 1.f; st.PopupBorderSize = 2.f; st.FrameBorderSize = 1.f; st.TabBorderSize = 1.f;
			st.WindowPadding = ImVec2(8, 8); st.FramePadding = ImVec2(6, 3); st.ItemSpacing = ImVec2(6, 4); st.ItemInnerSpacing = ImVec2(4, 4);
			st.ScrollbarSize = 12.f; st.GrabMinSize = 10.f; st.WindowTitleAlign = ImVec2(0.5f, 0.5f);
			st.AntiAliasedLines = false; st.AntiAliasedFill = false; st.AntiAliasedLinesUseTex = false; // pixel edges
			ImVec4* c = st.Colors;
			const ImVec4 panel(0.14f, 0.10f, 0.07f, 0.97f), panel2(0.20f, 0.14f, 0.10f, 1.f), panel3(0.28f, 0.20f, 0.13f, 1.f);
			const ImVec4 gold(0.80f, 0.62f, 0.25f, 1.f), goldHi(0.95f, 0.78f, 0.35f, 1.f), text(0.93f, 0.86f, 0.70f, 1.f);
			c[ImGuiCol_Text] = text; c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.50f, 0.40f, 1.f);
			c[ImGuiCol_WindowBg] = panel; c[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.07f, 0.05f, 0.85f); c[ImGuiCol_PopupBg] = panel2;
			c[ImGuiCol_Border] = gold; c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
			c[ImGuiCol_FrameBg] = panel2; c[ImGuiCol_FrameBgHovered] = panel3; c[ImGuiCol_FrameBgActive] = ImVec4(0.36f, 0.26f, 0.16f, 1.f);
			c[ImGuiCol_TitleBg] = panel2; c[ImGuiCol_TitleBgActive] = panel3; c[ImGuiCol_TitleBgCollapsed] = panel;
			c[ImGuiCol_MenuBarBg] = panel2; c[ImGuiCol_ScrollbarBg] = ImVec4(0.08f, 0.06f, 0.04f, 1.f);
			c[ImGuiCol_ScrollbarGrab] = ImVec4(0.50f, 0.38f, 0.18f, 1.f); c[ImGuiCol_ScrollbarGrabHovered] = gold; c[ImGuiCol_ScrollbarGrabActive] = goldHi;
			c[ImGuiCol_CheckMark] = goldHi; c[ImGuiCol_SliderGrab] = gold; c[ImGuiCol_SliderGrabActive] = goldHi;
			c[ImGuiCol_Button] = panel3; c[ImGuiCol_ButtonHovered] = ImVec4(0.45f, 0.32f, 0.16f, 1.f); c[ImGuiCol_ButtonActive] = ImVec4(0.60f, 0.44f, 0.20f, 1.f);
			c[ImGuiCol_Header] = panel3; c[ImGuiCol_HeaderHovered] = ImVec4(0.45f, 0.32f, 0.16f, 1.f); c[ImGuiCol_HeaderActive] = ImVec4(0.60f, 0.44f, 0.20f, 1.f);
			c[ImGuiCol_Separator] = ImVec4(0.55f, 0.42f, 0.18f, 1.f); c[ImGuiCol_SeparatorHovered] = gold; c[ImGuiCol_SeparatorActive] = goldHi;
			c[ImGuiCol_Tab] = panel2; c[ImGuiCol_TabHovered] = ImVec4(0.45f, 0.32f, 0.16f, 1.f); c[ImGuiCol_TabActive] = ImVec4(0.40f, 0.29f, 0.15f, 1.f);
			c[ImGuiCol_TabUnfocused] = panel2; c[ImGuiCol_TabUnfocusedActive] = panel3;
			c[ImGuiCol_PlotHistogram] = gold; c[ImGuiCol_TextSelectedBg] = ImVec4(0.60f, 0.44f, 0.20f, 0.6f);
			c[ImGuiCol_DragDropTarget] = goldHi; c[ImGuiCol_NavHighlight] = goldHi;
			st.ScaleAllSizes((float)g_scale);
		}

		int scaleFor(int h) { return std::max(1, std::min(4, h / 720)); }
	} // namespace

	void ensureContext()
	{
		if ( g_ready || g_failed ) { return; }
		if ( apx::headlessOn || !screen ) { return; } // no window to draw into
		IMGUI_CHECKVERSION();
		if ( !ImGui::GetCurrentContext() ) { ImGui::CreateContext(); }
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;  // no imgui.ini next to the game
		io.LogFilename = nullptr;
		ImGui_ImplSDL2_Passive = true; // never SDL_CaptureMouse, warp or read the global mouse: positions come from events
		if ( !ImGui_ImplSDL2_InitForOpenGL(screen, renderer) || !ImGui_ImplOpenGL3_Init() )
		{
			printlog("[apx-ui] ImGui backends failed to start; no apx UI this run");
			g_failed = true;
			return;
		}
		ImGui_t::colorBtnDefault = ImGui::GetStyleColorVec4(ImGuiCol_Button);
		ImGui_t::colorBtnDefaultActive = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
		ImGui_t::colorBtnDefaultHovered = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
		g_scale = scaleFor(yres);
		applyStyle();
		buildFonts();
		g_cost.assign(1500, 0.0); g_costBuild.assign(1500, 0.0); g_cpu.assign(1500, 0.0);
		g_ready = true;
		printlog("[apx-ui] context ready (%dx%d, ui scale %d)", xres, yres, g_scale);
	}

	bool contextReady() { return g_ready; }
	bool ownsTextInput() { return g_textStarted; }

	bool wantMouse()
	{
		if ( !g_ready ) { return false; }
		return inv::mouseOwned() || ImGui::GetIO().WantCaptureMouse;
	}

	bool processEvent(const SDL_Event& e)
	{
		if ( !g_ready ) { return false; }
		ImGuiIO& io = ImGui::GetIO();
		switch ( e.type )
		{
			case SDL_MOUSEMOTION:
			{
				// ImGui works in xres/yres pixels (DisplaySize); SDL reports window pixels
				int w = 0, h = 0;
				SDL_GetWindowSize(screen, &w, &h);
				g_mx = w > 0 ? e.motion.x * (float)xres / (float)w : (float)e.motion.x;
				g_my = h > 0 ? e.motion.y * (float)yres / (float)h : (float)e.motion.y;
				io.AddMousePosEvent(g_mx, g_my);
				return false; // Barony keeps its pointer (it draws the cursor over our window)
			}
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEWHEEL:
				ImGui_ImplSDL2_ProcessEvent(&e);
				if ( wantMouse() ) { ++g_gate.mouseSwallowed; return true; }
				return false;
			case SDL_MOUSEBUTTONUP:
			case SDL_KEYUP:
				ImGui_ImplSDL2_ProcessEvent(&e);
				return false; // releases always reach Barony: nothing sticks down
			case SDL_TEXTINPUT:
				ImGui_ImplSDL2_ProcessEvent(&e);
				if ( io.WantTextInput || g_textStarted ) { ++g_gate.textSwallowed; return true; }
				return false;
			case SDL_KEYDOWN:
				ImGui_ImplSDL2_ProcessEvent(&e);
				if ( io.WantTextInput || inv::wantsKey(e) ) { ++g_gate.keysSwallowed; return true; }
				return false;
			default:
				ImGui_ImplSDL2_ProcessEvent(&e);
				return false;
		}
	}

	void frame()
	{
		ensureContext();
		if ( !g_ready ) { return; }
		const auto t0 = clk::now();
		const double c0 = threadCpuMs();
		if ( g_begun ) { ImGui::EndFrame(); g_begun = false; } // a pass that drew nothing
		if ( scaleFor(yres) != g_scale ) { g_scale = scaleFor(yres); applyStyle(); buildFonts(); }
		ImGuiIO& io = ImGui::GetIO();
		if ( ImGui_t::isInit )
		{
			io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad; // the dev menu keeps its old gamepad nav
		}
		else
		{
			io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange; // Barony draws the cursor
			io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard); // our focus model
		}
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplSDL2_NewFrame();
		io.DisplaySize = ImVec2((float)xres, (float)yres);
		io.DisplayFramebufferScale = ImVec2(1.f, 1.f);
		if ( g_mx >= 0.f ) { io.AddMousePosEvent(g_mx, g_my); }
		ImGui::NewFrame();
		g_begun = true;
		g_prev.swap(g_cur);
		g_cur.clear();
		if ( ImGui_t::isInit ) { ImGui_t::drawDevMenu(); }
		inv::draw();
		// a focused text field of ours needs SDL text input (Barony keeps it off outside chat/console)
		if ( io.WantTextInput && !SDL_IsTextInputActive() ) { SDL_StartTextInput(); g_textStarted = true; }
		else if ( !io.WantTextInput && g_textStarted ) { SDL_StopTextInput(); g_textStarted = false; }
		g_frameMs = msSince(t0);
		g_frameCpuMs = threadCpuMs() - c0;
	}

	void render()
	{
		if ( !g_ready || !g_begun ) { return; }
		const auto t0 = clk::now();
		const double c0 = threadCpuMs();
		ImGui::Render();
		glViewport(0, 0, xres, yres); // no glGetError here: it can stall the driver every frame
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		g_begun = false;
		if ( inv::anyOpen() )
		{
			++g_gate.framesOpen;
			g_costBuild[g_costPos % g_cost.size()] = g_frameMs;
			g_cpu[g_costPos % g_cost.size()] = g_frameCpuMs + (threadCpuMs() - c0);
			g_cost[g_costPos++ % g_cost.size()] = g_frameMs + msSince(t0);
		}
	}

	void recordLastItem(const char* id)
	{
		if ( !g_ready || !id || !ImGui::IsItemVisible() ) { return; }
		ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 ca = dl->GetClipRectMin(), cb = dl->GetClipRectMax();
		a.x = std::max(a.x, ca.x); a.y = std::max(a.y, ca.y); b.x = std::min(b.x, cb.x); b.y = std::min(b.y, cb.y);
		if ( b.x <= a.x || b.y <= a.y ) { return; }
		g_cur.push_back({ id, a.x, a.y, b.x, b.y });
	}

	void recordRect(const char* id, const ImVec2& a, const ImVec2& b)
	{
		if ( g_ready && id && b.x > a.x && b.y > a.y ) { g_cur.push_back({ id, a.x, a.y, b.x, b.y }); }
	}

	bool findRect(const std::string& id, IdRect& out)
	{
		for ( auto& r : g_prev ) { if ( r.id == id ) { out = r; return true; } }
		for ( auto& r : g_cur ) { if ( r.id == id ) { out = r; return true; } }
		return false;
	}

	const std::vector<IdRect>& lastRects() { return g_prev; }

	ImFont* fontBody() { return g_body; }
	ImFont* fontSmall() { return g_small; }
	ImFont* fontHead() { return g_head; }
	int scale() { return g_scale; }

	void* itemIcon(int player, Item& item, int* w, int* h)
	{
		const std::string path = getItemSpritePath(player, item);
		if ( path.empty() ) { return nullptr; }
		Image* img = Image::get(path.c_str());
		if ( !img || !img->getTexID() ) { return nullptr; }
		if ( w ) { *w = (int)img->getWidth(); }
		if ( h ) { *h = (int)img->getHeight(); }
		return (void*)(intptr_t)img->getTexID();
	}

	Cost costWhileOpen()
	{
		Cost c;
		std::vector<double> v;
		const size_t n = std::min<size_t>(g_costPos, g_cost.size());
		v.assign(g_cost.begin(), g_cost.begin() + n);
		c.frames = (int)n;
		if ( v.empty() ) { return c; }
		c.last = g_cost[(g_costPos - 1) % g_cost.size()];
		std::sort(v.begin(), v.end());
		c.p50 = v[v.size() / 2];
		c.p99 = v[std::min(v.size() - 1, (size_t)(v.size() * 0.99))];
		c.max = v.back();
		std::vector<double> b(g_costBuild.begin(), g_costBuild.begin() + n);
		std::sort(b.begin(), b.end());
		c.buildP50 = b[b.size() / 2]; c.buildP99 = b[std::min(b.size() - 1, (size_t)(b.size() * 0.99))];
		std::vector<double> u(g_cpu.begin(), g_cpu.begin() + n);
		std::sort(u.begin(), u.end());
		c.cpuP50 = u[u.size() / 2]; c.cpuP99 = u[std::min(u.size() - 1, (size_t)(u.size() * 0.99))]; c.cpuMax = u.back();
		return c;
	}
	void resetCost() { g_costPos = 0; }

	const GateStats& gateStats() { return g_gate; }
} // namespace ui
} // namespace apx
