// About / Console panels, machine-wide preferences and external-editor launching.
#include <editor/editorui.h>
#include "nukeui.h"   // DocDetachAll
#include <nlohmann/json.hpp>
#include <boost/filesystem/fstream.hpp>
#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <API/Model/CrashReport.h>   // "last session crashed" viewer
#include <API/Model/DevConsole.h>    // console command line = the same executor as in-game
#include <interface/NukeVersion.h>   // NUKE_ENGINE_VERSION — the release name (About)

void EditorUI::winAbout()
{
	if (!win->about) return;
	NukeUI::DocPanel("panel:about", "About", &win->about, window_flags, 540, 330, [this]()
	{
#if defined(_WIN32)
		const char* plat = "Windows";
#elif defined(__APPLE__)
		const char* plat = "macOS";
#else
		const char* plat = "Linux";
#endif
		ImGui::Text("NukeEngine");
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(0.25f, 1.00f, 0.00f, 1.00f), NUKE_ENGINE_VERSION);
		ImGui::SameLine();
#ifdef NUKE_BUILD_ARCHS
		ImGui::TextDisabled("%s · %s", plat, NUKE_BUILD_ARCHS);
#else
		ImGui::TextDisabled("%s", plat);
#endif
		ImGui::TextWrapped("Free, modular C++20 engine for single-player desktop games "
		                   "with first-class modding.");
		ImGui::Spacing();
		ImGui::SeparatorText("");
		ImGui::BulletText("Hot-pluggable modules behind POD seams — renderer, physics,\n"
		                  "audio, scripting, runtime GUI; hot-reload with ABI gating");
		ImGui::BulletText("Diligent renderer: D3D11 / D3D12 / Vulkan, hardware ray tracing,\n"
		                  "HDR10, MSAA, water, VFX");
		ImGui::BulletText("Jolt physics · C# (.NET 8) + Lua scripting · dockable ImGui editor");
		ImGui::BulletText("Modding first-class: packed games mount read-only, mods are\n"
		                  "point diffs, mod C# loads additively — pipeline built into the editor");
		ImGui::BulletText("Windows, macOS, Linux — games ship self-contained\n"
		                  "(exe / .app / AppImage), packaged straight from the editor");
		ImGui::SeparatorText("");
		ImGui::TextDisabled("Luastris — luastris.com");
	});
}

