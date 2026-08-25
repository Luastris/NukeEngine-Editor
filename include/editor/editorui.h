#ifndef EDITORUI_H
#define EDITORUI_H
// Editor UI panels: menu, hierarchy, inspector, browser, viewport, settings.

#include "imgui.h"
#include "imgui_internal.h"   // BeginViewportSideBar
#include "nukeui.h"
#include "IconsLucide.h"
#include <interface/IconsFileTypes.h>  // ICON_FT_*: the glyphs a file type can claim
#include "ImGuizmo.h"         // gizmo lives in NukeImGui, shares the context
#include "config.h"
#include "interface/AppInstance.h"
#include "interface/Modular.h"
#include "API/Model/MeshRenderer.h"
#include "API/Model/PostProcess.h"
#include "API/Model/UnknownComponent.h"
#include "API/Model/resdb.h"
#include "API/Model/SequencePlayer.h"
#include "import/assimporter.h"
#include "API/Model/Prefab.h"
#include "reflect/Reflect.h"
#include "API/Model/Time.h"
#include "API/Model/Log.h"
#include "editor/exteditor.h"
#include "input/Hotkeys.h"
#include "input/InputTypes.h"
#include <boost/container/list.hpp>
#include <boost/bind/bind.hpp>
#include <cstring>
#include <nlohmann/json.hpp>
#include <boost/filesystem/fstream.hpp>
#include <map>
#include <functional>
#include <set>
#include <string>
#include <vector>
#include <cctype>
#include <cstdio>
#include <algorithm>
#include <boost/filesystem.hpp>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/ext.hpp>   // lookAtLH / perspectiveLH_ZO (renderer is LH, z 0..1)

using namespace nuke;
using namespace std;

// Native OS file dialogs (defined in main.cpp, which isolates <windows.h>).
// Each returns the picked path, or "" if cancelled.
std::string EditorPickModelFile();
std::string EditorPickIconFile();   // .ico picker (game icon)
std::string EditorPickFolder();     // folder picker (build path)
std::string EditorPickProjectFile();// .nuproj / .nupak / .numod picker
std::string EditorPickExeFile();    // .exe picker (custom external editor)
bool        EditorRelaunch(const std::string& projectPath);   // spawn a new editor on that project

class TextEditor;   // vendored ImGuiColorTextEdit (src/textedit)

// Register the .nuproj extension under HKEY_CURRENT_USER so double-clicking a project
// opens this editor. Returns success. (macOS: LaunchServices registration of the .app.)
bool RegisterProjectFileAssociation();

// macOS: LaunchServices hands a double-clicked document over as an Apple Event, not argv.
// Install the handler before the first event pump; the toolbar polls the queued path and
// routes it through RequestProjectSwitch. Both are no-ops on Windows/Linux (argv covers it).
void        EditorInstallOpenDocHandler();
std::string EditorTakeOpenDocRequest();   // queued document path, "" when none (one-shot)

// "Box (2)" -> "Box". Returns `s` unchanged when there is no trailing " (N)" counter.
static inline std::string StripNameCounter(const std::string& s)
{
	if (s.size() < 4 || s.back() != ')') return s;
	size_t open = s.rfind(" (");
	if (open == std::string::npos || open + 2 >= s.size() - 1) return s;
	for (size_t i = open + 2; i + 1 < s.size(); ++i)
		if (!isdigit((unsigned char)s[i])) return s;
	return s.substr(0, open);
}

// Row-major (renderer) -> column-major (ImGuizmo) 4x4 matrix layout.
static inline void Transpose4(const float* s, float* d)
{
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			d[c * 4 + r] = s[r * 4 + c];
}

// Shared chrome for the settings-style windows (Preferences / Project Settings / World
// Settings): a category sidebar on the left, a text filter on top, sections on the right.
// Usage per frame inside the window:
//   shell.Begin("id", cats, N);
//   if (shell.Section("Category", "Section label", "space-separated setting keywords")) { ... }
//   shell.End();
// With an empty filter the active category alone shows; a live filter searches EVERY section
// by label + keywords + category and shows the hits regardless of the selected category.
struct SettingsShell
{
	char filter[96] = "";
	int  active     = 0;
	bool Begin(const char* id, const char* const* cats, int catCount);
	bool Section(const char* cat, const char* label, const char* keywords = "");
	void End();
private:
	const char* const* cats_ = nullptr;
	int  catCount_ = 0;
	bool open_     = false;
};

