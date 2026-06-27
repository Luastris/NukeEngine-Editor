// browser panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>
#include "API/Model/Material.h"   // regen GUIDs of copied assets
#include "API/Model/Mesh.h"
#include "API/Model/Texture.h"
#include <nlohmann/json.hpp>      // dependency scan + unlink (rewrite reference fields)
#include <boost/filesystem/fstream.hpp>
#include <algorithm>
#include <iterator>

// Rename/move a file or folder on disk + keep ResDB's guid<->path in sync; a renamed .numat also
// gets its internal name re-synced. One canonical operation so undo can replay it in either direction.
static void DoFileMove(const std::string& from, const std::string& to)
{
	if (from == to) return;
	boost::system::error_code ec;
	bfs::rename(from, to, ec);
	ResDB* db = ResDB::getSingleton();
	db->MoveAssetPath(from, to);
	if (bfs::path(to).extension() == ".numat")
		if (Material* m = db->GetMaterial(db->GuidForPath(to)))
		{
			m->matName = bfs::path(to).stem().string();
			m->SaveToFile(to);
		}
}

void EditorUI::RecordFileMove(const std::string& from, const std::string& to)
{
	if (from == to) return;
	PushUndo("Move " + bfs::path(to).filename().string(),
		[from, to]{ DoFileMove(to, from); },
		[from, to]{ DoFileMove(from, to); });
}

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
Atom* EditorUI::InstantiatePrefab(const std::string& path)
{
	if (Atom* a = nuke::LoadPrefab(path))
	{
		AppInstance* app = AppInstance::GetSingleton();
		app->currentScene->Add(a);
		app->selectedInHieararchy = a;
		RecordAdd(a);
		cout << "[editor]\tinstantiated prefab " << path << endl;
		return a;
	}
	return nullptr;
}

// Begin renaming a browser entry: edit only the NAME — the extension is locked (changing it would
// make the engine unable to load the asset). Folders have no extension, so the whole name is edited.
void EditorUI::StartRename(const std::string& path)
{
	renamePath = path;
	bfs::path p(path);
	boost::system::error_code ec;
	std::string stem;
	if (bfs::is_directory(p, ec)) { stem = p.filename().string(); renameExt = ""; }
	else                          { stem = p.stem().string();     renameExt = p.extension().string(); }
	strncpy(renameBuf, stem.c_str(), sizeof(renameBuf) - 1);
	renameBuf[sizeof(renameBuf) - 1] = 0;
	openRenamePopup = true;
}

// Right-click context menu for a browser entry (call right after rendering the item).
void EditorUI::EntryContextMenu(const std::string& path, bool isDir)
{
	if (ImGui::BeginPopupContextItem())
	{
		// World-specific actions: open it, or make it the project's default.
		if (!isDir && bfs::path(path).extension() == ".nuworld")
		{
			if (ImGui::MenuItem(ICON_LC_GLOBE " Open World")) OpenWorldFromBrowser(path);
			if (ImGui::MenuItem(ICON_LC_STAR " Set as Default"))
			{
				boost::system::error_code ec;
				bfs::path rel = bfs::relative(bfs::path(path), bfs::path(contentDir), ec);
				if (!ec && !rel.empty()) { startupWorld = rel.generic_string(); SaveProject(); }
			}
			ImGui::Separator();
		}
		if (ImGui::MenuItem(ICON_LC_SCISSORS " Cut",  "Ctrl+X"))  { clipboard = { path }; clipboardCut = true;  }
		if (ImGui::MenuItem(ICON_LC_COPY " Copy",     "Ctrl+C"))  { clipboard = { path }; clipboardCut = false; }
		if (ImGui::MenuItem(ICON_LC_CLIPBOARD_PASTE " Paste", "Ctrl+V", false, !clipboard.empty())) BrowserPaste();
		ImGui::Separator();
		if (ImGui::MenuItem(ICON_LC_PENCIL " Rename")) StartRename(path);
		if (ImGui::MenuItem(ICON_LC_TRASH_2 " Delete")) RequestDelete(path);   // confirm (+ dependents list)
		ImGui::EndPopup();
	}
}

