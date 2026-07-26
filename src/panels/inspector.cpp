// inspector panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>
#include "nukeui.h"   // DocWindow: detachable panels (task #137)
#include <API/Model/Camera.h>   // restrict the PostProcess component to camera atoms
#include <API/Model/CharacterController.h>   // Fit To Mesh inspector button
#include <API/Model/Foliage.h>               // Foliage Fill/Clear + paint brush controls (7.4)
#include <API/Model/StatusBar.h>
#include <API/Model/Package.h>  // packed session: pickers list pak/mod content too (3.2)
#include <interface/Services.h> // csclass picker: enumerate scripting providers
#include <service/iScript.h>    // csclass picker: the C# backend lists its Electron classes
#include <set>
#include <memory>                // chroma-key undo snapshots (shared_ptr pixel blobs)
#include <cmath>                 // curve widget: tangent handle geometry (sqrtf/fabsf)
#include <API/Model/Texture.h>  // asset inspector: .nutex usage/info
#include <API/Model/Material.h> // asset inspector: .numat fields
#include <API/Model/Light.h>       // asset preview world: sun
#include <API/Model/Environment.h> // asset preview world: sky/ambient
#include <API/Model/Layers.h>      // render layers: atom Layer combo + camera Layer Mask
#include <API/Model/Canvas.h>      // Anchors block: canvas-child detection + units
#include <API/Model/RectAnchor.h>  // Anchors block: per-side anchors storage
#include <API/Model/Sprite.h>      // Anchors block: element size for distance capture
#include <functional>              // AtomRef picker: recursive hierarchy walk
#include <interface/AssetCreators.h>   // module-supplied asset editors (AssetEditorForExt)
#include <boost/filesystem.hpp>
namespace bfs = boost::filesystem;

// ---------------------------------------------------------------------------
// Inspector's asset 3D preview — one POOLED preview scene (see asseteditor.cpp for
// the pool: sky + shadowless sun + one mesh atom + camera into an own RT), staged
// with whatever the browser has selected.
// ---------------------------------------------------------------------------

void EditorUI::StageAssetPreview(const std::string& path, const std::string& ext)
{
	if (!inspPv) inspPv = AcquirePreview();
	if (!inspPv) return;
	ResDB* db = ResDB::getSingleton();
	const std::string guid = db->GuidForPath(path);

	if (ext == ".numesh")
	{
		inspPv->mr->meshGuid = guid;
		inspPv->mr->mesh = db->GetMesh(guid);
		inspPv->mr->matGuid = "builtin:default";
		if (inspPv->mr->mat) { delete inspPv->mr->mat; inspPv->mr->mat = nullptr; }   // re-resolve
	}
	else if (ext == ".numat")
	{
		inspPv->mr->meshGuid = "builtin:sphere";
		inspPv->mr->mesh = db->GetMesh("builtin:sphere");
		inspPv->mr->matGuid = guid;
		if (inspPv->mr->mat) { delete inspPv->mr->mat; inspPv->mr->mat = nullptr; }
	}
	FramePreview(*inspPv, nullptr);
	pvStaged = path;
}

void EditorUI::DrawAssetPreview3D(const std::string& path, const std::string& ext)
{
	if (pvStaged != path) StageAssetPreview(path, ext);
	if (!inspPv || !inspPv->mr || !inspPv->mr->mesh) return;
	float side = ImGui::GetContentRegionAvail().x;
	if (side > 384.0f) side = 384.0f;
	DrawPreviewImage(*inspPv, ImVec2(side, side));   // inline thumbnail: square
}

// Render hook (main.cpp), BEFORE the live scene: draw EVERY preview scene that was shown
// this frame (inspector + open asset editors); the live scene then re-pushes its own
// lights/sky/TLAS, so nothing leaks into the viewport image.
void EditorUI::RenderAssetPreview(iRender* r)
{
	if (!r) return;
	for (PreviewWorld* s : pvPool)
	{
		if (s->inUse && s->visible && s->world)
			s->world->Render(r);
		s->visible = false;
	}
}

// Layer-mask multi-select: an int bitmask over nuke::Layers drawn as a named dropdown
// (Everything / Nothing / per-layer checkboxes). Used by every [[nuke::prop(widget="layers")]]
// field via DrawFields — components never get a raw numeric box for a mask.
static bool DrawLayerMaskCombo(const char* id, int& mask)
{
	bool changed = false;
	unsigned int m = (unsigned int)mask;
	std::string shown = (m == 0xFFFFFFFFu) ? "Everything" : (m == 0 ? "Nothing" : "Mixed");
	if (m != 0xFFFFFFFFu && m != 0)   // count the named picks for a nicer summary
	{
		int bits = 0; std::string one;
		for (int i = 0; i < 32; ++i)
			if ((m >> i) & 1u) { ++bits; if (bits == 1) { one = nuke::Layers::Name(i); if (one.empty()) one = "Layer " + std::to_string(i); } }
		if (bits == 1) shown = one;
	}
	if (ImGui::BeginCombo(id, shown.c_str()))
	{
		if (ImGui::Selectable("Everything")) { mask = -1; m = 0xFFFFFFFFu; changed = true; }
		if (ImGui::Selectable("Nothing"))    { mask = 0;  m = 0; changed = true; }
		ImGui::Separator();
		for (int i = 0; i < 32; ++i)
		{
			std::string nm = nuke::Layers::Name(i);
			if (nm.empty()) continue;   // unnamed slots hidden (still reachable from scripts)
			bool on = (m >> i) & 1u;
			if (ImGui::Checkbox((nm + "##lm" + std::to_string(i)).c_str(), &on))
			{
				if (on) m |= (1u << i); else m &= ~(1u << i);
				mask = (int)m; changed = true;
			}
		}
		ImGui::EndCombo();
	}
	return changed;
}

void EditorUI::CamComponent(Camera* cam)
{
	if (cam->renderer)
	{
		ImGui::InputInt("Width", &cam->renderer->width);
		ImGui::InputInt("Height", &cam->renderer->height);
	}
	float fov = cam->fov * (float)M_PI / 180.f;
	ImGui::SliderAngle("FOV", &fov, 0, 180);
	cam->fov = fov * 180.f / (float)M_PI;
	ImGui::DragFloat("Near", &cam->_near);
	ImGui::DragFloat("Far", &cam->_far);
	ImGui::Checkbox("Free mode", &cam->freeMode);

	// Render-layer mask: which layers this camera draws (named multi-select over nuke::Layers).
	DrawLayerMaskCombo("Layer Mask", cam->layerMask);
}

// --- reusable asset picker -------------------------------------------------------------------
static bool ciContains(const std::string& hay, const std::string& needle)
{
	if (needle.empty()) return true;
	auto lower = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
	return lower(hay).find(lower(needle)) != std::string::npos;
}
static bool EndsWithCI(const std::string& s, const char* suf)
{
	size_t n = strlen(suf);
	if (s.size() < n) return false;
	for (size_t i = 0; i < n; ++i)
		if (tolower((unsigned char)s[s.size() - n + i]) != tolower((unsigned char)suf[i])) return false;
	return true;
}
// Audio clips are PLAIN files in content (no wrapper asset) — every format the audio
// service decodes. Referenced by content-relative path, exactly like scripts.
static bool IsAudioExt(std::string e)
{
	for (char& c : e) c = (char)tolower((unsigned char)c);
	return e == ".ogg" || e == ".wav" || e == ".mp3" || e == ".flac";
}
// "file:<ext>" kinds: a GENERIC by-extension content-file picker (value = content-relative
// path, like script/audio). Any module-owned file type gets a real picker by declaring
// `asset="file:.nutile"` on its reflected string prop — no per-type editor-core hardcode.
static bool IsFileKind(const std::string& kind) { return kind.rfind("file:", 0) == 0; }
static std::string FileKindExt(const std::string& kind)
{
	std::string e = kind.substr(5);
	for (char& c : e) c = (char)tolower((unsigned char)c);
	return e;
}
// Does a dropped file path match the field's asset kind? (rejects everything else)
static bool KindMatchesFile(const std::string& kind, const std::string& path)
{
	std::string e = bfs::path(path).extension().string();
	for (char& c : e) c = (char)tolower((unsigned char)c);
	if (IsFileKind(kind)) return e == FileKindExt(kind);
	if (kind == "mesh")     return e == ".numesh";
	if (kind == "material") return e == ".numat";
	if (kind == "texture")  return e == ".nutex";
	if (kind == "anim")     return e == ".nuanim";
	if (kind == "bonemap")  return e == ".nubonemap";
	if (kind == "script")   return e == ".lua";
	if (kind == "audio")    return IsAudioExt(e);
	if (kind == "shader")   { std::string fn = bfs::path(path).filename().string(); return EndsWithCI(fn, ".vs.hlsl") || EndsWithCI(fn, ".ps.hlsl"); }
	return false;
}
// Shader guid == base name (strip ".vs.hlsl" / ".ps.hlsl").
static std::string ShaderGuidFromPath(const std::string& path)
{
	std::string fn = bfs::path(path).filename().string();
	for (const char* suf : { ".vs.hlsl", ".ps.hlsl" })
		if (EndsWithCI(fn, suf)) return fn.substr(0, fn.size() - strlen(suf));
	return std::string();
}

bool EditorUI::AssetPicker(const char* label, std::string& guid, const std::string& kind, const std::string& defGuid)
{
	ResDB* db = ResDB::getSingleton();
	bool changed = false;

	// Display name = the asset's FILE name (so it tracks renames); fall back to the internal name for
	// built-ins that have no file. Shaders are keyed by name, so just show that.
	auto disp = [&](const std::string& g) -> std::string {
		if (g.empty()) return "(none)";
		if (kind == "script" || kind == "audio" || IsFileKind(kind)) return bfs::path(g).stem().string();   // value is a content-relative path
		if (kind == "shader" || kind == "postshader") { Shader* s = db->GetShader(g); return s ? s->name : g; }
		std::string p = db->PathForGuid(g);
		if (!p.empty()) return bfs::path(p).stem().string();
		// No file path (built-ins, pak-loaded assets): the internal name, never an empty
		// label (an empty Selectable renders an invisible row) — the guid as last resort.
		std::string n;
		if      (kind == "mesh")     { Mesh* m = db->GetMesh(g);         if (m) n = m->name; }
		else if (kind == "material") { Material* m = db->GetMaterial(g); if (m) n = m->matName; }
		else if (kind == "anim")     { AnimClip* c = db->GetClip(g);     if (c) n = c->name; }
		else if (kind == "bonemap")  { BoneMap* b = db->GetBoneMap(g);   if (b) n = b->name; }
		else if (kind == "texture")  { Texture* t = db->GetTexture(g);   if (t) n = t->name; }
		return n.empty() ? g : n;
	};

	ImGui::PushID(label);
	float full = ImGui::CalcItemWidth();
	std::string cur = disp(guid);
	if (ImGui::Button((cur + "##cur").c_str(), ImVec2(full - 52, 0))) { assetFilter[0] = 0; ImGui::OpenPopup("##assetpop"); }

	// Drag from the browser — only accepted if the file's type matches this field's kind.
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NUKE_ASSET"))
		{
			std::string path((const char*)p->Data);
			if (KindMatchesFile(kind, path))
			{
				std::string g;
				if      (kind == "script" || kind == "audio" || IsFileKind(kind)) { boost::system::error_code ec; g = bfs::relative(bfs::path(path), bfs::path(contentDir), ec).generic_string(); }
				else if (kind == "shader") g = ShaderGuidFromPath(path);
				else                       g = db->GuidForPath(path);
				if (!g.empty()) { guid = g; changed = true; }
			}
		}
		ImGui::EndDragDropTarget();
	}

	ImGui::SameLine(0, 2);
	if (ImGui::Button(ICON_LC_FOLDER_SEARCH "##loc"))   // locate the original file in the browser
	{
		std::string path;
		if      (kind == "script" || kind == "audio" || IsFileKind(kind)) { if (!guid.empty()) path = (bfs::path(contentDir) / guid).string(); }
		else if (kind == "shader" && db->GetShader(guid)) path = db->GetShader(guid)->vsPath;
		else                       path = db->PathForGuid(guid);
		if (!path.empty()) { BrowserNavigate(bfs::path(path).parent_path().string()); browserSel = path; if (win) win->browser = true; }
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Go to file");
	ImGui::SameLine(0, 2);
	if (ImGui::Button(ICON_LC_ROTATE_CCW "##rst")) { guid = defGuid; changed = true; }   // reset to default
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset to default");
	// Trailing label — skipped for hidden ids ("##..." would print literally; list rows pass those).
	if (label && label[0] && !(label[0] == '#' && label[1] == '#')) { ImGui::SameLine(0, 6); ImGui::TextUnformatted(label); }

	if (ImGui::BeginPopup("##assetpop"))
	{
		ImGui::SetNextItemWidth(260);
		ImGui::InputTextWithHint("##flt", ICON_LC_SEARCH " Filter", assetFilter, sizeof(assetFilter));
		std::string flt = assetFilter;
		ImGui::Separator();
		ImGui::BeginChild("##lst", ImVec2(280, 260));
		auto item = [&](const std::string& g, const std::string& n) {
			if (!ciContains(n, flt)) return;
			// "##<guid>" keeps the visible text but gives same-named assets distinct IDs (no ID clash).
			if (ImGui::Selectable((n + "##" + g).c_str(), g == guid)) { guid = g; changed = true; ImGui::CloseCurrentPopup(); }
		};
		if (ciContains("(none)", flt))
			if (ImGui::Selectable("(none)", guid.empty())) { guid.clear(); changed = true; ImGui::CloseCurrentPopup(); }
		if      (kind == "mesh")     for (Mesh* m : db->meshes)      { if (m) item(m->guid, disp(m->guid)); }
		else if (kind == "material") for (Material* m : db->materials) { if (m) item(m->guid, disp(m->guid)); }
		else if (kind == "shader")   for (Shader* s : db->shaders)   { if (s && !s->isPost) item(s->guid, disp(s->guid)); }
		else if (kind == "postshader") for (Shader* s : db->shaders) { if (s && s->isPost) item(s->guid, disp(s->guid)); }
		else if (kind == "texture")  for (Texture* t : db->textures) { if (t) item(t->guid, disp(t->guid)); }
		else if (kind == "anim")     for (AnimClip* c : db->clips)   { if (c) item(c->guid, disp(c->guid)); }
		else if (kind == "bonemap")  for (BoneMap* b : db->boneMaps) { if (b) item(b->guid, disp(b->guid)); }
		else if (kind == "csclass")
		{
			// C# script classes: the LOADED game assembly's Electron classes, straight from
			// the scripting seam (the C# backend enumerates them) — nobody types names.
			bool any = false;
			for (nuke::iScript* sv : nuke::GetServices<nuke::iScript>())
			{
				if (!sv || std::string(sv->Language()) != "cs") continue;
				int need = sv->ListClasses(nullptr, 0);
				if (need <= 0) break;
				std::string names(need, '\0');
				sv->ListClasses(&names[0], need);
				size_t start = 0;
				while (start < names.size())
				{
					size_t nl = names.find('\n', start);
					if (nl == std::string::npos) nl = names.size();
					std::string cls = names.substr(start, nl - start);
					if (!cls.empty()) { item(cls, cls); any = true; }
					start = nl + 1;
				}
				break;
			}
			if (!any) ImGui::TextDisabled("no C# classes loaded\n(add a .cs deriving from Electron;\nif the project HAS scripts, check the\nConsole for 'C# build FAILED')");
		}
		else if (kind == "script" || kind == "audio" || IsFileKind(kind))   // scan the project content (value = content-relative path)
		{
			auto matches = [&](std::string e) {
				for (char& c : e) c = (char)tolower((unsigned char)c);
				if (IsFileKind(kind)) return e == FileKindExt(kind);
				return kind == "script" ? (e == ".lua") : IsAudioExt(e);
			};
			std::set<std::string> seen;   // lowercase rel — the disk copy wins over pak layers
			boost::system::error_code ec;
			bfs::path croot(contentDir);
			if (bfs::exists(croot, ec))
				for (bfs::recursive_directory_iterator it(croot, ec), end; it != end; it.increment(ec))
				{
					if (ec) break;
					if (bfs::is_directory(it->path())) continue;
					if (!matches(it->path().extension().string())) continue;
					std::string rel = bfs::relative(it->path(), croot, ec).generic_string();
					std::string low = rel; for (char& c : low) c = (char)tolower((unsigned char)c);
					seen.insert(low);
					item(rel, bfs::path(rel).stem().string());
				}
			// Packed session (3.2): plain content files live in the mounted pak/mods, not on
			// disk — list them through the Package layers too (dedup vs the overlay scan).
			if (nuke::Package::MountedCount() > 0)
				for (const std::string& pr : nuke::Package::List("content/"))
				{
					if (!matches(bfs::path(pr).extension().string())) continue;
					std::string rel = pr.substr(strlen("content/"));
					std::string low = rel; for (char& c : low) c = (char)tolower((unsigned char)c);
					if (!seen.insert(low).second) continue;
					item(rel, bfs::path(rel).stem().string());
				}
		}
		ImGui::EndChild();
		ImGui::EndPopup();
	}
	ImGui::PopID();
	return changed;
}

