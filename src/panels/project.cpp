// project panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>
#include <API/Model/PostProcess.h>   // serialize the editor camera's post-effect chain (RTX/rtreflect)
#include <iterator>   // istreambuf_iterator (read world file for disk-sync)

// The project manifest (project/game.nuproj): content dir, startup world, plugin load list.
// Projects have a file (like .sln/.uproject); this is ours, extension .nuproj. The plugin
// pool is shared (modules/); "plugins" is THIS project's chosen load list (dll names).
void EditorUI::SetProjectFile(const std::string& path)
{
	bfs::path p = bfs::absolute(bfs::path(path));   // absolute, so it stays valid after cwd = editor dir
	projectFile = p.string();
	projectDir  = p.has_parent_path() ? p.parent_path().string() : std::string(".");
}

// Phase-1 boot helper: read a single service-provider choice from the .nuproj without
// running the full LoadProject() (which needs the render/UI up). Tolerant of a missing
// or invalid file — "" lets the loader fall back to the first discovered provider.
std::string EditorUI::EarlyProjectService(const std::string& service)
{
	bfs::ifstream f{bfs::path(projectFile)};
	if (!f) return "";
	nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
	if (j.is_discarded() || !j.contains("services") || !j["services"].is_object()) return "";
	return j["services"].value(service, std::string());
}

void EditorUI::SaveProject()
{
	boost::system::error_code ec; bfs::create_directories(projectDir, ec);
	nlohmann::json j;
	j["name"]         = "NukeGame";
	j["engine"]       = "NukeEngine";
	j["content"]      = "content";          // relative to the project dir
	j["startupWorld"] = startupWorld;        // the default world the game loads
	j["unlinkOnDelete"] = unlinkOnDelete;   // break refs to a deleted resource (vs leave dangling)
	j["reloadCleanMode"] = reloadCleanMode; // disk changed, editor clean: 0=ask,1=auto-reload
	j["conflictMode"]    = conflictMode;    // disk changed, editor dirty: 0=ask,1=reload,2=overwrite,3=merge
	j["msaa"]            = msaaSamples;      // anti-aliasing sample count (1/2/4/8)
	j["hdr"]             = hdrEnabled;       // HDR pipeline on/off
	j["hdrPaperWhite"]   = hdrPaperWhite;    // HDR10 diffuse-white nits
	j["hdrPeak"]         = hdrPeak;          // HDR10 peak nits
	j["plugins"]      = enabledPlugins;     // which pooled plugins this project loads
	// Service provider choices (service -> dll). For hot-swappable services the LIVE
	// provider is the truth; for PHASE_BOOT services (render) a persisted choice may be a
	// pending restart-switch from the plugin window — never overwrite it with the live one.
	for (auto& m : nuke::GetModules())
	{
		if (!m || !m->loaded || !*m->provides()) continue;
		if (m->phase() == nuke::PHASE_BOOT && serviceChoices.count(m->provides())) continue;
		serviceChoices[m->provides()] = m->moduleFile;
	}
	j["services"] = serviceChoices;
	nlohmann::json hk = nlohmann::json::object();   // hotkey bindings (id -> chord), saved with the project
	for (auto& kv : nuke::Hotkeys::Get()->ExportBindings()) hk[kv.first] = kv.second;
	j["hotkeys"] = hk;
	bfs::ofstream f{bfs::path(projectFile)};
	if (f) f << j.dump(2);
}
void EditorUI::LoadProject()
{
	boost::system::error_code ec; bfs::create_directories(projectDir, ec);
	bfs::ifstream f{bfs::path(projectFile)};
	if (!f) { SaveProject(); return; }   // first run — create a default .nuproj
	nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
	if (j.is_discarded()) return;
	startupWorld   = j.value("startupWorld", startupWorld);
	unlinkOnDelete = j.value("unlinkOnDelete", false);
	reloadCleanMode = j.value("reloadCleanMode", 0);
	conflictMode    = j.value("conflictMode", 0);
	msaaSamples     = j.value("msaa", 4);
	hdrEnabled      = j.value("hdr", true);
	hdrPaperWhite   = j.value("hdrPaperWhite", 200.0f);
	hdrPeak         = j.value("hdrPeak", 1000.0f);
	if (AppInstance::GetSingleton()->render)
	{
		AppInstance::GetSingleton()->render->setMSAA(msaaSamples);
		AppInstance::GetSingleton()->render->setHDR(hdrEnabled);
		AppInstance::GetSingleton()->render->setHDRNits(hdrPaperWhite, hdrPeak);
	}
	contentDir   = projectDir + "/" + j.value("content", std::string("content"));
	serviceChoices.clear();
	if (j.contains("services") && j["services"].is_object())
		for (auto& kv : j["services"].items())
			if (kv.value().is_string()) serviceChoices[kv.key()] = kv.value().get<std::string>();
	enabledPlugins.clear();
	if (j.contains("plugins") && j["plugins"].is_array())
	{
		pluginListLoaded = true;
		for (auto& p : j["plugins"]) enabledPlugins.push_back(p.get<std::string>());
	}
	// Hotkey bindings are applied AFTER plugins load (so plugin-registered hotkeys exist) — stash them.
	pendingHotkeyBinds.clear();
	if (j.contains("hotkeys") && j["hotkeys"].is_object())
		for (auto& kv : j["hotkeys"].items())
			if (kv.value().is_number_integer()) pendingHotkeyBinds[kv.key()] = kv.value().get<int>();
}

