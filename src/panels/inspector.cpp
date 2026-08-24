// inspector panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>
#include "editor/animshared.h"
#include "nukeui.h"   // DocPanel: detachable panels
#include <API/Model/Camera.h>
#include <API/Model/CharacterController.h>
#include <API/Model/Foliage.h>
#include <API/Model/Surface.h>
#include <API/Model/StatusBar.h>
#include <API/Model/Package.h>  // pickers list pak/mod content too
#include <interface/Services.h> // csclass picker: scripting providers
#include <service/iScript.h>
#include <reflect/ReflectBind.h>   // multi-edit: per-field mirror across the selection
#include <set>
#include <memory>
#include <cmath>
#include <API/Model/Texture.h>
#include <API/Model/Material.h>
#include <API/Model/Light.h>
#include <API/Model/Environment.h>
#include <API/Model/Layers.h>
#include <API/Model/Canvas.h>
#include <API/Model/RectAnchor.h>
#include <API/Model/Sprite.h>
#include <functional>
#include <interface/AssetCreators.h>   // module-supplied asset editors
#include <boost/filesystem.hpp>
namespace bfs = boost::filesystem;

// Asset 3D preview — stages the browser's selection into a pooled preview scene (pool: asseteditor.cpp).

void EditorUI::StageAssetPreview(const std::string& path, const std::string& ext)
{
	if (!inspPv) inspPv = AcquirePreview();
	if (!inspPv) return;
	// The pooled scene is re-staged in place across selections — reset per-type modes.
	inspPv->locked = false;
	if (inspPv->world) inspPv->world->editorGrid = true;
	ResDB* db = ResDB::getSingleton();
	const std::string guid = db->GuidForPath(path);

	if (ext == ".numesh")
	{
		inspPv->mr->meshGuid = guid;
		inspPv->mr->mesh = db->GetMesh(guid);
		EditorApplyMeshMaterials(inspPv->mr, guid);   // the mesh's OWN materials, not a white default
	}
	else if (ext == ".numat")
	{
		// Material preview is a STATIC shot: no grid through the sample, no camera input.
		inspPv->locked = true;
		inspPv->world->editorGrid = false;
		inspPv->mr->meshGuid = "builtin:sphere";
		inspPv->mr->mesh = db->GetMesh("builtin:sphere");
		inspPv->mr->matGuid = guid;
		// The renderer only clones matGuid -> mat inside Init(); this component is long past
		// it, so a bare guid left `mat` null and the preview drew the DEFAULT white material.
		if (inspPv->mr->mat) { delete inspPv->mr->mat; inspPv->mr->mat = nullptr; }
		if (Material* asset = db->GetMaterial(guid)) inspPv->mr->mat = asset->Clone();
	}
	FramePreview(*inspPv, nullptr);
	pvStaged = path;
}

void EditorUI::DrawAssetPreview3D(const std::string& path, const std::string& ext)
{
	if (pvStaged != path) StageAssetPreview(path, ext);
	if (!inspPv || !inspPv->mr || !inspPv->mr->mesh) return;
	const float side = ImGui::GetContentRegionAvail().x;   // full panel width, always
	DrawPreviewImage(*inspPv, ImVec2(side, side));
}

// Must run BEFORE the live scene renders: the live scene then re-pushes its lights/sky/TLAS.
void EditorUI::RenderAssetPreview(iRender* r)
{
	if (!r) return;
	for (PreviewWorld* s : pvPool)
	{
		if (s->inUse && s->visible && s->world)
		{
			// Preview worlds are auxiliary — World::Render only pumps the live world, so drive
			// this one here: tweens tick, hits spawn/expire AND liveFoliage grows on the sample.
			nuke::Surface::DriveFoliage(s->world);
			s->world->FlushDestroyQueue();   // hit spawns actually die (no Update() for previews)
			s->world->Render(r);
		}
		s->visible = false;
	}
}

