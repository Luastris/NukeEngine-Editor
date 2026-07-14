// setup_menu panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>
#include <import/assimporter.h>   // drag&drop import (ImportAny)
#include <API/Model/Package.h>    // mounted-pak session (3.2)
namespace bfs = boost::filesystem;

void EditorUI::SetUp()
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

	Atom* camObj = editor->currentWorld->Get("Editor Camera");
	if (camObj)
		editorCam = camObj->GetComponent<Camera>();
	if (editorCam)
	{
		// Park the editor camera so it looks at the origin (camera control TODO).
		editorCam->transform->position.x = 0; editorCam->transform->position.y = 0; editorCam->transform->position.z = -5;
		editorCam->fov = 60.0f;   // 90 vertical is too wide → strong edge distortion
		// rotation defaults to identity quaternion (looks +Z, at the origin).
	}
	// (meshGuid/matGuid now render as asset pickers via their [[nuke::prop(asset=...)]] metadata.)
	RegisterInspectorOverrides();   // per-type custom inspector drawing (e.g. MeshRenderer material panel)
	RegisterHotkeys();              // editor's built-in hotkeys (plugins add their own on load)

	LoadProject();   // .nuproj: content dir, startup world, plugin load list, hotkey bindings (default if missing)
	LoadPreferences();   // engine-wide (%APPDATA%): external editor choice + detection scan

	// Activate the project's chosen plugins from the shared pool (InitModules discovered
	// them already). Types register here (OnLoad) BEFORE the world auto-loads, so a world's
	// components resolve; plugins left off keep their components as inert placeholders.
	ApplyProjectPlugins();

	// Now that plugin hotkeys are registered too, apply the project's saved bindings over the defaults.
	nuke::Hotkeys::Get()->ApplyBindings(pendingHotkeyBinds);

	// Project content folder (imported assets live here). Create it + open the browser there.
	boost::system::error_code ec;
	bfs::create_directories(contentDir, ec);
	browserCwd = contentDir;

	// Content relative paths (scripts etc.) resolve against the project, not the exe root.
	AppInstance::GetSingleton()->contentRoot = contentDir;

	// Load native assets (.numesh) from content/ so meshGuid refs in saved worlds resolve.
	ResDB::getSingleton()->LoadContentDir(contentDir);
	// Opened from a .nupak (3.2): the base game is MOUNTED read-only (Bethesda-style, no
	// extraction) — register its assets too; the raw dir above is the modder's overlay.
	if (nuke::Package::MountedCount() > 0)
	{
		ResDB::getSingleton()->LoadContentPackaged();
		ResDB::getSingleton()->LoadShadersPackaged();   // content shaders straight from pak bytes
	}
	// Shaders from two roots: engine built-in (shaders/) then project (content/). Engine wins
	// on name clashes, so a project can't shadow the built-in "world" default by accident.
	ResDB::getSingleton()->LoadShadersDir("shaders");
	ResDB::getSingleton()->LoadShadersDir(contentDir);
	// Build a renderer pipeline per shader (render is already init'd before editorinit()).
	ResDB::getSingleton()->BuildShaderPipelines(AppInstance::GetSingleton()->render);
	ResDB::getSingleton()->CreateRenderTextures(AppInstance::GetSingleton()->render);   // RTs for RenderTextures
	if (iRender* r = AppInstance::GetSingleton()->render)   // push global RTX reflection settings (config/main.json)
	{
		nuke::NukeRT& rt = nuke::Config::getSingleton()->rt;
		r->setRTReflection(rt.intensity, rt.maxDist, rt.bounces, rt.roughCutoff);
	}

	// Drag&drop from the desktop/Explorer -> import the dropped model/image into the current
	// browser folder. ASYNC (2.4): the conversion runs on a worker — a dropped FBX must not
	// freeze the frame (this was the last synchronous import path).
	AppInstance::GetSingleton()->render->setOnFileDrop([this](const char* p) {
		std::string dest = browserCwd.empty() ? contentDir : browserCwd;
		std::string src = p;
		AssImporter::getSingleton()->ImportAnyAsync(src, dest, [src, dest](bool ok) {
			std::cout << "[editor]\tdrop-import " << (ok ? "ok" : "FAILED") << ": " << src << " -> " << dest << std::endl;
		});
	});

	// Editor state (project-tied): camera, selection, inspector + browser + panel state.
	LoadEditorState();
	// (The old "demo cube so the viewport shows something" seeded a stray Cube into every
	// session — an EMPTY world must open empty. Removed 2026-07-11.)

	// Open the last world the editor had open (editor_state.json); fall back to the project's
	// default world if none was recorded or its file is gone (renamed/deleted outside). Plugins
	// are already active, so its components deserialize correctly.
	{
		boost::system::error_code ec;
		std::string bootWorld = (!lastWorld.empty() && bfs::exists(bfs::path(editor->WorldFullPath(lastWorld)), ec))
		                      ? lastWorld : startupWorld;
		if (editor->OpenWorld(bootWorld))
			cout << "[editorui]\t\t" << "Opened " << (bootWorld == startupWorld ? "default" : "last")
			     << " world '" << bootWorld << "'." << endl;
	}

	SyncWorldBaseline();   // baseline = the world we just opened; title "NukeEngine Editor - <project> - <world>"
	cout << "[editorui]\t\t" << "EditorUI ready." << endl;
}

