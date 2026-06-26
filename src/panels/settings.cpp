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
	hk->Register("editor.world.open", "Open Default World",   ImGuiMod_Ctrl | ImGuiKey_O,     [this] { OpenWorldCmd(startupWorld); });
	hk->Register("editor.settings",   "Project Settings",     ImGuiMod_Ctrl | ImGuiKey_Comma, [this] { settingsOpen = true; });
	// Context hotkeys: registered in the pool (rebindable, shown in settings, conflict-aware) but
	// dispatched by the Browser only when it's hovered — so no global action (DispatchHotkeys skips
	// null-action entries). Default to the mouse back/forward buttons (M4/M5).
	hk->Register("editor.browser.back",    "Browser: Back",    ImGuiKey_MouseX1, nullptr);
	hk->Register("editor.browser.forward", "Browser: Forward", ImGuiKey_MouseX2, nullptr);
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

void EditorUI::NewWorldCmd() { AppInstance::GetSingleton()->NewWorld(); }

void EditorUI::SaveWorldCmd()
{
	AppInstance* app = AppInstance::GetSingleton();
	std::string path = !app->currentWorldPath.empty() ? app->currentWorldPath
	                 : (!startupWorld.empty() ? startupWorld : std::string("world.nuworld"));
	app->SaveWorld(path);                                // writes into the project content
	if (startupWorld.empty()) { startupWorld = path; SaveProject(); }   // first world becomes the default
}

void EditorUI::OpenWorldCmd(const std::string& relPath)
{
	if (relPath.empty()) return;
	AppInstance::GetSingleton()->OpenWorld(relPath);
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
				if (ImGui::Selectable(w.c_str(), w == startupWorld)) { startupWorld = w; SaveProject(); }
			ImGui::EndCombo();
		}

		// --- Hotkeys (centralized pool: rebind, see conflicts) ---
		ImGui::SeparatorText("Hotkeys");
		ImGui::TextDisabled("Rebind, then press a key combo. Conflicting hotkeys stay unbound — assign manually.");
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
		ImGui::TextDisabled("(current user; open .nuproj files in this editor)");

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
