// EditorUI setup, boot loading, style and main menu bar.
#include <editor/editorui.h>
#include <import/assimporter.h>
#include <API/Model/Package.h>
#include <API/Model/Jobs.h>
#include <API/Model/StatusBar.h>
namespace bfs = boost::filesystem;

void EditorUI::SetUp()
{
	cout << "[editorui]\t\t" << "EditorUI setup (imgui 1.92 / NukeUI)..." << endl;
	ApplyStyle();

	win = &Config::getSingleton()->window;

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
	// Lucide glyphs sit high in the line — the 4.0f offset nudges them to centre.
	NukeUI::MergeIconFont("fonts/lucide.ttf", 20.0f, 4.0f);

	InitMenu();

	// Project hub boot (no project chosen): no panels, no .nuproj, no world — Draw() shows only the hub.
	if (projectHubMode)
	{
		LoadPreferences();   // recent-projects list + machine prefs
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
		editorCam->transform->position.x = 0; editorCam->transform->position.y = 0; editorCam->transform->position.z = -5;
		editorCam->fov = 60.0f;
	}
	RegisterInspectorOverrides();   // per-type custom inspector drawing
	RegisterHotkeys();              // built-in hotkeys

	LoadPreferences();   // FIRST: the recent-projects list must be in memory before LoadProject records this project
	LoadProject();   // .nuproj: content dir, startup world, plugin list, hotkey bindings
	PushRecentProject(projectFile);

	// Project-local C++ game modules join the shared pool BEFORE the plugin list applies —
	// their types must register before the world loads.
	DiscoverProjectModules();

	// Plugin types register here (OnLoad) BEFORE the world auto-loads, so a world's components
	// resolve; plugins left off keep their components as inert placeholders.
	ApplyProjectPlugins();

	// Plugin hotkeys are registered by now: saved bindings can override the defaults.
	nuke::Hotkeys::Get()->ApplyBindings(pendingHotkeyBinds);

	boost::system::error_code ec;
	bfs::create_directories(contentDir, ec);
	browserCwd = contentDir;

	// Content-relative paths (scripts etc.) resolve against the project, not the exe root.
	AppInstance::GetSingleton()->contentRoot = contentDir;

	if (iRender* r = AppInstance::GetSingleton()->render)   // global RTX reflection settings
	{
		nuke::NukeRT& rt = nuke::Config::getSingleton()->rt;
		r->setRTReflection(rt.intensity, rt.maxDist, rt.bounces, rt.roughCutoff);
	}

	// Explorer drag&drop -> import into the current browser folder, on a worker so a dropped FBX
	// does not freeze the frame.
	AppInstance::GetSingleton()->render->setOnFileDrop([this](const char* p) {
		std::string dest = browserCwd.empty() ? contentDir : browserCwd;
		std::string src = p;
		AssImporter::getSingleton()->ImportAnyAsync(src, dest, [src, dest](bool ok) {
			std::cout << "[editor]\tdrop-import " << (ok ? "ok" : "FAILED") << ": " << src << " -> " << dest << std::endl;
		});
	});

	StartBootLoad();   // content, shaders, pipelines and the world load in the background

	cout << "[editorui]\t\t" << "EditorUI up — project content loading in background." << endl;
}

void EditorUI::StartBootLoad()
{
	bootLoading = 1;
	nuke::StatusBar::Set("boot", "Loading content...", nuke::StatusBar::kIndeterminate);
	const bool mounted = nuke::Package::MountedCount() > 0;
	const std::string cdir = contentDir;
	nuke::Jobs::Schedule([this, mounted, cdir]()
	{
		// Disk + CPU only. Jobs::Shutdown JOINS this job, hence the Stopping() checkpoints.
		ResDB::getSingleton()->LoadContentDir(cdir);
		if (nuke::Jobs::Stopping()) return;
		if (mounted)
		{
			nuke::StatusBar::Set("boot", "Loading packaged content...", nuke::StatusBar::kIndeterminate);
			ResDB::getSingleton()->LoadContentPackaged();
			ResDB::getSingleton()->LoadShadersPackaged();
			if (nuke::Jobs::Stopping()) return;
		}
		// Engine built-ins first: a project must not shadow the built-in "world" default.
		nuke::StatusBar::Set("boot", "Loading shaders...", nuke::StatusBar::kIndeterminate);
		ResDB::getSingleton()->LoadShadersDir("shaders");
		ResDB::getSingleton()->LoadShadersDir(cdir);
		if (nuke::Jobs::Stopping()) return;
		nuke::Jobs::RunOnMain([this]()
		{
			AppInstance* editor = AppInstance::GetSingleton();
			ResDB::getSingleton()->CreateRenderTextures(editor->render);
			LoadEditorState();
			// Boot world = the last one open (editor_state.json), else the project default.
			boost::system::error_code ec;
			bootWorldRel = (!lastWorld.empty() && bfs::exists(bfs::path(editor->WorldFullPath(lastWorld)), ec))
			             ? lastWorld : startupWorld;
			bootPrevActBudget = editor->GetWorldActivationBudget();
			editor->SetWorldActivationBudget(6.0);   // ms/frame
			if (!bootWorldRel.empty())
				editor->StartWorldLoadAsync(bootWorldRel);
			bootLoading = 2;
		});
	});
}

