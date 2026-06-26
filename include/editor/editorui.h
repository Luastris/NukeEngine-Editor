#ifndef EDITORUI_H
#define EDITORUI_H
// Editor UI panels, ported to Dear ImGui 1.92 and the NukeUI module.
//
// All the old plumbing is GONE — it now lives elsewhere:
//   * ImGui context / font / frame (NewFrame/Render) -> NukeUI module
//   * GPU rendering of draw data                      -> renderer via iRender seam
//   * input                                            -> (wired later via iRender callbacks)
//   * OpenGL2 immediate-mode renderer + freeglut       -> deleted
//   * ImGuizmo gizmo                                   -> deferred (lib not in build yet)
// What remains here is just the panels: menu, hierarchy, inspector, about, etc.

#include "imgui.h"
#include "imgui_internal.h"   // BeginViewportSideBar (toolbar attached under the main menu bar)
#include "nukeui.h"           // NukeUI::MergeIconFont
#include "IconsLucide.h"      // ICON_LC_* toolbar icons
#include "ImGuizmo.h"         // transform gizmo (lives in NukeImGui, shares the context)
#include "config.h"
#include "interface/AppInstance.h"
#include "interface/Modular.h"
#include "API/Model/MeshRenderer.h"
#include "API/Model/UnknownComponent.h"
#include "API/Model/resdb.h"   // asset database (meshes by GUID, browser)
#include "import/assimporter.h" // external model import -> native .numesh
#include "API/Model/Prefab.h"   // instantiate .nuprefab assets
#include "reflect/Reflect.h"   // auto-inspector: draw component fields from the schema
#include "API/Model/Time.h"    // per-frame delta/elapsed
#include <boost/container/list.hpp>
#include <boost/bind/bind.hpp>
#include <cstring>
#include <nlohmann/json.hpp>   // editor_state.json (editor-side state, not world state)
#include <boost/filesystem/fstream.hpp>   // boost file streams (project's stack — not std)
#include <map>
#include <string>
#include <vector>
#include <cctype>
#include <cstdio>
#include <algorithm>
#include <boost/filesystem.hpp>   // project content browser (bfs, matching the engine's stack)
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/ext.hpp>   // lookAtLH / perspectiveLH_ZO (match the renderer's LH, z0..1)

using namespace nuke;   // engine API lives in namespace nuke
using namespace std;    // cout/endl (previously leaked from engine headers)

// Native OS "open file" dialog for importing models. Defined in main.cpp (isolates <windows.h>).
// Returns the picked path, or "" if cancelled.
std::string EditorPickModelFile();

// Row-major (renderer) -> column-major (ImGuizmo) 4x4 matrix layout.
static inline void Transpose4(const float* s, float* d)
{
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			d[c * 4 + r] = s[r * 4 + c];
}

class EditorUI
{
private:
	EditorUI() {}
	~EditorUI() {}
	struct NukeWindow* win = nullptr;
	std::shared_ptr<NUKEModule> selectedPlugin = nullptr;
	int  selectedPluginIndex = -1;
	bool freezeWindows = true;
	ImGuiWindowFlags window_flags = 0;
	Camera* editorCam = nullptr;
	uint64_t sceneRTId = 0;   // render target the editor camera draws into
	uint64_t camPreviewRT = 0;          // small RT for the selected camera's preview
	nuke::Camera* previewCam = nullptr; // camera currently retargeted to the preview RT
	std::map<std::string, bool> uiOpen; // persisted CollapsingHeader states (Components + per atom/component)
	std::string pendingSelect;          // atom name to reselect after load (from editor_state.json)
	int  browserView = 0;               // asset browser: 0 Tiles, 1 List, 2 Tree, 3 By Type
	char browserSearch[128] = "";
	bool fMesh = true, fMat = true, fTex = true, fPrefab = true;   // browser type filters
	std::string contentDir = "project/content";   // project content root (imported assets live here)
	std::string browserCwd;                        // current folder shown in the browser
	std::string projectDir  = "project";           // project root
	std::string projectFile = "project/game.nuproj";
	std::string startupWorld = "scene.nuworld";    // from the .nuproj
	std::vector<std::string> enabledPlugins;       // per-project plugin load list (dll names)
	bool pluginListLoaded = false;                 // did the .nuproj specify a plugin list?
	std::vector<std::pair<nuke::NUKEModule*, bool>> pendingPluginToggle;   // applied after the window loop
	float camYaw = 0.0f, camPitch = 0.0f;   // editor camera look angles (radians)
	float gizmoMatrix[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };   // persistent during a gizmo drag
	std::string pieSnapshot;   // scene serialized on Play, restored on Stop (PIE)

public:
	static EditorUI* getSingleton()
	{
		static EditorUI instance;
		return &instance;
	}

	// Open-state for a CollapsingHeader, defaulting to `def` and persisted in uiOpen.
	bool& OpenState(const std::string& key, bool def = true)
	{
		auto it = uiOpen.find(key);
		if (it == uiOpen.end()) it = uiOpen.emplace(key, def).first;
		return it->second;
	}

	// The project manifest (project/game.nuproj): content dir, startup world, plugin load list.
	// Projects have a file (like .sln/.uproject); this is ours, extension .nuproj. The plugin
	// pool is shared (modules/); "plugins" is THIS project's chosen load list (dll names).
	void SaveProject()
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
	void LoadProject()
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
	void ApplyProjectPlugins()
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
	void SyncEnabledPlugins()
	{
		enabledPlugins.clear();
		for (auto& m : nuke::GetModules())
			if (m->loaded) enabledPlugins.push_back(m->moduleFile);
		SaveProject();
	}

	// Editor state (NOT world state) -> project/editor_state.json: camera, selection, which
	// inspector headers are expanded, the browser view/path/filters, and which panels are open.
	void SaveEditorState()
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

	void LoadEditorState()
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