// Register per-type custom inspector drawing. The generic loop draws reflected fields +
// dynamic props for every component; overrides add type-specific UI on top (keyed by the
// reflected type name). Plugins can extend this map for their own component types later.
void EditorUI::RegisterInspectorOverrides()
{
	inspectorOverrides["MeshRenderer"] = [this](nuke::Component* c) {
		DrawMeshRendererInspector(static_cast<nuke::MeshRenderer*>(c));
	};
	inspectorOverrides["PostProcess"] = [this](nuke::Component* c) {
		DrawPostProcessInspector(static_cast<nuke::PostProcess*>(c));
	};
	inspectorOverrides["Animator"] = [this](nuke::Component* c) {
		DrawAnimatorInspector(static_cast<nuke::Animator*>(c));
	};
	inspectorOverrides["CharacterController"] = [this](nuke::Component* c) {
		auto* cc = static_cast<nuke::CharacterController*>(c);
		// One click sizes + places the capsule from the sibling mesh's bounds (pivot,
		// offset, height, radius) — no manual aligning against the visual.
		if (ImGui::Button("Fit To Mesh", ImVec2(-FLT_MIN, 0)))
		{
			if (cc->FitToMesh()) worldDirty = true;
			else StatusBar::Set("cc.fit", "Fit To Mesh: no MeshRenderer with a mesh on this atom");
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Set Pivot/Capsule Offset/Height/Radius from the sibling mesh's bounds.");
	};
	inspectorOverrides["Foliage"] = [this](nuke::Component* c) {
		auto* fol = static_cast<nuke::Foliage*>(c);
		// Fill = full deterministic re-scatter by the rules; Clear wipes the layer.
		const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
		if (ImGui::Button("Fill", ImVec2(half, 0)))
		{
			fol->Rebuild();
			worldDirty = true;
			StatusBar::Set("foliage", std::string("Foliage: ") + std::to_string(fol->InstanceCount()) + " instances");
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Re-scatter the whole surface by the rules (density/seed/masks).");
		ImGui::SameLine();
		if (ImGui::Button("Clear", ImVec2(half, 0))) { fol->ClearInstances(); worldDirty = true; }
		// Brush: paint/erase strokes in the viewport while this layer stays selected.
		bool paint = foliageBrush == 1, erase = foliageBrush == 2;
		if (ImGui::Checkbox("Paint", &paint)) foliageBrush = paint ? 1 : 0;
		ImGui::SameLine();
		if (ImGui::Checkbox("Erase", &erase)) foliageBrush = erase ? 2 : 0;
		ImGui::SameLine();
		ImGui::TextDisabled("(%d)", fol->InstanceCount());
		if (foliageBrush != 0)
		{
			ImGui::SliderFloat("Brush Radius", &foliageBrushRadius, 0.25f, 20.0f, "%.2f m");
			if (foliageBrush == 1) ImGui::SliderFloat("Brush Density", &foliageBrushDensity, 0.1f, 4.0f, "x%.2f");
			ImGui::TextDisabled("LMB in the viewport: %s", foliageBrush == 1 ? "paint" : "erase");
		}
	};
}

// Animator: the serialized state machine, editable in place (states table + transitions +
// entry). Every edit goes through the component's own mutators, so smJson (the persisted
// form) stays in sync and saves with the world/prefab.
void EditorUI::DrawAnimatorInspector(nuke::Animator* an)
{
	an->EnsureSM();
	ImGui::SeparatorText("State Machine");

	// entry state combo
	{
		const char* cur = an->entryState.empty() ? "(none)" : an->entryState.c_str();
		ImGui::SetNextItemWidth(160);
		if (ImGui::BeginCombo("Entry", cur))
		{
			if (ImGui::Selectable("(none)", an->entryState.empty())) { an->SetEntry(""); worldDirty = true; }
			for (const auto& s : an->states)
				if (ImGui::Selectable(s.first.c_str(), s.first == an->entryState)) { an->SetEntry(s.first); worldDirty = true; }
			ImGui::EndCombo();
		}
	}

	// states: name | clip picker | loop | speed | remove
	std::string removeState;
	for (auto& s : an->states)
	{
		ImGui::PushID(("st" + s.first).c_str());
		ImGui::Bullet(); ImGui::SameLine();
		ImGui::TextUnformatted(s.first.c_str());
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 8);
		if (ImGui::SmallButton("X")) removeState = s.first;
		std::string clip = s.second.clip;
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
		if (AssetPicker("Clip", clip, "anim", "")) { s.second.clip = clip; an->EncodeSM(); worldDirty = true; }
		bool lp = s.second.loop;
		if (ImGui::Checkbox("Loop", &lp)) { s.second.loop = lp; an->EncodeSM(); worldDirty = true; }
		ImGui::SameLine();
		float sp = (float)s.second.speed;
		ImGui::SetNextItemWidth(120);
		if (ImGui::DragFloat("Speed", &sp, 0.01f, 0.0f, 10.0f)) { s.second.speed = sp; an->EncodeSM(); worldDirty = true; }
		ImGui::PopID();
	}
	if (!removeState.empty()) { an->RemoveState(removeState); worldDirty = true; }

	static char newState[64] = "";
	ImGui::SetNextItemWidth(160);
	ImGui::InputTextWithHint("##newstate", "state name", newState, sizeof(newState));
	ImGui::SameLine();
	if (ImGui::SmallButton(ICON_LC_PLUS " State") && newState[0])
	{
		an->AddState(newState, "", true, 1.0);
		newState[0] = 0;
		worldDirty = true;
	}

	// transitions: from -> to | fade | remove
	if (!an->transitions.empty()) ImGui::SeparatorText("Transitions");
	std::pair<std::string, std::string> removeTr;
	for (auto& from : an->transitions)
		for (auto& to : from.second)
		{
			ImGui::PushID(("tr" + from.first + ">" + to.first).c_str());
			ImGui::Text("%s " ICON_LC_ARROW_RIGHT " %s", from.first.c_str(), to.first.c_str());
			ImGui::SameLine();
			float fade = (float)to.second;
			ImGui::SetNextItemWidth(90);
			if (ImGui::DragFloat("##fade", &fade, 0.01f, 0.0f, 5.0f, "%.2f s")) { to.second = fade; an->EncodeSM(); worldDirty = true; }
			ImGui::SameLine();
			if (ImGui::SmallButton("X")) removeTr = { from.first, to.first };
			ImGui::PopID();
		}
	if (!removeTr.first.empty()) { an->RemoveTransition(removeTr.first, removeTr.second); worldDirty = true; }

	// add transition: two state combos + fade
	if (an->states.size() >= 2)
	{
		static std::string trFrom, trTo;
		static float trFade = 0.2f;
		auto stateCombo = [&](const char* id, std::string& v)
		{
			ImGui::SetNextItemWidth(110);
			if (ImGui::BeginCombo(id, v.empty() ? "..." : v.c_str()))
			{
				for (const auto& s : an->states)
					if (ImGui::Selectable(s.first.c_str(), s.first == v)) v = s.first;
				ImGui::EndCombo();
			}
		};
		stateCombo("##trfrom", trFrom); ImGui::SameLine();
		ImGui::TextUnformatted(ICON_LC_ARROW_RIGHT); ImGui::SameLine();
		stateCombo("##trto", trTo); ImGui::SameLine();
		ImGui::SetNextItemWidth(70);
		ImGui::DragFloat("##trfade", &trFade, 0.01f, 0.0f, 5.0f, "%.2f s");
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_LC_PLUS "##tradd") && !trFrom.empty() && !trTo.empty() && trFrom != trTo)
		{
			an->AddTransition(trFrom, trTo, trFade);
			worldDirty = true;
		}
	}
}