// "Last session crashed": bundle loaded at SetUp from CrashReport::PendingBundle().
void EditorUI::winCrash()
{
	if (!crashShow) return;
	// The window itself never scrolls: the report text lives in its own scrolling child and
	// the buttons stay pinned at the bottom.
	NukeUI::DocPanel("panel:crash", "Crash Report", &crashShow,
	                 window_flags | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse,
	                 620, 420, [this]()
	{
		ImGui::TextColored(ImVec4(1.00f, 0.40f, 0.35f, 1),
		                   ICON_LC_CIRCLE_X "  The previous session crashed.");
		// The bundle header is JSON on disk — show it as a readable line, not raw braces.
		if (!crashInfo.empty())
		{
			nlohmann::json ji = nlohmann::json::parse(crashInfo, nullptr, false);
			if (ji.is_object())
			{
				std::string t = ji.value("time", std::string());   // "YYYYMMDD-HHMMSS"
				if (t.size() == 15)
					t = t.substr(0, 4) + "-" + t.substr(4, 2) + "-" + t.substr(6, 2) + " "
					  + t.substr(9, 2) + ":" + t.substr(11, 2) + ":" + t.substr(13, 2);
				ImGui::TextDisabled("%s  ·  %s  ·  %s",
				                    ji.value("host", std::string("?")).c_str(),
				                    ji.value("platform", std::string("?")).c_str(), t.c_str());
			}
			else ImGui::TextDisabled("%s", crashInfo.c_str());
		}
		// Short path — the full one is a tooltip and the Open Folder button.
		{
			bfs::path cp(crashDir);
			std::string shortDir = std::string("...") + (char)bfs::path::preferred_separator
			                     + cp.parent_path().filename().string() + (char)bfs::path::preferred_separator
			                     + cp.filename().string();
			ImGui::TextDisabled(ICON_LC_FOLDER " %s", shortDir.c_str());
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", crashDir.c_str());
		}
		ImGui::Separator();
		// Selectable + copyable: read-only multiline over the report buffer (mouse selection
		// and Ctrl+C work), frameless so it reads as text, not as a form field.
		{
			static const char* kEmpty = "(no crash.txt in the bundle)";
			std::string& t = crashText;
			ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
			ImGui::InputTextMultiline("##crash-text",
				t.empty() ? const_cast<char*>(kEmpty) : t.data(),
				(t.empty() ? std::strlen(kEmpty) : t.size()) + 1,
				ImVec2(-FLT_MIN, -ImGui::GetFrameHeightWithSpacing()),
				ImGuiInputTextFlags_ReadOnly);
			ImGui::PopStyleColor();
		}
		if (ImGui::Button(ICON_LC_COPY " Copy"))
			ImGui::SetClipboardText(crashText.c_str());
		ImGui::SameLine();
		if (ImGui::Button("Open Folder"))
		{
#if defined(_WIN32)
			std::string cmd = "explorer \"" + crashDir + "\"";
#elif defined(__APPLE__)
			std::string cmd = "open \"" + crashDir + "\"";
#else
			std::string cmd = "xdg-open \"" + crashDir + "\"";
#endif
			std::system(cmd.c_str());
		}
		ImGui::SameLine();
		if (ImGui::Button("Dismiss"))
		{
			nuke::CrashReport::ClearPending();
			crashShow = false;
		}
	});
}

// ---- Console: viewer over the engine Log ring (cout/cerr are captured into it) ----

static const ImVec4 kLvColor[3] = { ImVec4(0.75f, 0.75f, 0.75f, 1),     // info
                                    ImVec4(1.00f, 0.80f, 0.30f, 1),     // warn
                                    ImVec4(1.00f, 0.40f, 0.35f, 1) };   // error
static const char* kLvIcon[3] = { ICON_LC_INFO, ICON_LC_TRIANGLE_ALERT, ICON_LC_CIRCLE_X };

// Mine a compiler-style "path(line[,col]): ..." reference out of a log line.
// Returns true and fills file/line on a match.
static bool ParseSourceRef(const std::string& text, std::string& file, int& line)
{
	size_t search = 0, open;
	while ((open = text.find('(', search)) != std::string::npos)
	{
		search = open + 1;
		size_t close = text.find(')', open);
		if (close == std::string::npos) return false;
		if (close + 1 >= text.size() || text[close + 1] != ':') continue;
		std::string inside = text.substr(open + 1, close - open - 1);
		size_t comma = inside.find(',');
		std::string lnStr  = comma == std::string::npos ? inside : inside.substr(0, comma);
		std::string colStr = comma == std::string::npos ? "1"    : inside.substr(comma + 1);
		auto digits = [](const std::string& s) {
			if (s.empty()) return false;
			for (char c : s) if (!isdigit((unsigned char)c)) return false;
			return true;
		};
		if (!digits(lnStr) || !digits(colStr)) continue;
		std::string path = text.substr(0, open);
		size_t start = path.find_last_of('\t');                       // capture prefixes end with a tab
		if (start != std::string::npos) path = path.substr(start + 1);
		while (!path.empty() && path.front() == ' ') path.erase(path.begin());
		while (!path.empty() && path.back() == ' ')  path.pop_back();
		std::string low = path;
		for (char& c : low) c = (char)tolower((unsigned char)c);
		bool known = false;
		for (const char* x : { ".cs", ".lua", ".hlsl", ".cpp", ".h", ".json" })
		{
			size_t n = strlen(x);
			known |= low.size() > n && low.compare(low.size() - n, n, x) == 0;
		}
		if (!known) continue;
		file = path;
		line = atoi(lnStr.c_str());
		return true;
	}
	return false;
}

