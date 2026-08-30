// .nuanim animation window: a live skeleton preview (rig atom in a pooled preview world,
// Animator serves the EDITING copy via previewClip), a scrubbable timeline with notify /
// event / curve / prop-track rows, the shared curve strip, and a retarget side-by-side
// preview (second world, target-skeleton rig, Bake -> .nuanim).
#include "editor/editorui.h"
#include "editor/animshared.h"
#include "API/Model/Animator.h"
#include "API/Model/SkinnedMeshRenderer.h"
#include "API/Model/Retarget.h"
#include "API/Model/Camera.h"
#include "API/Model/StatusBar.h"
#include "reflect/ReflectBind.h"
#include <service/iScript.h>
#include <interface/Services.h>
#include <boost/filesystem.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>

using namespace nuke;

// ---- shared rig plumbing (also used by the SM / blend-space editors) ------------------------

// EVERY mesh skinned to this skeleton, in ResDB order. A character is usually SEVERAL meshes
// (Mixamo's X_Bot = Beta_Joints + Beta_Surface), so a one-mesh preview shows half a body.
std::vector<std::string> EditorMeshesForSkeleton(const std::string& skelGuid)
{
	std::vector<std::string> out;
	if (skelGuid.empty()) return out;
	for (Mesh* m : ResDB::getSingleton()->meshes)
		if (m && m->skelGuid == skelGuid && m->boneWeight) out.push_back(m->guid);
	return out;
}

