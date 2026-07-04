// inspector panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>
#include <API/Model/Camera.h>   // restrict the PostProcess component to camera atoms
#include <API/Model/Texture.h>  // asset inspector: .nutex usage/info
#include <API/Model/Material.h> // asset inspector: .numat fields
#include <API/Model/Light.h>       // asset preview world: sun
#include <API/Model/Environment.h> // asset preview world: sky/ambient
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
	for (PreviewScene* s : pvPool)
	{
		if (s->inUse && s->visible && s->world)
			s->world->Render(r);
		s->visible = false;
	}
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
// Does a dropped file path match the field's asset kind? (rejects everything else)
static bool KindMatchesFile(const std::string& kind, const std::string& path)
{
	std::string e = bfs::path(path).extension().string();
	for (char& c : e) c = (char)tolower((unsigned char)c);
	if (kind == "mesh")     return e == ".numesh";
	if (kind == "material") return e == ".numat";
	if (kind == "texture")  return e == ".nutex";
	if (kind == "script")   return e == ".lua";
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
		if (kind == "script") return bfs::path(g).stem().string();   // value is a content-relative path
		if (kind == "shader" || kind == "postshader") { Shader* s = db->GetShader(g); return s ? s->name : g; }
		std::string p = db->PathForGuid(g);
		if (!p.empty()) return bfs::path(p).stem().string();
		if (kind == "mesh")     { Mesh* m = db->GetMesh(g);     return m ? std::string(m->name) : g; }
		if (kind == "material") { Material* m = db->GetMaterial(g); return m ? (m->matName.empty() ? g : m->matName) : g; }
		return g;   // texture
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
				if      (kind == "script") { boost::system::error_code ec; g = bfs::relative(bfs::path(path), bfs::path(contentDir), ec).generic_string(); }
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
		if      (kind == "script") { if (!guid.empty()) path = (bfs::path(contentDir) / guid).string(); }
		else if (kind == "shader" && db->GetShader(guid)) path = db->GetShader(guid)->vsPath;
		else                       path = db->PathForGuid(guid);
		if (!path.empty()) { BrowserNavigate(bfs::path(path).parent_path().string()); browserSel = path; if (win) win->browser = true; }
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Go to file");
	ImGui::SameLine(0, 2);
	if (ImGui::Button(ICON_LC_ROTATE_CCW "##rst")) { guid = defGuid; changed = true; }   // reset to default
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset to default");
	ImGui::SameLine(0, 6); ImGui::TextUnformatted(label);

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
		else if (kind == "script")   // scan the project content for .lua files (value = content-relative path)
		{
			boost::system::error_code ec;
			bfs::path croot(contentDir);
			if (bfs::exists(croot, ec))
				for (bfs::recursive_directory_iterator it(croot, ec), end; it != end; it.increment(ec))
				{
					if (ec) break;
					if (bfs::is_directory(it->path())) continue;
					std::string e = it->path().extension().string();
					for (char& c : e) c = (char)tolower((unsigned char)c);
					if (e != ".lua") continue;
					std::string rel = bfs::relative(it->path(), croot, ec).generic_string();
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
	World* w = AppInstance::GetSingleton()->currentScene;
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
	if (ImGui::Button(ICON_LC_PLUS " Add Effect"))
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
		switch (f.type)
		{
		case nuke::FT::Bool:   changed |= ImGui::Checkbox(n, (bool*)a); break;
		case nuke::FT::Int:
			if (!f.enumLabels.empty())   // [[prop(enum=...)]] -> dropdown; the int is the selected index
			{
				int* iv = (int*)a; int cur = (*iv < 0 || *iv >= (int)f.enumLabels.size()) ? 0 : *iv;
				if (ImGui::BeginCombo(n, f.enumLabels[cur].c_str()))
				{
					for (int e = 0; e < (int)f.enumLabels.size(); ++e)
						if (ImGui::Selectable(f.enumLabels[e].c_str(), e == cur)) { *iv = e; changed = true; }
					ImGui::EndCombo();
				}
			}
			else changed |= ImGui::InputInt(n, (int*)a);
			break;
		case nuke::FT::Float:
			if (f.fmax > f.fmin) changed |= ImGui::SliderFloat(n, (float*)a, f.fmin, f.fmax);   // [[prop(min,max)]]
			else                 changed |= ImGui::DragFloat(n, (float*)a, 0.05f);
			break;
		case nuke::FT::Double: changed |= ImGui::InputDouble(n, (double*)a); break;
		case nuke::FT::String:
		{
			std::string* s = (std::string*)a;
			if (!f.asset.empty()) { changed |= AssetPicker(n, *s, f.asset); break; }   // metadata-driven picker
			char buf[256]; strncpy(buf, s->c_str(), 255); buf[255] = 0;
			if (ImGui::InputText(n, buf, sizeof(buf))) { *s = buf; changed = true; }
			break;
		}
		case nuke::FT::Vec2:
		{
			Vector2* v = (Vector2*)a; float t[2] = { (float)v->x, (float)v->y };
			if (ImGui::DragFloat2(n, t, 0.05f)) { v->x = t[0]; v->y = t[1]; changed = true; }
			break;
		}
		case nuke::FT::Vec3:
		{
			Vector3* v = (Vector3*)a; float t[3] = { (float)v->x, (float)v->y, (float)v->z };
			if (ImGui::DragFloat3(n, t, 0.05f)) { v->x = t[0]; v->y = t[1]; v->z = t[2]; changed = true; }
			break;
		}
		case nuke::FT::Vec4:
		case nuke::FT::Quat:
		{
			Vector4* v = (Vector4*)a; float t[4] = { (float)v->x, (float)v->y, (float)v->z, (float)v->w };
			if (ImGui::DragFloat4(n, t, 0.05f)) { v->x = t[0]; v->y = t[1]; v->z = t[2]; v->w = t[3]; changed = true; }
			break;
		}
		case nuke::FT::Color:
		{
			Color* c = (Color*)a; float t[4] = { (float)c->r, (float)c->g, (float)c->b, (float)c->a };
			if (ImGui::ColorEdit4(n, t)) { c->r = t[0]; c->g = t[1]; c->b = t[2]; c->a = t[3]; changed = true; }
			break;
		}
		default: break;
		}
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
	World* w = app->currentScene;
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
	World* w = AppInstance::GetSingleton()->currentScene;
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
	ImGui::Begin("Inspector", &win->inspector, window_flags);
	if (auto sltd = AppInstance::GetSingleton()->selectedInHieararchy)
	{
		char name[128];
		strncpy(name, sltd->GetName().c_str(), 127); name[127] = 0;
		if (ImGui::InputText("Name", name, 128)) sltd->SetName(name);

		// Prefab instance bar: this atom IS an instance (a prefab with individual params). Manual sync only.
		if (!sltd->prefabGuid.empty())
		{
			std::string ppath = ResDB::getSingleton()->PathForGuid(sltd->prefabGuid);
			if (!ppath.empty())
			{
				ImGui::Text(ICON_LC_BOX " Prefab: %s", bfs::path(ppath).stem().string().c_str());
				if (ImGui::Button("Apply to prefab")) ApplyToPrefab(sltd);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Overwrite the prefab file with this instance's values");
				ImGui::SameLine();
				if (ImGui::Button("Reset to prefab")) { ResetToPrefab(sltd); ImGui::End(); return; }   // sltd is replaced
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
				ImGui::SetNextItemAllowOverlap();   // let the X button (drawn over the header) take its own clicks
				st = ImGui::CollapsingHeader(hdr.c_str());
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
		ImGui::Separator();
		if (ImGui::Button("Add Component"))
			ImGui::OpenPopup("addcomp");
		if (ImGui::BeginPopup("addcomp"))
		{
			for (nuke::TypeInfo* ti : nuke::Registry_All())
			{
				if (!ti->create || ti->base != "Component")
					continue;
				// PostProcess is a per-camera effect — only offer it on an atom that has a Camera component.
				if (ti->name == "PostProcess" && !sltd->GetComponent<nuke::Camera>())
					continue;
				if (ImGui::MenuItem(ti->name.c_str()))
					sltd->AddComponent((nuke::Component*)ti->create());
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
	ImGui::End();
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
		// Texture preview: decode mip0 to RGBA8 and upload once per selection/change.
		if (inspTex && !inspTex->renderTexture)
			if (iRender* r = AppInstance::GetSingleton()->render)
			{
				std::vector<unsigned char> rgba = inspTex->DecodeRGBA();
				if (!rgba.empty())
					inspTexPreviewId = r->createTexture2D(rgba.data(), inspTex->width, inspTex->height);
			}
	}

	ImGui::TextUnformatted(bfs::path(path).filename().string().c_str());
	ImGui::SameLine(); ImGui::TextDisabled("%s", ext.c_str());
	// Any text-editable type opens in the text editor (2.2); assets open their own editor window.
	if (IsTextFile(ext))
	{
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
		if (ImGui::SmallButton(ICON_LC_FILE_PEN " Edit")) OpenTextFile(path);
	}
	else if (ext == ".numat" || ext == ".numesh" || ext == ".nuprefab")
	{
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120.0f);
		if (ImGui::SmallButton(ICON_LC_PENCIL_RULER " Open in Editor")) OpenAssetEditor(path);
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
			ImGui::Image((ImTextureID)inspTexPreviewId, ImVec2(w, h));
			ImGui::Spacing();
		}
		const char* fmt = inspTex->format == nuke::Texture::FMT_BC1 ? "BC1" : inspTex->format == nuke::Texture::FMT_BC3 ? "BC3"
		                : inspTex->format == nuke::Texture::FMT_BC5 ? "BC5" : "RGBA8";
		ImGui::Text("%d x %d   %s   %d mip(s)", inspTex->width, inspTex->height, fmt, inspTex->mipCount);
		if (inspTex->frameCount > 1) ImGui::Text("Animated: %d frames", inspTex->frameCount);
		ImGui::Spacing();
		const char* usages[] = { "Color (sRGB)", "Normal Map", "Data (linear)", "Emissive (sRGB)" };
		int u = inspTex->usage;
		if (ImGui::Combo("Texture Type", &u, usages, IM_ARRAYSIZE(usages)) && u != inspTex->usage)
		{
			int before = inspTex->usage;
			auto setUsage = [this, path](int val) {
				if (inspAssetPath == path && inspTex) { inspTex->usage = val; inspTex->SaveToFile(path); }
				else if (nuke::Texture* t = nuke::Texture::LoadFromFile(path)) { t->usage = val; t->SaveToFile(path); delete t; }
			};
			setUsage(u);
			PushUndo("Texture type", [setUsage, before]{ setUsage(before); }, [setUsage, u]{ setUsage(u); });
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
				PushUndo("Normal green convention", [setIG, before]{ setIG(before); }, [setIG, ig]{ setIG(ig); });
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("On = OpenGL convention (+Y up, green flipped) — glTF/Blender/Substance default.\nOff = DirectX (-Y). Toggle if the relief looks inverted.");
		}

		// Compression override — re-compresses the .nutex in place (quality vs size). Normals default to BC5 (8bpp);
		// BC1 halves the size but is blocky. Applied immediately (decode -> re-encode) + live in the renderer.
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
				PushUndo("Texture compression", [applyFmt, before]{ applyFmt(before); }, [applyFmt, after]{ applyFmt(after); });
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Re-compress now. BC5 = 8 bpp (normals, quality); BC1 = 4 bpp (half the size, blocky). BC1<->BC5 is lossy.");
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
		if (ext == ".nuworld")  { if (ImGui::Button(ICON_LC_GLOBE " Open World"))  OpenWorldFromBrowser(path); }
		else if (ext == ".nuprefab")
		{
			ImGui::TextDisabled("Prefab — drag into the world to instantiate.");
			if (ImGui::Button(ICON_LC_PACKAGE_PLUS " Instantiate")) InstantiatePrefab(path);
		}
		else if (ext == ".nuproj")
		{
			ImGui::TextDisabled("Project descriptor.");
			if (ImGui::Button(ICON_LC_SETTINGS " Open Project Settings")) settingsOpen = true;
		}
		else if (!IsTextFile(ext)) ImGui::TextDisabled("No editable properties.");
	}
}