void EditorUI::winConsole()
{
	if (!win->console) return;
	NukeUI::DocPanel("panel:console", "Console", &win->console, window_flags, 920, 320, [this]()
	{

	int nInfo = 0, nWarn = 0, nErr = 0;
	nuke::Log::Counts(nInfo, nWarn, nErr);
	auto lvBtn = [&](int lv, int count) {
		char label[48];
		snprintf(label, sizeof(label), "%s %d##lv%d", kLvIcon[lv], count, lv);
		ImGui::PushStyleColor(ImGuiCol_Text, conShow[lv] ? kLvColor[lv]
		                                                 : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		if (ImGui::Button(label)) conShow[lv] = !conShow[lv];
		ImGui::PopStyleColor();
		ImGui::SameLine(0, 6);
	};
	lvBtn(0, nInfo); lvBtn(1, nWarn); lvBtn(2, nErr);
	if (ImGui::Button(ICON_LC_TRASH_2 " Clear")) nuke::Log::Clear();
	ImGui::SameLine(0, 6);
	ImGui::Checkbox("Auto-scroll", &conAutoScroll);
	ImGui::SameLine(0, 10);
	ImGui::SetNextItemWidth(-1);
	ImGui::InputTextWithHint("##confilter", ICON_LC_SEARCH " Filter (tag or text)", conFilter, sizeof(conFilter));
	ImGui::Separator();

	// Snapshot only when the ring changed; Version is a cheap counter.
	const uint64_t v = nuke::Log::Version();
	const bool grew = v != conVersion;
	if (grew) { conCache = nuke::Log::Snapshot(); conVersion = v; }

	auto ciContains = [](const std::string& hay, const char* needle) {
		std::string h = hay, n = needle;
		for (char& c : h) c = (char)tolower((unsigned char)c);
		for (char& c : n) c = (char)tolower((unsigned char)c);
		return n.empty() || h.find(n) != std::string::npos;
	};
	std::vector<const nuke::LogEntry*> vis;
	vis.reserve(conCache.size());
	for (const nuke::LogEntry& e : conCache)
	{
		if (!conShow[e.level]) continue;
		if (conFilter[0] && !ciContains(e.text, conFilter) && !ciContains(e.tag, conFilter)) continue;
		vis.push_back(&e);
	}

	// Horizontal scrollbar: long compiler-error lines must stay reachable, not clipped.
	// One input row stays reserved below the list: the console command line.
	ImGui::BeginChild("##conlist", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders,
	                  ImGuiWindowFlags_HorizontalScrollbar);
	static uint64_t conSelId = 0;       // selected entry
	std::string copyText;               // set when a copy is requested this frame
	ImGuiListClipper clip;
	clip.Begin((int)vis.size());
	while (clip.Step())
		for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
		{
			const nuke::LogEntry& e = *vis[i];
			ImGui::PushID((int)(e.id & 0x7fffffff));
			ImGui::PushStyleColor(ImGuiCol_Text, kLvColor[e.level]);
			std::string row = std::string(kLvIcon[e.level]) + "  ";
			if (e.count > 1) row += "x" + std::to_string(e.count) + "  ";
			if (!e.tag.empty()) row += "[" + e.tag + "]  ";
			row += e.text;
			if (ImGui::Selectable(row.c_str(), e.id == conSelId, ImGuiSelectableFlags_AllowDoubleClick))
				conSelId = e.id;
			ImGui::PopStyleColor();
			// Jump target: the entry's own file/line, else a reference mined from the text.
			std::string srcFile = e.file;
			int srcLine = e.line;
			if (srcFile.empty()) ParseSourceRef(e.text, srcFile, srcLine);
			if (ImGui::IsItemHovered())
			{
				if (!srcFile.empty())
					ImGui::SetTooltip("%s:%d\n(double-click to open)", srcFile.c_str(), srcLine);
				if (ImGui::IsMouseDoubleClicked(0) && !srcFile.empty())
				{
					nuke::LogEntry src = e;
					src.file = srcFile;
					src.line = srcLine;
					OpenLogSource(src);
				}
			}
			if (ImGui::BeginPopupContextItem("##conctx"))
			{
				conSelId = e.id;
				if (ImGui::MenuItem(ICON_LC_COPY " Copy")) copyText = e.text;
				if (!srcFile.empty() && ImGui::MenuItem(ICON_LC_FILE_PEN " Open source"))
				{
					nuke::LogEntry src = e;
					src.file = srcFile;
					src.line = srcLine;
					OpenLogSource(src);
				}
				ImGui::EndPopup();
			}
			if (e.id == conSelId && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
			    && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C))
				copyText = e.text;
			ImGui::PopID();
		}
	clip.End();
	if (!copyText.empty()) ImGui::SetClipboardText(copyText.c_str());
	if (conAutoScroll && grew && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40)
		ImGui::SetScrollHereY(1.0f);
	ImGui::EndChild();

	// The command line: the SAME executor as the in-game console (reflected statics + Lua).
	static char cmdBuf[1024] = { 0 };
	static std::vector<std::string> cmdHist;
	static int cmdPos = -1;
	auto histCb = [](ImGuiInputTextCallbackData* d) -> int {
		if (d->EventFlag != ImGuiInputTextFlags_CallbackHistory || cmdHist.empty()) return 0;
		if (d->EventKey == ImGuiKey_UpArrow)        cmdPos = cmdPos < 0 ? (int)cmdHist.size() - 1 : (cmdPos > 0 ? cmdPos - 1 : 0);
		else if (d->EventKey == ImGuiKey_DownArrow) { if (cmdPos >= 0 && ++cmdPos >= (int)cmdHist.size()) cmdPos = -1; }
		d->DeleteChars(0, d->BufTextLen);
		if (cmdPos >= 0 && cmdPos < (int)cmdHist.size()) d->InsertChars(0, cmdHist[cmdPos].c_str());
		return 0;
	};
	ImGui::SetNextItemWidth(-1);
	if (ImGui::InputTextWithHint("##concmd", ICON_LC_CHEVRON_RIGHT " Command (help / Type.Method args / Lua)",
	                             cmdBuf, sizeof(cmdBuf),
	                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, histCb))
	{
		if (cmdBuf[0])
		{
			if (cmdHist.empty() || cmdHist.back() != cmdBuf) cmdHist.push_back(cmdBuf);
			cmdPos = -1;
			nuke::Console::Execute(cmdBuf);
			cmdBuf[0] = 0;
		}
		ImGui::SetKeyboardFocusHere(-1);   // keep typing
	}
	});
}