// PostProcess panel: the ordered chain of custom post-effect shaders. Each effect = a post-shader pick +
// its params (parsed from the shader's PostParams), reorder + remove + add. Effects edit the runtime list
// and Commit() back to the serialized field.
void EditorUI::DrawPostProcessInspector(nuke::PostProcess* pp)
{
	ResDB* db = ResDB::getSingleton();
	pp->EnsureParsed();
	ImGui::SeparatorText("Post Effects (run in order — drag to reorder)");

	int removeAt = -1, dragFrom = -1, dragTo = -1;
	const ImGuiPayload* dpl = ImGui::GetDragDropPayload();
	bool dndActive = dpl && dpl->IsDataType("PP_FX");
	// Thin "insert before index N" zone on the top edge of a row (drag only; adds no layout height) — same
	// technique as the hierarchy, so the drop position follows the cursor instead of always landing on top.
	auto gapZone = [&](int beforeIdx, ImVec2 mn, ImVec2 mx)
	{
		if (!dndActive) return;
		ImVec2 saved = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(mn.x, mn.y - 3.0f));
		ImGui::InvisibleButton(("##gap" + std::to_string(beforeIdx)).c_str(), ImVec2(mx.x - mn.x, 6.0f));
		if (ImGui::BeginDragDropTarget())
		{
			ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mn.y), ImVec2(mx.x, mn.y), IM_COL32(255, 160, 30, 255), 2.0f);
			if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PP_FX")) { dragFrom = *(const int*)p->Data; dragTo = beforeIdx; }
			ImGui::EndDragDropTarget();
		}
		ImGui::SetCursorScreenPos(saved);
	};
	for (size_t i = 0; i < pp->effects.size(); ++i)
	{
		nuke::PostEffect& e = pp->effects[i];
		ImGui::PushID((int)i);
		bool en = e.enabled;
		if (ImGui::Checkbox("##en", &en)) { e.enabled = en; pp->Commit(); }
		ImGui::SameLine();
		nuke::Shader* sh = db->GetShader(e.shaderGuid);
		std::string title = sh ? sh->name : (e.shaderGuid.empty() ? std::string("(pick a shader)") : e.shaderGuid);
		ImGui::SetNextItemAllowOverlap();   // so the X button (drawn over the header) takes its own clicks
		bool open = ImGui::CollapsingHeader((title + "##h" + std::to_string(i)).c_str());
		ImVec2 hmn = ImGui::GetItemRectMin(), hmx = ImGui::GetItemRectMax();   // header rect (for the gap line)
		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
		{
			int src = (int)i; ImGui::SetDragDropPayload("PP_FX", &src, sizeof(int));
			ImGui::TextUnformatted(title.c_str());
			ImGui::EndDragDropSource();
		}
		ImGui::SameLine(ImGui::GetContentRegionMax().x - 24);
		if (ImGui::SmallButton(ICON_LC_X "##x")) removeAt = (int)i;
		gapZone((int)i, hmn, hmx);   // drop here = insert BEFORE effect i
		if (open)
		{
			if (AssetPicker("Shader", e.shaderGuid, "postshader")) { e.props.clear(); pp->Commit(); }
			sh = db->GetShader(e.shaderGuid);
			if (sh)
				for (const nuke::ShaderProp& sp : sh->props)
				{
					// g_Nuke* = SYSTEM params (audio analysis / time), engine-filled per
					// frame while packing the blob — never user-editable, so not drawn.
					if (sp.name.compare(0, 6, "g_Nuke") == 0) continue;
					auto it = e.props.find(sp.name);
					std::array<float, 4> val = (it != e.props.end()) ? it->second
						: std::array<float, 4>{ sp.def[0], sp.def[1], sp.def[2], sp.def[3] };
					bool ch = false;
					if (sp.isColor && sp.components >= 3)
						ch = (sp.components == 3) ? ImGui::ColorEdit3(sp.name.c_str(), val.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)
						                          : ImGui::ColorEdit4(sp.name.c_str(), val.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
					else if (sp.components == 1) ch = ImGui::DragFloat(sp.name.c_str(), val.data(), 0.01f);
					else if (sp.components == 2) ch = ImGui::DragFloat2(sp.name.c_str(), val.data(), 0.01f);
					else if (sp.components == 3) ch = ImGui::DragFloat3(sp.name.c_str(), val.data(), 0.01f);
					else                         ch = ImGui::DragFloat4(sp.name.c_str(), val.data(), 0.01f);
					if (ch) { e.props[sp.name] = val; pp->Commit(); }
				}
			if (!sh) ImGui::TextDisabled("Pick a post shader (a *.post.hlsl asset).");
		}
		ImGui::PopID();
	}
	// Tail zone: drop below the last effect = move to the END of the chain.
	if (dndActive && !pp->effects.empty())
	{
		ImVec2 mn = ImGui::GetCursorScreenPos();
		float w = ImGui::GetContentRegionAvail().x;
		ImGui::InvisibleButton("##gapend", ImVec2(w, 8.0f));
		if (ImGui::BeginDragDropTarget())
		{
			ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mn.y + 1), ImVec2(mn.x + w, mn.y + 1), IM_COL32(255, 160, 30, 255), 2.0f);
			if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PP_FX")) { dragFrom = *(const int*)p->Data; dragTo = (int)pp->effects.size(); }
			ImGui::EndDragDropTarget();
		}
	}
	// Structural edits (add / remove / reorder) are undoable as one atom-subtree delta (the param drags are
	// already covered by the active-widget edit detector; these are button/DnD clicks it can't see).
	nuke::Atom* owner = pp->atom;
	World* w = AppInstance::GetSingleton()->currentWorld;
	auto recordStructural = [&](const char* label, const std::function<void()>& mutate)
	{
		if (!owner || !w) { mutate(); return; }
		long id = owner->id.id, parent = owner->parent ? owner->parent->id.id : 0;
		int index = 0; { auto& lst = owner->parent ? owner->parent->children : w->GetHierarchy(); int i = 0; for (Atom* s : lst) { if (s == owner) { index = i; break; } ++i; } }
		std::string before = nuke::SaveAtomToString(owner);
		mutate();
		std::string after = nuke::SaveAtomToString(owner);
		if (after == before) return;
		editing = false; editAtomId = 0;   // this is its own command — don't double up with the auto detector
		PushUndo(label,
			[this, id, parent, index, before]{ ApplyAtomState(id, parent, index, before); },
			[this, id, parent, index, after ]{ ApplyAtomState(id, parent, index, after ); });
	};

	if (removeAt >= 0)
		recordStructural("Remove effect", [&] { pp->effects.erase(pp->effects.begin() + removeAt); pp->Commit(); });
	else if (dragFrom >= 0 && dragTo >= 0 && dragFrom != dragTo
	         && dragFrom < (int)pp->effects.size() && dragTo <= (int)pp->effects.size())
		recordStructural("Reorder effect", [&] {
			nuke::PostEffect moved = pp->effects[dragFrom];
			pp->effects.erase(pp->effects.begin() + dragFrom);
			int dst = (dragTo > dragFrom) ? dragTo - 1 : dragTo;   // index shifts after the erase
			if (dst < 0) dst = 0; if (dst > (int)pp->effects.size()) dst = (int)pp->effects.size();
			pp->effects.insert(pp->effects.begin() + dst, moved);
			pp->Commit();
		});

	ImGui::Separator();
	if (ImGui::Button(ICON_LC_PLUS " Add Effect", ImVec2(-FLT_MIN, 0)))
		recordStructural("Add effect", [&] { pp->effects.push_back(nuke::PostEffect{}); pp->Commit(); });
}

