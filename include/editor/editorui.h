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
#include "API/Model/PostProcess.h"   // custom post-effect chain inspector
#include "API/Model/UnknownComponent.h"
#include "API/Model/resdb.h"   // asset database (meshes by GUID, browser)
#include "import/assimporter.h" // external model import -> native .numesh
#include "API/Model/Prefab.h"   // instantiate .nuprefab assets
#include "reflect/Reflect.h"   // auto-inspector: draw component fields from the schema
#include "API/Model/Time.h"    // per-frame delta/elapsed
#include "API/Model/Log.h"     // console panel: the engine log ring
#include "editor/exteditor.h"  // external editor detection/launch (Preferences)
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
std::string EditorPickIconFile();   // .ico picker (game icon, Project Settings -> Packaging)
std::string EditorPickFolder();     // native folder picker (build path, Project Settings -> Packaging)
std::string EditorPickProjectFile();// .nuproj / .nupak / .numod picker (File -> Open Project)
std::string EditorPickExeFile();    // .exe picker (Preferences -> custom external editor)
bool        EditorRelaunch(const std::string& projectPath);   // spawn a new editor on that project

class TextEditor;   // vendored ImGuiColorTextEdit (src/textedit), compiled into the editor

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
	long pendingSelectId = 0;           // atom id to reselect after load (from editor_state.json; recursive)
	int  browserView = 0;               // asset browser: 0 Tiles, 1 List, 2 Tree, 3 By Type
	char browserSearch[128] = "";
	bool fMesh = true, fMat = true, fTex = true, fPrefab = true;   // browser type filters
	std::string contentDir = "project/content";   // project content root (imported assets live here)
	std::string browserCwd;                        // current folder shown in the browser
	std::string browserSel;                        // selected entry (full path; "" = none)
	std::vector<std::string> browserBack, browserFwd;   // folder navigation history (M4=back, M5=forward)
	std::vector<std::string> clipboard;            // browser cut/copy buffer (full paths)
	bool        clipboardCut = false;              // true: paste MOVES (cut); false: paste COPIES
	std::string pendingDelete;                     // browser: path awaiting delete-confirm ("" = none)
	bool        openDeletePopup = false;           // request to open the delete-confirm modal next frame
	std::vector<std::string> deleteDeps;           // resources depending on pendingDelete (shown in the modal)
	bool        unlinkOnDelete = false;            // project setting: break refs to a deleted resource
	// Disk<->editor sync of the open world. worldOnDisk = canonical JSON of the last loaded/saved state
	// (dirty baseline + the on-disk reference); worldDirty drives the "*" in the title + browser.
	std::string worldOnDisk;
	bool        worldDirty = false;
	long long   worldMtime = 0;                    // last-known disk mtime of the world file
	int         dirtyTick = 0, extTick = 0;        // throttles
	int         reloadCleanMode = 0;               // disk changed, editor clean: 0=ask, 1=auto-reload
	int         conflictMode    = 0;               // disk changed, editor dirty: 0=ask,1=reload,2=overwrite,3=merge
	int         msaaSamples     = 4;               // anti-aliasing sample count (1=off,2,4,8); applied via render->setMSAA
	bool        hdrEnabled      = true;            // HDR pipeline on/off; applied via render->setHDR
	float       hdrPaperWhite   = 200.0f;          // HDR10 diffuse-white nits (display mapping)
	float       hdrPeak         = 1000.0f;         // HDR10 highlight peak nits
	std::string pendingDisk;                        // disk JSON awaiting a reload/conflict decision
	bool        openReloadPopup = false, openConflictPopup = false;
	bool        wasWindowFocused = true;            // disk re-check fires on focus-gain (avoids mid-write triggers)
	Vector3     camFocusTarget;                      // smooth "focus selected": target editor-cam position
	bool        camFocusing = false;
	bool        mergeOpen = false;                  // merge/resolve window visible
	std::shared_ptr<void> mergeState;               // opaque diff tree (built in panels/merge.cpp)
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
	// Undo/redo: a GENERIC command stack. Each undoable action pushes its OWN inverse closures, so it
	// covers anything that changes — atom edits, spawns, reparenting, project/editor settings, paths —
	// not just atoms. Atom edits are captured as a delta (the affected atom subtree's before/after
	// JSON), never the whole world. Use PushUndo / RecordChange<T> at any mutation site.
	struct UndoCmd { std::function<void()> undo, redo; std::string label; long serial = 0; };
	std::vector<UndoCmd> undoStack, redoStack;
	// Monotonic edit id: every pushed command gets one; the world is DIRTY when the id on
	// top of the undo stack differs from the id recorded at the last save/load. This is
	// the whole dirty check — serializing the world to diff it (the old way) froze the
	// editor 4x/second on big scenes.
	long editSerial = 0;
	long savedWorldSerial = 0;
	long WorldEditSerial() const { return undoStack.empty() ? 0 : undoStack.back().serial; }
	std::string editBefore; long editAtomId = 0; bool editing = false;   // selected-atom edit detector
	unsigned int editActiveId = 0;   // ImGui active-widget id of the in-progress edit (flush when it changes)
	std::string  idleSnap; long idleAtomId = 0;   // last snapshot taken while NOTHING was being edited = true pre-edit "before"
	bool idleSnapValid = false;   // refresh ON DEMAND (selection change / after an edit) — NOT per
	                              // frame: serializing a selected 200-atom subtree every frame was
	                              // the "editor stutters, Player is fine" freeze
	Atom* pendingCompAtom = nullptr; Component* pendingCompDel = nullptr;   // deferred component removal
	std::string rebindId;                          // hotkey id currently being rebound ("" = none)
	bool        settingsOpen = false;              // Project Settings window open?
	bool        openSaveAsPopup = false;           // request to open the "Save World As" modal
	char        saveAsBuf[256] = "";               // edited world FILE name
	std::string saveAsDir;                         // chosen target folder (full path) in the save dialog
	std::map<std::string, int> pendingHotkeyBinds; // hotkey bindings from the .nuproj, applied after plugins load
	std::string projectDir  = "project";           // project root
	std::string projectFile = "project/game.nuproj";
	std::string projectName = "NukeGame";          // .nuproj "name" (dist/pak naming)
	// Packaging (3.2): compression of the project pak (immutable release artifact; zstd max
	// by default) and of mod paks (editable; store by default). Persisted in the .nuproj.
	int pakMethod = 2, pakLevel = 22;              // 0 store / 1 zlib / 2 zstd
	int modMethod = 0, modLevel = 0;
	std::string gameIcon;                          // .ico (content-relative) stamped onto the shipped exe
	std::string distPath;                          // build output ("" = default: <project>/dist; abs or project-relative)
	uint64_t    iconPrevTex = 0;                   // live preview texture of gameIcon (settings window)
	std::string iconPrevPath;                      // which path iconPrevTex was decoded from
	// Decode the best image of an .ico into RGBA8 (PNG-compressed and 32-bpp DIB entries).
	static bool DecodeIcoRGBA(const std::string& path, std::vector<unsigned char>& rgba, int& w, int& h);
	// New Project modal state (File -> New Project...). Creation + switch happen on OK:
	// scaffold <location>/<name>/game.nuproj + content/, relaunch the editor on it.
	bool openNewProjectPopup = false;
	char newProjName[128] = "MyGame";
	std::string newProjDir;
	void DrawNewProjectPopup();
	void OpenProjectCmd();                         // File -> Open Project... (raw or packed)
	void RequestProjectSwitch(const std::string& path);   // confirm unsaved world, then switch
	void DrawSwitchConfirmPopup();
	std::string pendingSwitchPath;                 // project awaiting the unsaved-world decision
	bool openSwitchConfirm = false;
	void SwitchToProject(const std::string& path); // relaunch this editor on `path` + close
	// Set when this project was opened FROM an archive (.nupak/.numod): the source pak.
	// Drives Package Mod (diff vs a .nupak base; in-place repack of a .numod).
	std::string basePakPath;
	// Package Mod modal state (File -> Package Mod...). The chosen name persists in the
	// work manifest ("modName") so repacking updates the SAME mod; a new name = a new file.
	bool openPackageModPopup = false;
	char packModName[128] = "";
	std::string modName;                           // last packaged mod name (from the .nuproj)
	// Mods panel (Project Settings, mounted-pak session): the game's mod list — enable/
	// disable + order writes config/mods.json; mounts happen at boot -> apply reloads the
	// session. Rows come from the config (order = load order) + disabled files in mods/.
	struct ModRow
	{
		std::string file, path, name, req;         // req = display string of `reqs`
		std::vector<std::string> reqs;             // dependency names from mod.json
		bool enabled = false, mounted = false, found = true;
		bool reqOk = true;                         // all requirements enabled+loadable (per frame)
		bool edMounted = false;                    // mounted in THIS editor session (separate list)
	};
	std::vector<ModRow> modsUi;
	int  modsUiTick = -1;                          // frame-count throttle for rescans
	void ScanModsUi();                             // rebuild modsUi (config + mods/ dir + manifests)
	void SaveModsUi();                             // write enabled rows (in order) to config/mods.json
	// The EDITOR's own mod selection (config/mods.json is the PLAYER's list): persisted as
	// editor_mods.json in the session overlay; saving REMOUNTS the stack live (base + the
	// selection) — reopen the world to see the merge.
	void SaveEditorMods();
	std::string GameRootFromBase() const;          // the game dir the session's pak belongs to
	// Console (viewer over the engine's Log ring — cout/cerr are captured into it).
	uint64_t conVersion = ~0ull;                   // last seen Log::Version (cheap change check)
	std::vector<nuke::LogEntry> conCache;          // snapshot, refreshed when the version moves
	bool conShow[3] = { true, true, true };        // info / warn / error visibility
	char conFilter[128] = "";                      // substring filter (tag + text)
	bool conAutoScroll = true;
	void OpenLogSource(const nuke::LogEntry& e);   // double-click: resolve + jump to file:line
	// Preferences (MACHINE-wide, %APPDATA%/NukeEngine/preferences.json — not per-project).
	bool prefsOpen = false, prefsFocus = false;
	std::vector<ExtEditor> extEditors;             // detected external editors + "Custom"
	std::string extEditorName;                     // the chosen one (persisted by name; "" = built-in)
	std::string extCustomExe, extCustomArgs;       // the "Custom" entry ({file}/{line} template)
	void LoadPreferences();
	void SavePreferences();
	void winPreferences();
	// Open file:line in the user's chosen editor (falls back to the built-in text editor).
	void OpenExternal(const std::string& file, int line);
	std::string startupWorld = "scene.nuworld";    // from the .nuproj
	std::string lastWorld;                         // from editor_state.json: world open when the editor last exited
	std::vector<std::string> enabledPlugins;       // per-project plugin load list (dll names)
	bool pluginListLoaded = false;                 // did the .nuproj specify a plugin list?
	// Per-project service provider choice (unified plugin model): service -> dll name, e.g.
	// "render" -> "NukeRenderDiligent.dll". Boot services (render) apply on next start.
	std::map<std::string, std::string> serviceChoices;
	std::vector<std::pair<nuke::NUKEModule*, bool>> pendingPluginToggle;   // applied after the window loop
	char        pluginFilter[128] = "";            // plugin window: text search over name + tags
	int         pluginServiceFilter = 0;           // plugin window: 0=All, 1=Utility, 2+=service index
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
	// Read one "services.<service>" choice straight from the .nuproj — used during phase-1
	// boot, BEFORE LoadProject() (the render provider must come up before any UI exists).
	// Returns "" when the project/key doesn't exist (caller falls back to the first provider).
	std::string EarlyProjectService(const std::string& service);
	void SaveProject();
	void LoadProject();
	// Packaging (3.2, packaging.cpp): build the full release dist/ (player + deps + used
	// modules + config + the project as content/game.nupak), or a mod overlay (.numod).
	// Each mod is its OWN file: the modder picks the name in a modal (DrawPackageModPopup),
	// the same name repacks/updates that mod, a new name creates a separate .numod.
	void PackageProject();           // Release build first (stale binaries never ship), then dist
	void PackageProjectNow();        // the packaging body itself (no build step)
	// Editor-driven builds (the unified root superbuild): `cmake --build <repo>/build
	// --config <cfg>` on a Jobs WORKER — output streams into the Console line by line,
	// progress ticks the status bar, msbuild /m parallelizes independent projects.
	// Skipped with a note when the asked config is the one this editor is RUNNING
	// (its binaries are locked). onDone fires on the game thread.
	void RunEngineBuild(const std::string& config, std::function<void(bool)> onDone);
	void PackageMod(const std::string& name = "");   // "" -> last used / project name (dev hook)
	void PackageModCmd();            // open the "Package Mod" modal (prefills the name)
	void DrawPackageModPopup();      // the modal itself (drawn each frame)
	// Open-with support (3.2). A .nupak NEVER extracts (the packed project is read-only —
	// an unpacked tree would be an unprotected copy anyone could repackage): it is MOUNTED
	// at runtime, Bethesda-style, and edits land in a "<stem>_mod" OVERLAY dir beside it
	// that holds ONLY the modder's files. A .numod (editable by design) extracts into
	// "<stem>_project" for full editing + in-place repack. Both return the work project's
	// .nuproj ("" on failure) and record the base pak for PackageMod.
	static std::string PrepareMountedProject(const std::string& pakAbs);   // .nupak
	static std::string PrepareArchiveProject(const std::string& pakAbs);   // .numod
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
	// World switches requested mid-frame (menu/browser clicks land inside the UI pass)
	// are QUEUED and applied here, first thing in the render callback — tearing a world
	// down mid-command-list makes D3D12 fail to close the list (render safety).
	std::string pendingWorldOpen;
	void ApplyPendingWorldOpen();
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
	void DrawPostProcessInspector(nuke::PostProcess* pp);
	void DrawAnimatorInspector(nuke::Animator* an);   // serialized state machine (3.1)
	void winWorldSettings();   // World Settings window (global shadow settings, saved in the .nuworld)
	bool worldSettingsOpen = false;
	bool worldSettingsFocus = false;   // focus the window only when opened via menu, not when restored on load
	World::Settings wsBefore;  // pre-edit snapshot of world settings (idle baseline for undo)
	bool wsEditing = false;

	// Project Settings undo: a snapshot of the value-based project settings (rendering + RTX + disk sync).
	// Same idle-snapshot -> PushUndo-on-settle scheme as world settings. Backend (restart-required) and the
	// Default World (own undo) are intentionally NOT part of this snapshot.
	struct ProjectSettings {
		int   msaa; bool hdr; float paperWhite, peak;
		float rtIntensity, rtMaxDist; int rtBounces; float rtRoughCutoff;
		int   reloadClean, conflict;
	};
	ProjectSettings psBefore{};   // idle baseline for undo
	bool psEditing = false;
	void ApplyProjectSettings(const ProjectSettings& ps);   // set members+config, push to renderer, persist (nuproj+config)
	bool DrawFields(void* obj, nuke::TypeInfo* ti);
	void DrawDynamicProps(nuke::Component* cmp);
	bool EditV3(const char* rowLabel, double v[3]);
	void winInspector();
	// Asset inspector: when nothing is selected in the Hierarchy but a project file is selected in the Browser,
	// the Inspector shows that asset's properties (texture usage/info, material fields, or read-only info + Open).
	void DrawAssetInspector(const std::string& path);
	std::string     inspAssetPath;                 // cache key: asset currently loaded into the inspector
	long long       inspAssetMtime = 0;            // + its last-write time, so a reimport (same path) reloads
	nuke::Texture*  inspTex = nullptr;             // cached loaded .nutex (usage editing)
	nuke::Material* inspMat = nullptr;             // cached loaded .numat (field editing)
	// text editor (2.2): tabs of open text files; syntax from the file-type descriptor (0.6);
	// Ctrl+S saves; saved shaders/materials hot-reload through the existing mtime watcher.
	struct TextDoc
	{
		std::string path;                       // full path on disk
		std::shared_ptr<TextEditor> ed;         // editor instance (owns text + undo)
		int  savedUndoIndex = 0;                // undo index at last save -> dirty when it differs
		bool wantFocus = false;                 // select this tab on the next draw
		bool open = true;                       // tab close-button state
	};
	std::vector<TextDoc> textDocs;
	bool textEditorOpen  = false;               // window visibility (opened on demand)
	int  textCloseConfirm = -1;                 // doc index awaiting the discard-changes modal
	bool IsTextFile(const std::string& ext);    // descriptor textEditable OR a known text extension
	void OpenTextFile(const std::string& path, int line = 0); // open (or focus) a file; line > 0 jumps there
	void SaveTextDoc(TextDoc& d);
	void winTextEditor();

	// --- shared 3D-preview infrastructure: POOLED mini-worlds (sky + shadowless sun +
	// one mesh atom + camera into an own RT). Pooled because the render seam has no
	// destroyRenderTarget — closed editors return their scene for reuse. Each World
	// pushes its own globals per Render() call, so extra worlds never taint the scene.
	struct PreviewScene
	{
		World*        world = nullptr;
		uint64_t      rt = 0;
		Camera*       cam = nullptr;
		Atom*         meshAtom = nullptr;   // holds the MeshRenderer below
		MeshRenderer* mr = nullptr;
		float   yaw = 0.7f, pitch = 0.35f;  // orbit (LMB drag on the preview image)
		float   dist = 0.0f;                // dolly (wheel); 0 = re-frame from bounds
		Vector3 center;                     // framed bounds
		float   radius = 1.0f;
		int     rtW = 384, rtH = 384;       // current RT size — follows the on-screen size (crisp)
		bool    visible = false;            // wants a render this frame (consumed by the hook)
		bool    inUse = false;
		ImVec2  rectMin, rectSize;          // last drawn screen rect (gizmo overlay target)
		int     wantW = 0, wantH = 0;       // debounced resize request (see DrawPreviewImage)
		int     wantFrames = 0;
		// The overlay gizmo is hovered/grabbed (set by the CALLER inside its ImGuizmo ID
		// scope — IsUsing()/IsOver() queried outside the PushID scope lie). Orbit is
		// suppressed while true.
		bool    gizmoBusy = false;
	};
	std::vector<PreviewScene*> pvPool;      // every created scene (in use or free)
	PreviewScene* AcquirePreview();
	void ReleasePreview(PreviewScene* s);
	// The interactive 3D view: fills exactly `size` (any aspect — the RT follows it),
	// LMB drag orbits (captured — never drags the window), wheel dollies.
	void DrawPreviewImage(PreviewScene& s, ImVec2 size);
	void FramePreview(PreviewScene& s, Atom* subtree);       // bounds of a subtree (or the mesh atom) -> center/radius

	// Inspector's asset preview (one pooled scene, staged by browser selection).
	uint64_t inspTexPreviewId = 0;          // GPU texture of the decoded .nutex (destroyTexture2D on change)
	PreviewScene* inspPv = nullptr;
	std::string   pvStaged;                 // asset path currently staged ("" = none)
	void StageAssetPreview(const std::string& path, const std::string& ext);
	void DrawAssetPreview3D(const std::string& path, const std::string& ext);   // inspector widget
	void RenderAssetPreview(iRender* r);    // render hook: draws every visible preview scene BEFORE the live scene

	// --- ASSET EDITORS: each .numat / .numesh / .nuprefab opens its OWN window with a
	// live 3D view (own preview scene) and type-specific editing; saves back to the file.
	struct AssetEditorWin
	{
		std::string path, ext;              // full path + lowercase extension
		PreviewScene* pv = nullptr;
		Material* mat = nullptr;            // .numat: owned editing copy (saved to the file)
		Atom*     prefabRoot = nullptr;     // .nuprefab: loaded subtree (lives in pv->world)
		long      prefabSelId = 0;          // selected atom in the prefab tree (stable id)
		int       previewMesh = 0;          // .numat: 0 sphere / 1 cube / 2 plane
		int       gizmoOp = 1;              // prefab 3D view: 0 none / 1 move / 2 rotate / 3 scale
		bool      gizmoWorld = true;        // gizmo space: world / local (X toggles, like the viewport)
		long      pendingDeleteId = 0;      // tree ops applied AFTER the tree walk
		long      pendingAddParentId = 0;
		// Animation preview (3.1): the ▶ toggle ticks the subtree's Animators each frame;
		// the pose snapshot taken at play start restores the prefab on stop (mini-PIE).
		bool        animPlay = false;
		std::string animSnap;
		// Audio preview (ogg/wav/mp3/flac): the live Preview-bus voice of this window.
		uint64_t audioVoice = 0;
		float    audioVol = 1.0f;
		float     gizmoMtx[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };   // persistent during a drag
		// Per-WINDOW undo/redo: Ctrl+Z/Ctrl+Y route here while this window is focused
		// (EditorUI::Undo/Redo check aeFocused). Snapshots — prefab: atom-subtree JSON;
		// material: owned clones. An edit BURST (drag) coalesces into ONE entry via the
		// idle-baseline latch, same idiom as the scene's TrackUndo.
		std::vector<std::string> undoP, redoP;   // .nuprefab history
		std::vector<Material*>   undoM, redoM;   // .numat history (owned clones)
		std::string idleP;                       // pre-edit baseline (prefab JSON)
		Material*   idleM = nullptr;             // pre-edit baseline (material clone)
		bool      editing = false;               // edit burst in progress (coalescing)
		bool      editedNow = false;             // an edit happened THIS frame (latch input)
		bool dirty = false, open = true, wantFocus = false;
	};
	std::vector<AssetEditorWin> assetEds;
	int  aeCloseConfirm = -1;               // editor index awaiting the discard-changes modal
	int  aeFocused   = -1;                  // asset-editor window focused THIS frame (undo routing)
	int  textFocused = -1;                  // text-editor window focused THIS frame (undo routing)
	void OpenAssetEditor(const std::string& path);   // open (or focus) the editor for an asset
	void winAssetEditors();
	void AssetEditorUndo(AssetEditorWin& w);
	void AssetEditorRedo(AssetEditorWin& w);
	void ToggleAnimPreview(AssetEditorWin& w);   // prefab editor ▶/■ (mini-PIE, snapshot-restored)
	void TickAnimPreview(AssetEditorWin& w);     // tick the subtree's Animators while playing
	void DrawPrefabTree(AssetEditorWin& w, Atom* a);        // recursive tree rows (select by id)
	bool DrawPrefabAtomEditor(AssetEditorWin& w, Atom* a);  // transform + components; true if edited

	// viewport
	void winRender();
	// Billboard icons for INVISIBLE entities (camera / light / probe / environment):
	// screen-space Lucide glyphs (same mapping as the hierarchy) overlaid on the viewport
	// image in EDIT mode — projected with the exact gizmo view/proj, clickable to select.
	void DrawEntityIcons(ImVec2 rmin, ImVec2 sz);
	// This frame's icon hit-rects (min.xy, max.zw -> atom), ordered back-to-front; the
	// viewport click handler tests these BEFORE the scene ray-pick (topmost icon wins).
	std::vector<std::pair<ImVec4, Atom*>> iconHits;
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
	void ApplyToPrefab(Atom* a);                                 // push this instance's state into its prefab file
	void ResetToPrefab(Atom* a);                                 // revert this instance to the prefab's saved state
	void BrowserPaste();                                          // paste the clipboard into the current folder (cut=move, copy=duplicate)
	void BrowserDelete(const std::string& path);                 // delete a file/folder from disk (immediate)
	void DrawDeletePopup();                                       // browser delete-confirm modal (Enter=Yes, Esc=Cancel)
	void RequestDelete(const std::string& path);                 // compute dependents + open the confirm modal
	void PerformDelete(const std::string& path);                 // (optionally unlink) + delete
	std::vector<std::string> FindDependents(const std::string& guid);   // content files referencing a guid
	void UnlinkResource(const std::string& guid);                // reset refs to a guid -> defaults (all worlds/assets)
	void UpdateWindowTitle();                                     // "NukeEngine Editor — <project> — <world>"
	// Disk<->editor world sync.
	void SyncWorldBaseline();        // call after open/new/save: snapshot the on-disk state + clear dirty
	void TrackDirty();               // per-frame: recompute worldDirty, refresh title on change
	void TrackExternalChange();      // per-frame: detect a disk edit of the open world + act per settings
	void ReloadWorld(const std::string& diskJson);   // load disk content, reset baseline + undo
	void OverwriteWorld();           // save editor state over disk + reset baseline
	void DrawReloadPopup();          // "changed on disk (editor clean): reload?"
	void DrawConflictPopup();        // "changed on disk AND in editor: reload / overwrite / merge / ignore"
	// Merge/resolve window: hierarchical per-object/param diff of editor vs disk, pick a side per node.
	void OpenMerge(const std::string& editorJson, const std::string& diskJson);
	void DrawMergeWindow();
	void AcceptAssetDropTarget();                    // viewport/hierarchy: accept an asset drop
	Atom* DropAsset(const std::string& path);        // instantiate by extension; returns the new atom (or null)
	// Drop a material/texture asset ONTO an existing atom (viewport DnD): .numat -> the atom's material,
	// .nutex -> the atom's material base-color (diffuse). Undoable. No-op if the atom has no MeshRenderer.
	void  DropAssetOnAtom(Atom* a, const std::string& path);
	Atom* SpawnMeshAsset(const std::string& path);   // .numesh -> new Atom + MeshRenderer
	// Create new assets in a content folder (from the browser's "New" menu).
	void CreateFolderAsset(const std::string& folder);
	void CreateWorldAsset(const std::string& folder);    // empty .nuworld
	void CreateMaterialAsset(const std::string& folder); // default .numat (registered in ResDB)
	void CreateBoneMapAsset(const std::string& folder);  // .nubonemap retarget map (JSON, 3.1)
	void CreateShaderAsset(const std::string& folder);   // .vs/.ps.hlsl pair (registered + pipeline built)
	void CreateRenderTextureAsset(const std::string& folder);   // .nutex RenderTexture (camera target)
	// plugins
	void PluginMGRWindow();
	// status bar (2.3): frame stats + scene counters + plugin fields (nuke::StatusBar)
	void StatusBarPanel();
	// toolbar
	bool ToolBtn(const char* icon, const char* tip, bool active, float w);
	void SpawnEmpty();
	void SpawnPrimitive(const char* atomName, const char* guid);
	void SpawnCube();
	void SpawnCamera();
	void SpawnLight(int type, const char* atomName);   // type 0=dir 1=point 2=spot
	void SpawnEnvironment();                           // atom + Environment (sky/ambient)
	void SpawnReflectionProbe();                       // atom + ReflectionProbe (scene-captured reflections)
	void Toolbar();
	void Draw();
	// undo/redo (generic command stack)
	void Undo();
	void Redo();
	// worldEdit=false for commands that do NOT change the open world (file moves, project
	// settings, asset tweaks) — they stay undoable but must not flip the world's dirty "*".
	void PushUndo(const std::string& label, std::function<void()> undoFn, std::function<void()> redoFn,
	              bool worldEdit = true);
	void TrackUndo();                  // detect a settled selected-atom edit (inspector/gizmo); not in PIE
	void ResetUndo();                  // clear history (on world load / new)
	void ApplyAtomState(long id, long parentId, int index, const std::string& json);   // undo primitive
	void RecordAdd(Atom* a);                                       // an atom was created
	void RecordReparent(Atom* a, long oldParent, int oldIndex);   // an atom moved in the hierarchy
	void RecordDelete(Atom* a);                                   // an atom was deleted
	void DeleteSelectedAtom();                                    // hierarchy: delete the selected atom (undoable)
	void RemoveComponent(Atom* a, Component* c);                  // inspector: remove a component (undoable)
	void RecordFileMove(const std::string& from, const std::string& to);   // a file/folder was renamed or moved
	// Generic value edit (settings, paths, flags…): records before/after of any comparable value.
	template<class T> void RecordChange(const std::string& label, T* slot, const T& before, const T& after)
	{ if (before == after) return; PushUndo(label, [slot, before]{ *slot = before; }, [slot, after]{ *slot = after; }); }
};

inline void editorinit() { EditorUI::getSingleton()->SetUp(); }
inline void editorDraw() { EditorUI::getSingleton()->Draw(); }

#endif // EDITORUI_H