void EditorUI::OpenLogSource(const nuke::LogEntry& e)
{
	boost::system::error_code ec;
	bfs::path p(e.file);
	if (!bfs::exists(p, ec))
	{
		bfs::path inContent = bfs::path(contentDir) / e.file;
		bfs::path inShaders = bfs::path("shaders") / bfs::path(e.file).filename();
		if      (bfs::exists(inContent, ec)) p = inContent;
		else if (bfs::exists(inShaders, ec)) p = inShaders;
		else { std::cout << "[Console]\tsource not found: " << e.file << std::endl; return; }
	}
	OpenExternal(bfs::absolute(p).string(), e.line);
}

// ---- Settings-window chrome (see SettingsShell in editorui.h) -------------------------------

static bool ShellContains(const char* hay, const char* needle)
{
	if (!hay || !needle || !*needle) return false;
	const size_t nh = strlen(hay), nn = strlen(needle);
	for (size_t i = 0; i + nn <= nh; ++i)
	{
		size_t j = 0;
		while (j < nn && tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j])) ++j;
		if (j == nn) return true;
	}
	return false;
}

bool SettingsShell::Begin(const char* id, const char* const* cats, int catCount)
{
	cats_ = cats; catCount_ = catCount;
	if (active >= catCount_) active = 0;
	ImGui::PushID(id);
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::InputTextWithHint("##filter", ICON_LC_SEARCH "  Filter settings...", filter, sizeof(filter));
	const float sideW = ImGui::GetFontSize() * 8.5f;
	ImGui::BeginChild("##cats", ImVec2(sideW, 0), ImGuiChildFlags_None);
	const bool filtering = filter[0] != 0;
	for (int i = 0; i < catCount_; ++i)
	{
		if (filtering) ImGui::BeginDisabled();   // the filter searches EVERY category
		if (ImGui::Selectable(cats_[i], !filtering && i == active)) active = i;
		if (filtering) ImGui::EndDisabled();
	}
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("##content", ImVec2(0, 0), ImGuiChildFlags_None);
	open_ = true;
	return true;
}

