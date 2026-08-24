// Project panel: .nuproj load/save, disk sync, plugin activation and project switching.
#include <editor/editorui.h>
#include <API/Model/PostProcess.h>
#include <API/Model/Layers.h>        // render-layer slot names persist in the .nuproj
#include <input/Input.h>             // Q6: explicit input-map list
#include <iterator>
#include <iostream>
#include <cstring>

// Point the editor at a .nuproj manifest, deriving the project directory from it.
void EditorUI::SetProjectFile(const std::string& path)
{
	bfs::path p = bfs::absolute(bfs::path(path));   // absolute: cwd later becomes the editor dir
	projectFile = p.string();
	projectDir  = p.has_parent_path() ? p.parent_path().string() : std::string(".");
}

// Read one service-provider choice from the .nuproj during boot, before LoadProject() can run.
// Returns "" for a missing/invalid file, which makes the loader pick the first discovered provider.
std::string EditorUI::EarlyProjectService(const std::string& service)
{
	bfs::ifstream f{bfs::path(projectFile)};
	if (!f) return "";
	nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
	if (j.is_discarded() || !j.contains("services") || !j["services"].is_object()) return "";
	return j["services"].value(service, std::string());
}

// Write the .nuproj manifest.
// Q6: push the enabled-map list into the engine filter. The filter gates the content SCAN,
// so newly excluded/included files take full effect on the next project load; the live
// merged map keeps what it already loaded (bindings only ADD at runtime).
void EditorUI::ApplyInputMaps()
{
	nuke::Input::SetEnabledMaps(inputMapsAuto ? std::vector<std::string>{} : inputMapsList);
}

