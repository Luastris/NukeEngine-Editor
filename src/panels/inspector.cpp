// inspector panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>

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

	auto nameOf = [&](const std::string& g) -> std::string {
		if (g.empty()) return "(none)";
		if (kind == "mesh")     { Mesh* m = db->GetMesh(g);     return m ? std::string(m->name) : g; }
		if (kind == "material") { Material* m = db->GetMaterial(g); return m ? (m->matName.empty() ? m->guid : m->matName) : g; }
		if (kind == "shader")   { Shader* s = db->GetShader(g); return s ? s->name : g; }
		return g;   // texture
	};

	ImGui::PushID(label);
	float full = ImGui::CalcItemWidth();
	std::string cur = nameOf(guid);
	if (ImGui::Button((cur + "##cur").c_str(), ImVec2(full - 52, 0))) { assetFilter[0] = 0; ImGui::OpenPopup("##assetpop"); }

	// Drag from the browser — only accepted if the file's type matches this field's kind.
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NUKE_ASSET"))
		{
			std::string path((const char*)p->Data);
			if (KindMatchesFile(kind, path))
			{
				std::string g = (kind == "shader") ? ShaderGuidFromPath(path) : db->GuidForPath(path);
				if (!g.empty()) { guid = g; changed = true; }
			}
		}
		ImGui::EndDragDropTarget();
	}

	ImGui::SameLine(0, 2);
	if (ImGui::Button(ICON_LC_FOLDER_SEARCH "##loc"))   // locate the original file in the browser
	{
		std::string path = (kind == "shader" && db->GetShader(guid)) ? db->GetShader(guid)->vsPath : db->PathForGuid(guid);
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
		if      (kind == "mesh")     for (Mesh* m : db->meshes)      { if (m) item(m->guid, m->name); }
		else if (kind == "material") for (Material* m : db->materials) { if (m) item(m->guid, m->matName.empty() ? m->guid : m->matName); }
		else if (kind == "shader")   for (Shader* s : db->shaders)   { if (s) item(s->guid, s->name); }
		else if (kind == "texture")  for (Texture* t : db->textures) { if (t) item(t->guid, t->guid); }
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
		AssetPicker("Shader", m->shaderGuid, "shader", "world");
		if (!m->shader || m->shader->guid != m->shaderGuid) m->shader = db->GetShader(m->shaderGuid);
		ImGui::ColorEdit4("Color", m->color);
		if (!m->diffuseGuid.empty())  ImGui::Text("diffuse:  %s", m->diffuseGuid.c_str());
		if (!m->normalGuid.empty())   ImGui::Text("normal:   %s", m->normalGuid.c_str());
		if (!m->specularGuid.empty()) ImGui::Text("specular: %s", m->specularGuid.c_str());

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
				switch (sp.components)
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
		case nuke::FT::Int:    changed |= ImGui::InputInt(n, (int*)a); break;
		case nuke::FT::Float:  changed |= ImGui::DragFloat(n, (float*)a, 0.05f); break;
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

void EditorUI::winInspector()
{
	if (!win->inspector) return;
	ImGui::Begin("Inspector", &win->inspector, window_flags);
	if (auto sltd = AppInstance::GetSingleton()->selectedInHieararchy)
	{
		char name[128];
		strncpy(name, sltd->GetName().c_str(), 127); name[127] = 0;
		if (ImGui::InputText("Name", name, 128)) sltd->SetName(name);

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
				st = ImGui::CollapsingHeader(hdr.c_str());
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
				if (ImGui::MenuItem(ti->name.c_str()))
					sltd->AddComponent((nuke::Component*)ti->create());
			}
			ImGui::EndPopup();
		}
	}
	else
	{
		ImGui::TextWrapped("Select an object in the Hierarchy.");
	}
	ImGui::End();
}
