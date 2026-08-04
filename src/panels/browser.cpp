// browser panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>
#include "nukeui.h"
#include "API/Model/Material.h"
#include "API/Model/Mesh.h"
#include "API/Model/Texture.h"
#include "API/Model/Package.h"
#include <nlohmann/json.hpp>
#include "interface/AssetCreators.h"
#include "API/Model/Prefab.h"
#include "API/Model/MeshRenderer.h"
#include <boost/filesystem/fstream.hpp>
#include <algorithm>
#include <iterator>
#include <set>

// Rename/move a file or folder on disk, keeping ResDB's guid<->path (and a .numat's internal
// name) in sync. Symmetric so undo can replay it in either direction.
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
		[from, to]{ DoFileMove(from, to); },
		false);   // file op: undoable, but the open world did not change
}

const char* EditorUI::ExtIcon(const std::string& ext)
{
	// The icon belongs to whoever OWNS the type: the engine registers its own formats, each
	// module registers the ones it brings, and the editor registers the plain source/text files
	// it edits (RegisterFileIcon / AssetCreator::icon). An unknown extension — a module that is
	// not loaded, someone's stray file — stays a generic page instead of guessing here.
	if (const char* g = nuke::FileIconForExt(ext))
		if (*g) return g;
	return ICON_LC_FILE;
}
// Whether this extension passes the current type filters.
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

// Recursive folder tree (Tree view), rooted at `dir`.
void EditorUI::BrowserTree(const std::string& dir)
{
	boost::system::error_code ec;
	std::set<std::string> shown;   // lowercase names at this level (disk wins over pak)
	auto lowName = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
	for (auto& de : bfs::directory_iterator(bfs::path(dir), ec))
	{
		std::string name = de.path().filename().string();
		shown.insert(lowName(name));
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
	// Mounted pak stack: this level's packed children too (read-only, content root only).
	if (browserRoot == 0 && nuke::Package::MountedCount() > 0)
	{
		std::string relDir = "content";
		bfs::path r2 = bfs::path(dir).lexically_relative(bfs::path(contentDir));
		std::string rs = r2.generic_string();
		if (!rs.empty() && rs != "." && rs.compare(0, 2, "..") != 0) relDir += "/" + rs;
		const std::string pfx = relDir + "/";
		std::set<std::string> pakDirs;
		for (const std::string& r : nuke::Package::List(pfx))
		{
			if (r.size() <= pfx.size()) continue;
			std::string tail = r.substr(pfx.size());
			size_t sl = tail.find('/');
			if (sl != std::string::npos) { pakDirs.insert(tail.substr(0, sl)); continue; }
			if (shown.count(lowName(tail))) continue;
			std::string ext = bfs::path(tail).extension().string();
			for (char& c : ext) c = (char)tolower((unsigned char)c);
			if (ExtVisible(ext) && SearchMatch(tail))
				ImGui::BulletText("%s %s (pak)", ExtIcon(ext), tail.c_str());
		}
		for (const std::string& d : pakDirs)
		{
			if (shown.count(lowName(d))) continue;
			if (ImGui::TreeNode((std::string(ICON_LC_FOLDER) + " " + d + " (pak)").c_str()))
			{
				BrowserTree((bfs::path(dir) / d).string());   // virtual: no disk dir needed
				ImGui::TreePop();
			}
		}
	}
}

// Instantiate a .nuprefab into the current world and select it; returns the new atom.
Atom* EditorUI::SpawnPrefab(const std::string& path)
{
	if (Atom* a = nuke::LoadPrefab(path))
	{
		AppInstance* app = AppInstance::GetSingleton();
		app->currentWorld->Add(a);
		app->selectedInHieararchy = a;
		RecordAdd(a);
		cout << "[editor]\tinstantiated prefab " << path << endl;
		return a;
	}
	return nullptr;
}

static std::string sFreshTemplatePath;      // file just created from a template
static std::string sFreshTemplateContent;   // its raw template (token not substituted)
// Substitute a creator template's %CLASSNAME% token with `stem` sanitized to an identifier.
static std::string InstantiateCreatorTemplate(const std::string& content, const std::string& stem)
{
	std::string cls;
	for (char c : stem) if (isalnum((unsigned char)c) || c == '_') cls += c;
	if (cls.empty() || isdigit((unsigned char)cls[0])) cls = "_" + cls;
	std::string out = content;
	const char* tok = "%CLASSNAME%";
	for (size_t pos = 0; (pos = out.find(tok, pos)) != std::string::npos; pos += cls.size())
		out.replace(pos, strlen(tok), cls);
	return out;
}

// Open the rename modal for a browser entry; only the name is editable, the extension is locked.
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

// Right-click context menu for a browser entry. Must be called right after rendering the item.
void EditorUI::EntryContextMenu(const std::string& path, bool isDir)
{
	if (ImGui::BeginPopupContextItem())
	{
		// Explorer semantics: right-clicking OUTSIDE the multi-selection re-selects that item.
		if (!browserMSel.count(path)) BrowserSelect(path);
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
		if (!isDir)
		{
			std::string cext = bfs::path(path).extension().string();
			for (char& c : cext) c = (char)std::tolower((unsigned char)c);
			if (cext == ".numat" || cext == ".numesh" || cext == ".nuprefab" || cext == ".nuinput" || cext == ".nuseq"
		    || cext == ".nuanim" || cext == ".nusm" || cext == ".nublend"
		    || cext == ".nuskel" || cext == ".nurag" || cext == ".nubonemap"
			    || cext == ".ogg" || cext == ".wav" || cext == ".mp3" || cext == ".flac")
			{
				if (ImGui::MenuItem(ICON_LC_PENCIL_RULER " Open in Editor")) OpenAssetEditor(path);
				if (cext == ".nuprefab" && ImGui::MenuItem(ICON_LC_PACKAGE_PLUS " Instantiate")) SpawnPrefab(path);
				if (cext == ".nuinput" && ImGui::MenuItem(ICON_LC_FILE_PEN " Edit as text")) OpenExternal(path, 0);
				ImGui::Separator();
			}
			else if (IsTextFile(cext))
			{
				if (ImGui::MenuItem(ICON_LC_FILE_PEN " Edit")) OpenExternal(path, 0);
				ImGui::Separator();
			}
		}
		const int nSel = (int)std::max<size_t>(browserMSel.size(), 1);
		if (ImGui::MenuItem(ICON_LC_SCISSORS " Cut",  "Ctrl+X"))  { clipboard = BrowserSelection(); clipboardCut = true;  }
		if (ImGui::MenuItem(ICON_LC_COPY " Copy",     "Ctrl+C"))  { clipboard = BrowserSelection(); clipboardCut = false; }
		if (ImGui::MenuItem(ICON_LC_CLIPBOARD_PASTE " Paste", "Ctrl+V", false, !clipboard.empty())) BrowserPaste();
		ImGui::Separator();
		if (ImGui::MenuItem(ICON_LC_PENCIL " Rename", nullptr, false, nSel <= 1)) StartRename(path);
		if (ImGui::MenuItem(nSel > 1 ? (ICON_LC_TRASH_2 " Delete " + std::to_string(nSel) + " items").c_str()
		                             : ICON_LC_TRASH_2 " Delete")) RequestDelete(BrowserSelection());
		ImGui::EndPopup();
	}
}

// Rename modal; renames within the same folder.
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
				DoFileMove(src.string(), dst.string());
				RecordFileMove(src.string(), dst.string());
				if (browserSel == src.string()) browserSel = dst.string();
				if (browserMSel.erase(src.string())) browserMSel.insert(dst.string());
				// CSharpScript binds classes by name, so a renamed .cs must have its
				// `class <oldStem>` declaration renamed too (identifier-bounded match).
				{
					std::string cext = dst.extension().string();
					for (char& c : cext) c = (char)tolower((unsigned char)c);
					if (cext == ".cs")
					{
						std::string txt;
						{
							bfs::ifstream in(dst, std::ios::binary);
							if (in) txt.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
						}
						const std::string oldStem = src.stem().string();
						std::string newStem;
						for (char c : dst.stem().string())
							if (isalnum((unsigned char)c) || c == '_') newStem += c;
						if (newStem.empty() || isdigit((unsigned char)newStem[0])) newStem = "_" + newStem;
						auto ident = [](char c) { return isalnum((unsigned char)c) || c == '_'; };
						const std::string key = "class " + oldStem;
						bool changed = false;
						for (size_t pos = 0; (pos = txt.find(key, pos)) != std::string::npos; )
						{
							const size_t after = pos + key.size();
							const bool boundBefore = pos == 0 || !ident(txt[pos - 1]);
							const bool boundAfter  = after >= txt.size() || !ident(txt[after]);
							if (boundBefore && boundAfter)
							{
								txt.replace(pos + 6, oldStem.size(), newStem);
								changed = true;
								pos += 6 + newStem.size();
							}
							else pos += key.size();
						}
						if (changed)
						{
							bfs::ofstream wf(dst, std::ios::binary | std::ios::trunc);
							if (wf)
							{
								wf << txt;
								std::cout << "[editor]\t.cs class '" << oldStem << "' -> '" << newStem << "'" << std::endl;
							}
						}
					}
				}
				if (src.string() == sFreshTemplatePath) sFreshTemplatePath = dst.string();
			}
			ImGui::CloseCurrentPopup();
		}
		if (cancel) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