// Rename modal (InputText + OK/Cancel). Performs bfs::rename within the same folder.
void EditorUI::DrawRenamePopup()
{
	if (openRenamePopup) { ImGui::OpenPopup("Rename##browser"); openRenamePopup = false; }
	if (ImGui::BeginPopupModal("Rename##browser", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("%s", renamePath.c_str());
		ImGui::SetNextItemWidth(280);
		bool enter = ImGui::InputText("##rn", renameBuf, sizeof(renameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
		if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
		if (!renameExt.empty()) { ImGui::SameLine(0, 0); ImGui::Text("%s", renameExt.c_str()); }   // locked extension

		boost::system::error_code ec;
		bfs::path src = renamePath;
		bfs::path dst = src.parent_path() / (std::string(renameBuf) + renameExt);   // name + locked ext
		// Block if a DIFFERENT entry with that name already exists (no silent overwrite).
		bool clash = renameBuf[0] && dst != src && bfs::exists(dst, ec);
		if (clash) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1), ICON_LC_TRIANGLE_ALERT " A file/folder with that name already exists here.");

		ImGui::BeginDisabled(!renameBuf[0] || clash);
		bool ok = ImGui::Button("OK") || (enter && renameBuf[0] && !clash);
		ImGui::EndDisabled();
		ImGui::SameLine();
		bool cancel = ImGui::Button("Cancel");
		if (ok && renameBuf[0] && !clash)
		{
			if (src != dst)
			{
				DoFileMove(src.string(), dst.string());          // rename + ResDB sync + .numat name
				RecordFileMove(src.string(), dst.string());      // undoable
				if (browserSel == src.string()) browserSel = dst.string();
			}
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

// Return `desired` if free, else append " (n)" before the extension until the name is unused —
// so moving/creating never silently overwrites an existing file or folder of the same name.
static bfs::path UniquePath(const bfs::path& desired)
{
	boost::system::error_code ec;
	if (!bfs::exists(desired, ec)) return desired;
	bfs::path dir = desired.parent_path();
	std::string stem = desired.stem().string(), ext = desired.extension().string();
	for (int n = 2; ; ++n)
	{
		bfs::path cand = dir / (stem + " (" + std::to_string(n) + ")" + ext);
		if (!bfs::exists(cand, ec)) return cand;
	}
}

// Make the last-drawn item a drag source carrying an asset path (payload "NUKE_ASSET").
void EditorUI::BrowserDragSource(const std::string& path)
{
	if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
	{
		ImGui::SetDragDropPayload("NUKE_ASSET", path.c_str(), path.size() + 1);
		ImGui::TextUnformatted(bfs::path(path).filename().string().c_str());
		ImGui::EndDragDropSource();
	}
}

// Save a scene atom (with its children) as a .nuprefab in `folder`. The prefab is a snapshot —
// later edits to the world atom don't change the file (no live link yet).
void EditorUI::SaveAtomAsPrefab(Atom* a, const std::string& folder)
{
	if (!a) return;
	bfs::path path = UniquePath(bfs::path(folder) / (a->GetName() + ".nuprefab"));
	a->prefabGuid = ResDB::NewGuid();   // the source atom becomes an instance of this new prefab
	if (nuke::SavePrefab(a, path.string()))
	{
		ResDB::getSingleton()->SetAssetPath(a->prefabGuid, path.string());   // resolve guid -> file
		browserSel = path.string();
		cout << "[editor]\tsaved prefab " << path.string() << " (" << a->prefabGuid << ")" << endl;
	}
}

// Recursively copy a file or a whole folder tree (dst must not already exist — callers use UniquePath).
static void CopyRecursive(const bfs::path& src, const bfs::path& dst, boost::system::error_code& ec)
{
	if (bfs::is_directory(src))
	{
		bfs::create_directories(dst, ec);
		for (bfs::directory_iterator it(src, ec), end; it != end && !ec; it.increment(ec))
			CopyRecursive(it->path(), dst / it->path().filename(), ec);
	}
	else bfs::copy_file(src, dst, ec);
}

// A freshly COPIED asset still carries the original's internal GUID — two files sharing one GUID
// collide in ResDB on the next project load. Rewrite the copy's GUID on disk (and register the copy
// live so it shows in pickers immediately). Worlds/prefabs have no ResDB-keyed GUID — left as-is.
static void RegenAssetGuid(const bfs::path& file)
{
	std::string ext = file.extension().string();
	for (char& c : ext) c = (char)tolower((unsigned char)c);
	ResDB* db = ResDB::getSingleton();
	const std::string p = file.string();
	if (ext == ".numat")
	{
		if (Material* m = Material::LoadFromFile(p)) { m->guid = ResDB::NewGuid(); m->SaveToFile(p); db->RegisterMaterial(m); db->SetAssetPath(m->guid, p); }
	}
	else if (ext == ".numesh")
	{
		if (Mesh* m = Mesh::LoadFromFile(p)) { m->guid = ResDB::NewGuid(); m->SaveToFile(p); db->RegisterMesh(m); db->SetAssetPath(m->guid, p); }
	}
	else if (ext == ".nutex")
	{
		if (Texture* t = Texture::LoadFromFile(p)) { t->guid = ResDB::NewGuid(); t->SaveToFile(p); db->RegisterTexture(t); db->SetAssetPath(t->guid, p); }
	}
}

// Regenerate GUIDs across a just-copied file or folder tree.
static void RegenGuidsIn(const bfs::path& path)
{
	boost::system::error_code ec;
	if (bfs::is_directory(path, ec))
	{
		for (bfs::recursive_directory_iterator it(path, ec), end; it != end && !ec; it.increment(ec))
			if (!bfs::is_directory(it->path(), ec)) RegenAssetGuid(it->path());
	}
	else RegenAssetGuid(path);
}

// Delete a file or folder from disk (immediate; no confirm — callers gate that).
void EditorUI::BrowserDelete(const std::string& path)
{
	boost::system::error_code ec;
	if (bfs::is_directory(path, ec)) bfs::remove_all(path, ec);
	else                             bfs::remove(path, ec);
	if (browserSel == path) browserSel.clear();
}

// Content files (worlds/prefabs/materials = JSON) that reference `guid` as text + the live world.
std::vector<std::string> EditorUI::FindDependents(const std::string& guid)
{
	std::vector<std::string> deps;
	if (guid.empty()) return deps;
	boost::system::error_code ec;
	bfs::path root(contentDir);
	if (bfs::exists(root, ec))
		for (bfs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec))
		{
			if (bfs::is_directory(it->path())) continue;
			std::string e = it->path().extension().string();
			for (char& c : e) c = (char)tolower((unsigned char)c);
			if (e != ".nuworld" && e != ".nuprefab" && e != ".numat") continue;
			if (it->path().string() == pendingDelete) continue;            // skip the file being deleted
			bfs::ifstream f(it->path()); if (!f) continue;
			std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			if (text.find(guid) != std::string::npos)
				deps.push_back(bfs::relative(it->path(), root, ec).generic_string());
		}
	if (AppInstance::GetSingleton()->currentScene->SaveToString().find(guid) != std::string::npos)
		deps.push_back("(current world)");
	return deps;
}

// Replace every reference to `guid` with its default, recursively, by the field it sits under.
static bool UnlinkInJson(nlohmann::json& n, const std::string& guid)
{
	bool changed = false;
	if (n.is_object())
		for (auto it = n.begin(); it != n.end(); ++it)
		{
			nlohmann::json& v = it.value();
			if (v.is_string() && v.get<std::string>() == guid)
			{
				const std::string& k = it.key();
				if      (k == "matGuid") { v = "builtin:default"; changed = true; }
				else if (k == "shader")  { v = "world";           changed = true; }
				else if (k == "prefab" || k == "meshGuid" || k == "diffuse" || k == "normal" || k == "specular") { v = ""; changed = true; }
			}
			else changed |= UnlinkInJson(v, guid);
		}
	else if (n.is_array())
		for (auto& e : n) changed |= UnlinkInJson(e, guid);
	return changed;
}

// Reset every reference to `guid` to defaults across the live world + all content files (irreversible).
void EditorUI::UnlinkResource(const std::string& guid)
{
	if (guid.empty()) return;
	AppInstance* app = AppInstance::GetSingleton();
	ResDB::getSingleton()->UnlinkGuid(guid);   // fix LOADED material templates first (shader/texture refs + Resolve)
	// 1) live world (may be unsaved) — re-clones instances from the now-fixed templates
	{
		nlohmann::json j = nlohmann::json::parse(app->currentScene->SaveToString(), nullptr, false);
		if (!j.is_discarded() && UnlinkInJson(j, guid))
		{
			app->selectedInHieararchy = nullptr;
			app->currentScene->LoadFromString(j.dump());
			if (!app->currentWorldPath.empty()) app->SaveWorld(app->currentWorldPath);
		}
	}
	// 2) every other world / prefab / material file on disk
	boost::system::error_code ec;
	bfs::path root(contentDir);
	std::string curFull = app->currentWorldPath.empty() ? std::string() : app->WorldFullPath(app->currentWorldPath);
	if (bfs::exists(root, ec))
		for (bfs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec))
		{
			if (bfs::is_directory(it->path())) continue;
			std::string e = it->path().extension().string();
			for (char& c : e) c = (char)tolower((unsigned char)c);
			if (e != ".nuworld" && e != ".nuprefab" && e != ".numat") continue;
			if (it->path().string() == pendingDelete) continue;
			if (!curFull.empty() && bfs::equivalent(it->path(), bfs::path(curFull), ec)) continue;   // done in memory
			bfs::ifstream f(it->path()); if (!f) continue;
			nlohmann::json j = nlohmann::json::parse(f, nullptr, false); f.close();
			if (j.is_discarded()) continue;
			if (UnlinkInJson(j, guid)) { bfs::ofstream o(it->path()); if (o) o << j.dump(2); }
		}
}

// Open the confirm modal, listing what depends on the resource.
void EditorUI::RequestDelete(const std::string& path)
{
	pendingDelete   = path;
	deleteDeps      = FindDependents(ResDB::getSingleton()->GuidForPath(path));
	openDeletePopup = true;
}

// Optionally break references, then delete the file from disk.
void EditorUI::PerformDelete(const std::string& path)
{
	ResDB* db = ResDB::getSingleton();
	std::string guid = db->GuidForPath(path);
	if (unlinkOnDelete && !guid.empty()) UnlinkResource(guid);   // reset refs in files + live world + DB templates
	if (!guid.empty()) db->RemoveByGuid(guid);                   // drop the deleted asset from the live DB (pickers)
	BrowserDelete(path);
}

// Delete-confirm modal: dependents list + irreversible warning + the persisted "Unlink?" toggle.
void EditorUI::DrawDeletePopup()
{
	if (openDeletePopup) { ImGui::OpenPopup("Delete?##browser"); openDeletePopup = false; }
	if (ImGui::BeginPopupModal("Delete?##browser", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("Delete \"%s\"?", bfs::path(pendingDelete).filename().string().c_str());
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.30f, 0.25f, 1.0f));
		ImGui::TextUnformatted(ICON_LC_TRIANGLE_ALERT " This is irreversible.");
		ImGui::PopStyleColor();
		if (!deleteDeps.empty())
		{
			ImGui::Spacing();
			ImGui::Text("Used by %d resource(s):", (int)deleteDeps.size());
			int rows = std::min((int)deleteDeps.size(), 8);
			ImGui::BeginChild("##deps", ImVec2(380, rows * ImGui::GetTextLineHeightWithSpacing() + 8), true);
			for (auto& d : deleteDeps) ImGui::BulletText("%s", d.c_str());
			ImGui::EndChild();
			if (!unlinkOnDelete)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.30f, 0.25f, 1.0f));
				ImGui::TextWrapped("These will keep dangling references unless Unlink is on.");
				ImGui::PopStyleColor();
			}
		}
		if (ImGui::Checkbox("Unlink? (reset those refs to defaults)", &unlinkOnDelete)) SaveProject();   // persisted
		ImGui::Separator();
		bool yes    = ImGui::Button("Yes") || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
		ImGui::SameLine();
		bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
		if (yes)         { PerformDelete(pendingDelete); pendingDelete.clear(); deleteDeps.clear(); ImGui::CloseCurrentPopup(); }
		else if (cancel) { pendingDelete.clear(); deleteDeps.clear(); ImGui::CloseCurrentPopup(); }
		ImGui::EndPopup();
	}
}