// The materials a mesh should render with when nothing else says otherwise: its import-time
// slots (v7), else the first prefab in the project that uses it (older assets keep their
// materials only in the prefab), else the default. Scanned once and cached per session.
static const std::vector<std::string>* PrefabMatsFor(const std::string& meshGuid)
{
	static std::map<std::string, std::vector<std::string>> cache;
	static bool scanned = false;
	if (!scanned)
	{
		scanned = true;
		AppInstance* app = AppInstance::GetSingleton();
		const std::string root = app ? app->ResolveContent("") : std::string();
		boost::system::error_code ec;
		if (!root.empty() && bfs::exists(root, ec))
			for (bfs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec))
			{
				if (ec) break;
				if (!bfs::is_regular_file(it->path(), ec)) continue;
				std::string e = it->path().extension().string();
				for (char& c : e) c = (char)tolower((unsigned char)c);
				if (e != ".nuprefab") continue;
				bfs::ifstream f(it->path());
				std::string txt((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
				nlohmann::json j = nlohmann::json::parse(txt, nullptr, false);
				if (j.is_discarded()) continue;
				// walk every component object anywhere in the tree
				std::function<void(const nlohmann::json&)> walk = [&](const nlohmann::json& n)
				{
					if (n.is_object())
					{
						if (n.contains("props") && n["props"].is_object())
						{
							const nlohmann::json& p = n["props"];
							const std::string mg = p.value("meshGuid", "");
							if (!mg.empty() && !cache.count(mg))
							{
								std::vector<std::string> mats;
								if (p.contains("matGuids") && p["matGuids"].is_array())
									for (const auto& g : p["matGuids"])
										mats.push_back(g.is_string() ? g.get<std::string>() : std::string());
								if (mats.empty())
								{
									const std::string one = p.value("matGuid", "");
									if (!one.empty()) mats.push_back(one);
								}
								if (!mats.empty()) cache[mg] = mats;
							}
						}
						for (auto it2 = n.begin(); it2 != n.end(); ++it2) walk(it2.value());
					}
					else if (n.is_array())
						for (const auto& e2 : n) walk(e2);
				};
				walk(j);
			}
	}
	auto it = cache.find(meshGuid);
	return it == cache.end() ? nullptr : &it->second;
}

void EditorApplyMeshMaterials(MeshRenderer* mr, const std::string& meshGuid)
{
	if (!mr) return;
	ResDB* db = ResDB::getSingleton();
	std::vector<std::string> mats;
	if (Mesh* m = db->GetMesh(meshGuid)) mats = m->defaultMats;
	if (mats.empty())
		if (const std::vector<std::string>* pm = PrefabMatsFor(meshGuid)) mats = *pm;
	mr->matGuids = mats;
	mr->matGuid = mats.empty() ? "builtin:default" : mats[0];
	if (mr->mat) { delete mr->mat; mr->mat = nullptr; }
	if (Material* a = db->GetMaterial(mr->matGuid)) mr->mat = a->Clone();
	mr->ResolveMaterials();
}

// The first mesh skinned to this skeleton (the picker's default; "" = none).
std::string EditorAutoMeshForSkeleton(const std::string& skelGuid)
{
	std::vector<std::string> all = EditorMeshesForSkeleton(skelGuid);
	return all.empty() ? std::string() : all.front();
}

// Find (or build) the preview rig: a root atom with a muted Animator and ONE
// SkinnedMeshRenderer CHILD per mesh of the SKELETON — a character is usually several meshes
// (Mixamo's X_Bot = Beta_Joints + Beta_Surface), and the Animator drives every subtree SMR
// sharing the skeleton, so the whole body poses as one.
Animator* EditorEnsureRig(EditorUI* ui, EditorUI::PreviewWorld* pv, long& atomId,
                          const std::string& skelGuid)
{
	if (!pv || skelGuid.empty()) return nullptr;
	const std::vector<std::string> want = EditorMeshesForSkeleton(skelGuid);
	if (want.empty()) return nullptr;

	Atom* rig = atomId ? pv->world->GetById(atomId) : nullptr;
	if (!rig)
	{
		rig = new Atom("Rig");
		Animator* na = new Animator();
		na->Init(rig);
		na->muteNotifies = true;
		pv->world->Add(rig);
		atomId = (long)rig->id.id;
	}
	Animator* an = rig->GetComponent<Animator>();

	std::vector<std::string> have;                     // current parts, in order
	for (Atom* ch : rig->children)
		if (SkinnedMeshRenderer* s = ch->GetComponent<SkinnedMeshRenderer>()) have.push_back(s->meshGuid);
	if (have != want)
	{
		while (!rig->children.empty())
			pv->world->RemoveAtomById((long)rig->children.front()->id.id);
		for (const std::string& g : want)
		{
			Atom* part = new Atom("Part");
			SkinnedMeshRenderer* s = new SkinnedMeshRenderer();
			s->Init(part);
			s->meshGuid = g;
			s->mesh = ResDB::getSingleton()->GetMesh(g);
			s->skelGuid = skelGuid;
			EditorApplyMeshMaterials(s, g);   // the mesh's own materials, not a white default
			// Skin the bind pose NOW: raw verts are NOT the rest pose (per-mesh bake spaces —
			// un-skinned parts render scattered), and idle editors (skeleton/ragdoll) never
			// tick an Animator to do it for us. Also fills Globals() for the bone overlays.
			s->ApplyPose();
			rig->AddChild(part);
		}
		if (an) an->Reset();        // rebind: pick up the new subtree SMRs + skeleton
		ui->FramePreview(*pv, rig);
	}
	return an;
}

Atom* EditorRigAtom(EditorUI::PreviewWorld* pv, long atomId)
{
	return (pv && atomId) ? pv->world->GetById(atomId) : nullptr;
}

// Model-space -> preview-rect projector (same math as the prefab gizmo). Overlays draw
// through ImGui — DebugDraw would leak the lines into the main viewport (its buffer is
// per-frame, not per-world).
bool EditorMakeProjector(EditorUI::PreviewWorld& pv, Atom* rig, EditorPvProj& out)
{
	if (!rig || pv.rectSize.x < 2 || pv.rectSize.y < 2 || !pv.cam || !pv.cam->transform) return false;
	Transform* ct = pv.cam->transform;
	Vector3 ce = ct->globalPosition();
	Vector3 cf = ct->direction(), cu = ct->up();
	const float aspect = pv.rectSize.x / pv.rectSize.y;
	const float fovy = (float)pv.cam->fov * 0.01745329252f;
	glm::mat4 V = glm::lookAtLH(glm::vec3((float)ce.x, (float)ce.y, (float)ce.z),
	                            glm::vec3((float)(ce.x + cf.x), (float)(ce.y + cf.y), (float)(ce.z + cf.z)),
	                            glm::vec3((float)cu.x, (float)cu.y, (float)cu.z));
	glm::mat4 P = glm::perspectiveLH_ZO(fovy, aspect, pv.cam->_near, pv.cam->_far);
	Transform& rt = rig->GetTransform();
	Vector3 rp = rt.globalPosition(); Quaternion rr = rt.globalRotation(); Vector3 rs = rt.globalScale();
	glm::mat4 W = glm::translate(glm::mat4(1.0f), glm::vec3((float)rp.x, (float)rp.y, (float)rp.z))
	            * glm::mat4_cast(glm::quat((float)rr.w, (float)rr.x, (float)rr.y, (float)rr.z))
	            * glm::scale(glm::mat4(1.0f), glm::vec3((float)rs.x, (float)rs.y, (float)rs.z));
	out.vpw = P * V * W;
	out.rectMin = pv.rectMin;
	out.rectSize = pv.rectSize;
	return true;
}

bool EditorPvProj::Project(const glm::vec3& model, ImVec2& out) const
{
	glm::vec4 c = vpw * glm::vec4(model, 1.0f);
	if (c.w < 1e-4f) return false;
	out.x = rectMin.x + (c.x / c.w * 0.5f + 0.5f) * rectSize.x;
	out.y = rectMin.y + (1.0f - (c.y / c.w * 0.5f + 0.5f)) * rectSize.y;
	return true;
}

// The rig's posed SkinnedMeshRenderer (its first part) — the pose/skeleton source overlays read.
SkinnedMeshRenderer* EditorRigSMR(Atom* rig)
{
	if (!rig) return nullptr;
	if (SkinnedMeshRenderer* s = rig->GetComponent<SkinnedMeshRenderer>()) return s;
	for (Atom* ch : rig->children)
		if (SkinnedMeshRenderer* s = ch->GetComponent<SkinnedMeshRenderer>()) return s;
	return nullptr;
}

// Bone overlay drawn over the LAST preview image rect.
void EditorDrawBoneOverlay(EditorUI::PreviewWorld& pv, Atom* rig)
{
	EditorPvProj pj;
	if (!EditorMakeProjector(pv, rig, pj)) return;
	SkinnedMeshRenderer* smr = EditorRigSMR(rig);
	Skeleton* sk = smr ? smr->EnsureSkeleton() : nullptr;
	if (!sk) return;
	const size_t nb = sk->bones.size();
	if (smr->Globals().size() < nb * 16) return;

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->PushClipRect(pv.rectMin, ImVec2(pv.rectMin.x + pv.rectSize.x, pv.rectMin.y + pv.rectSize.y), true);
	const ImU32 col = IM_COL32(255, 155, 40, 220);
	for (size_t i = 0; i < nb; ++i)
	{
		const float* g = smr->Globals().data() + i * 16;
		ImVec2 a;
		if (!pj.Project(glm::vec3(g[12], g[13], g[14]), a)) continue;
		const int par = sk->bones[i].parent;
		if (par >= 0)
		{
			const float* pg = smr->Globals().data() + par * 16;
			ImVec2 b;
			if (pj.Project(glm::vec3(pg[12], pg[13], pg[14]), b)) dl->AddLine(b, a, col, 1.5f);
		}
		dl->AddCircleFilled(a, 2.5f, col);
	}
	dl->PopClipRect();
}

// The keyable dimension of a reflected field type (0 = not keyable).
int EditorPropDim(nuke::FT t)
{
	using nuke::FT;
	switch (t)
	{
		case FT::Bool: case FT::Int: case FT::Float: case FT::Double: return 1;
		case FT::Vec2: return 2;
		case FT::Vec3: return 3;
		case FT::Vec4: case FT::Quat: case FT::Color: return 4;
		default: return 0;
	}
}

// The classes a track can address: reflected components out of the shared registry (the very
// list the inspector's Add Component builds), plus every class the scripting backends host.
// A backend names its own host component and its own classes — the editor knows no language,
// no component name and no file extension. Script classes are addressed "<Host>:<selector>";
// the host is what carries the props at runtime.
struct EditorClassEntry { std::string host, sel, label, group, icon; };

std::vector<EditorClassEntry> EditorScriptClasses()
{
	std::vector<EditorClassEntry> out;
	for (nuke::iScript* sv : nuke::GetServices<nuke::iScript>())
	{
		if (!sv) continue;
		const std::string host = sv->HostComponent() ? sv->HostComponent() : "";
		const std::string lang = sv->Language() ? sv->Language() : "";
		if (host.empty()) continue;               // this backend has no component-hosted classes
		int need = sv->ListClasses(nullptr, 0);
		if (need <= 0) continue;
		std::string raw((size_t)need, '\0');
		sv->ListClasses(&raw[0], need);
		for (size_t st = 0; st < raw.size(); )
		{
			size_t nl = raw.find('\n', st);
			if (nl == std::string::npos) nl = raw.size();
			std::string cls = raw.substr(st, nl - st);
			st = nl + 1;
			while (!cls.empty() && (cls.back() == '\r' || cls.back() == '\0')) cls.pop_back();
			if (cls.empty()) continue;
			// display name: the last path element without its extension, when the class IS a file
			std::string label = cls;
			const size_t slash = label.find_last_of("/\\");
			if (slash != std::string::npos) label = label.substr(slash + 1);
			const size_t dot = label.find_last_of('.');
			if (dot != std::string::npos && dot > 0) label = label.substr(0, dot);
			out.push_back({ host, cls, label, lang, sv->Icon() ? sv->Icon() : "" });
		}
	}
	return out;
}

// The props of one script class, asked of the backend that hosts it — matched by the host
// component it named, never by a language the editor would have to know.
std::vector<nuke::ScriptProp> EditorScriptClassProps(const std::string& host, const std::string& sel)
{
	std::vector<nuke::ScriptProp> out;
	for (nuke::iScript* sv : nuke::GetServices<nuke::iScript>())
	{
		if (!sv || !sv->HostComponent() || host != sv->HostComponent()) continue;
		int need = sv->ListClassProps(sel.c_str(), nullptr, 0);
		if (need <= 0) return out;
		std::string raw((size_t)need, '\0');
		sv->ListClassProps(sel.c_str(), &raw[0], need);
		for (size_t st = 0; st < raw.size(); )
		{
			size_t nl = raw.find('\n', st);
			if (nl == std::string::npos) nl = raw.size();
			const std::string line = raw.substr(st, nl - st);
			st = nl + 1;
			const size_t tab = line.find('\t');
			if (tab == std::string::npos) continue;
			nuke::ScriptProp sp;
			sp.name = line.substr(0, tab);
			const std::string kind = line.substr(tab + 1);
			sp.type = kind == "bool" ? nuke::FT::Bool : kind == "string" ? nuke::FT::String : nuke::FT::Double;
			if (!sp.name.empty()) out.push_back(sp);
		}
		return out;
	}
	return out;
}

// Reflected component/prop dropdowns: one searchable list of every class a track can address —
// components, C# classes, Lua scripts. Returns true when a prop was picked this frame.
bool EditorPropPicker(const char* id, float compW, float propW,
                      std::string& comp, std::string& prop, int* outDim)
{
	bool chosen = false;
	static const char kSep = ':';
	// A popup lives across MANY frames — a list built in the frame of the click is empty on every
	// frame that actually draws it. Cached per picker, refreshed when the popup appears.
	static std::map<std::string, std::vector<EditorClassEntry>> classCache;
	std::vector<EditorClassEntry>& classes = classCache[id ? id : ""];
	auto splitKey = [](const std::string& key, std::string& host, std::string& sel) -> bool
	{
		const size_t p = key.find(kSep);
		if (p == std::string::npos) return false;
		host = key.substr(0, p); sel = key.substr(p + 1);
		return true;
	};
	auto icontains = [](const std::string& hay, const std::string& needle)
	{
		if (needle.empty()) return true;
		std::string h = hay, n = needle;
		for (char& c : h) c = (char)tolower((unsigned char)c);
		for (char& c : n) c = (char)tolower((unsigned char)c);
		return h.find(n) != std::string::npos;
	};

	ImGui::PushID(id);

	// ---- class: one searchable, grouped popup over every class a track can address ----------
	{
		// Button label without asking any backend: the selector's own tail reads fine, and the
		// list (which does ask) is only built when the popup opens.
		std::string label = "(class)";
		if (!comp.empty())
		{
			std::string h, sl;
			label = splitKey(comp, h, sl) ? sl : comp;
			const size_t slash = label.find_last_of("/\\");
			if (slash != std::string::npos) label = label.substr(slash + 1);
		}
		if (ImGui::Button((label + "##compbtn").c_str(), ImVec2(compW, 0)))
			ImGui::OpenPopup("##comppick");
		if (ImGui::IsItemHovered() && !comp.empty()) ImGui::SetTooltip("%s", comp.c_str());
		if (ImGui::BeginPopup("##comppick"))
		{
			static char filter[64] = "";
			if (ImGui::IsWindowAppearing())
			{
				filter[0] = 0;
				ImGui::SetKeyboardFocusHere();
				classes = EditorScriptClasses();   // asked once per open, kept for the popup's life
			}
			ImGui::SetNextItemWidth(340);
			ImGui::InputTextWithHint("##f", ICON_LC_SEARCH " filter", filter, sizeof(filter));
			ImGui::Separator();
			ImGui::BeginChild("##list", ImVec2(340, 460));
			int shown = 0;
			auto pick = [&](const std::string& key)
			{
				if (comp != key) prop.clear();
				comp = key;
				ImGui::CloseCurrentPopup();
			};
			// script classes first — they are what a game actually animates, and a long
			// component list would otherwise push them out of sight
			std::vector<std::string> groups;
			for (const EditorClassEntry& e : classes)
				if (std::find(groups.begin(), groups.end(), e.group) == groups.end()) groups.push_back(e.group);
			std::sort(groups.begin(), groups.end());
			for (const std::string& g : groups)
			{
				bool head = false;
				for (const EditorClassEntry& e : classes)
				{
					if (e.group != g) continue;
					if (!icontains(e.label, filter) && !icontains(e.sel, filter)) continue;
					if (!head) { ImGui::SeparatorText(g.empty() ? "Scripts" : g.c_str()); head = true; }
					++shown;
					const std::string key = e.host + kSep + e.sel;
					// same class name can exist in two languages — the ID must be the KEY
					const std::string row = (e.icon.empty() ? std::string(ICON_FT_DEFAULT) : e.icon)
					                      + "  " + e.label + "##" + key;
					if (ImGui::Selectable(row.c_str(), comp == key)) pick(key);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s  (through %s)", e.sel.c_str(), e.host.c_str());
				}
			}
			// components, grouped by their reflected category ("" = Other)
			std::map<std::string, std::vector<std::string>> byCat;
			for (nuke::TypeInfo* ti : nuke::Registry_All())
			{
				if (!ti || !ti->create || !nuke::Registry_IsComponentType(ti)) continue;
				if (!icontains(ti->name, filter)) continue;
				byCat[ti->category.empty() ? std::string("Other") : ti->category].push_back(ti->name);
			}
			for (auto& kv : byCat) std::sort(kv.second.begin(), kv.second.end());
			for (auto& kv : byCat)
			{
				ImGui::SeparatorText(kv.first.c_str());
				for (const std::string& tn : kv.second)
				{
					++shown;
					const std::string row = std::string(ICON_LC_COMPONENT) + "  " + tn + "##comp:" + tn;
					if (ImGui::Selectable(row.c_str(), comp == tn)) pick(tn);
				}
			}
			if (!shown) ImGui::TextDisabled("Nothing matches the filter.");
			ImGui::EndChild();
			ImGui::EndPopup();
		}
	}

	// ---- prop: same treatment, filtered over the chosen class ------------------------------
	ImGui::SameLine();
	const bool haveComp = !comp.empty();
	if (!haveComp) ImGui::BeginDisabled();
	if (ImGui::Button(((prop.empty() ? std::string("(prop)") : prop) + "##propbtn").c_str(), ImVec2(propW, 0)))
		ImGui::OpenPopup("##proppick");
	if (!haveComp) ImGui::EndDisabled();
	if (ImGui::BeginPopup("##proppick"))
	{
		static char pfilter[64] = "";
		if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
		ImGui::SetNextItemWidth(260);
		ImGui::InputTextWithHint("##pf", ICON_LC_SEARCH " filter", pfilter, sizeof(pfilter));
		ImGui::Separator();
		ImGui::BeginChild("##plist", ImVec2(260, 320));
		std::string host, sel;
		if (splitKey(comp, host, sel))
		{
			// asked once per popup open, straight from the backend that owns the class
			static std::string cachedKey;
			static std::vector<nuke::ScriptProp> cached;
			if (ImGui::IsWindowAppearing() || cachedKey != comp)
			{
				cachedKey = comp;
				cached = EditorScriptClassProps(host, sel);
			}
			for (const nuke::ScriptProp& sp : cached)
			{
				if (!icontains(sp.name, pfilter)) continue;
				if (ImGui::Selectable(sp.name.c_str(), prop == sp.name))
				{
					prop = sp.name;
					if (outDim) *outDim = 1;   // script props are scalars
					chosen = true;
					ImGui::CloseCurrentPopup();
				}
			}
			if (cached.empty()) ImGui::TextDisabled("This class exposes no keyable props.");
		}
		else
			for (nuke::TypeInfo* ti = Registry_Find(comp); ti; ti = Registry_Find(ti->base))
				for (const nuke::Field& f : ti->fields)
				{
					const int dim = EditorPropDim(f.type);
					if (!dim || f.hidden || !icontains(f.name, pfilter)) continue;
					if (ImGui::Selectable(f.name.c_str(), prop == f.name))
					{
						prop = f.name;
						if (outDim) *outDim = dim;
						chosen = true;
						ImGui::CloseCurrentPopup();
					}
				}
		ImGui::EndChild();
		ImGui::EndPopup();
	}
	ImGui::PopID();
	return chosen;
}

// Atom-path picker: pick a DESCENDANT of `root` (the atom the track is relative to) from a
// tree instead of typing "Muzzle/Flash" by hand. Empty path = the root atom itself.
bool EditorAtomPathPicker(const char* id, nuke::Atom* root, std::string& path, float width)
{
	bool changed = false;
	ImGui::PushID(id);
	const std::string label = path.empty() ? std::string(root ? "(this atom)" : "(no atom selected)") : path;
	if (ImGui::Button((label + "##pathbtn").c_str(), ImVec2(width, 0))) ImGui::OpenPopup("##pathpick");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Path is relative to the atom the clip plays on%s",
		                  root ? "" : " — select that atom in the hierarchy first");
	if (ImGui::BeginPopup("##pathpick"))
	{
		if (!root) ImGui::TextDisabled("Select the character's atom in the hierarchy.");
		else
		{
			ImGui::TextDisabled("relative to %s", root->name.c_str());
			ImGui::Separator();
			ImGui::BeginChild("##ptree", ImVec2(300, 300));
			if (ImGui::Selectable("(this atom)", path.empty())) { path.clear(); changed = true; ImGui::CloseCurrentPopup(); }
			std::function<void(nuke::Atom*, const std::string&)> walk =
				[&](nuke::Atom* a, const std::string& prefix)
			{
				for (nuke::Atom* ch : a->children)
				{
					if (!ch) continue;
					const std::string p = prefix.empty() ? ch->name : prefix + "/" + ch->name;
					const bool leaf = ch->children.empty();
					ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen
					                      | ImGuiTreeNodeFlags_SpanAvailWidth
					                      | (leaf ? ImGuiTreeNodeFlags_Leaf : 0)
					                      | (path == p ? ImGuiTreeNodeFlags_Selected : 0);
					const bool open = ImGui::TreeNodeEx((void*)ch, fl, "%s", ch->name.c_str());
					if (ImGui::IsItemClicked()) { path = p; changed = true; ImGui::CloseCurrentPopup(); }
					if (open) { walk(ch, p); ImGui::TreePop(); }
				}
			};
			walk(root, "");
			ImGui::EndChild();
		}
		ImGui::EndPopup();
	}
	ImGui::PopID();
	return changed;
}