// MeshRenderer's custom panel: the selected material's sub-properties (shader/color/textures).
// The mesh/material GUID fields themselves render as pickers via reflection (asset metadata).
void EditorUI::DrawMeshRendererInspector(nuke::MeshRenderer* mr)
{
	ResDB* db = ResDB::getSingleton();
	if (!mr->mesh || mr->mesh->guid != mr->meshGuid) mr->mesh = db->GetMesh(mr->meshGuid);
	// Material picker changed -> (re)clone the asset into this object's OWNED instance. Editing the
	// instance below never touches the original .numat asset.
	std::string curMat = mr->mat ? mr->mat->guid : std::string();
	if (curMat != mr->matGuid)
	{
		Material* asset = db->GetMaterial(mr->matGuid);
		if (mr->mat) delete mr->mat;
		mr->mat = asset ? asset->Clone() : nullptr;
	}

	if (Material* m = mr->mat)
	{
		ImGui::SeparatorText("Material");
		// Reflection-driven: shader + every PBR map (asset pickers) + color swatch + metallic/roughness/
		// emissive — all from Material's [[nuke::prop]] schema, no per-field hardcode here.
		if (DrawFields(m, m->GetType()))
			m->Resolve();   // rebind shader/textures after an edit

		// Shader params: schema from the instance's shader, VALUES on the instance (m->props),
		// saved with the world. Unset shows the shader's default.
		if (m->shader && !m->shader->props.empty())
		{
			ImGui::SeparatorText("Shader Params");
			for (const nuke::ShaderProp& sp : m->shader->props)
			{
				float v[4] = { sp.def[0], sp.def[1], sp.def[2], sp.def[3] };
				auto it = m->props.find(sp.name);
				if (it != m->props.end())
					for (int i = 0; i < 4; ++i) v[i] = it->second[i];
				const char* lbl = sp.name.c_str();
				if (sp.name.rfind("g_", 0) == 0) lbl += 2;   // strip g_ prefix for display
				bool ch = false;
				if (sp.isColor && sp.components >= 3)   // @color: HDR-capable colour picker (tints may exceed 1)
					ch = (sp.components == 3) ? ImGui::ColorEdit3(lbl, v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)
					                          : ImGui::ColorEdit4(lbl, v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
				else switch (sp.components)
				{
				case 1:  ch = ImGui::DragFloat(lbl, v, 0.01f); break;
				case 2:  ch = ImGui::DragFloat2(lbl, v, 0.01f); break;
				case 3:  ch = ImGui::DragFloat3(lbl, v, 0.01f); break;
				default: ch = ImGui::DragFloat4(lbl, v, 0.01f); break;
				}
				if (ch)
				{
					std::array<float, 4>& a = m->props[sp.name];
					a = { 0, 0, 0, 0 };
					for (int i = 0; i < sp.components; ++i) a[i] = v[i];
				}
			}
		}
	}
}

// Auto-draw a reflected object's fields from its schema (component inspector).
bool EditorUI::DrawFields(void* obj, nuke::TypeInfo* ti)
{
	if (!ti) return false;
	bool changed = false;
	for (const nuke::Field& f : ti->fields)
	{
		if (f.hidden) continue;   // serialized but not shown (e.g. script props JSON)
		void* a = f.addr(obj);
		const char* n = f.label.empty() ? f.name.c_str() : f.label.c_str();   // metadata display name
		// Label BEFORE the field: draw the name, then the widget with a hidden id fills the rest of the
		// row (ImGui's default puts the label AFTER the widget, which reads backwards).
		std::string hid = "##" + f.name; const char* w = hid.c_str();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(n);
		// [[prop(tip="...")]] -> tooltip on the label AND on the widget (checked again after the switch)
		bool tipHover = !f.tip.empty() && ImGui::IsItemHovered();
		ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.42f);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		switch (f.type)
		{
		case nuke::FT::Bool:   changed |= ImGui::Checkbox(w, (bool*)a); break;
		case nuke::FT::Int:
			if (f.widget == "layers")    // [[prop(widget="layers")]] -> named multi-select over nuke::Layers
				changed |= DrawLayerMaskCombo(w, *(int*)a);
			else if (!f.enumLabels.empty())   // [[prop(enum=...)]] -> dropdown; the int is the selected index
			{
				int* iv = (int*)a; int cur = (*iv < 0 || *iv >= (int)f.enumLabels.size()) ? 0 : *iv;
				if (ImGui::BeginCombo(w, f.enumLabels[cur].c_str()))
				{
					for (int e = 0; e < (int)f.enumLabels.size(); ++e)
						if (ImGui::Selectable(f.enumLabels[e].c_str(), e == cur)) { *iv = e; changed = true; }
					ImGui::EndCombo();
				}
			}
			else changed |= ImGui::InputInt(w, (int*)a);
			break;
		case nuke::FT::Float:
			if (f.fmax > f.fmin) changed |= ImGui::SliderFloat(w, (float*)a, f.fmin, f.fmax);   // [[prop(min,max)]]
			else                 changed |= ImGui::DragFloat(w, (float*)a, 0.05f);
			break;
		case nuke::FT::Double: changed |= ImGui::InputDouble(w, (double*)a); break;
		case nuke::FT::String:
		{
			std::string* s = (std::string*)a;
			if (!f.asset.empty()) { changed |= AssetPicker(w, *s, f.asset); break; }   // metadata-driven picker
			char buf[256]; strncpy(buf, s->c_str(), 255); buf[255] = 0;
			if (ImGui::InputText(w, buf, sizeof(buf))) { *s = buf; changed = true; }
			break;
		}
		case nuke::FT::Vec2:
		{
			Vector2* v = (Vector2*)a; float t[2] = { (float)v->x, (float)v->y };
			if (ImGui::DragFloat2(w, t, 0.05f)) { v->x = t[0]; v->y = t[1]; changed = true; }
			break;
		}
		case nuke::FT::Vec3:
		{
			Vector3* v = (Vector3*)a; float t[3] = { (float)v->x, (float)v->y, (float)v->z };
			if (ImGui::DragFloat3(w, t, 0.05f)) { v->x = t[0]; v->y = t[1]; v->z = t[2]; changed = true; }
			break;
		}
		case nuke::FT::Vec4:
		case nuke::FT::Quat:
		{
			Vector4* v = (Vector4*)a; float t[4] = { (float)v->x, (float)v->y, (float)v->z, (float)v->w };
			if (ImGui::DragFloat4(w, t, 0.05f)) { v->x = t[0]; v->y = t[1]; v->z = t[2]; v->w = t[3]; changed = true; }
			break;
		}
		case nuke::FT::Color:
		{
			Color* c = (Color*)a; float t[4] = { (float)c->r, (float)c->g, (float)c->b, (float)c->a };
			if (ImGui::ColorEdit4(w, t)) { c->r = t[0]; c->g = t[1]; c->b = t[2]; c->a = t[3]; changed = true; }
			break;
		}
		case nuke::FT::AtomRef:
		{
			// A reference to a live Atom: combo of the world's atoms (+ None) and a drag-drop
			// target from the hierarchy. Stored as the pointer; serialization travels by stable id.
			// The prop's asset= hint FILTERS the picker to atoms carrying that component (e.g.
			// asset="Camera" lists only camera atoms) — drops of anything else are rejected too.
			Atom** slot = (Atom**)a;
			auto passes = [&](Atom* at) -> bool
			{
				if (f.asset.empty() || !at) return at != nullptr;
				for (nuke::Component* c : at->components)
					if (c) if (nuke::TypeInfo* ti = c->GetType()) if (ti->name == f.asset) return true;
				return false;
			};
			const char* cur = *slot ? (*slot)->name.c_str() : "<none>";
			if (ImGui::BeginCombo(w, cur))
			{
				if (ImGui::Selectable("<none>", *slot == nullptr)) { *slot = nullptr; changed = true; }
				std::function<void(bc::list<Atom*>&)> walk = [&](bc::list<Atom*>& gos)
				{
					for (Atom* at : gos)
					{
						if (!at) continue;
						if (passes(at))
						{
							ImGui::PushID((void*)at);
							if (ImGui::Selectable(at->name.c_str(), *slot == at)) { *slot = at; changed = true; }
							ImGui::PopID();
						}
						walk(at->children);
					}
				};
				walk(AppInstance::GetSingleton()->currentWorld->GetHierarchy());
				ImGui::EndCombo();
			}
			if (ImGui::BeginDragDropTarget())   // drop an atom from the hierarchy panel
			{
				if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NUKE_ATOM"))
				{
					Atom* dropped = *(Atom**)p->Data;
					if (passes(dropped)) { *slot = dropped; changed = true; }
				}
				ImGui::EndDragDropTarget();
			}
			break;
		}
		case nuke::FT::IntList:
		case nuke::FT::FloatList:
		case nuke::FT::DoubleList:
		case nuke::FT::StringList:
		{
			// Reflected std::vector<T>: the widget column shows "<N> [+]"; element rows follow
			// full-width, each with move-up/move-down/remove. Element widgets mirror the scalar
			// cases — a StringList with an asset= hint gets a real PICKER per row, not a text box.
			// The FIELD NAME scopes the ID stack: row ids are per-index, and two list fields
			// side by side would otherwise collide (row 0 of one == row 0 of the next).
			ImGui::PushID(f.name.c_str());
			// [[prop(widget="curve")]] on a float list: a BEZIER curve editor over
			// (t, value, inTangent, outTangent) keys — the exact format/math the VFX runtime
			// evaluates (cubic Hermite; tangents are dv/dt slopes the user shapes by hand).
			// Click a key = select it + show its tangent HANDLES (drag them to bend the curve),
			// drag a key to move it, double-click empty = add a key (auto tangent preserving
			// the local shape), right-click a key = delete. Legacy (t,v)-pair data (odd
			// stride) upgrades in place with smooth auto tangents, same as the runtime.
			if (f.type == nuke::FT::FloatList && f.widget == "curve")
			{
				std::vector<float>* c = (std::vector<float>*)a;
				if (!c->empty() && c->size() % 4 != 0 && c->size() % 2 == 0)   // legacy pairs -> keys
				{
					const size_t n = c->size() / 2;
					std::vector<float> up; up.reserve(n * 4);
					for (size_t k = 0; k < n; ++k)
					{
						size_t qa = k == 0 ? 0 : k - 1, qb = k == n - 1 ? n - 1 : k + 1;
						float dt = (*c)[qb * 2] - (*c)[qa * 2];
						float m = dt > 1e-6f ? ((*c)[qb * 2 + 1] - (*c)[qa * 2 + 1]) / dt : 0.f;
						up.insert(up.end(), { (*c)[k * 2], (*c)[k * 2 + 1], m, m });
					}
					c->swap(up); changed = true;
				}
				// keep keys sorted by t BEFORE anything evaluates/draws. Runs every frame except
				// mid-drag of a key (indices must stay stable under the mouse) — the old
				// "sort when changed after release" gate never fired (changed is per-frame and
				// the release frame has no change), leaving fresh keys appended out of order.
				{
					ImGuiStorage* sst = ImGui::GetStateStorage();
					const bool keyDragging = sst->GetInt(ImGui::GetID("##curvedrag"), -1) >= 0 && ImGui::IsMouseDown(0);
					if (!keyDragging && c->size() >= 8)
					{
						int selK = sst->GetInt(ImGui::GetID("##curvesel"), -1);
						float selT = selK >= 0 && selK < (int)(c->size() / 4) ? (*c)[selK * 4] : -1.f;
						float selV = selK >= 0 && selK < (int)(c->size() / 4) ? (*c)[selK * 4 + 1] : 0.f;
						bool reordered = false;
						for (size_t k = 4; k + 3 < c->size(); k += 4)
							for (size_t j = k; j >= 4 && (*c)[j] < (*c)[j - 4]; j -= 4, reordered = true)
								for (int q = 0; q < 4; ++q) std::swap((*c)[j + q], (*c)[j - 4 + q]);
						if (reordered)
						{
							changed = true;   // the sorted order must reach the component/undo
							if (selT >= 0.f)   // selection is BY INDEX — re-find the key
								for (int k = 0; k < (int)(c->size() / 4); ++k)
									if ((*c)[k * 4] == selT && (*c)[k * 4 + 1] == selV)
									{ sst->SetInt(ImGui::GetID("##curvesel"), k); break; }
						}
					}
				}
				const size_t nk = c->size() / 4;
				auto eval = [&](float t) -> float {
					const size_t nn = c->size() / 4; if (!nn) return 1.f;
					if (nn == 1 || t <= (*c)[0]) return (*c)[1];
					if (t >= (*c)[(nn - 1) * 4]) return (*c)[(nn - 1) * 4 + 1];
					size_t k = 1; while (k < nn && t > (*c)[k * 4]) ++k;
					const float t0 = (*c)[(k - 1) * 4], v0 = (*c)[(k - 1) * 4 + 1];
					const float t1 = (*c)[k * 4],       v1 = (*c)[k * 4 + 1];
					const float hh = t1 - t0; if (hh < 1e-6f) return v1;
					const float m0 = (*c)[(k - 1) * 4 + 3] * hh, m1 = (*c)[k * 4 + 2] * hh;
					const float x = (t - t0) / hh, x2 = x * x, x3 = x2 * x;
					return (2 * x3 - 3 * x2 + 1) * v0 + (x3 - 2 * x2 + x) * m0 + (-2 * x3 + 3 * x2) * v1 + (x3 - x2) * m1;
				};
				// view range: fit the keys AND the curve itself (tangent bulges overshoot keys)
				float vmax = 1.0f;
				for (size_t k = 0; k < nk; ++k) vmax = std::max(vmax, (*c)[k * 4 + 1]);
				for (int sN = 0; sN <= 32; ++sN) vmax = std::max(vmax, eval(sN / 32.0f));
				vmax *= 1.15f;
				// While ANYTHING is being dragged the view scale FREEZES: a live-fitting vmax fed
				// the growing value back through the mouse mapping — exponential runaway that
				// dragged a key into the MILLIONS in one hold (poisoned a world's alpha curve).
				{
					ImGuiStorage* stv = ImGui::GetStateStorage();
					const ImGuiID vmaxId = ImGui::GetID("##curvevmax");
					const bool anyDrag = (stv->GetInt(ImGui::GetID("##curvedrag"), -1) >= 0
					                   || stv->GetInt(ImGui::GetID("##curvehdl"), 0) != 0) && ImGui::IsMouseDown(0);
					if (anyDrag) vmax = stv->GetFloat(vmaxId, vmax);
					else         stv->SetFloat(vmaxId, vmax);
				}
				// value clamp from the prop's min=/max= hints (e.g. alpha curves are 0..1)
				const float vLo = f.fmax > f.fmin ? f.fmin : 0.0f;
				const float vHi = f.fmax > f.fmin ? f.fmax : 1e30f;
				const float wpx = ImGui::CalcItemWidth(), hpx = 110.0f;
				ImVec2 p0 = ImGui::GetCursorScreenPos();
				ImGui::InvisibleButton("##curvecanvas", ImVec2(wpx, hpx));
				const bool hov = ImGui::IsItemHovered();
				ImDrawList* dl = ImGui::GetWindowDrawList();
				dl->AddRectFilled(p0, ImVec2(p0.x + wpx, p0.y + hpx), IM_COL32(28, 28, 30, 255));
				for (int gl = 1; gl < 4; ++gl)   // quarter grid
				{
					dl->AddLine(ImVec2(p0.x + wpx * gl / 4, p0.y), ImVec2(p0.x + wpx * gl / 4, p0.y + hpx), IM_COL32(55, 55, 58, 255));
					dl->AddLine(ImVec2(p0.x, p0.y + hpx * gl / 4), ImVec2(p0.x + wpx, p0.y + hpx * gl / 4), IM_COL32(55, 55, 58, 255));
				}
				const float sy = hpx / vmax;   // px per value unit (x axis: wpx px per t unit)
				auto toPx = [&](float t, float v) { return ImVec2(p0.x + t * wpx, p0.y + (1.0f - v / vmax) * hpx); };
				ImVec2 prev = toPx(0.f, eval(0.f));
				for (int sN = 1; sN <= 96; ++sN)
				{
					ImVec2 cur = toPx(sN / 96.0f, eval(sN / 96.0f));
					dl->AddLine(prev, cur, IM_COL32(120, 190, 255, 255), 1.6f);
					prev = cur;
				}
				// interaction state (per-widget, in ImGui's storage): dragged key, selected
				// key, dragged tangent handle (0 none / 1 in / 2 out).
				ImGuiStorage* st = ImGui::GetStateStorage();
				const ImGuiID dragId = ImGui::GetID("##curvedrag");
				const ImGuiID selId  = ImGui::GetID("##curvesel");
				const ImGuiID hdlId  = ImGui::GetID("##curvehdl");
				int dragKey = st->GetInt(dragId, -1);
				int selKey  = st->GetInt(selId, -1);
				int dragHdl = st->GetInt(hdlId, 0);
				if (selKey >= (int)nk) { selKey = -1; st->SetInt(selId, -1); }
				const ImVec2 mp = ImGui::GetMousePos();
				const float hl = 28.0f;   // tangent handle arm length, px
				auto hdlPos = [&](int k, bool inH) {
					ImVec2 kp = toPx((*c)[k * 4], (*c)[k * 4 + 1]);
					float m = (*c)[k * 4 + (inH ? 2 : 3)];
					ImVec2 d(wpx, -m * sy);   // slope direction in pixel space
					float len = sqrtf(d.x * d.x + d.y * d.y); if (len < 1e-4f) len = 1.f;
					float s = (inH ? -hl : hl) / len;
					return ImVec2(kp.x + d.x * s, kp.y + d.y * s);
				};
				// hover: the selected key's tangent handles take priority over key dots
				int hotKey = -1, hotHdl = 0;
				if (selKey >= 0 && selKey < (int)nk && hov)
				{
					if (selKey > 0)
					{ ImVec2 hp = hdlPos(selKey, true);  float dx = mp.x - hp.x, dy = mp.y - hp.y; if (dx * dx + dy * dy < 49.f) hotHdl = 1; }
					if (!hotHdl && selKey + 1 < (int)nk)
					{ ImVec2 hp = hdlPos(selKey, false); float dx = mp.x - hp.x, dy = mp.y - hp.y; if (dx * dx + dy * dy < 49.f) hotHdl = 2; }
				}
				if (!hotHdl)
					for (int k = 0; k < (int)nk; ++k)
					{
						ImVec2 kp = toPx((*c)[k * 4], (*c)[k * 4 + 1]);
						float dx = mp.x - kp.x, dy = mp.y - kp.y;
						if (hov && dx * dx + dy * dy < 64.0f) hotKey = k;
					}
				// tangent handles of the selected key (endpoints only show their inner arm)
				if (selKey >= 0 && selKey < (int)nk)
				{
					ImVec2 kp = toPx((*c)[selKey * 4], (*c)[selKey * 4 + 1]);
					if (selKey > 0)
					{
						ImVec2 hp = hdlPos(selKey, true);
						dl->AddLine(kp, hp, IM_COL32(255, 200, 80, 160), 1.0f);
						dl->AddCircleFilled(hp, hotHdl == 1 || dragHdl == 1 ? 4.5f : 3.5f, IM_COL32(255, 200, 80, 255));
					}
					if (selKey + 1 < (int)nk)
					{
						ImVec2 hp = hdlPos(selKey, false);
						dl->AddLine(kp, hp, IM_COL32(255, 200, 80, 160), 1.0f);
						dl->AddCircleFilled(hp, hotHdl == 2 || dragHdl == 2 ? 4.5f : 3.5f, IM_COL32(255, 200, 80, 255));
					}
				}
				for (int k = 0; k < (int)nk; ++k)
				{
					ImVec2 kp = toPx((*c)[k * 4], (*c)[k * 4 + 1]);
					dl->AddCircleFilled(kp, k == dragKey || k == hotKey ? 5.0f : 3.5f,
					                    k == selKey ? IM_COL32(255, 200, 80, 255) : IM_COL32(230, 230, 235, 255));
				}
				// press priority: tangent handle > key (select + drag) > empty (deselect)
				if (hov && ImGui::IsMouseClicked(0))
				{
					if (hotHdl)           { st->SetInt(hdlId, hotHdl); dragHdl = hotHdl; }
					else if (hotKey >= 0) { st->SetInt(dragId, hotKey); dragKey = hotKey; st->SetInt(selId, hotKey); selKey = hotKey; }
					else                  { st->SetInt(selId, -1); selKey = -1; }
				}
				if (dragHdl && selKey >= 0 && selKey < (int)nk && ImGui::IsMouseDown(0))
				{
					ImVec2 kp = toPx((*c)[selKey * 4], (*c)[selKey * 4 + 1]);
					float dt = (mp.x - kp.x) / wpx, dv = -(mp.y - kp.y) / sy;
					if (dragHdl == 1) dt = std::min(dt, -0.004f); else dt = std::max(dt, 0.004f);
					(*c)[selKey * 4 + (dragHdl == 1 ? 2 : 3)] = dv / dt;
					changed = true;
				}
				if (dragKey >= 0 && dragKey < (int)nk && ImGui::IsMouseDown(0))
				{
					(*c)[dragKey * 4]     = std::max(0.f, std::min(1.f, (mp.x - p0.x) / wpx));
					(*c)[dragKey * 4 + 1] = std::max(vLo, std::min(vHi, (1.0f - (mp.y - p0.y) / hpx) * vmax));
					changed = true;
				}
				if (!ImGui::IsMouseDown(0)) { if (dragKey >= 0) st->SetInt(dragId, -1); if (dragHdl) st->SetInt(hdlId, 0); }
				if (hov && ImGui::IsMouseDoubleClicked(0) && hotKey < 0 && !hotHdl)   // add key
				{
					float t = std::max(0.f, std::min(1.f, (mp.x - p0.x) / wpx));
					float v = std::max(vLo, std::min(vHi, (1.0f - (mp.y - p0.y) / hpx) * vmax));
					// auto tangent = the curve's current slope at t: adding a key keeps the shape
					float m = nk ? (eval(std::min(1.f, t + 0.01f)) - eval(std::max(0.f, t - 0.01f))) / 0.02f : 0.f;
					c->insert(c->end(), { t, v, m, m });
					changed = true;
				}
				if (hov && ImGui::IsMouseClicked(1) && hotKey >= 0)        // right-click a key = delete
				{
					c->erase(c->begin() + hotKey * 4, c->begin() + hotKey * 4 + 4);
					st->SetInt(selId, -1); st->SetInt(dragId, -1); st->SetInt(hdlId, 0);
					changed = true;
				}
				if (nk == 0)
					dl->AddText(ImVec2(p0.x + 8, p0.y + hpx * 0.5f - 8), IM_COL32(140, 140, 145, 255),
					            "constant 1  -  double-click to add keys");
				else if (hov && hotKey < 0 && !hotHdl && dragKey < 0 && !dragHdl)
					dl->AddText(ImVec2(p0.x + 6, p0.y + 3), IM_COL32(120, 120, 125, 200),
					            "click key: tangents  |  dblclick: add  |  RMB key: delete");
				ImGui::PopID();
				break;
			}
			// [[prop(widget="gradient")]]: (t,r,g,b) stops as a PROPER gradient editor — the
			// live bar with draggable stop MARKERS under it. Click an empty spot on the bar =
			// add a stop with the color already there, drag a marker to move it, click a
			// marker (no drag) = color picker popup, right-click a marker = delete.
			if (f.type == nuke::FT::FloatList && f.widget == "gradient")
			{
				std::vector<float>* g4 = (std::vector<float>*)a;
				size_t nStops = g4->size() / 4;
				const float wpx = ImGui::CalcItemWidth(), bh = 22.0f, mh = 16.0f;
				ImVec2 p0 = ImGui::GetCursorScreenPos();
				ImGui::InvisibleButton("##gradbar", ImVec2(wpx, bh + mh));
				const bool hov = ImGui::IsItemHovered();
				ImDrawList* dl = ImGui::GetWindowDrawList();
				auto stopCol = [&](size_t i) {
					return IM_COL32((int)((*g4)[i * 4 + 1] * 255.f), (int)((*g4)[i * 4 + 2] * 255.f), (int)((*g4)[i * 4 + 3] * 255.f), 255);
				};
				// interpolated color at t — what a NEW stop inherits, so adding one is seamless
				auto evalCol = [&](float t, float out[3]) {
					out[0] = out[1] = out[2] = 1.f;
					const size_t ns = g4->size() / 4; if (!ns) return;
					if (t <= (*g4)[0]) { for (int q = 0; q < 3; ++q) out[q] = (*g4)[1 + q]; return; }
					for (size_t i = 1; i < ns; ++i)
						if (t <= (*g4)[i * 4])
						{
							float t0 = (*g4)[(i - 1) * 4], t1 = (*g4)[i * 4];
							float fq = (t1 - t0) > 1e-6f ? (t - t0) / (t1 - t0) : 0.f;
							for (int q = 0; q < 3; ++q) out[q] = (*g4)[(i - 1) * 4 + 1 + q] + ((*g4)[i * 4 + 1 + q] - (*g4)[(i - 1) * 4 + 1 + q]) * fq;
							return;
						}
					for (int q = 0; q < 3; ++q) out[q] = (*g4)[(ns - 1) * 4 + 1 + q];
				};
				if (nStops == 0)
					dl->AddRectFilled(p0, ImVec2(p0.x + wpx, p0.y + bh), IM_COL32(255, 255, 255, 255));
				else
				{
					dl->AddRectFilled(p0, ImVec2(p0.x + (*g4)[0] * wpx, p0.y + bh), stopCol(0));
					for (size_t k = 1; k < nStops; ++k)
					{
						float xa = p0.x + (*g4)[(k - 1) * 4] * wpx, xb = p0.x + (*g4)[k * 4] * wpx;
						dl->AddRectFilledMultiColor(ImVec2(xa, p0.y), ImVec2(xb, p0.y + bh),
						                            stopCol(k - 1), stopCol(k), stopCol(k), stopCol(k - 1));
					}
					dl->AddRectFilled(ImVec2(p0.x + (*g4)[(nStops - 1) * 4] * wpx, p0.y), ImVec2(p0.x + wpx, p0.y + bh), stopCol(nStops - 1));
				}
				dl->AddRect(p0, ImVec2(p0.x + wpx, p0.y + bh), IM_COL32(90, 90, 90, 255));
				if (nStops == 0)
					dl->AddText(ImVec2(p0.x + 8, p0.y + 4), IM_COL32(90, 90, 95, 255), "click to add a color stop");
				// stop markers (triangle + swatch) below the bar
				ImGuiStorage* st = ImGui::GetStateStorage();
				const ImGuiID dragId  = ImGui::GetID("##graddrag");
				const ImGuiID movedId = ImGui::GetID("##gradmoved");
				const ImGuiID editId  = ImGui::GetID("##gradedit");
				int dragStop = st->GetInt(dragId, -1);
				const ImVec2 mp = ImGui::GetMousePos();
				int hot = -1;
				for (int k = 0; k < (int)nStops; ++k)
					if (hov && fabsf(mp.x - (p0.x + (*g4)[k * 4] * wpx)) < 6.0f && mp.y > p0.y + bh - 4.0f) hot = k;
				for (int k = 0; k < (int)nStops; ++k)
				{
					float mx = p0.x + (*g4)[k * 4] * wpx;
					ImU32 oc = (k == hot || k == dragStop) ? IM_COL32(255, 200, 80, 255) : IM_COL32(210, 210, 215, 255);
					dl->AddTriangleFilled(ImVec2(mx, p0.y + bh), ImVec2(mx - 5, p0.y + bh + 6), ImVec2(mx + 5, p0.y + bh + 6), oc);
					dl->AddRectFilled(ImVec2(mx - 5, p0.y + bh + 6), ImVec2(mx + 5, p0.y + bh + mh - 1), stopCol(k));
					dl->AddRect(ImVec2(mx - 5, p0.y + bh + 6), ImVec2(mx + 5, p0.y + bh + mh - 1), oc);
				}
				if (hov && ImGui::IsMouseClicked(0))
				{
					if (hot >= 0) { st->SetInt(dragId, hot); st->SetInt(movedId, 0); dragStop = hot; }
					else if (mp.y < p0.y + bh)   // click the bar = add a stop with the color under it
					{
						float t = std::max(0.f, std::min(1.f, (mp.x - p0.x) / wpx));
						float rgb[3]; evalCol(t, rgb);
						g4->insert(g4->end(), { t, rgb[0], rgb[1], rgb[2] });
						changed = true;
					}
				}
				if (dragStop >= 0 && dragStop < (int)nStops && ImGui::IsMouseDown(0))
				{
					float t = std::max(0.f, std::min(1.f, (mp.x - p0.x) / wpx));
					if (fabsf(t - (*g4)[dragStop * 4]) > 1e-4f) { st->SetInt(movedId, 1); (*g4)[dragStop * 4] = t; changed = true; }
				}
				if (ImGui::IsMouseReleased(0) && dragStop >= 0)
				{
					if (!st->GetInt(movedId, 0) && dragStop < (int)nStops)   // clean click = edit color
					{ st->SetInt(editId, dragStop); ImGui::OpenPopup("##gradstop"); }
					st->SetInt(dragId, -1); dragStop = -1;
				}
				if (hov && ImGui::IsMouseClicked(1) && hot >= 0)   // right-click a marker = delete
				{
					g4->erase(g4->begin() + hot * 4, g4->begin() + hot * 4 + 4);
					nStops = g4->size() / 4;
					st->SetInt(dragId, -1);
					changed = true;
				}
				if (ImGui::BeginPopup("##gradstop"))
				{
					int es = st->GetInt(editId, -1);
					if (es >= 0 && es < (int)(g4->size() / 4))
					{
						float rgb[3] = { (*g4)[es * 4 + 1], (*g4)[es * 4 + 2], (*g4)[es * 4 + 3] };
						if (ImGui::ColorPicker3("##sc", rgb, ImGuiColorEditFlags_NoSidePreview))
						{ for (int q = 0; q < 3; ++q) (*g4)[es * 4 + 1 + q] = rgb[q]; changed = true; }
						ImGui::Text("t = %.2f", (*g4)[es * 4]);
						ImGui::SameLine();
						if (ImGui::SmallButton(ICON_LC_X " Delete"))
						{
							g4->erase(g4->begin() + es * 4, g4->begin() + es * 4 + 4);
							changed = true; ImGui::CloseCurrentPopup();
						}
					}
					else ImGui::CloseCurrentPopup();
					ImGui::EndPopup();
				}
				// keep stops sorted by t EVERY frame nothing is being dragged (not just on
				// changed — the release frame carries no change flag and would never sort);
				// the color-edit popup only changes rgb, so indices stay stable while it is open
				if (st->GetInt(dragId, -1) < 0)
				{
					bool reordered = false;
					for (size_t k = 4; k + 3 < g4->size(); k += 4)
						for (size_t j = k; j >= 4 && (*g4)[j] < (*g4)[j - 4]; j -= 4, reordered = true)
							for (int q = 0; q < 4; ++q) std::swap((*g4)[j + q], (*g4)[j - 4 + q]);
					if (reordered) changed = true;   // sorted order must reach the component/undo
				}
				ImGui::PopID();
				break;
			}
			auto listUI = [&](auto* vec, auto drawElem)
			{
				ImGui::Text("%d", (int)vec->size());
				ImGui::SameLine();
				if (ImGui::SmallButton(ICON_LC_PLUS)) { vec->emplace_back(); changed = true; }
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add element");
				int rm = -1, up = -1, dn = -1;
				const float bw = ImGui::GetFrameHeight();
				const float sp = ImGui::GetStyle().ItemInnerSpacing.x;
				ImGui::Indent();
				for (int i = 0; i < (int)vec->size(); ++i)
				{
					ImGui::PushID(i);
					ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - 3 * (bw + sp)));
					changed |= drawElem((*vec)[i]);
					ImGui::SameLine(0, sp); if (ImGui::Button(ICON_LC_CHEVRON_UP,   ImVec2(bw, 0))) up = i;
					ImGui::SameLine(0, sp); if (ImGui::Button(ICON_LC_CHEVRON_DOWN, ImVec2(bw, 0))) dn = i;
					ImGui::SameLine(0, sp); if (ImGui::Button(ICON_LC_X,            ImVec2(bw, 0))) rm = i;
					ImGui::PopID();
				}
				ImGui::Unindent();
				if      (up > 0)                                { std::swap((*vec)[up], (*vec)[up - 1]); changed = true; }
				else if (dn >= 0 && dn + 1 < (int)vec->size())  { std::swap((*vec)[dn], (*vec)[dn + 1]); changed = true; }
				else if (rm >= 0)                               { vec->erase(vec->begin() + rm); changed = true; }
			};
			if      (f.type == nuke::FT::IntList)    listUI((std::vector<int>*)a,    [&](int& e)    { return ImGui::InputInt("##e", &e); });
			else if (f.type == nuke::FT::FloatList)  listUI((std::vector<float>*)a,  [&](float& e)  { return ImGui::DragFloat("##e", &e, 0.05f); });
			else if (f.type == nuke::FT::DoubleList) listUI((std::vector<double>*)a, [&](double& e) { return ImGui::InputDouble("##e", &e); });
			else listUI((std::vector<std::string>*)a, [&](std::string& e) -> bool
			{
				if (!f.asset.empty()) return AssetPicker("##e", e, f.asset);
				char buf[256]; strncpy(buf, e.c_str(), 255); buf[255] = 0;
				if (ImGui::InputText("##e", buf, sizeof(buf))) { e = buf; return true; }
				return false;
			});
			ImGui::PopID();
			break;
		}
		default: break;
		}
		if (!f.tip.empty() && (tipHover || ImGui::IsItemHovered()))
			ImGui::SetTooltip("%s", f.tip.c_str());
	}
	return changed;
}