void EditorUI::SaveProject()
{
	boost::system::error_code ec; bfs::create_directories(projectDir, ec);
	nlohmann::json j;
	j["name"]         = projectName;
	j["engine"]       = "NukeEngine";
	j["content"]      = "content";          // relative to the project dir
	// Pak compression method: 0 store / 1 zlib / 2 zstd.
	j["pakMethod"] = pakMethod; j["pakLevel"] = pakLevel; j["pakBlockMB"] = pakBlockMB;
	j["modMethod"] = modMethod; j["modLevel"] = modLevel;
	j["modSplit"]  = modSplitMode;   // 0 one file / 1 by content type / 2 size cap
	j["modSplitCapMB"] = modSplitCapMB;
	j["gameIcon"]  = gameIcon;              // .ico stamped onto the shipped exe
	j["distPath"]  = distPath;              // build output ("" = <project>/dist)
	j["modName"]   = modName;               // last packaged mod name
	j["startupWorld"] = startupWorld;
	j["unlinkOnDelete"] = unlinkOnDelete;   // break refs to a deleted resource vs leave dangling
	j["reloadCleanMode"] = reloadCleanMode; // disk changed, editor clean: 0=ask,1=auto-reload
	j["conflictMode"]    = conflictMode;    // disk changed, editor dirty: 0=ask,1=reload,2=overwrite,3=merge
	j["msaa"]            = msaaSamples;      // 1/2/4/8
	j["hdr"]             = hdrEnabled;
	j["hdrPaperWhite"]   = hdrPaperWhite;    // HDR10 diffuse-white nits
	j["hdrPeak"]         = hdrPeak;          // HDR10 peak nits
	j["plugins"]      = enabledPlugins;
	// The seen-plugins ledger: an entry missing from "plugins" but present here was turned OFF
	// deliberately, so the editor must not re-enable it on the next start.
	j["pluginsSeen"]  = std::vector<std::string>(knownPlugins.begin(), knownPlugins.end());
	// Service choices (service -> dll). For PHASE_BOOT services a persisted choice may be a
	// pending restart-switch, so it must never be overwritten with the live provider.
	for (auto& m : nuke::GetModules())
	{
		if (!m || !m->loaded || !*m->provides()) continue;
		if (m->sharedService()) continue;   // shared services load via the plugin list, no single choice
		if (m->phase() == nuke::PHASE_BOOT && serviceChoices.count(m->provides())) continue;
		serviceChoices[m->provides()] = nuke::ModuleName(m->moduleFile);   // platform-neutral name
	}
	j["services"] = serviceChoices;
	j["layers"] = nuke::Layers::All();   // render-layer slot names
	// Q5 viewport snap (toggle + increments) — a project convention, not a machine preference.
	j["snap"] = { {"on", snapEnabled}, {"move", snapMove}, {"rot", snapRot}, {"scale", snapScale},
	              {"grid", gridVisible} };
	// Q6 input maps: the key is ABSENT in auto mode (every .nuinput loads); present = explicit.
	if (!inputMapsAuto) j["inputMaps"] = inputMapsList;
	// Serialize before opening the file: ofstream truncates on open, so a dump() throw would
	// leave a zero-byte .nuproj. `replace` turns stray non-UTF-8 bytes into U+FFFD.
	std::string out;
	try { out = j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace); }
	catch (const std::exception& e)
	{
		std::cout << "[editor]\tSaveProject: serialize FAILED (" << e.what() << ") — .nuproj left untouched" << std::endl;
		return;
	}
	bfs::ofstream f{bfs::path(projectFile)};
	if (f) f << out;
}
// Read the .nuproj manifest into the editor state, applying render settings as it goes.
void EditorUI::LoadProject()
{
	boost::system::error_code ec; bfs::create_directories(projectDir, ec);
	bfs::ifstream f{bfs::path(projectFile)};
	if (!f) { SaveProject(); return; }   // first run: create a default .nuproj
	nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
	if (j.is_discarded()) return;
	startupWorld   = j.value("startupWorld", startupWorld);
	projectName    = j.value("name", projectName);
	unlinkOnDelete = j.value("unlinkOnDelete", false);
	reloadCleanMode = j.value("reloadCleanMode", 0);
	conflictMode    = j.value("conflictMode", 0);
	pakMethod = j.value("pakMethod", 3); pakLevel = j.value("pakLevel", 9);
	pakBlockMB = j.value("pakBlockMB", 8); if (pakBlockMB < 1) pakBlockMB = 1;
	modMethod = j.value("modMethod", 0); modLevel = j.value("modLevel", 0);
	modSplitMode  = j.value("modSplit", 0);
	modSplitCapMB = j.value("modSplitCapMB", 512);
	gameIcon  = j.value("gameIcon", std::string());
	distPath  = j.value("distPath", std::string());
	modName   = j.value("modName", std::string());
	// If the project came from an archive, the extractor left a base-pak pointer for Package Mod.
	{
		bfs::ifstream bm{bfs::path(projectDir + "/.nupak_base")};
		if (bm) std::getline(bm, basePakPath);
	}
	if (j.contains("snap") && j["snap"].is_object())
	{
		snapEnabled = j["snap"].value("on", false);
		snapMove    = j["snap"].value("move", 0.5f);
		snapRot     = j["snap"].value("rot", 15.0f);
		snapScale   = j["snap"].value("scale", 0.1f);
		gridVisible = j["snap"].value("grid", true);
	}
	inputMapsAuto = true; inputMapsList.clear();
	if (j.contains("inputMaps") && j["inputMaps"].is_array())
	{
		inputMapsAuto = false;
		for (auto& m : j["inputMaps"]) if (m.is_string()) inputMapsList.push_back(m.get<std::string>());
	}
	// BEFORE the content scan: the scan is what loads .nuinput files.
	ApplyInputMaps();
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
			if (kv.value().is_string())
			{
				const std::string v = kv.value().get<std::string>();
				const std::string canon = nuke::ModuleName(v);   // platform-neutral
				if (canon != v) projectHealed = true;
				serviceChoices[kv.key()] = canon;
			}
	enabledPlugins.clear();
	knownPlugins.clear();
	// HEAL: a session that read module names out of dying DLL descriptors once persisted raw
	// binary garbage into these lists. Only printable module FILE names pass; everything else
	// is dropped loudly (the next save writes the clean lists, discovery re-adds real modules).
	// The list stores platform-neutral module NAMES ("NukeVFX") — legacy entries with an
	// extension ("NukeVFX.dll", "libNukeVFX.so") canonicalize on load, so a project moves
	// between Windows / macOS / Linux without its plugin list going stale.
	auto validModuleName = [](const std::string& n) -> bool
	{
		if (n.size() < 2 || n.size() > 128) return false;
		for (unsigned char c : n)
			if (c < 0x20 || c >= 0x7F || c == '/' || c == '\\') return false;
		return true;
	};
	if (j.contains("plugins") && j["plugins"].is_array())
	{
		pluginListLoaded = true;
		for (auto& p : j["plugins"])
		{
			const std::string n = p.get<std::string>();
			if (validModuleName(n))
			{
				const std::string canon = nuke::ModuleName(n);
				if (canon != n) projectHealed = true;   // legacy platform file name: migrate on save
				enabledPlugins.push_back(canon);
			}
			else { projectHealed = true; std::cout << "[editor]	game.nuproj: dropped a corrupted plugins entry (" << n.size() << " bytes)" << std::endl; }
		}
		// Older projects have no ledger: everything listed counts as seen, and anything else is
		// treated as new (offered once) rather than as a deliberate "off".
		for (const std::string& p : enabledPlugins) knownPlugins.insert(p);
	}
	if (j.contains("pluginsSeen") && j["pluginsSeen"].is_array())
		for (auto& p : j["pluginsSeen"])
		{
			const std::string n = p.get<std::string>();
			if (validModuleName(n))
			{
				const std::string canon = nuke::ModuleName(n);
				if (canon != n) projectHealed = true;
				knownPlugins.insert(canon);
			}
			else { projectHealed = true; std::cout << "[editor]	game.nuproj: dropped a corrupted pluginsSeen entry (" << n.size() << " bytes)" << std::endl; }
		}
	// Render-layer slot names -> the engine's Layers registry.
	if (j.contains("layers") && j["layers"].is_array())
	{
		std::vector<std::string> names;
		for (auto& n : j["layers"]) names.push_back(n.is_string() ? n.get<std::string>() : std::string());
		nuke::Layers::SetAll(names);
	}
	// LEGACY: hotkeys used to live in the .nuproj; they are machine preferences now
	// (config/editor_prefs). An old project's bindings still load, the prefs override them,
	// and the key is simply not written back.
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

// --- disk <-> editor world sync ---------------------------------------------

static std::string ReadFileText(const std::string& path)
{
	bfs::ifstream f{bfs::path(path)};
	if (!f) return std::string();
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
// Canonical JSON so formatting/indent differences don't read as "changed".
static std::string Canon(const std::string& s)
{
	nlohmann::json j = nlohmann::json::parse(s, nullptr, false);
	return j.is_discarded() ? s : j.dump();
}

// Re-baseline the open world against disk: content snapshot, dirty flag, mtime.
void EditorUI::SyncWorldBaseline()
{
	AppInstance* app = AppInstance::GetSingleton();
	worldOnDisk = app->currentWorld->SaveToString();   // baseline for the merge/conflict flows
	worldDirty  = false;
	savedWorldSerial = WorldEditSerial();              // dirty = undo cursor vs this
	worldMtime  = 0;
	boost::system::error_code ec;
	if (!app->currentWorldPath.empty())
	{
		std::string full = app->WorldFullPath(app->currentWorldPath);
		if (bfs::exists(full, ec)) worldMtime = (long long)bfs::last_write_time(full, ec);
	}
	UpdateWindowTitle();
}

// Update the dirty flag from the undo cursor. Deliberately serializes nothing: diffing the
// world JSON per frame is far too slow on large worlds, and every edit path goes through PushUndo.
void EditorUI::TrackDirty()
{
	const bool d = WorldEditSerial() != savedWorldSerial;
	if (d != worldDirty) { worldDirty = d; UpdateWindowTitle(); }
}

// Replace the open world with the given JSON from disk and reset undo/dirty state.
void EditorUI::ReloadWorld(const std::string& diskJson)
{
	AppInstance* app = AppInstance::GetSingleton();
	app->selectedInHieararchy = nullptr;
	app->currentWorld->LoadFromString(diskJson);
	worldOnDisk = Canon(diskJson);
	worldDirty  = false;
	ResetUndo();
	savedWorldSerial = WorldEditSerial();   // 0: the stacks were just cleared
	UpdateWindowTitle();
}

// Save the editor's world over the file on disk and re-baseline.
void EditorUI::OverwriteWorld()
{
	AppInstance* app = AppInstance::GetSingleton();
	if (app->currentWorldPath.empty()) return;
	app->SaveWorld(app->currentWorldPath);
	SyncWorldBaseline();
}

// Detect an external edit of the open world and route it to reload/overwrite/merge.
// Checks only on focus-gain, so a file still being written by another program never triggers.
void EditorUI::TrackExternalChange()
{
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
	// Unparseable = still being written: leave worldMtime alone so the next focus-gain retries.
	nlohmann::json pj = nlohmann::json::parse(ReadFileText(full), nullptr, false);
	if (pj.is_discarded()) return;
	std::string disk = pj.dump();
	worldMtime = mt;                                    // record so we don't re-trigger
	if (disk == worldOnDisk) return;                   // same content, e.g. our own save
	bool dirty = app->currentWorld->SaveToString() != worldOnDisk;
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
			case 3: OpenMerge(app->currentWorld->SaveToString(), disk); break; // merge window
			default: pendingDisk = disk; openConflictPopup = true; break;      // 0 = ask
		}
	}
}