// Inspector-style property row for the settings windows: the LABEL comes FIRST, in a fixed
// column, the widget after it. Returns the hidden "##label" id for the widget call:
//   if (ImGui::Combo(LProp("Anti-aliasing").c_str(), ...))
// `col` = label column width in font-size units — override for the odd long label.
inline std::string LProp(const char* label, float col = 13.0f)
{
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(label);
	ImGui::SameLine(ImGui::GetFontSize() * col);
	// Clamp to the panel: a fixed 15em used to run past the edge of narrow docks.
	ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 4.0f,
		std::min(ImGui::GetFontSize() * 15.0f, ImGui::GetContentRegionAvail().x)));
	return std::string("##") + label;
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
	long previewCamAtomId = 0;          // ...validated by STABLE id: stream parking can delete
	                                    // the atom between frames (dangling Camera* = crash)
	bool pieUseEditorCam = false;   // PIE possess: false = game main camera, true = editor camera
	std::map<std::string, bool> uiOpen; // persisted CollapsingHeader states (Components + per atom/component)
	long pendingSelectId = 0;           // atom id to reselect after load (from editor_state.json; recursive)
	int  browserView = 0;               // asset browser: 0 Tiles, 1 List, 2 Tree, 3 By Type
	int  browserRoot = 0;               // browse root: 0 = content, 1 = <project>/source
	char browserSearch[128] = "";
	bool fMesh = true, fMat = true, fTex = true, fPrefab = true;   // browser type filters
	std::string contentDir = "project/content";   // project content root (imported assets live here)
	std::string browserCwd;                        // current folder shown in the browser
	std::string browserSel;                        // primary selected entry (full path; "" = none)
	std::string browserLocate;                     // go-to-file: path to resolve+scroll to on the next browser draw
	std::set<std::string> browserMSel;             // multi-selection (ctrl/shift); superset incl. browserSel
	std::string browserSelAnchor;                  // shift-range anchor (path of the last plain click)
	std::vector<std::string> browserBack, browserFwd;   // folder navigation history (M4=back, M5=forward)
	std::vector<std::string> clipboard;            // browser cut/copy buffer (full paths)
	bool        clipboardCut = false;              // true: paste MOVES (cut); false: paste COPIES
	std::vector<std::string> pendingDeletes;       // browser: paths awaiting delete-confirm (empty = none)
	bool        openDeletePopup = false;           // request to open the delete-confirm modal next frame
	std::vector<std::string> deleteDeps;           // resources depending on pendingDeletes (shown in the modal)
	bool        unlinkOnDelete = false;            // project setting: break refs to a deleted resource
	// Disk<->editor sync: canonical JSON of the last loaded/saved world state.
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
	int   foliageBrush = 0;             // foliage paint: 0 = off, 1 = paint, 2 = erase
	float foliageBrushRadius = 2.0f;
	float foliageBrushDensity = 1.0f;   // density multiplier per stroke step
	int   maskBrush = 0;                // SurfaceMask paint: 0 = off, 1 = paint, 2 = erase
	int   maskBrushChannel = 0;         // grid channel 0..3 (state slot)
	float maskBrushRadius = 1.5f;
	float maskBrushStrength = 2.0f;     // condition value added per second of stroke
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
	// Deferred component move ("NUKE_COMPONENT" payload: inspector header -> hierarchy row).
	struct CompDragPayload { long atomId = 0; long compId = 0; };
	long dndCompAtomId = 0; long dndCompId = 0; Atom* dndCompDst = nullptr;
	// Deletion requested while the hierarchy tree is being walked: destroying an atom mid-walk
	// invalidates the list node the loop stands on, so the request is applied after the walk.
	long pendingDeleteId = 0;
	// Hierarchy: reveal (open the branch + scroll to) a selection made outside the panel.
	Atom* hierLastSel = nullptr;
	bool  hierRevealPending = false;
	// multi-select: the engine holds primary (`selectedInHieararchy`) + extras
	// (`selectedExtra`); the panel keeps the shift anchor and the visible-row order of the
	// LAST drawn frame (shift ranges resolve against it, browser-style).
	long hierAnchorId = 0;
	std::vector<Atom*> hierRows, hierRowsPrev;
	std::vector<long>  pendingDeleteIds;   // deferred multi-delete (applied after the walk)
	// Structural ops (group/ungroup/duplicate/paste/new-folder) requested from the context
	// menu run AFTER the tree walk — reparenting removes list nodes the walk stands on.
	std::vector<std::function<void()>> hierPendingOps;
	// Profiler window (Window menu / clicking the status-bar timings).
	bool profilerOpen = false, profilerFocus = false, profilerFrozen = false;
	bool meshCostView = false;   // profiler's mesh-cost overlay (iRender::setDebugView)
	char profilerFilter[64] = "";
	void DrawModuleOverlays(ImVec2 rmin, ImVec2 sz);   // module-registered viewport overlays (EditorHooks)
	// Edit-history window (over the same undo/redo stacks).
	bool historyOpen = false, historyFocus = false;
	int  undoTrimmed = 0;        // commands dropped by the 200 cap since the last reset
	// Generic undo/redo stack: each action pushes its own inverse closures (atom edits are
	// captured as a subtree delta, never the whole world). Push via PushUndo / RecordChange<T>.
	struct UndoCmd { std::function<void()> undo, redo; std::string label; long serial = 0; };
	std::vector<UndoCmd> undoStack, redoStack;
	// Monotonic edit id; the world is dirty when the top-of-stack id differs from the
	// one recorded at the last save/load. This is the whole dirty check.
	long editSerial = 0;
	long savedWorldSerial = 0;
	long WorldEditSerial() const { return undoStack.empty() ? 0 : undoStack.back().serial; }
	std::string editBefore; long editAtomId = 0; bool editing = false;   // selected-atom edit detector
	unsigned int editActiveId = 0;   // ImGui active-widget id of the in-progress edit (flush when it changes)
	std::string  idleSnap; long idleAtomId = 0;   // snapshot taken while nothing was being edited = pre-edit "before"
	bool idleSnapValid = false;   // refresh on demand (selection change / after an edit), never per frame
	Atom* pendingCompAtom = nullptr; Component* pendingCompDel = nullptr;   // deferred component removal
	std::string rebindId;                          // hotkey id currently being rebound ("" = none)
	bool        settingsOpen = false;              // Project Settings window open?
	bool        openSaveAsPopup = false;           // request to open the "Save World As" modal
	char        saveAsBuf[256] = "";               // edited world FILE name
	std::string saveAsDir;                         // chosen target folder (full path) in the save dialog
	bool projectHealed = false;                    // load dropped corrupted entries: persist the clean lists once
	std::map<std::string, int> pendingHotkeyBinds; // LEGACY hotkey bindings from an old .nuproj (prefs override them)
	std::map<std::string, int> prefsHotkeyBinds;   // hotkey bindings from Preferences, applied after plugins load
	std::string projectDir  = "project";           // project root
	std::string projectFile = "project/game.nuproj";
	std::string projectName = "NukeGame";          // .nuproj "name" (dist/pak naming)
	// Pak compression for the project (release) and for mods, persisted in the .nuproj.
	int pakMethod = 3, pakLevel = 9;               // 0 store / 1 zlib / 2 zstd / 3 gdeflate (DirectStorage)
	int modMethod = 0, modLevel = 0;
	int pakBlockMB = 8;                            // pak block size: one DirectStorage request (texture mips split to fit)
	nlohmann::json modSettings;                    // module-declared project settings (.nuproj "moduleSettings" object)
	std::string gameIcon;                          // .ico (content-relative) stamped onto the shipped exe
	std::string distPath;                          // build output ("" = default: <project>/dist; abs or project-relative)
	uint64_t    iconPrevTex = 0;                   // live preview texture of gameIcon (settings window)
	std::string iconPrevPath;                      // which path iconPrevTex was decoded from
	// Decode the best image of an .ico into RGBA8 (PNG-compressed and 32-bpp DIB entries).
	// Public: the Linux AppDir icon stamp (a free function in packaging.cpp) reuses it.
public:
	static bool DecodeIcoRGBA(const std::string& path, std::vector<unsigned char>& rgba, int& w, int& h);
private:
	// New Project modal state; on OK the project is scaffolded and the editor relaunches on it.
	bool openNewProjectPopup = false;
	char newProjName[128] = "MyGame";
	std::string newProjDir;
	std::map<std::string, bool> newProjMods;       // moduleFile -> load in this project?
	bool        newProjModsInit = false;           // lazily filled from the discovered pool
	std::string newProjRender;                     // chosen render provider moduleFile
	void DrawNewProjectPopup();
	// Project hub: the editor booted with no project, so only the hub window runs
	// (recent / open / create); picking a project relaunches the editor on it.
public:
	bool projectHubMode = false;   // set by main.cpp before any UI exists