// Single-select: the primary, the multi-set and the shift anchor all collapse to one path.
// Every non-click writer of the selection (create/paste/rename/locate) routes through here so
// stale multi-selections can never leak into group operations.
void EditorUI::BrowserSelect(const std::string& path)
{
	browserSel = path;
	browserMSel.clear();
	if (!path.empty()) browserMSel.insert(path);
	browserSelAnchor = path;
}

// The set group operations act on: the multi-selection, falling back to the primary (which
// external writers may have set directly).
std::vector<std::string> EditorUI::BrowserSelection() const
{
	if (!browserMSel.empty()) return std::vector<std::string>(browserMSel.begin(), browserMSel.end());
	if (!browserSel.empty()) return { browserSel };
	return {};
}

// Folder navigation history: Navigate pushes the current folder onto the back stack and clears
// forward; Back/Forward walk between them. Selections don't survive folder changes.
void EditorUI::BrowserNavigate(const std::string& path)
{
	if (path == browserCwd) return;
	browserBack.push_back(browserCwd);
	browserFwd.clear();
	browserCwd = path;
	BrowserSelect(std::string());
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

// Return `desired` if free, else append " (n)" before the extension until unused; an existing
// counter in the stem is stripped first ("Box (2).png" -> "Box (3).png").
static bfs::path UniquePath(const bfs::path& desired)
{
	boost::system::error_code ec;
	if (!bfs::exists(desired, ec)) return desired;
	bfs::path dir = desired.parent_path();
	std::string stem = StripNameCounter(desired.stem().string()), ext = desired.extension().string();
	for (int n = 2; ; ++n)
	{
		bfs::path cand = dir / (stem + " (" + std::to_string(n) + ")" + ext);
		if (!bfs::exists(cand, ec)) return cand;
	}
}

// Make the last-drawn item a drag source carrying asset path(s) (payload "NUKE_ASSET").
// Must be called immediately after the item. When the dragged item is part of the
// multi-selection, the payload packs EVERY selected path NUL-separated (+ trailing NUL) —
// single-target consumers read the first path (they stop at the first NUL), multi-aware
// targets walk the whole buffer.
void EditorUI::BrowserDragSource(const std::string& path)
{
	if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
	{
		if (browserMSel.size() > 1 && browserMSel.count(path))
		{
			std::string buf;
			buf += path; buf += '\0';                    // dragged item first = the primary target
			for (const std::string& p : browserMSel)
				if (p != path) { buf += p; buf += '\0'; }
			ImGui::SetDragDropPayload("NUKE_ASSET", buf.data(), buf.size() + 1);
			ImGui::Text("%d items", (int)browserMSel.size());
		}
		else
		{
			ImGui::SetDragDropPayload("NUKE_ASSET", path.c_str(), path.size() + 1);
			ImGui::TextUnformatted(bfs::path(path).filename().string().c_str());
		}
		ImGui::EndDragDropSource();
	}
}

// Every path packed into a "NUKE_ASSET" payload (single or NUL-separated multi).
static std::vector<std::string> PayloadPaths(const ImGuiPayload* p)
{
	std::vector<std::string> out;
	const char* s = (const char*)p->Data;
	const char* end = s + p->DataSize;
	while (s < end && *s)
	{
		out.push_back(std::string(s));
		s += out.back().size() + 1;
	}
	return out;
}

// Save an atom (with its children) as a .nuprefab in `folder`. Snapshot only — no live link.
void EditorUI::SaveAtomAsPrefab(Atom* a, const std::string& folder)
{
	if (!a) return;
	bfs::path path = UniquePath(bfs::path(folder) / (a->GetName() + ".nuprefab"));
	a->prefabGuid = ResDB::NewGuid();   // source atom becomes an instance of this new prefab
	if (nuke::SavePrefab(a, path.string()))
	{
		ResDB::getSingleton()->SetAssetPath(a->prefabGuid, path.string());
		BrowserSelect(path.string());
		cout << "[editor]\tsaved prefab " << path.string() << " (" << a->prefabGuid << ")" << endl;
	}
}

// Recursively copy a file or folder tree; dst must not already exist.
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

// Give a copied asset a fresh GUID on disk and register it live; two files sharing one GUID
// collide in ResDB. Worlds/prefabs have no ResDB-keyed GUID and are left as-is.
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

// Delete a file or folder from disk immediately; callers gate the confirmation.
void EditorUI::BrowserDelete(const std::string& path)
{
	boost::system::error_code ec;
	if (bfs::is_directory(path, ec)) bfs::remove_all(path, ec);
	else                             bfs::remove(path, ec);
	if (browserSel == path) browserSel.clear();
	browserMSel.erase(path);
	if (browserSelAnchor == path) browserSelAnchor.clear();
}

// Content files (worlds/prefabs/materials) referencing ANY of the guids, plus the live world.
// ONE pass over the content tree regardless of how many guids are being checked — per-file
// scans froze the editor on multi-delete.
std::vector<std::string> EditorUI::FindDependents(const std::vector<std::string>& guids)
{
	std::vector<std::string> deps;
	std::vector<std::string> gs;
	for (const std::string& g : guids)
		if (!g.empty()) gs.push_back(g);
	if (gs.empty()) return deps;
	boost::system::error_code ec;
	bfs::path root(contentDir);
	if (bfs::exists(root, ec))
		for (bfs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec))
		{
			if (bfs::is_directory(it->path())) continue;
			std::string e = it->path().extension().string();
			for (char& c : e) c = (char)tolower((unsigned char)c);
			if (e != ".nuworld" && e != ".nuprefab" && e != ".numat") continue;
			if (std::find(pendingDeletes.begin(), pendingDeletes.end(), it->path().string()) != pendingDeletes.end())
				continue;   // files on the chopping block don't count as dependents
			bfs::ifstream f(it->path()); if (!f) continue;
			std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			for (const std::string& g : gs)
				if (text.find(g) != std::string::npos)
				{
					deps.push_back(bfs::relative(it->path(), root, ec).generic_string());
					break;
				}
		}
	const std::string live = AppInstance::GetSingleton()->currentWorld->SaveToString();
	for (const std::string& g : gs)
		if (live.find(g) != std::string::npos) { deps.push_back("(current world)"); break; }
	return deps;
}