// Also drives the async-world machinery in edit mode, where World::Update never runs.
void EditorUI::PumpBootLoad()
{
	AppInstance* app = AppInstance::GetSingleton();
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
			app->ActivateLoadedWorld();   // the swap happens in the pump above, next frame
		const double lp = app->WorldLoadProgress(), ap = app->WorldActivationProgress();
		if      (ap >= 0) nuke::StatusBar::Set("boot", "Activating world...", (float)ap);
		else if (lp >= 0) nuke::StatusBar::Set("boot", "Loading world '" + bootWorldRel + "'...", (float)lp);
		else
		{
			// Both idle: the world is in (or was missing — already logged).
			app->SetWorldActivationBudget(bootPrevActBudget);
			SyncWorldBaseline();
			// Dev hook: NUKE_OPEN_ASSET=<content-relative path>[;<path>...]
			if (const char* oa = std::getenv("NUKE_OPEN_ASSET"))
				if (oa[0])
				{
					std::string all = oa;
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
	// Docking tab / dock colors, kept dark so the light label text reads.
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
			// Open accepts .nuproj, .nupak or .numod; switching relaunches the editor on the picked path.
			if (ImGui::MenuItem(ICON_LC_FOLDER_PLUS " New Project...")) openNewProjectPopup = true;
			if (ImGui::MenuItem(ICON_LC_FOLDER_OPEN " Open Project...")) OpenProjectCmd();
			ImGui::Separator();
			MenuHotkeyItem("New World",           "editor.world.new");
			MenuHotkeyItem("Open Default World",  "editor.world.open");
			// Mounted-pak session: the base game's worlds live in the pak, not the overlay dir the browser shows.
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
			// Packaging is mutually exclusive by session kind: an authoring project (no basePakPath)
			// packages itself; an archive session opened from .nupak/.numod can only package a mod.
			if (basePakPath.empty())
			{
				if (ImGui::MenuItem(ICON_LC_PACKAGE " Package Project (dist)...")) PackageProjectCmd();   // Game Build dialog first
				// DLC: a crc diff of the cooked project vs a shipped base pak.
				if (ImGui::MenuItem(ICON_LC_PACKAGE " Package DLC (.nupak)..."))    PackageDlcCmd();
			}
			else if (ImGui::MenuItem(ICON_LC_PUZZLE " Package Mod (.numod)..."))   PackageModCmd();
			ImGui::Separator();
			// Root superbuild on a worker; the config the editor currently RUNS is locked and skipped.
			if (ImGui::MenuItem(ICON_LC_HAMMER " Build Engine (Release)")) RunEngineBuild("Release", nullptr);
			if (ImGui::MenuItem(ICON_LC_HAMMER " Build Engine (Debug)"))   RunEngineBuild("Debug", nullptr);
			ImGui::Separator();
			// Build & Reload hot-swaps <project>/source module DLLs in place; refused while playing.
			if (ImGui::MenuItem(ICON_LC_FILE_PLUS_2 " New C++ Game Module...")) gmNamePopup = true;
			if (ImGui::MenuItem(ICON_LC_HAMMER " Build & Reload Game Modules")) BuildGameModules();
			ImGui::Separator();
			// Same path as the window X: the render loop ends and main() runs the normal shutdown.
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
			// Chord labels come from the pool, but the actions are called directly: pool entries stay
			// callback-free because the same chords dispatch per focused window (browser = files).
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
			// Engine-wide preferences: per machine/user, not per project.
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
