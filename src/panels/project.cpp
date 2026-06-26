// project panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>

// The project manifest (project/game.nuproj): content dir, startup world, plugin load list.
// Projects have a file (like .sln/.uproject); this is ours, extension .nuproj. The plugin
// pool is shared (modules/); "plugins" is THIS project's chosen load list (dll names).
void EditorUI::SaveProject()
{
	boost::system::error_code ec; bfs::create_directories(projectDir, ec);
	nlohmann::json j;
	j["name"]         = "NukeGame";
	j["engine"]       = "NukeEngine";
	j["content"]      = "content";          // relative to the project dir
	j["startupWorld"] = startupWorld;
	j["plugins"]      = enabledPlugins;     // which pooled plugins this project loads
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
	startupWorld = j.value("startupWorld", startupWorld);
	contentDir   = projectDir + "/" + j.value("content", std::string("content"));
	enabledPlugins.clear();
	if (j.contains("plugins") && j["plugins"].is_array())
	{
		pluginListLoaded = true;
		for (auto& p : j["plugins"]) enabledPlugins.push_back(p.get<std::string>());
	}
}

// Activate the project's chosen plugins from the shared (already-discovered) pool. On a
// project with no list yet (first run), default every discovered plugin ON and persist it.
void EditorUI::ApplyProjectPlugins()
{
	auto& mods = nuke::GetModules();
	if (!pluginListLoaded)
	{
		enabledPlugins.clear();
		for (auto& m : mods) enabledPlugins.push_back(m->moduleFile);
		pluginListLoaded = true;
		SaveProject();
	}
	for (auto& m : mods)
	{
		bool want = std::find(enabledPlugins.begin(), enabledPlugins.end(), m->moduleFile) != enabledPlugins.end();
		if (want) nuke::EnablePlugin(m.get());
	}
}

// Rebuild the project's plugin list from what's currently loaded, and persist it.
void EditorUI::SyncEnabledPlugins()
{
	enabledPlugins.clear();
	for (auto& m : nuke::GetModules())
		if (m->loaded) enabledPlugins.push_back(m->moduleFile);
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
		j["editorCamera"]["pos"] = { t.position.x, t.position.y, t.position.z };
		j["editorCamera"]["rot"] = { t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w };
	}
	if (auto sel = AppInstance::GetSingleton()->selectedInHieararchy)
		j["selected"] = sel->GetName();
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
	if (j.contains("selected") && j["selected"].is_string())
		pendingSelect = j["selected"].get<std::string>();
	if (j.contains("editorCamera") && editorCam && editorCam->transform)
	{
		nlohmann::json& jc = j["editorCamera"];
		Transform& t = *editorCam->transform;
		if (jc.contains("pos")) { auto p = jc["pos"]; t.position.x = p[0]; t.position.y = p[1]; t.position.z = p[2]; }
		if (jc.contains("rot")) { auto r = jc["rot"]; t.rotation.x = r[0]; t.rotation.y = r[1]; t.rotation.z = r[2]; t.rotation.w = r[3]; }
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
}