// Recursively replace every reference to `guid` with the default for the field it sits under.
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

// Reset every reference to the guids to defaults across the live world and all content files.
// Irreversible. ONE pass over the content tree for the whole batch.
void EditorUI::UnlinkResources(const std::vector<std::string>& guids)
{
	std::vector<std::string> gs;
	for (const std::string& g : guids)
		if (!g.empty()) gs.push_back(g);
	if (gs.empty()) return;
	AppInstance* app = AppInstance::GetSingleton();
	for (const std::string& g : gs)
		ResDB::getSingleton()->UnlinkGuid(g);   // loaded templates first: the world re-clones from them
	{
		nlohmann::json j = nlohmann::json::parse(app->currentWorld->SaveToString(), nullptr, false);
		bool changed = false;
		if (!j.is_discarded())
			for (const std::string& g : gs) changed |= UnlinkInJson(j, g);
		if (changed)
		{
			app->selectedInHieararchy = nullptr;
			app->currentWorld->LoadFromString(j.dump());
			if (!app->currentWorldPath.empty()) app->SaveWorld(app->currentWorldPath);
		}
	}
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
			if (std::find(pendingDeletes.begin(), pendingDeletes.end(), it->path().string()) != pendingDeletes.end())
				continue;
			if (!curFull.empty() && bfs::equivalent(it->path(), bfs::path(curFull), ec)) continue;   // done in memory
			bfs::ifstream f(it->path()); if (!f) continue;
			nlohmann::json j = nlohmann::json::parse(f, nullptr, false); f.close();
			if (j.is_discarded()) continue;
			bool changed = false;
			for (const std::string& g : gs) changed |= UnlinkInJson(j, g);
			if (changed) { bfs::ofstream o(it->path()); if (o) o << j.dump(2); }
		}
}

void EditorUI::UnlinkResource(const std::string& guid) { UnlinkResources({ guid }); }

// Collect dependents (one scan for the whole batch) and open the delete-confirm modal.
void EditorUI::RequestDelete(const std::vector<std::string>& paths)
{
	if (paths.empty()) return;
	pendingDeletes = paths;
	std::vector<std::string> guids;
	for (const std::string& p : paths)
		guids.push_back(ResDB::getSingleton()->GuidForPath(p));
	deleteDeps = FindDependents(guids);
	openDeletePopup = true;
}

// Optionally break references (one batch pass), then delete the files from disk.
void EditorUI::PerformDeletes(const std::vector<std::string>& paths)
{
	if (paths.empty()) return;
	pendingDeletes = paths;   // the unlink scan skips the doomed files
	ResDB* db = ResDB::getSingleton();
	std::vector<std::string> guids;
	for (const std::string& p : paths)
	{
		const std::string g = db->GuidForPath(p);
		if (!g.empty()) guids.push_back(g);
	}
	if (unlinkOnDelete) UnlinkResources(guids);
	for (const std::string& g : guids) db->RemoveByGuid(g);   // drop from the live DB so pickers forget it
	for (const std::string& p : paths) BrowserDelete(p);
	pendingDeletes.clear();
}

void EditorUI::PerformDelete(const std::string& path) { PerformDeletes({ path }); }

