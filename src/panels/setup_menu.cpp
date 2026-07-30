// setup_menu panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>
#include <import/assimporter.h>   // drag&drop import (ImportAny)
#include <API/Model/Package.h>    // mounted-pak session (3.2)
#include <API/Model/Jobs.h>       // background boot load (StartBootLoad)
#include <API/Model/StatusBar.h>  // step-by-step boot reporting
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

	// PROJECT HUB boot (no project chosen): none of the project machinery runs — no panels,
	// no .nuproj load, no content scan, no world. Draw() renders only the hub window
	// (recent / open / create); picking a project relaunches the editor on it.
	if (projectHubMode)
	{
		LoadPreferences();   // recent-projects list + machine prefs (backend, detach, ...)
		cout << "[editorui]\t\t" << "Project hub: no project loaded." << endl;
		return;
	}

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

	LoadPreferences();   // engine-wide (%APPDATA%) FIRST: recent-projects list must be in
	                     // memory before LoadProject records this project into it
	LoadProject();   // .nuproj: content dir, startup world, plugin load list, hotkey bindings (default if missing)
	PushRecentProject(projectFile);   // machine-wide recent list ("open last project" startup)

	// Project-local C++ GAME modules (<project>/modules, Phase 6.0) join the shared pool
	// BEFORE the plugin list applies — their types must register before the world loads.
	DiscoverProjectModules();

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

	// The heavy tail — content scan, shader loads, pipeline compiles, the boot world — runs in
	// the BACKGROUND from here (roadmap: fast editor boot). The window and panels are already
	// up; Hierarchy/Inspector/Viewport/Browser stay locked and the status bar reports each step
	// until PumpBootLoad() (called every frame from Draw) flips bootLoading back to 0.
	StartBootLoad();

	cout << "[editorui]\t\t" << "EditorUI up — project content loading in background." << endl;
}

// Background tail of SetUp(): scan content and load shader sources on a worker, then hop back
// to the game thread for GPU-facing steps and stage the boot world asynchronously. Split off
// so the editor window appears immediately instead of after the whole project load.
void EditorUI::StartBootLoad()
{
	bootLoading = 1;
	nuke::StatusBar::Set("boot", "Loading content...", nuke::StatusBar::kIndeterminate);
	const bool mounted = nuke::Package::MountedCount() > 0;
	const std::string cdir = contentDir;
	nuke::Jobs::Schedule([this, mounted, cdir]()
	{
		// Worker-safe half: pure disk + CPU. Panels that read ResDB are locked while this runs.
		// Checkpoints between the steps (and inside the scans themselves): closing the editor
		// mid-boot must exit promptly — Jobs::Shutdown JOINS this job.
		ResDB::getSingleton()->LoadContentDir(cdir);
		if (nuke::Jobs::Stopping()) return;
		if (mounted)
		{
			// Opened from a .nupak (3.2): the base game is MOUNTED read-only (Bethesda-style,
			// no extraction) — register its assets too; the raw dir is the modder's overlay.
			nuke::StatusBar::Set("boot", "Loading packaged content...", nuke::StatusBar::kIndeterminate);
			ResDB::getSingleton()->LoadContentPackaged();
			ResDB::getSingleton()->LoadShadersPackaged();   // content shaders straight from pak bytes
			if (nuke::Jobs::Stopping()) return;
		}
		// Shaders from two roots: engine built-in (shaders/) then project (content/). Engine wins
		// on name clashes, so a project can't shadow the built-in "world" default by accident.
		nuke::StatusBar::Set("boot", "Loading shaders...", nuke::StatusBar::kIndeterminate);
		ResDB::getSingleton()->LoadShadersDir("shaders");
		ResDB::getSingleton()->LoadShadersDir(cdir);
		if (nuke::Jobs::Stopping()) return;
		nuke::Jobs::RunOnMain([this]()
		{
			// Game-thread tail: GPU resources + editor state, then stage the world. Pipelines
			// are NOT built here in one gulp — PumpBootLoad compiles a couple per frame.
			AppInstance* editor = AppInstance::GetSingleton();
			ResDB::getSingleton()->CreateRenderTextures(editor->render);   // RTs for RenderTextures
			// Editor state (project-tied): camera, selection, browser + panel state, lastWorld.
			LoadEditorState();
			// Boot world = the last one open (editor_state.json), else the project default.
			boost::system::error_code ec;
			bootWorldRel = (!lastWorld.empty() && bfs::exists(bfs::path(editor->WorldFullPath(lastWorld)), ec))
			             ? lastWorld : startupWorld;
			bootPrevActBudget = editor->GetWorldActivationBudget();
			editor->SetWorldActivationBudget(6.0);   // ms/frame: atoms stream in without hitching the UI
			if (!bootWorldRel.empty())
				editor->StartWorldLoadAsync(bootWorldRel);   // read+merge+parse on a worker
			bootLoading = 2;
		});
	});
}

