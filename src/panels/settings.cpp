// settings panel — hotkeys, world commands, Project/World Settings windows. EditorUI methods.
#include <editor/editorui.h>
#include "nukeui.h"
#include <API/Model/Layers.h>
#include <API/Model/Wind.h>
#include <API/Model/Surface.h>
#include <API/Model/Package.h>   // pak compression levels per method
#include <input/Input.h>   // Input Maps section (filter + provenance)
#include <map>
#include <algorithm>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <config.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <sstream>
#include <utility>
#include <set>

// Register the editor's built-in hotkeys in the shared pool; conflicts resolve to unbound.
void EditorUI::RegisterHotkeys()
{
	nuke::Hotkeys* hk = nuke::Hotkeys::Get();
	hk->Register("editor.world.new",  "New World",            ImGuiMod_Ctrl | ImGuiKey_N,     [this] { NewWorldCmd(); });
	hk->Register("editor.world.save", "Save World",           ImGuiMod_Ctrl | ImGuiKey_S,     [this] { SaveWorldCmd(); });
	hk->Register("editor.world.saveas", "Save World As",     ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, [this] { SaveWorldAsCmd(); });
	hk->Register("editor.world.open", "Open Default World",   ImGuiMod_Ctrl | ImGuiKey_O,     [this] { OpenWorldCmd(startupWorld); });
	hk->Register("editor.settings",   "Project Settings",     ImGuiMod_Ctrl | ImGuiKey_Comma, [this] { settingsOpen = true; });
	hk->Register("editor.edit.undo",  "Undo",                 ImGuiMod_Ctrl | ImGuiKey_Z,     [this] { Undo(); });
	hk->Register("editor.edit.redo",  "Redo",                 ImGuiMod_Ctrl | ImGuiKey_Y,     [this] { Redo(); });
	// Context hotkeys carry a null action: DispatchHotkeys skips them, the panels dispatch them.
	hk->Register("editor.browser.back",    "Browser: Back",    ImGuiKey_MouseX1, nullptr);
	hk->Register("editor.browser.forward", "Browser: Forward", ImGuiKey_MouseX2, nullptr);
	hk->Register("editor.delete",          "Delete",           ImGuiKey_Delete, nullptr);
	hk->Register("editor.delete.force",    "Delete (no confirm)", ImGuiMod_Shift | ImGuiKey_Delete, nullptr);
	// One id per clipboard chord: the pool forbids a second binding of the same chord, so
	// window-scoped actions must share ids and dispatch per focused window.
	hk->Register("editor.cut",       "Cut",       ImGuiMod_Ctrl | ImGuiKey_X, nullptr);
	hk->Register("editor.copy",      "Copy",      ImGuiMod_Ctrl | ImGuiKey_C, nullptr);
	hk->Register("editor.paste",     "Paste",     ImGuiMod_Ctrl | ImGuiKey_V, nullptr);
	hk->Register("editor.duplicate", "Duplicate", ImGuiMod_Ctrl | ImGuiKey_D, nullptr);
}

// Fire bound hotkeys whose chord is pressed this frame.
void EditorUI::DispatchHotkeys()
{
	if (ImGui::GetIO().WantTextInput) return;   // typing in a field
	if (!rebindId.empty()) return;              // capturing a rebind — swallow input
	for (const nuke::Hotkey& h : nuke::Hotkeys::Get()->All())
		if (h.bound && h.action && ImGui::IsKeyChordPressed((ImGuiKeyChord)h.chord))
			h.action();
}

// A menu entry whose shortcut text and action come from the pooled hotkey `id`.
void EditorUI::MenuHotkeyItem(const char* label, const char* id)
{
	nuke::Hotkey* h = nuke::Hotkeys::Get()->Find(id);
	const char* sc = (h && h->bound) ? ImGui::GetKeyChordName((ImGuiKeyChord)h->chord) : nullptr;
	if (ImGui::MenuItem(label, sc) && h && h->action) h->action();
}

void EditorUI::NewWorldCmd()
{
	// Unsaved changes are never silently thrown away (a lost world taught us this).
	if (WorldEditSerial() != savedWorldSerial) { openNewWorldConfirm = true; return; }
	DoNewWorld();
}

// New World is UNDOABLE: the whole old world rides in the closure (the PIE-snapshot tech),
// so Ctrl+Z brings everything back, path included.
void EditorUI::DoNewWorld()
{
	AppInstance* app = AppInstance::GetSingleton();
	const std::string snapshot = app->currentWorld ? app->currentWorld->SaveToString() : std::string();
	const std::string oldPath = app->currentWorldPath;
	app->NewWorld();
	editing = false; editAtomId = 0;   // own command: the auto edit-detector stays out
	PushUndo("New World",
		[snapshot, oldPath]{
			AppInstance* a = AppInstance::GetSingleton();
			if (a->currentWorld && !snapshot.empty()) a->currentWorld->LoadFromString(snapshot);
			a->currentWorldPath = oldPath;
			if (!oldPath.empty()) a->NameWorldFromPath(oldPath);
		},
		[]{ AppInstance::GetSingleton()->NewWorld(); });
	SyncWorldBaseline();
}

void EditorUI::SaveWorldCmd()
{
	AppInstance* app = AppInstance::GetSingleton();
	// No path = no guessing. The old fallback silently targeted the STARTUP world's file and
	// once overwrote a real world with an empty one — an unsaved world goes through Save As.
	if (app->currentWorldPath.empty()) { SaveWorldAsCmd(); return; }
	app->SaveWorld(app->currentWorldPath);
	if (startupWorld.empty()) { startupWorld = app->currentWorldPath; SaveProject(); }   // first world becomes the default
	SyncWorldBaseline();
}

void EditorUI::OpenWorldCmd(const std::string& relPath)
{
	if (relPath.empty()) return;
	pendingWorldOpen = relPath;   // applied at the frame boundary (see ApplyPendingWorldOpen)
}

// Open the "Save World As" modal, defaulting the folder to the browser's current one.
void EditorUI::SaveWorldAsCmd()
{
	AppInstance* app = AppInstance::GetSingleton();
	saveAsDir = browserCwd.empty() ? contentDir : browserCwd;
	std::string name = !app->currentWorldPath.empty() ? bfs::path(app->currentWorldPath).filename().string()
	                                                   : std::string("untitled.nuworld");
	strncpy(saveAsBuf, name.c_str(), sizeof(saveAsBuf) - 1); saveAsBuf[sizeof(saveAsBuf) - 1] = 0;
	openSaveAsPopup = true;
}

// Recursive folder tree for the save dialog; clicking a folder selects it as the save target.
void EditorUI::SaveAsFolderTree(const std::string& dir)
{
	boost::system::error_code ec;
	for (auto& de : bfs::directory_iterator(bfs::path(dir), ec))
	{
		if (!bfs::is_directory(de.path())) continue;
		std::string full = de.path().string();
		ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
		if (full == saveAsDir) fl |= ImGuiTreeNodeFlags_Selected;
		bool open = ImGui::TreeNodeEx((std::string(ICON_LC_FOLDER) + " " + de.path().filename().string()).c_str(), fl);
		if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) saveAsDir = full;
		if (open) { SaveAsFolderTree(full); ImGui::TreePop(); }
	}
}