bool SettingsShell::Section(const char* cat, const char* label, const char* keywords)
{
	bool show;
	if (filter[0])
		show = ShellContains(label, filter) || ShellContains(keywords, filter) || ShellContains(cat, filter);
	else
		show = cats_ && active < catCount_ && strcmp(cat, cats_[active]) == 0;
	if (show) ImGui::SeparatorText(label);
	return show;
}

void SettingsShell::End()
{
	if (!open_) return;
	ImGui::EndChild();
	ImGui::PopID();
	open_ = false;
}

// ---- Preferences: machine-wide editor settings ---------------------------------------------
// <userDataDir>/NukeEngine/preferences.json: %APPDATA% on Windows, ~/Library/Application
// Support on macOS, XDG config on Linux.

static bfs::path PreferencesPath()
{
	return nuke::Config::userDataDir() / "NukeEngine" / "preferences.json";
}

void EditorUI::LoadPreferences()
{
	extEditors = EditorDetectExternalEditors();   // scan first: the saved choice must resolve
	bfs::ifstream f(PreferencesPath());
	if (f)
	{
		nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
		if (!j.is_discarded() && j.is_object())
		{
			extEditorName  = j.value("externalEditor", std::string());
			extCustomExe   = j.value("customEditorExe", std::string());
			extCustomArgs  = j.value("customEditorArgs", std::string());
			detachAssetEditors = j.value("detachAssetEditors", false);
#ifndef _WIN32
			editorBackend = 2;   // D3D is Windows-only — heal a prefs file carried over from Windows
#endif
			uiScalePct         = j.value("uiScale", 100);
			if (uiScalePct < 25) uiScalePct = 25; else if (uiScalePct > 300) uiScalePct = 300;
			displayBackend     = j.value("displayBackend", std::string("auto"));
			editorBackend      = j.value("editorBackend", 2);   // editor default = Vulkan
			editorRayTracing   = j.value("editorRayTracing", true);
			startupProjectMode = j.value("startupProject", 0);  // 0 = open last, 1 = always ask (hub)
			recentProjects.clear();
			if (j.contains("recentProjects") && j["recentProjects"].is_array())
				for (auto& r : j["recentProjects"])
					if (r.is_string()) recentProjects.push_back(r.get<std::string>());
		}
	}
	// Doc windows must know the default BEFORE their first frame: module draw callbacks can
	// run ahead of winAssetEditors' per-frame refresh.
	NukeUI::DocDetachDefault(detachAssetEditors);
	// ApplyStyle may have run before these prefs existed in memory — re-derive the scale
	// from the captured baseline (repeat applications never compound).
	NukeUI::SetUserUIScale((float)uiScalePct / 100.0f);
	if (!extCustomExe.empty())
		extEditors.push_back({ "Custom", extCustomExe,
		                       extCustomArgs.empty() ? "\"{file}\"" : extCustomArgs });
}

