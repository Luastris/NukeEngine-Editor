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
	RecordAdd(go);
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
	RecordAdd(go);
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
	RecordAdd(go);
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
				// Remember the selection by id — LoadFromString recreates every atom (the old
				// pointer dies), so re-resolve it by its stable id on the restored scene.
				long selId = app->selectedInHieararchy ? app->selectedInHieararchy->id.id : 0;
				app->selectedInHieararchy = nullptr;
				app->currentScene->LoadFromString(pieSnapshot);   // restore scene on stop
				if (selId) app->selectedInHieararchy = app->currentScene->GetById(selId);
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
	DrawSaveAsPopup();    // "Save World As" modal
	TrackUndo();          // capture a selected-atom edit for undo when the UI settles
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

// ---- undo/redo (generic command stack) ----
static long AtomParentId(Atom* a) { return (a && a->parent) ? a->parent->id.id : 0; }
static int  AtomIndex(World* w, Atom* a)
{
	if (!a) return -1;
	auto& lst = a->parent ? a->parent->children : w->GetHierarchy();
	int i = 0; for (Atom* s : lst) { if (s == a) return i; ++i; }
	return -1;
}

void EditorUI::PushUndo(const std::string& label, std::function<void()> undoFn, std::function<void()> redoFn)
{
	undoStack.push_back({ std::move(undoFn), std::move(redoFn), label });
	if (undoStack.size() > 200) undoStack.erase(undoStack.begin());
	redoStack.clear();
}

void EditorUI::ResetUndo() { undoStack.clear(); redoStack.clear(); editing = false; editAtomId = 0; editBefore.clear(); }

void EditorUI::Undo()
{
	if (AppInstance::GetSingleton()->playState != 0) return;   // not during PIE
	if (ImGui::GetIO().WantTextInput) return;                  // let text fields keep their own undo
	if (undoStack.empty()) return;
	UndoCmd c = undoStack.back(); undoStack.pop_back();
	c.undo();
	redoStack.push_back(c);
}

void EditorUI::Redo()
{
	if (AppInstance::GetSingleton()->playState != 0) return;
	if (ImGui::GetIO().WantTextInput) return;
	if (redoStack.empty()) return;
	UndoCmd c = redoStack.back(); redoStack.pop_back();
	c.redo();
	undoStack.push_back(c);
}

// Undo primitive for atom deltas: replace the atom (by id) with the given serialized state at a
// placement (parentId 0 = root). Empty json = remove the atom.
void EditorUI::ApplyAtomState(long id, long parentId, int index, const std::string& json)
{
	World* w = AppInstance::GetSingleton()->currentScene;
	w->RemoveAtomById(id);
	if (!json.empty()) { if (Atom* a = LoadAtomFromString(json)) w->InsertAtom(a, parentId, index); }
	AppInstance::GetSingleton()->selectedInHieararchy = w->GetById(id);   // null if it was removed
	editing = false; editAtomId = 0;                                      // don't re-capture this change
}

// Detect a settled edit of the SELECTED atom (inspector widget or gizmo): capture before on the first
// active frame, push one delta command when it deactivates. Other change kinds use PushUndo/RecordChange.
void EditorUI::TrackUndo()
{
	AppInstance* app = AppInstance::GetSingleton();
	if (app->playState != 0) return;   // never track during PIE
	bool active = ImGui::IsAnyItemActive() || ImGuizmo::IsUsing();
	Atom* sel = app->selectedInHieararchy;
	if (active && !editing)
	{
		editing    = true;
		editAtomId = sel ? sel->id.id : 0;
		editBefore = sel ? SaveAtomToString(sel) : std::string();
	}
	else if (!active && editing)
	{
		editing = false;
		if (!editAtomId) return;
		Atom* a = app->currentScene->GetById(editAtomId);
		if (!a) return;
		std::string after = SaveAtomToString(a);
		if (after == editBefore) return;                       // nothing actually changed on this atom
		long id = editAtomId, parent = AtomParentId(a); int index = AtomIndex(app->currentScene, a);
		std::string before = editBefore;
		PushUndo("Edit " + a->GetName(),
			[this, id, parent, index, before]{ ApplyAtomState(id, parent, index, before); },
			[this, id, parent, index, after ]{ ApplyAtomState(id, parent, index, after ); });
	}
}

void EditorUI::RecordAdd(Atom* a)
{
	if (!a) return;
	World* w = AppInstance::GetSingleton()->currentScene;
	long id = a->id.id, parent = AtomParentId(a); int index = AtomIndex(w, a);
	std::string json = SaveAtomToString(a);
	PushUndo("Add " + a->GetName(),
		[this, id, parent, index]      { ApplyAtomState(id, parent, index, std::string()); },   // undo: remove
		[this, id, parent, index, json]{ ApplyAtomState(id, parent, index, json); });            // redo: re-add
}

void EditorUI::RecordReparent(Atom* a, long oldParent, int oldIndex)
{
	if (!a) return;
	World* w = AppInstance::GetSingleton()->currentScene;
	long id = a->id.id, newParent = AtomParentId(a); int newIndex = AtomIndex(w, a);
	if (oldParent == newParent && oldIndex == newIndex) return;
	std::string json = SaveAtomToString(a);
	PushUndo("Reparent " + a->GetName(),
		[this, id, oldParent, oldIndex, json]{ ApplyAtomState(id, oldParent, oldIndex, json); },
		[this, id, newParent, newIndex, json]{ ApplyAtomState(id, newParent, newIndex, json); });
}

void EditorUI::RecordDelete(Atom* a)
{
	if (!a) return;
	World* w = AppInstance::GetSingleton()->currentScene;
	long id = a->id.id, parent = AtomParentId(a); int index = AtomIndex(w, a);
	std::string json = SaveAtomToString(a);   // capture BEFORE the atom is removed
	PushUndo("Delete " + a->GetName(),
		[this, id, parent, index, json]{ ApplyAtomState(id, parent, index, json); },          // undo: re-create
		[this, id, parent, index]      { ApplyAtomState(id, parent, index, std::string()); }); // redo: remove
}

void EditorUI::DeleteSelectedAtom()
{
	AppInstance* app = AppInstance::GetSingleton();
	Atom* a = app->selectedInHieararchy;
	if (!a || a->GetName() == "Editor Camera") return;   // never delete the editor camera
	RecordDelete(a);                                     // serialize for undo first
	long id = a->id.id;
	app->selectedInHieararchy = nullptr;
	app->currentScene->RemoveAtomById(id);               // deletes the atom + its subtree
}