// Paste the clipboard into the current folder. Cut = move (and clear the clipboard); copy = duplicate.
void EditorUI::BrowserPaste()
{
	if (clipboard.empty()) return;
	bfs::path destDir(browserCwd.empty() ? contentDir : browserCwd);
	std::string lastSel;
	for (const std::string& srcStr : clipboard)
	{
		bfs::path src(srcStr);
		boost::system::error_code ec;
		if (!bfs::exists(src, ec)) continue;
		// Don't paste a folder into itself or its own subtree.
		if (bfs::is_directory(src, ec) && destDir.string().rfind(src.string(), 0) == 0) continue;
		bfs::path dst = UniquePath(destDir / src.filename());
		if (clipboardCut)
		{
			DoFileMove(srcStr, dst.string());
			RecordFileMove(srcStr, dst.string());   // undoable (cut = move)
			ec.clear();
		}
		else { CopyRecursive(src, dst, ec); if (!ec) RegenGuidsIn(dst); }   // a copy needs a fresh GUID
		if (!ec) lastSel = dst.string();
	}
	if (!lastSel.empty()) browserSel = lastSel;
	if (clipboardCut) clipboard.clear();   // a cut is one-shot
}

// Make the last-drawn item (a folder) a drop target: move a dragged file/folder into it, or save a
// dragged scene atom into it as a prefab.
void EditorUI::BrowserFolderDropTarget(const std::string& folderPath)
{
	if (!ImGui::BeginDragDropTarget()) return;
	if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NUKE_ASSET"))
	{
		std::string srcStr((const char*)p->Data);
		bfs::path src(srcStr), dstDir(folderPath);
		// Skip no-ops and moving a folder into itself / its own subtree.
		bool intoSelf = dstDir.string().rfind(src.string(), 0) == 0;
		if (src.parent_path() != dstDir && !intoSelf)
		{
			bfs::path dst = UniquePath(dstDir / src.filename());   // never clobber a same-named entry
			DoFileMove(srcStr, dst.string());
			RecordFileMove(srcStr, dst.string());                  // undoable
			if (browserSel == srcStr) browserSel = dst.string();
		}
	}
	if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NUKE_ATOM"))
		SaveAtomAsPrefab(*(Atom**)p->Data, folderPath);
	ImGui::EndDragDropTarget();
}