// Modal for "changed on disk, editor clean".
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
		if (ImGui::Button("Ignore")) { pendingDisk.clear(); ImGui::CloseCurrentPopup(); }   // mtime already advanced
		ImGui::EndPopup();
	}
}

// Modal for "changed on disk and in the editor": reload / overwrite / merge / ignore.
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
		if (ImGui::Button("Merge…"))
		{
			OpenMerge(AppInstance::GetSingleton()->currentWorld->SaveToString(), pendingDisk);
			pendingDisk.clear(); ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Ignore")) { pendingDisk.clear(); ImGui::CloseCurrentPopup(); }
		ImGui::EndPopup();
	}
}

// Activate the project's chosen plugins from the discovered pool; a project with no list yet
// defaults every plugin on. PHASE_BOOT providers are driven by serviceChoices, not this list.
void EditorUI::ApplyProjectPlugins()
{
	auto& mods = nuke::GetModules();
	if (!pluginListLoaded)
	{
		enabledPlugins.clear();
		for (auto& m : mods)
			if (m->phase() != nuke::PHASE_BOOT) enabledPlugins.push_back(nuke::ModuleName(m->moduleFile));
		pluginListLoaded = true;
		SaveProject();
	}
	auto listed = [&](const std::string& file)
	{
		// Stem match: a project written on Windows lists "NukeVFX.dll" — that entry must keep
		// enabling NukeVFX.so/.dylib here (and the other way round).
		for (const std::string& e : enabledPlugins)
			if (nuke::ModuleFileMatches(e, file)) return true;
		return false;
	};

	for (auto& m : mods)
	{
		if (m->phase() == nuke::PHASE_BOOT) continue;
		// An editor companion is OFFERED by default, not forced: a module the user unticked stays
		// off (it is simply absent from the project's list), and one that appeared after the list
		// was written is enabled on first sight and recorded. Every vtable query goes through the
		// engine's shielded wrappers — a stale plugin must not kill the editor at startup.
		const bool isTool = nuke::ModuleIsEditorTool(m.get());
		bool want = listed(m->moduleFile);
		if (!want && isTool && !knownPlugins.count(nuke::ModuleName(m->moduleFile)))
		{
			want = true;
			enabledPlugins.push_back(nuke::ModuleName(m->moduleFile));
			pluginListDirty = true;
		}
		// A companion has nothing to edit while its runtime module is off — and its panels would
		// reference component types that are not registered at all.
		const std::string companion = nuke::ModuleCompanionOf(m.get());
		if (want && !companion.empty() && !listed(companion))
		{
			std::cout << "[editor]	" << m->moduleFile << " stays off: its runtime module '"
			          << companion << "' is disabled" << std::endl;
			want = false;
		}
		knownPlugins.insert(nuke::ModuleName(m->moduleFile));
		if (want) nuke::EnablePlugin(m.get());
	}
	if (pluginListDirty) { pluginListDirty = false; SaveProject(); }
}

