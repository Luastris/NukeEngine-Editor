// setup_menu panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>
#include <import/assimporter.h>   // drag&drop import (ImportAny)

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
	// (meshGuid/matGuid now render as asset pickers via their [[nuke::prop(asset=...)]] metadata.)
	RegisterInspectorOverrides();   // per-type custom inspector drawing (e.g. MeshRenderer material panel)
	RegisterHotkeys();              // editor's built-in hotkeys (plugins add their own on load)

	LoadProject();   // .nuproj: content dir, startup world, plugin load list, hotkey bindings (default if missing)

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
	// Shaders from two roots: engine built-in (shaders/) then project (content/). Engine wins
	// on name clashes, so a project can't shadow the built-in "world" default by accident.
	ResDB::getSingleton()->LoadShadersDir("shaders");
	ResDB::getSingleton()->LoadShadersDir(contentDir);
	// Build a renderer pipeline per shader (render is already init'd before editorinit()).
	ResDB::getSingleton()->BuildShaderPipelines(AppInstance::GetSingleton()->render);
	ResDB::getSingleton()->CreateRenderTextures(AppInstance::GetSingleton()->render);   // RTs for RenderTextures

	// Drag&drop from the desktop/Explorer -> import the dropped model/image into the current browser folder.
	AppInstance::GetSingleton()->render->setOnFileDrop([this](const char* p) {
		std::string dest = browserCwd.empty() ? contentDir : browserCwd;
		bool ok = AssImporter::getSingleton()->ImportAny(p, dest.c_str());
		std::cout << "[editor]\tdrop-import " << (ok ? "ok" : "FAILED") << ": " << p << " -> " << dest << std::endl;
	});

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

	// Open the project's default world from content (replaces the demo cube if present). Plugins are
	// already active, so its components deserialize correctly.
	if (editor->OpenWorld(startupWorld))
		cout << "[editorui]\t\t" << "Opened default world '" << startupWorld << "'." << endl;

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
			MenuHotkeyItem("New World",           "editor.world.new");
			MenuHotkeyItem("Open Default World",  "editor.world.open");
			MenuHotkeyItem("Save World",          "editor.world.save");
			MenuHotkeyItem("Save World As...",    "editor.world.saveas");
			ImGui::Separator();
			MenuHotkeyItem("Project Settings...", "editor.settings");
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
			ImGui::EndMenu();
		}
		ImGui::EndMainMenuBar();
	}
}