// Draw a component's dynamic props (e.g. a Lua script's exported vars). The component
// supplies data only (DynamicProps/SetDynamicProp); all UI lives here in the editor.
void EditorUI::DrawDynamicProps(nuke::Component* cmp)
{
	std::vector<nuke::DynProp> props = cmp->DynamicProps();
	if (props.empty()) return;
	ImGui::Separator();
	ImGui::Text("Script Props");
	for (nuke::DynProp& p : props)
	{
		bool edited = false;
		nuke::NukeVar nv = p.value;
		switch (p.value.kind)
		{
		case nuke::NukeVar::Kind::Number:
		{
			float f = (float)p.value.num;
			if (ImGui::DragFloat(p.name.c_str(), &f, 0.05f)) { nv.num = f; edited = true; }
			break;
		}
		case nuke::NukeVar::Kind::Bool:
		{
			bool b = p.value.b;
			if (ImGui::Checkbox(p.name.c_str(), &b)) { nv.b = b; edited = true; }
			break;
		}
		case nuke::NukeVar::Kind::String:
		{
			char buf[256]; strncpy(buf, p.value.str.c_str(), 255); buf[255] = 0;
			if (ImGui::InputText(p.name.c_str(), buf, sizeof(buf))) { nv.str = buf; edited = true; }
			break;
		}
		case nuke::NukeVar::Kind::AtomRef:
		{
			// A REFERENCE to a live atom by STABLE id — the picker (combo over the world +
			// hierarchy drag-drop), same as reflected Atom* props. Never a text box.
			World* w = AppInstance::GetSingleton()->currentWorld;
			Atom* cur = (w && p.value.refId) ? w->GetById((long)p.value.refId) : nullptr;
			const std::string curLabel = cur ? cur->name
			                          : p.value.refId ? ("<missing #" + std::to_string(p.value.refId) + ">")
			                                          : "<none>";
			if (ImGui::BeginCombo(p.name.c_str(), curLabel.c_str()))
			{
				if (ImGui::Selectable("<none>", p.value.refId == 0)) { nv.refId = 0; edited = true; }
				std::function<void(bc::list<Atom*>&)> walk = [&](bc::list<Atom*>& gos)
				{
					for (Atom* at : gos)
					{
						if (!at) continue;
						ImGui::PushID((void*)at);
						if (ImGui::Selectable(at->name.c_str(), at == cur)) { nv.refId = (long long)at->id.id; edited = true; }
						ImGui::PopID();
						walk(at->children);
					}
				};
				if (w) walk(w->GetHierarchy());
				ImGui::EndCombo();
			}
			if (ImGui::BeginDragDropTarget())   // drop an atom from the hierarchy panel
			{
				if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("NUKE_ATOM"))
				{
					Atom* dropped = *(Atom**)pl->Data;
					if (dropped) { nv.refId = (long long)dropped->id.id; edited = true; }
				}
				ImGui::EndDragDropTarget();
			}
			break;
		}
		default: continue;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(("Reset##" + p.name).c_str())) { nv = p.def; edited = true; }
		if (edited) cmp->SetDynamicProp(p.name, nv);
	}
}