// The component TYPE a prop-track "comp" field addresses ("CSharpScript:Foo" -> "CSharpScript").
std::string EditorPropHostComponent(const std::string& comp)
{
	const size_t p = comp.find(':');
	return p == std::string::npos ? comp : comp.substr(0, p);
}

// ---- clip metadata snapshots (undo/redo; the binary clip has no ToString) -------------------

std::string EditorAnimMetaJson(const AnimClip* c)
{
	nlohmann::json j;
	j["duration"] = c->duration;
	nlohmann::json ev = nlohmann::json::array();
	for (const AnimClip::Event& e : c->events) ev.push_back({ { "t", e.t }, { "name", e.name } });
	j["events"] = ev;
	nlohmann::json ns = nlohmann::json::array();
	for (const AnimClip::Notify& n : c->notifies)
		ns.push_back({ { "t", n.t }, { "type", n.type }, { "name", n.name }, { "asset", n.asset },
		               { "socket", n.socket }, { "a", n.a }, { "b", n.b }, { "c", n.c } });
	j["notifies"] = ns;
	auto keysJson = [](const std::vector<AnimClip::Key>& keys)
	{
		nlohmann::json a = nlohmann::json::array();
		for (const AnimClip::Key& k : keys) a.push_back({ k.t, k.v[0], k.v[1], k.v[2], k.v[3] });
		return a;
	};
	nlohmann::json cs = nlohmann::json::array();
	for (const AnimClip::Curve& cu : c->curves) cs.push_back({ { "name", cu.name }, { "keys", keysJson(cu.keys) } });
	j["curves"] = cs;
	nlohmann::json ps = nlohmann::json::array();
	for (const AnimClip::PropTrack& p : c->propTracks)
		ps.push_back({ { "path", p.path }, { "comp", p.comp }, { "prop", p.prop }, { "dim", p.dim },
		               { "keys", keysJson(p.keys) } });
	j["propTracks"] = ps;
	return j.dump();
}

