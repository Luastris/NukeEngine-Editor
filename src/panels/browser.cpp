// browser panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>

// Icon for a (lowercased) file extension.
const char* EditorUI::ExtIcon(const std::string& ext)
{
	if (ext == ".numesh") return ICON_LC_BOX;
	if (ext == ".numat")  return ICON_LC_PALETTE;
	if (ext == ".nutex" || ext == ".png" || ext == ".jpg" || ext == ".jpeg") return ICON_LC_IMAGE;
	if (ext == ".nuprefab") return ICON_LC_PACKAGE;
	if (ext == ".nuworld")  return ICON_LC_GLOBE;
	if (ext == ".lua")      return ICON_LC_FILE_CODE;
	if (ext == ".hlsl" || ext == ".nushader") return ICON_LC_FILE_CODE;
	return ICON_LC_FILE;
}
// Whether a file of this extension passes the current type filters.
bool EditorUI::ExtVisible(const std::string& ext)
{
	if (ext == ".numesh") return fMesh;
	if (ext == ".numat")  return fMat;
	if (ext == ".nutex" || ext == ".png" || ext == ".jpg" || ext == ".jpeg") return fTex;
	if (ext == ".nuprefab") return fPrefab;
	return true;   // scripts, worlds, unknown — always shown
}
bool EditorUI::SearchMatch(const std::string& name)
{
	std::string q = browserSearch; for (char& c : q) c = (char)tolower((unsigned char)c);
	if (q.empty()) return true;
	std::string s = name; for (char& c : s) c = (char)tolower((unsigned char)c);
	return s.find(q) != std::string::npos;
}

// Recursive folder tree (Tree mode), rooted at the project content folder.
void EditorUI::BrowserTree(const std::string& dir)
{
	boost::system::error_code ec;
	for (auto& de : bfs::directory_iterator(bfs::path(dir), ec))
	{
		std::string name = de.path().filename().string();
		if (bfs::is_directory(de.path()))
		{
			if (ImGui::TreeNode((std::string(ICON_LC_FOLDER) + " " + name).c_str()))
			{
				BrowserTree(de.path().string());
				ImGui::TreePop();
			}
		}
		else
		{
			std::string ext = de.path().extension().string();
			for (char& c : ext) c = (char)tolower((unsigned char)c);
			if (ExtVisible(ext) && SearchMatch(name))
				ImGui::BulletText("%s %s", ExtIcon(ext), name.c_str());
		}
	}
}

// Reconstruct a .nuprefab into the current world and select it.
void EditorUI::InstantiatePrefab(const std::string& path)
{
	if (Atom* a = nuke::LoadPrefab(path))
	{
		AppInstance* app = AppInstance::GetSingleton();
		app->currentScene->Add(a);
		app->selectedInHieararchy = a;
		cout << "[editor]\tinstantiated prefab " << path << endl;
	}
}

// Begin renaming a browser entry: stash its path + current name, open the modal next frame.
void EditorUI::StartRename(const std::string& path)
{
	renamePath = path;
	std::string nm = bfs::path(path).filename().string();
	strncpy(renameBuf, nm.c_str(), sizeof(renameBuf) - 1);
	renameBuf[sizeof(renameBuf) - 1] = 0;
	openRenamePopup = true;
}

// Right-click context menu for a browser entry (call right after rendering the item).
void EditorUI::EntryContextMenu(const std::string& path, bool isDir)
{
	if (ImGui::BeginPopupContextItem())
	{
		if (ImGui::MenuItem(ICON_LC_PENCIL " Rename")) StartRename(path);
		if (ImGui::MenuItem(ICON_LC_TRASH_2 " Delete"))
		{
			boost::system::error_code ec;
			if (isDir) bfs::remove_all(path, ec); else bfs::remove(path, ec);
		}
		ImGui::EndPopup();
	}
}