// Rebuild the project's plugin list from what's currently loaded, and persist it.
void EditorUI::SyncEnabledPlugins()
{
	enabledPlugins.clear();
	for (auto& m : nuke::GetModules())
	{
		if (!m || m->phase() == nuke::PHASE_BOOT) continue;
		knownPlugins.insert(nuke::ModuleName(m->moduleFile));   // seen, whether it ends up on or off
		if (m->loaded) enabledPlugins.push_back(nuke::ModuleName(m->moduleFile));
	}
	SaveProject();
}

// --- New / Open Project ----------------------------------------------------

// Switch projects by relaunching the editor on the given path: the project lifecycle
// (PHASE_BOOT renderer, plugin set, ResDB) is bound to startup and cannot be swapped live.
void EditorUI::SwitchToProject(const std::string& path)
{
	if (path.empty()) return;
	if (!EditorRelaunch(path))
	{
		std::cout << "[editor]	failed to launch the editor on " << path << std::endl;
		return;
	}
	std::cout << "[editor]	switching to " << path << std::endl;
	if (AppInstance::GetSingleton()->render)
		AppInstance::GetSingleton()->render->requestClose();   // this instance hands over
}

// Switch projects, prompting first if the open world is dirty.
void EditorUI::RequestProjectSwitch(const std::string& path)
{
	if (path.empty()) return;
	if (worldDirty) { pendingSwitchPath = path; openSwitchConfirm = true; }
	else SwitchToProject(path);
}