// Delete-confirm modal: dependents list, warning, and the persisted "Unlink?" toggle.
void EditorUI::DrawDeletePopup()
{
	if (openDeletePopup) { ImGui::OpenPopup("Delete?##browser"); openDeletePopup = false; }
	if (ImGui::BeginPopupModal("Delete?##browser", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		if (pendingDeletes.size() <= 1)
			ImGui::Text("Delete \"%s\"?", pendingDeletes.empty() ? "" : bfs::path(pendingDeletes[0]).filename().string().c_str());
		else
		{
			ImGui::Text("Delete %d items?", (int)pendingDeletes.size());
			const int rows = std::min((int)pendingDeletes.size(), 8);
			ImGui::BeginChild("##delitems", ImVec2(380, rows * ImGui::GetTextLineHeightWithSpacing() + 8), true);
			for (const std::string& p : pendingDeletes)
				ImGui::BulletText("%s", bfs::path(p).filename().string().c_str());
			ImGui::EndChild();
		}
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
		if (yes)
		{
			// copy: BrowserDelete mutates selection state; pendingDeletes gates the scans
			const std::vector<std::string> doomed = pendingDeletes;
			PerformDeletes(doomed);
			pendingDeletes.clear(); deleteDeps.clear();
			ImGui::CloseCurrentPopup();
		}
		else if (cancel) { pendingDeletes.clear(); deleteDeps.clear(); ImGui::CloseCurrentPopup(); }
		ImGui::EndPopup();
	}
}

// Paste the clipboard into the current folder: cut = move (one-shot), copy = duplicate.
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
			RecordFileMove(srcStr, dst.string());
			ec.clear();
		}
		else { CopyRecursive(src, dst, ec); if (!ec) RegenGuidsIn(dst); }   // a copy needs a fresh GUID
		if (!ec) lastSel = dst.string();
	}
	if (!lastSel.empty()) BrowserSelect(lastSel);
	if (clipboardCut) clipboard.clear();
}

// Make the last-drawn folder item a drop target: move a dragged file/folder into it, or save a
// dragged atom into it as a prefab.
void EditorUI::BrowserFolderDropTarget(const std::string& folderPath)
{
	if (!ImGui::BeginDragDropTarget()) return;
	if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NUKE_ASSET"))
	{
		for (const std::string& srcStr : PayloadPaths(p))   // single or multi-selection drag
		{
			bfs::path src(srcStr), dstDir(folderPath);
			bool intoSelf = dstDir.string().rfind(src.string(), 0) == 0;   // folder into its own subtree
			if (src.parent_path() == dstDir || intoSelf) continue;
			bfs::path dst = UniquePath(dstDir / src.filename());
			DoFileMove(srcStr, dst.string());
			RecordFileMove(srcStr, dst.string());
			if (browserSel == srcStr) browserSel = dst.string();
			if (browserMSel.erase(srcStr)) browserMSel.insert(dst.string());
		}
	}
	if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NUKE_ATOM"))
		SaveAtomAsPrefab(*(Atom**)p->Data, folderPath);
	ImGui::EndDragDropTarget();
}

// Viewport/hierarchy drop target: accept an asset and instantiate it.
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
	if      (ext == ".nuprefab") return SpawnPrefab(path);
	else if (ext == ".numesh")   return SpawnMeshAsset(path);
	else if (ext == ".nuworld")  { OpenWorldFromBrowser(path); return nullptr; }
	return nullptr;
}

void EditorUI::DropAssetOnAtom(Atom* a, const std::string& path)
{
	if (!a) return;
	nuke::MeshRenderer* mr = a->GetComponent<nuke::MeshRenderer>();
	if (!mr) return;   // only mesh objects carry a material
	std::string ext = bfs::path(path).extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
	if (ext != ".numat" && ext != ".nutex") return;

	ResDB* db = ResDB::getSingleton();
	std::string before = nuke::SaveAtomToString(a);
	bool changed = false;

	if (ext == ".numat")
	{
		std::string guid = db->GuidForPath(path);
		if (guid.empty())   // not indexed yet: load + register
			if (Material* m = Material::LoadFromFile(path)) { db->RegisterMaterial(m); db->SetAssetPath(m->guid, path); guid = m->guid; }
		if (!guid.empty())
		{
			Material* asset = db->GetMaterial(guid);
			mr->matGuid = guid;
			if (mr->mat) delete mr->mat;
			mr->mat = asset ? asset->Clone() : nullptr;   // own an instance so edits stay local
			changed = true;
		}
	}
	else   // .nutex
	{
		std::string guid = db->GuidForPath(path);
		if (guid.empty())
			if (Texture* t = Texture::LoadFromFile(path)) { db->RegisterTexture(t); db->SetAssetPath(t->guid, path); guid = t->guid; }
		if (!guid.empty())
		{
			if (!mr->mat)   // ensure an instance exists to override
			{
				Material* asset = db->GetMaterial(mr->matGuid);
				mr->mat = asset ? asset->Clone() : new Material();
			}
			Texture* tx = db->GetTexture(guid);
			if (tx && tx->renderTexture)
			{
				// Camera feed goes to emissive; the map is tinted by color x intensity, so
				// those must be white x 1 or it renders black.
				mr->mat->emissiveGuid = guid;
				mr->mat->emissive = Color(1, 1, 1, 1);
				mr->mat->emissiveIntensity = 1.0f;
			}
			else
				mr->mat->diffuseGuid = guid;
			mr->mat->Resolve();
			changed = true;
		}
	}

	if (!changed) return;
	AppInstance::GetSingleton()->selectedInHieararchy = a;
	std::string after = nuke::SaveAtomToString(a);
	World* w = AppInstance::GetSingleton()->currentWorld;
	long id = a->id.id, parent = a->parent ? a->parent->id.id : 0;
	int index = 0; { auto& lst = a->parent ? a->parent->children : w->GetHierarchy(); int i = 0; for (Atom* s : lst) { if (s == a) { index = i; break; } ++i; } }
	PushUndo(ext == ".numat" ? "Assign material" : "Assign texture",
		[this, id, parent, index, before]{ ApplyAtomState(id, parent, index, before); },
		[this, id, parent, index, after ]{ ApplyAtomState(id, parent, index, after ); });
}

Atom* EditorUI::SpawnMeshAsset(const std::string& path)
{
	Mesh* m = Mesh::LoadFromFile(path);
	if (!m) return nullptr;
	ResDB* db = ResDB::getSingleton();
	if (Mesh* ex = db->GetMesh(m->guid)) { delete m; m = ex; }   // reuse the loaded asset
	else                                  db->RegisterMesh(m);
	Atom* atom = new Atom(bfs::path(path).stem().string().c_str());
	MeshRenderer* mr = new MeshRenderer();
	atom->AddComponent(mr);
	mr->meshGuid = m->guid; mr->mesh = m;
	AppInstance::GetSingleton()->currentWorld->Add(atom);
	AppInstance::GetSingleton()->selectedInHieararchy = atom;
	RecordAdd(atom);
	return atom;
}

