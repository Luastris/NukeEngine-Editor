// toolbar panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>
#include <API/Model/Light.h>
#include <API/Model/Time.h>   // PIE enter/exit resets the game-speed scale
#include <API/Model/Collider.h>   // primitives spawn with a matching collider by default
#include <API/Model/Environment.h>
#include <API/Model/ReflectionProbe.h>
#include <API/Model/Sprite.h>
#include <API/Model/Canvas.h>
#include <API/Model/Decal.h>
#include <API/Model/Jobs.h>   // PumpMain each editor frame (2.4)
#include <interface/AtomCreators.h>   // registered atom templates ("+" menu, module types)
#include <reflect/Reflect.h>          // Registry_All: creator components by TYPE NAME
#include <iostream>                   // missing-type log line
#include <algorithm>          // std::find (creator categories)
#include <cstring>            // strcmp (NUKE_PACKAGE_MOD=<name> dev hook)
#include <nlohmann/json.hpp>  // atom-clipboard envelope (copy/paste)
using json = nlohmann::json;

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

// Where a freshly-created object lands: on the surface the editor camera looks at (ray hit), else a
// fixed distance straight ahead — so new atoms appear in view instead of at the world origin.
Vector3 EditorUI::SpawnPos()
{
	AppInstance* app = AppInstance::GetSingleton();
	if (editorCam && editorCam->transform && app->currentWorld)
	{
		Transform* t = editorCam->transform;
		Vector3 o = t->globalPosition(), f = t->direction();
		float dist = 0.0f;
		Atom* hit = app->currentWorld->PickDist(o, f, dist);
		double d = (hit && dist > 0.02f && dist < 1e29f) ? (double)dist : 8.0;
		return Vector3(o.x + f.x * d, o.y + f.y * d, o.z + f.z * d);
	}
	return Vector3(0, 0, 0);
}
Atom* EditorUI::FinishSpawn(Atom* atom)
{
	AppInstance* app = AppInstance::GetSingleton();
	atom->GetTransform().position = SpawnPos();   // in front of the camera / on the looked-at surface
	app->currentWorld->Add(atom);
	app->selectedInHieararchy = atom;
	RecordAdd(atom);
	return atom;
}

