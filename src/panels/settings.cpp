// settings panel — hotkeys, world (level) commands, Project Settings window. EditorUI methods.
#include <editor/editorui.h>
#include <boost/filesystem.hpp>
#include <vector>

// Register the editor's built-in hotkeys in the shared pool. Plugins register their own the same
// way (nuke::Hotkeys::Get()->Register(...)); conflicts auto-resolve to UNBOUND for manual fixup.
void EditorUI::RegisterHotkeys()
{
	nuke::Hotkeys* hk = nuke::Hotkeys::Get();
	hk->Register("editor.world.new",  "New World",            ImGuiMod_Ctrl | ImGuiKey_N,     [this] { NewWorldCmd(); });
	hk->Register("editor.world.save", "Save World",           ImGuiMod_Ctrl | ImGuiKey_S,     [this] { SaveWorldCmd(); });
	hk->Register("editor.world.saveas", "Save World As",     ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, [this] { SaveWorldAsCmd(); });
	hk->Register("editor.world.open", "Open Default World",   ImGuiMod_Ctrl | ImGuiKey_O,     [this] { OpenWorldCmd(startupWorld); });
	hk->Register("editor.settings",   "Project Settings",     ImGuiMod_Ctrl | ImGuiKey_Comma, [this] { settingsOpen = true; });
	hk->Register("editor.edit.undo",  "Undo",                 ImGuiMod_Ctrl | ImGuiKey_Z,     [this] { Undo(); });
	hk->Register("editor.edit.redo",  "Redo",                 ImGuiMod_Ctrl | ImGuiKey_Y,     [this] { Redo(); });
	// Context hotkeys: registered in the pool (rebindable, shown in settings, conflict-aware) but
	// dispatched by the Browser only when it's hovered — so no global action (DispatchHotkeys skips
	// null-action entries). Default to the mouse back/forward buttons (M4/M5).
	hk->Register("editor.browser.back",    "Browser: Back",    ImGuiKey_MouseX1, nullptr);
	hk->Register("editor.browser.forward", "Browser: Forward", ImGuiKey_MouseX2, nullptr);
	hk->Register("editor.delete",          "Delete",           ImGuiKey_Delete, nullptr);
	hk->Register("editor.delete.force",    "Delete (no confirm)", ImGuiMod_Shift | ImGuiKey_Delete, nullptr);
	hk->Register("editor.browser.cut",     "Browser: Cut",     ImGuiMod_Ctrl | ImGuiKey_X, nullptr);
	hk->Register("editor.browser.copy",    "Browser: Copy",    ImGuiMod_Ctrl | ImGuiKey_C, nullptr);
	hk->Register("editor.browser.paste",   "Browser: Paste",   ImGuiMod_Ctrl | ImGuiKey_V, nullptr);
}

// Fire bound hotkeys whose chord is pressed this frame. Each chord maps to exactly one bound hotkey
// (conflicts are unbound), so nothing fires twice even when two plugins wanted the same combo.
void EditorUI::DispatchHotkeys()
{
	if (ImGui::GetIO().WantTextInput) return;            // typing in a field — don't trigger shortcuts
	if (!rebindId.empty()) return;                       // capturing a rebind — swallow input
	for (const nuke::Hotkey& h : nuke::Hotkeys::Get()->All())
		if (h.bound && h.action && ImGui::IsKeyChordPressed((ImGuiKeyChord)h.chord))
			h.action();
}

// A menu entry whose shortcut text + action come from a pooled hotkey (so the menu shows the live
// binding and stays in sync with rebinds).
void EditorUI::MenuHotkeyItem(const char* label, const char* id)
{
	nuke::Hotkey* h = nuke::Hotkeys::Get()->Find(id);
	const char* sc = (h && h->bound) ? ImGui::GetKeyChordName((ImGuiKeyChord)h->chord) : nullptr;
	if (ImGui::MenuItem(label, sc) && h && h->action) h->action();
}

void EditorUI::NewWorldCmd() { AppInstance::GetSingleton()->NewWorld(); ResetUndo(); SyncWorldBaseline(); }

void EditorUI::SaveWorldCmd()
{
	AppInstance* app = AppInstance::GetSingleton();
	std::string path = !app->currentWorldPath.empty() ? app->currentWorldPath
	                 : (!startupWorld.empty() ? startupWorld : std::string("world.nuworld"));
	app->SaveWorld(path);                                // writes into the project content
	if (startupWorld.empty()) { startupWorld = path; SaveProject(); }   // first world becomes the default
	SyncWorldBaseline();   // saved == disk now
}

void EditorUI::OpenWorldCmd(const std::string& relPath)
{
	if (relPath.empty()) return;
	AppInstance::GetSingleton()->OpenWorld(relPath);
	ResetUndo();
	SyncWorldBaseline();
}