// Viewport / hierarchy drop: accept an asset and instantiate it.
void EditorUI::AcceptAssetDropTarget()
{
	if (!ImGui::BeginDragDropTarget()) return;
	if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NUKE_ASSET"))
		DropAsset(std::string((const char*)p->Data));
	ImGui::EndDragDropTarget();
}

Atom* EditorUI::DropAsset(const std::string& path)
{
	std::string ext = bfs::path(path).extension().string();
	if      (ext == ".nuprefab") return InstantiatePrefab(path);
	else if (ext == ".numesh")   return SpawnMeshAsset(path);
	else if (ext == ".nuworld")  { OpenWorldFromBrowser(path); return nullptr; }
	return nullptr;
}

Atom* EditorUI::SpawnMeshAsset(const std::string& path)
{
	Mesh* m = Mesh::LoadFromFile(path);
	if (!m) return nullptr;
	ResDB* db = ResDB::getSingleton();
	if (Mesh* ex = db->GetMesh(m->guid)) { delete m; m = ex; }   // reuse the already-loaded asset
	else                                  db->RegisterMesh(m);
	Atom* go = new Atom(bfs::path(path).stem().string().c_str());
	MeshRenderer* mr = new MeshRenderer();
	go->AddComponent(mr);
	mr->meshGuid = m->guid; mr->mesh = m;
	AppInstance::GetSingleton()->currentScene->Add(go);
	AppInstance::GetSingleton()->selectedInHieararchy = go;
	RecordAdd(go);
	return go;
}