void EditorUI::DrawSaveAsPopup()
{
	// New World over a dirty world: Save / Discard / Cancel — never a silent wipe.
	if (openNewWorldConfirm) { ImGui::OpenPopup("New World?"); openNewWorldConfirm = false; }
	if (ImGui::BeginPopupModal("New World?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		AppInstance* app = AppInstance::GetSingleton();
		ImGui::TextUnformatted("The current world has unsaved changes.");
		const bool canSave = !app->currentWorldPath.empty();
		if (!canSave) ImGui::TextDisabled("This world was never saved — use Save As first to keep it.");
		if (canSave)
		{
			if (ImGui::Button(ICON_LC_SAVE " Save & New")) { SaveWorldCmd(); DoNewWorld(); ImGui::CloseCurrentPopup(); }
			ImGui::SameLine();
		}
		if (ImGui::Button(ICON_LC_TRASH_2 " Discard & New")) { DoNewWorld(); ImGui::CloseCurrentPopup(); }
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	if (openSaveAsPopup) { ImGui::OpenPopup("Save World As"); openSaveAsPopup = false; }
	if (ImGui::BeginPopupModal("Save World As", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("Folder (under project content):");
		ImGui::BeginChild("##satree", ImVec2(400, 220), true);
		ImGuiTreeNodeFlags rf = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
		if (saveAsDir == contentDir) rf |= ImGuiTreeNodeFlags_Selected;
		bool rootOpen = ImGui::TreeNodeEx(ICON_LC_FOLDER " content", rf);
		if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) saveAsDir = contentDir;
		if (rootOpen) { SaveAsFolderTree(contentDir); ImGui::TreePop(); }
		ImGui::EndChild();

		ImGui::SetNextItemWidth(400);
		bool enter = ImGui::InputText("Name", saveAsBuf, sizeof(saveAsBuf), ImGuiInputTextFlags_EnterReturnsTrue);

		std::string name = saveAsBuf;
		const std::string ext = ".nuworld";
		if (!name.empty() && (name.size() < ext.size() || name.compare(name.size() - ext.size(), ext.size(), ext) != 0)) name += ext;
		boost::system::error_code ec;
		std::string folder = saveAsDir.empty() ? contentDir : saveAsDir;
		bfs::path   full    = bfs::path(folder) / name;
		bfs::path   relp    = bfs::relative(full, bfs::path(contentDir), ec);
		std::string rel     = (!ec && !relp.empty()) ? relp.generic_string() : name;
		bool        exists  = !name.empty() && bfs::exists(full, ec);
		bool        empty   = (saveAsBuf[0] == 0);

		ImGui::Text("-> content/%s", rel.c_str());
		if (exists) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1), ICON_LC_TRIANGLE_ALERT " Already exists — will be overwritten.");

		const char* label = exists ? "Overwrite" : "Save";
		ImGui::BeginDisabled(empty);
		// Enter saves a new file directly; an overwrite requires the explicit button.
		if (ImGui::Button(label, ImVec2(120, 0)) || (enter && !exists && !empty))
		{
			AppInstance::GetSingleton()->SaveWorld(rel);
			SyncWorldBaseline();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

// Queue a .nuworld given by full path (browser double-click); converted to content-relative.
void EditorUI::OpenWorldFromBrowser(const std::string& fullPath)
{
	boost::system::error_code ec;
	bfs::path rel = bfs::relative(bfs::path(fullPath), bfs::path(contentDir), ec);
	pendingWorldOpen = (!ec && !rel.empty()) ? rel.generic_string() : fullPath;
}

// Perform a queued world switch. Must run FIRST in the render callback, before any recording:
// tearing the world down mid-frame frees resources the open command list still references.
void EditorUI::ApplyPendingWorldOpen()
{
	// Dev hook: NUKE_OPEN_WORLD=<content-relative> queues that world ~1.5 s after boot.
	static int devDelay = -2;
	if (devDelay == -2)
	{
		const char* w = std::getenv("NUKE_OPEN_WORLD");
		devDelay = (w && w[0]) ? 90 : -1;
	}
	if (devDelay > 0 && --devDelay == 0) pendingWorldOpen = std::getenv("NUKE_OPEN_WORLD");

	if (pendingWorldOpen.empty()) return;
	std::string path;
	path.swap(pendingWorldOpen);
	AppInstance::GetSingleton()->OpenWorld(path);
	ResetUndo();
	SyncWorldBaseline();
}

// Persist the RTX quality block into the editor's config json (read-modify-write; other
// sections are preserved).
static void WriteRTBlock(const boost::filesystem::path& cfg, float inten, float maxDist, int bounces, float rough)
{
	try
	{
		nlohmann::json j;
		{
			boost::filesystem::ifstream in(cfg);
			if (in) { std::stringstream ss; ss << in.rdbuf(); j = nlohmann::json::parse(ss.str(), nullptr, false, true); }
		}
		if (!j.is_object()) j = nlohmann::json::object();
		j["raytracing"] = { {"intensity", inten}, {"maxDist", maxDist}, {"bounces", bounces}, {"roughCutoff", rough} };
		boost::system::error_code ec;
		if (cfg.has_parent_path()) boost::filesystem::create_directories(cfg.parent_path(), ec);
		boost::filesystem::ofstream out(cfg, std::ios::trunc);
		if (out) out << j.dump(2);
	}
	catch (...) {}
}

// Apply a full project-settings snapshot: editor members, engine config, live renderer state,
// and persistence (game.nuproj + config/main.json). One call fully restores a state.
void EditorUI::ApplyProjectSettings(const ProjectSettings& ps)
{
	msaaSamples     = ps.msaa;
	hdrEnabled      = ps.hdr;
	hdrPaperWhite   = ps.paperWhite;
	hdrPeak         = (ps.peak < ps.paperWhite) ? ps.paperWhite : ps.peak;
	reloadCleanMode = ps.reloadClean;
	conflictMode    = ps.conflict;
	if (nuke::Config* cfg = nuke::Config::getSingleton())
	{
		cfg->rt.intensity   = ps.rtIntensity;
		cfg->rt.maxDist     = ps.rtMaxDist;
		cfg->rt.bounces     = ps.rtBounces;
		cfg->rt.roughCutoff = ps.rtRoughCutoff;
	}
	if (iRender* r = AppInstance::GetSingleton()->render)
	{
		r->setMSAA(msaaSamples); msaaSamples = r->getMSAA();   // device may clamp
		r->setHDR(hdrEnabled);
		r->setHDRNits(hdrPaperWhite, hdrPeak);
		r->setRTReflection(ps.rtIntensity, ps.rtMaxDist, ps.rtBounces, ps.rtRoughCutoff);
	}
	SaveProject();   // msaa/hdr/nits/disk-modes live in game.nuproj
	WriteRTBlock(nuke::Config::baseDir() / "config" / "main.json", ps.rtIntensity, ps.rtMaxDist, ps.rtBounces, ps.rtRoughCutoff);
}

void EditorUI::winSettings()
{
	if (!settingsOpen) return;
	NukeUI::DocPanel("panel:settings", "Project Settings", &settingsOpen, 0, 680, 500, [this]()
	{
		// Snapshot helpers for the undoable project settings.
		auto capturePS = [this]() -> ProjectSettings {
			nuke::NukeRT rt; if (nuke::Config* c = nuke::Config::getSingleton()) rt = c->rt;
			return { msaaSamples, hdrEnabled, hdrPaperWhite, hdrPeak, rt.intensity, rt.maxDist, rt.bounces, rt.roughCutoff, reloadCleanMode, conflictMode };
		};
		auto defaultPS = []() -> ProjectSettings {
			nuke::NukeRT d;
			return { 4, true, 200.0f, 1000.0f, d.intensity, d.maxDist, d.bounces, d.roughCutoff, 0, 0 };
		};
		auto samePS = [](const ProjectSettings& a, const ProjectSettings& b) {
			return a.msaa == b.msaa && a.hdr == b.hdr && a.paperWhite == b.paperWhite && a.peak == b.peak
			    && a.rtIntensity == b.rtIntensity && a.rtMaxDist == b.rtMaxDist && a.rtBounces == b.rtBounces
			    && a.rtRoughCutoff == b.rtRoughCutoff && a.reloadClean == b.reloadClean && a.conflict == b.conflict;
		};
		// Per-field reset button; `after` = the current snapshot with one field defaulted.
		auto resetBtn = [&](const char* id, const ProjectSettings& after, const char* label) {
			ImGui::SameLine();
			ImGui::PushID(id);
			bool hit = ImGui::SmallButton(ICON_LC_ROTATE_CCW);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset to default");
			ImGui::PopID();
			if (hit)
			{
				ProjectSettings before = capturePS();
				if (!samePS(before, after))
				{
					ApplyProjectSettings(after);
					psBefore = capturePS();   // new idle baseline, else the settler double-records
					PushUndo(label, [this, before]{ ApplyProjectSettings(before); },
					                [this, after ]{ ApplyProjectSettings(after ); });
				}
			}
		};

		const ProjectSettings PSD = defaultPS();   // per-field default source for resetBtn

		static const char* kCats[] = { "World", "Rendering", "Packaging", "Modules", "Mods", "Disk sync", "Layers", "Input" };
		shellProj.Begin("projset", kCats, IM_ARRAYSIZE(kCats));
		if (shellProj.Section("World", "World", "default world startup scene"))
		{
		std::vector<std::string> worlds;
		{
			boost::system::error_code ec;
			bfs::path root(contentDir);
			if (bfs::exists(root, ec))
				for (bfs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec))
				{
					if (ec) break;
					if (bfs::is_directory(it->path())) continue;
					// Cell files of a split-saved streamed world are internals, not worlds.
					const std::string rel = bfs::relative(it->path(), root, ec).generic_string();
					if (rel.find(".cells/") != std::string::npos) continue;
					if (it->path().extension() == ".nuworld")
						worlds.push_back(bfs::relative(it->path(), root, ec).string());
				}
		}
		const char* cur = startupWorld.empty() ? "(none)" : startupWorld.c_str();
		if (ImGui::BeginCombo(LProp("Default World").c_str(), cur))
		{
			for (auto& w : worlds)
				if (ImGui::Selectable(w.c_str(), w == startupWorld) && w != startupWorld)
				{
					std::string before = startupWorld, after = w;
					startupWorld = after; SaveProject();
					PushUndo("Default world", [this, before]{ startupWorld = before; SaveProject(); },
					                          [this, after ]{ startupWorld = after;  SaveProject(); },
					         false);   // project setting, not a change of the open world
				}
			ImGui::EndCombo();
		}
		}
		if (shellProj.Section("Rendering", "Rendering", "anti-aliasing msaa hdr paper white peak nits rtx intensity bounces reflections"))
		{
		const char* aaModes[] = { "Off", "MSAA 2x", "MSAA 4x", "MSAA 8x" };
		int aaIdx = (msaaSamples >= 8) ? 3 : (msaaSamples >= 4) ? 2 : (msaaSamples >= 2) ? 1 : 0;
		if (ImGui::Combo(LProp("Anti-aliasing").c_str(), &aaIdx, aaModes, IM_ARRAYSIZE(aaModes)))
		{
			int s = (aaIdx == 3) ? 8 : (aaIdx == 2) ? 4 : (aaIdx == 1) ? 2 : 1;
			msaaSamples = s;
			if (AppInstance::GetSingleton()->render)
			{
				AppInstance::GetSingleton()->render->setMSAA(s);
				msaaSamples = AppInstance::GetSingleton()->render->getMSAA();   // device may clamp (e.g. 8x->4x)
			}
			SaveProject();
		}
		{ ProjectSettings a = capturePS(); a.msaa = PSD.msaa; resetBtn("rst_aa", a, "Reset Anti-aliasing"); }
		ImGui::SameLine(); ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hardware multisampling for the world. Clamped to GPU support.");

		if (ImGui::Checkbox(LProp("HDR").c_str(), &hdrEnabled))
		{
			if (AppInstance::GetSingleton()->render) AppInstance::GetSingleton()->render->setHDR(hdrEnabled);
			SaveProject();
		}
		{ ProjectSettings a = capturePS(); a.hdr = PSD.hdr; resetBtn("rst_hdr", a, "Reset HDR"); }
		ImGui::SameLine(); ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("On: float (RGBA16F) rendering, real dynamic range (enables bloom later).\nOff: LDR (RGBA8), cheaper, tonemap inline.");

		// HDR10 display mapping; only affects real HDR10 output in the Player.
		bool nitsCh = false;
		nitsCh |= ImGui::SliderFloat(LProp("HDR Paper White (nits)").c_str(), &hdrPaperWhite, 80.0f, 400.0f, "%.0f");
		{ ProjectSettings a = capturePS(); a.paperWhite = PSD.paperWhite; resetBtn("rst_pw", a, "Reset HDR Paper White"); }
		nitsCh |= ImGui::SliderFloat(LProp("HDR Peak (nits)").c_str(), &hdrPeak, 200.0f, 4000.0f, "%.0f");
		{ ProjectSettings a = capturePS(); a.peak = PSD.peak; resetBtn("rst_peak", a, "Reset HDR Peak"); }
		if (nitsCh)
		{
			if (hdrPeak < hdrPaperWhite) hdrPeak = hdrPaperWhite;
			if (AppInstance::GetSingleton()->render) AppInstance::GetSingleton()->render->setHDRNits(hdrPaperWhite, hdrPeak);
			SaveProject();
		}

		// Global RT reflection quality; the per-camera "rtreflect" post effect is the on/off switch.
		{
			nuke::Config* cfg = nuke::Config::getSingleton();
			if (cfg)
			{
				nuke::NukeRT& rt = cfg->rt;
				bool ch = false, done = false;
				ch |= ImGui::SliderFloat(LProp("RTX Intensity").c_str(), &rt.intensity, 0.0f, 2.0f, "%.2f");        done |= ImGui::IsItemDeactivatedAfterEdit();
				{ ProjectSettings a = capturePS(); a.rtIntensity = PSD.rtIntensity; resetBtn("rst_rti", a, "Reset RTX Intensity"); }
				ch |= ImGui::SliderFloat(LProp("RTX Max Distance").c_str(), &rt.maxDist, 1.0f, 1000.0f, "%.0f");     done |= ImGui::IsItemDeactivatedAfterEdit();
				{ ProjectSettings a = capturePS(); a.rtMaxDist = PSD.rtMaxDist; resetBtn("rst_rtd", a, "Reset RTX Max Distance"); }
				ch |= ImGui::SliderInt(LProp("RTX Bounces").c_str(), &rt.bounces, 1, 7);                             done |= ImGui::IsItemDeactivatedAfterEdit();
				{ ProjectSettings a = capturePS(); a.rtBounces = PSD.rtBounces; resetBtn("rst_rtb", a, "Reset RTX Bounces"); }
				ch |= ImGui::SliderFloat(LProp("RTX Roughness Cutoff").c_str(), &rt.roughCutoff, 0.05f, 1.0f, "%.2f"); done |= ImGui::IsItemDeactivatedAfterEdit();
				{ ProjectSettings a = capturePS(); a.rtRoughCutoff = PSD.rtRoughCutoff; resetBtn("rst_rtr", a, "Reset RTX Roughness Cutoff"); }
				ImGui::SameLine(); ImGui::TextDisabled("(?)");
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Global RT reflection quality. Add the 'rtreflect' post effect to a camera to enable RT there\n(needs an RT-capable backend: D3D12 on Windows, Vulkan on Linux).");
				if (ch && AppInstance::GetSingleton()->render)   // live preview
					AppInstance::GetSingleton()->render->setRTReflection(rt.intensity, rt.maxDist, rt.bounces, rt.roughCutoff);
				if (done)   // persist on edit-end
					WriteRTBlock(nuke::Config::baseDir() / "config" / "main.json", rt.intensity, rt.maxDist, rt.bounces, rt.roughCutoff);
			}
		}

		}
		// Packaging: the project pak is the release artifact, mod paks are editable overlays.
		if (shellProj.Section("Packaging", "Packaging", "game name icon build path dist pak compression zstd split"))
		{
			// Name, icon and window title of the shipped exe, applied by Package Project.
			char nameBuf[128];
			strncpy(nameBuf, projectName.c_str(), sizeof(nameBuf) - 1); nameBuf[sizeof(nameBuf) - 1] = 0;
			if (ImGui::InputText(LProp("Game name").c_str(), nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue)
			    || (ImGui::IsItemDeactivatedAfterEdit()))
			{
				if (projectName != nameBuf && nameBuf[0]) { projectName = nameBuf; SaveProject(); }
			}
			// Game icon. The value stays content-relative; a file picked outside the project
			// is copied into content, since the pak must be self-contained.
			{
				nuke::iRender* r = AppInstance::GetSingleton()->render;
				if (gameIcon != iconPrevPath)   // (re)build the preview texture lazily
				{
					if (iconPrevTex && r) { r->destroyTexture2D(iconPrevTex); iconPrevTex = 0; }
					iconPrevPath = gameIcon;
					std::vector<unsigned char> rgba; int iw = 0, ih = 0;
					if (!gameIcon.empty() && r
					    && DecodeIcoRGBA(AppInstance::GetSingleton()->ResolveContent(gameIcon), rgba, iw, ih)
					    && iw > 0 && ih > 0)
						iconPrevTex = r->createTexture2D(rgba.data(), iw, ih);
				}
				const float box = 40.0f;
				LProp("Game icon");
				if (iconPrevTex) ImGui::Image((ImTextureID)iconPrevTex, ImVec2(box, box));
				else
				{
					ImVec2 p0 = ImGui::GetCursorScreenPos();
					ImGui::Dummy(ImVec2(box, box));
					ImGui::GetWindowDrawList()->AddRect(p0, ImVec2(p0.x + box, p0.y + box), IM_COL32(120, 120, 130, 255));
					ImGui::GetWindowDrawList()->AddText(ImVec2(p0.x + 7, p0.y + box * 0.3f), IM_COL32(120, 120, 130, 255), "ico");
				}
				ImGui::SameLine();
				ImGui::BeginGroup();
				std::string cur = gameIcon.empty() ? "(none)" : bfs::path(gameIcon).filename().string();
				if (ImGui::Button((cur + "##gicon").c_str(), ImVec2(220, 0))) ImGui::OpenPopup("##giconpop");
				if (ImGui::BeginDragDropTarget())   // drag an .ico from the browser onto the button
				{
					if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NUKE_ASSET"))
					{
						std::string path((const char*)p->Data);
						std::string e = bfs::path(path).extension().string();
						for (char& c : e) c = (char)tolower((unsigned char)c);
						if (e == ".ico")
						{
							boost::system::error_code ec;
							std::string rel = bfs::relative(bfs::path(path), bfs::path(contentDir), ec).generic_string();
							if (!ec && !rel.empty()) { gameIcon = rel; SaveProject(); }
						}
					}
					ImGui::EndDragDropTarget();
				}
				if (ImGui::BeginPopup("##giconpop"))   // every .ico in the project content
				{
					if (ImGui::Selectable("(none)", gameIcon.empty())) { gameIcon.clear(); SaveProject(); ImGui::CloseCurrentPopup(); }
					boost::system::error_code ec;
					bfs::path croot(contentDir);
					if (bfs::exists(croot, ec))
						for (bfs::recursive_directory_iterator dit(croot, ec), dend; dit != dend; dit.increment(ec))
						{
							if (ec) break;
							if (bfs::is_directory(dit->path())) continue;
							std::string e = dit->path().extension().string();
							for (char& c : e) c = (char)tolower((unsigned char)c);
							if (e != ".ico") continue;
							std::string rel = bfs::relative(dit->path(), croot, ec).generic_string();
							if (ImGui::Selectable((rel + "##gi").c_str(), rel == gameIcon))
							{ gameIcon = rel; SaveProject(); ImGui::CloseCurrentPopup(); }
						}
					ImGui::EndPopup();
				}
				ImGui::SameLine(0, 2);
				if (ImGui::Button(ICON_LC_FOLDER_SEARCH "##gibrowse"))   // native dialog; copies into content
				{
					std::string picked = EditorPickIconFile();
					if (!picked.empty())
					{
						boost::system::error_code ec;
						bfs::path src(picked);
						bfs::path rel = bfs::relative(src, bfs::path(contentDir), ec);
						if (ec || rel.empty() || rel.string().compare(0, 2, "..") == 0)
						{
							bfs::path dst = bfs::path(contentDir) / src.filename();
							for (int k = 1; bfs::exists(dst, ec); ++k)
								dst = bfs::path(contentDir) / (src.stem().string() + "_" + std::to_string(k) + ".ico");
							bfs::copy_file(src, dst, ec);
							if (!ec) { gameIcon = bfs::relative(dst, bfs::path(contentDir), ec).generic_string(); SaveProject(); }
						}
						else { gameIcon = rel.generic_string(); SaveProject(); }
					}
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Browse for an .ico (copied into content if outside the project)");
				ImGui::SameLine(0, 2);
				if (ImGui::Button(ICON_LC_ROTATE_CCW "##girst")) { gameIcon.clear(); SaveProject(); }
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear (ship the default player icon)");
				ImGui::EndGroup();
			}
			// Build output folder; defaults to <project>/dist. Relative paths resolve
			// against the project.
			{
				std::string shown = distPath.empty() ? std::string("dist  (default, in the project root)") : distPath;
				LProp("Build path");
				if (ImGui::Button((shown + "##distp").c_str(), ImVec2(320, 0)))
				{
					std::string picked = EditorPickFolder();
					if (!picked.empty())
					{
						boost::system::error_code ec;
						bfs::path rel = bfs::relative(bfs::path(picked), bfs::path(projectDir), ec);
						std::string r = ec ? std::string() : rel.generic_string();
						// Inside the project -> relative (portable .nuproj); outside -> absolute.
						distPath = (!r.empty() && r != "." && r.compare(0, 2, "..") != 0) ? r : picked;
						if (r == ".") distPath.clear();   // the project root is not a build dir
						SaveProject();
					}
				}
				if (ImGui::IsItemHovered())
				{
					std::string eff = distPath.empty() ? (bfs::path(projectDir) / "dist").string()
					                : (bfs::path(distPath).is_absolute() ? distPath : (bfs::path(projectDir) / distPath).string());
					ImGui::SetTooltip("Package Project output:\n%s\nClick to pick a folder.", eff.c_str());
				}
				ImGui::SameLine(0, 2);
				if (ImGui::Button(ICON_LC_ROTATE_CCW "##distrst")) { distPath.clear(); SaveProject(); }
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset to the default (<project>/dist)");
			}
			const char* methods[] = { "Store (no compression)", "Zlib", "Zstd", "GDeflate (DirectStorage, GPU decompression)" };
			if (ImGui::Combo(LProp("Project pak compression").c_str(), &pakMethod, methods, IM_ARRAYSIZE(methods))) SaveProject();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("GDeflate: the D3D12 runtime reads the pak through DirectStorage and inflates on the GPU,\n"
				                  "textures land straight in VRAM. Other backends/platforms inflate it on the CPU.\n"
				                  "Zstd packs tighter; DirectStorage then only bypasses the file stack (CPU inflate).");
			if (pakMethod != 0)
			{
				int maxLv = nuke::Package::MaxLevel(pakMethod);
				if (pakLevel > maxLv) pakLevel = maxLv;
				if (ImGui::SliderInt(LProp("Project pak level").c_str(), &pakLevel, 1, maxLv)) SaveProject();
			}
			if (ImGui::DragInt(LProp("Pak block size (MB)").c_str(), &pakBlockMB, 0.25f, 1, 256)) { if (pakBlockMB < 1) pakBlockMB = 1; SaveProject(); }
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Files pack in independent blocks of this size (each one is a DirectStorage request;\n"
				                  "texture mips split to fit). Must not exceed the runtime staging buffer (config io.stagingMB, 32 default).");
			if (ImGui::Combo(LProp("Mod pak compression").c_str(), &modMethod, methods, IM_ARRAYSIZE(methods))) SaveProject();
			if (modMethod != 0)
			{
				int maxLv = nuke::Package::MaxLevel(modMethod);
				if (modLevel < 1) modLevel = 1;
				if (modLevel > maxLv) modLevel = maxLv;
				if (ImGui::SliderInt(LProp("Mod pak level").c_str(), &modLevel, 1, maxLv)) SaveProject();
			}
			ImGui::SameLine(); ImGui::TextDisabled("(?)");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Project pak: zstd max = smallest release, fast to load.\nMods default to Store so they stay editable/diffable.");

			// Split: heavy content moves into side "part" paks that mount with their main one.
			// Applies to the game pak, DLC paks and mods alike.
			if (ImGui::Combo(LProp("Pak split").c_str(), &modSplitMode,
			                 "None (single file)\0By content type (textures/audio/meshes)\0Size cap per file\0"))
				SaveProject();
			if (modSplitMode == 2)
			{
				if (ImGui::InputInt(LProp("Split cap (MB)").c_str(), &modSplitCapMB, 64, 256))
				{
					if (modSplitCapMB < 16) modSplitCapMB = 16;
					SaveProject();
				}
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Applies to the game pak, DLC and mods. Worlds, their merge basis, manifests\n"
				                  "and script assemblies always stay in the MAIN pak.");
		}

		// Module-declared project settings (NUKEModule::projectSettings): one section per module,
		// widgets from the data descriptors, values in the .nuproj "moduleSettings" object.
		for (auto& mod : nuke::GetModules())
		{
			if (!mod || !mod->loaded || nuke::ModuleAbi(mod.get()) < 5) continue;
			std::vector<NukeModuleSetting> decl;
			mod->projectSettings(decl);
			if (decl.empty()) continue;
			std::string kw;
			for (const NukeModuleSetting& s : decl) { kw += s.label; kw += ' '; }
			if (!shellProj.Section("Modules", mod->title, kw.c_str())) continue;
			for (const NukeModuleSetting& s : decl)
			{
				const std::string id = LProp(s.label.c_str());
				bool changed = false;
				const bool has = modSettings.contains(s.key);
				if (s.type == 0)
				{
					bool v = has && modSettings[s.key].is_boolean() ? modSettings[s.key].get<bool>() : s.defVal != 0.0;
					if (ImGui::Checkbox(id.c_str(), &v)) { modSettings[s.key] = v; changed = true; }
				}
				else if (s.type == 1)
				{
					int v = has && modSettings[s.key].is_number() ? (int)modSettings[s.key].get<double>() : (int)s.defVal;
					if (ImGui::DragInt(id.c_str(), &v, 0.05f, (int)s.minV, (int)s.maxV))
					{
						if (s.minV != s.maxV) v = std::max((int)s.minV, std::min((int)s.maxV, v));
						modSettings[s.key] = v; changed = true;
					}
				}
				else if (s.type == 2)
				{
					float v = has && modSettings[s.key].is_number() ? (float)modSettings[s.key].get<double>() : (float)s.defVal;
					if (ImGui::DragFloat(id.c_str(), &v, 0.01f, (float)s.minV, (float)s.maxV))
					{
						if (s.minV != s.maxV) v = std::max((float)s.minV, std::min((float)s.maxV, v));
						modSettings[s.key] = v; changed = true;
					}
				}
				else
				{
					std::string v = has && modSettings[s.key].is_string() ? modSettings[s.key].get<std::string>() : s.defStr;
					char buf[256];
					strncpy(buf, v.c_str(), sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
					if (ImGui::InputText(id.c_str(), buf, sizeof(buf))) { modSettings[s.key] = std::string(buf); changed = true; }
				}
				if (!s.tip.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.tip.c_str());
				if (changed) SaveProject();
			}
		}

		// Mods list, backed by config/mods.json: checkbox = enabled, row order = load order.
		// Mounts happen at boot, so changes apply on session reload.
		if (basePakPath.size() > 6 && basePakPath.compare(basePakPath.size() - 6, 6, ".nupak") == 0
		    && shellProj.Section("Mods", "Mods", "load order requires modules mount enabled"))
		{
			// No rescan while an item is active: a drag-reorder must not have its rows rebuilt.
			if (!ImGui::IsAnyItemActive() && (modsUiTick < 0 || ImGui::GetFrameCount() - modsUiTick > 120))
			{ ScanModsUi(); modsUiTick = ImGui::GetFrameCount(); }

			// Which mods will actually load: the loader's fixpoint over the enabled set
			// (a mod is loadable when every requirement is an enabled loadable mod).
			auto lowerS = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
			{
				std::set<std::string> placed;
				for (bool progress = true; progress; )
				{
					progress = false;
					for (ModRow& r : modsUi)
					{
						if (!r.enabled || !r.found || placed.count(lowerS(r.name))) continue;
						bool ok = true;
						for (const std::string& q : r.reqs) ok &= placed.count(lowerS(q)) != 0;
						if (ok) { placed.insert(lowerS(r.name)); progress = true; }
					}
				}
				for (ModRow& r : modsUi)
				{
					r.reqOk = true;
					for (const std::string& q : r.reqs) r.reqOk &= placed.count(lowerS(q)) != 0;
				}
			}

			bool changed = false;                // checkbox / auto-sort: save right away
			static bool modsDragDirty = false;   // drag reorder: save when the mouse releases
			if (ImGui::BeginTable("modsui", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
			{
				ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthFixed, 38);     // the player's list
				ImGui::TableSetupColumn("Editor", ImGuiTableColumnFlags_WidthFixed, 44);   // this session's mounts
				ImGui::TableSetupColumn("Mod");
				ImGui::TableSetupColumn("Requires");
				ImGui::TableSetupColumn("Modules");   // engine plugins the mod's content needs
				ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 120);
				ImGui::TableHeadersRow();
				for (int i = 0; i < (int)modsUi.size(); ++i)
				{
					ModRow& r = modsUi[i];
					ImGui::TableNextRow();
					// A missing dependency — mod or module — paints the row red; an installed but
					// switched-off module is a warning, not a block.
					if (!r.reqOk || !r.found || !r.modsInstalled)
						ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(150, 40, 40, 90));
					ImGui::PushID(r.file.c_str());   // id follows the mod through reorders
					ImGui::TableNextColumn();
					const bool blockOn = !r.enabled && (!r.reqOk || !r.found || !r.modsInstalled);
					ImGui::BeginDisabled(blockOn);
					if (ImGui::Checkbox("##on", &r.enabled)) changed = true;
					ImGui::EndDisabled();
					if (blockOn && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					{
						if (!r.found)              ImGui::SetTooltip("File not found.");
						else if (!r.modsInstalled) ImGui::SetTooltip("Needs engine module(s) that are not installed: %s", r.modReq.c_str());
						else                       ImGui::SetTooltip("Enable its requirements first: %s", r.req.c_str());
					}
					ImGui::TableNextColumn();
					ImGui::BeginDisabled(!r.found);
					if (ImGui::Checkbox("##ed", &r.edMounted)) SaveEditorMods();
					ImGui::EndDisabled();
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
						ImGui::SetTooltip("Mount in THIS editor session (the game's own list is the 'Game' column)");
					ImGui::TableNextColumn();
					// The name cell doubles as a drag handle; only enabled rows have a position.
					ImGui::Selectable((ICON_LC_GRIP_HORIZONTAL "  " + r.name).c_str(), false,
					                  ImGuiSelectableFlags_AllowOverlap);
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s", r.file.c_str(), r.enabled ? "\n(drag to reorder)" : "");
					if (r.enabled && ImGui::IsItemActive() && !ImGui::IsItemHovered())
					{
						// One swap per row of mouse travel: swapping when the cursor leaves the
						// item also fires in the inter-row padding and skips a position.
						const float rowH = ImGui::GetItemRectSize().y + ImGui::GetStyle().CellPadding.y * 2.0f;
						const float dy = ImGui::GetMouseDragDelta(0).y;
						if (dy <= -rowH * 0.6f || dy >= rowH * 0.6f)
						{
							int next = i + (dy < 0.0f ? -1 : 1);
							if (next >= 0 && next < (int)modsUi.size() && modsUi[next].enabled)
							{
								std::swap(modsUi[i], modsUi[next]);
								modsDragDirty = true;
								ImGui::ResetMouseDragDelta();
							}
						}
					}
					ImGui::TableNextColumn();
					if (r.req.empty()) ImGui::TextDisabled("-");
					else if (r.reqOk)  ImGui::TextUnformatted(r.req.c_str());
					else               ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "%s", r.req.c_str());
					ImGui::TableNextColumn();
					// Engine plugins: red = not installed (the mod cannot load), amber = installed
					// but switched off (it loads, its components stay inert).
					if (r.modReq.empty())        ImGui::TextDisabled("-");
					else if (!r.modsInstalled)   ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "%s", r.modReq.c_str());
					else if (!r.modsEnabled)     ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1), "%s", r.modReq.c_str());
					else                         ImGui::TextUnformatted(r.modReq.c_str());
					if (!r.modReq.empty() && ImGui::IsItemHovered())
						ImGui::SetTooltip(!r.modsInstalled ? "Not installed — the mod is skipped on load."
						                 : !r.modsEnabled  ? "Installed but disabled — its components would load inert."
						                                   : "Installed and enabled.");
					ImGui::TableNextColumn();
					if      (!r.found)             ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1), "file not found");
					else if (!r.modsInstalled)     ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1), "missing module");
					else if (r.enabled && !r.reqOk) ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1), "missing deps");
					else if (r.mounted)            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "mounted (editor)");
					else if (r.enabled)            ImGui::TextColored(ImVec4(0.6f, 0.75f, 1.0f, 1), "on (game)");
					else                           ImGui::TextDisabled("off");
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			if (modsUi.empty()) ImGui::TextDisabled("No mods in the game's mods/ folder.");
			// A drag writes the config once, on release: a mid-drag save would rebuild the rows.
			if (modsDragDirty && !ImGui::IsMouseDown(0)) { SaveModsUi(); modsDragDirty = false; }
			if (changed) SaveModsUi();
			ImGui::TextDisabled("'Game' = the PLAYER's list (config/mods.json). 'Editor' = mounted in THIS session only\n(applies immediately; reopen the world to see the merge). A mod always loads AFTER its requirements.");
			// Rewrite the config in dependency order (stable); unsatisfiable mods sink to the end.
			if (ImGui::Button("Auto-sort (dependencies)"))
			{
				std::vector<ModRow> sorted;
				std::set<std::string> placed;
				std::vector<bool> done(modsUi.size(), false);
				for (bool progress = true; progress; )
				{
					progress = false;
					for (size_t i = 0; i < modsUi.size(); ++i)
					{
						if (done[i] || !modsUi[i].enabled || !modsUi[i].found) continue;
						bool ok = true;
						for (const std::string& q : modsUi[i].reqs) ok &= placed.count(lowerS(q)) != 0;
						if (!ok) continue;
						placed.insert(lowerS(modsUi[i].name));
						sorted.push_back(modsUi[i]);
						done[i] = true;
						progress = true;
					}
				}
				for (size_t i = 0; i < modsUi.size(); ++i) if (!done[i] && modsUi[i].enabled) sorted.push_back(modsUi[i]);   // unsatisfiable last
				for (size_t i = 0; i < modsUi.size(); ++i) if (!modsUi[i].enabled) sorted.push_back(modsUi[i]);
				modsUi = std::move(sorted);
				SaveModsUi();
			}
			ImGui::SameLine();
			if (ImGui::Button("Apply (reload session)")) RequestProjectSwitch(basePakPath);
			ImGui::SameLine(); ImGui::TextDisabled("(?)");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Mods mount when the session opens.\nChanges are saved to config/mods.json immediately —\nthe running game picks them up on its next start too.");
		}

		if (shellProj.Section("Disk sync", "Disk sync", "disk changed reload conflict merge overwrite"))
		{
		const char* cleanModes[] = { "Ask", "Auto-reload" };
		if (ImGui::Combo(LProp("Disk changed (editor clean)", 15.0f).c_str(), &reloadCleanMode, cleanModes, IM_ARRAYSIZE(cleanModes))) SaveProject();
		{ ProjectSettings a = capturePS(); a.reloadClean = PSD.reloadClean; resetBtn("rst_rc", a, "Reset disk (clean)"); }
		const char* conflModes[] = { "Ask", "Reload (use disk)", "Overwrite (use editor)", "Merge / resolve" };
		if (ImGui::Combo(LProp("Disk changed (editor dirty)", 15.0f).c_str(), &conflictMode, conflModes, IM_ARRAYSIZE(conflModes))) SaveProject();
		{ ProjectSettings a = capturePS(); a.conflict = PSD.conflict; resetBtn("rst_cf", a, "Reset disk (dirty)"); }
		}

		// Undo for the value edits above: snapshot while idle, push one command when an edit
		// settles. Default World and hotkeys keep their own undo and are excluded from samePS.
		bool psActive = ImGui::IsAnyItemActive();
		if (psActive) psEditing = true;
		else if (psEditing)
		{
			psEditing = false;
			ProjectSettings after = capturePS();
			if (!samePS(after, psBefore))
			{
				ProjectSettings before = psBefore;
				PushUndo("Project settings",
					[this, before]{ ApplyProjectSettings(before); },
					[this, after ]{ ApplyProjectSettings(after ); },
					false);   // project settings, not a change of the open world
			}
		}
		if (!psActive) psBefore = capturePS();

		// 32 named render channels (Atom.layer + Camera.layerMask filter on them).
		if (shellProj.Section("Layers", "Layers", "render channels layer mask atom camera"))
		{
		ImGui::Text("Named render channels. Atoms pick a Layer; cameras pick what they draw via Layer Mask.");
		if (ImGui::BeginTable("layers", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit, ImVec2(320, 0)))
		{
			ImGui::TableSetupColumn("##idx",  ImGuiTableColumnFlags_WidthFixed, 30.0f);
			ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
			for (int i = 0; i < 32; ++i)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%d", i);
				ImGui::TableNextColumn();
				char lbuf[64]; std::string nm = nuke::Layers::Name(i);
				strncpy(lbuf, nm.c_str(), sizeof(lbuf) - 1); lbuf[sizeof(lbuf) - 1] = 0;
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::PushID(2000 + i);
				if (ImGui::InputText("##ln", lbuf, sizeof(lbuf)))
				{
					nuke::Layers::SetName(i, lbuf);
					SaveProject();   // slot names live in game.nuproj "layers"
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}

		}
		if (shellProj.Section("Input", "Input Maps", "input nuinput bindings maps provenance conflicts"))
		{
		// which .nuinput files feed the merged action table, and who defined what.
		bool autoMode = inputMapsAuto;
		if (ImGui::RadioButton("Auto (load every .nuinput in content)", autoMode))
		{ inputMapsAuto = true; SaveProject(); ApplyInputMaps(); }
		if (ImGui::RadioButton("Explicit list", !autoMode))
		{ inputMapsAuto = false; SaveProject(); ApplyInputMaps(); }

		// Discovered maps (content-relative), with enable checkboxes in explicit mode.
		std::vector<std::string> found;
		{
			boost::system::error_code ec;
			bfs::path root(contentDir);
			if (bfs::exists(root, ec))
				for (bfs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec))
					if (!ec && it->path().extension() == ".nuinput")
					{
						std::string rel = bfs::relative(it->path(), root, ec).generic_string();
						if (!ec) found.push_back(rel);
					}
		}
		if (found.empty()) ImGui::TextDisabled("No .nuinput files in content.");
		for (const std::string& rel : found)
		{
			bool on = inputMapsAuto ||
			          std::find(inputMapsList.begin(), inputMapsList.end(), rel) != inputMapsList.end();
			ImGui::BeginDisabled(inputMapsAuto);
			ImGui::PushID(rel.c_str());
			if (ImGui::Checkbox(rel.c_str(), &on))
			{
				if (on) inputMapsList.push_back(rel);
				else inputMapsList.erase(std::remove(inputMapsList.begin(), inputMapsList.end(), rel),
				                         inputMapsList.end());
				SaveProject(); ApplyInputMaps();
			}
			ImGui::PopID();
			ImGui::EndDisabled();
		}

		// PROVENANCE: the LIVE merged map — which file defined each context/action/binding, and
		// which layer won (asset < user). A CONFLICT is only a FULL collision: two DIFFERENT
		// actions on the same control in the same context, same phase AND same modifiers (and
		// even that can be intended — the flag says merge/debounce, not error). One action
		// across several controls/devices (WASD + a gamepad stick, all "Move") is COMPOSITION,
		// never a conflict; several files feeding one action is the merge working as designed.
		ImGui::SeparatorText("Live map provenance");
		for (const nuke::InputContext& c : nuke::Input::ListContexts())
		{
			std::string clabel = c.name + "  [" + (c.source.empty() ? "code" : c.source) + "]";
			if (!ImGui::TreeNode(clabel.c_str())) continue;
			// (control | phase | modifiers) -> actions: >1 DIFFERENT actions = a full collision.
			std::map<std::string, std::set<std::string>> byControl;
			auto slotKey = [](const nuke::InputBinding& b, const std::string& ctl)
			{
				std::string key = ctl + "|" + std::to_string((int)b.phase) + "|";
				std::vector<std::string> mods = b.modifiers;
				std::sort(mods.begin(), mods.end());
				for (const std::string& m : mods) { key += m; key += ','; }
				return key;
			};
			for (const nuke::InputBinding& b : c.bindings)
				for (const std::string& ctl : b.controls) byControl[slotKey(b, ctl)].insert(b.action);
			std::map<std::string, std::set<std::string>> actionSources;
			for (const nuke::InputBinding& b : c.bindings)
				actionSources[b.action].insert(b.source.empty() ? "code" : b.source);
			if (ImGui::BeginTable("prov", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
			{
				ImGui::TableSetupColumn("Action"); ImGui::TableSetupColumn("Controls"); ImGui::TableSetupColumn("Source");
				ImGui::TableHeadersRow();
				for (const nuke::InputBinding& b : c.bindings)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(b.action.c_str());
					if (actionSources[b.action].size() > 1 && actionSources[b.action].count("user") == 0)
					{
						// Informational, NOT a warning: multi-file composition is the design.
						ImGui::SameLine();
						ImGui::TextDisabled(ICON_LC_LAYERS);
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("Composed from several files — they merge by design");
					}
					ImGui::TableNextColumn();
					std::string ctls;
					for (const std::string& ctl : b.controls) { if (!ctls.empty()) ctls += " + "; ctls += ctl; }
					ImGui::TextUnformatted(ctls.c_str());
					bool conflict = false;
					for (const std::string& ctl : b.controls) if (byControl[slotKey(b, ctl)].size() > 1) conflict = true;
					if (conflict)
					{
						ImGui::SameLine();
						ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.3f, 1.0f), ICON_LC_TRIANGLE_ALERT);
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("FULL collision: this control, phase and modifiers also drive a DIFFERENT action\n"
						                                              "in this context. If intended, fine — otherwise rebind, or split by phase/modifier.");
					}
					ImGui::TableNextColumn();
					const bool user = b.source == "user";
					ImGui::TextColored(user ? ImVec4(0.5f, 0.9f, 0.5f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
					                   "%s%s", b.source.empty() ? "code" : b.source.c_str(), user ? " (wins)" : "");
				}
				ImGui::EndTable();
			}
			ImGui::TreePop();
		}
		}
		shellProj.End();
		// (".nuproj file association" moved to Preferences -> System: it is machine-wide.)
	});
}