// Rename modal (InputText + OK/Cancel). Performs bfs::rename within the same folder.
void EditorUI::DrawRenamePopup()
{
	if (openRenamePopup) { ImGui::OpenPopup("Rename##browser"); openRenamePopup = false; }
	if (ImGui::BeginPopupModal("Rename##browser", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextDisabled("%s", renamePath.c_str());
		ImGui::SetNextItemWidth(320);
		bool enter = ImGui::InputText("##rn", renameBuf, sizeof(renameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
		if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
		bool ok = ImGui::Button("OK") || enter;
		ImGui::SameLine();
		bool cancel = ImGui::Button("Cancel");
		if (ok && renameBuf[0])
		{
			boost::system::error_code ec;
			bfs::path src = renamePath;
			bfs::path dst = src.parent_path() / renameBuf;
			if (src != dst) bfs::rename(src, dst, ec);
			ImGui::CloseCurrentPopup();
		}
		if (cancel) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

// Folder navigation history (file-explorer style). NavigateTo pushes the current folder onto the
// back stack and clears forward; Back/Forward walk between them (driven by buttons + mouse M4/M5).
void EditorUI::BrowserNavigate(const std::string& path)
{
	if (path == browserCwd) return;
	browserBack.push_back(browserCwd);
	browserFwd.clear();
	browserCwd = path;
}
void EditorUI::BrowserBack()
{
	if (browserBack.empty()) return;
	browserFwd.push_back(browserCwd);
	browserCwd = browserBack.back();
	browserBack.pop_back();
}
void EditorUI::BrowserForward()
{
	if (browserFwd.empty()) return;
	browserBack.push_back(browserCwd);
	browserCwd = browserFwd.back();
	browserFwd.pop_back();
}

void EditorUI::winBrowser()
{
	if (!win->browser) return;
	ImGui::Begin("Browser", &win->browser, window_flags);

	// Back/Forward navigate the folder history while the browser is hovered. The chords come from the
	// centralized hotkey pool (rebindable in Project Settings, conflict-aware); default M4/M5. They're
	// dispatched HERE (context-sensitive) rather than globally, so they only act over the browser.
	if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
	{
		nuke::Hotkeys* hk = nuke::Hotkeys::Get();
		nuke::Hotkey* b = hk->Find("editor.browser.back");
		nuke::Hotkey* f = hk->Find("editor.browser.forward");
		if (b && b->bound && ImGui::IsKeyChordPressed((ImGuiKeyChord)b->chord)) BrowserBack();
		if (f && f->bound && ImGui::IsKeyChordPressed((ImGuiKeyChord)f->chord)) BrowserForward();
	}

	// --- toolbar: view mode | search | filters ---
	const char* modes[] = { ICON_LC_LAYOUT_GRID " Tiles", ICON_LC_LIST " List",
	                        ICON_LC_FOLDER_TREE " Tree", ICON_LC_BOXES " By Type" };
	ImGui::SetNextItemWidth(130);
	ImGui::Combo("##bview", &browserView, modes, 4);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(180);
	ImGui::InputTextWithHint("##bsearch", ICON_LC_SEARCH " Search", browserSearch, sizeof(browserSearch));
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_FILTER " Filters")) ImGui::OpenPopup("bfilters");
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_DOWNLOAD " Import"))
	{
		std::string src = EditorPickModelFile();   // OBJ/FBX/glTF/...
		if (!src.empty())
		{
			std::string dest = browserCwd.empty() ? contentDir : browserCwd;
			int n = AssImporter::getSingleton()->ImportToContent(src.c_str(), dest.c_str());
			cout << "[editor]\timported " << n << " mesh(es) into " << dest << endl;
		}
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Import a model (OBJ/FBX/glTF) -> .numesh in this folder");
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_FOLDER_PLUS " New Folder"))
	{
		boost::system::error_code fec;
		bfs::path base = browserCwd.empty() ? bfs::path(contentDir) : bfs::path(browserCwd);
		bfs::path dir  = base / "New Folder";
		for (int n = 1; bfs::exists(dir, fec); ++n) dir = base / ("New Folder (" + std::to_string(n) + ")");
		bfs::create_directory(dir, fec);
		StartRename(dir.string());   // immediately let the user name it
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Create a new folder here");
	if (ImGui::BeginPopup("bfilters"))
	{
		ImGui::Checkbox("Meshes", &fMesh);    ImGui::Checkbox("Materials", &fMat);
		ImGui::Checkbox("Textures", &fTex);   ImGui::Checkbox("Prefabs", &fPrefab);
		ImGui::EndPopup();
	}

	DrawRenamePopup();   // rename modal (works in all views; before the mode early-returns)

	// --- By Type: in-memory ResDB dump (kept as a separate mode) ---
	if (browserView == 3)
	{
		ImGui::Separator();
		ResDB* db = ResDB::getSingleton();
		if (ImGui::CollapsingHeader("Meshes", ImGuiTreeNodeFlags_DefaultOpen))
			for (Mesh* m : db->meshes) if (m) ImGui::BulletText("%s  (%s)", m->name, m->guid.c_str());
		if (ImGui::CollapsingHeader("Materials")) ImGui::Text("%d material(s)", (int)db->materials.size());
		if (ImGui::CollapsingHeader("Textures"))  ImGui::Text("%d texture(s)", (int)db->textures.size());
		if (ImGui::CollapsingHeader("Prefabs"))
			for (Atom* p : db->prefabs) if (p) ImGui::BulletText("%s", p->GetName().c_str());
		ImGui::End();
		return;
	}

	bfs::path root = bfs::path(contentDir);
	bfs::path cwd  = browserCwd.empty() ? root : bfs::path(browserCwd);

	// --- path bar: Up + current location (relative to the content root) ---
	ImGui::Separator();
	boost::system::error_code rc;
	bool atRoot = (cwd == root) || (bfs::exists(cwd, rc) && bfs::exists(root, rc) && bfs::equivalent(cwd, root, rc));
	nuke::Hotkeys* nav = nuke::Hotkeys::Get();
	auto navTip = [&](const char* label, const char* id) {
		nuke::Hotkey* h = nav->Find(id);
		if (h && h->bound) ImGui::SetTooltip("%s (%s)", label, ImGui::GetKeyChordName((ImGuiKeyChord)h->chord));
		else               ImGui::SetTooltip("%s", label);
	};
	ImGui::BeginDisabled(browserBack.empty());
	if (ImGui::Button(ICON_LC_ARROW_LEFT "##back")) BrowserBack();
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered()) navTip("Back", "editor.browser.back");
	ImGui::SameLine();
	ImGui::BeginDisabled(browserFwd.empty());
	if (ImGui::Button(ICON_LC_ARROW_RIGHT "##fwd")) BrowserForward();
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered()) navTip("Forward", "editor.browser.forward");
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_CORNER_LEFT_UP "##up") && !atRoot)
		BrowserNavigate(cwd.parent_path().string());
	ImGui::SameLine();
	bfs::path rel = bfs::relative(cwd, root, rc);
	std::string loc = "content";
	if (!rc && !rel.empty() && rel.generic_string() != ".") loc += "/" + rel.generic_string();
	ImGui::TextDisabled("%s", loc.c_str());
	ImGui::Separator();

	if (browserView == 2)   // Tree (recursive folders from the content root)
	{
		BrowserTree(root.string());
		ImGui::End();
		return;
	}

	// --- gather the current folder's entries (Tiles / List) ---
	struct FEntry { std::string name, path, ext; bool isDir; const char* icon; };
	std::vector<FEntry> entries;
	boost::system::error_code ec;
	const bool searching = (browserSearch[0] != 0);
	if (searching)
	{
		// Recurse: a search spans the whole subtree under the current folder (files only).
		for (auto& de : bfs::recursive_directory_iterator(cwd, ec))
		{
			if (bfs::is_directory(de.path())) continue;
			std::string name = de.path().filename().string();
			std::string ext  = de.path().extension().string();
			for (char& c : ext) c = (char)tolower((unsigned char)c);
			if (!ExtVisible(ext) || !SearchMatch(name)) continue;
			entries.push_back({ name, de.path().string(), ext, false, ExtIcon(ext) });
		}
	}
	else
	{
		for (auto& de : bfs::directory_iterator(cwd, ec))
		{
			bool dir = bfs::is_directory(de.path());
			std::string name = de.path().filename().string();
			std::string ext  = dir ? "" : de.path().extension().string();
			for (char& c : ext) c = (char)tolower((unsigned char)c);
			if (!dir && !ExtVisible(ext)) continue;
			entries.push_back({ name, de.path().string(), ext, dir, dir ? ICON_LC_FOLDER : ExtIcon(ext) });
		}
	}
	std::sort(entries.begin(), entries.end(), [](const FEntry& a, const FEntry& b) {
		if (a.isDir != b.isDir) return a.isDir > b.isDir;
		return a.name < b.name;
	});

	if (browserView == 0)            // Tiles
	{
		float cell = 84.0f, availW = ImGui::GetContentRegionAvail().x;
		int per = (int)(availW / cell); if (per < 1) per = 1;
		int i = 0;
		for (FEntry& e : entries)
		{
			ImGui::PushID(i);
			ImGui::BeginGroup();
			if (ImGui::Button(e.icon, ImVec2(64, 64)) && e.isDir) BrowserNavigate(e.path);
			if (!e.isDir && e.ext == ".nuprefab" && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
				InstantiatePrefab(e.path);
			EntryContextMenu(e.path, e.isDir);   // right-click: Rename / Delete
			char nm[24]; snprintf(nm, sizeof(nm), "%.20s", e.name.c_str());
			ImGui::TextUnformatted(nm);
			ImGui::EndGroup();
			ImGui::PopID();
			if (++i % per != 0) ImGui::SameLine();
		}
	}
	else                             // List
	{
		int i = 0;
		for (FEntry& e : entries)
		{
			ImGui::PushID(i++);
			if (ImGui::Selectable((std::string(e.icon) + "  " + e.name).c_str(), false,
			                      ImGuiSelectableFlags_AllowDoubleClick)
			    && ImGui::IsMouseDoubleClicked(0))
			{
				if (e.isDir)                       BrowserNavigate(e.path);
				else if (e.ext == ".nuprefab")     InstantiatePrefab(e.path);
			}
			EntryContextMenu(e.path, e.isDir);   // right-click: Rename / Delete
			ImGui::PopID();
		}
	}
	ImGui::End();
}