	void SetUp()
	{
		cout << "[editorui]\t\t" << "EditorUI setup (imgui 1.92 / NukeUI)..." << endl;
		ApplyStyle();

		win = &Config::getSingleton()->window;

		// The UI module owns the font atlas, but the APP chooses its font.
		ImGuiIO& io = ImGui::GetIO();
		if (!win->mainFont.empty())
		{
			cout << "[editorui]\t\t" << "Loading font: " << win->mainFont << endl;
			io.Fonts->AddFontFromFileTTF(win->mainFont.c_str(), 19.0f);
		}
		else
		{
			io.Fonts->AddFontDefault();
		}
		// Merge Lucide icons on top so toolbar/panels can use ICON_LC_* glyphs.
		// Lucide glyphs sit high in the line, so nudge them down to centre.
		NukeUI::MergeIconFont("fonts/lucide.ttf", 20.0f, 4.0f);

		InitMenu();

		AppInstance* editor = AppInstance::GetSingleton();
		editor->PushWindow("nukeeditor-about", boost::bind(&EditorUI::winAbout, this));
		editor->PushWindow("nukeeditor-browser", boost::bind(&EditorUI::winBrowser, this));
		editor->PushWindow("nukeeditor-console", boost::bind(&EditorUI::winConsole, this));
		editor->PushWindow("nukeeditor-hierarchy", boost::bind(&EditorUI::winHierarchy, this));
		editor->PushWindow("nukeeditor-inspector", boost::bind(&EditorUI::winInspector, this));
		editor->PushWindow("nukeeditor-render", boost::bind(&EditorUI::winRender, this));
		editor->PushWindow("nukeeditor-plugins", boost::bind(&EditorUI::PluginMGRWindow, this));

		Atom* camObj = editor->currentScene->Get("Editor Camera");
		if (camObj)
			editorCam = camObj->GetComponent<Camera>();
		if (editorCam)
		{
			// Park the editor camera so it looks at the origin (camera control TODO).
			editorCam->transform->position.x = 0; editorCam->transform->position.y = 0; editorCam->transform->position.z = -5;
			editorCam->fov = 60.0f;   // 90 vertical is too wide → strong edge distortion
			// rotation defaults to identity quaternion (looks +Z, at the origin).
		}
		// meshGuid is edited via the Mesh asset picker (combo) below, not as a raw GUID string.
		if (nuke::TypeInfo* ti = nuke::Registry_Find("MeshRenderer"))
			for (nuke::Field& f : ti->fields)
				if (f.name == "meshGuid" || f.name == "matGuid") f.hidden = true;

		LoadProject();   // .nuproj: content dir, startup world, plugin load list (default if missing)

		// Activate the project's chosen plugins from the shared pool (InitModules discovered
		// them already). Types register here (OnLoad) BEFORE the world auto-loads, so a world's
		// components resolve; plugins left off keep their components as inert placeholders.
		ApplyProjectPlugins();

		// Project content folder (imported assets live here). Create it + open the browser there.
		boost::system::error_code ec;
		bfs::create_directories(contentDir, ec);
		browserCwd = contentDir;

		// Load native assets (.numesh) from content/ so meshGuid refs in saved worlds resolve.
		ResDB::getSingleton()->LoadContentDir(contentDir);

		// Editor state (project-tied): camera, selection, inspector + browser + panel state.
		LoadEditorState();
		// Demo geometry via the spawn API so the viewport shows something.
		{
			Atom* cube = new Atom("Cube");
			MeshRenderer* mr = new MeshRenderer();
			cube->AddComponent(mr);
			mr->meshGuid = "builtin:cube";
			mr->mesh = ResDB::getSingleton()->GetMesh("builtin:cube");
			editor->currentScene->Add(cube);
		}
		cout << "[editorui]\t\t" << "EditorUI ready." << endl;
	}