void EditorAnimMetaLoad(AnimClip* c, const std::string& json)
{
	try
	{
		nlohmann::json j = nlohmann::json::parse(json);
		c->duration = j.value("duration", c->duration);
		auto keysLoad = [](const nlohmann::json& a)
		{
			std::vector<AnimClip::Key> keys;
			for (const auto& kj : a)
			{
				AnimClip::Key k;
				k.t = kj[0].get<float>();
				for (int i = 0; i < 4; ++i) k.v[i] = kj[1 + i].get<float>();
				keys.push_back(k);
			}
			return keys;
		};
		c->events.clear();
		for (const auto& e : j["events"]) c->events.push_back({ e.value("t", 0.0f), e.value("name", std::string()) });
		c->notifies.clear();
		for (const auto& n : j["notifies"])
		{
			AnimClip::Notify nf;
			nf.t = n.value("t", 0.0f); nf.type = n.value("type", 0);
			nf.name = n.value("name", ""); nf.asset = n.value("asset", ""); nf.socket = n.value("socket", "");
			nf.a = n.value("a", 0.0f); nf.b = n.value("b", 0.0f); nf.c = n.value("c", 0.0f);
			c->notifies.push_back(nf);
		}
		c->curves.clear();
		for (const auto& cu : j["curves"]) c->curves.push_back({ cu.value("name", ""), keysLoad(cu["keys"]) });
		c->propTracks.clear();
		for (const auto& p : j["propTracks"])
		{
			AnimClip::PropTrack tr;
			tr.path = p.value("path", ""); tr.comp = p.value("comp", ""); tr.prop = p.value("prop", "");
			tr.dim = p.value("dim", 1);
			tr.keys = keysLoad(p["keys"]);
			c->propTracks.push_back(tr);
		}
	}
	catch (const std::exception&) {}
}