void EditorUI::CreateFolderAsset(const std::string& folder)
{
	boost::system::error_code ec;
	bfs::path dir = UniquePath(bfs::path(folder) / "New Folder");
	bfs::create_directory(dir, ec);
	browserSel = dir.string();
	StartRename(dir.string());   // immediately let the user name it
}

void EditorUI::CreateWorldAsset(const std::string& folder)
{
	bfs::path path = UniquePath(bfs::path(folder) / "New World.nuworld");
	World* w = new World();           // empty world -> canonical JSON
	w->SaveToFile(path.string());
	delete w;
	browserSel = path.string();
	StartRename(path.string());
}

void EditorUI::CreateMaterialAsset(const std::string& folder)
{
	bfs::path path = UniquePath(bfs::path(folder) / "New Material.numat");
	Material* m = new Material();
	m->guid    = ResDB::NewGuid();
	m->matName = "New Material";
	m->SaveToFile(path.string());
	ResDB::getSingleton()->RegisterMaterial(m);   // show up in material pickers immediately
	ResDB::getSingleton()->SetAssetPath(m->guid, path.string());
	browserSel = path.string();
	StartRename(path.string());
}

void EditorUI::CreateShaderAsset(const std::string& folder)
{
	// A shader is a "<base>.vs.hlsl" + "<base>.ps.hlsl" pair — find a base where neither exists.
	boost::system::error_code ec;
	std::string base = "NewShader";
	for (int n = 1; bfs::exists(bfs::path(folder) / (base + ".vs.hlsl"), ec) ||
	                bfs::exists(bfs::path(folder) / (base + ".ps.hlsl"), ec); )
		base = "NewShader" + std::to_string(++n);
	bfs::path vsp = bfs::path(folder) / (base + ".vs.hlsl");
	bfs::path psp = bfs::path(folder) / (base + ".ps.hlsl");
	// Seed from the built-in "world" shader so it compiles out of the box.
	Shader* w = ResDB::getSingleton()->GetShader("world");
	{ bfs::ofstream f(vsp); if (f) f << (w ? w->vsSource : std::string()); }
	{ bfs::ofstream f(psp); if (f) f << (w ? w->psSource : std::string()); }
	if (Shader* s = Shader::LoadPair(base, vsp.string(), psp.string()))
	{
		ResDB::getSingleton()->RegisterShader(s);
		ResDB::getSingleton()->SetAssetPath(base, vsp.string());
		if (iRender* r = AppInstance::GetSingleton()->render)
			s->rendererHandle = r->createShaderPipeline(s->vsSource.c_str(), s->psSource.c_str());
	}
	browserSel = vsp.string();
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

		// Clipboard (don't steal keys while typing in the search/rename fields).
		if (!ImGui::GetIO().WantTextInput)
		{
			auto chord = [&](const char* id) { nuke::Hotkey* h = hk->Find(id); return h && h->bound && ImGui::IsKeyChordPressed((ImGuiKeyChord)h->chord); };
			if (chord("editor.browser.cut")   && !browserSel.empty()) { clipboard = { browserSel }; clipboardCut = true;  }
			if (chord("editor.browser.copy")  && !browserSel.empty()) { clipboard = { browserSel }; clipboardCut = false; }
			if (chord("editor.browser.paste")) BrowserPaste();
		}
	}

	// Delete: behaviour is per-active-window — only act when the Browser is FOCUSED. Shift+Delete deletes
	// immediately; plain Delete asks for confirmation. Chords come from the shared pool.
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && !browserSel.empty())
	{
		nuke::Hotkeys* hk = nuke::Hotkeys::Get();
		nuke::Hotkey* d  = hk->Find("editor.delete");
		nuke::Hotkey* df = hk->Find("editor.delete.force");
		if (df && df->bound && ImGui::IsKeyChordPressed((ImGuiKeyChord)df->chord)) PerformDelete(browserSel);            // Shift+Del: no confirm
		else if (d && d->bound && ImGui::IsKeyChordPressed((ImGuiKeyChord)d->chord)) RequestDelete(browserSel);          // Del: confirm
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
	if (ImGui::Button(ICON_LC_FILE_PLUS " New")) ImGui::OpenPopup("bnew");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Create a new asset/folder here");
	if (ImGui::BeginPopup("bnew"))
	{
		std::string folder = browserCwd.empty() ? contentDir : browserCwd;
		if (ImGui::MenuItem(ICON_LC_FOLDER " Folder"))     CreateFolderAsset(folder);
		ImGui::Separator();
		if (ImGui::MenuItem(ICON_LC_GLOBE " World"))       CreateWorldAsset(folder);
		if (ImGui::MenuItem(ICON_LC_PALETTE " Material"))  CreateMaterialAsset(folder);
		if (ImGui::MenuItem(ICON_LC_FILE_CODE " Shader"))  CreateShaderAsset(folder);
		ImGui::EndPopup();
	}
	if (ImGui::BeginPopup("bfilters"))
	{
		ImGui::Checkbox("Meshes", &fMesh);    ImGui::Checkbox("Materials", &fMat);
		ImGui::Checkbox("Textures", &fTex);   ImGui::Checkbox("Prefabs", &fPrefab);
		ImGui::EndPopup();
	}

	DrawRenamePopup();   // rename modal (works in all views; before the mode early-returns)
	DrawDeletePopup();   // delete-confirm modal

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
	ImGui::Text("%s", loc.c_str());
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
		if (!atRoot)   // ".." cell: click = go up, drop a file here = move it up one level
		{
			ImGui::PushID("up");
			ImGui::BeginGroup();
			if (ImGui::Button(ICON_LC_CORNER_LEFT_UP "##upcell", ImVec2(64, 64))) BrowserNavigate(cwd.parent_path().string());
			BrowserFolderDropTarget(cwd.parent_path().string());
			ImGui::TextUnformatted("..");
			ImGui::EndGroup();
			ImGui::PopID();
			if (++i % per != 0) ImGui::SameLine();
		}
		for (FEntry& e : entries)
		{
			ImGui::PushID(i);
			ImGui::BeginGroup();
			bool seld = (e.path == browserSel);
			if (seld) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.42f, 0.68f, 1.0f));
			bool clicked = ImGui::Button(e.icon, ImVec2(64, 64));
			if (seld) ImGui::PopStyleColor();
			BrowserDragSource(e.path);                       // drag this entry
			if (e.isDir) BrowserFolderDropTarget(e.path);    // drop a file onto this folder = move
			if (clicked) browserSel = e.path;
			if (clicked && e.isDir) BrowserNavigate(e.path);
			if (!e.isDir && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
			{
				if      (e.ext == ".nuprefab") InstantiatePrefab(e.path);
				else if (e.ext == ".nuworld")  OpenWorldFromBrowser(e.path);
			}
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
		if (!atRoot)   // ".." row: DOUBLE-click = go up (same as every other row), drop a file here = move it up
		{
			bool upc = ImGui::Selectable(ICON_LC_CORNER_LEFT_UP "  ..", false, ImGuiSelectableFlags_AllowDoubleClick);
			BrowserFolderDropTarget(cwd.parent_path().string());
			if (upc && ImGui::IsMouseDoubleClicked(0)) BrowserNavigate(cwd.parent_path().string());
		}
		for (FEntry& e : entries)
		{
			ImGui::PushID(i++);
			bool clicked = ImGui::Selectable((std::string(e.icon) + "  " + e.name).c_str(),
			                                 e.path == browserSel, ImGuiSelectableFlags_AllowDoubleClick);
			BrowserDragSource(e.path);                       // drag this entry
			if (e.isDir) BrowserFolderDropTarget(e.path);    // drop a file onto this folder = move
			if (clicked)
			{
				browserSel = e.path;
				if (ImGui::IsMouseDoubleClicked(0))
				{
					if (e.isDir)                   BrowserNavigate(e.path);
					else if (e.ext == ".nuprefab") InstantiatePrefab(e.path);
					else if (e.ext == ".nuworld")  OpenWorldFromBrowser(e.path);
				}
			}
			EntryContextMenu(e.path, e.isDir);   // right-click: Rename / Delete
			ImGui::PopID();
		}
	}

	// Empty area below the entries: drag a scene atom here to save it as a prefab in this folder.
	ImVec2 rest = ImGui::GetContentRegionAvail();
	if (rest.y < 24.0f) rest.y = 24.0f;
	ImGui::InvisibleButton("##browser-drop", rest);
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NUKE_ATOM"))
			SaveAtomAsPrefab(*(Atom**)p->Data, browserCwd.empty() ? contentDir : browserCwd);
		ImGui::EndDragDropTarget();
	}
	ImGui::End();
}