	// NukeEngine dark theme (ported from the old gui.cpp to imgui 1.92 enums).
	void ApplyStyle()
	{
		ImGuiStyle* s = &ImGui::GetStyle();
		s->WindowPadding     = ImVec2(15, 15);
		s->WindowRounding    = 0.0f;
		s->FramePadding      = ImVec2(5, 5);
		s->FrameRounding     = 0.0f;
		s->ItemSpacing       = ImVec2(6, 6);
		s->ItemInnerSpacing  = ImVec2(6, 6);
		s->IndentSpacing     = 25.0f;
		s->ScrollbarSize     = 15.0f;
		s->ScrollbarRounding = 0.0f;
		s->GrabMinSize       = 5.0f;
		s->GrabRounding      = 3.0f;
		ImVec4* c = s->Colors;
		c[ImGuiCol_Text]                 = ImVec4(0.80f, 0.80f, 0.83f, 1.00f);
		c[ImGuiCol_TextDisabled]         = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
		c[ImGuiCol_WindowBg]             = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
		c[ImGuiCol_ChildBg]              = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
		c[ImGuiCol_PopupBg]              = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
		c[ImGuiCol_Border]               = ImVec4(0.80f, 0.80f, 0.80f, 0.48f);
		c[ImGuiCol_FrameBg]              = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		c[ImGuiCol_FrameBgHovered]       = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
		c[ImGuiCol_FrameBgActive]        = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		c[ImGuiCol_TitleBg]              = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		c[ImGuiCol_TitleBgActive]        = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
		c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.20f, 0.58f, 0.55f, 0.25f);
		c[ImGuiCol_MenuBarBg]            = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		c[ImGuiCol_ScrollbarBg]          = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.80f, 0.80f, 0.83f, 0.31f);
		c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
		c[ImGuiCol_CheckMark]            = ImVec4(0.80f, 0.80f, 0.83f, 0.31f);
		c[ImGuiCol_SliderGrab]           = ImVec4(0.80f, 0.80f, 0.83f, 0.31f);
		c[ImGuiCol_SliderGrabActive]     = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
		c[ImGuiCol_Button]               = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		c[ImGuiCol_ButtonHovered]        = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
		c[ImGuiCol_ButtonActive]         = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		c[ImGuiCol_Header]               = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		c[ImGuiCol_HeaderHovered]        = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		c[ImGuiCol_HeaderActive]         = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
		c[ImGuiCol_Separator]            = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		c[ImGuiCol_SeparatorHovered]     = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
		c[ImGuiCol_SeparatorActive]      = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		c[ImGuiCol_ResizeGrip]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
		c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		c[ImGuiCol_ResizeGripActive]     = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
		c[ImGuiCol_PlotLines]            = ImVec4(0.40f, 0.39f, 0.38f, 0.63f);
		c[ImGuiCol_PlotLinesHovered]     = ImVec4(0.25f, 1.00f, 0.00f, 1.00f);
		c[ImGuiCol_PlotHistogram]        = ImVec4(0.40f, 0.39f, 0.38f, 0.63f);
		c[ImGuiCol_PlotHistogramHovered] = ImVec4(0.25f, 1.00f, 0.00f, 1.00f);
		c[ImGuiCol_TextSelectedBg]       = ImVec4(0.25f, 1.00f, 0.00f, 0.43f);
		c[ImGuiCol_ModalWindowDimBg]     = ImVec4(1.00f, 0.98f, 0.95f, 0.73f);
		// Docking-branch tab / dock colors (kept dark so the light label text reads).
		c[ImGuiCol_Tab]                       = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		c[ImGuiCol_TabHovered]                = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
		c[ImGuiCol_TabSelected]               = ImVec4(0.18f, 0.17f, 0.22f, 1.00f);
		c[ImGuiCol_TabSelectedOverline]       = ImVec4(0.25f, 1.00f, 0.00f, 0.50f);
		c[ImGuiCol_TabDimmed]                 = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
		c[ImGuiCol_TabDimmedSelected]         = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
		c[ImGuiCol_DockingPreview]            = ImVec4(0.25f, 1.00f, 0.00f, 0.30f);
		c[ImGuiCol_DockingEmptyBg]            = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
	}

	// ---- menu ----
	void InitMenu()
	{
		MenuStrip* mstrip = AppInstance::GetSingleton()->menuStrip = new MenuStrip();
		mstrip->AddItem("Tools/", "Plugin manager", TogglePluginMGR);
	}

	static void TogglePluginMGR()
	{
		Config::getSingleton()->window.plugmgr = !Config::getSingleton()->window.plugmgr;
	}

	bool EditorSubMenu(MenuItem* item)
	{
		if (item->subitems.size() > 0)
		{
			if (ImGui::BeginMenu(item->name.c_str()))
			{
				for (auto subitem : item->subitems)
					EditorSubMenu(subitem);
				ImGui::EndMenu();
			}
		}
		else if (item->callback)
		{
			if (ImGui::MenuItem(item->name.c_str()))
				item->callback();
		}
		return true;
	}

	void EditorMenu()
	{
		if (ImGui::BeginMainMenuBar())
		{
			if (ImGui::BeginMenu("File"))
			{
				if (ImGui::MenuItem("New")) {}
				if (ImGui::MenuItem("Open", "Ctrl+O"))
				{
					AppInstance::GetSingleton()->selectedInHieararchy = nullptr;
					AppInstance::GetSingleton()->currentScene->LoadFromFile("scene.nuworld");
				}
				if (ImGui::MenuItem("Save", "Ctrl+S"))
					AppInstance::GetSingleton()->currentScene->SaveToFile("scene.nuworld");
				ImGui::Separator();
				if (ImGui::MenuItem("Quit", "Alt+F4")) {}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Edit"))
			{
				if (ImGui::MenuItem("Undo", "CTRL+Z")) {}
				if (ImGui::MenuItem("Redo", "CTRL+Y", false, false)) {}
				ImGui::Separator();
				if (ImGui::MenuItem("Cut", "CTRL+X")) {}
				if (ImGui::MenuItem("Copy", "CTRL+C")) {}
				if (ImGui::MenuItem("Paste", "CTRL+V")) {}
				ImGui::EndMenu();
			}
			for (auto rootElement : AppInstance::GetSingleton()->menuStrip->strip)
				EditorSubMenu(rootElement);
			if (ImGui::BeginMenu("Window"))
			{
				ImGui::MenuItem("Freeze windows", "F8", &freezeWindows);
				ImGui::Separator();
				ImGui::MenuItem("Hierarchy", nullptr, &win->hierarchy);
				ImGui::MenuItem("Console", nullptr, &win->console);
				ImGui::MenuItem("Browser", nullptr, &win->browser);
				ImGui::MenuItem("Inspector", nullptr, &win->inspector);
				ImGui::MenuItem("Render", nullptr, &win->render);
				ImGui::MenuItem("Plugins", nullptr, &win->plugmgr);
				ImGui::MenuItem("About", nullptr, &win->about);
				ImGui::EndMenu();
			}
			ImGui::EndMainMenuBar();
		}
	}

	// ---- panels ----
	void DisplayRecursiveAtomHierarchy(bc::list<Atom*>& gos)
	{
		int i = 0;
		for (auto go : gos)
		{
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
			if (AppInstance::GetSingleton()->selectedInHieararchy == go)
				flags |= ImGuiTreeNodeFlags_Selected;
			if (go->children.size() == 0)
				flags |= ImGuiTreeNodeFlags_Leaf;

			bool opened = ImGui::TreeNodeEx((void*)(intptr_t)i, flags, "%s", go->GetName().c_str());
			if (ImGui::IsItemClicked())
				AppInstance::GetSingleton()->selectedInHieararchy = go;
			if (opened)
			{
				if (go->children.size() > 0)
					DisplayRecursiveAtomHierarchy(go->children);
				ImGui::TreePop();
			}
			++i;
		}
	}

	void winHierarchy()
	{
		if (!win->hierarchy) return;
		ImGui::Begin("Hierarchy", &win->hierarchy, window_flags);
		DisplayRecursiveAtomHierarchy(AppInstance::GetSingleton()->currentScene->GetHierarchy());
		ImGui::End();
	}

	void CamComponent(Camera* cam)
	{
		if (cam->renderer)
		{
			ImGui::InputInt("Width", &cam->renderer->width);
			ImGui::InputInt("Height", &cam->renderer->height);
		}
		float fov = cam->fov * (float)M_PI / 180.f;
		ImGui::SliderAngle("FOV", &fov, 0, 180);
		cam->fov = fov * 180.f / (float)M_PI;
		ImGui::DragFloat("Near", &cam->_near);
		ImGui::DragFloat("Far", &cam->_far);
		ImGui::Checkbox("Free mode", &cam->freeMode);
	}

	// Auto-draw a reflected object's fields from its schema (component inspector).
	bool DrawFields(void* obj, nuke::TypeInfo* ti)
	{
		if (!ti) return false;
		bool changed = false;
		for (const nuke::Field& f : ti->fields)
		{
			if (f.hidden) continue;   // serialized but not shown (e.g. script props JSON)
			void* a = f.addr(obj);
			const char* n = f.name.c_str();
			switch (f.type)
			{
			case nuke::FT::Bool:   changed |= ImGui::Checkbox(n, (bool*)a); break;
			case nuke::FT::Int:    changed |= ImGui::InputInt(n, (int*)a); break;
			case nuke::FT::Float:  changed |= ImGui::DragFloat(n, (float*)a, 0.05f); break;
			case nuke::FT::Double: changed |= ImGui::InputDouble(n, (double*)a); break;
			case nuke::FT::String:
			{
				std::string* s = (std::string*)a;
				char buf[256]; strncpy(buf, s->c_str(), 255); buf[255] = 0;
				if (ImGui::InputText(n, buf, sizeof(buf))) { *s = buf; changed = true; }
				break;
			}
			case nuke::FT::Vec2:
			{
				Vector2* v = (Vector2*)a; float t[2] = { (float)v->x, (float)v->y };
				if (ImGui::DragFloat2(n, t, 0.05f)) { v->x = t[0]; v->y = t[1]; changed = true; }
				break;
			}
			case nuke::FT::Vec3:
			{
				Vector3* v = (Vector3*)a; float t[3] = { (float)v->x, (float)v->y, (float)v->z };
				if (ImGui::DragFloat3(n, t, 0.05f)) { v->x = t[0]; v->y = t[1]; v->z = t[2]; changed = true; }
				break;
			}
			case nuke::FT::Vec4:
			case nuke::FT::Quat:
			{
				Vector4* v = (Vector4*)a; float t[4] = { (float)v->x, (float)v->y, (float)v->z, (float)v->w };
				if (ImGui::DragFloat4(n, t, 0.05f)) { v->x = t[0]; v->y = t[1]; v->z = t[2]; v->w = t[3]; changed = true; }
				break;
			}
			default: break;
			}
		}
		return changed;
	}

	// Draw a component's dynamic props (e.g. a Lua script's exported vars). The component
	// supplies data only (DynamicProps/SetDynamicProp); all UI lives here in the editor.
	void DrawDynamicProps(nuke::Component* cmp)
	{
		std::vector<nuke::DynProp> props = cmp->DynamicProps();
		if (props.empty()) return;
		ImGui::Separator();
		ImGui::Text("Script Props");
		for (nuke::DynProp& p : props)
		{
			bool edited = false;
			nuke::NukeVar nv = p.value;
			switch (p.value.kind)
			{
			case nuke::NukeVar::Kind::Number:
			{
				float f = (float)p.value.num;
				if (ImGui::DragFloat(p.name.c_str(), &f, 0.05f)) { nv.num = f; edited = true; }
				break;
			}
			case nuke::NukeVar::Kind::Bool:
			{
				bool b = p.value.b;
				if (ImGui::Checkbox(p.name.c_str(), &b)) { nv.b = b; edited = true; }
				break;
			}
			case nuke::NukeVar::Kind::String:
			{
				char buf[256]; strncpy(buf, p.value.str.c_str(), 255); buf[255] = 0;
				if (ImGui::InputText(p.name.c_str(), buf, sizeof(buf))) { nv.str = buf; edited = true; }
				break;
			}
			default: continue;
			}
			ImGui::SameLine();
			if (ImGui::SmallButton(("Reset##" + p.name).c_str())) { nv = p.def; edited = true; }
			if (edited) cmp->SetDynamicProp(p.name, nv);
		}
	}

	// Vector3 editor with colored X/Y/Z axis labels (Unity-style). True if edited.
	bool EditV3(const char* rowLabel, double v[3])
	{
		static const char* ax[3] = { "X", "Y", "Z" };
		static const ImVec4 col[3] = { ImVec4(0.86f,0.34f,0.34f,1.0f), ImVec4(0.42f,0.74f,0.36f,1.0f), ImVec4(0.36f,0.55f,0.92f,1.0f) };
		bool ch = false;
		ImGui::PushID(rowLabel);
		float w = (ImGui::GetContentRegionAvail().x - 150.0f) / 3.0f;
		if (w < 36.0f) w = 36.0f;
		for (int i = 0; i < 3; ++i)
		{
			ImGui::PushID(i);
			ImGui::TextColored(col[i], "%s", ax[i]);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(w);
			ch |= ImGui::InputDouble("##v", &v[i], 0.0, 0.0, "%.3f");
			ImGui::SameLine();
			ImGui::PopID();
		}
		ImGui::TextUnformatted(rowLabel);
		ImGui::PopID();
		return ch;
	}

	void winInspector()
	{
		if (!win->inspector) return;
		ImGui::Begin("Inspector", &win->inspector, window_flags);
		if (auto sltd = AppInstance::GetSingleton()->selectedInHieararchy)
		{
			char name[128];
			strncpy(name, sltd->GetName().c_str(), 127); name[127] = 0;
			if (ImGui::InputText("Name", name, 128)) sltd->SetName(name);

			ImGui::SeparatorText("Transform");
			Transform& t = sltd->GetTransform();
			double p[3] = { t.position.x, t.position.y, t.position.z };
			if (EditV3("Position", p))
			{ t.position.x = p[0]; t.position.y = p[1]; t.position.z = p[2]; }
			Vector3 er = t.EulerDeg();
			double r[3] = { er.x, er.y, er.z };
			if (EditV3("Rotation (deg)", r))
				t.SetEulerDeg(Vector3(r[0], r[1], r[2]));
			double s[3] = { t.scale.x, t.scale.y, t.scale.z };
			if (EditV3("Scale", s))
			{ t.scale.x = s[0]; t.scale.y = s[1]; t.scale.z = s[2]; }

			// Expand/Collapse all — affects the Components category AND every component header.
			std::string atomName = sltd->GetName();
			{
				int force = -1;
				if (ImGui::SmallButton("Expand All"))   force = 1;
				ImGui::SameLine();
				if (ImGui::SmallButton("Collapse All")) force = 0;
				if (force != -1)
				{
					OpenState("Components") = (force == 1);
					for (auto cmp : sltd->components)
						OpenState(atomName + "/" + cmp->name) = (force == 1);
				}
			}

			// Headers are driven by uiOpen (persisted): set state -> draw -> read the toggle back.
			bool& compsOpen = OpenState("Components");
			ImGui::SetNextItemOpen(compsOpen);
			compsOpen = ImGui::CollapsingHeader("Components");
			if (compsOpen)
			{
				for (auto cmp : sltd->components)
				{
					ImGui::PushID(cmp);   // unique ID per component (avoid "Enabled" ID clashes)
					nuke::UnknownComponent* uc = dynamic_cast<nuke::UnknownComponent*>(cmp);
					std::string label = uc ? (uc->typeName.empty() ? std::string("Unknown") : uc->typeName)
					                       : std::string(cmp->name);
					bool& st = OpenState(atomName + "/" + label);
					ImGui::SetNextItemOpen(st);
					std::string hdr = uc ? (label + "  (plugin not loaded)") : label;
					st = ImGui::CollapsingHeader(hdr.c_str());
					if (st)
					{
						if (uc)
						{
							// Component whose plugin isn't loaded: kept inert, data preserved.
							ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.20f, 1.0f), ICON_LC_PLUG " Requires plugin: %s",
								uc->requiredPlugin.empty() ? "(unknown)" : uc->requiredPlugin.c_str());
							ImGui::TextDisabled("Enable it in the Plugins window to restore this component.");
						}
						else
						{
							ImGui::Checkbox("Enabled", &cmp->enabled);
							if (nuke::TypeInfo* cti = cmp->GetType())   // which plugin provides this type
							{
								const char* pl = nuke::PluginForType(cti->name);
								if (pl && pl[0]) ImGui::TextDisabled(ICON_LC_PLUG " %s", pl);
							}
							DrawFields(cmp, cmp->GetType());   // auto fields from [[nuke::prop]] schema
							DrawDynamicProps(cmp);             // dynamic props (e.g. Lua script vars)

							// Mesh asset picker (replaces the raw meshGuid string field).
							if (auto* mr = dynamic_cast<nuke::MeshRenderer*>(cmp))
							{
								ResDB* db = ResDB::getSingleton();
								const char* cur = mr->mesh ? mr->mesh->name
								                : (mr->meshGuid.empty() ? "(none)" : mr->meshGuid.c_str());
								if (ImGui::BeginCombo("Mesh", cur))
								{
									for (Mesh* msh : db->meshes)
										if (msh && ImGui::Selectable(msh->name, mr->meshGuid == msh->guid))
										{
											mr->meshGuid = msh->guid;
											mr->mesh     = msh;
										}
									ImGui::EndCombo();
								}

								// Material asset picker (replaces the raw matGuid string field).
								const char* curMat = mr->mat
								    ? (mr->mat->matName.empty() ? mr->mat->guid.c_str() : mr->mat->matName.c_str())
								    : (mr->matGuid.empty() ? "(none)" : mr->matGuid.c_str());
								if (ImGui::BeginCombo("Material", curMat))
								{
									for (Material* mt : db->materials)
										if (mt)
										{
											const char* lbl = mt->matName.empty() ? mt->guid.c_str() : mt->matName.c_str();
											if (ImGui::Selectable(lbl, mr->matGuid == mt->guid))
											{
												mr->matGuid = mt->guid;
												mr->mat     = mt;
											}
										}
									ImGui::EndCombo();
								}
							}
						}
					}
					ImGui::PopID();
				}
			}

			// Add any registered, create-able Component type (incl. ones added by plugins).
			ImGui::Separator();
			if (ImGui::Button("Add Component"))
				ImGui::OpenPopup("addcomp");
			if (ImGui::BeginPopup("addcomp"))
			{
				for (nuke::TypeInfo* ti : nuke::Registry_All())
				{
					if (!ti->create || ti->base != "Component")
						continue;
					if (ImGui::MenuItem(ti->name.c_str()))
						sltd->AddComponent((nuke::Component*)ti->create());
				}
				ImGui::EndPopup();
			}
		}
		else
		{
			ImGui::TextWrapped("Select an object in the Hierarchy.");
		}
		ImGui::End();
	}

	void winRender()
	{
		if (!win->render) return;
		ImGui::Begin("Render", &win->render, window_flags);

		// Hotkeys (when focused, not typing, and NOT flying with RMB): Q/W/E/R = tools, X = World/Local.
		if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
		{
			AppInstance* a = AppInstance::GetSingleton();
			if (ImGui::IsKeyPressed(ImGuiKey_Q)) a->manipulationMode = 0;
			if (ImGui::IsKeyPressed(ImGuiKey_W)) a->manipulationMode = 1;
			if (ImGui::IsKeyPressed(ImGuiKey_E)) a->manipulationMode = 2;
			if (ImGui::IsKeyPressed(ImGuiKey_R)) a->manipulationMode = 3;
			if (ImGui::IsKeyPressed(ImGuiKey_X)) a->manipulationWorld = !a->manipulationWorld;
		}

		ImVec2 avail = ImGui::GetContentRegionAvail();
		iRender* r = AppInstance::GetSingleton()->render;
		if (r && avail.x >= 1.0f && avail.y >= 1.0f)
		{
			if (sceneRTId == 0)
			{
				sceneRTId = r->createRenderTarget((int)avail.x, (int)avail.y);
				if (editorCam) editorCam->renderTarget = sceneRTId; // editor cam draws here
			}
			else
			{
				r->resizeRenderTarget(sceneRTId, (int)avail.x, (int)avail.y); // match the panel
			}
			uint64_t tex = r->getRenderTargetTexture(sceneRTId);
			if (tex)
				ImGui::Image((ImTextureID)tex, avail); // the live scene viewport
			else
				ImGui::TextDisabled("No scene texture.");

			// --- selected-camera preview: a small overlay in the viewport's bottom-right ---
			if (previewCam) { previewCam->renderTarget = 0; previewCam = nullptr; }   // release last frame's
			{
				Atom* sel = AppInstance::GetSingleton()->selectedInHieararchy;
				Camera* selCam = sel ? sel->GetComponent<Camera>() : nullptr;
				if (tex && selCam && selCam != editorCam)
				{
					if (camPreviewRT == 0) camPreviewRT = r->createRenderTarget(256, 144);
					selCam->renderTarget = camPreviewRT;   // World::Render draws it here next pass
					previewCam = selCam;

					ImVec2 imax = ImGui::GetItemRectMax();  // bottom-right of the scene image
					ImVec2 pv(256, 144), pad(12, 12);
					ImVec2 p0(imax.x - pv.x - pad.x, imax.y - pv.y - pad.y);
					ImVec2 p1(p0.x + pv.x, p0.y + pv.y);
					ImDrawList* dl = ImGui::GetWindowDrawList();
					dl->AddRectFilled(ImVec2(p0.x - 2, p0.y - 16), ImVec2(p1.x + 2, p1.y + 2), IM_COL32(15, 15, 15, 220));
					if (uint64_t ptex = r->getRenderTargetTexture(camPreviewRT))
						dl->AddImage((ImTextureID)ptex, p0, p1);
					dl->AddRect(p0, p1, IM_COL32(180, 180, 180, 255));
					dl->AddText(ImVec2(p0.x + 3, p0.y - 15), IM_COL32_WHITE, sel->GetName().c_str());
				}
			}

			// Transform gizmo over the selected object (only when a manip tool is active).
			{
				AppInstance* gapp = AppInstance::GetSingleton();
				Atom* gsel = gapp->selectedInHieararchy;
				if (gsel && editorCam && gapp->manipulationMode != 0)
				{
					ImGuizmo::SetOrthographic(false);
					ImGuizmo::SetDrawlist();
					ImVec2 grmin = ImGui::GetItemRectMin();
					ImVec2 gsz   = ImGui::GetItemRectSize();
					ImGuizmo::SetRect(grmin.x, grmin.y, gsz.x, gsz.y);

					// Build view/proj on the editor side in glm column-major, in the SAME
					// convention as the renderer (Diligent: left-handed, depth 0..1), so the
					// gizmo overlays the rendered image and ImGuizmo gets valid input.
					// Feed ImGuizmo the renderer's EXACT view/proj (Diligent: row-major, LH,
					// depth 0..1) so screen<->world matches the image precisely — per-axis
					// scale needs an exact ray, and ImGuizmo then detects handedness right.
					float gview[16], gproj[16];
					{
						Transform* gcam = editorCam->transform;
						Vector3 ge = gcam->globalPosition();
						Vector3 gf = gcam->direction(), gu = gcam->up();
						float gaspect = (gsz.y > 0.0f) ? gsz.x / gsz.y : 1.0f;
						float gfovy   = (float)editorCam->fov * 0.01745329252f;
						glm::mat4 gv = glm::lookAtLH(
							glm::vec3((float)ge.x, (float)ge.y, (float)ge.z),
							glm::vec3((float)(ge.x + gf.x), (float)(ge.y + gf.y), (float)(ge.z + gf.z)),
							glm::vec3((float)gu.x, (float)gu.y, (float)gu.z));
						glm::mat4 gp = glm::perspectiveLH_ZO(gfovy, gaspect, editorCam->_near, editorCam->_far);
						// ImGuizmo uses ROW convention, glm uses COLUMN — transpose all matrices.
						memcpy(gview, glm::value_ptr(gv), sizeof(gview));   // glm passed directly (no transpose)
						memcpy(gproj, glm::value_ptr(gp), sizeof(gproj));
					}

					Transform& gtt = gsel->GetTransform();
					glm::quat gq((float)gtt.rotation.w, (float)gtt.rotation.x, (float)gtt.rotation.y, (float)gtt.rotation.z);
					glm::mat4 gworld = glm::translate(glm::mat4(1.0f), glm::vec3((float)gtt.position.x, (float)gtt.position.y, (float)gtt.position.z))
					                 * glm::mat4_cast(gq)
					                 * glm::scale(glm::mat4(1.0f), glm::vec3((float)gtt.scale.x, (float)gtt.scale.y, (float)gtt.scale.z));
					(void)gworld;   // model is built via ImGuizmo's own compose below
					if (!ImGuizmo::IsUsing())   // resync from the object only when NOT dragging
					{
						// Build the model with ImGuizmo's OWN compose so it's in exactly the
						// convention ImGuizmo expects — this is what makes per-axis scale work.
						Vector3 ep = gtt.position, ee = gtt.EulerDeg(), es = gtt.scale;
						float t3[3] = { (float)ep.x, (float)ep.y, (float)ep.z };
						float r3[3] = { (float)ee.x, (float)ee.y, (float)ee.z };
						float s3[3] = { (float)es.x, (float)es.y, (float)es.z };
						ImGuizmo::RecomposeMatrixFromComponents(t3, r3, s3, gizmoMatrix);
					}

					ImGuizmo::OPERATION gop = (gapp->manipulationMode == 1) ? ImGuizmo::TRANSLATE
					                        : (gapp->manipulationMode == 2) ? ImGuizmo::ROTATE
					                                                        : ImGuizmo::SCALE;
					ImGuizmo::MODE gmode = (gop != ImGuizmo::SCALE && gapp->manipulationWorld != 0) ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
					float gsnapv   = (gop == ImGuizmo::TRANSLATE) ? 0.5f : (gop == ImGuizmo::ROTATE) ? 15.0f : 0.1f;
					float gsnap[3] = { gsnapv, gsnapv, gsnapv };
					float* gsnapPtr = ImGui::GetIO().KeyCtrl ? gsnap : nullptr;   // hold Ctrl to snap
					ImGuizmo::Manipulate(gview, gproj, gop, gmode, gizmoMatrix, nullptr, gsnapPtr);
					if (ImGuizmo::IsUsing())
					{
						float gtr[3], gro[3], gsc[3];
						ImGuizmo::DecomposeMatrixToComponents(gizmoMatrix, gtr, gro, gsc);
						bool gok = true;
						for (int i = 0; i < 3; ++i)
							gok = gok && std::isfinite(gtr[i]) && std::isfinite(gro[i]) && std::isfinite(gsc[i]);
						if (gok)   // skip degenerate results (e.g. scale dragged through zero -> NaN)
						{
							for (int i = 0; i < 3; ++i)
								if (fabsf(gsc[i]) < 1e-3f) gsc[i] = (gsc[i] < 0.0f) ? -1e-3f : 1e-3f;
							gtt.position = Vector3(gtr[0], gtr[1], gtr[2]);
							gtt.SetEulerDeg(Vector3(gro[0], gro[1], gro[2]));
							gtt.scale = Vector3(gsc[0], gsc[1], gsc[2]);
						}
					}
				}
			}

			// Viewport camera control (while hovering the image):
			//   RMB drag = orbit/look, MMB drag = pan, wheel = dolly.
			if (editorCam && editorCam->transform && ImGui::IsItemHovered())
			{
				ImGuiIO& io = ImGui::GetIO();
				Transform* t = editorCam->transform;
				const float rotSpeed = 0.005f, panSpeed = 0.01f, zoomSpeed = 0.5f;

				// Left-click: pick the object under the cursor (null = deselect).
				// Skip if the gizmo is being interacted with, so dragging it doesn't deselect.
				if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsUsing() && !ImGuizmo::IsOver())
				{
					ImVec2 rmin = ImGui::GetItemRectMin();
					ImVec2 sz   = ImGui::GetItemRectSize();
					ImVec2 mp   = io.MousePos;
					float ndcx = ((mp.x - rmin.x) / sz.x) * 2.0f - 1.0f;
					float ndcy = 1.0f - ((mp.y - rmin.y) / sz.y) * 2.0f;
					Vector3 o = t->globalPosition();
					Vector3 f = t->direction(), rr = t->right(), uu = t->up();
					float aspect = (sz.y > 0.0f) ? sz.x / sz.y : 1.0f;
					float thf = tanf((float)editorCam->fov * 0.5f * 0.01745329252f);
					Vector3 dir(f.x + ndcx * thf * aspect * rr.x + ndcy * thf * uu.x,
					            f.y + ndcx * thf * aspect * rr.y + ndcy * thf * uu.y,
					            f.z + ndcx * thf * aspect * rr.z + ndcy * thf * uu.z);
					AppInstance::GetSingleton()->selectedInHieararchy =
						AppInstance::GetSingleton()->currentScene->Pick(o, dir);
				}

				if (ImGui::IsMouseDragging(ImGuiMouseButton_Right))
				{
					camYaw   += io.MouseDelta.x * rotSpeed;
					camPitch += io.MouseDelta.y * rotSpeed;
					const float lim = 1.55f; // ~89deg pitch clamp
					if (camPitch >  lim) camPitch =  lim;
					if (camPitch < -lim) camPitch = -lim;
					t->SetEulerDeg(Vector3(camPitch * 57.29578f, camYaw * 57.29578f, 0.0f));
				}
				if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
				{
					t->position += t->right() * (double)(-io.MouseDelta.x * panSpeed)
					             + t->up()    * (double)( io.MouseDelta.y * panSpeed);
				}
				if (io.MouseWheel != 0.0f)
					t->position += t->direction() * (double)(io.MouseWheel * zoomSpeed);

				// Free-flight: hold RMB + WASD (Q/E = down/up, Shift = faster), Unity/UE-style.
				if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
				{
					float fly = 5.0f * io.DeltaTime;
					if (io.KeyShift) fly *= 3.0f;
					if (ImGui::IsKeyDown(ImGuiKey_W)) t->position += t->direction() * (double) fly;
					if (ImGui::IsKeyDown(ImGuiKey_S)) t->position += t->direction() * (double)-fly;
					if (ImGui::IsKeyDown(ImGuiKey_D)) t->position += t->right()     * (double) fly;
					if (ImGui::IsKeyDown(ImGuiKey_A)) t->position += t->right()     * (double)-fly;
					if (ImGui::IsKeyDown(ImGuiKey_E)) t->position += t->up()        * (double) fly;
					if (ImGui::IsKeyDown(ImGuiKey_Q)) t->position += t->up()        * (double)-fly;
				}
			}
		}
		ImGui::End();
	}

	void winAbout()
	{
		if (!win->about) return;
		ImGui::Begin("About", &win->about, window_flags);
		ImGui::TextWrapped("NukeEngine - free, modular game engine. Renderer (Diligent) and UI (ImGui) "
		                   "are loaded as independent modules and communicate only through a neutral seam.");
		ImGui::End();
	}

	void winConsole()
	{
		if (!win->console) return;
		ImGui::Begin("Console", &win->console, window_flags);
		ImGui::End();
	}

	// Icon for a (lowercased) file extension.
	const char* ExtIcon(const std::string& ext)
	{
		if (ext == ".numesh") return ICON_LC_BOX;
		if (ext == ".numat")  return ICON_LC_PALETTE;
		if (ext == ".nutex" || ext == ".png" || ext == ".jpg" || ext == ".jpeg") return ICON_LC_IMAGE;
		if (ext == ".nuprefab") return ICON_LC_PACKAGE;
		if (ext == ".nuworld")  return ICON_LC_GLOBE;
		if (ext == ".lua")      return ICON_LC_FILE_CODE;
		return ICON_LC_FILE;
	}
	// Whether a file of this extension passes the current type filters.
	bool ExtVisible(const std::string& ext)
	{
		if (ext == ".numesh") return fMesh;
		if (ext == ".numat")  return fMat;
		if (ext == ".nutex" || ext == ".png" || ext == ".jpg" || ext == ".jpeg") return fTex;
		if (ext == ".nuprefab") return fPrefab;
		return true;   // scripts, worlds, unknown — always shown
	}
	bool SearchMatch(const std::string& name)
	{
		std::string q = browserSearch; for (char& c : q) c = (char)tolower((unsigned char)c);
		if (q.empty()) return true;
		std::string s = name; for (char& c : s) c = (char)tolower((unsigned char)c);
		return s.find(q) != std::string::npos;
	}

	// Recursive folder tree (Tree mode), rooted at the project content folder.
	void BrowserTree(const std::string& dir)
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
	void InstantiatePrefab(const std::string& path)
	{
		if (Atom* a = nuke::LoadPrefab(path))
		{
			AppInstance* app = AppInstance::GetSingleton();
			app->currentScene->Add(a);
			app->selectedInHieararchy = a;
			cout << "[editor]\tinstantiated prefab " << path << endl;
		}
	}

	void winBrowser()
	{
		if (!win->browser) return;
		ImGui::Begin("Browser", &win->browser, window_flags);

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
		if (ImGui::BeginPopup("bfilters"))
		{
			ImGui::Checkbox("Meshes", &fMesh);    ImGui::Checkbox("Materials", &fMat);
			ImGui::Checkbox("Textures", &fTex);   ImGui::Checkbox("Prefabs", &fPrefab);
			ImGui::EndPopup();
		}

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
		if (ImGui::Button(ICON_LC_CORNER_LEFT_UP "##up") && !atRoot)
			browserCwd = cwd.parent_path().string();
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
				if (ImGui::Button(e.icon, ImVec2(64, 64)) && e.isDir) browserCwd = e.path;
				if (!e.isDir && e.ext == ".nuprefab" && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
					InstantiatePrefab(e.path);
				char nm[24]; snprintf(nm, sizeof(nm), "%.20s", e.name.c_str());
				ImGui::TextUnformatted(nm);
				ImGui::EndGroup();
				ImGui::PopID();
				if (++i % per != 0) ImGui::SameLine();
			}
		}
		else                             // List
		{
			for (FEntry& e : entries)
				if (ImGui::Selectable((std::string(e.icon) + "  " + e.name).c_str(), false,
				                      ImGuiSelectableFlags_AllowDoubleClick)
				    && ImGui::IsMouseDoubleClicked(0))
				{
					if (e.isDir)                       browserCwd = e.path;
					else if (e.ext == ".nuprefab")     InstantiatePrefab(e.path);
				}
		}
		ImGui::End();
	}

	void PluginMGRWindow()
	{
		if (!win->plugmgr) return;
		if (ImGui::Begin("Plugins", &win->plugmgr, window_flags))
		{
			// Left: the list of loaded plugins.
			ImGui::BeginChild("pluglist", ImVec2(180, 0), ImGuiChildFlags_Borders);
			int idx = 0;
			for (auto& mod : nuke::GetModules())   // shared pool, single instance in the engine DLL
			{
				ImGui::PushID(idx);
				bool on = mod->loaded;
				if (ImGui::Checkbox("##en", &on))   // load / unload (applied + persisted after the frame)
					pendingPluginToggle.push_back({ mod.get(), on });
				ImGui::SameLine();
				bool sel = (selectedPluginIndex == idx);
				if (!mod->loaded) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				if (ImGui::Selectable(mod->title, sel))
				{
					selectedPluginIndex = idx;
					selectedPlugin = mod;
				}
				if (!mod->loaded) ImGui::PopStyleColor();
				ImGui::PopID();
				++idx;
			}
			ImGui::EndChild();

			ImGui::SameLine();

			// Right: selected plugin details + its inline settings panel.
			ImGui::BeginChild("plugdetails", ImVec2(0, 0));
			if (selectedPlugin)
			{
				ImGui::TextUnformatted(selectedPlugin->title);
				ImGui::TextUnformatted(selectedPlugin->author);
				ImGui::TextUnformatted(selectedPlugin->version);
				ImGui::TextDisabled("%s", selectedPlugin->moduleFile.c_str());
				ImGui::TextWrapped("%s", selectedPlugin->description);

				bool on = selectedPlugin->loaded;
				if (ImGui::Checkbox("Loaded for this project", &on))
					pendingPluginToggle.push_back({ selectedPlugin.get(), on });
				if (selectedPlugin->loaded && selectedPlugin->HasSettings())
				{
					ImGui::SeparatorText("Settings");
					selectedPlugin->Settings();   // plugin draws its settings inline (a panel here)
				}
			}
			else
			{
				ImGui::TextWrapped("Select a plugin on the left. To install one, put its DLL in the `modules` directory.");
			}
			ImGui::EndChild();
		}
		ImGui::End();
	}

	// ---- toolbar ----
	// A flat button that stays highlighted while `active` (radio/toggle look).
	bool ToolBtn(const char* icon, const char* tip, bool active, float w)
	{
		if (active)
		{
			ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.42f, 0.30f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.55f, 0.38f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.30f, 0.65f, 0.45f, 1.0f));
		}
		bool clicked = ImGui::Button(icon, ImVec2(w, 0));
		if (active) ImGui::PopStyleColor(3);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
		return clicked;
	}

	void SpawnEmpty()
	{
		AppInstance* app = AppInstance::GetSingleton();
		Atom* go = new Atom("Empty");
		app->currentScene->Add(go);
		app->selectedInHieararchy = go;
	}
	void SpawnPrimitive(const char* atomName, const char* guid)
	{
		AppInstance* app = AppInstance::GetSingleton();
		Atom* go = new Atom(atomName);
		MeshRenderer* mr = new MeshRenderer();
		go->AddComponent(mr);
		mr->meshGuid = guid;
		mr->mesh = ResDB::getSingleton()->GetMesh(guid);
		app->currentScene->Add(go);
		app->selectedInHieararchy = go;
	}
	void SpawnCube() { SpawnPrimitive("Cube", "builtin:cube"); }
	void SpawnCamera()
	{
		AppInstance* app = AppInstance::GetSingleton();
		Atom* go = new Atom("Camera");
		Camera* c = new Camera();
		c->renderer = app->render;          // share the active renderer (avoids re-init / null deref)
		go->AddComponent(c);
		app->currentScene->Add(go);
		app->selectedInHieararchy = go;
	}

	// Second row under the main menu: tools (left) | PIE (center) | viewport mode (right).
	void Toolbar()
	{
		ImGuiViewport* vp = ImGui::GetMainViewport();
		// Keep WindowPadding pushed across the WHOLE window scope (Begin..End) so the
		// top and bottom padding match — otherwise the row sticks to the top edge.
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
		float barH = ImGui::GetFrameHeight() + 12.0f;
		ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
		bool open = ImGui::BeginViewportSideBar("##nuke-toolbar", vp, ImGuiDir_Up, barH, flags);
		if (open)
		{
			AppInstance* app = AppInstance::GetSingleton();
			ImGuiStyle& st = ImGui::GetStyle();
			const float bw = ImGui::GetFrameHeight();   // square icon buttons

			// LEFT — manipulation tools + create
			if (ToolBtn(ICON_LC_MOUSE_POINTER, "Select", app->manipulationMode == 0, bw)) app->manipulationMode = 0; ImGui::SameLine();
			if (ToolBtn(ICON_LC_MOVE,          "Move",   app->manipulationMode == 1, bw)) app->manipulationMode = 1; ImGui::SameLine();
			if (ToolBtn(ICON_LC_ROTATE_3D,     "Rotate", app->manipulationMode == 2, bw)) app->manipulationMode = 2; ImGui::SameLine();
			if (ToolBtn(ICON_LC_SCALING,       "Scale",  app->manipulationMode == 3, bw)) app->manipulationMode = 3; ImGui::SameLine();
			if (ToolBtn(ICON_LC_PLUS,          "Create", false,                       bw)) ImGui::OpenPopup("##nuke-create");
			if (ImGui::BeginPopup("##nuke-create"))
			{
				if (ImGui::MenuItem("Empty"))  SpawnEmpty();
				if (ImGui::MenuItem("Cube"))   SpawnPrimitive("Cube",   "builtin:cube");
				if (ImGui::MenuItem("Sphere")) SpawnPrimitive("Sphere", "builtin:sphere");
				if (ImGui::MenuItem("Plane"))  SpawnPrimitive("Plane",  "builtin:plane");
				if (ImGui::MenuItem("Camera")) SpawnCamera();
				ImGui::EndPopup();
			}
			// World/Local space toggle for the gizmo (also hotkey X).
			ImGui::SameLine();
			bool worldMode = (app->manipulationWorld != 0);
			if (ToolBtn(worldMode ? ICON_LC_GLOBE : ICON_LC_AXIS_3D,
			            worldMode ? "World space (X)" : "Local space (X)", false, bw))
				app->manipulationWorld = !app->manipulationWorld;

			// CENTER — PIE (Play / Pause / Stop)
			float winW = ImGui::GetWindowWidth();
			float centerW = bw * 3 + st.ItemSpacing.x * 2;
			ImGui::SameLine();
			ImGui::SetCursorPosX((winW - centerW) * 0.5f);
			if (ToolBtn(ICON_LC_PLAY, "Play", app->playState == 1, bw))
			{
				if (app->playState == 0) pieSnapshot = app->currentScene->SaveToString();   // snapshot on entering play
				app->playState = 1;
			}
			ImGui::SameLine();
			if (ToolBtn(ICON_LC_PAUSE, "Pause", app->playState == 2, bw))
			{
				if (app->playState != 0) app->playState = 2;   // pause only while playing
			}
			ImGui::SameLine();
			if (ToolBtn(ICON_LC_SQUARE, "Stop", app->playState == 0, bw))
			{
				if (app->playState != 0 && !pieSnapshot.empty())
				{
					app->selectedInHieararchy = nullptr;
					app->currentScene->LoadFromString(pieSnapshot);   // restore scene on stop
				}
				app->playState = 0;
			}

			// RIGHT — viewport draw mode (Solid / Wireframe)
			float rightW = bw * 2 + st.ItemSpacing.x;
			ImGui::SameLine();
			ImGui::SetCursorPosX(winW - rightW - 8.0f);
			if (ToolBtn(ICON_LC_BOX,      "Solid",     !app->wireframe, bw)) app->wireframe = false; ImGui::SameLine();
			if (ToolBtn(ICON_LC_GRID_3X3, "Wireframe",  app->wireframe, bw)) app->wireframe = true;
		}
		ImGui::End();
		ImGui::PopStyleVar();
	}

	void Draw()
	{
		ImGuizmo::BeginFrame();   // must come right after ImGui::NewFrame (done by NukeUI)

		nuke::Time::getSingleton()->NewFrame();   // real frame delta/elapsed (scripts & systems)

		// PIE: while playing, run game logic (component Update) each frame.
		if (AppInstance::GetSingleton()->playState == 1)
			AppInstance::GetSingleton()->currentScene->Update();

		// Restore the selection saved in editor_state.json, once the scene is loaded.
		if (!pendingSelect.empty())
		{
			if (Atom* a = AppInstance::GetSingleton()->currentScene->Get(pendingSelect.c_str()))
				AppInstance::GetSingleton()->selectedInHieararchy = a;
			pendingSelect.clear();
		}
		// Order matters: main menu, then the toolbar side-bar, then the dock space —
		// each reserves viewport work-area for the next, so panels sit below both bars.
		EditorMenu();
		Toolbar();
		// Full-window dock space so panels can be docked/tabbed/split (sticky).
		// PassthruCentralNode leaves the centre transparent for the scene viewport.
		ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
		window_flags = 0; // panels are dockable/movable now
		for (auto tup : *AppInstance::GetSingleton()->editorWindows)
			tup.second();

		// Apply queued plugin toggles AFTER the window loop: DisablePlugin()'s Shutdown may
		// PopWindow (mutating editorWindows), which would invalidate the iterator above.
		if (!pendingPluginToggle.empty())
		{
			for (auto& pt : pendingPluginToggle)
			{
				if (pt.second) nuke::EnablePlugin(pt.first);
				else           nuke::DisablePlugin(pt.first);
			}
			pendingPluginToggle.clear();
			SyncEnabledPlugins();   // persist the new load list to the .nuproj
		}
	}
};

inline void editorinit() { EditorUI::getSingleton()->SetUp(); }
inline void editorDraw() { EditorUI::getSingleton()->Draw(); }

#endif // EDITORUI_H