// World Settings window: world-level render/physics/wind settings, saved in the .nuworld.
// Edits push to the renderer live and mark the world dirty.
void EditorUI::winWorldSettings()
{
	if (!worldSettingsOpen) return;
	if (worldSettingsFocus) { NukeUI::DocFocus("panel:worldsettings"); worldSettingsFocus = false; }   // menu-opened only
	// NoFocusOnAppearing: restored-open on load must not steal the dock's active tab.
	NukeUI::DocPanel("panel:worldsettings", "World Settings", &worldSettingsOpen, ImGuiWindowFlags_NoFocusOnAppearing, 500, 560, [this]()
	{
		World* w = AppInstance::GetSingleton()->currentWorld;
		if (!w) { ImGui::TextDisabled("No world loaded."); return; }
		World::Settings& s = w->settings;
		auto apply = [this](const World::Settings& st) {
			World* ww = AppInstance::GetSingleton()->currentWorld; if (!ww) return;
			ww->settings = st;
			if (iRender* r = AppInstance::GetSingleton()->render)
				r->setShadowSettings(st.shadowRes, st.shadowDistance, st.shadowDepthBias, st.shadowNormalBias, st.shadowSoftness);
			worldDirty = true; UpdateWindowTitle();
		};
		auto same = [](const World::Settings& a, const World::Settings& b) {
			return a.shadowRes == b.shadowRes && a.shadowDistance == b.shadowDistance && a.shadowDepthBias == b.shadowDepthBias
			    && a.shadowNormalBias == b.shadowNormalBias && a.shadowSoftness == b.shadowSoftness && a.frustumCull == b.frustumCull
			    && a.occlusionCull == b.occlusionCull
			    && a.gravity[0] == b.gravity[0] && a.gravity[1] == b.gravity[1] && a.gravity[2] == b.gravity[2]
			    && a.fixedDt == b.fixedDt;
		};

		// Inspector-style: a flat label-first prop list — four small sections never needed
		// the category-sidebar chrome the bigger settings windows use.
		bool changed = false;
		ImGui::SeparatorText("Shadows (global)");
		{
		ImGui::TextDisabled("Which lights cast is per-Light; these tune all shadow maps.");
		const char* resLabels[] = { "1024", "2048", "4096" };
		const int   resVals[]   = { 1024, 2048, 4096 };
		int ri = (s.shadowRes >= 4096) ? 2 : (s.shadowRes >= 2048 ? 1 : 0);
		if (ImGui::Combo(LProp("Resolution").c_str(), &ri, resLabels, IM_ARRAYSIZE(resLabels))) { s.shadowRes = resVals[ri]; changed = true; }
		changed |= ImGui::SliderFloat(LProp("Distance (directional)").c_str(), &s.shadowDistance, 5.0f, 300.0f, "%.0f");
		changed |= ImGui::SliderFloat(LProp("Depth Bias").c_str(), &s.shadowDepthBias, 0.0f, 0.01f, "%.4f");
		changed |= ImGui::SliderFloat(LProp("Normal Bias").c_str(), &s.shadowNormalBias, 0.0f, 0.5f, "%.3f");
		changed |= ImGui::SliderFloat(LProp("Softness (PCF)").c_str(), &s.shadowSoftness, 0.0f, 4.0f, "%.2f");
		}
		ImGui::SeparatorText("Culling");
		{
		changed |= ImGui::Checkbox(LProp("Frustum Culling").c_str(), &s.frustumCull);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Skip drawing objects outside the camera frustum (perf).\nTurn off if off-screen geometry must still render (e.g. reflections).");
		changed |= ImGui::Checkbox(LProp("Occlusion Culling (Hi-Z)").c_str(), &s.occlusionCull);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("GPU depth-pyramid occlusion over meshes and instanced chunks:\nobjects fully hidden behind others are not drawn. Toolbar: Freeze Culling shows what it removes.");
		}
		ImGui::SeparatorText("Physics");
		{
		changed |= ImGui::DragFloat3(LProp("Gravity").c_str(), s.gravity, 0.05f);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("World gravity (m/s^2), pushed to the physics service each step.");
		changed |= ImGui::DragFloat(LProp("Fixed Timestep").c_str(), &s.fixedDt, 0.0005f, 0.001f, 0.1f, "%.4f s");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fixed simulation step (seconds). 1/60 by default;\nsmaller = more precise, more CPU.");
		}
		ImGui::SeparatorText("Streaming (World Partition)");
		{
		changed |= ImGui::Checkbox(LProp("Enabled").c_str(), &s.streamEnabled);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save splits spatial atoms into per-cell files; play streams cells\nby camera distance with baked HLOD proxies for far cells.\nThe editor always loads the whole world. Takes effect on Save.");
		if (s.streamEnabled)
		{
			changed |= ImGui::DragFloat(LProp("Cell Size").c_str(), &s.streamCellSize, 1.0f, 16.0f, 4096.0f, "%.0f");
			changed |= ImGui::DragFloat(LProp("Load Range").c_str(), &s.streamRange, 1.0f, 32.0f, 16384.0f, "%.0f");
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cells within this distance of a camera load; unload at x1.3.");
			changed |= ImGui::DragFloat(LProp("HLOD Range").c_str(), &s.streamHlodRange, 8.0f, 0.0f, 100000.0f, "%.0f");
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Unloaded cells inside this distance draw their baked proxy mesh\n(0 = unlimited). Proxies bake automatically on Save.");
			ImGui::TextDisabled("Mark managers/global atoms 'Always Loaded' in the inspector.");
		}
		}

		if (changed) apply(s);   // live apply + mark dirty

		// The world's global wind field; local volumes are WindZone components on atoms.
		ImGui::SeparatorText("Wind");
		{
			bool wch = false;
			nuke::Vector3 d = nuke::Wind::Direction();
			float yawDeg = atan2f((float)d.x, (float)d.z) * 57.29578f;
			float pitchDeg = asinf(std::max(-1.f, std::min(1.f, (float)-d.y))) * 57.29578f;
			if (ImGui::SliderFloat(LProp("Direction (yaw)").c_str(), &yawDeg, -180.0f, 180.0f, "%.0f deg")) wch = true;
			if (ImGui::SliderFloat(LProp("Direction (pitch)").c_str(), &pitchDeg, -89.0f, 89.0f, "%.0f deg")) wch = true;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Downward slope of the wind (0 = horizontal).");
			if (wch)
			{
				const float cy = cosf(pitchDeg / 57.29578f);
				nuke::Wind::SetDirection(nuke::Vector3(sinf(yawDeg / 57.29578f) * cy, -sinf(pitchDeg / 57.29578f), cosf(yawDeg / 57.29578f) * cy));
			}
			float str = (float)nuke::Wind::Strength();
			if (ImGui::SliderFloat(LProp("Strength").c_str(), &str, 0.0f, 40.0f, "%.1f m/s")) { nuke::Wind::SetStrength(str); wch = true; }
			float ga = (float)nuke::Wind::GustAmount(), gf = (float)nuke::Wind::GustFrequency();
			bool g = ImGui::SliderFloat(LProp("Gust Amount").c_str(), &ga, 0.0f, 1.0f, "%.2f");
			g     |= ImGui::SliderFloat(LProp("Gust Frequency").c_str(), &gf, 0.0f, 4.0f, "%.2f");
			if (g) { nuke::Wind::SetGusts(ga, gf); wch = true; }
			float ta = (float)nuke::Wind::TurbulenceAmount(), ts = (float)nuke::Wind::TurbulenceScale();
			bool t = ImGui::SliderFloat(LProp("Turbulence").c_str(), &ta, 0.0f, 1.0f, "%.2f");
			t     |= ImGui::SliderFloat(LProp("Turbulence Scale").c_str(), &ts, 1.0f, 200.0f, "%.0f m");
			if (t) { nuke::Wind::SetTurbulence(ta, ts); wch = true; }
			ImGui::TextDisabled("Local volumes: add a WindZone component to an atom.");
			if (wch) { worldDirty = true; UpdateWindowTitle(); }   // wind saves with the world
		}

		// Global surface conditions (LiveMaterial): named 0..1 values live materials respond to.
		ImGui::SeparatorText("Surface Conditions");
		{
			bool sch = false;
			std::string killState;
			for (const auto& kv : nuke::Surface::All())
			{
				ImGui::PushID(kv.first.c_str());
				float v = kv.second;
				ImGui::SetNextItemWidth(std::max(80.0f, ImGui::CalcItemWidth()
					- ImGui::CalcTextSize(ICON_LC_TRASH_2).x - ImGui::GetStyle().FramePadding.x * 2.0f
					- ImGui::GetStyle().ItemSpacing.x));
				if (ImGui::DragFloat(LProp(kv.first.c_str()).c_str(), &v, 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp))
				{
					// 0 must not drop the row mid-edit: keep a tiny epsilon until deleted explicitly.
					nuke::Surface::SetCondition(kv.first, v > 0.0f ? v : 0.001f);
					sch = true;
				}
				ImGui::SameLine();
				if (ImGui::SmallButton(ICON_LC_TRASH_2)) { killState = kv.first; sch = true; }
				ImGui::PopID();
			}
			if (!killState.empty()) nuke::Surface::SetCondition(killState, 0.0);
			static char newState[64] = "";
			ImGui::SetNextItemWidth(160);
			ImGui::InputTextWithHint("##newcond", "wet / snow / dust / rust ...", newState, sizeof(newState));
			ImGui::SameLine();
			if (ImGui::Button(ICON_LC_PLUS " Add Condition") && newState[0])
			{
				nuke::Surface::SetCondition(newState, 0.5);
				newState[0] = 0;
				sch = true;
			}
			ImGui::TextDisabled("Per-atom: SurfaceState component. Painted patches: SurfaceMask component.");
			if (sch) { worldDirty = true; UpdateWindowTitle(); }   // conditions save with the world
		}

		// Undo: snapshot while idle, push one command when an edit settles.
		bool active = ImGui::IsAnyItemActive();
		if (active) wsEditing = true;
		else if (wsEditing)
		{
			wsEditing = false;
			if (!same(s, wsBefore))
			{
				World::Settings before = wsBefore, after = s;
				PushUndo("World settings",
					[this, apply, before]{ apply(before); },
					[this, apply, after ]{ apply(after ); });
			}
		}
		if (!active) wsBefore = s;   // refresh the idle baseline
	});
}