void EditorUI::SavePreferences()
{
	boost::system::error_code ec;
	bfs::create_directories(PreferencesPath().parent_path(), ec);
	nlohmann::json j;
	j["externalEditor"]   = extEditorName;
	j["customEditorExe"]  = extCustomExe;
	j["customEditorArgs"] = extCustomArgs;
	j["detachAssetEditors"] = detachAssetEditors;
	j["uiScale"]            = uiScalePct;
	j["displayBackend"]     = displayBackend;   // linux: auto / x11 / wayland (read RAW in main)
	j["editorBackend"]      = editorBackend;
	j["editorRayTracing"]   = editorRayTracing;
	j["startupProject"]     = startupProjectMode;   // 0 = open last project, 1 = always ask (hub)
	j["recentProjects"]     = recentProjects;       // newest-first absolute .nuproj paths
	bfs::ofstream f(PreferencesPath(), std::ios::trunc);
	if (f) f << j.dump(2);
}

void EditorUI::winPreferences()
{
	if (!prefsOpen) return;
	if (prefsFocus) { NukeUI::DocFocus("panel:preferences"); prefsFocus = false; }
	NukeUI::DocPanel("panel:preferences", "Preferences", &prefsOpen, window_flags, 640, 480, [this]()
	{
		static const char* kCats[] = { "Editor", "Interface", "Windows", "External editor", "System" };
		shellPrefs.Begin("prefs", kCats, IM_ARRAYSIZE(kCats));
		if (shellPrefs.Section("Editor", "Editor", "render backend ray tracing startup project vulkan d3d12"))
		{
			// Editor-only backend; the runtime (Player) backend is a project setting.
#ifdef _WIN32
			const char* ebModes[] = { "Direct3D 11", "Direct3D 12 (ray tracing)", "Vulkan (default)" };
			int eb = editorBackend;
			if (ImGui::Combo(LProp("Editor Render Backend").c_str(), &eb, ebModes, IM_ARRAYSIZE(ebModes)) && eb != editorBackend)
			{
				editorBackend = eb;
				SavePreferences();
			}
			ImGui::SameLine(); ImGui::TextDisabled("(?)");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Vulkan: native detachable windows (default).\n"
				                  "D3D12: RT reflections in the editor viewport.\n"
				                  "Applied on the next editor restart.");
#else
			// The D3D backends are Windows-only — offering dead options is worse than none.
			LProp("Editor Render Backend");
			ImGui::TextDisabled("Vulkan (the only backend on this OS)");
#endif
			// Off compiles shaders for the raster path (shadow maps + SSR) — no ray queries at all.
			bool ert = editorRayTracing;
			if (ImGui::Checkbox(LProp("Ray Tracing (editor)").c_str(), &ert) && ert != editorRayTracing)
			{
				editorRayTracing = ert;
				SavePreferences();
			}
			ImGui::SameLine(); ImGui::TextDisabled("(?)");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Off: the editor renders with shadow maps + SSR even on an\n"
				                  "RT-capable GPU (cheaper; matches non-RT players).\n"
				                  "The game's own switch is window.rayTracing in config.\n"
				                  "Applied on the next editor restart.");
			// Explicit opens (CLI arg, double-clicked .nuproj) always win over this.
			const char* spModes[] = { "Open last project", "Show project window" };
			int sp = startupProjectMode;
			if (ImGui::Combo(LProp("On startup").c_str(), &sp, spModes, IM_ARRAYSIZE(spModes)) && sp != startupProjectMode)
			{
				startupProjectMode = sp;
				SavePreferences();
			}
			ImGui::SameLine(); ImGui::TextDisabled("(?)");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Open last project: boots straight into the most recent project.\n"
				                  "Show project window: always asks (create / open / recent).\n"
				                  "Opening a .nuproj explicitly (argument, double-click) skips both.");
		}
		if (shellPrefs.Section("Interface", "Interface", "ui scale dpi hidpi display server wayland x11 xwayland"))
		{
			ImGui::SliderInt(LProp("UI Scale").c_str(), &uiScalePct, 25, 300, "%d%%", ImGuiSliderFlags_AlwaysClamp);
			// Applied on RELEASE, not per drag frame — the dynamic font atlas would otherwise
			// rasterize a size per pixel of mouse travel.
			if (ImGui::IsItemDeactivatedAfterEdit())
			{
				SavePreferences();
				NukeUI::SetUserUIScale((float)uiScalePct / 100.0f);
			}
			ImGui::SameLine(); ImGui::TextDisabled("(?)");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Size of the whole editor UI (text, panels, spacing).\n"
				                  "On top of the monitor's own scale: 100%% keeps the OS-correct\n"
				                  "size on Retina/HiDPI displays too. Applies when released.");