// Vector3 editor with colored X/Y/Z axis labels (Unity-style). True if edited.
bool EditorUI::EditV3(const char* rowLabel, double v[3])
{
	static const char* ax[3] = { "X", "Y", "Z" };
	static const ImVec4 col[3] = { ImVec4(0.86f,0.34f,0.34f,1.0f), ImVec4(0.42f,0.74f,0.36f,1.0f), ImVec4(0.36f,0.55f,0.92f,1.0f) };
	bool ch = false;
	ImGui::PushID(rowLabel);
	float w = (ImGui::GetContentRegionAvail().x - 150.0f) / 3.0f;
	if (w < 36.0f) w = 36.0f;
	for (int i = 0; i < 3; ++i)
	{
		ImGui::PushID(i);
		ImGui::TextColored(col[i], "%s", ax[i]);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(w);
		ch |= ImGui::InputDouble("##v", &v[i], 0.0, 0.0, "%.3f");
		ImGui::SameLine();
		ImGui::PopID();
	}
	ImGui::TextUnformatted(rowLabel);
	ImGui::PopID();
	return ch;
}

// Push this instance's current state into its prefab file (manual; overwrites the .nuprefab, keeps its guid).
void EditorUI::ApplyToPrefab(Atom* a)
{
	if (!a || a->prefabGuid.empty()) return;
	std::string path = ResDB::getSingleton()->PathForGuid(a->prefabGuid);
	if (path.empty()) return;
	nuke::SavePrefab(a, path);
	std::cout << "[editor]\tapplied instance to prefab " << path << std::endl;
}

// Revert this instance to the prefab's saved state (drops its individual overrides). Keeps the atom's
// id + placement + prefab link; undoable as one delta. The old instance object is replaced.
void EditorUI::ResetToPrefab(Atom* a)
{
	if (!a || a->prefabGuid.empty()) return;
	AppInstance* app = AppInstance::GetSingleton();
	World* w = app->currentWorld;
	std::string path = ResDB::getSingleton()->PathForGuid(a->prefabGuid);
	if (path.empty()) return;
	Atom* fresh = nuke::LoadPrefab(path);   // prefab defaults (fresh ids; prefabGuid from the file)
	if (!fresh) return;
	long id = a->id.id, parent = a->parent ? a->parent->id.id : 0;
	int index = 0; { auto& lst = a->parent ? a->parent->children : w->GetHierarchy(); int i = 0; for (Atom* s : lst) { if (s == a) { index = i; break; } ++i; } }
	std::string before = nuke::SaveAtomToString(a);
	fresh->id.id = id;                      // keep this instance's identity + placement
	w->RemoveAtomById(id);
	w->InsertAtom(fresh, parent, index);
	app->selectedInHieararchy = fresh;
	std::string after = nuke::SaveAtomToString(fresh);
	PushUndo("Reset to prefab",
		[this, id, parent, index, before]{ ApplyAtomState(id, parent, index, before); },
		[this, id, parent, index, after ]{ ApplyAtomState(id, parent, index, after ); });
}

// Remove a component from an atom, undoable (captures the atom subtree before/after as one delta).
void EditorUI::RemoveComponent(Atom* a, Component* c)
{
	if (!a || !c) return;
	World* w = AppInstance::GetSingleton()->currentWorld;
	long id = a->id.id, parent = a->parent ? a->parent->id.id : 0;
	int index = 0; { auto& lst = a->parent ? a->parent->children : w->GetHierarchy(); int i = 0; for (Atom* s : lst) { if (s == a) { index = i; break; } ++i; } }
	std::string before = nuke::SaveAtomToString(a);
	a->components.remove(c);   // edit-time removal: NOT Destroy() — that's the runtime/teardown hook
	delete c;
	std::string after = nuke::SaveAtomToString(a);
	editing = false; editAtomId = 0;   // suppress the auto edit-detector (this is its own command)
	PushUndo("Remove component",
		[this, id, parent, index, before]{ ApplyAtomState(id, parent, index, before); },
		[this, id, parent, index, after ]{ ApplyAtomState(id, parent, index, after ); });
}