private:
	int  startupProjectMode = 0;                   // pref: 0 = open last project, 1 = always show the hub
	std::vector<std::string> recentProjects;       // pref: most-recent-first absolute .nuproj paths
	void PushRecentProject(const std::string& path);   // record an opened project (dedup, cap, persist)
	void DrawProjectHub();
	void OpenProjectCmd();                         // File -> Open Project... (raw or packed)
	void RequestProjectSwitch(const std::string& path);   // confirm unsaved world, then switch
	void DrawSwitchConfirmPopup();
	std::string pendingSwitchPath;                 // project awaiting the unsaved-world decision
	bool openSwitchConfirm = false;
	void SwitchToProject(const std::string& path); // relaunch this editor on `path` + close
	// Source pak when this project was opened from an archive (.nupak/.numod); drives Package Mod.
	std::string basePakPath;
	// Package Mod modal state. The name persists in the manifest ("modName"), so repacking
	// updates the SAME mod and a new name creates a new file.
	bool openPackageModPopup = false;
	char packModName[128] = "";
	std::string modName;                           // last packaged mod name (from the .nuproj)
	// Package Mod split: 0 = one .numod, 1 = parts by content type, 2 = parts under a size cap.
	int modSplitMode = 0;
	int modSplitCapMB = 512;
	// Package DLC: name + the shipped base game.nupak to diff against.
	bool openPackageDlcPopup = false;
	char packDlcName[128] = "";
	char packDlcBase[512] = "";
	// Game Build modal: boot settings tweaked before packaging. The dist config/main.json is
	// FORMED at packaging (editor config + these overrides) — nothing is stored elsewhere.
	bool openPackageProjectPopup = false;
	bool gbWinSet = false;                         // dialog confirmed: worker overrides the window block
	nuke::NukeWindow gbWin{};                      // dialog model (game window settings for the dist)
	bool gbLog   = false;                          // dist logToConsole
	bool gbDebug = false;                          // dist gpuValidation (debug layer)
	bool gbConsole = false;                        // dist devConsole (the in-game ~ console; ships the GUI backend)
	int  gbBuildCfg = 0;                           // dist binaries: 0 = Release (ship), 1 = Debug (dev)
	bool pkgAnalyzed = false;                      // PackageProject's pre-pass already logged the module decisions

public:
	// Packaging: one discovered module, snapshotted on the game thread for the pack workers.
	// Public: the selection logic (ComputeShipModules) lives in file-static packaging helpers.
	struct PkgMod
	{
		std::string name;         // canonical platform-neutral module name
		std::string file;         // absolute binary path (the editor's discovered copy)
		std::string service;      // service key when CHOSEN as the provider ("render"/"gui"/...), else ""
		std::string provides;     // the module's own provides() ("" for plain plugins)
		bool shared     = false;  // sharedService(): several providers live at once (scripting)
		bool enabled    = false;  // on the project's plugin list (no list = all; PHASE_BOOT = yes)
		bool editorTool = false;  // editor tooling: never ships
		bool project    = false;  // project-local game module: the game's own code, always ships
		std::vector<std::string> extraPak;                            // its shipExtras (pak files)
		std::vector<std::pair<std::string, std::string>> extraDist;   // its shipExtras (dist copies)
		void* script = nullptr;   // its iScript instance when it is a scripting backend, else null
	};
	std::map<std::string, PkgMod> SnapshotPkgMods();   // keyed by lowercase name (game thread only)
	// Package Project's build half: the engine targets the dist needs, then the project's game
	// modules, then PackageProjectNow.
	void PackageProjectBuild(const std::string& cfg, const std::vector<std::string>& targets);