// Open the "Save World As" modal; default the folder to the one currently open in the browser.
void EditorUI::SaveWorldAsCmd()
{
	AppInstance* app = AppInstance::GetSingleton();
	saveAsDir = browserCwd.empty() ? contentDir : browserCwd;
	std::string name = !app->currentWorldPath.empty() ? bfs::path(app->currentWorldPath).filename().string()
	                                                   : std::string("untitled.nuworld");
	strncpy(saveAsBuf, name.c_str(), sizeof(saveAsBuf) - 1); saveAsBuf[sizeof(saveAsBuf) - 1] = 0;
	openSaveAsPopup = true;
}

// Recursive folder tree for the save dialog — click a folder to select it as the save target.
void EditorUI::SaveAsFolderTree(const std::string& dir)
{
	boost::system::error_code ec;
	for (auto& de : bfs::directory_iterator(bfs::path(dir), ec))
	{
		if (!bfs::is_directory(de.path())) continue;
		std::string full = de.path().string();
		ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
		if (full == saveAsDir) fl |= ImGuiTreeNodeFlags_Selected;
		bool open = ImGui::TreeNodeEx((std::string(ICON_LC_FOLDER) + " " + de.path().filename().string()).c_str(), fl);
		if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) saveAsDir = full;
		if (open) { SaveAsFolderTree(full); ImGui::TreePop(); }
	}
}