void EditorUI::SpawnEmpty()
{
	FinishSpawn(new Atom("Empty"));
}
void EditorUI::SpawnPrimitive(const char* atomName, const char* guid)
{
	Atom* atom = new Atom(atomName);
	MeshRenderer* mr = new MeshRenderer();
	atom->AddComponent(mr);
	mr->meshGuid = guid;
	mr->mesh = ResDB::getSingleton()->GetMesh(guid);
	// Every primitive ships with a MATCHING collider by default — a spawned floor/wall/
	// prop must just work with physics, characters and camera probes (remove if unwanted).
	nuke::Collider* col = new nuke::Collider();
	const std::string g = guid;
	if      (g == "builtin:sphere")   col->shape = nuke::Collider::S_Sphere;    // r 0.5 = the mesh
	else if (g == "builtin:capsule")  col->shape = nuke::Collider::S_Capsule;   // r 0.5 / hh 0.5 = the mesh
	else if (g == "builtin:cylinder") { col->shape = nuke::Collider::S_Mesh; col->convex = true; }   // exact hull
	else if (g == "builtin:plane")    col->halfExtents = Vector3(0.5, 0.01, 0.5);   // flat box
	// cube: the Box default (0.5)^3 matches exactly
	atom->AddComponent(col);
	FinishSpawn(atom);
}
void EditorUI::SpawnCube() { SpawnPrimitive("Cube", "builtin:cube"); }
void EditorUI::SpawnLight(int type, const char* atomName)
{
	Atom* atom = new Atom(atomName);
	Light* l = new Light();
	l->type = (Light::Type)type;   // menu passes the raw index
	atom->AddComponent(l);
	FinishSpawn(atom);
}
void EditorUI::SpawnEnvironment()
{
	Atom* atom = new Atom("Environment");
	atom->AddComponent(new Environment());
	FinishSpawn(atom);
}
void EditorUI::SpawnReflectionProbe()
{
	Atom* atom = new Atom("Reflection Probe");
	atom->AddComponent(new ReflectionProbe());
	FinishSpawn(atom);
}
void EditorUI::SpawnCamera()
{
	Atom* atom = new Atom("Camera");
	Camera* c = new Camera();
	c->renderer = AppInstance::GetSingleton()->render;   // share the active renderer (avoids re-init / null deref)
	atom->AddComponent(c);
	FinishSpawn(atom);
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
			if (ImGui::MenuItem(ICON_LC_SQUARE_DASHED " Empty"))  SpawnEmpty();
			if (ImGui::MenuItem(ICON_LC_BOX    " Cube"))   SpawnPrimitive("Cube",   "builtin:cube");
			if (ImGui::MenuItem(ICON_LC_CIRCLE " Sphere")) SpawnPrimitive("Sphere", "builtin:sphere");
			if (ImGui::MenuItem(ICON_LC_SQUARE " Plane"))  SpawnPrimitive("Plane",  "builtin:plane");
			if (ImGui::MenuItem(ICON_LC_CYLINDER " Cylinder")) SpawnPrimitive("Cylinder", "builtin:cylinder");
			if (ImGui::MenuItem(ICON_LC_PILL     " Capsule"))  SpawnPrimitive("Capsule",  "builtin:capsule");
			if (ImGui::MenuItem(ICON_LC_IMAGE    " Sprite"))
			{
				Atom* atom = new Atom("Sprite");
				atom->AddComponent(new Sprite());
				FinishSpawn(atom);
			}
			if (ImGui::MenuItem(ICON_LC_FRAME    " Canvas"))
			{
				Atom* atom = new Atom("Canvas");
				atom->AddComponent(new Canvas());
				FinishSpawn(atom);
			}
			if (ImGui::MenuItem(ICON_LC_STICKER  " Decal"))
			{
				Atom* atom = new Atom("Decal");
				atom->AddComponent(new Decal());
				FinishSpawn(atom);
			}
			if (ImGui::MenuItem(ICON_LC_VIDEO  " Camera")) SpawnCamera();
			if (ImGui::BeginMenu(ICON_LC_LIGHTBULB " Light"))
			{
				if (ImGui::MenuItem(ICON_LC_SUN       " Directional")) SpawnLight(0, "Directional Light");
				if (ImGui::MenuItem(ICON_LC_LIGHTBULB " Point"))       SpawnLight(1, "Point Light");
				if (ImGui::MenuItem(ICON_LC_SPOTLIGHT " Spot"))        SpawnLight(2, "Spot Light");
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem(ICON_LC_CLOUD_SUN " Environment")) SpawnEnvironment();
			if (ImGui::MenuItem(ICON_LC_GLOBE " Reflection Probe")) SpawnReflectionProbe();

			// REGISTERED atom templates (interface/AtomCreators.h): engine systems and MODULES
			// add their own entries with NO editor linkage — components are created through
			// reflection by TYPE NAME. Grouped by category, appended after the built-ins.
			{
				std::vector<std::string> cats;
				for (const nuke::AtomCreator& ac : nuke::AtomCreators())
					if (std::find(cats.begin(), cats.end(), ac.category) == cats.end()) cats.push_back(ac.category);
				auto spawnFromCreator = [&](const nuke::AtomCreator& ac)
				{
					Atom* a = new Atom(ac.label.c_str());
					for (const std::string& tn : ac.components)
					{
						bool found = false;
						for (nuke::TypeInfo* ti : nuke::Registry_All())
							if (ti->name == tn && ti->create && nuke::Registry_IsComponentType(ti))
							{ a->AddComponent((nuke::Component*)ti->create()); found = true; break; }
						if (!found)
							std::cout << "[editor]\t\tatom creator '" << ac.label << "': component type '"
							          << tn << "' is not registered (module not loaded?)" << std::endl;
					}
					FinishSpawn(a);   // ray-place at SpawnPos + add to world + select + undo record
				};
				for (const std::string& cat : cats)
				{
					if (cat.empty())
					{
						for (const nuke::AtomCreator& ac : nuke::AtomCreators())
							if (ac.category.empty() && ImGui::MenuItem((ac.icon + (ac.icon.empty() ? "" : " ") + ac.label).c_str()))
								spawnFromCreator(ac);
					}
					else if (ImGui::BeginMenu(cat.c_str()))
					{
						for (const nuke::AtomCreator& ac : nuke::AtomCreators())
							if (ac.category == cat && ImGui::MenuItem((ac.icon + (ac.icon.empty() ? "" : " ") + ac.label).c_str()))
								spawnFromCreator(ac);
						ImGui::EndMenu();
					}
				}
			}
			ImGui::EndPopup();
		}
		// World/Local space toggle for the gizmo (also hotkey X).
		ImGui::SameLine();
		bool worldMode = (app->manipulationWorld != 0);
		if (ToolBtn(worldMode ? ICON_LC_GLOBE : ICON_LC_AXIS_3D,
		            worldMode ? "World space (X)" : "Local space (X)", false, bw))
			app->manipulationWorld = !app->manipulationWorld;

		// CENTER — PIE (Play / Pause / Stop + possess switch)
		float winW = ImGui::GetWindowWidth();
		float centerW = bw * 4 + st.ItemSpacing.x * 3;
		ImGui::SameLine();
		ImGui::SetCursorPosX((winW - centerW) * 0.5f);
		if (ToolBtn(ICON_LC_PLAY, "Play", app->playState == 1, bw))
		{
			if (app->playState == 0)   // snapshot scene + edit target on entering play
			{
				pieSnapshot  = app->currentWorld->SaveToString();
				pieWorldPath = app->currentWorldPath;
				// A fresh PIE session starts at NORMAL speed — a leftover Game.SetTimeScale
				// from the previous session (slow-mo, freeze) must not leak in.
				Time::getSingleton()->scale = 1.0;
			}
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
			const bool wasPlaying = app->playState != 0;
			// Leave play mode BEFORE the restore: LoadFromString's teardown carries persistent
			// atoms only while playing — PIE-born persistent atoms must NOT leak into the
			// restored EDIT scene (Ctrl+S would write them into the world file).
			app->playState = 0;
			if (wasPlaying && !pieSnapshot.empty())
			{
				// Remember the selection by id — LoadFromString recreates every atom (the old
				// pointer dies), so re-resolve it by its stable id on the restored scene.
				long selId = app->selectedInHieararchy ? app->selectedInHieararchy->id.id : 0;
				app->selectedInHieararchy = nullptr;
				app->currentWorld->LoadFromString(pieSnapshot);   // restore scene on stop
				app->currentWorldPath = pieWorldPath;             // ...and the edit target (a script's
				                                                  // Game.LoadWorld must not leak past Stop)
				if (selId) app->selectedInHieararchy = app->currentWorld->GetById(selId);
			}
			// A game script may have locked/hidden the cursor (Input.SetCursorMode) —
			// stopping play must hand the editor a normal cursor back, always.
			if (app->render) app->render->setCursorMode(0);
			// ...and the game-speed setting dies with the session: edit mode runs at 1x.
			Time::getSingleton()->scale = 1.0;
			// ...and an in-flight Game.LoadWorldAsync must not survive into edit mode
			// (activating a staged game world over the restored edit scene).
			app->CancelWorldLoadAsync();
		}
		// PIE possess switch (UE-style): Game Camera = play through World::GetMainCamera (the one
		// flagged Main, else the world's first camera); Editor Camera = keep the free-fly view.
		// Editor-only — the player always plays through the game camera rule.
		ImGui::SameLine();
		if (ToolBtn(pieUseEditorCam ? ICON_LC_EYE : ICON_LC_VIDEO,
		            pieUseEditorCam ? "PIE view: Editor Camera (click: play through the game's Main Camera)"
		                            : "PIE view: Game Camera — Main flag, else the first camera (click: keep the editor camera view)",
		            !pieUseEditorCam && app->playState != 0, bw))
			pieUseEditorCam = !pieUseEditorCam;

		// RIGHT — camera projection (Perspective / Orthographic) + viewport draw mode (Solid / Wireframe)
		float rightW = bw * 3 + st.ItemSpacing.x * 2;
		ImGui::SameLine();
		ImGui::SetCursorPosX(winW - rightW - 8.0f);
		bool ortho = editorCam && editorCam->projection == nuke::Projection::Orthographic;
		if (ToolBtn(ortho ? ICON_LC_PROPORTIONS : ICON_LC_BOXES,
		            ortho ? "Orthographic (click: Perspective)" : "Perspective (click: Orthographic)", ortho, bw))
			if (editorCam) editorCam->projection = ortho ? nuke::Projection::Perspective : nuke::Projection::Orthographic;
		ImGui::SameLine();
		if (ToolBtn(ICON_LC_BOX,      "Solid",     !app->wireframe, bw)) app->wireframe = false; ImGui::SameLine();
		if (ToolBtn(ICON_LC_GRID_3X3, "Wireframe",  app->wireframe, bw)) app->wireframe = true;
	}
	ImGui::End();
	ImGui::PopStyleVar();
}