void EditorUI::OpenProjectCmd()
{
	std::string picked = EditorPickProjectFile();
	if (!picked.empty()) RequestProjectSwitch(picked);
}

// Unsaved-world guard for a project switch.
void EditorUI::DrawSwitchConfirmPopup()
{
	if (openSwitchConfirm) { ImGui::OpenPopup("Unsaved changes##switch"); openSwitchConfirm = false; }
	if (ImGui::BeginPopupModal("Unsaved changes##switch", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("The open world has unsaved changes.");
		ImGui::Separator();
		if (ImGui::Button("Save and switch"))
		{
			SaveWorldCmd();
			std::string p = pendingSwitchPath; pendingSwitchPath.clear();
			ImGui::CloseCurrentPopup();
			SwitchToProject(p);
		}
		ImGui::SameLine();
		if (ImGui::Button("Switch without saving"))
		{
			std::string p = pendingSwitchPath; pendingSwitchPath.clear();
			ImGui::CloseCurrentPopup();
			SwitchToProject(p);
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) { pendingSwitchPath.clear(); ImGui::CloseCurrentPopup(); }
		ImGui::EndPopup();
	}
}

// "New Project" modal: scaffolds <location>/<name>/{game.nuproj, content/} and relaunches on it.
void EditorUI::DrawNewProjectPopup()
{
	if (openNewProjectPopup)
	{
		ImGui::OpenPopup("New Project");
		openNewProjectPopup = false;
	}
	if (ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::SetNextItemWidth(480);
		ImGui::InputText("Name", newProjName, sizeof(newProjName));
		std::string locShown = newProjDir.empty() ? std::string("(pick a folder)") : newProjDir;
		if (ImGui::Button((locShown + "##nploc").c_str(), ImVec2(480, 0)))
		{
			std::string d = EditorPickFolder();
			if (!d.empty()) newProjDir = d;
		}
		ImGui::SameLine(0, 6); ImGui::TextUnformatted("Location");

		// Offer the shared module pool only: project-local modules belong to their own project.
		// Render providers are a single services.render choice; everything else is a checkbox.
		boost::system::error_code mec;
		const std::string poolPrefix = bfs::absolute(bfs::path("modules"), mec).generic_string();
		auto inSharedPool = [&](NUKEModule* m)
		{
			boost::system::error_code ec2;
			const std::string mp = bfs::absolute(bfs::path(m->modulePath), ec2).generic_string();
			return mp.rfind(poolPrefix, 0) == 0;
		};
		if (!newProjModsInit)
		{
			newProjMods.clear(); newProjRender.clear();
			for (auto& m : nuke::GetModules())
			{
				if (!m || !inSharedPool(m.get())) continue;
				if (m->phase() == nuke::PHASE_BOOT)
				{
					// default = the renderer this session runs on, else the first provider
					if (std::string(m->provides()) == "render" && (newProjRender.empty() || m->loaded))
						newProjRender = m->moduleFile;
				}
				else newProjMods[m->moduleFile] = true;   // default: everything on
			}
			newProjModsInit = true;
		}
		ImGui::SeparatorText("Modules");
		// Fixed-height scrollable table so a large pool cannot stretch the modal.
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
		ImGui::BeginChild("np_mods", ImVec2(560, 240), ImGuiChildFlags_Borders);
		ImGui::PopStyleVar();
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8, 3));
		if (ImGui::BeginTable("np_mods_tbl", 2,
		                      ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
		                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX))
		{
			ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthFixed, 210.0f);
			ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
			for (auto& m : nuke::GetModules())
			{
				if (!m || m->phase() == nuke::PHASE_BOOT) continue;
				auto it = newProjMods.find(m->moduleFile);
				if (it == newProjMods.end()) continue;
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Checkbox((std::string(m->title) + "##npm" + m->moduleFile).c_str(), &it->second);
				ImGui::TableSetColumnIndex(1);
				// First line of the module's self-description; full text on hover.
				std::string d = m->description;
				const size_t nl = d.find('\n');
				ImGui::TextDisabled("%s", (nl == std::string::npos ? d : d.substr(0, nl)).c_str());
				if (!d.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
					ImGui::SetTooltip("%s", d.c_str());
			}
			ImGui::EndTable();
		}
		ImGui::PopStyleVar();
		ImGui::EndChild();
		// Renderer combo, only when there is more than one shared provider.
		{
			std::vector<NUKEModule*> renders;
			for (auto& m : nuke::GetModules())
				if (m && m->phase() == nuke::PHASE_BOOT && std::string(m->provides()) == "render"
				    && inSharedPool(m.get()))
					renders.push_back(m.get());
			if (renders.size() > 1)
			{
				const char* cur = "?";
				for (auto* r : renders) if (newProjRender == r->moduleFile) cur = r->title;
				ImGui::SetNextItemWidth(300);
				if (ImGui::BeginCombo("Renderer", cur))
				{
					for (auto* r : renders)
						if (ImGui::Selectable(r->title, newProjRender == r->moduleFile))
							newProjRender = r->moduleFile;
					ImGui::EndCombo();
				}
			}
		}

		// Require a clean name and a picked folder; refuse to hijack an existing project.
		std::string name = newProjName;
		bool nameOk = !name.empty();
		for (char c : name) nameOk &= (std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == ' ');
		boost::system::error_code ec;
		bfs::path target = newProjDir.empty() ? bfs::path() : bfs::path(newProjDir) / name;
		bool exists = !target.empty() && bfs::exists(target / "game.nuproj", ec);
		if (!target.empty() && nameOk) ImGui::TextDisabled("-> %s", target.string().c_str());
		if (!nameOk)  ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1), "Name: letters, digits, space, _ and - only.");
		if (exists)   ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1), "A project already exists there.");

		ImGui::Separator();
		ImGui::BeginDisabled(!nameOk || newProjDir.empty() || exists);
		if (ImGui::Button("Create", ImVec2(120, 0)))
		{
			bfs::create_directories(target / "content", ec);
			nlohmann::json j;
			j["name"] = name;
			j["engine"] = "NukeEngine";
			j["content"] = "content";
			// Write a real plugin list so the first LoadProject does not default everything on.
			nlohmann::json plugins = nlohmann::json::array();
			for (auto& kv : newProjMods) if (kv.second) plugins.push_back(kv.first);
			j["plugins"] = plugins;
			if (!newProjRender.empty()) j["services"] = { { "render", newProjRender } };
			bfs::ofstream f{target / "game.nuproj"};
			if (f)
			{
				f << j.dump(2);
				f.close();
				ImGui::CloseCurrentPopup();
				newProjModsInit = false;   // fresh defaults next time
				RequestProjectSwitch((target / "game.nuproj").string());
			}
			else std::cout << "[editor]	can't create " << (target / "game.nuproj").string() << std::endl;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0))) { newProjModsInit = false; ImGui::CloseCurrentPopup(); }
		ImGui::EndPopup();
	}
}