private:
	void PackageProjectCmd();                      // pre-fill (editor config + prev dist) -> open the modal
	void DrawPackageProjectPopup();
	// One row of the Mods panel; enable/disable + order writes config/mods.json.
	struct ModRow
	{
		std::string file, path, name, req;         // req = display string of `reqs`
		std::vector<std::string> reqs;             // dependency names from mod.json
		// Engine plugins the mod declares ("modules"): missing = it cannot load at all,
		// installed-but-off = it loads with dead components.
		std::vector<std::string> mods;
		std::string modReq;                        // display string of `mods`
		bool modsInstalled = true, modsEnabled = true;
		bool enabled = false, mounted = false, found = true;
		bool reqOk = true;                         // all requirements enabled+loadable (per frame)
		bool edMounted = false;                    // mounted in THIS editor session (separate list)
	};
	std::vector<ModRow> modsUi;
	int  modsUiTick = -1;                          // frame-count throttle for rescans
	void ScanModsUi();                             // rebuild modsUi (config + mods/ dir + manifests)
	void SaveModsUi();                             // write enabled rows (in order) to config/mods.json
	// Persist the EDITOR's own mod selection (config/mods.json is the PLAYER's list) and
	// remount the stack live; the world must be reopened to see the merge.
	void SaveEditorMods();
	std::string GameRootFromBase() const;          // the game dir the session's pak belongs to
	// Console: viewer over the engine's Log ring (cout/cerr are captured into it).
	uint64_t conVersion = ~0ull;                   // last seen Log::Version (cheap change check)
	std::vector<nuke::LogEntry> conCache;          // snapshot, refreshed when the version moves
	bool conShow[3] = { true, true, true };        // info / warn / error visibility
	char conFilter[128] = "";                      // substring filter (tag + text)
	bool conAutoScroll = true;
	void OpenLogSource(const nuke::LogEntry& e);   // double-click: resolve + jump to file:line
	// Preferences: machine-wide (%APPDATA%/NukeEngine/preferences.json), not per-project.
	bool prefsOpen = false, prefsFocus = false;
	std::vector<ExtEditor> extEditors;             // detected external editors + "Custom"
	std::string extEditorName;                     // the chosen one (persisted by name; "" = built-in)
	std::string extCustomExe, extCustomArgs;       // the "Custom" entry ({file}/{line} template)
	bool detachAssetEditors = false;   // asset editors as separate OS windows (else docked)
	int  uiScalePct = 100;             // UI scale slider (25..300%); × the OS content scale
	std::string displayBackend = "auto";   // Linux: auto (X11 for the editor) / x11 / wayland
	SettingsShell shellPrefs, shellProj;   // settings-window chrome (World Settings stays a flat inspector)
	// Editor render backend (machine preference): 0=D3D11, 1=D3D12, 2=Vulkan.
	// The RUNTIME backend is a project setting (config/main.json window.backend).
	int editorBackend = 2;
	bool editorRayTracing = true;   // false = raster path (shadow maps/SSR) in the editor; restart to apply
	void DrawAssetEditorBody(int i);   // one editor's content (docked window OR host window)
	void LoadPreferences();
	void SavePreferences();
	void winPreferences();
	void winProfiler();                                           // live phase breakdown (CPU + GPU)
	void winHistory();                                            // edit-history timeline (click = jump)
	// Open file:line in the user's chosen editor (falls back to the built-in text editor).
	void OpenExternal(const std::string& file, int line);
	std::string startupWorld = "scene.nuworld";    // from the .nuproj
	std::string lastWorld;                         // from editor_state.json: world open when the editor last exited
	std::vector<std::string> enabledPlugins;       // per-project plugin load list (dll names)
	// Every plugin this project has ever seen. Without it, "absent from enabledPlugins" cannot
	// tell a module the user turned OFF from one that did not exist when the list was written.
	std::set<std::string> knownPlugins;
	bool pluginListDirty = false;
	bool pluginListLoaded = false;                 // did the .nuproj specify a plugin list?
	// Per-project service provider choice: service -> dll name. Boot services apply on next start.
	std::map<std::string, std::string> serviceChoices;
	std::vector<std::pair<nuke::NUKEModule*, bool>> pendingPluginToggle;   // applied after the window loop
	char        pluginFilter[128] = "";            // plugin window: text search over name + tags
	int         pluginServiceFilter = 0;           // plugin window: 0=All, 1=Utility, 2+=service index
	float camYaw = 0.0f, camPitch = 0.0f;   // editor camera look angles (radians)
	float gizmoMatrix[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };   // persistent during a gizmo drag
	// grid snap: toolbar toggle + increments, persisted in the .nuproj. Ctrl INVERTS the
	// toggle while held (temporary snap / temporary free); V holds surface snap while moving.
	bool  snapEnabled = false;
	float snapMove = 0.5f, snapRot = 15.0f, snapScale = 0.1f;
	bool  gridVisible = true;   // the world grid (Y=0) drawn in the viewport; spacing = snapMove
	// input maps: auto (every content .nuinput loads, historical) or an explicit list.
	bool inputMapsAuto = true;
	std::vector<std::string> inputMapsList;   // content-relative paths, '/' separators
	void ApplyInputMaps();                    // push the list into the engine + reload the live map
	std::string pieSnapshot;   // world serialized on Play, restored on Stop (PIE)
	// Edit target captured on Play, restored on Stop: Game.LoadWorld during PIE retargets
	// AppInstance::currentWorldPath, and the restored world must not inherit that path.
	std::string pieWorldPath;

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
	// Read one "services.<service>" choice straight from the .nuproj during phase-1 boot,
	// BEFORE LoadProject(). Returns "" when the project/key doesn't exist.
	std::string EarlyProjectService(const std::string& service);
	void SaveProject();
	void LoadProject();
	// Build the full release dist/ (player + deps + used modules + config + the project
	// packed as content/game.nupak).
	void PackageProject();           // Release build first, then dist
	void PackageProjectNow();        // the packaging body itself (no build step)
	// Run `cmake --build <repo>/build --config <config>` on a Jobs worker, streaming output
	// into the Console. Skipped when `config` is the one this editor is running (locked
	// binaries). onDone fires on the game thread.
	void RunEngineBuild(const std::string& config, std::function<void(bool)> onDone);
	// Same, restricted to the named superbuild targets (packaging: the player + the modules the
	// dist ships — the editor and unused modules stay untouched). Empty list = everything.
	void RunEngineBuild(const std::string& config, const std::vector<std::string>& targets,
	                    std::function<void(bool)> onDone);

	// C++ game modules: native NUKEModule DLLs in <project>/source, built to <project>/modules.
	// Scaffold generates one; Discover pulls DLLs into the plugin pool (auto-enabled on first
	// sight); Build runs rebuild + DLL hot-swap (components survive as placeholders).
	void CreateGameModuleScaffold(const std::string& name);
	void DiscoverProjectModules();
	void BuildGameModules();
	// Explicit config (nullptr = the running editor's) + completion on the game thread.
	void BuildGameModules(const char* config, std::function<void(bool)> onDone);
	bool gmNamePopup = false;            // "New C++ Game Module" modal trigger (main menu)
	char gmNameBuf[64] = "";
	void PackageMod(const std::string& name = "");   // "" -> last used / project name (dev hook)
	void PackageModCmd();            // open the "Package Mod" modal (prefills the name)
	void DrawPackageModPopup();      // the modal itself (drawn each frame)
	void PackageDlc(const std::string& name, const std::string& basePak);   // crc-diff of the cooked project vs basePak
	void PackageDlcCmd();            // open the "Package DLC" modal
	void DrawPackageDlcPopup();      // the modal itself (drawn each frame)
	// Open-with support. A .nupak never extracts: it is MOUNTED and edits land in a
	// "<stem>_mod" overlay dir; a .numod extracts into "<stem>_project" for full editing.
	// Both return the work project's .nuproj ("" on failure) and record the base pak.
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
	void NewWorldCmd();              // New World (dirty world -> Save/Discard/Cancel modal)
	void DoNewWorld();               // the actual clear, pushed as ONE undoable command
	bool openNewWorldConfirm = false;   // request to open the "New World?" modal next frame
	void SaveWorldCmd();             // save the current world (to its path, or the project default)
	void SaveWorldAsCmd();           // open the "Save World As" modal (pick name/location)
	void DrawSaveAsPopup();          // the modal itself (drawn each frame)
	void SaveAsFolderTree(const std::string& dir);   // recursive folder tree (pick the save folder)
	void OpenWorldCmd(const std::string& relPath);            // open a world from project content
	void OpenWorldFromBrowser(const std::string& fullPath);   // open a .nuworld picked in the browser
	// World switches requested mid-frame are QUEUED and applied first thing in the render
	// callback: tearing a world down mid-command-list makes D3D12 fail to close the list.
	std::string pendingWorldOpen;
	void ApplyPendingWorldOpen();
	void winSettings();              // Project Settings window (default world + hotkeys)
	// hierarchy
	void winHierarchy();
	void DrawAtomNode(Atom* atom);                 // one tree row + DnD (recurses children)
	void HierGap(Atom* before);  // thin insertion zone overlaid on a row's top edge (only while dragging an atom)
	bool HierMatch(Atom* atom);                    // search: atom name OR a component type matches
	bool HierMatchDeep(Atom* atom);                // this atom or any descendant matches
	const char* AtomIcon(Atom* atom);              // icon by the atom's components
	void FocusSelected();                        // frame the selected atom with the editor camera
	// inspector
	void CamComponent(Camera* cam);
	// Type-locked asset-reference picker (mesh/material/shader/texture) with a browser DnD
	// target, locate/reset buttons and a filterable list. Returns true when the value changed.
	bool AssetPicker(const char* label, std::string& guid, const std::string& kind, const std::string& defGuid = "");
	// THE curve widget: interactive cubic-Hermite editor over flat (t, value, inTan, outTan)
	// keys — inspector [[prop(widget="curve")]] lists and the tween easing draw this. Legacy
	// (t, v) pairs upgrade in place. Callers scope it with a unique PushID. True on change.
	// trig*: OPTIONAL keyframe triggers — times bound to key times (drag follows, delete
	// kills), assigned on the selected key from `trigOptions` (material event names).
	static bool CurveKeysEditor(std::vector<float>& keys, float vLo, float vHi, const char* emptyText,
	                            std::vector<float>* trigT = nullptr,
	                            std::vector<std::string>* trigEv = nullptr,
	                            const std::vector<std::string>* trigOptions = nullptr);
	// THE gradient widget: (t, r, g, b[, a]) stops drawn as a bar with draggable markers —
	// inspector [[prop(widget="gradient")]] lists (rgb) and color tweens (rgba). True on change.
	static bool GradientStopsEditor(std::vector<float>& stops, bool alpha,
	                                std::vector<float>* trigT = nullptr,
	                                std::vector<std::string>* trigEv = nullptr,
	                                const std::vector<std::string>* trigOptions = nullptr);
	bool DrawLiveMaterialSections(nuke::Material* m);   // .numat editor: LiveMaterial sections (states/hits/sound/surface)
	void RegisterInspectorOverrides();
	void DrawMeshRendererInspector(nuke::MeshRenderer* mr);
	void DrawPostProcessInspector(nuke::PostProcess* pp);
	void DrawAnimatorInspector(nuke::Animator* an);   // serialized state machine
	void winWorldSettings();   // World Settings window (global shadow settings, saved in the .nuworld)
	bool worldSettingsOpen = false;
	bool worldSettingsFocus = false;   // focus the window only when opened via menu, not when restored on load

	World::Settings wsBefore;  // pre-edit snapshot of world settings (idle baseline for undo)
	bool wsEditing = false;

	// Snapshot of the value-based project settings for undo (rendering + RTX + disk sync).
	// Backend and Default World are deliberately excluded.
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
	// Inspector body for a browser-selected asset (texture usage/info, material fields,
	// or read-only info + Open), shown when the Hierarchy selection is empty.
	void DrawAssetInspector(const std::string& path);
	std::string     inspAssetPath;                 // cache key: asset currently loaded into the inspector
	long long       inspAssetMtime = 0;            // + its last-write time, so a reimport (same path) reloads
	nuke::Texture*  inspTex = nullptr;             // cached loaded .nutex (usage editing)
	float inspChroma[3] = { 1.0f, 0.0f, 1.0f };    // chroma-key colour (magenta default)
	int   inspChromaTol = 24;                       // per-channel tolerance
	bool  inspChromaPick = false;                   // eyedropper armed (click the preview to sample)
	bool  inspChromaOutside = true;                 // key only the border-connected background (keep enclosed areas)
	nuke::Material* inspMat = nullptr;             // cached loaded .numat (field editing)
	// One tab of the text editor; syntax comes from the file-type descriptor.
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

	// Shared 3D-preview mini-world (sky + sun + one mesh atom + camera into an own RT).
	// Pooled: the render seam has no destroyRenderTarget, so closed editors return theirs.
	struct PreviewWorld
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
		// Overlay gizmo hovered/grabbed; must be set by the caller INSIDE its ImGuizmo ID
		// scope (IsUsing()/IsOver() lie outside it). Suppresses orbit while true.
		bool    gizmoBusy = false;
		float   flyMul = 1.0f;              // camera speed multiplier (wheel while RMB-flying)
		float   flyMulHud = 0.0f;           // seconds left to show the speed HUD after a change
		// Material editor: RMB ORBITS the framed subject (wheel = dolly, MMB = shift the
		// pivot) instead of the free-fly camera the mesh/prefab windows keep.
		bool    orbit = false;
		// Static shot: no camera input at all (inspector material preview — just a look).
		bool    locked = false;
	};
	std::vector<PreviewWorld*> pvPool;      // every created scene (in use or free)
	PreviewWorld* AcquirePreview();
	void ReleasePreview(PreviewWorld* s);
	// Interactive 3D view filling exactly `size`: LMB drag orbits, wheel dollies.
	void DrawPreviewImage(PreviewWorld& s, ImVec2 size);
	void FramePreview(PreviewWorld& s, Atom* subtree);       // bounds of a subtree (or the mesh atom) -> center/radius

	// Inspector's asset preview (one pooled scene, staged by browser selection).
	uint64_t inspTexPreviewId = 0;          // GPU texture of the decoded .nutex (destroyTexture2D on change)
	PreviewWorld* inspPv = nullptr;
	std::string   pvStaged;                 // asset path currently staged ("" = none)
	void StageAssetPreview(const std::string& path, const std::string& ext);
	void DrawAssetPreview3D(const std::string& path, const std::string& ext);   // inspector widget
	void RenderAssetPreview(iRender* r);    // render hook: draws every visible preview scene BEFORE the live scene

	// --- ASSET EDITORS: each .numat / .numesh / .nuprefab opens its own window.
	// Sprite-slice metadata snapshot: margins ml/mr/mt/mb, spacing sx/sy, 9-slice sl/sr/st/sb.
	struct SpriteMeta { int cols=1,rows=1, ml=0,mr=0,mt=0,mb=0, sx=0,sy=0, sl=0,sr=0,st=0,sb=0; bool nine=false; };

	struct AssetEditorWin
	{
		// Trigger tool (material editor): pick an event, click the sample -> raycast fire.
		bool evtTool = false;
		int  evtSel  = 0;

		std::string path, ext;              // full path + lowercase extension
		void* host = nullptr;               // detached mode: the editor-owned OS window (NukeUI host)
		bool detached = false;              // PER-WINDOW mode (drag-driven); pref = default for new editors
		bool wantDock = false;              // host dropped onto the main window — processed by winAssetEditors next frame
		bool dragOut = false;               // torn off mid-drag: the new host must pick the drag up (HostBeginDrag)
		bool hasDrop = false;               // re-dock placement pending: float the window at dropX/dropY
		float dropX = 0, dropY = 0;         // drop point in main-window client coords
		PreviewWorld* pv = nullptr;
		Material* mat = nullptr;            // .numat: owned editing copy (saved to the file)
		// .nutex Sprite Slicer: owned editing copy + downsampled GPU preview + 2D view state.
		nuke::Texture* tex = nullptr;
		uint64_t  texPreview = 0;
		int       texPrevW = 0, texPrevH = 0;
		int       slMode = 0;               // left dropdown: 0 = Sprite Slicer (room for more modes)
		float     slZoomMul = 1.0f, slPanX = 0, slPanY = 0;   // view: zoom = fitScale*slZoomMul; pan in screen px
		bool      slUserView = false;       // false = auto-fit+center each frame (adapts to resize); true once panned/zoomed
		int       slDrag = 0;               // active ruler/grid-line drag handle (see DrawSpriteSlicer)
		int       slFirst = 0, slCount = 0; float slFps = 12.0f;   // anim preview range
		bool      slPlay = false; float slAcc = 0; int slCur = 0;
		bool      slShowMirror = false; int slPadL = 0, slPadR = 0, slPadT = 0, slPadB = 0;   // cell-0 markup mirrored dimly
		std::vector<SpriteMeta> undoS, redoS; SpriteMeta idleS; bool haveIdleS = false;
		// .nuinput: the parsed input map, edited in place and saved back to the file.
		std::vector<nuke::InputAction>  inActions;
		std::vector<nuke::InputContext> inContexts;
		std::vector<std::string> undoI, redoI;   // .nuinput history (serialized map JSON)
		std::string idleI;                        // pre-edit baseline (map JSON)
		// .nucursor: parsed frames, animated preview state.
		struct CurFrame { std::string image; int hotX = 0, hotY = 0; uint64_t tex = 0; int tw = 0, th = 0; };
		std::vector<CurFrame> curFrames;
		float  curFps = 12.0f;
		bool   curLoop = true;
		int    curSel = 0;
		double curAnim = 0.0;
		bool   curPlay = true;
		Atom*     prefabRoot = nullptr;     // .nuprefab: loaded subtree (lives in pv->world)
		long      prefabSelId = 0;          // selected atom in the prefab tree (stable id)
		// .nuseq sequencer: owned editing copy + a detached preview player (atom-less; "/"
		// paths resolve from the live world's roots) + timeline/record state.
		nuke::Sequence*       seq = nullptr;
		nuke::SequencePlayer* seqPv = nullptr;
		double seqTime = 0; bool seqPlaying = false, seqRecord = false;
		int    seqSelKind = -1, seqSelTrack = -1, seqSelKey = -1;   // curve-editor target
		float  seqZoom = 80.0f;             // pixels per second
		float  seqSnap[10]; bool seqSnapValid = false;   // RECORD: last-frame TRS of the selection
		std::vector<std::string> undoQ, redoQ;   // .nuseq history (JSON)
		std::string idleQ;                        // pre-edit baseline
		// .nuanim animation window: owned editing copy (previewClip-served to the rig) +
		// timeline/selection state + optional retarget side-by-side (second preview world).
		nuke::AnimClip* anim = nullptr;
		double anTime = 0; bool anPlaying = false; float anSpeed = 1.0f;
		float  anZoom = 80.0f;              // pixels per second
		int    anSelNotify = -1, anSelEvent = -1, anSelCurve = -1, anSelProp = -1;
		std::string anRigSkel;             // preview mesh (auto-resolved by skelGuid, overridable)
		long   anAtomId = 0;                // preview rig atom (lives in pv->world)
		std::string anRetargetSkel;         // retarget preview: target skeleton guid ("" = off)
		std::string anRetargetSkel2;         //   target rig mesh (auto by skelGuid, overridable)
		PreviewWorld* anPv2 = nullptr;      //   right-hand preview world
		long   anAtomId2 = 0;               //   target rig atom (lives in anPv2->world)
		std::vector<std::string> undoAn, redoAn;   // clip metadata history (JSON)
		std::string idleAn;
		// .nusm node graph: owned editing copy + canvas state + a live preview rig driven by
		// the controller (SetPreviewController keeps the graph on the EDITING copy).
		nuke::AnimSM* sm = nullptr;
		int    smLayer = 0;
		std::vector<std::string> smPath;    // sub-machine navigation (state names from the layer)
		std::string smSelState;             // selected state name in the CURRENT scope
		int    smSelTrans = -1;             // selected transition index in the CURRENT scope
		float  smPanX = 0, smPanY = 0, smZoom = 1.0f;
		bool   smLink = false;              // add-transition mode (drag a link to the target)
		std::string smLinkFrom;             // link source state name ("*" = Any State)
		std::string smRigSkel;             // preview rig mesh (auto by the first clip's skeleton)
		long   smAtomId = 0;
		std::vector<std::string> undoSm, redoSm;
		std::string idleSm;
		// .nuskel skeleton editor: owned editing copy (metadata hot-applies to the live asset
		// on save) + selection + a bind-pose rig for the overlay.
		nuke::Skeleton* skel = nullptr;
		int    skSelBone = -1, skSelSocket = -1, skSelGroup = -1, skSelChain = -1;
		std::string skRigSkel;
		long   skAtomId = 0;
		std::vector<std::string> undoSk, redoSk;
		std::string idleSk;
		// .nurag ragdoll editor: owned editing copy + capsule selection + fit rig.
		nuke::RagdollDef* rag = nullptr;
		int    rgSelBody = -1;
		std::string rgRigSkel;
		long   rgAtomId = 0;
		std::vector<std::string> undoRg, redoRg;
		std::string idleRg;
		// .nubonemap editor: owned editing copy + the two skeletons framing the name pairs.
		nuke::BoneMap* bmap = nullptr;
		std::string bmSrcSkel, bmDstSkel;
		std::vector<std::string> undoBm, redoBm;
		std::string idleBm;
		// .nublend canvas: owned editing copy + an owned single-state controller driving the rig.
		nuke::BlendSpace* blend = nullptr;
		nuke::AnimSM*     blSm = nullptr;   // "preview-sm": one state, motion = the blend space
		int    blSel = -1;                  // selected point
		float  blQX = 0, blQY = 0;          // preview query marker (drives paramX/paramY)
		std::string blRigSkel;
		long   blAtomId = 0;
		std::vector<std::string> undoB, redoB;
		std::string idleB;
		int       previewMesh = 0;          // .numat: 0 sphere / 1 cube / 2 plane
		int       gizmoOp = 1;              // prefab 3D view: 0 none / 1 move / 2 rotate / 3 scale
		bool      gizmoWorld = true;        // gizmo space: world / local (X toggles, like the viewport)
		long      pendingDeleteId = 0;      // tree ops applied AFTER the tree walk
		long      pendingAddParentId = 0;
		// Animation preview: ticks the subtree's Animators; animSnap restores the pose on stop.
		bool        animPlay = false;
		std::string animSnap;
		uint64_t audioVoice = 0;            // live Preview-bus voice of this window
		float    audioVol = 1.0f;
		float     gizmoMtx[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };   // persistent during a drag
		// Per-window undo/redo: Ctrl+Z/Ctrl+Y route here while this window is focused
		// (Undo/Redo check aeFocused). An edit burst coalesces via the idle-baseline latch.
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
	void DrawSpriteSlicer(AssetEditorWin& w);        // .nutex Sprite Slicer body
	void DrawSequenceEditor(AssetEditorWin& w);      // .nuseq timeline body (sequencer.cpp)
	void DrawAnimEditor(AssetEditorWin& w);          // .nuanim window (animeditor.cpp)
	void DrawSMEditor(AssetEditorWin& w);            // .nusm node graph (smeditor.cpp)
	void DrawBlendEditor(AssetEditorWin& w);         // .nublend canvas (blendeditor.cpp)
	void DrawSkeletonEditor(AssetEditorWin& w);      // .nuskel sockets/groups/IK rig (skeleteditor.cpp)
	void DrawRagdollEditor(AssetEditorWin& w);       // .nurag capsules/joints (ragdolleditor.cpp)
	void DrawBoneMapEditor(AssetEditorWin& w);
	void DrawCursorEditor(AssetEditorWin& w);   // .nucursor: frames/hotspot/fps + animated preview
	void SaveCursorAsset(AssetEditorWin& w);
	// Trigger-tool click: unproject the mouse, raycast the preview mesh, fire the picked
	// event AT the hit (UV or world point, by the space of the event's target mask).
	void FireEventAtPreview(AssetEditorWin& w);       // .nubonemap name pairs (skeleteditor.cpp)
	// Shared timeline widgets (sequencer.cpp), reused by the stage-10 editors:
	// sorted-upsert of a key, a diamond-key row, and the per-component curve strip.
	static void UpsertSharedKey(std::vector<nuke::AnimClip::Key>& keys, double t, const float v[4]);
	static int  SharedKeyRow(const char* id, std::vector<nuke::AnimClip::Key>* keys, float x0, float y,
	                         float pps, double dur, double* dragT, bool& edited);
	static bool DrawKeysCurve(const char* id, std::vector<nuke::AnimClip::Key>& keys, int dim, float height);
	void        DrawInputEditor(AssetEditorWin& w);  // .nuinput editor body (actions/contexts/bindings)
	std::string InputMapJson(AssetEditorWin& w);                              // serialize w.in* -> JSON
	void        LoadInputMapJson(AssetEditorWin& w, const std::string& json); // JSON -> w.in*
	void        SaveInputAsset(AssetEditorWin& w);                            // write the file + apply to live
	void SlicerPushUndo(AssetEditorWin& w);          // snapshot sprite metadata for undo (coalesced)
	// Decode + upload a texture's mip0 as a GPU preview, box-downsampled to longest side <= cap.
	// Returns the handle and fills the uploaded size.
	uint64_t UploadTexPreview(nuke::Texture* t, int cap, int& outW, int& outH);
	void AssetEditorUndo(AssetEditorWin& w);
	void AssetEditorRedo(AssetEditorWin& w);
	void ToggleAnimPreview(AssetEditorWin& w);   // prefab editor ▶/■ (mini-PIE, snapshot-restored)
	void TickAnimPreview(AssetEditorWin& w);     // tick the subtree's Animators while playing
	void DrawPrefabTree(AssetEditorWin& w, Atom* a);        // recursive tree rows (select by id)
	bool DrawPrefabAtomEditor(AssetEditorWin& w, Atom* a);  // transform + components; true if edited

	// viewport
	void winRender();
	// Clickable billboard icons for invisible entities (camera / light / probe / environment),
	// overlaid on the viewport image in edit mode using the gizmo's view/proj.
	void DrawEntityIcons(ImVec2 rmin, ImVec2 sz);
	// The World Partition streaming overlay — XZ cell rectangles colored by state
	// (loaded / parked / cold / loading / HLOD) with per-cell sizes and a summary line.
	void DrawStreamCells(ImVec2 rmin, ImVec2 sz);
	bool streamVizVisible = false;   // toolbar toggle (session-only, like a debug view)
	// This frame's icon hit-rects (min.xy, max.zw -> atom), back-to-front; the viewport
	// click handler tests these BEFORE the scene ray-pick (topmost icon wins).
	std::vector<std::pair<ImVec4, Atom*>> iconHits;
	// dialogs
	void winAbout();
	void winConsole();
	// "Last session crashed" viewer: filled at boot from CrashReport::PendingBundle().
	void winCrash();
	bool        crashShow = false;
	std::string crashDir, crashInfo, crashText;
	// browser
	const char* ExtIcon(const std::string& ext);
	bool ExtVisible(const std::string& ext);
	bool SearchMatch(const std::string& name);
	void BrowserTree(const std::string& dir);
	Atom* SpawnPrefab(const std::string& path);
	void StartRename(const std::string& path);
	void EntryContextMenu(const std::string& path, bool isDir);
	void DrawRenamePopup();
	void winBrowser();
	void BrowserNavigate(const std::string& path);   // change folder + push history (clears forward)
	void BrowserBack();                              // M4 / back button
	void BrowserForward();                           // M5 / forward button
	// Browser drag & drop; payload "NUKE_ASSET" = full path (folder = move, viewport = instantiate).
	void BrowserDragSource(const std::string& path);
	void BrowserFolderDropTarget(const std::string& folderPath);
	void SaveAtomAsPrefab(Atom* a, const std::string& folder);   // drag an atom into the browser -> .nuprefab
	void ApplyToPrefab(Atom* a);                                 // push this instance's state into its prefab file
	void ResetToPrefab(Atom* a);                                 // revert this instance to the prefab's saved state
	void BrowserPaste();                                          // paste the clipboard into the current folder (cut=move, copy=duplicate)
	void BrowserDelete(const std::string& path);                 // delete a file/folder from disk (immediate)
	void BrowserSelect(const std::string& path);                 // single-select: primary + multi-set + anchor
	// The multi-selection when non-empty, else the primary (external writers only set browserSel).
	std::vector<std::string> BrowserSelection() const;
	void DrawDeletePopup();                                       // browser delete-confirm modal (Enter=Yes, Esc=Cancel)
	void RequestDelete(const std::vector<std::string>& paths);   // compute dependents + open the confirm modal
	void PerformDelete(const std::string& path);                 // single-path convenience (= PerformDeletes)
	// Batch delete: ONE content scan for the whole set, whatever its size (unlink included).
	void PerformDeletes(const std::vector<std::string>& paths);
	// Content files referencing ANY of the guids — one pass over the content tree.
	std::vector<std::string> FindDependents(const std::vector<std::string>& guids);
	void UnlinkResource(const std::string& guid);                // reset refs to a guid -> defaults (all worlds/assets)
	void UnlinkResources(const std::vector<std::string>& guids); // batch: one pass for the whole set
	void UpdateWindowTitle();                                     // "NukeEngine Editor — <project> — <world>"
	// Disk<->editor world sync.
	void SyncWorldBaseline();        // call after open/new/save: snapshot the on-disk state + clear dirty
	void TrackDirty();               // per-frame: recompute worldDirty, refresh title on change
	void TrackExternalChange();      // per-frame: detect a disk edit of the open world + act per settings
	void ReloadWorld(const std::string& diskJson);   // load disk content, reset baseline + undo
	void OverwriteWorld();           // save editor state over disk + reset baseline
	void DrawReloadPopup();          // "changed on disk (editor clean): reload?"
	void DrawConflictPopup();        // "changed on disk AND in editor: reload / overwrite / merge / ignore"
	// Merge/resolve window: per-object/param diff of editor vs disk, pick a side per node.
	void OpenMerge(const std::string& editorJson, const std::string& diskJson);
	void DrawMergeWindow();
	void AcceptAssetDropTarget();                    // viewport/hierarchy: accept an asset drop
	Atom* DropAsset(const std::string& path);        // instantiate by extension; returns the new atom (or null)
	// Drop a .numat (-> material) or .nutex (-> base color) onto an atom. Undoable;
	// no-op if the atom has no MeshRenderer.
	void  DropAssetOnAtom(Atom* a, const std::string& path);
	Atom* SpawnMeshAsset(const std::string& path);   // .numesh -> new Atom + MeshRenderer
	// Create new assets in a content folder (from the browser's "New" menu).
	void CreateFolderAsset(const std::string& folder);
	void CreateWorldAsset(const std::string& folder);    // empty .nuworld
	void CreateMaterialAsset(const std::string& folder); // default .numat (registered in ResDB)
	void CreateBoneMapAsset(const std::string& folder);  // .nubonemap retarget map (JSON)
	void CreateAnimSMAsset(const std::string& folder);   // .nusm animation state machine (JSON)
	void CreateBlendSpaceAsset(const std::string& folder); // .nublend blend space (JSON)
	void CreateSequenceAsset(const std::string& folder);   // .nuseq sequence (JSON)
	void CreateShaderAsset(const std::string& folder);   // .vs/.ps.hlsl pair (registered + pipeline built)
	void CreateRenderTextureAsset(const std::string& folder);   // .nutex RenderTexture (camera target)
	// plugins
	void PluginMGRWindow();
	// status bar: frame stats + scene counters + plugin fields (nuke::StatusBar)
	void StatusBarPanel();
	// toolbar
	bool ToolBtn(const char* icon, const char* tip, bool active, float w);
	void SpawnEmpty();
	void SpawnPrimitive(const char* atomName, const char* guid);
	// Spawn placement in front of the editor camera (on the ray-hit surface if any).
	// FinishSpawn positions, adds, selects and records a freshly-created atom.
	Vector3 SpawnPos();
	Atom*   FinishSpawn(Atom* atom);
	void SpawnCube();
	void SpawnCamera();
	void SpawnLight(int type, const char* atomName);   // type 0=dir 1=point 2=spot
	void SpawnEnvironment();                           // atom + Environment (sky/ambient)
	void SpawnReflectionProbe();                       // atom + ReflectionProbe (scene-captured reflections)
	// Background project load. 0 = done, 1 = content+shaders on a worker, 2 = pipelines/world,
	// 3 = warm-up frame. Non-zero locks the Hierarchy/Inspector/Viewport/Browser panels.
	int bootLoading = 0;
	std::string bootWorldRel;
	double bootPrevActBudget = 0.0;
	void StartBootLoad();   // start the background tail of SetUp()
	void PumpBootLoad();    // per frame: pipelines, world activation, unlock
	void Toolbar();
	void Draw();
	// undo/redo (generic command stack)
	void Undo();
	void Redo();
	// Push an undoable command. worldEdit=false for commands that do not change the open
	// world (file moves, project settings): undoable, but they must not flip the dirty "*".
	void PushUndo(const std::string& label, std::function<void()> undoFn, std::function<void()> redoFn,
	              bool worldEdit = true);
	void TrackUndo();                  // detect a settled selected-atom edit (inspector/gizmo); not in PIE
	void ResetUndo();                  // clear history (on world load / new)
	void ApplyAtomState(long id, long parentId, int index, const std::string& json);   // undo primitive
	void RecordAdd(Atom* a);                                       // an atom was created
	// An atom moved in the hierarchy. beforeJson = its serialized state BEFORE the move.
	void RecordReparent(Atom* a, long oldParent, int oldIndex, const std::string& beforeJson);
	void RecordDelete(Atom* a);                                   // an atom was deleted
	void DeleteSelectedAtom();                                    // hierarchy: delete the selected atom (undoable)
	void ApplyPendingAtomDelete();                                // ...applied after the UI walk
	// Atom clipboard, all undoable. Copy serializes the subtree onto the OS clipboard as a
	// JSON envelope (so paste works across editor instances); paste clones with fresh ids.
	void CopySelectedAtom();
	void CutSelectedAtom();                                       // copy + undoable delete
	void PasteAtom();
	void DuplicateSelectedAtom();
	bool AtomClipboardAvailable();                                // clipboard holds an atom envelope

	// ---- selection ops (selection.cpp) ------------------------------------------------
	void HierSelect(Atom* a);                    // plain click: collapse to a single selection
	void HierToggle(Atom* a);                    // ctrl-click: add/remove; primary follows
	void HierRange(Atom* a, bool additive);      // shift(+ctrl) click: anchor range over hierRowsPrev
	std::vector<Atom*> SelectionTopLevel();      // selection minus atoms with a selected ancestor
	void DeleteSelection();                      // whole selection, ONE undo step
	void DuplicateSelection();
	void CopySelection();                        // multi-atom clipboard envelope
	void CutSelection();
	void PasteAtoms();                           // single or multi envelope
	void SetSelectionEnabled(bool on);           // context toggle over the whole selection
	Atom* CreateFolderAtom(Atom* parent);        // folder node (undoable, selected)
	void GroupSelection(bool asFolder);          // Ctrl+G — new parent at the bounds center
	void UngroupSelection();                     // children out (world poses kept), shell removed
	void RemoveComponent(Atom* a, Component* c);                  // inspector: remove a component (undoable)
	void MoveComponent(Atom* src, Component* c, Atom* dst);       // DnD: move a component to another atom (undoable)
	void RecordFileMove(const std::string& from, const std::string& to);   // a file/folder was renamed or moved
	// Generic value edit (settings, paths, flags…): records before/after of any comparable value.
	template<class T> void RecordChange(const std::string& label, T* slot, const T& before, const T& after)
	{ if (before == after) return; PushUndo(label, [slot, before]{ *slot = before; }, [slot, after]{ *slot = after; }); }
};

inline void editorinit() { EditorUI::getSingleton()->SetUp(); }
inline void editorDraw() { EditorUI::getSingleton()->Draw(); }

#endif // EDITORUI_H