namespace {

// A generic diamond-marker row (events/notifies aren't AnimClip::Key). Returns the clicked
// index (-2 none); dragging retimes, right-click deletes, double-click on the row adds.
struct MarkerRow
{
	int   count = 0;
	float x0 = 0, y = 0, pps = 80;
	double dur = 1;
	std::function<float(int)>        getT;
	std::function<void(int, float)>  setT;
	std::function<void(int)>         del;
	std::function<void(double)>      add;
	std::function<ImU32(int)>        color;
};
int DrawMarkerRow(const char* id, MarkerRow& r, bool& edited)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	int hit = -2;
	ImGui::PushID(id);
	// row backdrop doubles as the add-target (double-click); markers overlap it
	ImGui::SetCursorScreenPos(ImVec2(r.x0, r.y - 8));
	ImGui::SetNextItemAllowOverlap();
	ImGui::InvisibleButton("##row", ImVec2(std::max(60.0f, (float)(r.dur * r.pps) + 20.0f), 16));
	if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && r.add)
	{
		double t = (ImGui::GetIO().MousePos.x - r.x0) / r.pps;
		r.add(std::max(0.0, std::min((double)r.dur, t)));
		edited = true;
	}
	for (int i = 0; i < r.count; ++i)
	{
		const float kx = r.x0 + r.getT(i) * r.pps;
		const ImVec2 c(kx, r.y);
		ImGui::PushID(i);
		ImGui::SetCursorScreenPos(ImVec2(c.x - 5, c.y - 5));
		ImGui::InvisibleButton("##m", ImVec2(10, 10));
		const bool hov = ImGui::IsItemHovered();
		if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
		{
			float nt = (ImGui::GetIO().MousePos.x - r.x0) / r.pps;
			nt = std::max(0.0f, std::min((float)r.dur, nt));
			r.setT(i, nt);
			edited = true;
			hit = i;
		}
		if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && r.del)
		{
			r.del(i);
			edited = true;
			ImGui::PopID();
			ImGui::PopID();
			return -2;
		}
		if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) hit = i;
		const ImU32 col = r.color ? r.color(i) : IM_COL32(210, 170, 60, 255);
		dl->AddQuadFilled(ImVec2(c.x, c.y - 5), ImVec2(c.x + 5, c.y), ImVec2(c.x, c.y + 5), ImVec2(c.x - 5, c.y),
		                  hov ? IM_COL32(255, 230, 120, 255) : col);
		ImGui::PopID();
	}
	ImGui::PopID();
	return hit;
}

const ImU32 kNotifyColors[4] = {
	IM_COL32(210, 170, 60, 255),    // 0 Event
	IM_COL32(120, 200, 120, 255),   // 1 SpawnPrefab
	IM_COL32(120, 160, 240, 255),   // 2 Sound
	IM_COL32(230, 110, 110, 255),   // 3 Shake
};

}  // namespace