void EditorUI::CreateFolderAsset(const std::string& folder)
{
	boost::system::error_code ec;
	bfs::path dir = UniquePath(bfs::path(folder) / "New Folder");
	bfs::create_directory(dir, ec);
	BrowserSelect(dir.string());
	StartRename(dir.string());
}

void EditorUI::CreateWorldAsset(const std::string& folder)
{
	bfs::path path = UniquePath(bfs::path(folder) / "New World.nuworld");
	World* w = new World();
	w->SaveToFile(path.string());
	delete w;
	BrowserSelect(path.string());
	StartRename(path.string());
}

void EditorUI::CreateBoneMapAsset(const std::string& folder)
{
	bfs::path path = UniquePath(bfs::path(folder) / "New BoneMap.nubonemap");
	nuke::BoneMap* b = new nuke::BoneMap();
	b->guid = ResDB::NewGuid();
	b->name = path.stem().string();
	b->map["sourceBoneName"] = "targetBoneName";
	b->SaveToFile(path.string());
	ResDB::getSingleton()->RegisterBoneMap(b);
	ResDB::getSingleton()->SetAssetPath(b->guid, path.string());
	BrowserSelect(path.string());
	StartRename(path.string());
}

void EditorUI::CreateAnimSMAsset(const std::string& folder)
{
	bfs::path path = UniquePath(bfs::path(folder) / "New Controller.nusm");
	nuke::AnimSM* m = new nuke::AnimSM();
	m->guid = ResDB::NewGuid();
	m->name = path.stem().string();
	nuke::AnimSM::Layer base;
	base.name = "Base";
	m->layers.push_back(base);
	m->SaveToFile(path.string());
	ResDB::getSingleton()->RegisterAnimSM(m);
	ResDB::getSingleton()->SetAssetPath(m->guid, path.string());
	BrowserSelect(path.string());
	StartRename(path.string());
}

void EditorUI::CreateSequenceAsset(const std::string& folder)
{
	bfs::path path = UniquePath(bfs::path(folder) / "New Sequence.nuseq");
	nuke::Sequence* q = new nuke::Sequence();
	q->guid = ResDB::NewGuid();
	q->name = path.stem().string();
	q->SaveToFile(path.string());
	ResDB::getSingleton()->RegisterSequence(q);
	ResDB::getSingleton()->SetAssetPath(q->guid, path.string());
	BrowserSelect(path.string());
	StartRename(path.string());
}

void EditorUI::CreateBlendSpaceAsset(const std::string& folder)
{
	bfs::path path = UniquePath(bfs::path(folder) / "New BlendSpace.nublend");
	nuke::BlendSpace* b = new nuke::BlendSpace();
	b->guid = ResDB::NewGuid();
	b->name = path.stem().string();
	b->dims = 1;
	b->paramX = "Speed";
	b->SaveToFile(path.string());
	ResDB::getSingleton()->RegisterBlendSpace(b);
	ResDB::getSingleton()->SetAssetPath(b->guid, path.string());
	BrowserSelect(path.string());
	StartRename(path.string());
}

void EditorUI::CreateMaterialAsset(const std::string& folder)
{
	bfs::path path = UniquePath(bfs::path(folder) / "New Material.numat");
	Material* m = new Material();
	m->guid    = ResDB::NewGuid();
	m->matName = "New Material";
	m->SaveToFile(path.string());
	ResDB::getSingleton()->RegisterMaterial(m);
	ResDB::getSingleton()->SetAssetPath(m->guid, path.string());
	BrowserSelect(path.string());
	StartRename(path.string());
}

void EditorUI::CreateRenderTextureAsset(const std::string& folder)
{
	bfs::path path = UniquePath(bfs::path(folder) / "New RenderTexture.nutex");
	Texture* t = new Texture();
	t->guid = ResDB::NewGuid();
	t->renderTexture = true;
	t->width = 512; t->height = 512;
	t->SaveToFile(path.string());
	ResDB* db = ResDB::getSingleton();
	db->RegisterTexture(t);
	db->SetAssetPath(t->guid, path.string());
	if (AppInstance::GetSingleton()->render)   // allocate the GPU render target now
		t->rtId = AppInstance::GetSingleton()->render->createRenderTarget(t->width, t->height);
	BrowserSelect(path.string());
	StartRename(path.string());
}

void EditorUI::CreateShaderAsset(const std::string& folder)
{
	// A shader is a "<base>.vs.hlsl" + "<base>.ps.hlsl" pair; find a base where neither exists.
	boost::system::error_code ec;
	std::string base = "NewShader";
	for (int n = 1; bfs::exists(bfs::path(folder) / (base + ".vs.hlsl"), ec) ||
	                bfs::exists(bfs::path(folder) / (base + ".ps.hlsl"), ec); )
		base = "NewShader" + std::to_string(++n);
	bfs::path vsp = bfs::path(folder) / (base + ".vs.hlsl");
	bfs::path psp = bfs::path(folder) / (base + ".ps.hlsl");
	// Seed from the built-in "world" shader so the new pair compiles as-is.
	Shader* w = ResDB::getSingleton()->GetShader("world");
	{ bfs::ofstream f(vsp); if (f) f << (w ? w->vsSource : std::string()); }
	{ bfs::ofstream f(psp); if (f) f << (w ? w->psSource : std::string()); }
	if (Shader* s = Shader::LoadPair(base, vsp.string(), psp.string()))
	{
		ResDB::getSingleton()->RegisterShader(s);
		ResDB::getSingleton()->SetAssetPath(base, vsp.string());
		if (iRender* r = AppInstance::GetSingleton()->render)
			s->rendererHandle = r->createShaderPipeline(s->name.c_str(), s->vsSource.c_str(), s->psSource.c_str());
	}
	BrowserSelect(vsp.string());
}