// Per-frame boot pump (called from Draw): finishes material pipelines a couple per frame,
// activates the staged world under the activation budget, then holds ONE more locked frame
// (lazy shader bursts — water — land there) before unlocking the panels.
void EditorUI::PumpBootLoad()
{
	AppInstance* app = AppInstance::GetSingleton();
	// Outside PIE nothing pumps the async world machinery (World::Update is PIE-gated in the
	// editor) — pump it here so background loads work in edit mode too, boot or not.
	if (app->playState == 0)
	{
		app->ApplyAsyncWorldLoad();
		app->ContinueWorldActivation();
	}
	if (!bootLoading || bootLoading == 1) return;   // phase 1 completes via RunOnMain
	if (bootLoading == 2)
	{
		const int left = ResDB::getSingleton()->BuildShaderPipelinesStep(app->render, 2);
		if (left > 0)
		{
			nuke::StatusBar::Set("boot", "Compiling pipelines... (" + std::to_string(left) + " left)",
			                     nuke::StatusBar::kIndeterminate);
			return;
		}
		if (app->WorldLoadReady())
			app->ActivateLoadedWorld();   // swap happens in the pump above next frame
		const double lp = app->WorldLoadProgress(), ap = app->WorldActivationProgress();
		if      (ap >= 0) nuke::StatusBar::Set("boot", "Activating world...", (float)ap);
		else if (lp >= 0) nuke::StatusBar::Set("boot", "Loading world '" + bootWorldRel + "'...", (float)lp);
		else
		{
			// Both idle: the world finished (or was empty/missing — already logged). Housekeep.
			app->SetWorldActivationBudget(bootPrevActBudget);
			SyncWorldBaseline();   // baseline = the world we just opened (title, dirty "*")
			// Dev hook (NUKE_OPEN_ASSET=<content-relative path>): open asset editors after the
			// content is actually in — same family as NUKE_GM_NEW/NUKE_GM_BUILD.
			if (const char* oa = std::getenv("NUKE_OPEN_ASSET"))
				if (oa[0])
				{
					std::string all = oa;   // ';'-separated list
					for (size_t p = 0; p < all.size(); )
					{
						size_t q = all.find(';', p);
						if (q == std::string::npos) q = all.size();
						const std::string one = all.substr(p, q - p);
						p = q + 1;
						if (one.empty()) continue;
						const std::string full = AppInstance::GetSingleton()->ResolveContent(one);
						cout << "[editorui]\t\t" << "NUKE_OPEN_ASSET -> " << full << endl;
						OpenAssetEditor(full);
					}
				}
			nuke::StatusBar::Set("boot", "Warming up...", nuke::StatusBar::kIndeterminate);
			bootLoading = 3;   // one more locked frame: the first world frame compiles lazy shaders
		}
		return;
	}
	// bootLoading == 3: the warm-up frame rendered — unlock.
	nuke::StatusBar::Remove("boot");
	bootLoading = 0;
	cout << "[editorui]\t\t" << "Project loaded — editor unlocked." << endl;
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
				if (ImGui::MenuItem(ICON_LC_PACKAGE " Package Project (dist)...")) PackageProjectCmd();   // Game Build dialog first
				// DLC (developer-only): a crc diff of the cooked project vs a SHIPPED base pak —
				// same container, mounts between the base and the mods, never editable itself.
				if (ImGui::MenuItem(ICON_LC_PACKAGE " Package DLC (.nupak)..."))    PackageDlcCmd();
			}
			else if (ImGui::MenuItem(ICON_LC_PUZZLE " Package Mod (.numod)..."))   PackageModCmd();
			ImGui::Separator();
			// Editor-driven builds (root superbuild): output -> Console, progress -> status
			// bar, worker thread. The config the editor RUNS is locked (skipped with a note).
			if (ImGui::MenuItem(ICON_LC_HAMMER " Build Engine (Release)")) RunEngineBuild("Release", nullptr);
			if (ImGui::MenuItem(ICON_LC_HAMMER " Build Engine (Debug)"))   RunEngineBuild("Debug", nullptr);
			ImGui::Separator();
			// C++ GAME modules (Phase 6.0): the game lives in <project>/source as native
			// NUKEModule DLLs; Build & Reload rebuilds them and hot-swaps the DLLs in place
			// (components survive as placeholders through the swap). Refused while playing.
			if (ImGui::MenuItem(ICON_LC_FILE_PLUS_2 " New C++ Game Module...")) gmNamePopup = true;
			if (ImGui::MenuItem(ICON_LC_HAMMER " Build & Reload Game Modules")) BuildGameModules();
			ImGui::Separator();
			// Same path as closing the window with X/Alt+F4: the render loop ends and the
			// normal shutdown (unsaved-changes handling included) runs in main().
			if (ImGui::MenuItem("Quit", "Alt+F4"))
				if (AppInstance* app = AppInstance::GetSingleton(); app && app->render)
					app->render->requestClose();
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Edit"))
		{
			if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undoStack.empty())) Undo();
			if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !redoStack.empty())) Redo();
			ImGui::Separator();
			// Atom clipboard: acts on the hierarchy selection. Chord labels come from the pool
			// (rebindable); the actions are called directly — the pool entries stay callback-free
			// because the same chords are dispatched per focused window (browser = files).
			{
				AppInstance* app = AppInstance::GetSingleton();
				Atom* sel = app->selectedInHieararchy;
				const bool haveSel = sel && sel->GetName() != "Editor Camera";
				auto chordName = [](const char* id) -> const char*
				{
					nuke::Hotkey* h = nuke::Hotkeys::Get()->Find(id);
					return (h && h->bound) ? ImGui::GetKeyChordName((ImGuiKeyChord)h->chord) : nullptr;
				};
				if (ImGui::MenuItem("Cut",       chordName("editor.cut"),       false, haveSel)) CutSelectedAtom();
				if (ImGui::MenuItem("Copy",      chordName("editor.copy"),      false, haveSel)) CopySelectedAtom();
				if (ImGui::MenuItem("Paste",     chordName("editor.paste"),     false, AtomClipboardAvailable())) PasteAtom();
				if (ImGui::MenuItem("Duplicate", chordName("editor.duplicate"), false, haveSel)) DuplicateSelectedAtom();
			}
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

	// "New C++ Game Module" name modal (opened from the Project menu above).
	if (gmNamePopup) { ImGui::OpenPopup("New C++ Game Module"); gmNamePopup = false; gmNameBuf[0] = 0; }
	if (ImGui::BeginPopupModal("New C++ Game Module", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextUnformatted("Module name (a C++ identifier, e.g. MyGame):");
		ImGui::SetNextItemWidth(280);
		bool enter = ImGui::InputText("##gmname", gmNameBuf, sizeof(gmNameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
		bool valid = gmNameBuf[0] && (isalpha((unsigned char)gmNameBuf[0]) || gmNameBuf[0] == '_');
		for (char* c = gmNameBuf; valid && *c; ++c)
			if (!isalnum((unsigned char)*c) && *c != '_') valid = false;
		if (!valid && gmNameBuf[0]) ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "Letters/digits/underscore, not starting with a digit.");
		if ((ImGui::Button("Create") || enter) && valid)
		{
			CreateGameModuleScaffold(gmNameBuf);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}