#if !defined(_WIN32) && !defined(__APPLE__)
			const char* dbModes[] = { "Auto (X11 for the editor)", "X11 (XWayland)", "Wayland (native)" };
			int db = displayBackend == "x11" ? 1 : displayBackend == "wayland" ? 2 : 0;
			if (ImGui::Combo(LProp("Display server").c_str(), &db, dbModes, IM_ARRAYSIZE(dbModes)))
			{
				displayBackend = db == 1 ? "x11" : db == 2 ? "wayland" : "auto";
				SavePreferences();
			}
			ImGui::SameLine(); ImGui::TextDisabled("(?)");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(
					"X11 (XWayland): the full multi-window editor — panels tear off into frameless\n"
					"  OS windows, ride on top while dragged, and drag-dock back. The compatibility\n"
					"  layer every Wayland desktop ships; there is no downside for a tool like this.\n"
					"Wayland (native): no XWayland in between — but the protocol forbids apps from\n"
					"  positioning windows or reading the global cursor, so: panels stay INSIDE the\n"
					"  main window, torn-off windows get a system frame and cannot drag-dock back.\n"
					"Auto: X11 here. (The game Player picks native Wayland on its own regardless —\n"
					"  a single game window needs none of the above.)\n"
					"Applied on the next editor restart. NUKE_DISPLAY_BACKEND overrides this.");
#endif
		}
		if (shellPrefs.Section("Windows", "Windows", "detach asset editors separate os windows dock"))
		{
		if (ImGui::Checkbox(LProp("Open asset editors detached by default", 20.0f).c_str(), &detachAssetEditors))
		{
			SavePreferences();
			// Applies to already-open editors too; hosts are torn down via wantDock on the
			// next frame, never from inside their own tick.
			for (AssetEditorWin& w : assetEds)
			{
				if (detachAssetEditors) { if (!w.host) w.detached = true; }
				else if (w.host)        { w.wantDock = true; }
			}
			NukeUI::DocDetachAll(detachAssetEditors);   // text editor + module doc windows too
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("On: material/mesh/prefab/... editors open as separate OS windows.\n"
			                  "Off (default): they open INSIDE the main window (dock or float them\n"
			                  "like any panel). Either way: drag an editor's title bar past the\n"
			                  "main-window edge to tear it off into its own OS window, and drag\n"
			                  "the detached window's tab back onto the main window to re-dock it.");
		}
		if (shellPrefs.Section("External editor", "External editor", "vs code rider sublime custom exe arguments file line"))
		{
		ImGui::TextDisabled("Used by the Console's double-click-to-source and script/asset editing.");
		std::string cur = extEditorName.empty() ? std::string("(built-in text editor)") : extEditorName;
		if (ImGui::BeginCombo(LProp("Default editor").c_str(), cur.c_str()))
		{
			if (ImGui::Selectable("(built-in text editor)", extEditorName.empty()))
			{ extEditorName.clear(); SavePreferences(); }
			for (const ExtEditor& ed : extEditors)
				if (ImGui::Selectable((ed.name + "##" + ed.exe).c_str(), extEditorName == ed.name))
				{ extEditorName = ed.name; SavePreferences(); }
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if (ImGui::Button(ICON_LC_ROTATE_CCW " Rescan"))
		{
			extEditors = EditorDetectExternalEditors();
			if (!extCustomExe.empty())
				extEditors.push_back({ "Custom", extCustomExe,
				                       extCustomArgs.empty() ? "\"{file}\"" : extCustomArgs });
		}
		for (const ExtEditor& ed : extEditors)
			if (ed.name == extEditorName)
				ImGui::TextDisabled("%s %s", ed.exe.c_str(), ed.args.c_str());

		ImGui::Spacing();
		ImGui::TextUnformatted("Custom editor");
		std::string exeShown = extCustomExe.empty() ? std::string("(pick an executable)") : extCustomExe;
		LProp("Executable");
		if (ImGui::Button((exeShown + "##customexe").c_str(), ImVec2(ImGui::GetFontSize() * 15.0f, 0)))
		{
			std::string p = EditorPickExeFile();
			if (!p.empty())
			{
				extCustomExe = p;
				SavePreferences();
				extEditors = EditorDetectExternalEditors();
				extEditors.push_back({ "Custom", extCustomExe,
				                       extCustomArgs.empty() ? "\"{file}\"" : extCustomArgs });
			}
		}
		{
			char buf[512];
			strncpy(buf, extCustomArgs.c_str(), sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
			if (ImGui::InputText(LProp("Arguments").c_str(), buf, sizeof(buf)))
			{
				extCustomArgs = buf;
				SavePreferences();
				for (ExtEditor& ed : extEditors)
					if (ed.name == "Custom") ed.args = extCustomArgs.empty() ? "\"{file}\"" : extCustomArgs;
			}
			ImGui::SameLine(); ImGui::TextDisabled("(?)");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("{file} and {line} expand, e.g.:  -g \"{file}:{line}\"");
		}
		}
		if (shellPrefs.Section("System", "System", "register nuproj file association double click open"))
		{
			// Machine-wide, not per-project — that's why it lives HERE and not in Project
			// Settings. Windows: HKCU. macOS: LaunchServices. Linux: XDG mime + .desktop.
			if (ImGui::Button("Register .nuproj file association"))
				RegisterProjectFileAssociation();
			ImGui::SameLine();
			ImGui::TextDisabled("(current user; open .nuproj files in this editor)");
		}
		shellPrefs.End();
	});
}