// Window title: "NukeEngine Editor - <project> - <world>".
void EditorUI::UpdateWindowTitle()
{
	AppInstance* app = AppInstance::GetSingleton();
	if (!app->render) return;
	std::string proj = bfs::path(projectDir).filename().string();
	if (proj.empty()) proj = "Untitled";
	std::string title = "NukeEngine Editor - " + proj;
	if (!app->currentWorldPath.empty()) title += " - " + bfs::path(app->currentWorldPath).stem().string();
	if (worldDirty) title += " *";   // unsaved editor changes
	app->render->setWindowTitle(title.c_str());
}

// --- disk <-> editor world sync ---------------------------------------------------------------
static std::string ReadFileText(const std::string& path)
{
	bfs::ifstream f{bfs::path(path)};
	if (!f) return std::string();
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
// Canonical JSON (parse + re-dump) so formatting/indent differences don't read as "changed".
static std::string Canon(const std::string& s)
{
	nlohmann::json j = nlohmann::json::parse(s, nullptr, false);
	return j.is_discarded() ? s : j.dump();
}

void EditorUI::SyncWorldBaseline()
{
	AppInstance* app = AppInstance::GetSingleton();
	worldOnDisk = app->currentScene->SaveToString();   // kept for the merge/conflict flows (once per open/save)
	worldDirty  = false;
	savedWorldSerial = WorldEditSerial();              // dirty tracking = undo cursor vs this
	worldMtime  = 0;
	boost::system::error_code ec;
	if (!app->currentWorldPath.empty())
	{
		std::string full = app->WorldFullPath(app->currentWorldPath);
		if (bfs::exists(full, ec)) worldMtime = (long long)bfs::last_write_time(full, ec);
	}
	UpdateWindowTitle();
}

void EditorUI::TrackDirty()
{
	// Dirty = the undo cursor moved since the last save/load. NOTHING is serialized
	// here: the old implementation diffed the WHOLE world JSON every 15 frames, which
	// froze the editor on big scenes (a 200+-atom prefab = tens of ms, 4x per second —
	// the "editor stutters, Player is fine" asymmetry). Every edit path pushes through
	// PushUndo, so the cursor IS the edit state.
	const bool d = WorldEditSerial() != savedWorldSerial;
	if (d != worldDirty) { worldDirty = d; UpdateWindowTitle(); }
}

void EditorUI::ReloadWorld(const std::string& diskJson)
{
	AppInstance* app = AppInstance::GetSingleton();
	app->selectedInHieararchy = nullptr;
	app->currentScene->LoadFromString(diskJson);
	worldOnDisk = Canon(diskJson);
	worldDirty  = false;
	ResetUndo();
	savedWorldSerial = WorldEditSerial();   // = 0 (stacks just cleared)
	UpdateWindowTitle();
}

void EditorUI::OverwriteWorld()
{
	AppInstance* app = AppInstance::GetSingleton();
	if (app->currentWorldPath.empty()) return;
	app->SaveWorld(app->currentWorldPath);
	SyncWorldBaseline();
}

void EditorUI::TrackExternalChange()
{
	// Re-check ONLY when the editor window regains focus — so a file mid-write (while you're in the
	// other program) never triggers; by the time you tab back it's done. Cross-platform via GLFW focus.
	AppInstance* app = AppInstance::GetSingleton();
	bool focused = !app->render || app->render->isWindowFocused();
	bool gained  = focused && !wasWindowFocused;
	wasWindowFocused = focused;
	if (!gained) return;
	if (openReloadPopup || openConflictPopup) return;   // already prompting
	if (app->currentWorldPath.empty()) return;
	std::string full = app->WorldFullPath(app->currentWorldPath);
	boost::system::error_code ec;
	if (!bfs::exists(full, ec)) return;
	long long mt = (long long)bfs::last_write_time(full, ec);
	if (ec || mt == worldMtime) return;                 // unchanged since we last looked
	// Stability gate: if the file doesn't parse as valid JSON it's still being written / locked —
	// leave worldMtime untouched so we retry on the next focus-gain.
	nlohmann::json pj = nlohmann::json::parse(ReadFileText(full), nullptr, false);
	if (pj.is_discarded()) return;
	std::string disk = pj.dump();
	worldMtime = mt;                                    // record so we don't re-trigger
	if (disk == worldOnDisk) return;                   // same content (e.g. our own save)
	bool dirty = app->currentScene->SaveToString() != worldOnDisk;
	if (!dirty)
	{
		if (reloadCleanMode == 1) ReloadWorld(disk);
		else { pendingDisk = disk; openReloadPopup = true; }
	}
	else
	{
		switch (conflictMode)
		{
			case 1: ReloadWorld(disk); break;                                  // ignore editor, reload from disk
			case 2: OverwriteWorld();  break;                                  // ignore disk, overwrite from editor
			case 3: OpenMerge(app->currentScene->SaveToString(), disk); break; // merge/resolve window
			default: pendingDisk = disk; openConflictPopup = true; break;      // 0 = ask
		}
	}
}

void EditorUI::DrawReloadPopup()
{
	if (openReloadPopup) { ImGui::OpenPopup("Changed on disk##reload"); openReloadPopup = false; }
	if (ImGui::BeginPopupModal("Changed on disk##reload", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextUnformatted("The open world was changed on disk.");
		ImGui::TextUnformatted("Your editor copy has no unsaved changes.");
		ImGui::Separator();
		if (ImGui::Button("Reload")) { ReloadWorld(pendingDisk); pendingDisk.clear(); ImGui::CloseCurrentPopup(); }
		ImGui::SameLine();
		if (ImGui::Button("Ignore")) { pendingDisk.clear(); ImGui::CloseCurrentPopup(); }   // keep editor copy; mtime already advanced
		ImGui::EndPopup();
	}
}

void EditorUI::DrawConflictPopup()
{
	if (openConflictPopup) { ImGui::OpenPopup("Conflict##disk"); openConflictPopup = false; }
	if (ImGui::BeginPopupModal("Conflict##disk", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.30f, 0.25f, 1.0f));
		ImGui::TextUnformatted(ICON_LC_TRIANGLE_ALERT " Changed on disk AND in the editor.");
		ImGui::PopStyleColor();
		ImGui::Separator();
		if (ImGui::Button("Reload (lose editor changes)"))  { ReloadWorld(pendingDisk); pendingDisk.clear(); ImGui::CloseCurrentPopup(); }
		ImGui::SameLine();
		if (ImGui::Button("Overwrite (lose disk changes)")) { OverwriteWorld();          pendingDisk.clear(); ImGui::CloseCurrentPopup(); }
		ImGui::SameLine();
		if (ImGui::Button("Merge…"))   // open the resolve window
		{
			OpenMerge(AppInstance::GetSingleton()->currentScene->SaveToString(), pendingDisk);
			pendingDisk.clear(); ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Ignore")) { pendingDisk.clear(); ImGui::CloseCurrentPopup(); }
		ImGui::EndPopup();
	}
}

// Activate the project's chosen plugins from the shared (already-discovered) pool. On a
// project with no list yet (first run), default every discovered plugin ON and persist it.
// PHASE_BOOT providers (the renderer) are NOT part of the plugin list — they were enabled
// in boot phase 1 and are driven by serviceChoices instead.
void EditorUI::ApplyProjectPlugins()
{
	auto& mods = nuke::GetModules();
	if (!pluginListLoaded)
	{
		enabledPlugins.clear();
		for (auto& m : mods)
			if (m->phase() != nuke::PHASE_BOOT) enabledPlugins.push_back(m->moduleFile);
		pluginListLoaded = true;
		SaveProject();
	}
	for (auto& m : mods)
	{
		if (m->phase() == nuke::PHASE_BOOT) continue;
		bool want = std::find(enabledPlugins.begin(), enabledPlugins.end(), m->moduleFile) != enabledPlugins.end();
		if (want) nuke::EnablePlugin(m.get());
	}
}

// Rebuild the project's plugin list from what's currently loaded, and persist it.
void EditorUI::SyncEnabledPlugins()
{
	enabledPlugins.clear();
	for (auto& m : nuke::GetModules())
		if (m->loaded && m->phase() != nuke::PHASE_BOOT) enabledPlugins.push_back(m->moduleFile);
	SaveProject();
}

// Editor state (NOT world state) -> project/editor_state.json: camera, selection, which
// inspector headers are expanded, the browser view/path/filters, and which panels are open.
void EditorUI::SaveEditorState()
{
	nlohmann::json j;
	if (editorCam && editorCam->transform)
	{
		Transform& t = *editorCam->transform;
		nlohmann::json& jc = j["editorCamera"];
		jc["pos"] = { t.position.x, t.position.y, t.position.z };
		jc["rot"] = { t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w };
		jc["fov"] = editorCam->fov; jc["near"] = editorCam->_near; jc["far"] = editorCam->_far;
		jc["rw"] = editorCam->r_width; jc["rh"] = editorCam->r_height; jc["depth"] = editorCam->depth;
		jc["free"] = editorCam->freeMode; jc["invMouse"] = editorCam->invertMouse;
		jc["yaw"] = camYaw; jc["pitch"] = camPitch;   // editor look angles (not on the Camera component)
		// Post-effect chain (RTX/rtreflect/...) lives on a PostProcess sibling component — persist it too.
		if (Atom* a = AppInstance::GetSingleton()->currentScene->Get("Editor Camera"))
			if (nuke::PostProcess* pp = a->GetComponent<nuke::PostProcess>()) { pp->Commit(); jc["post"] = pp->effectsData; }
	}
	if (auto sel = AppInstance::GetSingleton()->selectedInHieararchy)
		j["selected"] = (long long)sel->id.id;   // stable id (recursive lookup), not name (name misses children)
	nlohmann::json o = nlohmann::json::object();
	for (auto& kv : uiOpen) o[kv.first] = kv.second;
	j["uiOpen"]  = o;
	j["browser"] = { {"view", browserView}, {"cwd", browserCwd}, {"search", std::string(browserSearch)},
	                 {"fMesh", fMesh}, {"fMat", fMat}, {"fTex", fTex}, {"fPrefab", fPrefab} };
	if (win) j["panels"] = { {"hierarchy", win->hierarchy}, {"console", win->console}, {"browser", win->browser},
	                         {"inspector", win->inspector}, {"render", win->render}, {"plugmgr", win->plugmgr}, {"about", win->about} };
	nlohmann::json wo = nlohmann::json::object();   // host-owned window open flags (e.g. plugin windows)
	for (auto& kv : AppInstance::GetSingleton()->windowOpen) wo[kv.first] = kv.second;
	j["windowOpen"] = wo;
	j["worldSettingsOpen"] = worldSettingsOpen;   // keep the World Settings window open across restarts
	if (iRender* r = AppInstance::GetSingleton()->render) j["maximized"] = r->isWindowMaximized();
	bfs::ofstream f{bfs::path(projectDir + "/editor_state.json")};
	if (f) f << j.dump(2);
}

void EditorUI::LoadEditorState()
{
	bfs::ifstream f{bfs::path(projectDir + "/editor_state.json")};
	if (!f) return;
	nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
	if (j.is_discarded()) return;
	if (j.contains("uiOpen") && j["uiOpen"].is_object())
		for (auto& kv : j["uiOpen"].items()) uiOpen[kv.key()] = kv.value().get<bool>();
	// NOTE: asset editors deliberately do NOT auto-reopen (user: surprise windows at
	// startup; and a bad asset would crash every launch). Stale "assetEditors" keys in
	// old state files are simply ignored. DEV HOOK: the env var below opens one at boot
	// for headless testing of the secondary-window render path.
	if (const char* devOpen = std::getenv("NUKE_OPEN_ASSET"))
		if (devOpen[0]) OpenAssetEditor(devOpen);
	if (j.contains("selected") && j["selected"].is_number_integer())
		pendingSelectId = (long)j["selected"].get<long long>();
	if (j.contains("editorCamera") && editorCam && editorCam->transform)
	{
		nlohmann::json& jc = j["editorCamera"];
		Transform& t = *editorCam->transform;
		if (jc.contains("pos")) { auto p = jc["pos"]; t.position.x = p[0]; t.position.y = p[1]; t.position.z = p[2]; }
		if (jc.contains("rot")) { auto r = jc["rot"]; t.rotation.x = r[0]; t.rotation.y = r[1]; t.rotation.z = r[2]; t.rotation.w = r[3]; }
		editorCam->fov = jc.value("fov", editorCam->fov);
		editorCam->_near = jc.value("near", editorCam->_near); editorCam->_far = jc.value("far", editorCam->_far);
		editorCam->r_width = jc.value("rw", editorCam->r_width); editorCam->r_height = jc.value("rh", editorCam->r_height);
		editorCam->depth = jc.value("depth", editorCam->depth);
		editorCam->freeMode = jc.value("free", editorCam->freeMode); editorCam->invertMouse = jc.value("invMouse", editorCam->invertMouse);
		camYaw = jc.value("yaw", camYaw); camPitch = jc.value("pitch", camPitch);
		if (jc.contains("post"))   // restore the post-effect chain (RTX/rtreflect/...); recreate the component if needed
		{
			if (Atom* a = AppInstance::GetSingleton()->currentScene->Get("Editor Camera"))
			{
				nuke::PostProcess* pp = a->GetComponent<nuke::PostProcess>();
				if (!pp) { pp = new nuke::PostProcess(); a->AddComponent(pp); }
				pp->effectsData = jc["post"].get<std::string>();
				pp->EnsureParsed();
			}
		}
	}
	if (j.contains("browser"))
	{
		nlohmann::json& b = j["browser"];
		browserView = b.value("view", browserView);
		browserCwd  = b.value("cwd", browserCwd);
		std::string s = b.value("search", std::string());
		strncpy(browserSearch, s.c_str(), sizeof(browserSearch) - 1); browserSearch[sizeof(browserSearch) - 1] = 0;
		fMesh = b.value("fMesh", true); fMat = b.value("fMat", true);
		fTex  = b.value("fTex", true);  fPrefab = b.value("fPrefab", true);
	}
	if (j.contains("panels") && win)
	{
		nlohmann::json& p = j["panels"];
		win->hierarchy = p.value("hierarchy", win->hierarchy);
		win->console   = p.value("console",   win->console);
		win->browser   = p.value("browser",   win->browser);
		win->inspector = p.value("inspector", win->inspector);
		win->render    = p.value("render",    win->render);
		win->plugmgr   = p.value("plugmgr",   win->plugmgr);
		win->about     = p.value("about",     win->about);
	}
	// Pre-populate host window flags so plugin windows (pushed later) restore their state.
	if (j.contains("windowOpen") && j["windowOpen"].is_object())
		for (auto& kv : j["windowOpen"].items())
			AppInstance::GetSingleton()->windowOpen[kv.key()] = kv.value().get<bool>();
	worldSettingsOpen = j.value("worldSettingsOpen", false);
	if (j.contains("maximized"))
		if (iRender* r = AppInstance::GetSingleton()->render) r->setWindowMaximized(j["maximized"].get<bool>());
}