void EditorUI::winInspector()
{
	if (!win->inspector) return;
	NukeUI::DocPanel("panel:inspector", "Inspector", &win->inspector, window_flags, 380, 700, [this]()
	{
	if (auto sltd = AppInstance::GetSingleton()->selectedInHieararchy)
	{
		char name[128];
		strncpy(name, sltd->GetName().c_str(), 127); name[127] = 0;
		if (ImGui::InputText("Name", name, 128)) sltd->SetName(name);

		// Render layer: which channel this atom renders on (cameras filter by their Layer Mask).
		{
			std::string cur = nuke::Layers::Name(sltd->layer);
			if (cur.empty()) cur = "Layer " + std::to_string(sltd->layer);
			ImGui::SetNextItemWidth(180);
			if (ImGui::BeginCombo("Layer", cur.c_str()))
			{
				for (int i = 0; i < 32; ++i)
				{
					std::string nm = nuke::Layers::Name(i);
					if (nm.empty() && i != sltd->layer) continue;   // unnamed slots hidden
					if (nm.empty()) nm = "Layer " + std::to_string(i);
					if (ImGui::Selectable(nm.c_str(), i == sltd->layer)) sltd->layer = i;
				}
				ImGui::EndCombo();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Render channel (named in Project Settings > Layers); cameras pick what they draw via Layer Mask");
			// Persistent (DontDestroyOnLoad): a ROOT atom flagged here survives GAME world
			// switches during play — subtree, components and live script state carry over.
			ImGui::SameLine();
			bool prs = sltd->persistent;
			if (ImGui::Checkbox("Persistent", &prs)) sltd->persistent = prs;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Survive game world switches (Game.LoadWorld / async activation).\n"
			                                              "Applies to ROOT atoms while PLAYING; children ride with their root.\n"
			                                              "Editor world opens and savegame loads never carry atoms.");
		}

		// Prefab instance bar: this atom IS an instance (a prefab with individual params). Manual sync only.
		if (!sltd->prefabGuid.empty())
		{
			std::string ppath = ResDB::getSingleton()->PathForGuid(sltd->prefabGuid);
			if (!ppath.empty())
			{
				ImGui::Text(ICON_LC_BOX " Prefab: %s", bfs::path(ppath).stem().string().c_str());
				if (ImGui::Button("Apply to prefab", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f - ImGui::GetStyle().ItemSpacing.x * 0.5f, 0))) ApplyToPrefab(sltd);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Overwrite the prefab file with this instance's values");
				ImGui::SameLine();
				if (ImGui::Button("Reset to prefab", ImVec2(-FLT_MIN, 0))) { ResetToPrefab(sltd); ImGui::End(); return; }   // sltd is replaced
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Discard this instance's overrides; reload from the prefab");
			}
		}

		ImGui::SeparatorText("Transform");
		Transform& t = sltd->GetTransform();
		double p[3] = { t.position.x, t.position.y, t.position.z };
		if (EditV3("Position", p))
		{ t.position.x = p[0]; t.position.y = p[1]; t.position.z = p[2]; }
		Vector3 er = t.EulerDeg();
		double r[3] = { er.x, er.y, er.z };
		if (EditV3("Rotation (deg)", r))
			t.SetEulerDeg(Vector3(r[0], r[1], r[2]));
		double s[3] = { t.scale.x, t.scale.y, t.scale.z };
		if (EditV3("Scale", s))
		{ t.scale.x = s[0]; t.scale.y = s[1]; t.scale.z = s[2]; }

		// --- Anchors (canvas children only): pin edges to canvas sides. Enabling a side captures the
		// CURRENT distance; opposite pair enabled = the element stretches with the canvas. Backed by a
		// RectAnchor component (auto-added on first enable); applied every frame by the world.
		{
			nuke::Canvas* cvAnc = nullptr;
			for (Atom* p = sltd->parent; p && !cvAnc; p = p->parent) cvAnc = p->GetComponent<nuke::Canvas>();
			if (cvAnc && cvAnc->transform)
			{
				ImGui::SeparatorText("Anchors");
				nuke::RectAnchor* ra = sltd->GetComponent<nuke::RectAnchor>();
				// Current geometry in canvas units (same conventions as the world's layout pass).
				const bool  world = (cvAnc->mode == nuke::CanvasMode::WorldSpace);
				const float ppu = world ? 1.0f : (cvAnc->pixelsPerUnit > 0.01f ? cvAnc->pixelsPerUnit : 100.0f);
				const float hw = cvAnc->width * 0.5f, hh = cvAnc->height * 0.5f;
				Vector3 cp = cvAnc->transform->globalPosition();
				Vector3 gp = t.globalPosition(), gs = t.globalScale();
				float cx, cy;
				if (world)
				{
					Vector3 R = cvAnc->transform->right(), U = cvAnc->transform->up();
					Vector3 d(gp.x - cp.x, gp.y - cp.y, gp.z - cp.z);
					cx = (float)(d.x*R.x + d.y*R.y + d.z*R.z);
					cy = (float)(d.x*U.x + d.y*U.y + d.z*U.z);
				}
				else { cx = (float)(gp.x - cp.x) * ppu; cy = (float)(gp.y - cp.y) * ppu; }
				nuke::Sprite* spA = sltd->GetComponent<nuke::Sprite>();
				const float ew = spA ? spA->width  * (float)gs.x * ppu : 0.0f;
				const float eh = spA ? spA->height * (float)gs.y * ppu : 0.0f;

				auto ensureRA = [&]() -> nuke::RectAnchor*
				{
					if (!ra) { ra = new nuke::RectAnchor(); sltd->AddComponent(ra); }
					return ra;
				};
				bool l = ra && ra->left, r_ = ra && ra->right, tp = ra && ra->top, b = ra && ra->bottom;
				bool nl = l, nr = r_, nt = tp, nb = b;
				ImGui::Checkbox("Left##anc", &nl);   ImGui::SameLine(120);
				ImGui::Checkbox("Right##anc", &nr);
				ImGui::Checkbox("Bottom##anc", &nb); ImGui::SameLine(120);
				ImGui::Checkbox("Top##anc", &nt);
				if (nl != l || nr != r_ || nt != tp || nb != b)
				{
					nuke::RectAnchor* r2 = ensureRA();
					if (nl && !l) r2->distLeft   = (cx - ew * 0.5f) - (-hw);
					if (nr && !r_) r2->distRight = hw - (cx + ew * 0.5f);
					if (nb && !b) r2->distBottom = (cy - eh * 0.5f) - (-hh);
					if (nt && !tp) r2->distTop   = hh - (cy + eh * 0.5f);
					r2->left = nl; r2->right = nr; r2->top = nt; r2->bottom = nb;
				}
				if (ra)
				{
					const char* unit = world ? "u" : "px";
					if (ra->left)   { ImGui::SetNextItemWidth(90); ImGui::DragFloat((std::string("Dist Left (") + unit + ")##anc").c_str(),   &ra->distLeft); }
					if (ra->right)  { ImGui::SetNextItemWidth(90); ImGui::DragFloat((std::string("Dist Right (") + unit + ")##anc").c_str(),  &ra->distRight); }
					if (ra->bottom) { ImGui::SetNextItemWidth(90); ImGui::DragFloat((std::string("Dist Bottom (") + unit + ")##anc").c_str(), &ra->distBottom); }
					if (ra->top)    { ImGui::SetNextItemWidth(90); ImGui::DragFloat((std::string("Dist Top (") + unit + ")##anc").c_str(),    &ra->distTop); }
					if ((ra->left && ra->right) || (ra->top && ra->bottom))
						ImGui::TextDisabled("opposite sides pinned: stretches with the canvas");
				}
			}
		}

		// Expand/Collapse all — affects the Components category AND every component header.
		std::string atomName = sltd->GetName();
		{
			int force = -1;
			if (ImGui::SmallButton("Expand All"))   force = 1;
			ImGui::SameLine();
			if (ImGui::SmallButton("Collapse All")) force = 0;
			if (force != -1)
			{
				OpenState("Components") = (force == 1);
				for (auto cmp : sltd->components)
					OpenState(atomName + "/" + cmp->name) = (force == 1);
			}
		}

		// Headers are driven by uiOpen (persisted): set state -> draw -> read the toggle back.
		bool& compsOpen = OpenState("Components");
		ImGui::SetNextItemOpen(compsOpen);
		compsOpen = ImGui::CollapsingHeader("Components");
		if (compsOpen)
		{
			for (auto cmp : sltd->components)
			{
				ImGui::PushID(cmp);   // unique ID per component (avoid "Enabled" ID clashes)
				nuke::UnknownComponent* uc = dynamic_cast<nuke::UnknownComponent*>(cmp);
				std::string label = uc ? (uc->typeName.empty() ? std::string("Unknown") : uc->typeName)
				                       : std::string(cmp->name);
				bool& st = OpenState(atomName + "/" + label);
				ImGui::SetNextItemOpen(st);
				std::string hdr = uc ? (label + "  (plugin not loaded)") : label;
				if (!cmp->modOrigin.empty()) hdr += "  [" + cmp->modOrigin + "]";   // mod provenance badge
				ImGui::SetNextItemAllowOverlap();   // let the X button (drawn over the header) take its own clicks
				st = ImGui::CollapsingHeader(hdr.c_str());
				if (!cmp->modOrigin.empty() && ImGui::IsItemHovered())
					ImGui::SetTooltip("Added by mod: %s", cmp->modOrigin.c_str());
				ImGui::SameLine(ImGui::GetContentRegionMax().x - 22);   // remove (undoable)
				if (ImGui::SmallButton(ICON_LC_X "##delcomp")) { pendingCompAtom = sltd; pendingCompDel = cmp; }
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove component");
				if (st)
				{
					if (uc)
					{
						// Component whose plugin isn't loaded: kept inert, data preserved.
						ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.20f, 1.0f), ICON_LC_PLUG " Requires plugin: %s",
							uc->requiredPlugin.empty() ? "(unknown)" : uc->requiredPlugin.c_str());
						ImGui::Text("Enable it in the Plugins window to restore this component.");
					}
					else
					{
						ImGui::Checkbox("Enabled", &cmp->enabled);
						// Tick interval (6.8): Update() every Nth frame, staggered by component id.
						// Anchored to the RIGHT edge. InputInt's item width covers the WHOLE widget
						// (text box + both step buttons): 64 left a ~14px text box that collapsed to
						// nothing on narrow panels — give the number itself real room.
						{
							const float fieldW = 110.0f;   // text box + [-][+] buttons
							const float lblW   = ImGui::CalcTextSize("Tick every").x;
							const float inner  = ImGui::GetStyle().ItemInnerSpacing.x;
							ImGui::SameLine();
							float x = ImGui::GetWindowContentRegionMax().x - fieldW - inner - lblW;
							if (x > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(x);
							ImGui::TextUnformatted("Tick every");
							ImGui::SameLine(0, inner);
							ImGui::SetNextItemWidth(fieldW);
							if (ImGui::InputInt("##tickevery", &cmp->tickEvery) && cmp->tickEvery < 1) cmp->tickEvery = 1;
						}
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip("Run Update() every Nth frame (1 = every frame). Staggered across components,\nso heavy crowds spread over frames. FixedUpdate is unaffected.");
						if (nuke::TypeInfo* cti = cmp->GetType())   // which plugin provides this type
						{
							const char* pl = nuke::PluginForType(cti->name);
							if (pl && pl[0]) ImGui::Text(ICON_LC_PLUG " %s", pl);
						}
						DrawFields(cmp, cmp->GetType());   // auto fields from [[nuke::prop]] schema
						DrawDynamicProps(cmp);             // dynamic props (e.g. Lua script vars)

						// Per-type custom inspector drawing beyond the reflected fields (registered overrides).
						if (nuke::TypeInfo* cti = cmp->GetType())
						{
							auto ov = inspectorOverrides.find(cti->name);
							if (ov != inspectorOverrides.end()) ov->second(cmp);
						}
					}
				}
				ImGui::PopID();
			}
		}

		// Apply a deferred component removal now that the component loop is done (can't mutate mid-iterate).
		if (pendingCompDel) { RemoveComponent(pendingCompAtom, pendingCompDel); pendingCompDel = nullptr; pendingCompAtom = nullptr; }

		// Add any registered, create-able Component type (incl. ones added by plugins).
		// The list keeps GROWING with modules — so: search box (Enter takes the top hit),
		// CATEGORY tree (TypeInfo::category from NUKE_CLASS's 3rd arg; "" = Other), and a
		// height-capped scroll area. Filter walks the whole reflected base CHAIN, so derived
		// components (Foliage : InstancedMesh) are offered too.
		ImGui::Separator();
		if (ImGui::Button("Add Component", ImVec2(-FLT_MIN, 0)))
			ImGui::OpenPopup("addcomp");
		if (ImGui::BeginPopup("addcomp"))
		{
			static char compSearch[64] = "";
			if (ImGui::IsWindowAppearing()) { compSearch[0] = 0; ImGui::SetKeyboardFocusHere(); }
			ImGui::SetNextItemWidth(240.0f);
			ImGui::InputTextWithHint("##compsearch", "Search components...", compSearch, sizeof(compSearch));

			std::vector<nuke::TypeInfo*> comps;
			for (nuke::TypeInfo* ti : nuke::Registry_All())
			{
				if (!ti->create || !nuke::Registry_IsComponentType(ti)) continue;
				// PostProcess is a per-camera effect — only offer it on an atom that has a Camera.
				if (ti->name == "PostProcess" && !sltd->GetComponent<nuke::Camera>()) continue;
				comps.push_back(ti);
			}
			std::sort(comps.begin(), comps.end(),
			          [](nuke::TypeInfo* a, nuke::TypeInfo* b) { return a->name < b->name; });
			auto lc = [](std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; };
			const std::string needle = lc(compSearch);
			auto matches = [&](nuke::TypeInfo* ti) { return needle.empty() || lc(ti->name).find(needle) != std::string::npos; };

			nuke::TypeInfo* picked = nullptr;
			if (!needle.empty())
			{
				// searching: a flat filtered list (height-capped scroll); Enter = first hit
				const float listH = std::min(420.0f, ImGui::GetIO().DisplaySize.y * 0.5f);
				ImGui::BeginChild("##complist", ImVec2(240.0f, listH), false);
				nuke::TypeInfo* first = nullptr;
				for (nuke::TypeInfo* ti : comps)
				{
					if (!matches(ti)) continue;
					if (!first) first = ti;
					if (ImGui::Selectable(ti->name.c_str())) picked = ti;
				}
				if (!picked && first && ImGui::IsKeyPressed(ImGuiKey_Enter)) picked = first;
				ImGui::EndChild();
			}
			else
			{
				// browsing: CASCADING category submenus ("Other" always last), like the "+" menu
				ImGui::Separator();
				std::vector<std::string> cats;
				for (nuke::TypeInfo* ti : comps)
				{
					std::string c = ti->category.empty() ? "Other" : ti->category;
					if (std::find(cats.begin(), cats.end(), c) == cats.end()) cats.push_back(c);
				}
				std::sort(cats.begin(), cats.end());
				auto other = std::find(cats.begin(), cats.end(), "Other");
				if (other != cats.end()) { cats.erase(other); cats.push_back("Other"); }
				for (const std::string& cat : cats)
				{
					if (!ImGui::BeginMenu(cat.c_str())) continue;
					for (nuke::TypeInfo* ti : comps)
						if ((ti->category.empty() ? std::string("Other") : ti->category) == cat)
							if (ImGui::MenuItem(ti->name.c_str())) picked = ti;
					ImGui::EndMenu();
				}
			}
			if (picked)
			{
				sltd->AddComponent((nuke::Component*)picked->create());
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}
	else if (!browserSel.empty() && bfs::is_regular_file(bfs::path(browserSel)))
	{
		DrawAssetInspector(browserSel);
	}
	else
	{
		ImGui::TextWrapped("Select an object in the Hierarchy, or an asset in the Browser.");
	}
	});
}