void EditorUI::DrawAnimEditor(AssetEditorWin& w)
{
	AnimClip* clip = w.anim;
	if (!clip) { ImGui::TextDisabled("Failed to load clip."); return; }
	AppInstance* app = AppInstance::GetSingleton();
	bool edited = false;

	// ---- preview rigs ----------------------------------------------------------------------
	if (w.anRigSkel.empty()) w.anRigSkel = clip->skelGuid;   // the rig wears the clip's skeleton
	Animator* an = EditorEnsureRig(this, w.pv, w.anAtomId, w.anRigSkel);
	if (an)
	{
		an->previewClip = clip;
		if (an->clipGuid != clip->guid) { an->clipGuid = clip->guid; an->playOnStart = true; an->loop = true; }
	}
	Animator* an2 = nullptr;
	if (!w.anRetargetSkel.empty())
	{
		if (!w.anPv2) w.anPv2 = AcquirePreview();
		an2 = EditorEnsureRig(this, w.anPv2, w.anAtomId2, w.anRetargetSkel);
		if (an2)
		{
			an2->previewClip = clip;   // its rig wears the TARGET skeleton -> plays retargeted
			if (an2->clipGuid != clip->guid) { an2->clipGuid = clip->guid; an2->playOnStart = true; an2->loop = true; }
		}
	}
	else if (w.anPv2)
	{
		if (w.anAtomId2) { w.anPv2->world->RemoveAtomById(w.anAtomId2); w.anAtomId2 = 0; }
		ReleasePreview(w.anPv2);
		w.anPv2 = nullptr;
	}

	// advance + drive: the editor OWNS the clock (speed 0 on the rig; time set every frame)
	if (w.anPlaying)
	{
		w.anTime += ImGui::GetIO().DeltaTime * w.anSpeed;
		if (clip->duration > 1e-6 && w.anTime >= clip->duration) w.anTime = fmod(w.anTime, clip->duration);
	}
	for (Animator* a : { an, an2 })
		if (a)
		{
			a->speed = 0.0f;
			a->SetClipTime(w.anTime);
			a->Update();
		}

	// F = frame the rig(s); scoped to this window, suppressed while typing/flying
	if ((ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
	     || ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
	    && !ImGui::GetIO().WantTextInput && !ImGui::IsMouseDown(ImGuiMouseButton_Right)
	    && ImGui::IsKeyPressed(ImGuiKey_F))
	{
		if (w.pv) FramePreview(*w.pv, EditorRigAtom(w.pv, w.anAtomId));
		if (w.anPv2) FramePreview(*w.anPv2, EditorRigAtom(w.anPv2, w.anAtomId2));
	}

	// ---- toolbar ---------------------------------------------------------------------------
	if (ImGui::Button(w.anPlaying ? ICON_LC_PAUSE : ICON_LC_PLAY)) w.anPlaying = !w.anPlaying;
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_SQUARE)) { w.anPlaying = false; w.anTime = 0; }
	ImGui::SameLine();
	ImGui::SetNextItemWidth(90);
	float ft = (float)w.anTime;
	if (ImGui::DragFloat("##antime", &ft, 0.01f, 0.0f, (float)clip->duration, "%.2fs"))
		w.anTime = ft;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(70);
	ImGui::DragFloat("##anspeed", &w.anSpeed, 0.02f, 0.05f, 4.0f, "x%.2f");
	ImGui::SameLine();
	ImGui::TextDisabled("len %.2fs", clip->duration);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(170);
	AssetPicker("##anskel", w.anRigSkel, "skeleton");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Preview rig: EVERY mesh of this skeleton (from the clip by default)");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(170);
	AssetPicker("##anretskel", w.anRetargetSkel, "skeleton");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Retarget preview: target skeleton (side-by-side)");
	if (!w.anRetargetSkel.empty())
	{
		ImGui::SameLine();
		if (ImGui::Button(ICON_LC_BONE " Bake"))
		{
			boost::system::error_code ec;
			bfs::path rel = bfs::relative(bfs::path(w.path).parent_path(),
			                              bfs::path(app->ResolveContent("")), ec);
			Skeleton* tsk = ResDB::getSingleton()->GetSkeleton(w.anRetargetSkel);
			const std::string stem = bfs::path(w.path).stem().string()
			                       + "_" + (tsk ? tsk->name : "retarget") + ".nuanim";
			const std::string out = (rel / stem).generic_string();
			const std::string g = Retargeter::Bake(clip->guid, w.anRetargetSkel, out);
			StatusBar::Set("anim", g.empty() ? "Bake failed" : ("Baked " + stem));
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Write the retargeted clip as a .nuanim on the target skeleton");
	}
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_SAVE " Save")) { clip->SaveToFile(w.path); w.dirty = false; }

	// ---- previews + right panel ------------------------------------------------------------
	const float panelW = 300.0f;
	const float tlH = 190.0f;
	ImVec2 avail = ImGui::GetContentRegionAvail();
	const float viewH = std::max(120.0f, avail.y - tlH);
	ImGui::BeginChild("##anview", ImVec2(avail.x - panelW - 6, viewH), false,
	                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	{
		ImVec2 va = ImGui::GetContentRegionAvail();
		if (an2 && w.anPv2)
		{
			ImVec2 half((va.x - 4) * 0.5f, va.y);
			if (w.pv) DrawPreviewImage(*w.pv, half);
			EditorDrawBoneOverlay(*w.pv, EditorRigAtom(w.pv, w.anAtomId));
			ImGui::SameLine();
			DrawPreviewImage(*w.anPv2, half);
			EditorDrawBoneOverlay(*w.anPv2, EditorRigAtom(w.anPv2, w.anAtomId2));
		}
		else if (w.pv)
		{
			if (w.anRigSkel.empty() || EditorMeshesForSkeleton(w.anRigSkel).empty())
				ImGui::TextDisabled("No skinned mesh found for this skeleton — pick another rig above.");
			else
			{
				DrawPreviewImage(*w.pv, va);
				EditorDrawBoneOverlay(*w.pv, EditorRigAtom(w.pv, w.anAtomId));
			}
		}
	}
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("##anprops", ImVec2(panelW, viewH), true);
	{
		if (w.anSelNotify >= 0 && w.anSelNotify < (int)clip->notifies.size())
		{
			AnimClip::Notify& n = clip->notifies[w.anSelNotify];
			ImGui::SeparatorText("Notify");
			static const char* kTypes = "Event\0Spawn Prefab\0Sound\0Camera Shake\0";
			if (ImGui::Combo("Type", &n.type, kTypes)) edited = true;
			char nb[128]; strncpy(nb, n.name.c_str(), sizeof(nb)); nb[sizeof(nb) - 1] = 0;
			if (ImGui::InputText("Name", nb, sizeof(nb))) { n.name = nb; edited = true; }
			float nt = n.t;
			if (ImGui::DragFloat("Time", &nt, 0.01f, 0.0f, (float)clip->duration, "%.2fs")) { n.t = nt; edited = true; }
			if (n.type == 1)
			{
				char ab[256]; strncpy(ab, n.asset.c_str(), sizeof(ab)); ab[sizeof(ab) - 1] = 0;
				if (ImGui::InputText("Prefab", ab, sizeof(ab))) { n.asset = ab; edited = true; }
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Content-relative .nuprefab path");
				char sb[64]; strncpy(sb, n.socket.c_str(), sizeof(sb)); sb[sizeof(sb) - 1] = 0;
				if (ImGui::InputText("Socket", sb, sizeof(sb))) { n.socket = sb; edited = true; }
				if (ImGui::DragFloat("Lifetime", &n.a, 0.05f, 0.0f, 60.0f, "%.2fs")) edited = true;
			}
			else if (n.type == 2)
			{
				if (AssetPicker("Sound", n.asset, "audio")) edited = true;
				char sb[64]; strncpy(sb, n.socket.c_str(), sizeof(sb)); sb[sizeof(sb) - 1] = 0;
				if (ImGui::InputText("Socket", sb, sizeof(sb))) { n.socket = sb; edited = true; }
				if (ImGui::DragFloat("Volume", &n.a, 0.02f, 0.0f, 2.0f)) edited = true;
			}
			else if (n.type == 3)
			{
				if (ImGui::DragFloat("Amplitude", &n.a, 0.01f, 0.0f, 5.0f)) edited = true;
				if (ImGui::DragFloat("Frequency", &n.b, 0.1f, 0.1f, 60.0f, "%.1f Hz")) edited = true;
				if (ImGui::DragFloat("Duration", &n.c, 0.02f, 0.05f, 10.0f, "%.2fs")) edited = true;
			}
			if (ImGui::Button(ICON_LC_TRASH_2 " Remove Notify"))
			{
				clip->notifies.erase(clip->notifies.begin() + w.anSelNotify);
				w.anSelNotify = -1;
				edited = true;
			}
		}
		else if (w.anSelEvent >= 0 && w.anSelEvent < (int)clip->events.size())
		{
			AnimClip::Event& e = clip->events[w.anSelEvent];
			ImGui::SeparatorText("Event");
			char eb[128]; strncpy(eb, e.name.c_str(), sizeof(eb)); eb[sizeof(eb) - 1] = 0;
			if (ImGui::InputText("Name", eb, sizeof(eb))) { e.name = eb; edited = true; }
			float et = e.t;
			if (ImGui::DragFloat("Time", &et, 0.01f, 0.0f, (float)clip->duration, "%.2fs")) { e.t = et; edited = true; }
			if (ImGui::Button(ICON_LC_TRASH_2 " Remove Event"))
			{
				clip->events.erase(clip->events.begin() + w.anSelEvent);
				w.anSelEvent = -1;
				edited = true;
			}
		}
		else ImGui::TextDisabled("Select a marker on the timeline.");

		ImGui::SeparatorText("Curves");
		for (int i = 0; i < (int)clip->curves.size(); ++i)
		{
			ImGui::PushID(i);
			if (ImGui::Selectable(clip->curves[i].name.c_str(), w.anSelCurve == i))
			{
				w.anSelCurve = i; w.anSelProp = -1;
			}
			if (ImGui::BeginPopupContextItem("##curvectx"))
			{
				if (ImGui::MenuItem(ICON_LC_TRASH_2 " Remove Curve"))
				{
					clip->curves.erase(clip->curves.begin() + i);
					if (w.anSelCurve == i) w.anSelCurve = -1;
					edited = true;
				}
				ImGui::EndPopup();
			}
			ImGui::PopID();
		}
		static char newCurve[64] = "";
		ImGui::SetNextItemWidth(-70);
		ImGui::InputTextWithHint("##newcurve", "curve name", newCurve, sizeof(newCurve));
		ImGui::SameLine();
		if (ImGui::Button(ICON_LC_PLUS "##addcurve") && newCurve[0])
		{
			clip->AddCurveKey(newCurve, 0.0, 0.0);
			clip->AddCurveKey(newCurve, std::max(0.1, clip->duration), 0.0);
			w.anSelCurve = (int)clip->curves.size() - 1;
			newCurve[0] = 0;
			edited = true;
		}

		ImGui::SeparatorText("Prop Tracks");
		for (int i = 0; i < (int)clip->propTracks.size(); ++i)
		{
			AnimClip::PropTrack& tr = clip->propTracks[i];
			ImGui::PushID(1000 + i);
			const std::string label = (tr.path.empty() ? "." : tr.path) + "|" + tr.comp + "|" + tr.prop;
			if (ImGui::Selectable(label.c_str(), w.anSelProp == i)) { w.anSelProp = i; w.anSelCurve = -1; }
			if (ImGui::BeginPopupContextItem("##propctx"))
			{
				if (ImGui::MenuItem(ICON_LC_TRASH_2 " Remove Track"))
				{
					clip->propTracks.erase(clip->propTracks.begin() + i);
					if (w.anSelProp == i) w.anSelProp = -1;
					edited = true;
				}
				ImGui::EndPopup();
			}
			ImGui::PopID();
		}
		static std::string npPath;
		static std::string npComp, npProp;
		static int npDim = 1;
		EditorAtomPathPicker("##nppath", app->selectedInHieararchy, npPath, -1);
		EditorPropPicker("##npp", 150, 130, npComp, npProp, &npDim);
		ImGui::SameLine();
		if (ImGui::Button(ICON_LC_PLUS "##addprop") && !npComp.empty() && !npProp.empty())
		{
			clip->AddPropKey(npPath.c_str(), npComp, npProp, 0.0, 0.0);
			for (int i = 0; i < (int)clip->propTracks.size(); ++i)
				if (clip->propTracks[i].path == npPath && clip->propTracks[i].comp == npComp
				    && clip->propTracks[i].prop == npProp)
				{
					clip->propTracks[i].dim = npDim;
					w.anSelProp = i;
				}
			npComp.clear(); npProp.clear();
			edited = true;
		}
	}
	ImGui::EndChild();

	// ---- timeline --------------------------------------------------------------------------
	ImGui::BeginChild("##antl", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
	{
		const float pps = w.anZoom;
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float x0 = origin.x + 96;   // label gutter
		ImDrawList* dl = ImGui::GetWindowDrawList();

		// ruler + scrub
		{
			ImGui::SetCursorScreenPos(ImVec2(x0, origin.y));
			ImGui::InvisibleButton("##ruler", ImVec2(std::max(60.0f, (float)(clip->duration * pps) + 20.0f), 18));
			if (ImGui::IsItemActive() || (ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left)))
			{
				w.anTime = std::max(0.0, std::min(clip->duration,
				                                  (double)((ImGui::GetIO().MousePos.x - x0) / pps)));
				w.anPlaying = false;
			}
			for (double t = 0; t <= clip->duration + 1e-6; t += (pps >= 60 ? 0.25 : 1.0))
			{
				const float tx = x0 + (float)(t * pps);
				dl->AddLine(ImVec2(tx, origin.y + 10), ImVec2(tx, origin.y + 18), IM_COL32(120, 120, 130, 255));
				if (fmod(t, 1.0) < 1e-6)
				{
					char tb[16];
					snprintf(tb, sizeof(tb), "%.0fs", t);
					dl->AddText(ImVec2(tx + 2, origin.y), IM_COL32(150, 150, 160, 255), tb);
				}
			}
		}

		float y = origin.y + 30;
		auto rowLabel = [&](const char* txt)
		{
			dl->AddText(ImVec2(origin.x, y - 7), IM_COL32(170, 170, 180, 255), txt);
		};

		// events row
		rowLabel("Events");
		{
			MarkerRow r;
			r.count = (int)clip->events.size(); r.x0 = x0; r.y = y; r.pps = pps; r.dur = clip->duration;
			r.getT = [&](int i) { return clip->events[i].t; };
			r.setT = [&](int i, float t) { clip->events[i].t = t; };
			r.del  = [&](int i) { clip->events.erase(clip->events.begin() + i); w.anSelEvent = -1; };
			r.add  = [&](double t) { clip->AddEvent((float)t, "event"); };
			r.color = [&](int) { return IM_COL32(200, 200, 210, 255); };
			const int hit = DrawMarkerRow("##evrow", r, edited);
			if (hit >= 0) { w.anSelEvent = hit; w.anSelNotify = -1; }
		}
		y += 22;

		// notifies row (colored by type)
		rowLabel("Notifies");
		{
			MarkerRow r;
			r.count = (int)clip->notifies.size(); r.x0 = x0; r.y = y; r.pps = pps; r.dur = clip->duration;
			r.getT = [&](int i) { return clip->notifies[i].t; };
			r.setT = [&](int i, float t) { clip->notifies[i].t = t; };
			r.del  = [&](int i) { clip->notifies.erase(clip->notifies.begin() + i); w.anSelNotify = -1; };
			r.add  = [&](double t) { clip->AddNotify(t, 0, "notify", "", "", 0, 0, 0); };
			r.color = [&](int i) { return kNotifyColors[clip->notifies[i].type & 3]; };
			const int hit = DrawMarkerRow("##ntrow", r, edited);
			if (hit >= 0) { w.anSelNotify = hit; w.anSelEvent = -1; }
		}
		y += 22;

		// curve / prop key rows
		bool rowsEdited = false;
		if (w.anSelCurve >= 0 && w.anSelCurve < (int)clip->curves.size())
		{
			rowLabel(clip->curves[w.anSelCurve].name.c_str());
			double dragT = -1;
			SharedKeyRow("##ckeys", &clip->curves[w.anSelCurve].keys, x0, y, pps, clip->duration, &dragT, rowsEdited);
			y += 22;
		}
		if (w.anSelProp >= 0 && w.anSelProp < (int)clip->propTracks.size())
		{
			rowLabel(clip->propTracks[w.anSelProp].prop.c_str());
			double dragT = -1;
			SharedKeyRow("##pkeys", &clip->propTracks[w.anSelProp].keys, x0, y, pps, clip->duration, &dragT, rowsEdited);
			y += 22;
		}
		edited = edited || rowsEdited;

		// playhead
		{
			const float px = x0 + (float)(w.anTime * pps);
			dl->AddLine(ImVec2(px, origin.y), ImVec2(px, y + 4), IM_COL32(255, 80, 80, 255), 1.5f);
		}

		// curve strip for the selected curve/prop track
		ImGui::SetCursorScreenPos(ImVec2(origin.x, y + 8));
		std::vector<AnimClip::Key>* stripKeys = nullptr;
		int stripDim = 1;
		if (w.anSelCurve >= 0 && w.anSelCurve < (int)clip->curves.size())
			stripKeys = &clip->curves[w.anSelCurve].keys;
		else if (w.anSelProp >= 0 && w.anSelProp < (int)clip->propTracks.size())
		{
			stripKeys = &clip->propTracks[w.anSelProp].keys;
			stripDim = std::max(1, clip->propTracks[w.anSelProp].dim);
		}
		if (stripKeys)
		{
			const float stripH = std::max(56.0f, ImGui::GetContentRegionAvail().y - 6);
			if (DrawKeysCurve("##anstrip", *stripKeys, stripDim, stripH)) edited = true;
		}
		else
		{
			ImGui::TextDisabled("Select a curve or prop track for the value strip. Double-click a row to add markers;");
			ImGui::TextDisabled("drag to retime, right-click to delete. Zoom: Ctrl+wheel on the timeline.");
		}

		// zoom
		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::GetIO().KeyCtrl
		    && ImGui::GetIO().MouseWheel != 0.0f)
			w.anZoom = std::max(20.0f, std::min(400.0f, w.anZoom * (1.0f + ImGui::GetIO().MouseWheel * 0.12f)));
	}
	ImGui::EndChild();

	if (edited) { w.dirty = true; w.editedNow = true; }
}