// Push a path onto the machine-wide recent-projects list: newest first, case-insensitively
// deduped, capped at 10, persisted to preferences.
void EditorUI::PushRecentProject(const std::string& path)
{
	if (path.empty()) return;
	std::string key = bfs::absolute(bfs::path(path)).string();
	auto low = [](std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; };
	const std::string keyLow = low(key);
	recentProjects.erase(std::remove_if(recentProjects.begin(), recentProjects.end(),
		[&](const std::string& r) { return low(r) == keyLow; }), recentProjects.end());
	recentProjects.insert(recentProjects.begin(), key);
	if (recentProjects.size() > 10) recentProjects.resize(10);
	SavePreferences();
}

// Project hub: the entire UI when the editor booted with no project. Recent list, Open, New.
void EditorUI::DrawProjectHub()
{
	ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
	                               vp->WorkPos.y + vp->WorkSize.y * 0.5f),
	                        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(640, 460), ImGuiCond_Once);
	ImGui::SetNextWindowViewport(vp->ID);   // pin: NoAutoMerge must not float it into its own OS window
	// Tighter padding than the theme; the pushes must cover Begin and the child below.
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
	ImGui::Begin("Projects##hub", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking);
	ImGui::TextUnformatted("NukeEngine");
	ImGui::TextDisabled("Choose a project to open, or create a new one.");
	ImGui::Separator();
	ImGui::TextDisabled("Recent");
	const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));   // the table has its own cell padding
	ImGui::BeginChild("hub_recent", ImVec2(0, -footer), ImGuiChildFlags_Borders);
	ImGui::PopStyleVar();
	if (recentProjects.empty()) ImGui::TextDisabled("(no recent projects)");
	else
	{
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8, 4));
		if (ImGui::BeginTable("hub_recent_tbl", 2,
		                      ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
		                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY |
		                      ImGuiTableFlags_PadOuterX))   // without outer V borders imgui drops edge padding
		{
			ImGui::TableSetupColumn("Project", ImGuiTableColumnFlags_WidthFixed, 170.0f);
			ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();
			int idx = 0;
			for (auto& p : recentProjects)
			{
				boost::system::error_code ec;
				const bool exists = bfs::exists(bfs::path(p), ec);
				bfs::path pp(p);
				std::string name = pp.parent_path().filename().string();
				if (name.empty()) name = pp.stem().string();
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::BeginDisabled(!exists);
				// Full-row click target; the path renders over it in column 1.
				if (ImGui::Selectable((name + "##rec" + std::to_string(idx++)).c_str(), false,
				                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
					RequestProjectSwitch(p);
				ImGui::EndDisabled();
				ImGui::TableSetColumnIndex(1);
				ImGui::TextDisabled("%s%s", p.c_str(), exists ? "" : "  (missing)");
			}
			ImGui::EndTable();
		}
		ImGui::PopStyleVar();
	}
	ImGui::EndChild();
	if (ImGui::Button("New Project...", ImVec2(140, 0))) openNewProjectPopup = true;
	ImGui::SameLine();
	if (ImGui::Button("Open Project...", ImVec2(140, 0))) OpenProjectCmd();
	ImGui::SameLine();
	if (ImGui::Button("Exit", ImVec2(100, 0)) && AppInstance::GetSingleton()->render)
		AppInstance::GetSingleton()->render->requestClose();
	ImGui::End();
	ImGui::PopStyleVar();   // hub WindowPadding
}

