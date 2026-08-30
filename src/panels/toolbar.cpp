// toolbar panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>
#include <API/Model/Light.h>
#include <API/Model/Time.h>
#include <API/Model/Collider.h>
#include <API/Model/Environment.h>
#include <API/Model/ReflectionProbe.h>
#include <API/Model/Sprite.h>
#include <API/Model/Canvas.h>
#include <API/Model/Decal.h>
#include <API/Model/Jobs.h>
#include <interface/AtomCreators.h>   // registered atom templates for the "+" menu
#include <reflect/Reflect.h>          // Registry_All: creator components by type name
#include <interface/EditorHooks.h>    // module-registered view toggles in the snap popup
#include <config.h>                   // packaging dev hooks mirror the Game Build dialog
#include <import/assimporter.h>       // NUKE_IMPORT dev hook (probe runs)
#include <API/Model/Animator.h>       // NUKE_PREFAB_ANIM_TEST dev hook
#include <API/Model/BoneMap.h>
#include <API/Model/Game.h>           // NUKE_ASSET_SHOT dev hook (editor screenshot)
#include <API/Model/SkinnedMeshRenderer.h>
#include <API/Model/AnimClip.h>
#include <API/Model/resdb.h>
#include <functional>
#include <cmath>
#include <interface/Importers.h>      // NUKE_PLUGIN_TOGGLE_TEST: registry purge probe
#include <interface/Modular.h>
#include <interface/AppInstance.h>
#include <boost/filesystem.hpp>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <nlohmann/json.hpp>
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
	atom->GetTransform().position = SpawnPos();
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
	// Each primitive gets a collider matching its mesh dimensions.
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
	c->renderer = AppInstance::GetSingleton()->render;   // share the active renderer
	atom->AddComponent(c);
	FinishSpawn(atom);
}