// Open file:line in the chosen external editor; falls back to the built-in text panel.
// A .cs file is opened through its generated GameScripts.csproj so the IDE gets project context.
void EditorUI::OpenExternal(const std::string& file, int line)
{
	boost::system::error_code ec;
	std::string ctxProject, ctxDir;
	{
		std::string ext = bfs::path(file).extension().string();
		for (char& c : ext) c = (char)tolower((unsigned char)c);
		if (ext == ".cs")
		{
			bfs::path cs = bfs::path(projectDir) / "managed" / "GameScripts.csproj";
			if (bfs::exists(cs, ec))
			{
				ctxProject = bfs::absolute(cs).string();
				ctxDir     = bfs::absolute(bfs::path(projectDir)).string();
			}
		}
	}
	for (const ExtEditor& ed : extEditors)
	{
		if (ed.name != extEditorName || extEditorName.empty()) continue;
		if (!bfs::exists(bfs::path(ed.exe), ec)) break;   // stale detection -> fallback
		// The project-context argument variant is for the FIRST launch only: a running IDE
		// takes the file-only args so it reuses the live instance instead of spawning a window.
		const bool running = EditorProcessRunning(ed.exe);
		std::string args = (!running && !ctxProject.empty() && !ed.argsProj.empty()) ? ed.argsProj : ed.args;
		auto replaceAll = [&](const char* what, const std::string& with) {
			const size_t n = strlen(what);
			for (size_t pos = 0; (pos = args.find(what, pos)) != std::string::npos; pos += with.size())
				args.replace(pos, n, with);
		};
		replaceAll("{file}", file);
		replaceAll("{line}", std::to_string(line > 0 ? line : 1));
		replaceAll("{project}", ctxProject);
		replaceAll("{projectDir}", ctxDir);
		if (EditorLaunchDetached(ed.exe, args)) return;
		break;   // launch failed -> fallback
	}
	OpenTextFile(file, line);   // built-in editor fallback
}