// Persist editor state (not world state) to project/editor_state.json: camera, selection,
// browser view/path/filters, and which panels and windows are open.
void EditorUI::SaveEditorState()
{
	if (projectHubMode) return;   // no project open: nowhere to write
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
		jc["yaw"] = camYaw; jc["pitch"] = camPitch;   // editor look angles, not on the Camera component
		// The post-effect chain lives on a sibling PostProcess component.
		if (Atom* a = AppInstance::GetSingleton()->currentWorld->Get("Editor Camera"))
			if (nuke::PostProcess* pp = a->GetComponent<nuke::PostProcess>()) { pp->Commit(); jc["post"] = pp->effectsData; }
	}
	if (auto sel = AppInstance::GetSingleton()->selectedInHieararchy)
		j["selected"] = (long long)sel->id.id;   // stable id, not name: names miss children
	nlohmann::json o = nlohmann::json::object();
	for (auto& kv : uiOpen) o[kv.first] = kv.second;
	j["uiOpen"]  = o;
	j["browser"] = { {"view", browserView}, {"cwd", browserCwd}, {"search", std::string(browserSearch)},
	                 {"fMesh", fMesh}, {"fMat", fMat}, {"fTex", fTex}, {"fPrefab", fPrefab} };
	if (win) j["panels"] = { {"hierarchy", win->hierarchy}, {"console", win->console}, {"browser", win->browser},
	                         {"inspector", win->inspector}, {"render", win->render}, {"plugmgr", win->plugmgr}, {"about", win->about} };
	nlohmann::json wo = nlohmann::json::object();   // host-owned window open flags
	for (auto& kv : AppInstance::GetSingleton()->windowOpen) wo[kv.first] = kv.second;
	j["windowOpen"] = wo;
	j["worldSettingsOpen"] = worldSettingsOpen;
	j["lastWorld"] = AppInstance::GetSingleton()->currentWorldPath;   // reopened next launch; "" = default
	if (iRender* r = AppInstance::GetSingleton()->render) j["maximized"] = r->isWindowMaximized();
	bfs::ofstream f{bfs::path(projectDir + "/editor_state.json")};
	if (f) f << j.dump(2);
}

// Restore editor state from project/editor_state.json.
void EditorUI::LoadEditorState()
{
	bfs::ifstream f{bfs::path(projectDir + "/editor_state.json")};
	if (!f) return;
	nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
	if (j.is_discarded()) return;
	if (j.contains("uiOpen") && j["uiOpen"].is_object())
		for (auto& kv : j["uiOpen"].items()) uiOpen[kv.key()] = kv.value().get<bool>();
	// Asset editors deliberately do not auto-reopen; stale "assetEditors" keys are ignored.
	if (j.contains("selected") && j["selected"].is_number_integer())
		pendingSelectId = (long)j["selected"].get<long long>();
	lastWorld = j.value("lastWorld", std::string());
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
		if (jc.contains("post"))   // restore the post-effect chain, recreating the component if needed
		{
			if (Atom* a = AppInstance::GetSingleton()->currentWorld->Get("Editor Camera"))
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
