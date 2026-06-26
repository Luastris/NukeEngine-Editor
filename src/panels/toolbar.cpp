// toolbar panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>

// ---- toolbar ----
// A flat button that stays highlighted while `active` (radio/toggle look).
bool EditorUI::ToolBtn(const char* icon, const char* tip, bool active, float w)
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

void EditorUI::SpawnEmpty()
{
	AppInstance* app = AppInstance::GetSingleton();
	Atom* go = new Atom("Empty");
	app->currentScene->Add(go);
	app->selectedInHieararchy = go;
}
void EditorUI::SpawnPrimitive(const char* atomName, const char* guid)
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
void EditorUI::SpawnCube() { SpawnPrimitive("Cube", "builtin:cube"); }
void EditorUI::SpawnCamera()
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
void EditorUI::Toolbar()
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

void EditorUI::Draw()
{
	ImGuizmo::BeginFrame();   // must come right after ImGui::NewFrame (done by NukeUI)

	nuke::Time::getSingleton()->NewFrame();   // real frame delta/elapsed (scripts & systems)

	// Hot-reload shaders edited on disk (~twice a second; cheap mtime checks).
	if ((++hotReloadTick % 30) == 0)
		ResDB::getSingleton()->HotReloadShaders(AppInstance::GetSingleton()->render);

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

	winSettings();        // Project Settings window (default world + hotkeys)
	DispatchHotkeys();    // fire any pressed hotkey chord (after the UI, so fields take input first)

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