// Second row under the main menu: tools (left) | PIE (center) | viewport mode (right).
void EditorUI::Toolbar()
{
	ImGuiViewport* vp = ImGui::GetMainViewport();
	// ImGui: WindowPadding must stay pushed across the whole Begin..End scope, else the row sticks to the top edge.
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
			if (ImGui::MenuItem(ICON_LC_FOLDER " Folder"))        CreateFolderAtom(nullptr);
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

			// Registered atom templates (AtomCreators): components created via reflection by
			// type name, grouped by category and appended after the built-ins.
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
					FinishSpawn(a);
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

		// grid snap: click toggles, right-click opens the increments (persisted per project).
		ImGui::SameLine();
		if (ToolBtn(ICON_LC_MAGNET, snapEnabled ? "Snap ON (Ctrl = free)\nRight-click: increments"
		                                        : "Snap OFF (Ctrl = snap)\nRight-click: increments",
		            snapEnabled, bw))
			snapEnabled = !snapEnabled;
		if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##nuke-snap");
		if (ImGui::BeginPopup("##nuke-snap"))
		{
			ImGui::TextDisabled("Snap increments (move = WORLD grid)");
			ImGui::SetNextItemWidth(120); ImGui::DragFloat("Move",   &snapMove,  0.05f, 0.01f, 100.0f, "%.2f");
			ImGui::SetNextItemWidth(120); ImGui::DragFloat("Rotate", &snapRot,   0.5f,  0.5f,  90.0f,  "%.1f deg");
			ImGui::SetNextItemWidth(120); ImGui::DragFloat("Scale",  &snapScale, 0.01f, 0.01f, 10.0f,  "%.2f");
			ImGui::Checkbox("Show world grid", &gridVisible);
			ImGui::Checkbox("Show streaming cells", &streamVizVisible);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("World Partition overlay: XZ cells colored by state\n"
			                                              "(loaded / parked / cold on disk / loading / HLOD) + sizes.");
			// Module-registered view toggles (interface/EditorHooks.h) — no module names here.
			for (const EditorToggle& t : EditorToggles())
			{
				bool on = t.get();
				if (ImGui::Checkbox(t.label.c_str(), &on)) t.set(on);
				if (!t.tip.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t.tip.c_str());
			}
			ImGui::TextDisabled("Hold V while moving: surface snap");
			ImGui::EndPopup();
		}

		// CENTER — PIE (Play / Pause / Stop + possess switch)
		float winW = ImGui::GetWindowWidth();
		float centerW = bw * 4 + st.ItemSpacing.x * 3;
		ImGui::SameLine();
		ImGui::SetCursorPosX((winW - centerW) * 0.5f);
		// PIE on a half-loaded world would snapshot (and later restore) a partial scene.
		ImGui::BeginDisabled(bootLoading != 0);
		if (ToolBtn(ICON_LC_PLAY, "Play", app->playState == 1, bw))
		{
			if (app->playState == 0)   // snapshot scene + edit target on entering play
			{
				pieSnapshot  = app->currentWorld->SaveToString();
				pieWorldPath = app->currentWorldPath;
				Time::getSingleton()->scale = 1.0;   // a leftover SetTimeScale must not leak into a fresh session
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
			// Leave play mode BEFORE the restore: LoadFromString's teardown keeps persistent
			// atoms only while playing, so PIE-born ones would leak into the edit scene.
			app->playState = 0;
			if (wasPlaying && !pieSnapshot.empty())
			{
				// LoadFromString recreates every atom — re-resolve the selection by stable id.
				long selId = app->selectedInHieararchy ? app->selectedInHieararchy->id.id : 0;
				app->selectedInHieararchy = nullptr;
				app->currentWorld->LoadFromString(pieSnapshot);
				app->currentWorldPath = pieWorldPath;             // a script's Game.LoadWorld must not leak past Stop
				if (selId) app->selectedInHieararchy = app->currentWorld->GetById(selId);
			}
			if (app->render) app->render->setCursorMode(0);   // a script may have locked/hidden the cursor
			Time::getSingleton()->scale = 1.0;
			app->CancelWorldLoadAsync();   // a staged async world must not activate over the restored edit scene
		}
		// PIE possess switch: Game Camera = World::GetMainCamera; Editor Camera = keep the free-fly view.
		ImGui::SameLine();
		if (ToolBtn(pieUseEditorCam ? ICON_LC_EYE : ICON_LC_VIDEO,
		            pieUseEditorCam ? "PIE view: Editor Camera (click: play through the game's Main Camera)"
		                            : "PIE view: Game Camera — Main flag, else the first camera (click: keep the editor camera view)",
		            !pieUseEditorCam && app->playState != 0, bw))
			pieUseEditorCam = !pieUseEditorCam;
		ImGui::EndDisabled();   // boot-load PIE lock

		// RIGHT — camera projection (Perspective / Orthographic) + viewport draw mode (Solid / Wireframe)
		float rightW = bw * 4 + st.ItemSpacing.x * 3;
		ImGui::SameLine();
		ImGui::SetCursorPosX(winW - rightW - 8.0f);
		// Freeze culling (debug view): frustum + occlusion verdicts stay put while the camera
		// moves; draws the occlusion-culled boxes in red.
		if (ToolBtn(ICON_LC_SNOWFLAKE, app->freezeCulling ? "Freeze Culling: ON (click: resume)" : "Freeze Culling (debug: what the culling removes)",
		            app->freezeCulling, bw))
			app->freezeCulling = !app->freezeCulling;
		ImGui::SameLine();
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
	// ImGuizmo's own fullscreen canvas window is floating: pin it to the main viewport so
	// ConfigViewportsNoAutoMerge doesn't give it its own OS window.
	ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
	ImGuizmo::BeginFrame();   // must come right after ImGui::NewFrame (done by NukeUI)

	nuke::Time::getSingleton()->NewFrame();

	nuke::Jobs::PumpMain();   // deliver background-job results to the game thread

	if (!projectHubMode)
		PumpBootLoad();   // background project load + the async-world pump (edit mode)

	// Pointer gate: closed until the viewport panel proves the mouse is over the game view
	// (or the game owns the cursor) later this frame — a hidden viewport keeps it closed.
	nuke::AppInstance::GetSingleton()->gamePointerActive = false;

	// Dev hooks: env vars fire packaging/build/play actions a few seconds after boot.
	{
		static int pkgDelay = -2;
		static bool pkgSkipBuild = false;   // NUKE_PACKAGE=2: package as-built (probe runs)
		if (pkgDelay == -2)
		{
			// NUKE_STREAM_VIZ=1: boot with the streaming overlay on (probe runs).
			if (const char* sv = std::getenv("NUKE_STREAM_VIZ"))
				streamVizVisible = *sv == '1';
			const char* e = std::getenv("NUKE_PACKAGE");
			pkgDelay = (e && (*e == '1' || *e == '2')) ? 150 : -1;
			pkgSkipBuild = e && *e == '2';
			if (pkgDelay > 0)
			{
				// Headless equivalents of the Game Build dialog's knobs (probe runs).
				const char* dbg = std::getenv("NUKE_PACKAGE_DEBUG");
				if (dbg && *dbg == '1') gbBuildCfg = 1;
				const char* con = std::getenv("NUKE_PACKAGE_CONSOLE");
				if (con && *con == '1')
				{
					gbWin = nuke::Config::getSingleton()->window;   // the dialog prefills the same way
					gbWinSet = true;
					gbConsole = true;
				}
			}
		}
		if (pkgDelay > 0 && --pkgDelay == 0) { if (pkgSkipBuild) PackageProjectNow(); else PackageProject(); }
		static int modDelay = -2;
		static std::string modHookName;   // NUKE_PACKAGE_MOD=<name>; "1" = fallbacks
		if (modDelay == -2)
		{
			const char* e = std::getenv("NUKE_PACKAGE_MOD");
			modDelay = (e && e[0]) ? 150 : -1;
			if (modDelay > 0 && strcmp(e, "1") != 0) modHookName = e;
		}
		if (modDelay > 0 && --modDelay == 0) PackageMod(modHookName);
		// NUKE_IMPORT=<file;file;...>: convert sources into content/Imported (probe runs).
		static int impDelay = -2;
		static std::string impList;
		if (impDelay == -2)
		{
			const char* e = std::getenv("NUKE_IMPORT");
			impDelay = (e && e[0]) ? 120 : -1;
			if (impDelay > 0) impList = e;
		}
		// NUKE_PLUGIN_TOGGLE_TEST=<dll file name>: disable -> probe -> re-enable (registry
		// purge check: a disabled plugin's import formats must leave the registries).
		static int tglDelay = -2;
		static std::string tglName;
		if (tglDelay == -2)
		{
			const char* e = std::getenv("NUKE_PLUGIN_TOGGLE_TEST");
			tglDelay = (e && e[0]) ? 220 : -1;
			if (tglDelay > 0) tglName = e;
		}
		if (tglDelay > 0 && --tglDelay == 0)
			for (auto& m : nuke::GetModules())
				if (m && bfs::path(m->moduleFile).filename().string() == tglName)
				{
					const bool before = nuke::ImporterForExt(".vrm") != nullptr;
					nuke::DisablePlugin(m.get());
					const bool off = nuke::ImporterForExt(".vrm") != nullptr;
					nuke::EnablePlugin(m.get());
					const bool back = nuke::ImporterForExt(".vrm") != nullptr;
					std::cout << "[PluginToggleTest]	" << tglName << " .vrm importer: before="
					          << before << " disabled=" << off << " reenabled=" << back << std::endl;
					break;
				}
		if (impDelay > 0 && --impDelay == 0)
		{
			namespace bfs = boost::filesystem;
			bfs::path dest = bfs::path(nuke::AppInstance::GetSingleton()->contentRoot) / "Imported";
			boost::system::error_code iec;
			bfs::create_directories(dest, iec);
			size_t p = 0;
			while (p < impList.size())
			{
				size_t sc = impList.find(';', p);
				if (sc == std::string::npos) sc = impList.size();
				std::string src = impList.substr(p, sc - p);
				p = sc + 1;
				if (src.empty()) continue;
				const bool ok = nuke::AssImporter::getSingleton()->ImportAny(src.c_str(), dest.string().c_str());
				std::cout << "[Import]\thook " << (ok ? "ok " : "FAILED ") << src << std::endl;
			}
			std::cout << "[Import]\thook done" << std::endl;
		}
		// NUKE_GM_NEW=<Name>: scaffold a C++ game module; NUKE_GM_BUILD=1: Build & Reload Game Modules.
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
		// NUKE_PLAY=1: enter PIE ~4 s after boot.
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
		// NUKE_PREFAB_SIM_TEST=<.nuprefab path>: open the prefab editor, toggle simulate,
		// verify the subtree's rigidbody FALLS in the private sandbox, stop, verify restore.
		static int simDelay = -2;
		static std::string simPath;
		static int simPhase = 0, simFrames = 0;
		static double simY0 = 0.0;
		if (simDelay == -2)
		{
			const char* e = std::getenv("NUKE_PREFAB_SIM_TEST");
			simDelay = (e && e[0]) ? 200 : -1;
			if (simDelay > 0) simPath = e;
		}
		if (simDelay > 0 && --simDelay == 0) { OpenAssetEditor(simPath); simPhase = 1; }
		if (simPhase > 0)
			for (auto& w : assetEds)
				if (w.path == simPath && w.prefabRoot)
				{
					if (simPhase == 1 && ++simFrames > 30)
					{
						simY0 = w.prefabRoot->GetTransform().position.y;
						ToggleAnimPreview(w);
						simPhase = 2; simFrames = 0;
					}
					else if (simPhase == 2 && ++simFrames > 150)
					{
						const double y1 = w.prefabRoot->GetTransform().position.y;
						ToggleAnimPreview(w);
						const double y2 = w.prefabRoot->GetTransform().position.y;
						std::cout << "[PrefabSimTest]\ty0=" << simY0 << " y1=" << y1
						          << " restored=" << y2
						          << " fell=" << (y1 < simY0 - 0.5 ? 1 : 0)
						          << " landed=" << (std::fabs(y1 - 0.5) < 0.2 ? 1 : 0)
						          << " back=" << (std::fabs(y2 - simY0) < 0.01 ? 1 : 0) << std::endl;
						std::cout << "[PrefabSimTest]\tdone" << std::endl;
						simPhase = 0;
					}
					break;
				}
		// NUKE_PREFAB_ANIM_TEST=<.nuprefab path>|<clip name>[|<bonemap name>|<probe bone>]:
		// open the prefab editor, set the clip (and bone map) on the preview Animator (the
		// inspector picker path), toggle Play, verify the pose actually moves — and that no
		// skinned mesh EXPLODES (posed AABB diagonal vs the bind diagonal, every enabled SMR).
		static int patDelay = -2;
		static std::string patPath, patClip, patMap, patBone = "J_Bip_L_Hand";
		static int patPhase = 0, patFrames = 0;
		static float patP0[16], patPeak = 0.0f;
		if (patDelay == -2)
		{
			const char* e = std::getenv("NUKE_PREFAB_ANIM_TEST");
			patDelay = (e && e[0]) ? 200 : -1;
			if (patDelay > 0)
			{
				std::string s = e;
				std::vector<std::string> part;
				size_t p0 = 0;
				while (true)
				{
					const size_t bar = s.find('|', p0);
					part.push_back(s.substr(p0, bar == std::string::npos ? std::string::npos : bar - p0));
					if (bar == std::string::npos) break;
					p0 = bar + 1;
				}
				patPath = part.size() > 0 ? part[0] : "";
				patClip = part.size() > 1 ? part[1] : "";
				patMap  = part.size() > 2 ? part[2] : "";
				if (part.size() > 3 && !part[3].empty()) patBone = part[3];
			}
		}
		if (patDelay > 0 && --patDelay == 0) { OpenAssetEditor(patPath); patPhase = 1; }
		if (patPhase > 0)
			for (auto& w : assetEds)
				if (w.path == patPath && w.prefabRoot)
				{
					nuke::SkinnedMeshRenderer* smr = nullptr;
					{
						std::function<nuke::SkinnedMeshRenderer*(nuke::Atom*)> find =
							[&](nuke::Atom* a) -> nuke::SkinnedMeshRenderer*
						{
							if (!a) return nullptr;
							if (auto* s = a->GetComponent<nuke::SkinnedMeshRenderer>()) return s;
							for (nuke::Atom* ch : a->children)
								if (auto* s = find(ch)) return s;
							return nullptr;
						};
						smr = find(w.prefabRoot);
					}
					if (patPhase == 1 && ++patFrames > 30)
					{
						nuke::Animator* an = w.prefabRoot->GetComponent<nuke::Animator>();
						nuke::AnimClip* c = an ? nuke::ResDB::getSingleton()->GetClipByName(patClip) : nullptr;
						std::cout << "[PrefabAnimTest]\tanimator=" << (an != nullptr)
						          << " clip=" << (c != nullptr) << " smr=" << (smr != nullptr) << std::endl;
						if (an && c) { an->clipGuid = c->guid; an->playOnStart = true; an->loop = true; }
						if (an && !patMap.empty())
							for (nuke::BoneMap* bm : nuke::ResDB::getSingleton()->boneMaps)
								if (bm && bm->name == patMap) { an->boneMapGuid = bm->guid; break; }
						ToggleAnimPreview(w);
						if (smr) smr->BoneGlobal(patBone, patP0);
						patPhase = 2; patFrames = 0; patPeak = 0.0f;
					}
					else if (patPhase == 2)
					{
						float g[16];
						if (smr && smr->BoneGlobal(patBone, g))
						{
							const float dx = g[12] - patP0[12], dy = g[13] - patP0[13], dz = g[14] - patP0[14];
							const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
							if (d > patPeak) patPeak = d;
						}
						if (++patFrames > 240)
						{
							std::cout << "[PrefabAnimTest]\tbonePeak=" << patPeak
							          << " ok=" << (patPeak > 0.005f ? 1 : 0) << std::endl;
							// Explosion check: every enabled SMR's POSED bounds stay in the same
							// order of magnitude as its bind bounds (heterogeneous binds used to
							// smear variant meshes across the world on the first ApplyPose).
							{
								std::function<void(nuke::Atom*)> walk = [&](nuke::Atom* a)
								{
									if (!a || !a->enabled) return;
									if (auto* s = a->GetComponent<nuke::SkinnedMeshRenderer>())
										if (s->enabled && s->mesh && s->mesh->vertexArray)
										{
											s->mesh->boundsValid = false;
											s->mesh->EnsureBounds();
											auto diag = [](const float* mn, const float* mx)
											{
												const float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
												return std::sqrt(dx * dx + dy * dy + dz * dz);
											};
											nuke::Mesh* src = !s->meshGuid.empty()
											                ? nuke::ResDB::getSingleton()->GetMesh(s->meshGuid) : nullptr;
											float b = 0.0f;
											if (src) { src->EnsureBounds(); b = diag(src->aabbMin, src->aabbMax); }
											const float d = diag(s->mesh->aabbMin, s->mesh->aabbMax);
											std::cout << "[PrefabAnimTest]\tsmr '" << a->GetName()
											          << "' posedDiag=" << d << " bindDiag=" << b
											          << " blown=" << ((b > 0.0f && d > b * 3.0f + 1.0f) ? 1 : 0)
											          << std::endl;
										}
									for (nuke::Atom* ch : a->children) walk(ch);
								};
								walk(w.prefabRoot);
							}
							ToggleAnimPreview(w);
							std::cout << "[PrefabAnimTest]\tdone" << std::endl;
							patPhase = 0;
						}
					}
					break;
				}
		// NUKE_ASSET_SHOT=<asset path>|<png>: open the asset's editor window, let it settle,
		// screenshot the editor (probe runs: verify skeleton/ragdoll/mesh previews visually).
		static int ashotDelay = -2, ashotWait = 0;
		static std::string ashotPath, ashotPng;
		if (ashotDelay == -2)
		{
			const char* e = std::getenv("NUKE_ASSET_SHOT");
			ashotDelay = (e && e[0]) ? 200 : -1;
			if (ashotDelay > 0)
			{
				std::string s = e;
				const size_t bar = s.find('|');
				ashotPath = s.substr(0, bar == std::string::npos ? s.size() : bar);
				ashotPng = bar == std::string::npos ? "asset_shot.png" : s.substr(bar + 1);
			}
		}
		if (ashotDelay > 0 && --ashotDelay == 0) { OpenAssetEditor(ashotPath); ashotWait = 180; }
		if (ashotWait > 0 && --ashotWait == 0)
		{
			nuke::Game::Screenshot(ashotPng);
			std::cout << "[AssetShot]\twrote " << ashotPng << std::endl;
			// Rig-preview coherence dump: the POSED bounds of every skinned part (skeleton /
			// ragdoll editors build the rig by code — scattered parts mean the bind skin
			// never ran or the palettes disagree).
			for (auto& w : assetEds)
				if (w.path == ashotPath && w.pv && w.pv->world)
				{
					const long rid = w.skAtomId ? w.skAtomId : w.rgAtomId;
					if (nuke::Atom* rig = rid ? w.pv->world->GetById(rid) : nullptr)
						for (nuke::Atom* ch : rig->children)
							if (auto* s = ch->GetComponent<nuke::SkinnedMeshRenderer>())
								if (s->mesh && s->mesh->boundsValid)
									std::cout << "[AssetShot]\tpart '" << s->mesh->name
									          << "' y=[" << s->mesh->aabbMin[1] << " .. "
									          << s->mesh->aabbMax[1] << "]" << std::endl;
					break;
				}
			std::cout << "[AssetShot]\tdone" << std::endl;
		}
		// NUKE_DELETE_TEST=<path>: PerformDeletes on the path (folder or file) and report how
		// many live skeletons/clips remain registered — the folder-delete unload probe.
		static int delDelay = -2;
		static std::string delPath;
		if (delDelay == -2)
		{
			const char* e = std::getenv("NUKE_DELETE_TEST");
			delDelay = (e && e[0]) ? 200 : -1;
			if (delDelay > 0) delPath = e;
		}
		if (delDelay > 0 && --delDelay == 0)
		{
			nuke::ResDB* db = nuke::ResDB::getSingleton();
			const size_t sk0 = db->skeletons.size(), cl0 = db->clips.size(), mt0 = db->materials.size();
			PerformDeletes({ delPath });
			std::cout << "[DeleteTest]\tskeletons " << sk0 << " -> " << db->skeletons.size()
			          << ", clips " << cl0 << " -> " << db->clips.size()
			          << ", materials " << mt0 << " -> " << db->materials.size() << std::endl;
			std::cout << "[DeleteTest]\tdone" << std::endl;
		}
		// NUKE_OPEN_PROJECT=<path>: the child inherits the env var, so skip when the target
		// is already open — otherwise the spawned editor relaunches itself forever.
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
		// macOS odoc: a document double-clicked at launch or mid-session arrives as an Apple
		// Event — same switch flow as the picker (dirty-confirm included). No-op elsewhere.
		{
			const std::string od = EditorTakeOpenDocRequest();
			boost::system::error_code odec;
			if (!od.empty() && !bfs::equivalent(bfs::path(od), bfs::path(projectFile), odec))
				RequestProjectSwitch(od);
		}
	}

	// Project hub (booted with no project): the hub is the whole UI, so its popups draw
	// here — the normal frame body below never runs.
	if (projectHubMode)
	{
		ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
		DrawProjectHub();
		DrawNewProjectPopup();
		DrawSwitchConfirmPopup();
		return;
	}

	// Hot-reload shaders + assets edited on disk. Not while booting: the content scan is
	// still writing ResDB on a worker.
	if (!bootLoading && (++hotReloadTick % 30) == 0)
	{
		ResDB::getSingleton()->HotReloadShaders(AppInstance::GetSingleton()->render);
		ResDB::getSingleton()->HotReloadAssets(AppInstance::GetSingleton()->render);
	}

	// PIE: run per-frame game logic while playing. The fixed-step update runs on
	// AppInstance's fixed-frequency thread, gated on playState internally.
	if (AppInstance::GetSingleton()->playState == 1)
		AppInstance::GetSingleton()->currentWorld->Update();

	// Restore the saved selection only once the world is fully loaded — resolving it against
	// a still-growing world would drop it.
	if (pendingSelectId && !bootLoading)
	{
		if (Atom* a = AppInstance::GetSingleton()->currentWorld->GetById(pendingSelectId))
			AppInstance::GetSingleton()->selectedInHieararchy = a;
		pendingSelectId = 0;
	}
	// Order matters: menu, toolbar, status bar, then the dock space — each reserves
	// viewport work-area for the next.
	EditorMenu();
	Toolbar();
	StatusBarPanel();
	// PassthruCentralNode leaves the centre transparent for the scene viewport.
	ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
	window_flags = 0; // panels are dockable/movable now
	for (auto tup : *AppInstance::GetSingleton()->editorWindows)
		tup.second();

	winSettings();        // Project Settings window (default world + hotkeys)
	winWorldSettings();   // World Settings window (global shadows etc., saved in the .nuworld)
	winPreferences();     // engine-wide Preferences (external editor etc., %APPDATA% scope)
	winProfiler();        // live CPU/GPU phase breakdown
	winHistory();         // edit-history timeline (click = jump)
	winTextEditor();      // Text editor: opened from the browser / asset inspector
	winAssetEditors();    // Asset editors: material / mesh / prefab windows
	DrawSaveAsPopup();    // "Save World As" modal
	DrawNewProjectPopup();      // "New Project" modal (File menu)
	DrawPackageModPopup();      // "Package Mod" modal (pick the mod's name)
	DrawPackageDlcPopup();      // "Package DLC" modal (name + the base pak to diff against)
	DrawPackageProjectPopup();  // "Game Build" modal (game boot settings, then pack)
	DrawSwitchConfirmPopup();   // unsaved-world guard before a project switch
	TrackUndo();          // capture a selected-atom edit for undo when the UI settles
	TrackDirty();         // refresh the dirty "*" marker
	TrackExternalChange();// detect a disk edit of the open world
	DrawReloadPopup();    // disk changed (editor clean) -> reload?
	DrawConflictPopup();  // disk changed + editor dirty -> reload/overwrite/merge/ignore
	DrawMergeWindow();    // the resolve window
	DispatchHotkeys();    // fire any pressed hotkey chord (after the UI, so fields take input first)
	ApplyPendingAtomDelete();   // queued hierarchy deletion, now that no tree walk holds the atom

	// Apply queued plugin toggles AFTER the window loop: DisablePlugin()'s Shutdown may
	// PopWindow (mutating editorWindows), which would invalidate the iterator above.
	if (!pendingPluginToggle.empty())
	{
		for (auto& pt : pendingPluginToggle)
		{
			// PHASE_BOOT providers can't be swapped live: enabling one only persists the
			// choice for next start, and a live one can't be turned off at all.
			if (pt.first->phase() == nuke::PHASE_BOOT)
			{
				if (pt.second && *pt.first->provides())
				{
					serviceChoices[pt.first->provides()] = nuke::ModuleName(pt.first->moduleFile);
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
	// Non-world commands inherit the current world serial, so they don't flip the dirty state.
	const long ws = worldEdit ? ++editSerial : WorldEditSerial();
	undoStack.push_back({ std::move(undoFn), std::move(redoFn), label, ws });
	if (undoStack.size() > 200) { undoStack.erase(undoStack.begin()); ++undoTrimmed; }
	redoStack.clear();
	idleSnapValid = false;   // any recorded edit may have touched the selected subtree
}

void EditorUI::ResetUndo() { undoStack.clear(); redoStack.clear(); undoTrimmed = 0; editing = false; editAtomId = 0; editBefore.clear(); idleSnap.clear(); idleAtomId = 0; idleSnapValid = false; }

void EditorUI::Undo()
{
	// Focused editor windows own their history: text widgets handle Ctrl+Z themselves,
	// asset editors route to their per-window snapshot stack.
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

// Replace the atom (by id) with the given serialized state at a placement (parentId 0 = root).
// Empty json = remove the atom.
void EditorUI::ApplyAtomState(long id, long parentId, int index, const std::string& json)
{
	World* w = AppInstance::GetSingleton()->currentWorld;
	w->RemoveAtomById(id);
	if (!json.empty()) { if (Atom* a = LoadAtomFromString(json)) w->InsertAtom(a, parentId, index); }
	AppInstance::GetSingleton()->selectedInHieararchy = w->GetById(id);   // null if it was removed
	editing = false; editAtomId = 0;                                      // don't re-capture this change
}

void EditorUI::TrackUndo()
{
	AppInstance* app = AppInstance::GetSingleton();
	if (app->playState != 0) return;   // never track during PIE
	ImGuiID activeId = ImGui::GetActiveID();
	bool active = (activeId != 0) || ImGuizmo::IsUsing();
	Atom* sel = app->selectedInHieararchy;
	long  selId = sel ? sel->id.id : 0;

	// Also flush when focus jumps straight to a DIFFERENT widget: with no idle frame between,
	// both props would merge into one undo step. The gizmo isn't an ImGui item (activeId 0).
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
	// "before" = the just-flushed "after" when chaining from another widget, else the last idle
	// snapshot — a same-frame snapshot would already include a slider's click-to-position jump.
	if (active && !editing)
	{
		editing      = true;
		editAtomId   = selId;
		editActiveId = activeId;
		if (focusChanged && !flushedAfter.empty())          editBefore = flushedAfter;
		else if (selId && selId == idleAtomId && !idleSnap.empty()) editBefore = idleSnap;
		else                                                editBefore = sel ? SaveAtomToString(sel) : std::string();
	}
	// Idle snapshot of the selected atom = the "before" for the NEXT edit. Refreshed on demand
	// only (selection change / invalidation): serializing a big subtree every frame is too slow.
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

void EditorUI::RecordReparent(Atom* a, long oldParent, int oldIndex, const std::string& beforeJson)
{
	if (!a) return;
	World* w = AppInstance::GetSingleton()->currentWorld;
	long id = a->id.id, newParent = AtomParentId(a); int newIndex = AtomIndex(w, a);
	if (oldParent == newParent && oldIndex == newIndex) return;
	// Two snapshots: keep-world rewrote the locals, so a single shared JSON would restore the wrong pose.
	std::string json = SaveAtomToString(a);
	PushUndo("Reparent " + a->GetName(),
		[this, id, oldParent, oldIndex, beforeJson]{ ApplyAtomState(id, oldParent, oldIndex, beforeJson); },
		[this, id, newParent, newIndex, json]      { ApplyAtomState(id, newParent, newIndex, json); });
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
	app->selectedInHieararchy = nullptr;
	// Deleting here would free the atom while the hierarchy is iterating over it (the context
	// menu is drawn mid-walk): the request is applied at the end of the frame instead.
	pendingDeleteId = a->id.id;
}

// Applies a queued deletion once no tree walk is standing on the atom. Called at the end of the
// editor frame; safe to call when nothing is queued.
void EditorUI::ApplyPendingAtomDelete()
{
	AppInstance* app = AppInstance::GetSingleton();
	if (pendingDeleteId)
	{
		const long id = pendingDeleteId;
		pendingDeleteId = 0;
		if (app && app->currentWorld) app->currentWorld->RemoveAtomById(id);   // atom + its subtree
	}
	if (!pendingDeleteIds.empty())
	{
		std::vector<long> ids;
		ids.swap(pendingDeleteIds);
		if (app && app->currentWorld)
			for (long id : ids) app->currentWorld->RemoveAtomById(id);
	}
}

// ---- atom clipboard (copy / cut / paste / duplicate) ----
// Atoms travel as a JSON envelope on the OS clipboard, so stray clipboard text is never
// mistaken for an atom. Only the pasted root gets a uniquified name.

static std::string AtomClipEnvelope(const std::string& atomJson)
{ return std::string("{\"nukeClipboard\":\"atom\",\"atom\":") + atomJson + "}"; }

// "Box" -> "Box (2)" when a sibling at the destination already holds the name (first free N).
// The existing counter is stripped first: "Box (2)" -> "Box (3)", not "Box (2) (2)".
static void UniquifySiblingName(World* w, Atom* a, long parentId)
{
	Atom* parent = parentId ? w->GetById(parentId) : nullptr;
	auto& lst = parent ? parent->children : w->GetHierarchy();
	auto taken = [&](const std::string& n)
	{ for (Atom* s : lst) if (s && s != a && s->GetName() == n) return true; return false; };
	if (!taken(a->GetName())) return;
	std::string base = StripNameCounter(a->GetName());
	for (int n = 2; ; ++n)
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
	// Placement: sibling after the selection; nothing selected -> end of the root level.
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