void EditorUI::DrawSaveAsPopup()
{
	if (openSaveAsPopup) { ImGui::OpenPopup("Save World As"); openSaveAsPopup = false; }
	if (ImGui::BeginPopupModal("Save World As", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("Folder (under project content):");
		ImGui::BeginChild("##satree", ImVec2(400, 220), true);
		ImGuiTreeNodeFlags rf = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
		if (saveAsDir == contentDir) rf |= ImGuiTreeNodeFlags_Selected;
		bool rootOpen = ImGui::TreeNodeEx(ICON_LC_FOLDER " content", rf);
		if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) saveAsDir = contentDir;
		if (rootOpen) { SaveAsFolderTree(contentDir); ImGui::TreePop(); }
		ImGui::EndChild();

		ImGui::SetNextItemWidth(400);
		bool enter = ImGui::InputText("Name", saveAsBuf, sizeof(saveAsBuf), ImGuiInputTextFlags_EnterReturnsTrue);

		// Build the target path = chosen folder / name(.nuworld), then a content-relative path for SaveWorld.
		std::string name = saveAsBuf;
		const std::string ext = ".nuworld";
		if (!name.empty() && (name.size() < ext.size() || name.compare(name.size() - ext.size(), ext.size(), ext) != 0)) name += ext;
		boost::system::error_code ec;
		std::string folder = saveAsDir.empty() ? contentDir : saveAsDir;
		bfs::path   full    = bfs::path(folder) / name;
		bfs::path   relp    = bfs::relative(full, bfs::path(contentDir), ec);
		std::string rel     = (!ec && !relp.empty()) ? relp.generic_string() : name;
		bool        exists  = !name.empty() && bfs::exists(full, ec);
		bool        empty   = (saveAsBuf[0] == 0);

		ImGui::Text("-> content/%s", rel.c_str());
		if (exists) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1), ICON_LC_TRIANGLE_ALERT " Already exists — will be overwritten.");

		const char* label = exists ? "Overwrite" : "Save";
		ImGui::BeginDisabled(empty);
		// Enter saves a NEW file directly; an overwrite requires the explicit button (confirmation).
		if (ImGui::Button(label, ImVec2(120, 0)) || (enter && !exists && !empty))
		{
			AppInstance::GetSingleton()->SaveWorld(rel);   // forced into project content
			SyncWorldBaseline();   // saved == disk; path may have changed
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

// Open a .nuworld that was double-clicked in the browser (its full path -> content-relative).
void EditorUI::OpenWorldFromBrowser(const std::string& fullPath)
{
	boost::system::error_code ec;
	bfs::path rel = bfs::relative(bfs::path(fullPath), bfs::path(contentDir), ec);
	std::string r = (!ec && !rel.empty()) ? rel.generic_string() : fullPath;
	AppInstance::GetSingleton()->OpenWorld(r);
	ResetUndo();
	SyncWorldBaseline();
}

void EditorUI::winSettings()
{
	if (!settingsOpen) return;
	ImGui::SetNextWindowSize(ImVec2(460, 420), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Project Settings", &settingsOpen))
	{
		// --- Default world (the game loads this one after loading the project) ---
		ImGui::SeparatorText("World");
		std::vector<std::string> worlds;
		{
			boost::system::error_code ec;
			bfs::path root(contentDir);
			if (bfs::exists(root, ec))
				for (bfs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec))
				{
					if (ec) break;
					if (bfs::is_directory(it->path())) continue;
					if (it->path().extension() == ".nuworld")
						worlds.push_back(bfs::relative(it->path(), root, ec).string());
				}
		}
		const char* cur = startupWorld.empty() ? "(none)" : startupWorld.c_str();
		if (ImGui::BeginCombo("Default World", cur))
		{
			for (auto& w : worlds)
				if (ImGui::Selectable(w.c_str(), w == startupWorld) && w != startupWorld)
				{
					std::string before = startupWorld, after = w;
					startupWorld = after; SaveProject();
					PushUndo("Default world", [this, before]{ startupWorld = before; SaveProject(); },
					                          [this, after ]{ startupWorld = after;  SaveProject(); });
				}
			ImGui::EndCombo();
		}

		ImGui::SeparatorText("Rendering");
		const char* aaModes[] = { "Off", "MSAA 2x", "MSAA 4x", "MSAA 8x" };
		int aaIdx = (msaaSamples >= 8) ? 3 : (msaaSamples >= 4) ? 2 : (msaaSamples >= 2) ? 1 : 0;
		if (ImGui::Combo("Anti-aliasing", &aaIdx, aaModes, IM_ARRAYSIZE(aaModes)))
		{
			int s = (aaIdx == 3) ? 8 : (aaIdx == 2) ? 4 : (aaIdx == 1) ? 2 : 1;
			msaaSamples = s;
			if (AppInstance::GetSingleton()->render)
			{
				AppInstance::GetSingleton()->render->setMSAA(s);
				msaaSamples = AppInstance::GetSingleton()->render->getMSAA();   // device may clamp (e.g. 8x->4x)
			}
			SaveProject();
		}
		ImGui::SameLine(); ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hardware multisampling for the world. Clamped to GPU support.");

		if (ImGui::Checkbox("HDR", &hdrEnabled))
		{
			if (AppInstance::GetSingleton()->render) AppInstance::GetSingleton()->render->setHDR(hdrEnabled);
			SaveProject();
		}
		ImGui::SameLine(); ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("On: float (RGBA16F) rendering, real dynamic range (enables bloom later).\nOff: LDR (RGBA8), cheaper, tonemap inline.");

		ImGui::SeparatorText("Disk sync");
		const char* cleanModes[] = { "Ask", "Auto-reload" };
		if (ImGui::Combo("Disk changed (editor clean)", &reloadCleanMode, cleanModes, IM_ARRAYSIZE(cleanModes))) SaveProject();
		const char* conflModes[] = { "Ask", "Reload (use disk)", "Overwrite (use editor)", "Merge / resolve" };
		if (ImGui::Combo("Disk changed (editor dirty)", &conflictMode, conflModes, IM_ARRAYSIZE(conflModes))) SaveProject();

		// --- Hotkeys (centralized pool: rebind, see conflicts) ---
		ImGui::SeparatorText("Hotkeys");
		ImGui::Text("Rebind, then press a key combo. Conflicting hotkeys stay unbound — assign manually.");
		nuke::Hotkeys* hk = nuke::Hotkeys::Get();
		if (ImGui::BeginTable("hk", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Action");
			ImGui::TableSetupColumn("Binding");
			ImGui::TableSetupColumn("");
			ImGui::TableHeadersRow();
			for (const nuke::Hotkey& h : hk->All())
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn(); ImGui::TextUnformatted(h.name.c_str());
				ImGui::TableNextColumn();
				if (rebindId == h.id) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1), "press keys...");
				else if (h.bound)     ImGui::TextUnformatted(ImGui::GetKeyChordName((ImGuiKeyChord)h.chord));
				else                  ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1), "(unbound - conflict)");
				ImGui::TableNextColumn();
				ImGui::PushID(h.id.c_str());
				if (rebindId == h.id) { if (ImGui::SmallButton("cancel")) rebindId.clear(); }
				else                  { if (ImGui::SmallButton("rebind")) rebindId = h.id; }
				if (h.bound) { ImGui::SameLine(); if (ImGui::SmallButton("clear")) { hk->Unbind(h.id); SaveProject(); } }
				ImGui::PopID();
			}
			ImGui::EndTable();
		}

		// --- OS integration ---
		ImGui::SeparatorText("System");
		if (ImGui::Button("Register .nuproj file association"))
			RegisterProjectFileAssociation();   // HKCU: double-clicking a .nuproj opens this editor
		ImGui::SameLine();
		ImGui::Text("(current user; open .nuproj files in this editor)");

		// Capture a chord for the hotkey being rebound (first non-modifier key press + current mods).
		if (!rebindId.empty())
		{
			ImGuiIO& io = ImGui::GetIO();
			int mods = (io.KeyCtrl ? ImGuiMod_Ctrl : 0) | (io.KeyShift ? ImGuiMod_Shift : 0) | (io.KeyAlt ? ImGuiMod_Alt : 0);
			for (ImGuiKey k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k = (ImGuiKey)(k + 1))
			{
				if (k >= ImGuiKey_LeftCtrl && k <= ImGuiKey_RightSuper) continue;   // skip pure modifier keys
				if (ImGui::IsKeyPressed(k, false))
				{
					hk->Rebind(rebindId, mods | k);   // false on conflict -> hotkey keeps its state; pick another
					SaveProject();
					rebindId.clear();
					break;
				}
			}
		}
	}
	ImGui::End();
}