void EditorUI::Draw()
{
	// ImGuizmo::BeginFrame opens its own transparent fullscreen "gizmo" window as the
	// default canvas. It's a FLOATING window — with ConfigViewportsNoAutoMerge it would
	// get its own OS window. Pin it to the main viewport: the actual gizmo drawing goes
	// through SetDrawlist() into the Render panel anyway (works in any viewport).
	ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
	ImGuizmo::BeginFrame();   // must come right after ImGui::NewFrame (done by NukeUI)

	nuke::Time::getSingleton()->NewFrame();   // real frame delta/elapsed (scripts & systems)

	nuke::Jobs::PumpMain();   // deliver background-job results to the game thread (2.4)

	// DEV HOOKS (3.2 packaging tests): NUKE_PACKAGE=1 fires Package Project ~2.5 s after
	// boot, NUKE_PACKAGE_MOD=1 fires Package Mod — headless verification without clicks.
	{
		static int pkgDelay = -2;
		if (pkgDelay == -2) { const char* e = std::getenv("NUKE_PACKAGE"); pkgDelay = (e && *e == '1') ? 150 : -1; }
		if (pkgDelay > 0 && --pkgDelay == 0) PackageProject();
		static int modDelay = -2;
		static std::string modHookName;   // NUKE_PACKAGE_MOD=<name> picks the mod's name; "1" = fallbacks
		if (modDelay == -2)
		{
			const char* e = std::getenv("NUKE_PACKAGE_MOD");
			modDelay = (e && e[0]) ? 150 : -1;
			if (modDelay > 0 && strcmp(e, "1") != 0) modHookName = e;
		}
		if (modDelay > 0 && --modDelay == 0) PackageMod(modHookName);
		// NUKE_GM_NEW=<Name>: scaffold a C++ game module ~2 s after boot; NUKE_GM_BUILD=1:
		// fire Build & Reload Game Modules (6.0) — headless verification without clicks.
		static int gmNewDelay = -2;
		static std::string gmNewName;
		if (gmNewDelay == -2)
		{
			const char* e = std::getenv("NUKE_GM_NEW");
			gmNewDelay = (e && e[0]) ? 120 : -1;
			if (gmNewDelay > 0) gmNewName = e;
		}
		if (gmNewDelay > 0 && --gmNewDelay == 0) CreateGameModuleScaffold(gmNewName);
		static int gmBuildDelay = -2;
		if (gmBuildDelay == -2) { const char* e = std::getenv("NUKE_GM_BUILD"); gmBuildDelay = (e && *e == '1') ? 180 : -1; }
		if (gmBuildDelay > 0 && --gmBuildDelay == 0) BuildGameModules();
		// NUKE_PLAY=1: enter PIE ~4 s after boot (headless play verification — scripts,
		// physics, audio all run their real play paths without a click).
		static int playDelay = -2;
		if (playDelay == -2) { const char* e = std::getenv("NUKE_PLAY"); playDelay = (e && *e == '1') ? 250 : -1; }
		if (playDelay > 0 && --playDelay == 0)
		{
			AppInstance* app = AppInstance::GetSingleton();
			if (app->playState == 0)
			{
				pieSnapshot  = app->currentWorld->SaveToString();
				pieWorldPath = app->currentWorldPath;
			}
			app->playState = 1;
			std::cout << "[editor]\t\tNUKE_PLAY hook: entering PIE" << std::endl;
		}
		// NUKE_OPEN_PROJECT=<path> exercises the New/Open Project switch (relaunch + close).
		// The child INHERITS the env var — skip when the target is already open, else the
		// spawned editor would relaunch itself forever.
		static int swDelay = -2;
		static std::string swPath;
		if (swDelay == -2)
		{
			const char* e = std::getenv("NUKE_OPEN_PROJECT");
			boost::system::error_code ec;
			swDelay = (e && e[0]
			           && !bfs::equivalent(bfs::path(e), bfs::path(projectFile), ec)) ? 150 : -1;
			if (swDelay > 0) swPath = e;
		}
		if (swDelay > 0 && --swDelay == 0) RequestProjectSwitch(swPath);
	}

	// PROJECT HUB (booted with no project): the hub IS the whole UI — no menu, no toolbar,
	// no panels, no world logic. Its popups (New Project, unsaved-switch guard) draw here
	// because the normal frame body below never runs.
	if (projectHubMode)
	{
		ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
		DrawProjectHub();
		DrawNewProjectPopup();
		DrawSwitchConfirmPopup();
		return;
	}

	// Hot-reload shaders + materials/textures edited on disk (~twice a second; cheap mtime checks).
	if ((++hotReloadTick % 30) == 0)
	{
		ResDB::getSingleton()->HotReloadShaders(AppInstance::GetSingleton()->render);
		ResDB::getSingleton()->HotReloadAssets(AppInstance::GetSingleton()->render);
	}

	// PIE: while playing, run game logic (component Update) each frame. The fixed-step
	// update (physics + Component::FixedUpdate) runs on AppInstance's fixed-frequency
	// thread — gated on playState internally, independent of the frame rate.
	if (AppInstance::GetSingleton()->playState == 1)
		AppInstance::GetSingleton()->currentWorld->Update();

	// Restore the selection saved in editor_state.json, once the scene is loaded (by stable id, recursive).
	if (pendingSelectId)
	{
		if (Atom* a = AppInstance::GetSingleton()->currentWorld->GetById(pendingSelectId))
			AppInstance::GetSingleton()->selectedInHieararchy = a;
		pendingSelectId = 0;
	}
	// Order matters: main menu, then the toolbar side-bar, then the dock space —
	// each reserves viewport work-area for the next, so panels sit below both bars.
	EditorMenu();
	Toolbar();
	StatusBarPanel();   // bottom side-bar reserves its strip before the dock space
	// Full-window dock space so panels can be docked/tabbed/split (sticky).
	// PassthruCentralNode leaves the centre transparent for the scene viewport.
	ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
	window_flags = 0; // panels are dockable/movable now
	for (auto tup : *AppInstance::GetSingleton()->editorWindows)
		tup.second();

	winSettings();        // Project Settings window (default world + hotkeys)
	winWorldSettings();   // World Settings window (global shadows etc., saved in the .nuworld)
	winPreferences();     // engine-wide Preferences (external editor etc., %APPDATA% scope)
	winTextEditor();      // Text editor (2.2): opened from the browser / asset inspector
	winAssetEditors();    // Asset editors: material / mesh / prefab windows
	DrawSaveAsPopup();    // "Save World As" modal
	DrawNewProjectPopup();      // "New Project" modal (File menu)
	DrawPackageModPopup();      // "Package Mod" modal (pick the mod's name)
	DrawPackageProjectPopup();  // "Game Build" modal (game boot settings, then pack)
	DrawSwitchConfirmPopup();   // unsaved-world guard before a project switch
	TrackUndo();          // capture a selected-atom edit for undo when the UI settles
	TrackDirty();         // refresh the dirty "*" marker
	TrackExternalChange();// detect a disk edit of the open world
	DrawReloadPopup();    // disk changed (editor clean) -> reload?
	DrawConflictPopup();  // disk changed + editor dirty -> reload/overwrite/merge/ignore
	DrawMergeWindow();    // the resolve window
	DispatchHotkeys();    // fire any pressed hotkey chord (after the UI, so fields take input first)

	// Apply queued plugin toggles AFTER the window loop: DisablePlugin()'s Shutdown may
	// PopWindow (mutating editorWindows), which would invalidate the iterator above.
	if (!pendingPluginToggle.empty())
	{
		for (auto& pt : pendingPluginToggle)
		{
			// PHASE_BOOT providers (the renderer) can't be swapped live: enabling one only
			// persists the choice — it becomes the project's provider on next start. A live
			// boot provider can't be turned OFF either (the engine can't run without it).
			if (pt.first->phase() == nuke::PHASE_BOOT)
			{
				if (pt.second && *pt.first->provides())
				{
					serviceChoices[pt.first->provides()] = pt.first->moduleFile;
					SaveProject();
				}
				continue;
			}
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

void EditorUI::PushUndo(const std::string& label, std::function<void()> undoFn, std::function<void()> redoFn,
                        bool worldEdit)
{
	// Non-world commands (file ops, settings, asset tweaks) inherit the current world
	// serial: they sit on the stack without flipping the world's dirty state.
	const long ws = worldEdit ? ++editSerial : WorldEditSerial();
	undoStack.push_back({ std::move(undoFn), std::move(redoFn), label, ws });
	if (undoStack.size() > 200) undoStack.erase(undoStack.begin());
	redoStack.clear();
	idleSnapValid = false;   // any recorded edit may have touched the selected subtree
}

void EditorUI::ResetUndo() { undoStack.clear(); redoStack.clear(); editing = false; editAtomId = 0; editBefore.clear(); idleSnap.clear(); idleAtomId = 0; idleSnapValid = false; }

void EditorUI::Undo()
{
	// Focused editor WINDOWS own their history: a text editor's widget handles Ctrl+Z
	// itself (do nothing here — a scene undo underneath it would be destructive);
	// an asset editor routes to ITS per-window snapshot stack.
	if (textFocused >= 0) return;
	if (aeFocused >= 0 && aeFocused < (int)assetEds.size()) { AssetEditorUndo(assetEds[aeFocused]); return; }
	if (AppInstance::GetSingleton()->playState != 0) return;   // not during PIE
	if (ImGui::GetIO().WantTextInput) return;                  // let text fields keep their own undo
	if (undoStack.empty()) return;
	UndoCmd c = undoStack.back(); undoStack.pop_back();
	c.undo();
	redoStack.push_back(c);
	idleSnapValid = false;   // atoms just changed under the selection
}

void EditorUI::Redo()
{
	if (textFocused >= 0) return;
	if (aeFocused >= 0 && aeFocused < (int)assetEds.size()) { AssetEditorRedo(assetEds[aeFocused]); return; }
	if (AppInstance::GetSingleton()->playState != 0) return;
	if (ImGui::GetIO().WantTextInput) return;
	if (redoStack.empty()) return;
	UndoCmd c = redoStack.back(); redoStack.pop_back();
	c.redo();
	undoStack.push_back(c);
	idleSnapValid = false;   // atoms just changed under the selection
}

// Undo primitive for atom deltas: replace the atom (by id) with the given serialized state at a
// placement (parentId 0 = root). Empty json = remove the atom.
void EditorUI::ApplyAtomState(long id, long parentId, int index, const std::string& json)
{
	World* w = AppInstance::GetSingleton()->currentWorld;
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
	ImGuiID activeId = ImGui::GetActiveID();
	bool active = (activeId != 0) || ImGuizmo::IsUsing();
	Atom* sel = app->selectedInHieararchy;
	long  selId = sel ? sel->id.id : 0;

	// Flush the in-progress edit when manipulation ends, OR when focus jumps straight to a DIFFERENT widget
	// (clicking prop B while A is still active — no idle frame between — would otherwise merge both props into
	// one undo step). The gizmo isn't an ImGui item (activeId 0), so don't treat it as a widget change.
	bool focusChanged = editing && active && !ImGuizmo::IsUsing() && activeId != editActiveId;
	std::string flushedAfter;
	if (editing && (!active || focusChanged))
	{
		editing = false;
		Atom* a = editAtomId ? app->currentWorld->GetById(editAtomId) : nullptr;
		if (a)
		{
			std::string after = SaveAtomToString(a);
			flushedAfter = after;
			if (after != editBefore)                            // something actually changed on this atom
			{
				long id = editAtomId, parent = AtomParentId(a); int index = AtomIndex(app->currentWorld, a);
				std::string before = editBefore;
				PushUndo("Edit " + a->GetName(),
					[this, id, parent, index, before]{ ApplyAtomState(id, parent, index, before); },
					[this, id, parent, index, after ]{ ApplyAtomState(id, parent, index, after ); });
			}
		}
	}
	// Begin a fresh edit. "before" = the state from BEFORE this edit: when chaining straight from another
	// widget, the just-flushed "after"; otherwise the last idle snapshot (true pre-edit state — dodges the
	// slider's click-to-position jump that a same-frame snapshot would already include).
	if (active && !editing)
	{
		editing      = true;
		editAtomId   = selId;
		editActiveId = activeId;
		if (focusChanged && !flushedAfter.empty())          editBefore = flushedAfter;
		else if (selId && selId == idleAtomId && !idleSnap.empty()) editBefore = idleSnap;
		else                                                editBefore = sel ? SaveAtomToString(sel) : std::string();
	}
	// While nothing is being manipulated, keep a valid snapshot of the selected atom — the
	// correct "before" for the NEXT edit. Refreshed ON DEMAND only (selection change, or an
	// edit invalidated it): serializing the subtree EVERY frame froze the editor when a big
	// prefab root was selected.
	if (!active)
	{
		if (!flushedAfter.empty())   // reuse the flush's serialization — it IS the fresh state
		{
			idleAtomId = editAtomId ? editAtomId : selId;
			idleSnap = flushedAfter;
			idleSnapValid = (idleAtomId == selId);
		}
		if (selId != idleAtomId || !idleSnapValid)
		{
			idleAtomId = selId;
			idleSnap = sel ? SaveAtomToString(sel) : std::string();
			idleSnapValid = true;
		}
	}
}

void EditorUI::RecordAdd(Atom* a)
{
	if (!a) return;
	World* w = AppInstance::GetSingleton()->currentWorld;
	long id = a->id.id, parent = AtomParentId(a); int index = AtomIndex(w, a);
	std::string json = SaveAtomToString(a);
	PushUndo("Add " + a->GetName(),
		[this, id, parent, index]      { ApplyAtomState(id, parent, index, std::string()); },   // undo: remove
		[this, id, parent, index, json]{ ApplyAtomState(id, parent, index, json); });            // redo: re-add
}

void EditorUI::RecordReparent(Atom* a, long oldParent, int oldIndex)
{
	if (!a) return;
	World* w = AppInstance::GetSingleton()->currentWorld;
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
	World* w = AppInstance::GetSingleton()->currentWorld;
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
	app->currentWorld->RemoveAtomById(id);               // deletes the atom + its subtree
}

// ---- atom clipboard (copy / cut / paste / duplicate) ----
// Atoms travel as a JSON ENVELOPE on the OS clipboard: paste works across two running editors,
// and stray clipboard text can never be mistaken for an atom. Only the pasted ROOT gets a
// uniquified name — children are namespaced by their parent.

static std::string AtomClipEnvelope(const std::string& atomJson)
{ return std::string("{\"nukeClipboard\":\"atom\",\"atom\":") + atomJson + "}"; }

// "Box" -> "Box (2)" when a sibling at the destination already holds the name (first free N).
static void UniquifySiblingName(World* w, Atom* a, long parentId)
{
	Atom* parent = parentId ? w->GetById(parentId) : nullptr;
	auto& lst = parent ? parent->children : w->GetHierarchy();
	auto taken = [&](const std::string& n)
	{ for (Atom* s : lst) if (s && s != a && s->GetName() == n) return true; return false; };
	std::string base = a->GetName();
	if (!taken(base)) return;
	for (int n = 2; n < 1000; ++n)
	{
		std::string cand = base + " (" + std::to_string(n) + ")";
		if (!taken(cand)) { a->SetName(cand); return; }
	}
}

bool EditorUI::AtomClipboardAvailable()
{
	const char* clip = ImGui::GetClipboardText();
	return clip && strstr(clip, "\"nukeClipboard\"") != nullptr;
}

void EditorUI::CopySelectedAtom()
{
	Atom* a = AppInstance::GetSingleton()->selectedInHieararchy;
	if (!a || a->GetName() == "Editor Camera") return;   // the editor camera is not scene content
	ImGui::SetClipboardText(AtomClipEnvelope(SaveAtomToString(a)).c_str());
}

void EditorUI::CutSelectedAtom()
{
	Atom* a = AppInstance::GetSingleton()->selectedInHieararchy;
	if (!a || a->GetName() == "Editor Camera") return;
	CopySelectedAtom();
	DeleteSelectedAtom();   // undoable — a regretted cut comes back with Ctrl+Z
}

void EditorUI::PasteAtom()
{
	AppInstance* app = AppInstance::GetSingleton();
	const char* clip = ImGui::GetClipboardText();
	if (!clip || !strstr(clip, "\"nukeClipboard\"")) return;
	json j = json::parse(clip, nullptr, false);
	if (j.is_discarded() || j.value("nukeClipboard", std::string()) != "atom" || !j.contains("atom")) return;
	Atom* a = CloneAtomFromString(j["atom"].dump());
	if (!a) return;
	// Placement: sibling AFTER the selection (same parent — the copy lands where you look);
	// nothing selected -> end of the root level.
	World* w = app->currentWorld;
	Atom* sel = app->selectedInHieararchy;
	long parentId = 0; int index = -1;
	if (sel && sel->GetName() != "Editor Camera")
	{ parentId = AtomParentId(sel); index = AtomIndex(w, sel) + 1; }
	UniquifySiblingName(w, a, parentId);
	w->InsertAtom(a, parentId, index);
	app->selectedInHieararchy = a;
	RecordAdd(a);
}

void EditorUI::DuplicateSelectedAtom()
{
	AppInstance* app = AppInstance::GetSingleton();
	Atom* src = app->selectedInHieararchy;
	if (!src || src->GetName() == "Editor Camera") return;
	Atom* a = CloneAtomFromString(SaveAtomToString(src));   // clipboard untouched
	if (!a) return;
	World* w = app->currentWorld;
	long parentId = AtomParentId(src); int index = AtomIndex(w, src) + 1;   // right below the source
	UniquifySiblingName(w, a, parentId);
	w->InsertAtom(a, parentId, index);
	app->selectedInHieararchy = a;
	RecordAdd(a);
}