// Inspector for a project asset selected in the Browser. Dispatched by extension: .nutex (usage + info),
// .numat (reflected material fields), else read-only info + Open. Loaded assets are cached (reloaded on
// selection change) so edits persist to the file (and are undoable).
void EditorUI::DrawAssetInspector(const std::string& path)
{
	std::string ext = bfs::path(path).extension().string();
	for (char& c : ext) c = (char)std::tolower((unsigned char)c);

	boost::system::error_code mec;
	long long mtime = (long long)bfs::last_write_time(bfs::path(path), mec);
	if (path != inspAssetPath || mtime != inspAssetMtime)   // selection changed OR file changed on disk (reimport) -> refresh
	{
		if (inspTex) { delete inspTex; inspTex = nullptr; }
		if (inspMat) { delete inspMat; inspMat = nullptr; }
		// Drop the GPU preview of the previous texture (rebuilt below for the new one).
		if (inspTexPreviewId)
		{
			if (iRender* r = AppInstance::GetSingleton()->render) r->destroyTexture2D(inspTexPreviewId);
			inspTexPreviewId = 0;
		}
		pvStaged.clear();   // restage the 3D preview for the new selection
		inspAssetPath = path; inspAssetMtime = mtime;
		if      (ext == ".nutex") inspTex = nuke::Texture::LoadFromFile(path);
		else if (ext == ".numat") inspMat = nuke::Material::LoadFromFile(path);
		// Texture preview: decode mip0 to RGBA8 and upload once per selection/change. Huge source
		// textures (multi-thousand-px sprite sheets) are box-downsampled to a 2048 cap first: a full
		// 7000x2628 preview is a 73 MB GPU upload that spikes VRAM on every edit — pointless for a
		// panel-width thumbnail, and the spike could starve real scene texture uploads.
		if (inspTex && !inspTex->renderTexture)
			if (iRender* r = AppInstance::GetSingleton()->render)
			{
				std::vector<unsigned char> rgba = inspTex->DecodeRGBA();
				int pw = inspTex->width, ph = inspTex->height;
				if (!rgba.empty() && pw > 0 && ph > 0)
				{
					const int cap = 2048;
					if (pw > cap || ph > cap)
					{
						float s = (float)cap / (float)(pw > ph ? pw : ph);
						int dw = (int)(pw * s); if (dw < 1) dw = 1;
						int dh = (int)(ph * s); if (dh < 1) dh = 1;
						std::vector<unsigned char> ds((size_t)dw * dh * 4);
						for (int y = 0; y < dh; ++y)
							for (int x = 0; x < dw; ++x)
							{
								int sx = (int)(x / s); if (sx >= pw) sx = pw - 1;
								int sy = (int)(y / s); if (sy >= ph) sy = ph - 1;
								const unsigned char* src = &rgba[((size_t)sy * pw + sx) * 4];
								unsigned char* dst = &ds[((size_t)y * dw + x) * 4];
								dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
							}
						rgba.swap(ds); pw = dw; ph = dh;
					}
					inspTexPreviewId = r->createTexture2D(rgba.data(), pw, ph);
				}
			}
	}

	ImGui::TextUnformatted(bfs::path(path).filename().string().c_str());
	ImGui::SameLine(); ImGui::TextDisabled("%s", ext.c_str());
	// Assets open their OWN editor window (module-supplied types included — e.g. .nutile
	// from NukeTilemapEditor); text-editable types additionally offer the text editor.
	const bool hasOwnEditor = ext == ".numat" || ext == ".numesh" || ext == ".nuprefab"
	                       || nuke::AssetEditorForExt(ext) != nullptr;
	if (hasOwnEditor)
	{
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - (IsTextFile(ext) ? 190.0f : 120.0f));
		if (ImGui::SmallButton(ICON_LC_PENCIL_RULER " Open in Editor")) OpenAssetEditor(path);
		if (IsTextFile(ext))
		{
			ImGui::SameLine();
			if (ImGui::SmallButton(ICON_LC_FILE_PEN " Edit")) OpenExternal(path, 0);
		}
	}
	else if (IsTextFile(ext))
	{
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
		if (ImGui::SmallButton(ICON_LC_FILE_PEN " Edit")) OpenExternal(path, 0);
	}
	ImGui::Separator();

	if (ext == ".nutex" && inspTex)
	{
		if (inspTex->renderTexture) { ImGui::TextDisabled("Render texture (%dx%d).", inspTex->width, inspTex->height); return; }
		// Image preview (fit to the panel width, aspect kept; mip0 / frame 0).
		if (inspTexPreviewId)
		{
			float w = ImGui::GetContentRegionAvail().x;
			if (w > 384.0f) w = 384.0f;
			float h = (inspTex->width > 0) ? w * (float)inspTex->height / (float)inspTex->width : w;
			ImVec2 imgTL = ImGui::GetCursorScreenPos();
			ImGui::Image((ImTextureID)inspTexPreviewId, ImVec2(w, h));
			// Eyedropper: while armed, clicking the preview samples that texel's colour into the chroma key.
			if (inspChromaPick && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && w > 0 && h > 0)
			{
				float u = (ImGui::GetIO().MousePos.x - imgTL.x) / w, v = (ImGui::GetIO().MousePos.y - imgTL.y) / h;
				std::vector<unsigned char> full = inspTex->DecodeRGBA();
				int tw = inspTex->width, th = inspTex->height;
				if (!full.empty() && tw > 0 && th > 0)
				{
					int px = (int)(u * tw); if (px < 0) px = 0; if (px >= tw) px = tw - 1;
					int py = (int)(v * th); if (py < 0) py = 0; if (py >= th) py = th - 1;
					const unsigned char* s = &full[((size_t)py * tw + px) * 4];
					inspChroma[0] = s[0] / 255.0f; inspChroma[1] = s[1] / 255.0f; inspChroma[2] = s[2] / 255.0f;
				}
				inspChromaPick = false;
			}
			if (inspChromaPick) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			ImGui::Spacing();
		}
		const char* fmt = inspTex->format == nuke::Texture::FMT_BC1 ? "BC1" : inspTex->format == nuke::Texture::FMT_BC3 ? "BC3"
		                : inspTex->format == nuke::Texture::FMT_BC5 ? "BC5" : "RGBA8";
		ImGui::Text("%d x %d   %s   %d mip(s)", inspTex->width, inspTex->height, fmt, inspTex->mipCount);
		if (inspTex->frameCount > 1) ImGui::Text("Animated: %d frames", inspTex->frameCount);
		ImGui::Spacing();
		const char* usages[] = { "Color (sRGB)", "Normal Map", "Data (linear)", "Emissive (sRGB)", "Sprite (sheet)" };
		int u = inspTex->usage;
		if (ImGui::Combo("Texture Type", &u, usages, IM_ARRAYSIZE(usages)) && u != inspTex->usage)
		{
			int before = inspTex->usage;
			auto setUsage = [this, path](int val) {
				if (inspAssetPath == path && inspTex) { inspTex->usage = (nuke::Texture::Usage)val; inspTex->SaveToFile(path); }
				else if (nuke::Texture* t = nuke::Texture::LoadFromFile(path)) { t->usage = (nuke::Texture::Usage)val; t->SaveToFile(path); delete t; }
			};
			setUsage(u);
			PushUndo("Texture type", [setUsage, before]{ setUsage(before); }, [setUsage, u]{ setUsage(u); }, false);   // asset edit, not the world
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("How the engine treats this texture (color space / compression / normal handling).\nAuto-guessed from the filename at import; override here.\nNormal -> BC5 + green flip (re-import to re-compress after changing to/from Normal).");

		if (inspTex->usage == nuke::Texture::UsageNormal)   // green convention (only meaningful for normal maps)
		{
			bool ig = inspTex->invertGreen;
			if (ImGui::Checkbox("Invert Green (OpenGL +Y)", &ig) && ig != inspTex->invertGreen)
			{
				bool before = inspTex->invertGreen;
				auto setIG = [this, path](bool val) {
					std::string guid;
					if (inspAssetPath == path && inspTex) { inspTex->invertGreen = val; inspTex->SaveToFile(path); guid = inspTex->guid; }
					else if (nuke::Texture* t = nuke::Texture::LoadFromFile(path)) { t->invertGreen = val; t->SaveToFile(path); guid = t->guid; delete t; }
					// live: update the ResDB-registered copy the renderer reads (green flip applies next frame, no reload)
					if (!guid.empty()) if (nuke::Texture* live = nuke::ResDB::getSingleton()->GetTexture(guid)) live->invertGreen = val;
				};
				setIG(ig);
				PushUndo("Normal green convention", [setIG, before]{ setIG(before); }, [setIG, ig]{ setIG(ig); }, false);   // asset edit, not the world
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("On = OpenGL convention (+Y up, green flipped) — glTF/Blender/Substance default.\nOff = DirectX (-Y). Toggle if the relief looks inverted.");
		}

		// Compression override — re-compresses the .nutex in place (quality vs size). Normals default to BC5 (8bpp);
		// BC1 halves the size but is blocky. Applied immediately (decode -> re-encode) + live in the renderer.
		if (inspTex->usage == nuke::Texture::UsageSprite)   // grid/margin/spacing/9-slice live in the dedicated slicer
		{
			if (ImGui::Button(ICON_LC_GRID_2X2 " Open Sprite Slicer", ImVec2(-FLT_MIN, 0))) OpenAssetEditor(path);
			ImGui::TextDisabled("Grid %dx%d  ·  margin %d/%d/%d/%d  ·  spacing %d,%d", inspTex->spriteColumns, inspTex->spriteRows,
				inspTex->spriteMarginLeft, inspTex->spriteMarginRight, inspTex->spriteMarginTop, inspTex->spriteMarginBottom,
				inspTex->spriteSpacingX, inspTex->spriteSpacingY);
			ImGui::TextDisabled("Slice the sheet visually (rulers, animation preview, 9-slice) in the slicer.");
		}

		if (inspTex->format != nuke::Texture::FMT_RGBA8)
		{
			bool isNormal = (inspTex->usage == nuke::Texture::UsageNormal);
			const char* opts[2]; int vals[2];
			if (isNormal) { opts[0] = "BC5 (quality)"; opts[1] = "BC1 (small)";  vals[0] = nuke::Texture::FMT_BC5; vals[1] = nuke::Texture::FMT_BC1; }
			else          { opts[0] = "BC1 (opaque)";  opts[1] = "BC3 (alpha)";  vals[0] = nuke::Texture::FMT_BC1; vals[1] = nuke::Texture::FMT_BC3; }
			int cur = (inspTex->format == vals[1]) ? 1 : 0;
			if (ImGui::Combo("Compression", &cur, opts, 2) && inspTex->format != vals[cur])
			{
				int before = inspTex->format, after = vals[cur];
				auto applyFmt = [this, path](int f) {
					bool owned = false;
					nuke::Texture* t = (inspAssetPath == path && inspTex) ? inspTex : nullptr;
					if (!t) { t = nuke::Texture::LoadFromFile(path); owned = true; }
					if (!t) return;
					if (t->Recompress(f)) t->SaveToFile(path);
					if (nuke::Texture* live = nuke::ResDB::getSingleton()->GetTexture(t->guid))
					{
						if (live != t) { live->format = t->format; live->mipCount = t->mipCount; live->pixels = t->pixels; }
						if (iRender* r = AppInstance::GetSingleton()->render) r->invalidateTexture(live);   // renderer re-uploads
					}
					if (owned) delete t;
				};
				applyFmt(after);
				PushUndo("Texture compression", [applyFmt, before]{ applyFmt(before); }, [applyFmt, after]{ applyFmt(after); }, false);   // asset edit, not the world
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Re-compress now. BC5 = 8 bpp (normals, quality); BC1 = 4 bpp (half the size, blocky). BC1<->BC5 is lossy.");
		}

		// Chroma key: knock out a flat background colour (sprites shot on green/magenta) -> transparent alpha.
		if (ImGui::CollapsingHeader("Chroma Key"))
		{
			ImGui::SetNextItemWidth(120);
			ImGui::ColorEdit3("##chroma", inspChroma, ImGuiColorEditFlags_NoInputs);
			ImGui::SameLine();
			if (ImGui::Button(inspChromaPick ? "Picking\xE2\x80\xA6 (click image)" : "Pick")) inspChromaPick = !inspChromaPick;
			ImGui::SetNextItemWidth(160); ImGui::SliderInt("Tolerance", &inspChromaTol, 0, 128);
			ImGui::Checkbox("Outside only (keep enclosed areas)", &inspChromaOutside);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("On: clears only the background touching the image edge (white eyes etc. stay).\nOff: clears every matching pixel.");
			if (ImGui::Button("Apply Chroma Key") && inspTex)
			{
				// Writes pixels/format/mip to the file + the live ResDB copy, then re-uploads. Used by undo/redo too.
				auto setBlob = [this, path](std::shared_ptr<std::vector<unsigned char>> px, int fmt, int mip) {
					bool owned = false;
					nuke::Texture* t = (inspAssetPath == path && inspTex) ? inspTex : nullptr;
					if (!t) { t = nuke::Texture::LoadFromFile(path); owned = true; }
					if (!t) return;
					t->pixels = *px; t->format = fmt; t->mipCount = mip; t->SaveToFile(path);
					if (nuke::Texture* live = nuke::ResDB::getSingleton()->GetTexture(t->guid)) {
						if (live != t) { live->pixels = *px; live->format = fmt; live->mipCount = mip; }
						if (iRender* r = AppInstance::GetSingleton()->render) r->invalidateTexture(live);
					}
					if (owned) delete t;
				};
				auto before = std::make_shared<std::vector<unsigned char>>(inspTex->pixels);
				int bfmt = inspTex->format, bmip = inspTex->mipCount;
				int kr = (int)(inspChroma[0] * 255 + 0.5f), kg = (int)(inspChroma[1] * 255 + 0.5f), kb = (int)(inspChroma[2] * 255 + 0.5f);
				if (inspTex->ApplyChromaKey(kr, kg, kb, inspChromaTol, inspChromaOutside))
				{
					inspTex->SaveToFile(path);
					auto after = std::make_shared<std::vector<unsigned char>>(inspTex->pixels);
					int afmt = inspTex->format, amip = inspTex->mipCount;
					if (nuke::Texture* live = nuke::ResDB::getSingleton()->GetTexture(inspTex->guid)) {
						if (live != inspTex) { live->pixels = inspTex->pixels; live->format = afmt; live->mipCount = amip; }
						if (iRender* r = AppInstance::GetSingleton()->render) r->invalidateTexture(live);
					}
					PushUndo("Chroma key", [setBlob, before, bfmt, bmip]{ setBlob(before, bfmt, bmip); },
					                       [setBlob, after, afmt, amip]{ setBlob(after, afmt, amip); }, false);
				}
			}
			ImGui::TextDisabled("Pick the background colour (or eyedrop the preview), then Apply.\nBC textures re-encode to BC3 to carry alpha.");
		}
	}
	else if (ext == ".numat" && inspMat)
	{
		// Live 3D preview on a sphere (edits below re-stage through the mtime refresh).
		DrawAssetPreview3D(path, ext);
		if (nuke::TypeInfo* ti = inspMat->GetType())
			if (DrawFields(inspMat, ti))
			{
				inspMat->SaveToFile(path);   // reflected material fields; save on edit
				pvStaged.clear();            // re-resolve the preview instance from the saved asset
			}
	}
	else if (ext == ".numesh")
	{
		DrawAssetPreview3D(path, ext);
		if (inspPv && inspPv->mr->mesh && pvStaged == path)
		{
			nuke::Mesh* m = inspPv->mr->mesh;
			ImGui::Text("%d vertices   %d triangles", m->numVerts, m->numVerts / 3);
			m->EnsureBounds();
			ImGui::Text("Bounds: %.2f x %.2f x %.2f",
				m->aabbMax[0] - m->aabbMin[0], m->aabbMax[1] - m->aabbMin[1], m->aabbMax[2] - m->aabbMin[2]);
		}
		else ImGui::TextDisabled("Mesh asset (not in the resource DB).");
	}
	else
	{
		boost::system::error_code ec;
		uintmax_t sz = bfs::file_size(bfs::path(path), ec);
		if (!ec) ImGui::Text("Size: %.1f KB", (double)sz / 1024.0);
		if (ext == ".nuworld")  { if (ImGui::Button(ICON_LC_GLOBE " Open World", ImVec2(-FLT_MIN, 0)))  OpenWorldFromBrowser(path); }
		else if (ext == ".nuprefab")
		{
			ImGui::TextDisabled("Prefab — drag into the world to instantiate.");
			if (ImGui::Button(ICON_LC_PACKAGE_PLUS " Instantiate", ImVec2(-FLT_MIN, 0))) SpawnPrefab(path);
		}
		else if (ext == ".nuproj")
		{
			ImGui::TextDisabled("Project descriptor.");
			if (ImGui::Button(ICON_LC_SETTINGS " Open Project Settings", ImVec2(-FLT_MIN, 0))) settingsOpen = true;
		}
		else if (!IsTextFile(ext)) ImGui::TextDisabled("No editable properties.");
	}
}