void EditorUI::winBrowser()
{
	if (!win->browser) return;
	NukeUI::DocPanel("panel:browser", "Browser", &win->browser, window_flags, 920, 380, [this]()
	{
	// Locked while booting: the content scan is still writing ResDB on a worker.
	if (bootLoading) { ImGui::TextDisabled("Loading project..."); return; }

	// Hotkeys are dispatched here rather than globally so they only act over the browser.
	if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
	{
		nuke::Hotkeys* hk = nuke::Hotkeys::Get();
		nuke::Hotkey* b = hk->Find("editor.browser.back");
		nuke::Hotkey* f = hk->Find("editor.browser.forward");
		if (b && b->bound && ImGui::IsKeyChordPressed((ImGuiKeyChord)b->chord)) BrowserBack();
		if (f && f->bound && ImGui::IsKeyChordPressed((ImGuiKeyChord)f->chord)) BrowserForward();

		if (!ImGui::GetIO().WantTextInput)   // don't steal keys while typing in a field
		{
			auto chord = [&](const char* id) { nuke::Hotkey* h = hk->Find(id); return h && h->bound && ImGui::IsKeyChordPressed((ImGuiKeyChord)h->chord); };
			if (chord("editor.cut")   && !BrowserSelection().empty()) { clipboard = BrowserSelection(); clipboardCut = true;  }
			if (chord("editor.copy")  && !BrowserSelection().empty()) { clipboard = BrowserSelection(); clipboardCut = false; }
			if (chord("editor.paste")) BrowserPaste();
		}
	}

	// Delete is per-active-window: only act when the browser is focused.
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && !BrowserSelection().empty())
	{
		nuke::Hotkeys* hk = nuke::Hotkeys::Get();
		nuke::Hotkey* d  = hk->Find("editor.delete");
		nuke::Hotkey* df = hk->Find("editor.delete.force");
		if (df && df->bound && ImGui::IsKeyChordPressed((ImGuiKeyChord)df->chord))
			PerformDeletes(BrowserSelection());   // no confirm
		else if (d && d->bound && ImGui::IsKeyChordPressed((ImGuiKeyChord)d->chord)) RequestDelete(BrowserSelection());
	}

	// Browse root: content, or the project's C++ sources when <project>/source exists.
	const bfs::path srcRootP = bfs::path(projectDir) / "source";
	boost::system::error_code srcEc;
	const bool hasSrc = bfs::exists(srcRootP, srcEc) && bfs::is_directory(srcRootP, srcEc);
	if (!hasSrc) browserRoot = 0;

	// Toolbar: view mode | search | filters.
	const char* modes[] = { ICON_LC_LAYOUT_GRID " Tiles", ICON_LC_LIST " List", ICON_LC_FOLDER_TREE " Tree" };
	if (browserView < 0 || browserView > 2) browserView = 0;
	ImGui::SetNextItemWidth(130);
	ImGui::Combo("##bview", &browserView, modes, 3);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(180);
	ImGui::InputTextWithHint("##bsearch", ICON_LC_SEARCH " Search", browserSearch, sizeof(browserSearch));
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_FILTER " Filters")) ImGui::OpenPopup("bfilters");
	if (browserRoot == 0)   // Import/New create assets: meaningless in the C++ source root
	{
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_DOWNLOAD " Import"))
	{
		std::string src = EditorPickModelFile();   // models or images
		if (!src.empty())
		{
			std::string dest = browserCwd.empty() ? contentDir : browserCwd;
			AssImporter::getSingleton()->ImportAnyAsync(src, dest, [src, dest](bool ok) {
				cout << "[editor]\timport " << (ok ? "ok" : "FAILED") << ": " << src << " -> " << dest << endl;
			});
		}
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Import a model (OBJ/FBX/glTF) or image (PNG/JPG/TGA/...) into this folder");
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_FILE_PLUS " New")) ImGui::OpenPopup("bnew");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Create a new asset/folder here");
	}
	if (ImGui::BeginPopup("bnew"))
	{
		std::string folder = browserCwd.empty() ? contentDir : browserCwd;
		if (ImGui::MenuItem(ICON_LC_FOLDER " Folder"))     CreateFolderAsset(folder);
		ImGui::Separator();
		if (ImGui::MenuItem(ICON_LC_GLOBE " World"))       CreateWorldAsset(folder);
		if (ImGui::MenuItem(ICON_LC_PALETTE " Material"))  CreateMaterialAsset(folder);
		if (ImGui::MenuItem(ICON_LC_BONE " Bone Map"))     CreateBoneMapAsset(folder);
		if (ImGui::MenuItem(ICON_LC_WORKFLOW " Anim Controller")) CreateAnimSMAsset(folder);
		if (ImGui::MenuItem(ICON_LC_BLEND " Blend Space")) CreateBlendSpaceAsset(folder);
		if (ImGui::MenuItem(ICON_LC_FILM " Sequence"))     CreateSequenceAsset(folder);
		if (ImGui::MenuItem(ICON_LC_FILE_CODE " Shader"))  CreateShaderAsset(folder);
		if (ImGui::MenuItem(ICON_LC_IMAGE " RenderTexture")) CreateRenderTextureAsset(folder);
		// Plugin-registered file types, grouped by category ("" = flat entry).
		const std::vector<nuke::AssetCreator>& creators = nuke::AssetCreators();
		if (!creators.empty()) ImGui::Separator();
		auto creatorItem = [&](const nuke::AssetCreator& ac)
		{
			const char* icon = ac.icon.empty() ? ICON_LC_FILE_CODE : ac.icon.c_str();
			if (ImGui::MenuItem((std::string(icon) + " " + ac.label).c_str()))
			{
				bfs::path p = UniquePath(bfs::path(folder) / (ac.baseName + ac.ext));
				// Binary: text mode would CRLF-mangle the template and break the
				// "still the untouched template" compare on rename.
				bfs::ofstream wf(p, std::ios::binary);
				if (wf) wf << InstantiateCreatorTemplate(ac.content, p.stem().string());
				sFreshTemplatePath    = p.string();
				sFreshTemplateContent = ac.content;
				BrowserSelect(p.string());
				StartRename(p.string());
			}
		};
		std::vector<std::string> doneCategories;
		for (const nuke::AssetCreator& ac : creators)
		{
			if (ac.category.empty()) { creatorItem(ac); continue; }
			if (std::find(doneCategories.begin(), doneCategories.end(), ac.category) != doneCategories.end())
				continue;   // whole category rendered on its first appearance
			doneCategories.push_back(ac.category);
			if (ImGui::BeginMenu(ac.category.c_str()))
			{
				for (const nuke::AssetCreator& in : creators)
					if (in.category == ac.category) creatorItem(in);
				ImGui::EndMenu();
			}
		}
		ImGui::EndPopup();
	}
	if (ImGui::BeginPopup("bfilters"))
	{
		ImGui::Checkbox("Meshes", &fMesh);    ImGui::Checkbox("Materials", &fMat);
		ImGui::Checkbox("Textures", &fTex);   ImGui::Checkbox("Prefabs", &fPrefab);
		ImGui::EndPopup();
	}

	DrawRenamePopup();   // before the per-view early-returns, so modals work in every view
	DrawDeletePopup();

	bfs::path root = (browserRoot == 1) ? srcRootP : bfs::path(contentDir);
	bfs::path cwd  = browserCwd.empty() ? root : bfs::path(browserCwd);
	{
		// A cwd outside the current root (root switch, deleted folder, stale state) snaps back.
		boost::system::error_code cec;
		bfs::path relc = bfs::relative(cwd, root, cec);
		const std::string rs = relc.generic_string();
		if (cec || rs.compare(0, 2, "..") == 0 || !bfs::exists(cwd, cec))
		{
			cwd = root;
			browserCwd = root.string();
		}
	}

	// Path bar: Back/Forward/Up + root selector + current location.
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
	if (hasSrc)   // root selector is a dropdown only when the project has sources
	{
		const char* roots[] = { "content", "source" };
		ImGui::SetNextItemWidth(96);
		int r = browserRoot;
		if (ImGui::Combo("##broot", &r, roots, 2) && r != browserRoot)
		{
			browserRoot = r;
			root = (r == 1) ? srcRootP : bfs::path(contentDir);
			cwd  = root;
			browserCwd = root.string();
			browserBack.clear(); browserFwd.clear(); BrowserSelect(std::string());
			atRoot = true;
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Browse the project content or its C++ sources");
		ImGui::SameLine();
	}
	bfs::path rel = bfs::relative(cwd, root, rc);
	std::string loc = hasSrc ? "" : "content";   // the combo already names the root
	if (!rc && !rel.empty() && rel.generic_string() != ".") loc += "/" + rel.generic_string();
	ImGui::Text("%s", loc.c_str());
	ImGui::Separator();

	if (browserView == 2)   // Tree
	{
		ImGui::BeginChild("##browserfiles");   // only the tree scrolls
		BrowserTree(root.string());
		ImGui::EndChild();
		ImGui::End();
		return;
	}

	// Gather the current folder's entries (Tiles / List).
	struct FEntry { std::string name, path, ext; bool isDir; const char* icon; bool pak = false; };
	std::vector<FEntry> entries;
	boost::system::error_code ec;
	const bool searching = (browserSearch[0] != 0);
	if (searching)
	{
		// A search spans the whole subtree under the current folder (files only).
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
	// Union the mounted pak content with the disk overlay: disk wins name collisions, pak-only
	// entries are read-only "pak://<rel>" paths. Content root only.
	if (browserRoot == 0 && Package::MountedCount() > 0)
	{
		std::string relDir = "content";
		{
			bfs::path r2 = cwd.lexically_relative(root);   // lexical: pak-only folders have no disk dir
			std::string rs = r2.generic_string();
			if (!rs.empty() && rs != "." && rs.compare(0, 2, "..") != 0) relDir += "/" + rs;
		}
		auto lowName = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
		std::set<std::string> have;
		for (const FEntry& e : entries) have.insert(lowName(e.name));
		const std::string pfx = relDir + "/";
		for (const std::string& r : Package::List(pfx))
		{
			if (r.size() <= pfx.size()) continue;
			std::string tail = r.substr(pfx.size());
			if (searching)
			{
				std::string name = bfs::path(tail).filename().string();
				std::string ext  = bfs::path(name).extension().string();
				for (char& c : ext) c = (char)tolower((unsigned char)c);
				if (!ExtVisible(ext) || !SearchMatch(name) || have.count(lowName(name))) continue;
				have.insert(lowName(name));
				entries.push_back({ name, "pak://" + r, ext, false, ExtIcon(ext), true });
				continue;
			}
			size_t sl = tail.find('/');
			if (sl != std::string::npos)   // a subfolder at this level
			{
				std::string name = tail.substr(0, sl);
				if (have.count(lowName(name))) continue;
				have.insert(lowName(name));
				entries.push_back({ name, "pak://" + pfx + name, "", true, ICON_LC_FOLDER, true });
			}
			else
			{
				std::string ext = bfs::path(tail).extension().string();
				for (char& c : ext) c = (char)tolower((unsigned char)c);
				if (!ExtVisible(ext) || have.count(lowName(tail))) continue;
				have.insert(lowName(tail));
				entries.push_back({ tail, "pak://" + r, ext, false, ExtIcon(ext), true });
			}
		}
	}
	std::sort(entries.begin(), entries.end(), [](const FEntry& a, const FEntry& b) {
		if (a.isDir != b.isDir) return a.isDir > b.isDir;
		return a.name < b.name;
	});

	// The open world gets a "*" when it has unsaved changes.
	std::string dirtyWorld;
	if (worldDirty && !AppInstance::GetSingleton()->currentWorldPath.empty())
	{
		boost::system::error_code dec;
		dirtyWorld = bfs::weakly_canonical(bfs::path(AppInstance::GetSingleton()->WorldFullPath(AppInstance::GetSingleton()->currentWorldPath)), dec).generic_string();
	}
	auto isDirty = [&](const FEntry& e) {
		if (dirtyWorld.empty() || e.ext != ".nuworld") return false;
		boost::system::error_code dec;
		return bfs::weakly_canonical(bfs::path(e.path), dec).generic_string() == dirtyWorld;
	};

	// Shared click-select over the sorted entries: Ctrl toggles, Shift range-selects from the
	// anchor (in display order), plain click single-selects. Pak entries never multi-select.
	auto entryClick = [&](const FEntry& e)
	{
		AppInstance::GetSingleton()->selectedInHieararchy = nullptr;
		const bool ctrl = ImGui::GetIO().KeyCtrl, shift = ImGui::GetIO().KeyShift;
		if (e.pak || (!ctrl && !shift)) { BrowserSelect(e.path); return; }
		if (ctrl && !shift)
		{
			if (browserMSel.count(e.path))
			{
				browserMSel.erase(e.path);
				if (browserSel == e.path)
					browserSel = browserMSel.empty() ? std::string() : *browserMSel.begin();
			}
			else
			{
				browserMSel.insert(e.path);
				browserSel = browserSelAnchor = e.path;
			}
			return;
		}
		// shift: range between the anchor and the clicked entry in the current sort order
		int ia = -1, ib = -1;
		for (int k = 0; k < (int)entries.size(); ++k)
		{
			if (entries[k].path == browserSelAnchor) ia = k;
			if (entries[k].path == e.path)           ib = k;
		}
		if (ia < 0) { BrowserSelect(e.path); return; }
		if (!ctrl) browserMSel.clear();
		for (int k = std::min(ia, ib); k <= std::max(ia, ib); ++k)
			if (!entries[k].pak) browserMSel.insert(entries[k].path);
		browserSel = e.path;   // anchor stays put for follow-up ranges
	};

	ImGui::BeginChild("##browserfiles");   // pins the toolbar + path bar; only the list scrolls

	if (browserView == 0)            // Tiles
	{
		const float tile = 64.0f, cell = 84.0f;
		float availW = ImGui::GetContentRegionAvail().x;
		int per = (int)(availW / cell); if (per < 1) per = 1;
		// Truncate a label to the tile width.
		auto fit = [tile](const std::string& s) {
			if (ImGui::CalcTextSize(s.c_str()).x <= tile) return s;
			std::string o = s;
			while (!o.empty() && ImGui::CalcTextSize((o + "..").c_str()).x > tile) o.pop_back();
			return o + "..";
		};
		int i = 0;
		if (!atRoot)   // ".." cell
		{
			ImGui::PushID("up");
			ImGui::BeginGroup();
			ImGui::Button(ICON_LC_CORNER_LEFT_UP "##upcell", ImVec2(tile, tile));
			BrowserFolderDropTarget(cwd.parent_path().string());
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) BrowserNavigate(cwd.parent_path().string());
			ImGui::TextUnformatted("..");
			ImGui::EndGroup();
			ImGui::PopID();
			if (++i % per != 0) ImGui::SameLine();
		}
		for (FEntry& e : entries)
		{
			ImGui::PushID(i);
			ImGui::BeginGroup();
			bool seld = (e.path == browserSel) || browserMSel.count(e.path);
			if (seld) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.42f, 0.68f, 1.0f));
			bool clicked = ImGui::Button(e.icon, ImVec2(tile, tile));
			if (seld) ImGui::PopStyleColor();
			if (!e.pak) BrowserDragSource(e.path);
			if (e.isDir && !e.pak) BrowserFolderDropTarget(e.path);
			if (clicked) entryClick(e);
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
			{
				if      (e.isDir)              BrowserNavigate(e.pak ? (cwd / e.name).string() : e.path);
				else if (e.ext == ".nuworld")
				{
					// Pak-only worlds must open through the layered stack so mods merge in.
					if (e.pak) OpenWorldCmd(e.path.substr(strlen("pak://content/")));
					else       OpenWorldFromBrowser(e.path);
				}
				else if (!e.pak && (e.ext == ".nuprefab" || e.ext == ".numat" || e.ext == ".numesh" || e.ext == ".nuinput" || e.ext == ".nuseq"
			         || e.ext == ".nuanim" || e.ext == ".nusm" || e.ext == ".nublend"
			         || e.ext == ".nuskel" || e.ext == ".nurag" || e.ext == ".nubonemap"
				         || e.ext == ".ogg" || e.ext == ".wav" || e.ext == ".mp3" || e.ext == ".flac"
				         || nuke::AssetEditorForExt(e.ext)))   // module-supplied editors
					OpenAssetEditor(e.path);
				else if (!e.pak && IsTextFile(e.ext)) OpenExternal(e.path, 0);
			}
			if (!e.pak) EntryContextMenu(e.path, e.isDir);
			std::string disp = isDirty(e) ? e.name + " *" : e.name;
			ImGui::TextUnformatted(fit(disp).c_str());
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(e.pak ? "%s  (pak/mod — read-only)" : "%s", disp.c_str());
			ImGui::EndGroup();
			ImGui::PopID();
			if (++i % per != 0) ImGui::SameLine();
		}
	}
	else                             // List
	{
		int i = 0;
		if (!atRoot)   // ".." row
		{
			bool upc = ImGui::Selectable(ICON_LC_CORNER_LEFT_UP "  ..", false, ImGuiSelectableFlags_AllowDoubleClick);
			BrowserFolderDropTarget(cwd.parent_path().string());
			if (upc && ImGui::IsMouseDoubleClicked(0)) BrowserNavigate(cwd.parent_path().string());
		}
		for (FEntry& e : entries)
		{
			ImGui::PushID(i++);
			std::string lbl = std::string(e.icon) + "  " + e.name + (isDirty(e) ? " *" : "") + (e.pak ? "  (pak)" : "");
			bool clicked = ImGui::Selectable(lbl.c_str(), e.path == browserSel || browserMSel.count(e.path),
			                                 ImGuiSelectableFlags_AllowDoubleClick);
			if (!e.pak) BrowserDragSource(e.path);
			if (e.isDir && !e.pak) BrowserFolderDropTarget(e.path);
			if (clicked)
			{
				entryClick(e);
				if (ImGui::IsMouseDoubleClicked(0))
				{
					if (e.isDir)                   BrowserNavigate(e.pak ? (cwd / e.name).string() : e.path);
					else if (e.ext == ".nuworld")
					{
						if (e.pak) OpenWorldCmd(e.path.substr(strlen("pak://content/")));
						else       OpenWorldFromBrowser(e.path);
					}
					else if (!e.pak && (e.ext == ".nuprefab" || e.ext == ".numat" || e.ext == ".numesh" || e.ext == ".nuinput" || e.ext == ".nuseq"
			         || e.ext == ".nuanim" || e.ext == ".nusm" || e.ext == ".nublend"
			         || e.ext == ".nuskel" || e.ext == ".nurag" || e.ext == ".nubonemap"
					         || e.ext == ".ogg" || e.ext == ".wav" || e.ext == ".mp3" || e.ext == ".flac"
					         || nuke::AssetEditorForExt(e.ext)))   // module-supplied editors
						OpenAssetEditor(e.path);
					else if (!e.pak && IsTextFile(e.ext)) OpenExternal(e.path, 0);
				}
			}
			if (!e.pak) EntryContextMenu(e.path, e.isDir);
			ImGui::PopID();
		}
	}

	// Empty area below the entries: drop an atom here to save it as a prefab in this folder.
	ImVec2 rest = ImGui::GetContentRegionAvail();
	if (rest.y < 24.0f) rest.y = 24.0f;
	ImGui::InvisibleButton("##browser-drop", rest);
	if (ImGui::BeginDragDropTarget())
	{
		// Prefabs are assets: a drop while browsing sources lands in the content root.
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NUKE_ATOM"))
			SaveAtomAsPrefab(*(Atom**)p->Data, (browserRoot != 0 || browserCwd.empty()) ? contentDir : browserCwd);
		ImGui::EndDragDropTarget();
	}
	ImGui::EndChild();
	});
}
