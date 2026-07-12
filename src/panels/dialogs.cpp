// dialogs panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>
#include <nlohmann/json.hpp>
#include <boost/filesystem/fstream.hpp>
#include <cstdlib>   // getenv (preferences path)

void EditorUI::winAbout()
{
	if (!win->about) return;
	ImGui::Begin("About", &win->about, window_flags);
	ImGui::TextWrapped("NukeEngine - free, modular game engine. Renderer (Diligent) and UI (ImGui) "
	                   "are loaded as independent modules and communicate only through a neutral seam.");
	ImGui::End();
}

// ---- Console: viewer over the engine Log ring (cout/cerr are captured into it) -----------------

static const ImVec4 kLvColor[3] = { ImVec4(0.75f, 0.75f, 0.75f, 1),     // info
                                    ImVec4(1.00f, 0.80f, 0.30f, 1),     // warn
                                    ImVec4(1.00f, 0.40f, 0.35f, 1) };   // error
static const char* kLvIcon[3] = { ICON_LC_INFO, ICON_LC_TRIANGLE_ALERT, ICON_LC_CIRCLE_X };

// Mine a compiler-style "path(line[,col]): ..." reference out of a log LINE. dotnet/MSVC
// errors arrive through the cout capture as plain text — no structured source on the
// entry, but the reference is right there in the message.
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
	ImGui::Begin("Console", &win->console, window_flags);

	// Toolbar: severity toggles (with live counts), filter, clear, auto-scroll — all
	// FULL-SIZE controls (a SmallButton row next to a regular checkbox/input reads broken).
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

	// Snapshot only when the ring changed (Version is a cheap counter).
	const uint64_t v = nuke::Log::Version();
	const bool grew = v != conVersion;
	if (grew) { conCache = nuke::Log::Snapshot(); conVersion = v; }

	// Visible = severity toggles + substring filter (tag or text, case-insensitive).
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

	// Horizontal scrollbar: long lines (compiler errors with full paths) must stay
	// REACHABLE, not silently clipped at the panel edge.
	ImGui::BeginChild("##conlist", ImVec2(0, 0), ImGuiChildFlags_Borders,
	                  ImGuiWindowFlags_HorizontalScrollbar);
	static uint64_t conSelId = 0;       // selected entry (click) — Ctrl+C / context "Copy"
	std::string copyText;               // set when a copy is requested this frame
	ImGuiListClipper clip;
	clip.Begin((int)vis.size());
	while (clip.Step())
		for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
		{
			const nuke::LogEntry& e = *vis[i];
			ImGui::PushID((int)(e.id & 0x7fffffff));
			ImGui::PushStyleColor(ImGuiCol_Text, kLvColor[e.level]);
			// One selectable row: icon, xN collapse count, [tag], message.
			std::string row = std::string(kLvIcon[e.level]) + "  ";
			if (e.count > 1) row += "x" + std::to_string(e.count) + "  ";
			if (!e.tag.empty()) row += "[" + e.tag + "]  ";
			row += e.text;
			if (ImGui::Selectable(row.c_str(), e.id == conSelId, ImGuiSelectableFlags_AllowDoubleClick))
				conSelId = e.id;
			ImGui::PopStyleColor();
			// Source to jump to: the entry's own file/line, else a compiler-style
			// "path(line,col):" reference mined from the TEXT (dotnet/MSVC errors).
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
	ImGui::End();
}

// Resolve a log entry's source path (absolute; else project content; else engine shaders)
// and jump to it in the user's editor of choice.
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

// ---- Preferences: MACHINE-wide editor settings (%APPDATA%/NukeEngine/preferences.json) ---------

static bfs::path PreferencesPath()
{
	const char* appdata = std::getenv("APPDATA");
	bfs::path dir = appdata && *appdata ? bfs::path(appdata) / "NukeEngine" : bfs::path("config");
	return dir / "preferences.json";
}

void EditorUI::LoadPreferences()
{
	extEditors = EditorDetectExternalEditors();   // scan first — the choice must resolve
	bfs::ifstream f(PreferencesPath());
	if (f)
	{
		nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
		if (!j.is_discarded() && j.is_object())
		{
			extEditorName  = j.value("externalEditor", std::string());
			extCustomExe   = j.value("customEditorExe", std::string());
			extCustomArgs  = j.value("customEditorArgs", std::string());
		}
	}
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
	bfs::ofstream f(PreferencesPath(), std::ios::trunc);
	if (f) f << j.dump(2);
}

void EditorUI::winPreferences()
{
	if (!prefsOpen) return;
	if (prefsFocus) { ImGui::SetNextWindowFocus(); prefsFocus = false; }
	ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Preferences", &prefsOpen, window_flags | ImGuiWindowFlags_AlwaysAutoResize))
	{
		// Engine-wide (per machine/user, NOT per project) — lives in %APPDATA%/NukeEngine.
		ImGui::SeparatorText("External editor");
		ImGui::TextDisabled("Used by the Console's double-click-to-source and script/asset editing.");
		std::string cur = extEditorName.empty() ? std::string("(built-in text editor)") : extEditorName;
		ImGui::SetNextItemWidth(300);
		if (ImGui::BeginCombo("Default editor", cur.c_str()))
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

		// Custom entry: any exe + an argument template ({file} and {line} expand).
		ImGui::Spacing();
		ImGui::TextUnformatted("Custom editor");
		std::string exeShown = extCustomExe.empty() ? std::string("(pick an .exe)") : extCustomExe;
		if (ImGui::Button((exeShown + "##customexe").c_str(), ImVec2(300, 0)))
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
		ImGui::SameLine(0, 6); ImGui::TextUnformatted("Executable");
		{
			char buf[512];
			strncpy(buf, extCustomArgs.c_str(), sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
			ImGui::SetNextItemWidth(300);
			if (ImGui::InputText("Arguments", buf, sizeof(buf)))
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
	ImGui::End();
}

// Open file:line in the chosen external editor; no choice (or a vanished exe) falls back
// to the built-in text editor panel. A C# script resolves its PROJECT CONTEXT (the
// generated managed/GameScripts.csproj) — the IDE opens the whole project with
// IntelliSense over the engine API, never a lone file.
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
		// REUSE a running IDE: when its process already exists, the file-only arguments
		// route into the live instance (devenv /Edit, VSCode -r, Rider/N++ forwarders) —
		// the project-context variant is for the FIRST launch only, or every open spawns
		// a fresh IDE window.
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
	OpenTextFile(file, line);   // built-in editor (jumps to the line too)
}