// Draws an int layer bitmask as an Everything/Nothing/per-layer dropdown. Returns true if changed.
static bool DrawLayerMaskCombo(const char* id, int& mask)
{
	bool changed = false;
	unsigned int m = (unsigned int)mask;
	std::string shown = (m == 0xFFFFFFFFu) ? "Everything" : (m == 0 ? "Nothing" : "Mixed");
	if (m != 0xFFFFFFFFu && m != 0)
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
// Audio clips are plain content files (no wrapper asset), referenced by content-relative path.
static bool IsAudioExt(std::string e)
{
	for (char& c : e) c = (char)tolower((unsigned char)c);
	return e == ".ogg" || e == ".wav" || e == ".mp3" || e == ".flac";
}
// `asset="file:<ext>"` kinds: generic by-extension content-file picker; value = content-relative path.
static bool IsFileKind(const std::string& kind) { return kind.rfind("file:", 0) == 0; }
static std::string FileKindExt(const std::string& kind)
{
	std::string e = kind.substr(5);
	for (char& c : e) c = (char)tolower((unsigned char)c);
	return e;
}
// The file extension an asset KIND (the reflection hint) is stored as; "" when the kind is not
// a single file type. Feeds both drop-matching and the picker's icons.
static std::string KindExt(const std::string& kind)
{
	if (IsFileKind(kind))   return FileKindExt(kind);
	if (kind == "mesh")     return ".numesh";
	if (kind == "material") return ".numat";
	if (kind == "texture")  return ".nutex";
	if (kind == "anim")     return ".nuanim";
	if (kind == "bonemap")  return ".nubonemap";
	if (kind == "skeleton") return ".nuskel";
	if (kind == "animsm")   return ".nusm";
	if (kind == "blendspace") return ".nublend";
	if (kind == "sequence") return ".nuseq";
	if (kind == "ragdoll")  return ".nurag";
	if (kind == "prefab")   return ".nuprefab";
	if (kind == "world")    return ".nuworld";
	if (kind == "script")   return ".lua";
	if (kind == "shader" || kind == "postshader") return ".hlsl";
	return "";
}

// True if a dropped file path matches the field's asset kind.
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
	if (kind == "skeleton") return e == ".nuskel";
	if (kind == "animsm")   return e == ".nusm";
	if (kind == "blendspace") return e == ".nublend";
	if (kind == "sequence") return e == ".nuseq";
	if (kind == "ragdoll")  return e == ".nurag";
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

	// Display name = the asset's FILE name so it tracks renames; internal name for file-less built-ins.
	auto disp = [&](const std::string& g) -> std::string {
		if (g.empty()) return "(none)";
		if (kind == "script" || kind == "audio" || IsFileKind(kind)) return bfs::path(g).stem().string();   // value is a content-relative path
		if (kind == "shader" || kind == "postshader") { Shader* s = db->GetShader(g); return s ? s->name : g; }
		std::string p = db->PathForGuid(g);
		if (!p.empty()) return bfs::path(p).stem().string();
		// Never return an empty label: an empty Selectable renders as an invisible row.
		std::string n;
		if      (kind == "mesh")     { Mesh* m = db->GetMesh(g);         if (m) n = m->name; }
		else if (kind == "material") { Material* m = db->GetMaterial(g); if (m) n = m->matName; }
		else if (kind == "anim")     { AnimClip* c = db->GetClip(g);     if (c) n = c->name; }
		else if (kind == "bonemap")  { BoneMap* b = db->GetBoneMap(g);   if (b) n = b->name; }
		else if (kind == "skeleton") { Skeleton* sk = db->GetSkeleton(g); if (sk) n = sk->name; }
		else if (kind == "animsm")   { AnimSM* m = db->GetAnimSM(g);     if (m) n = m->name; }
		else if (kind == "blendspace") { BlendSpace* b = db->GetBlendSpace(g); if (b) n = b->name; }
		else if (kind == "sequence") { Sequence* q = db->GetSequence(g); if (q) n = q->name; }
		else if (kind == "ragdoll")  { RagdollDef* r = db->GetRagdoll(g); if (r) n = r->name; }
		else if (kind == "texture")  { Texture* t = db->GetTexture(g);   if (t) n = t->name; }
		return n.empty() ? g : n;
	};

	ImGui::PushID(label);
	float full = ImGui::CalcItemWidth();
	// The value button + the two icon buttons must fit EXACTLY in the item width — a fixed
	// reserve under-counted the icons' frame padding and the row spilled past the panel edge.
	const ImGuiStyle& ast = ImGui::GetStyle();
	const float icoW = ImGui::CalcTextSize(ICON_LC_FOLDER_SEARCH).x + ast.FramePadding.x * 2.0f
	                 + ImGui::CalcTextSize(ICON_LC_ROTATE_CCW).x   + ast.FramePadding.x * 2.0f + 4.0f;
	std::string cur = disp(guid);
	if (ImGui::Button((cur + "##cur").c_str(), ImVec2(std::max(40.0f, full - icoW), 0))) { assetFilter[0] = 0; ImGui::OpenPopup("##assetpop"); }

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
		if (!path.empty())
		{
			// Navigate in the browser's own path form (native separators): its selection compares
			// paths as strings against directory-iterator output, so a mixed-separator path from an
			// asset value would highlight nothing. The draw resolves browserLocate by filesystem
			// equivalence and scrolls the row into view.
			bfs::path bp = bfs::path(path).make_preferred();
			BrowserNavigate(bp.parent_path().string());
			BrowserSelect(bp.string());
			browserLocate = bp.string();
			if (win) win->browser = true;
			NukeUI::DocFocus("panel:browser");
		}
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Go to file");
	ImGui::SameLine(0, 2);
	if (ImGui::Button(ICON_LC_ROTATE_CCW "##rst")) { guid = defGuid; changed = true; }   // reset to default
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset to default");
	// Skip hidden ids: "##..." would print literally here.
	if (label && label[0] && !(label[0] == '#' && label[1] == '#')) { ImGui::SameLine(0, 6); ImGui::TextUnformatted(label); }

	if (ImGui::BeginPopup("##assetpop"))
	{
		ImGui::SetNextItemWidth(260);
		ImGui::InputTextWithHint("##flt", ICON_LC_SEARCH " Filter", assetFilter, sizeof(assetFilter));
		std::string flt = assetFilter;
		ImGui::Separator();
		ImGui::BeginChild("##lst", ImVec2(280, 260));
		// The row's icon comes from the type registry, never from a switch here: a value that IS
		// a path answers by its own extension, otherwise the kind's extension does.
		const std::string kindExt = KindExt(kind);
		auto rowIcon = [&](const std::string& value) -> const char*
		{
			const std::string e = bfs::path(value).extension().string();
			return ExtIcon(e.empty() ? kindExt : e);
		};
		auto item = [&](const std::string& g, const std::string& n) {
			if (!ciContains(n, flt)) return;
			// "##<guid>" keeps the visible text but gives same-named assets distinct IDs (no ID clash).
			const std::string row = std::string(rowIcon(g)) + "  " + n + "##" + g;
			if (ImGui::Selectable(row.c_str(), g == guid)) { guid = g; changed = true; ImGui::CloseCurrentPopup(); }
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
		else if (kind == "skeleton") for (Skeleton* sk : db->skeletons) { if (sk) item(sk->guid, disp(sk->guid)); }
		else if (kind == "animsm")   for (AnimSM* m : db->animSMs)   { if (m) item(m->guid, disp(m->guid)); }
		else if (kind == "blendspace") for (BlendSpace* b : db->blendSpaces) { if (b) item(b->guid, disp(b->guid)); }
		else if (kind == "sequence") for (Sequence* q : db->sequences) { if (q) item(q->guid, disp(q->guid)); }
		else if (kind == "ragdoll")  for (RagdollDef* r : db->ragdolls) { if (r) item(r->guid, disp(r->guid)); }
		else if (kind == "csclass")
		{
			// Electron classes come from the loaded game assembly via the scripting seam.
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
					if (!cls.empty())
					{
						const char* ic = sv->Icon();
						const std::string row = std::string(*ic ? ic : ICON_FT_DEFAULT) + "  " + cls + "##" + cls;
						if (!ciContains(cls, flt)) { start = nl + 1; continue; }
						if (ImGui::Selectable(row.c_str(), cls == guid))
						{ guid = cls; changed = true; ImGui::CloseCurrentPopup(); }
						any = true;
					}
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
			// Packed session: content files live in mounted paks, not on disk (dedup vs the disk scan).
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

// Fills inspectorOverrides: extra type-specific UI drawn on top of the generic reflected fields.
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
		if (ImGui::Button("Fit To Mesh", ImVec2(-FLT_MIN, 0)))
		{
			if (cc->FitToMesh()) worldDirty = true;
			else StatusBar::Set("cc.fit", "Fit To Mesh: no MeshRenderer with a mesh on this atom");
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Set Pivot/Capsule Offset/Height/Radius from the sibling mesh's bounds.");
	};
	inspectorOverrides["SurfaceMask"] = [this](nuke::Component* c) {
		auto* m = static_cast<nuke::SurfaceMask*>(c);
		bool paint = maskBrush == 1, erase = maskBrush == 2;
		if (ImGui::Checkbox("Paint##mask", &paint)) maskBrush = paint ? 1 : 0;
		ImGui::SameLine();
		if (ImGui::Checkbox("Erase##mask", &erase)) maskBrush = erase ? 2 : 0;
		ImGui::SameLine();
		if (ImGui::Button("Clear Mask")) { m->Clear(); worldDirty = true; }
		if (maskBrush != 0)
		{
			// Channel labels come from the mask's own state bindings.
			const std::string chLbl[4] = {
				"R: " + (m->state0.empty() ? std::string("(unbound)") : m->state0),
				"G: " + (m->state1.empty() ? std::string("(unbound)") : m->state1),
				"B: " + (m->state2.empty() ? std::string("(unbound)") : m->state2),
				"A: " + (m->state3.empty() ? std::string("(unbound)") : m->state3) };
			if (maskBrushChannel < 0 || maskBrushChannel > 3) maskBrushChannel = 0;
			if (ImGui::BeginCombo("Channel", chLbl[maskBrushChannel].c_str()))
			{
				for (int i = 0; i < 4; ++i)
					if (ImGui::Selectable(chLbl[i].c_str(), i == maskBrushChannel)) maskBrushChannel = i;
				ImGui::EndCombo();
			}
			ImGui::DragFloat("Brush Radius", &maskBrushRadius, 0.05f, 0.1f, 50.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
			ImGui::DragFloat("Brush Strength", &maskBrushStrength, 0.05f, 0.1f, 10.0f, "%.2f /s", ImGuiSliderFlags_AlwaysClamp);
			ImGui::TextDisabled("LMB in the viewport: %s", maskBrush == 1 ? "paint the condition" : "erase");
		}
	};
	inspectorOverrides["Foliage"] = [this](nuke::Component* c) {
		auto* fol = static_cast<nuke::Foliage*>(c);
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
		bool paint = foliageBrush == 1, erase = foliageBrush == 2;
		if (ImGui::Checkbox("Paint", &paint)) foliageBrush = paint ? 1 : 0;
		ImGui::SameLine();
		if (ImGui::Checkbox("Erase", &erase)) foliageBrush = erase ? 2 : 0;
		ImGui::SameLine();
		ImGui::TextDisabled("(%d)", fol->InstanceCount());
		if (foliageBrush != 0)
		{
			ImGui::DragFloat("Brush Radius", &foliageBrushRadius, 0.1f, 0.25f, 100.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
			if (foliageBrush == 1) ImGui::DragFloat("Brush Density", &foliageBrushDensity, 0.02f, 0.1f, 16.0f, "x%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::TextDisabled("LMB in the viewport: %s", foliageBrush == 1 ? "paint" : "erase");
		}
	};
}

// State-machine editor (entry, states, transitions). Edits go through the component's mutators
// so the serialized smJson stays in sync.
void EditorUI::DrawAnimatorInspector(nuke::Animator* an)
{
	an->EnsureSM();
	ImGui::SeparatorText("State Machine");

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

// Ordered chain of post-effect shaders: pick, params, reorder, add/remove. Commit()s back to the
// serialized field.
void EditorUI::DrawPostProcessInspector(nuke::PostProcess* pp)
{
	ResDB* db = ResDB::getSingleton();
	pp->EnsureParsed();
	ImGui::SeparatorText("Post Effects (run in order — drag to reorder)");

	int removeAt = -1, dragFrom = -1, dragTo = -1;
	const ImGuiPayload* dpl = ImGui::GetDragDropPayload();
	bool dndActive = dpl && dpl->IsDataType("PP_FX");
	// "Insert before index N" zone straddling a row's top edge; restores the cursor so it adds no height.
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
					if (sp.name.compare(0, 6, "g_Nuke") == 0) continue;   // system params, engine-filled
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
	// Tail zone: a drop below the last effect moves to the END of the chain.
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
	// Add/remove/reorder are undoable as one atom-subtree delta: the active-widget edit detector
	// cannot see button/DnD clicks (it already covers the param drags).
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
		editing = false; editAtomId = 0;   // own command: suppress the auto edit-detector
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

// Draws the selected material's sub-properties (shader/color/textures); the mesh/material GUID
// fields themselves come from reflection.
void EditorUI::DrawMeshRendererInspector(nuke::MeshRenderer* mr)
{
	ResDB* db = ResDB::getSingleton();
	if (!mr->mesh || mr->mesh->guid != mr->meshGuid) mr->mesh = db->GetMesh(mr->meshGuid);
	// A changed picker re-clones the asset into an OWNED instance, so edits never touch the .numat.
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
		if (DrawFields(m, m->GetType()))
			m->Resolve();   // rebind shader/textures after an edit

		// Schema comes from the shader, values live on the instance (m->props); unset shows the
		// default. Props declared in #include'd files (matcb_std) are engine plumbing — hidden.
		bool anyCustom = false;
		if (m->shader)
			for (const nuke::ShaderProp& sp : m->shader->props)
				if (!m->shader->FromInclude(sp.name)) { anyCustom = true; break; }
		if (m->shader && anyCustom)
		{
			ImGui::SeparatorText("Shader Params");
			for (const nuke::ShaderProp& sp : m->shader->props)
			{
				if (m->shader->FromInclude(sp.name)) continue;   // std MatCB block, engine-driven
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
// Cross-editor timeline sync: while a key/stop is dragged in ANY curve or gradient editor,
// every editor drawn the same frame shows a dashed vertical line at that t (aligning keys
// across curves), and Ctrl snaps the dragged time to OTHER editors' key times collected
// last frame (never to the dragged curve's own keys).
static int    g_cvFrame     = -1;
static float  g_cvDragT     = -1.0f;   // written by the dragging editor this frame
static float  g_cvDragTDraw = -1.0f;   // last frame's value — what everyone draws
static std::vector<std::pair<ImGuiID, float>> g_cvTimes, g_cvTimesDraw;   // (owner widget, t)
static void CurveSyncFrame()
{
	if (ImGui::GetFrameCount() == g_cvFrame) return;
	g_cvFrame = ImGui::GetFrameCount();
	g_cvDragTDraw = g_cvDragT; g_cvDragT = -1.0f;
	g_cvTimesDraw.swap(g_cvTimes); g_cvTimes.clear();
}

// THE gradient widget: (t, r, g, b[, a]) stops drawn as a bar with draggable markers —
// inspector [[prop(widget="gradient")]] lists (rgb) and color tweens (rgba) both draw this.
bool EditorUI::GradientStopsEditor(std::vector<float>& stops, bool alpha,
                                   std::vector<float>* trigT, std::vector<std::string>* trigEv,
                                   const std::vector<std::string>* trigOptions)
{
	std::vector<float>* g4 = &stops;
	const int str = alpha ? 5 : 4;   // floats per stop
	bool changed = false;
	ImGui::PushID(alpha ? "##gradrgba" : "##gradrgb");
	CurveSyncFrame();
	const ImGuiID cvSelf = ImGui::GetID("##cvowner");   // this editor's identity in the sync pool
	size_t nStops = g4->size() / str;
	const float wpx = ImGui::CalcItemWidth(), bh = 22.0f, mh = 16.0f;
	ImVec2 p0 = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##gradbar", ImVec2(wpx, bh + mh));
	const bool hov = ImGui::IsItemHovered();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	auto stopCol = [&](size_t i) {
		return IM_COL32((int)((*g4)[i * str + 1] * 255.f), (int)((*g4)[i * str + 2] * 255.f), (int)((*g4)[i * str + 3] * 255.f), 255);
	};
	// interpolated color at t — what a NEW stop inherits, so adding one is seamless
	auto evalCol = [&](float t, float out[4]) {
		out[0] = out[1] = out[2] = out[3] = 1.f;
		const size_t ns = g4->size() / str; if (!ns) return;
		if (t <= (*g4)[0]) { for (int q = 0; q < str - 1; ++q) out[q] = (*g4)[1 + q]; return; }
		for (size_t i = 1; i < ns; ++i)
			if (t <= (*g4)[i * str])
			{
				float t0 = (*g4)[(i - 1) * str], t1 = (*g4)[i * str];
				float fq = (t1 - t0) > 1e-6f ? (t - t0) / (t1 - t0) : 0.f;
				for (int q = 0; q < str - 1; ++q) out[q] = (*g4)[(i - 1) * str + 1 + q] + ((*g4)[i * str + 1 + q] - (*g4)[(i - 1) * str + 1 + q]) * fq;
				return;
			}
		for (int q = 0; q < str - 1; ++q) out[q] = (*g4)[(ns - 1) * str + 1 + q];
	};
	if (nStops == 0)
		dl->AddRectFilled(p0, ImVec2(p0.x + wpx, p0.y + bh), IM_COL32(255, 255, 255, 255));
	else
	{
		dl->AddRectFilled(p0, ImVec2(p0.x + (*g4)[0] * wpx, p0.y + bh), stopCol(0));
		for (size_t k = 1; k < nStops; ++k)
		{
			float xa = p0.x + (*g4)[(k - 1) * str] * wpx, xb = p0.x + (*g4)[k * str] * wpx;
			dl->AddRectFilledMultiColor(ImVec2(xa, p0.y), ImVec2(xb, p0.y + bh),
			                            stopCol(k - 1), stopCol(k), stopCol(k), stopCol(k - 1));
		}
		dl->AddRectFilled(ImVec2(p0.x + (*g4)[(nStops - 1) * str] * wpx, p0.y), ImVec2(p0.x + wpx, p0.y + bh), stopCol(nStops - 1));
	}
	dl->AddRect(p0, ImVec2(p0.x + wpx, p0.y + bh), IM_COL32(90, 90, 90, 255));
	if (trigT)   // trigger flags sit ON their stops
		for (float kt : *trigT)
		{
			const float lx = p0.x + std::max(0.0f, std::min(1.0f, kt)) * wpx;
			dl->AddTriangleFilled(ImVec2(lx - 4.0f, p0.y + 1.0f), ImVec2(lx + 4.0f, p0.y + 1.0f),
			                      ImVec2(lx, p0.y + 9.0f), IM_COL32(255, 160, 40, 230));
		}
	if (g_cvDragTDraw >= 0.0f)   // alignment line while any timeline key is dragged
	{
		const float lx = p0.x + g_cvDragTDraw * wpx;
		for (float y = p0.y; y < p0.y + bh; y += 8.0f)
			dl->AddLine(ImVec2(lx, y), ImVec2(lx, std::min(y + 4.0f, p0.y + bh)), IM_COL32(255, 200, 80, 150), 1.0f);
	}
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
		if (hov && fabsf(mp.x - (p0.x + (*g4)[k * str] * wpx)) < 6.0f && mp.y > p0.y + bh - 4.0f) hot = k;
	for (int k = 0; k < (int)nStops; ++k)
	{
		float mx = p0.x + (*g4)[k * str] * wpx;
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
			float col[4]; evalCol(t, col);
			g4->push_back(t);
			for (int q = 0; q < str - 1; ++q) g4->push_back(col[q]);
			changed = true;
		}
	}
	if (dragStop >= 0 && dragStop < (int)nStops && ImGui::IsMouseDown(0))
	{
		float t = std::max(0.f, std::min(1.f, (mp.x - p0.x) / wpx));
		if (ImGui::GetIO().KeyCtrl)   // Ctrl = snap to OTHER editors' key times
		{
			float best = t, bd = 1e9f;
			for (const auto& tt : g_cvTimesDraw)
				if (tt.first != cvSelf)
				{ const float d = fabsf(tt.second - t); if (d < bd) { bd = d; best = tt.second; } }
			if (bd < 8.0f / wpx) t = best;
		}
		const float oldStopT = (*g4)[dragStop * str];
		if (fabsf(t - oldStopT) > 1e-4f)
		{
			st->SetInt(movedId, 1); (*g4)[dragStop * str] = t; changed = true;
			if (trigT)   // a trigger sits ON its stop: it moves with it
				for (float& kt : *trigT)
					if (fabsf(kt - oldStopT) < 1e-4f) kt = t;
		}
		g_cvDragT = t;
	}
	if (ImGui::IsMouseReleased(0) && dragStop >= 0)
	{
		if (!st->GetInt(movedId, 0) && dragStop < (int)nStops)   // clean click = edit color
		{ st->SetInt(editId, dragStop); ImGui::OpenPopup("##gradstop"); }
		st->SetInt(dragId, -1); dragStop = -1;
	}
	if (hov && ImGui::IsMouseClicked(1) && hot >= 0)   // right-click a marker = delete
	{
		const float delStopT = (*g4)[hot * str];
		if (trigT && trigEv)
			for (size_t q = trigT->size(); q-- > 0;)
				if (fabsf((*trigT)[q] - delStopT) < 1e-4f)
				{
					trigT->erase(trigT->begin() + q);
					if (q < trigEv->size()) trigEv->erase(trigEv->begin() + q);
					changed = true;
				}
		g4->erase(g4->begin() + hot * str, g4->begin() + hot * str + str);
		nStops = g4->size() / str;
		st->SetInt(dragId, -1);
		changed = true;
	}
	if (ImGui::BeginPopup("##gradstop"))
	{
		int es = st->GetInt(editId, -1);
		if (es >= 0 && es < (int)(g4->size() / str))
		{
			float col[4] = { (*g4)[es * str + 1], (*g4)[es * str + 2], (*g4)[es * str + 3],
			                 alpha ? (*g4)[es * str + 4] : 1.f };
			const bool ce = alpha ? ImGui::ColorPicker4("##sc", col, ImGuiColorEditFlags_NoSidePreview)
			                      : ImGui::ColorPicker3("##sc", col, ImGuiColorEditFlags_NoSidePreview);
			if (ce) { for (int q = 0; q < str - 1; ++q) (*g4)[es * str + 1 + q] = col[q]; changed = true; }
			ImGui::Text("t = %.2f", (*g4)[es * str]);
			if (trigT && trigEv && trigOptions && !trigOptions->empty())
			{
				const float kt = (*g4)[es * str];
				int ti = -1;
				for (int q = 0; q < (int)trigT->size(); ++q)
					if (fabsf((*trigT)[q] - kt) < 1e-4f) { ti = q; break; }
				if (ti >= 0 && ti >= (int)trigEv->size()) trigEv->resize(ti + 1);
				ImGui::SetNextItemWidth(160);
				if (ImGui::BeginCombo("##stoptrig", ti >= 0 ? (*trigEv)[ti].c_str() : "(no trigger)"))
				{
					if (ImGui::Selectable("(no trigger)", ti < 0) && ti >= 0)
					{
						trigT->erase(trigT->begin() + ti);
						trigEv->erase(trigEv->begin() + ti);
						changed = true;
					}
					for (const std::string& en : *trigOptions)
						if (ImGui::Selectable(en.c_str(), ti >= 0 && (*trigEv)[ti] == en))
						{
							if (ti >= 0) (*trigEv)[ti] = en;
							else { trigT->push_back(kt); trigEv->push_back(en); }
							changed = true;
						}
					ImGui::EndCombo();
				}
			}
			ImGui::SameLine();
			if (ImGui::SmallButton(ICON_LC_X " Delete"))
			{
				g4->erase(g4->begin() + es * str, g4->begin() + es * str + str);
				changed = true; ImGui::CloseCurrentPopup();
			}
		}
		else ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	// Sort stops by t every frame nothing is dragged; the color popup only edits the color,
	// so indices stay stable while it is open.
	if (st->GetInt(dragId, -1) < 0)
	{
		bool reordered = false;
		for (size_t k = str; k + (str - 1) < g4->size(); k += str)
			for (size_t j = k; j >= (size_t)str && (*g4)[j] < (*g4)[j - str]; j -= str, reordered = true)
				for (int q = 0; q < str; ++q) std::swap((*g4)[j + q], (*g4)[j - str + q]);
		if (reordered) changed = true;   // sorted order must reach the owner/undo
	}
	for (int k = 0; k < (int)(g4->size() / str); ++k)
		g_cvTimes.push_back({ cvSelf, (*g4)[k * str] });
	ImGui::PopID();
	return changed;
}

bool EditorUI::CurveKeysEditor(std::vector<float>& keys, float vLo, float vHi, const char* emptyText,
                               std::vector<float>* trigT, std::vector<std::string>* trigEv,
                               const std::vector<std::string>* trigOptions)
{
	std::vector<float>* c = &keys;
	bool changed = false;
	ImGui::PushID("##curvekeys");
	CurveSyncFrame();
	const ImGuiID cvSelf = ImGui::GetID("##cvowner");   // this editor's identity in the sync pool
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
	// Sort keys by t before anything evaluates/draws — every frame EXCEPT mid-drag,
	// where indices must stay stable under the mouse.
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
				changed = true;   // the sorted order must reach the owner/undo
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
	// view range: fit the keys AND the curve itself (tangent bulges overshoot keys); the
	// floor drops below 0 only when the curve actually dips there (ease anticipation).
	float vmax = 1.0f, vmin = 0.0f;
	for (size_t k = 0; k < nk; ++k)
	{ vmax = std::max(vmax, (*c)[k * 4 + 1]); vmin = std::min(vmin, (*c)[k * 4 + 1]); }
	for (int sN = 0; sN <= 32; ++sN)
	{ const float sv = eval(sN / 32.0f); vmax = std::max(vmax, sv); vmin = std::min(vmin, sv); }
	const float vpad = (vmax - vmin) * 0.15f; vmax += vpad; if (vmin < 0.f) vmin -= vpad;
	// Freeze the view scale during any drag: a live-fitting range feeds the value back
	// through the mouse mapping and runs away exponentially.
	{
		ImGuiStorage* stv = ImGui::GetStateStorage();
		const ImGuiID vmaxId = ImGui::GetID("##curvevmax");
		const ImGuiID vminId = ImGui::GetID("##curvevmin");
		const bool anyDrag = (stv->GetInt(ImGui::GetID("##curvedrag"), -1) >= 0
		                   || stv->GetInt(ImGui::GetID("##curvehdl"), 0) != 0) && ImGui::IsMouseDown(0);
		if (anyDrag) { vmax = stv->GetFloat(vmaxId, vmax); vmin = stv->GetFloat(vminId, vmin); }
		else         { stv->SetFloat(vmaxId, vmax); stv->SetFloat(vminId, vmin); }
	}
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
	// axis numbers: value scale down the left grid lines, time along the bottom
	{
		char axl[24];
		for (int gl = 0; gl <= 4; ++gl)
		{
			snprintf(axl, sizeof(axl), "%.3g", vmax - (vmax - vmin) * gl / 4.0f);
			dl->AddText(ImVec2(p0.x + 3.0f, p0.y + hpx * gl / 4.0f + (gl == 4 ? -15.0f : 2.0f)),
			            IM_COL32(120, 120, 125, 190), axl);
		}
		for (int gl = 1; gl <= 4; ++gl)
		{
			snprintf(axl, sizeof(axl), "%.3g", gl / 4.0f);
			dl->AddText(ImVec2(p0.x + wpx * gl / 4.0f + (gl == 4 ? -30.0f : 3.0f), p0.y + hpx - 15.0f),
			            IM_COL32(120, 120, 125, 190), axl);
		}
	}
	const float sy = hpx / (vmax - vmin);   // px per value unit (x axis: wpx px per t unit)
	auto toPx = [&](float t, float v) { return ImVec2(p0.x + t * wpx, p0.y + (1.0f - (v - vmin) / (vmax - vmin)) * hpx); };
	if (vmin < 0.f)   // zero baseline so negative dips read as such
	{
		const float zy = toPx(0.f, 0.f).y;
		dl->AddLine(ImVec2(p0.x, zy), ImVec2(p0.x + wpx, zy), IM_COL32(90, 90, 95, 255));
	}
	ImVec2 prev = toPx(0.f, eval(0.f));
	for (int sN = 1; sN <= 96; ++sN)
	{
		ImVec2 cur = toPx(sN / 96.0f, eval(sN / 96.0f));
		dl->AddLine(prev, cur, IM_COL32(120, 190, 255, 255), 1.6f);
		prev = cur;
	}
	if (g_cvDragTDraw >= 0.0f)   // alignment line while any timeline key is dragged
	{
		const float lx = p0.x + g_cvDragTDraw * wpx;
		for (float y = p0.y; y < p0.y + hpx; y += 8.0f)
			dl->AddLine(ImVec2(lx, y), ImVec2(lx, std::min(y + 4.0f, p0.y + hpx)), IM_COL32(255, 200, 80, 150), 1.0f);
	}
	// Per-widget interaction state in ImGui storage; handle: 0 none / 1 in / 2 out.
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
		float nt = std::max(0.f, std::min(1.f, (mp.x - p0.x) / wpx));
		if (ImGui::GetIO().KeyCtrl)   // Ctrl = snap to OTHER editors' key times
		{
			float best = nt, bd = 1e9f;
			for (const auto& tt : g_cvTimesDraw)
				if (tt.first != cvSelf)
				{ const float d = fabsf(tt.second - nt); if (d < bd) { bd = d; best = tt.second; } }
			if (bd < 8.0f / wpx) nt = best;
		}
		const float oldKeyT = (*c)[dragKey * 4];
		(*c)[dragKey * 4]     = nt;
		(*c)[dragKey * 4 + 1] = std::max(vLo, std::min(vHi, vmin + (1.0f - (mp.y - p0.y) / hpx) * (vmax - vmin)));
		changed = true;
		if (trigT && oldKeyT != nt)   // a trigger sits ON its key: it moves with it
			for (float& kt : *trigT)
				if (std::fabs(kt - oldKeyT) < 1e-4f) kt = nt;
		g_cvDragT = nt;
	}
	if (!ImGui::IsMouseDown(0)) { if (dragKey >= 0) st->SetInt(dragId, -1); if (dragHdl) st->SetInt(hdlId, 0); }
	if (hov && ImGui::IsMouseDoubleClicked(0) && hotKey < 0 && !hotHdl)   // add key
	{
		float t = std::max(0.f, std::min(1.f, (mp.x - p0.x) / wpx));
		float v = std::max(vLo, std::min(vHi, vmin + (1.0f - (mp.y - p0.y) / hpx) * (vmax - vmin)));
		// auto tangent = the curve's current slope at t: adding a key keeps the shape
		float m = nk ? (eval(std::min(1.f, t + 0.01f)) - eval(std::max(0.f, t - 0.01f))) / 0.02f : 0.f;
		c->insert(c->end(), { t, v, m, m });
		changed = true;
	}
	if (hov && ImGui::IsMouseClicked(1) && hotKey >= 0)        // right-click a key = delete
	{
		const float delKeyT = (*c)[hotKey * 4];
		if (trigT && trigEv)   // the key's trigger dies with the key
			for (size_t q = trigT->size(); q-- > 0;)
				if (std::fabs((*trigT)[q] - delKeyT) < 1e-4f)
				{
					trigT->erase(trigT->begin() + q);
					if (q < trigEv->size()) trigEv->erase(trigEv->begin() + q);
					changed = true;
				}
		c->erase(c->begin() + hotKey * 4, c->begin() + hotKey * 4 + 4);
		st->SetInt(selId, -1); st->SetInt(dragId, -1); st->SetInt(hdlId, 0);
		changed = true;
	}
	if (nk == 0 && emptyText)
		dl->AddText(ImVec2(p0.x + 8, p0.y + hpx * 0.5f - 8), IM_COL32(140, 140, 145, 255), emptyText);
	else if (hov && hotKey < 0 && !hotHdl && dragKey < 0 && !dragHdl)
		dl->AddText(ImVec2(p0.x + 6, p0.y + 3), IM_COL32(120, 120, 125, 200),
		            "click key: tangents  |  dblclick: add  |  RMB key: delete");
	if (trigT)   // trigger flags sit ON their keys
		for (float kt : *trigT)
		{
			const float lx = p0.x + std::max(0.0f, std::min(1.0f, kt)) * wpx;
			dl->AddTriangleFilled(ImVec2(lx - 4.0f, p0.y + 2.0f), ImVec2(lx + 4.0f, p0.y + 2.0f),
			                      ImVec2(lx, p0.y + 11.0f), IM_COL32(255, 160, 40, 230));
		}
	// The SELECTED key's trigger: pick a material event to fire when playback crosses it.
	if (trigT && trigEv && trigOptions && !trigOptions->empty() && selKey >= 0 && selKey < (int)nk)
	{
		const float kt = (*c)[selKey * 4];
		int ti = -1;
		for (int q = 0; q < (int)trigT->size(); ++q)
			if (std::fabs((*trigT)[q] - kt) < 1e-4f) { ti = q; break; }
		if (ti >= 0 && ti >= (int)trigEv->size()) trigEv->resize(ti + 1);
		ImGui::SetNextItemWidth(std::max(120.0f, ImGui::CalcItemWidth() * 0.5f));
		if (ImGui::BeginCombo("##keytrig", ti >= 0 ? (*trigEv)[ti].c_str() : "(no trigger)"))
		{
			if (ImGui::Selectable("(no trigger)", ti < 0) && ti >= 0)
			{
				trigT->erase(trigT->begin() + ti);
				trigEv->erase(trigEv->begin() + ti);
				ti = -1;   // the slot is gone — the loop below must not index it
				changed = true;
			}
			for (const std::string& en : *trigOptions)
				if (ImGui::Selectable(en.c_str(), ti >= 0 && (*trigEv)[ti] == en))
				{
					if (ti >= 0) (*trigEv)[ti] = en;
					else { trigT->push_back(kt); trigEv->push_back(en); }
					changed = true;
				}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("trigger on key %.2f", kt);
	}
	for (int k2 = 0; k2 < (int)(c->size() / 4); ++k2)
		g_cvTimes.push_back({ cvSelf, (*c)[k2 * 4] });
	ImGui::PopID();
	return changed;
}

bool EditorUI::DrawFields(void* obj, nuke::TypeInfo* ti)
{
	if (!ti) return false;
	bool changed = false;
	for (const nuke::Field& f : ti->fields)
	{
		if (f.hidden) continue;   // serialized but not shown (e.g. script props JSON)
		void* a = f.addr(obj);
		const char* n = f.label.empty() ? f.name.c_str() : f.label.c_str();   // metadata display name
		// Label drawn manually BEFORE the widget; the widget takes a hidden id (ImGui puts labels after).
		std::string hid = "##" + f.name; const char* w = hid.c_str();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(n);
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
			else if (f.fmax > f.fmin)   // [[prop(min,max)]]: clamped drag — double-click edits as text
				changed |= ImGui::DragInt(w, (int*)a, std::max(0.02f, (f.fmax - f.fmin) / 250.0f),
				                          (int)f.fmin, (int)f.fmax, "%d", ImGuiSliderFlags_AlwaysClamp);
			else changed |= ImGui::InputInt(w, (int*)a);
			break;
		case nuke::FT::Float:
			// [[prop(min,max)]]: clamped drag, NOT a slider — double-click still edits as text.
			if (f.fmax > f.fmin) changed |= ImGui::DragFloat(w, (float*)a, std::max(0.001f, (f.fmax - f.fmin) / 250.0f),
			                                                 f.fmin, f.fmax, "%.3f", ImGuiSliderFlags_AlwaysClamp);
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
			// The prop's asset= hint filters the picker (and drops) to atoms carrying that component.
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
			// Reflected std::vector<T>: header row + per-element rows. The field name scopes the ID
			// stack — row ids are per-index, so two lists side by side would otherwise collide.
			ImGui::PushID(f.name.c_str());
			// [[prop(widget="curve")]] float list: cubic-Hermite keys (t, value, inTangent, outTangent),
			// the same layout/math the VFX runtime evaluates — drawn by the shared curve widget.
			if (f.type == nuke::FT::FloatList && f.widget == "curve")
			{
				// value clamp from the prop's min=/max= hints (e.g. alpha curves are 0..1)
				const float vLo = f.fmax > f.fmin ? f.fmin : 0.0f;
				const float vHi = f.fmax > f.fmin ? f.fmax : 1e30f;
				changed |= CurveKeysEditor(*(std::vector<float>*)a, vLo, vHi, "constant 1  -  double-click to add keys");
				ImGui::PopID();
				break;
			}
			// [[prop(widget="gradient")]] float list: (t,r,g,b) stops — the shared gradient bar.
			if (f.type == nuke::FT::FloatList && f.widget == "gradient")
			{
				changed |= GradientStopsEditor(*(std::vector<float>*)a, false);
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

// Draws a component's dynamic props (e.g. a Lua script's exported vars) via DynamicProps/SetDynamicProp.
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
			// Reference to a live atom by stable id; same picker as reflected Atom* props.
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
// The row LABEL leads (fixed column), the fields fill the rest — a row reads name -> values
// (user 2026-08-24: trailing labels made the transform section read backwards).
bool EditorUI::EditV3(const char* rowLabel, double v[3])
{
	static const char* ax[3] = { "X", "Y", "Z" };
	static const ImVec4 col[3] = { ImVec4(0.86f,0.34f,0.34f,1.0f), ImVec4(0.42f,0.74f,0.36f,1.0f), ImVec4(0.36f,0.55f,0.92f,1.0f) };
	bool ch = false;
	ImGui::PushID(rowLabel);
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(rowLabel);
	ImGui::SameLine(84.0f);
	float w = (ImGui::GetContentRegionAvail().x - 66.0f) / 3.0f;   // 3 axis letters + spacing
	if (w < 36.0f) w = 36.0f;
	for (int i = 0; i < 3; ++i)
	{
		ImGui::PushID(i);
		ImGui::TextColored(col[i], "%s", ax[i]);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(w);
		ch |= ImGui::InputDouble("##v", &v[i], 0.0, 0.0, "%.3f");
		if (i < 2) ImGui::SameLine();
		ImGui::PopID();
	}
	ImGui::PopID();
	return ch;
}

void EditorUI::ApplyToPrefab(Atom* a)
{
	if (!a || a->prefabGuid.empty()) return;
	std::string path = ResDB::getSingleton()->PathForGuid(a->prefabGuid);
	if (path.empty()) return;
	nuke::SavePrefab(a, path);
	std::cout << "[editor]\tapplied instance to prefab " << path << std::endl;
}

// Replaces the instance object but keeps its id + placement + prefab link. Undoable as one delta.
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

// The component travels as a live pointer: only its owner back-references are rewired and Init is
// NOT re-run. No-op on null / src == dst.
void EditorUI::MoveComponent(Atom* src, Component* c, Atom* dst)
{
	if (!src || !c || !dst || src == dst) return;
	World* w = AppInstance::GetSingleton()->currentWorld;
	auto place = [&](Atom* a, long& parent, int& index)
	{
		parent = a->parent ? a->parent->id.id : 0;
		index = 0; auto& lst = a->parent ? a->parent->children : w->GetHierarchy();
		int i = 0; for (Atom* s : lst) { if (s == a) { index = i; break; } ++i; }
	};
	long sid = src->id.id, did = dst->id.id, sPar = 0, dPar = 0; int sIdx = 0, dIdx = 0;
	place(src, sPar, sIdx); place(dst, dPar, dIdx);
	std::string sBefore = nuke::SaveAtomToString(src), dBefore = nuke::SaveAtomToString(dst);
	src->components.remove(c);
	dst->components.push_back(c);
	c->transform = &dst->GetTransform();   // rewired by hand: several Inits skip `atom`
	c->atom = dst;
	std::string sAfter = nuke::SaveAtomToString(src), dAfter = nuke::SaveAtomToString(dst);
	editing = false; editAtomId = 0;   // own command: suppress the auto edit-detector
	// ApplyAtomState re-creates a whole subtree by id: if one atom contains the other, apply the ANCESTOR first.
	bool srcFirst = true;
	for (Atom* p = src->parent; p; p = p->parent) if (p == dst) { srcFirst = false; break; }
	PushUndo("Move component",
		[this, sid, sPar, sIdx, sBefore, did, dPar, dIdx, dBefore, srcFirst]
		{
			if (srcFirst) { ApplyAtomState(sid, sPar, sIdx, sBefore); ApplyAtomState(did, dPar, dIdx, dBefore); }
			else          { ApplyAtomState(did, dPar, dIdx, dBefore); ApplyAtomState(sid, sPar, sIdx, sBefore); }
		},
		[this, sid, sPar, sIdx, sAfter, did, dPar, dIdx, dAfter, srcFirst]
		{
			if (srcFirst) { ApplyAtomState(sid, sPar, sIdx, sAfter); ApplyAtomState(did, dPar, dIdx, dAfter); }
			else          { ApplyAtomState(did, dPar, dIdx, dAfter); ApplyAtomState(sid, sPar, sIdx, sAfter); }
		});
}

void EditorUI::winInspector()
{
	if (!win->inspector) return;
	NukeUI::DocPanel("panel:inspector", "Inspector", &win->inspector, window_flags, 380, 700, [this]()
	{
	if (bootLoading) { ImGui::TextDisabled("Loading project..."); return; }   // half-deserialized selection
	if (auto sltd = AppInstance::GetSingleton()->selectedInHieararchy)
	{
		// Multi-selection banner: edits below flow to every selected atom where it makes sense
		// (layer/persistent/enabled fan out; name and components stay on the primary).
		const std::vector<Atom*> msel = AppInstance::GetSingleton()->Selection();
		const bool multi = msel.size() > 1;
		if (multi) ImGui::TextDisabled(ICON_LC_LAYERS " %zu atoms selected — editing '%s'",
		                               msel.size(), sltd->GetName().c_str());

		char name[128];
		strncpy(name, sltd->GetName().c_str(), 127); name[127] = 0;
		if (ImGui::InputText("Name", name, 128)) sltd->SetName(name);

		// a folder is pure organization — name + enabled only, transform locked to identity.
		if (sltd->folder)
		{
			bool ena = sltd->enabled;
			if (ImGui::Checkbox("Enabled", &ena)) { sltd->enabled = ena; }
			ImGui::TextDisabled(ICON_LC_FOLDER " Folder: children keep their world poses;\n"
			                    "the runtime treats it as a plain empty.");
			return;
		}

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
					if (ImGui::Selectable(nm.c_str(), i == sltd->layer))
					{
						sltd->layer = i;
						if (multi) for (Atom* m : msel) if (m) m->layer = i;   // fan out
					}
				}
				ImGui::EndCombo();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Render channel (named in Project Settings > Layers); cameras pick what they draw via Layer Mask");
			ImGui::SameLine();
			bool prs = sltd->persistent;
			if (ImGui::Checkbox("Persistent", &prs))
			{ sltd->persistent = prs; if (multi) for (Atom* m : msel) if (m) m->persistent = prs; }
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Survive game world switches (Game.LoadWorld / async activation).\n"
			                                              "Applies to ROOT atoms while PLAYING; children ride with their root.\n"
			                                              "Editor world opens and savegame loads never carry atoms.");
			ImGui::SameLine();
			bool ena = sltd->enabled;
			if (ImGui::Checkbox("Enabled", &ena))
			{ sltd->enabled = ena; if (multi) for (Atom* m : msel) if (m) m->enabled = ena; }
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Whole-atom switch: unticked disables this atom AND its subtree\n"
			                                              "(updates, rendering, events, physics bodies).");
			// World Partition membership — only meaningful in a streamed world.
			if (AppInstance::GetSingleton()->currentWorld
			    && AppInstance::GetSingleton()->currentWorld->settings.streamEnabled)
			{
				bool al = sltd->alwaysLoaded;
				if (ImGui::Checkbox("Always Loaded", &al))
				{ sltd->alwaysLoaded = al; if (multi) for (Atom* m : msel) if (m) m->alwaysLoaded = al; }
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep this ROOT atom in the main world instead of streaming it\n"
				                                              "with its grid cell (terrain managers, global logic, sky).");
			}
		}

		// Prefab instance bar — sync is manual in both directions.
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

		// Anchors (canvas children only): enabling a side captures the CURRENT distance. Backed by a
		// RectAnchor component auto-added on first enable; the world applies it every frame.
		{
			nuke::Canvas* cvAnc = nullptr;
			for (Atom* p = sltd->parent; p && !cvAnc; p = p->parent) cvAnc = p->GetComponent<nuke::Canvas>();
			if (cvAnc && cvAnc->transform)
			{
				ImGui::SeparatorText("Anchors");
				nuke::RectAnchor* ra = sltd->GetComponent<nuke::RectAnchor>();
				// Canvas units — must match the conventions of the world's layout pass.
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
				// Drag the header onto a hierarchy row to move this component to that atom.
				if (ImGui::BeginDragDropSource())
				{
					CompDragPayload pay; pay.atomId = sltd->id.id; pay.compId = cmp->id.id;
					ImGui::SetDragDropPayload("NUKE_COMPONENT", &pay, sizeof(pay));
					ImGui::Text("%s  (drop on an atom)", label.c_str());
					ImGui::EndDragDropSource();
				}
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
						// InputInt's item width covers the whole widget (text box + both step buttons),
						// so the field must stay wide enough for the number on narrow panels.
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
						// multi-edit: snapshot the fields, draw, then mirror ONLY the fields
						// that changed to every selected atom carrying the same component type.
						nuke::TypeInfo* mti = cmp->GetType();
						std::vector<nuke::ReflectValue> mbefore;
						if (multi && mti)
							for (const nuke::Field& f : mti->fields) mbefore.push_back(nuke::Reflect_GetField(cmp, f));
						const bool mchg = DrawFields(cmp, cmp->GetType());   // auto fields from [[nuke::prop]] schema
						if (multi && mti && mchg)
						{
							size_t fi = 0;
							for (const nuke::Field& f : mti->fields)
							{
								nuke::ReflectValue now = nuke::Reflect_GetField(cmp, f);
								const nuke::ReflectValue& was = mbefore[fi++];
								const bool same = now.type == was.type && now.b == was.b && now.num == was.num
								               && now.str == was.str && now.atom == was.atom && now.obj == was.obj
								               && memcmp(now.v, was.v, sizeof(now.v)) == 0;
								if (same) continue;
								for (Atom* m : msel)
								{
									if (!m || m == sltd) continue;
									if (nuke::Component* mc = nuke::Reflect_FindComponent(m, mti->name))
									{
										nuke::Reflect_SetField(mc, f, now);
										nuke::Reflect_ComponentFieldChanged(mc, f);
									}
								}
							}
						}
						DrawDynamicProps(cmp);             // dynamic props (e.g. Lua script vars)

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

		// Add Component: every registered create-able type, grouped by TypeInfo::category ("" = Other).
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

// Dispatches by extension: .nutex (usage + info), .numat (reflected fields), else read-only info.
// The loaded asset is cached until the selection or the file's mtime changes.
void EditorUI::DrawAssetInspector(const std::string& path)
{
	std::string ext = bfs::path(path).extension().string();
	for (char& c : ext) c = (char)std::tolower((unsigned char)c);

	boost::system::error_code mec;
	long long mtime = (long long)bfs::last_write_time(bfs::path(path), mec);
	if (path != inspAssetPath || mtime != inspAssetMtime)   // new selection or a reimport on disk
	{
		if (inspTex) { delete inspTex; inspTex = nullptr; }
		if (inspMat) { delete inspMat; inspMat = nullptr; }
		if (inspTexPreviewId)
		{
			if (iRender* r = AppInstance::GetSingleton()->render) r->destroyTexture2D(inspTexPreviewId);
			inspTexPreviewId = 0;
		}
		pvStaged.clear();   // restage the 3D preview for the new selection
		inspAssetPath = path; inspAssetMtime = mtime;
		if      (ext == ".nutex") inspTex = nuke::Texture::LoadFromFile(path);
		else if (ext == ".numat") inspMat = nuke::Material::LoadFromFile(path);
		// Decode mip0 and upload once per change; downsample to a 2048 cap first so huge sheets
		// don't spike VRAM for a panel-width thumbnail.
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
	// Types with a dedicated editor window, module-supplied ones included.
	const bool hasOwnEditor = ext == ".numat" || ext == ".numesh" || ext == ".nuprefab"
	                       || ext == ".nuanim" || ext == ".nusm" || ext == ".nublend"
	                       || ext == ".nuskel" || ext == ".nurag" || ext == ".nubonemap"
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
		if (inspTexPreviewId)
		{
			const float w = ImGui::GetContentRegionAvail().x;   // full panel width, always
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

		if (inspTex->usage == nuke::Texture::UsageSprite)   // grid/margin/spacing/9-slice live in the slicer
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
			ImGui::SetNextItemWidth(160); ImGui::DragInt("Tolerance", &inspChromaTol, 0.5f, 0, 128, "%d", ImGuiSliderFlags_AlwaysClamp);
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
			ImGui::Text("%d vertices   %d triangles   %d section(s)   %d LOD(s)",
			            m->numVerts, m->TriCount(), m->SectionCount(), m->LodCount());
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