// NukeEngine dark theme (ported from the old gui.cpp to imgui 1.92 enums).
void EditorUI::ApplyStyle()
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
	c[ImGuiCol_TextDisabled]         = ImVec4(0.55f, 0.55f, 0.60f, 1.00f);   // readable grey, not near-black
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
	c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.00f, 0.00f, 0.00f, 0.55f);   // dark dim, not a white flash
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
void EditorUI::InitMenu()
{
	MenuStrip* mstrip = AppInstance::GetSingleton()->menuStrip = new MenuStrip();
	mstrip->AddItem("Tools/", "Plugin manager", TogglePluginMGR);
}

void EditorUI::TogglePluginMGR()
{
Config::getSingleton()->window.plugmgr = !Config::getSingleton()->window.plugmgr;
}

bool EditorUI::EditorSubMenu(MenuItem* item)
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

void EditorUI::EditorMenu()
{
	if (ImGui::BeginMainMenuBar())
	{
		if (ImGui::BeginMenu("File"))
		{
			// Projects: create a fresh one, or open ANY form — raw .nuproj (development),
			// packed .nupak (release game, mounted read-only + mod overlay), .numod (mod).
			// Switching relaunches the editor on the picked path (project lifecycle = boot).
			if (ImGui::MenuItem(ICON_LC_FOLDER_PLUS " New Project...")) openNewProjectPopup = true;
			if (ImGui::MenuItem(ICON_LC_FOLDER_OPEN " Open Project...")) OpenProjectCmd();
			ImGui::Separator();
			MenuHotkeyItem("New World",           "editor.world.new");
			MenuHotkeyItem("Open Default World",  "editor.world.open");
			// Mounted-pak session: the base game's worlds live in the pak, not the overlay
			// dir the browser shows — list them here so a modder can open any of them.
			if (nuke::Package::MountedCount() > 0 && ImGui::BeginMenu("Open World (package)"))
			{
				for (const std::string& rel : nuke::Package::List("content/"))
				{
					if (rel.size() < 9 || rel.compare(rel.size() - 8, 8, ".nuworld") != 0) continue;
					std::string worldRel = rel.substr(8);   // strip "content/"
					if (ImGui::MenuItem(worldRel.c_str())) OpenWorldCmd(worldRel);
				}
				ImGui::EndMenu();
			}
			MenuHotkeyItem("Save World",          "editor.world.save");
			MenuHotkeyItem("Save World As...",    "editor.world.saveas");
			ImGui::Separator();
			MenuHotkeyItem("Project Settings...", "editor.settings");
			if (ImGui::MenuItem("World Settings...")) { worldSettingsOpen = true; worldSettingsFocus = true; }
			ImGui::Separator();
			// Packaging (3.2) — the two commands are mutually exclusive by session kind.
			// AUTHORING project (no basePakPath): Package Project only — there is nothing to
			// mod, you own the game. ARCHIVE session (opened from .nupak/.numod): Package Mod
			// only — repackaging someone's shipped game into a full second game is forbidden.
			if (basePakPath.empty())
			{
				if (ImGui::MenuItem(ICON_LC_PACKAGE " Package Project (dist)")) PackageProject();
			}
			else if (ImGui::MenuItem(ICON_LC_PUZZLE " Package Mod (.numod)..."))   PackageModCmd();
			ImGui::Separator();
			// Editor-driven builds (root superbuild): output -> Console, progress -> status
			// bar, worker thread. The config the editor RUNS is locked (skipped with a note).
			if (ImGui::MenuItem(ICON_LC_HAMMER " Build Engine (Release)")) RunEngineBuild("Release", nullptr);
			if (ImGui::MenuItem(ICON_LC_HAMMER " Build Engine (Debug)"))   RunEngineBuild("Debug", nullptr);
			ImGui::Separator();
			if (ImGui::MenuItem("Quit", "Alt+F4")) {}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Edit"))
		{
			if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undoStack.empty())) Undo();
			if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !redoStack.empty())) Redo();
			ImGui::Separator();
			if (ImGui::MenuItem("Cut", "CTRL+X")) {}
			if (ImGui::MenuItem("Copy", "CTRL+C")) {}
			if (ImGui::MenuItem("Paste", "CTRL+V")) {}
			ImGui::Separator();
			// ENGINE-wide preferences (per machine/user, not per project) — external editor etc.
			if (ImGui::MenuItem(ICON_LC_SETTINGS_2 " Preferences...")) { prefsOpen = true; prefsFocus = true; }
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
			ImGui::Separator();
			ImGui::MenuItem("Project Settings", nullptr, &settingsOpen);
			if (ImGui::MenuItem("World Settings", nullptr, &worldSettingsOpen)) if (worldSettingsOpen) worldSettingsFocus = true;
			ImGui::EndMenu();
		}
		ImGui::EndMainMenuBar();
	}
}
