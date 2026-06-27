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
#include "input/Hotkeys.h"     // centralized hotkey pool (editor + plugins)
#include <boost/container/list.hpp>
#include <boost/bind/bind.hpp>
#include <cstring>
#include <nlohmann/json.hpp>   // editor_state.json (editor-side state, not world state)
#include <boost/filesystem/fstream.hpp>   // boost file streams (project's stack — not std)
#include <map>
#include <functional>
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

// Register the .nuproj file extension (HKEY_CURRENT_USER) so double-clicking a project opens it in
// this editor. User-scope + reversible; defined in main.cpp (isolates <windows.h>). Returns success.
bool RegisterProjectFileAssociation();

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
	std::string browserSel;                        // selected entry (full path; "" = none)
	std::vector<std::string> browserBack, browserFwd;   // folder navigation history (M4=back, M5=forward)
	std::vector<std::string> clipboard;            // browser cut/copy buffer (full paths)
	bool        clipboardCut = false;              // true: paste MOVES (cut); false: paste COPIES
	std::string renamePath;                        // browser: full path being renamed ("" = none)
	char        renameBuf[256] = "";               // edited NAME (without extension)
	std::string renameExt;                         // locked extension (kept as-is; "" for folders)
	bool        openRenamePopup = false;           // request to open the rename modal next frame
	int         hotReloadTick = 0;                  // throttles shader hot-reload checks
	std::map<std::string, std::function<void(nuke::Component*)>> inspectorOverrides;  // per-type custom inspector drawing
	char        assetFilter[128] = "";             // filter text in the asset-picker popup
	char        hierSearch[128] = "";              // hierarchy search (atom name / component type)
	// Deferred reparent (applied AFTER the tree is drawn — mutating the lists mid-iteration corrupts it).
	Atom* dndAtom = nullptr; Atom* dndBefore = nullptr; Atom* dndParent = nullptr; bool dndPending = false;
	std::string dndAsset; Atom* dndAssetParent = nullptr;   // deferred: instantiate a browser asset (then parent)
	std::string rebindId;                          // hotkey id currently being rebound ("" = none)
	bool        settingsOpen = false;              // Project Settings window open?
	bool        openSaveAsPopup = false;           // request to open the "Save World As" modal
	char        saveAsBuf[256] = "";               // edited world FILE name
	std::string saveAsDir;                         // chosen target folder (full path) in the save dialog
	std::map<std::string, int> pendingHotkeyBinds; // hotkey bindings from the .nuproj, applied after plugins load
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

	// --- panel methods (definitions in src/panels/*.cpp) ---
	// project
	void SaveProject();
	void LoadProject();
	void ApplyProjectPlugins();
	void SyncEnabledPlugins();
	void SaveEditorState();
	void LoadEditorState();
	// setup_menu
	void SetUp();
	void ApplyStyle();
	void InitMenu();
	bool EditorSubMenu(MenuItem* item);
	void EditorMenu();
	static void TogglePluginMGR();   // menu callback (static → plain fn pointer)
	// Hotkeys + world commands + project settings.
	void RegisterHotkeys();          // register the editor's built-in hotkeys in the shared pool
	void DispatchHotkeys();          // fire bound hotkeys whose chord is pressed (one per chord)
	void MenuHotkeyItem(const char* label, const char* id);   // menu entry driven by a pooled hotkey
	void SetProjectFile(const std::string& path);   // point the editor at a specific .nuproj (CLI/open-with)
	void NewWorldCmd();              // New World (keeps the editor camera)
	void SaveWorldCmd();             // save the current world (to its path, or the project default)
	void SaveWorldAsCmd();           // open the "Save World As" modal (pick name/location)
	void DrawSaveAsPopup();          // the modal itself (drawn each frame)
	void SaveAsFolderTree(const std::string& dir);   // recursive folder tree (pick the save folder)
	void OpenWorldCmd(const std::string& relPath);            // open a world from project content
	void OpenWorldFromBrowser(const std::string& fullPath);   // open a .nuworld picked in the browser
	void winSettings();              // Project Settings window (default world + hotkeys)
	// hierarchy
	void winHierarchy();
	void DrawAtomNode(Atom* go);                 // one tree row + DnD (recurses children)
	void HierGap(Atom* before);  // thin insertion zone overlaid on a row's top edge (only while dragging an atom)
	bool HierMatch(Atom* go);                    // search: atom name OR a component type matches
	bool HierMatchDeep(Atom* go);                // this atom or any descendant matches
	const char* AtomIcon(Atom* go);              // icon by the atom's components
	void FocusSelected();                        // frame the selected atom with the editor camera
	// inspector
	void CamComponent(Camera* cam);
	// Reusable asset-reference picker (mesh/material/shader/texture). Type-locked (rejects other
	// kinds), DnD target from the browser, "locate original" + "reset to default" buttons, and a
	// filterable popup list of every asset of that type in the project. Same-named files in different
	// folders are fine — assets are keyed by GUID. Returns true when the value changed.
	bool AssetPicker(const char* label, std::string& guid, const std::string& kind, const std::string& defGuid = "");
	void RegisterInspectorOverrides();
	void DrawMeshRendererInspector(nuke::MeshRenderer* mr);
	bool DrawFields(void* obj, nuke::TypeInfo* ti);
	void DrawDynamicProps(nuke::Component* cmp);
	bool EditV3(const char* rowLabel, double v[3]);
	void winInspector();
	// viewport
	void winRender();
	// dialogs
	void winAbout();
	void winConsole();
	// browser
	const char* ExtIcon(const std::string& ext);
	bool ExtVisible(const std::string& ext);
	bool SearchMatch(const std::string& name);
	void BrowserTree(const std::string& dir);
	Atom* InstantiatePrefab(const std::string& path);
	void StartRename(const std::string& path);
	void EntryContextMenu(const std::string& path, bool isDir);
	void DrawRenamePopup();
	void winBrowser();
	void BrowserNavigate(const std::string& path);   // change folder + push history (clears forward)
	void BrowserBack();                              // M4 / back button
	void BrowserForward();                           // M5 / forward button
	// Drag & drop: drag a browser entry (payload "NUKE_ASSET" = full path); drop on a folder to move,
	// or on the viewport / hierarchy to instantiate.
	void BrowserDragSource(const std::string& path);
	void BrowserFolderDropTarget(const std::string& folderPath);
	void SaveAtomAsPrefab(Atom* a, const std::string& folder);   // drag an atom into the browser -> .nuprefab
	void BrowserPaste();                                          // paste the clipboard into the current folder (cut=move, copy=duplicate)
	void AcceptAssetDropTarget();                    // viewport/hierarchy: accept an asset drop
	Atom* DropAsset(const std::string& path);        // instantiate by extension; returns the new atom (or null)
	Atom* SpawnMeshAsset(const std::string& path);   // .numesh -> new Atom + MeshRenderer
	// Create new assets in a content folder (from the browser's "New" menu).
	void CreateFolderAsset(const std::string& folder);
	void CreateWorldAsset(const std::string& folder);    // empty .nuworld
	void CreateMaterialAsset(const std::string& folder); // default .numat (registered in ResDB)
	void CreateShaderAsset(const std::string& folder);   // .vs/.ps.hlsl pair (registered + pipeline built)
	// plugins
	void PluginMGRWindow();
	// toolbar
	bool ToolBtn(const char* icon, const char* tip, bool active, float w);
	void SpawnEmpty();
	void SpawnPrimitive(const char* atomName, const char* guid);
	void SpawnCube();
	void SpawnCamera();
	void Toolbar();
	void Draw();
};

inline void editorinit() { EditorUI::getSingleton()->SetUp(); }
inline void editorDraw() { EditorUI::getSingleton()->Draw(); }

#endif // EDITORUI_H
