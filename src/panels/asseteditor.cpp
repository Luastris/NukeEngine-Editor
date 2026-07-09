// Asset editors (user request on top of 2.2): each material / mesh / prefab opens its
// OWN window with a live 3D view and type-specific editing, saving back to the asset
// file. Built on POOLED preview scenes — tiny worlds (sky + shadowless sun + one mesh
// atom + camera into an own RT) that render through the hook BEFORE the live scene, so
// their lights/sky/TLAS never taint the viewport. Pooled because the render seam has no
// destroyRenderTarget: closing an editor returns its scene for reuse.
#include <editor/editorui.h>
#include <API/Model/Material.h>
#include <API/Model/Light.h>
#include <API/Model/Environment.h>
#include <API/Model/Prefab.h>
#include <boost/filesystem.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>   // prefab gizmo: decompose the manipulated matrix
#include <glm/gtc/type_ptr.hpp>
namespace bfs = boost::filesystem;

// ---------------------------------------------------------------------------
// Preview-scene pool
// ---------------------------------------------------------------------------

EditorUI::PreviewScene* EditorUI::AcquirePreview()
{
	for (PreviewScene* s : pvPool)
		if (!s->inUse)
		{
			s->inUse = true;
			s->yaw = 0.7f; s->pitch = 0.35f; s->dist = 0.0f;
			return s;
		}
	iRender* r = AppInstance::GetSingleton()->render;
	if (!r) return nullptr;

	PreviewScene* s = new PreviewScene();
	s->rt = r->createRenderTarget(384, 384);
	s->world = new World();
	s->world->name = "Asset Preview";
	s->world->auxiliary = true;   // skip global heavy passes (RT TLAS) — see World::Render

	Atom* env = new Atom("PreviewEnv");
	env->AddComponent(new Environment());
	s->world->Add(env);

	Atom* sun = new Atom("PreviewSun");
	Light* l = new Light();
	l->type = 0;                 // directional
	l->castShadows = false;      // no shadow passes for previews
	sun->AddComponent(l);
	sun->GetTransform().SetEulerDeg(Vector3(50, -30, 0));
	s->world->Add(sun);

	s->meshAtom = new Atom("PreviewMesh");
	s->mr = new MeshRenderer();
	s->meshAtom->AddComponent(s->mr);
	s->world->Add(s->meshAtom);

	Atom* cam = new Atom("PreviewCam");
	s->cam = new Camera();
	cam->AddComponent(s->cam);
	s->cam->renderTarget = s->rt;
	s->world->Add(cam);

	s->inUse = true;
	pvPool.push_back(s);
	return s;
}

void EditorUI::ReleasePreview(PreviewScene* s)
{
	if (!s) return;
	// Strip what the user staged; the scene skeleton (env/sun/mesh atom/camera) is reused.
	s->mr->mesh = nullptr;
	s->mr->meshGuid.clear();
	s->mr->matGuid.clear();
	if (s->mr->mat) { delete s->mr->mat; s->mr->mat = nullptr; }
	s->visible = false;
	s->inUse = false;
}

// Union of the subtree's mesh bounds (spheres at global positions) -> center/radius.
static void SubtreeBounds(nuke::Atom* a, nuke::Vector3& c, float& r, bool& any)
{
	if (!a) return;
	if (nuke::MeshRenderer* mr = a->GetComponent<nuke::MeshRenderer>())
		if (mr->mesh)
		{
			mr->mesh->EnsureBounds();
			const float* mn = mr->mesh->aabbMin;
			const float* mx = mr->mesh->aabbMax;
			nuke::Vector3 lc((mn[0] + mx[0]) * 0.5, (mn[1] + mx[1]) * 0.5, (mn[2] + mx[2]) * 0.5);
			nuke::Vector3 sc = a->GetTransform().globalScale();
			nuke::Vector3 gp = a->GetTransform().globalPosition();
			const double sm = std::max(std::abs(sc.x), std::max(std::abs(sc.y), std::abs(sc.z)));
			const float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
			const float lr = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz) * (float)sm;
			nuke::Vector3 wc(gp.x + lc.x * sc.x, gp.y + lc.y * sc.y, gp.z + lc.z * sc.z);
			if (!any) { c = wc; r = lr; any = true; }
			else
			{
				// grow the sphere to include the new one
				const double ddx = wc.x - c.x, ddy = wc.y - c.y, ddz = wc.z - c.z;
				const float d = (float)std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
				const float nr = std::max(r, d + lr);
				c = Vector3((c.x + wc.x) * 0.5, (c.y + wc.y) * 0.5, (c.z + wc.z) * 0.5);
				r = nr;
			}
		}
	for (nuke::Atom* ch : a->children) SubtreeBounds(ch, c, r, any);
}

void EditorUI::FramePreview(PreviewScene& s, Atom* subtree)
{
	bool any = false;
	Vector3 c(0, 0, 0); float r = 1.0f;
	if (subtree) SubtreeBounds(subtree, c, r, any);
	else         SubtreeBounds(s.meshAtom, c, r, any);
	s.center = any ? c : Vector3(0, 0, 0);
	s.radius = any ? std::max(r, 0.01f) : 1.0f;
	s.dist = 0.0f;   // re-derive from radius on next draw
}

void EditorUI::DrawPreviewImage(PreviewScene& s, ImVec2 size)
{
	iRender* r = AppInstance::GetSingleton()->render;
	// NOTE: Camera::Init fills `transform`, NOT `atom` — check/use the transform.
	if (!r || !s.cam || !s.cam->transform) return;
	// The hosting OS window is minimized: draw nothing, resize nothing, render nothing —
	// iconified sizes would churn the RT (and its per-camera buffers) for no one to see.
	if (ImGuiViewport* vp = ImGui::GetWindowViewport())
		if (vp->Flags & ImGuiViewportFlags_IsMinimized) return;

	// Auto-frame ONCE per staging (dist == 0): put the camera on the orbit sphere
	// looking at the bounds center. Afterwards the camera is FREE — the transform
	// persists and the viewport-style controls below move it.
	if (s.dist <= 0.0f)
	{
		s.dist = s.radius / std::tan((float)s.cam->fov * 0.5f * 0.01745329252f) * 1.5f;
		const double cp0 = std::cos(s.pitch), sp0 = std::sin(s.pitch);
		const double sy0 = std::sin(s.yaw),   cy0 = std::cos(s.yaw);
		Vector3 dir0(cp0 * sy0, sp0, cp0 * cy0);                 // center -> camera
		Transform& ft = *s.cam->transform;
		ft.position = Vector3(s.center.x + dir0.x * s.dist, s.center.y + dir0.y * s.dist, s.center.z + dir0.z * s.dist);
		const double fp = std::asin(dir0.y);                     // forward = -dir
		const double fyw = std::atan2(-dir0.x, -dir0.z);
		ft.SetEulerDeg(Vector3(fp * 57.29577951308232, fyw * 57.29577951308232, 0.0));
	}

	// Fill EXACTLY the given rect (any aspect) — no fixed square, no dead space.
	float pw = size.x, ph = size.y;
	if (pw < 64.0f) pw = 64.0f;
	if (ph < 64.0f) ph = 64.0f;

	// The RT follows the on-screen size, but QUANTIZED (16px grid) and DEBOUNCED
	// (stable for several frames): resizing every frame shows a permanently EMPTY
	// texture (the world fills it only on the NEXT frame's render pass) and churns
	// GPU resources hard — an ImGui scrollbar flicker used to oscillate the size at
	// 60 Hz, black preview + dynamic-heap exhaustion included.
	const int qw = std::max(64, ((int)pw + 15) / 16 * 16);
	const int qh = std::max(64, ((int)ph + 15) / 16 * 16);
	if (qw != s.rtW || qh != s.rtH)
	{
		if (qw == s.wantW && qh == s.wantH) ++s.wantFrames;
		else { s.wantW = qw; s.wantH = qh; s.wantFrames = 1; }
		if (s.wantFrames >= 5 || s.rtW <= 0)   // settled (or first ever size): apply
		{
			r->resizeRenderTarget(s.rt, qw, qh);
			s.rtW = qw; s.rtH = qh;
			s.wantFrames = 0;
		}
	}
	else s.wantFrames = 0;
	uint64_t tex = r->getRenderTargetTexture(s.rt);
	if (!tex) return;

	// A PLAIN Image, like the scene viewport: it has no interactive identity, so
	// ImGuizmo can grab clicks over it (an InvisibleButton here used to steal them —
	// the gizmo never activated). Window-dragging from content is off globally
	// (ConfigWindowsMoveFromTitleBarOnly), so hover-based orbit is safe.
	const ImVec2 p0 = ImGui::GetCursorScreenPos();
	s.rectMin = p0; s.rectSize = ImVec2(pw, ph);   // the gizmo overlay targets this rect
	// Show the top-left (pw x ph) region of the (quantized) RT — 1:1 pixels, no squash.
	const ImVec2 uv1(std::min(1.0f, pw / (float)s.rtW), std::min(1.0f, ph / (float)s.rtH));
	ImGui::Image((ImTextureID)tex, ImVec2(pw, ph), ImVec2(0, 0), uv1);
	ImGuiIO& io = ImGui::GetIO();
	if (ImGui::IsItemHovered())
	{
		// The MAIN VIEWPORT's camera controls, scaled to the asset's size:
		//   RMB drag = look, MMB drag = pan, wheel = dolly, RMB + WASD/QE = fly (Shift faster).
		// LMB stays free for the gizmo and for picking (prefab editor).
		Transform* t = s.cam->transform;
		const float k = std::max(0.2f, s.radius);
		const float rotSpeed = 0.005f, panSpeed = 0.0025f * k, zoomSpeed = 0.25f * k;

		// Sync the look angles from the camera when the RMB drag STARTS (derived from
		// FORWARD — same idiom as the viewport, avoids euler-recompute snaps).
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			Vector3 f = t->direction();
			double len = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
			if (len > 1e-6) { f.x /= len; f.y /= len; f.z /= len; }
			double py = f.y; if (py > 1.0) py = 1.0; if (py < -1.0) py = -1.0;
			s.pitch = (float)std::asin(-py);
			s.yaw   = (float)std::atan2(f.x, f.z);
		}
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Right))
		{
			s.yaw   += io.MouseDelta.x * rotSpeed;
			s.pitch += io.MouseDelta.y * rotSpeed;
			const float lim = 1.55f;
			if (s.pitch >  lim) s.pitch =  lim;
			if (s.pitch < -lim) s.pitch = -lim;
			t->SetEulerDeg(Vector3(s.pitch * 57.29578f, s.yaw * 57.29578f, 0.0f));
		}
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
			t->position += t->right() * (double)(-io.MouseDelta.x * panSpeed)
			             + t->up()    * (double)( io.MouseDelta.y * panSpeed);
		if (io.MouseWheel != 0.0f)
			t->position += t->direction() * (double)(io.MouseWheel * zoomSpeed);

		// Free-flight while RMB is held (Unity/UE-style, like the scene viewport).
		if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
		{
			float fly = 2.5f * k * io.DeltaTime;
			if (io.KeyShift) fly *= 3.0f;
			if (ImGui::IsKeyDown(ImGuiKey_W)) t->position += t->direction() * (double) fly;
			if (ImGui::IsKeyDown(ImGuiKey_S)) t->position += t->direction() * (double)-fly;
			if (ImGui::IsKeyDown(ImGuiKey_D)) t->position += t->right()     * (double) fly;
			if (ImGui::IsKeyDown(ImGuiKey_A)) t->position += t->right()     * (double)-fly;
			if (ImGui::IsKeyDown(ImGuiKey_E)) t->position += t->up()        * (double) fly;
			if (ImGui::IsKeyDown(ImGuiKey_Q)) t->position += t->up()        * (double)-fly;
		}
	}

	s.visible = true;   // ask the render hook to draw this scene this frame
}

// ---------------------------------------------------------------------------
// Asset editor windows
// ---------------------------------------------------------------------------

static const char* kPreviewMeshGuid[] = { "builtin:sphere", "builtin:cube", "builtin:plane" };

static nuke::Atom* FindInSubtree(nuke::Atom* a, long id);   // defined below (used by the tree)

// ---------------------------------------------------------------------------
// Per-window undo/redo (prefab = subtree JSON snapshots, material = clones)
// ---------------------------------------------------------------------------

static const size_t kAeUndoCap = 64;

// Swap the live prefab subtree for a snapshot (ids are preserved by the atom
// serializer, so the selection survives when the atom still exists).
static void RestorePrefabState(EditorUI::AssetEditorWin& w, const std::string& json)
{
	if (w.prefabRoot) w.pv->world->RemoveAtomById((long)w.prefabRoot->id.id);
	w.prefabRoot = nuke::LoadAtomFromString(json);
	if (w.prefabRoot)
	{
		w.pv->world->Add(w.prefabRoot);
		if (!FindInSubtree(w.prefabRoot, w.prefabSelId))
			w.prefabSelId = (long)w.prefabRoot->id.id;
	}
	w.dirty = true;   // restored state differs from the file (editedNow NOT raised: this IS undo)
}

// ---------------------------------------------------------------------------
// Animation preview (3.1): a per-window mini-PIE — the ▶ toggle ticks the
// subtree's Animators; ■ swaps the pre-play snapshot back (transform animation
// mutates atom transforms, skinning swaps MeshRenderer meshes — both restored).
// ---------------------------------------------------------------------------

static bool SubtreeHasAnimator(nuke::Atom* a)
{
	if (!a) return false;
	if (a->GetComponent<nuke::Animator>()) return true;
	for (nuke::Atom* ch : a->children)
		if (SubtreeHasAnimator(ch)) return true;
	return false;
}

static void TickAnimators(nuke::Atom* a)
{
	if (!a) return;
	if (nuke::Animator* an = a->GetComponent<nuke::Animator>())
		if (an->enabled) an->Update();
	for (nuke::Atom* ch : a->children) TickAnimators(ch);
}

void EditorUI::TickAnimPreview(AssetEditorWin& w)
{
	if (w.prefabRoot) TickAnimators(w.prefabRoot);
}

void EditorUI::ToggleAnimPreview(AssetEditorWin& w)
{
	if (!w.animPlay)
	{
		if (!w.prefabRoot) return;
		w.animSnap = nuke::SaveAtomToString(w.prefabRoot);   // pose/structure before play
		w.animPlay = true;
	}
	else
	{
		w.animPlay = false;
		if (!w.animSnap.empty())
		{
			const bool wasDirty = w.dirty;   // restoring the pre-play state is NOT an edit
			RestorePrefabState(w, w.animSnap);
			w.dirty = wasDirty;
			w.animSnap.clear();
		}
	}
}

static void RestoreMaterialState(EditorUI::AssetEditorWin& w, nuke::Material* snap)
{
	if (!snap) return;
	delete w.mat;
	w.mat = snap->Clone();
	if (w.pv->mr->mat) delete w.pv->mr->mat;
	w.pv->mr->mat = w.mat->Clone();   // live preview follows
	w.dirty = true;   // (editedNow NOT raised: this IS undo/redo)
}

void EditorUI::AssetEditorUndo(AssetEditorWin& w)
{
	if (w.ext == ".nuprefab" && w.prefabRoot && !w.undoP.empty())
	{
		w.redoP.push_back(nuke::SaveAtomToString(w.prefabRoot));
		RestorePrefabState(w, w.undoP.back());
		w.undoP.pop_back();
		w.idleP = w.prefabRoot ? nuke::SaveAtomToString(w.prefabRoot) : "";
		w.editing = false;
	}
	else if (w.ext == ".numat" && w.mat && !w.undoM.empty())
	{
		w.redoM.push_back(w.mat->Clone());
		RestoreMaterialState(w, w.undoM.back());
		delete w.undoM.back(); w.undoM.pop_back();
		delete w.idleM; w.idleM = w.mat->Clone();
		w.editing = false;
	}
}

void EditorUI::AssetEditorRedo(AssetEditorWin& w)
{
	if (w.ext == ".nuprefab" && w.prefabRoot && !w.redoP.empty())
	{
		w.undoP.push_back(nuke::SaveAtomToString(w.prefabRoot));
		RestorePrefabState(w, w.redoP.back());
		w.redoP.pop_back();
		w.idleP = w.prefabRoot ? nuke::SaveAtomToString(w.prefabRoot) : "";
		w.editing = false;
	}
	else if (w.ext == ".numat" && w.mat && !w.redoM.empty())
	{
		w.undoM.push_back(w.mat->Clone());
		RestoreMaterialState(w, w.redoM.back());
		delete w.redoM.back(); w.redoM.pop_back();
		delete w.idleM; w.idleM = w.mat->Clone();
		w.editing = false;
	}
}

void EditorUI::OpenAssetEditor(const std::string& path)
{
	for (AssetEditorWin& w : assetEds)
		if (w.path == path) { w.wantFocus = true; return; }

	std::string ext = bfs::path(path).extension().string();
	for (char& c : ext) c = (char)std::tolower((unsigned char)c);
	if (ext != ".numat" && ext != ".numesh" && ext != ".nuprefab") return;

	AssetEditorWin w;
	w.path = path; w.ext = ext; w.wantFocus = true;
	w.pv = AcquirePreview();
	if (!w.pv) return;
	ResDB* db = ResDB::getSingleton();

	if (ext == ".numat")
	{
		w.mat = nuke::Material::LoadFromFile(path);
		if (!w.mat) { ReleasePreview(w.pv); return; }
		w.pv->mr->meshGuid = kPreviewMeshGuid[0];
		w.pv->mr->mesh = db->GetMesh(kPreviewMeshGuid[0]);
		w.pv->mr->matGuid.clear();                 // the preview draws OUR editing copy
		w.pv->mr->mat = w.mat->Clone();
		w.idleM = w.mat->Clone();                  // undo baseline
		FramePreview(*w.pv, nullptr);
	}
	else if (ext == ".numesh")
	{
		const std::string guid = db->GuidForPath(path);
		w.pv->mr->meshGuid = guid;
		w.pv->mr->mesh = db->GetMesh(guid);
		w.pv->mr->matGuid = "builtin:default";
		if (w.pv->mr->mat) { delete w.pv->mr->mat; w.pv->mr->mat = nullptr; }
		FramePreview(*w.pv, nullptr);
	}
	else   // .nuprefab
	{
		w.prefabRoot = nuke::LoadPrefab(path);
		if (!w.prefabRoot) { ReleasePreview(w.pv); return; }
		w.pv->mr->mesh = nullptr;                  // the skeleton mesh atom stays empty
		w.pv->world->Add(w.prefabRoot);
		w.prefabSelId = (long)w.prefabRoot->id.id;
		w.idleP = nuke::SaveAtomToString(w.prefabRoot);   // undo baseline
		FramePreview(*w.pv, w.prefabRoot);
	}
	assetEds.push_back(std::move(w));
}

void EditorUI::DrawPrefabTree(AssetEditorWin& w, Atom* a)
{
	if (!a) return;
	ImGui::PushID((int)a->id.id);
	ImGuiTreeNodeFlags tf = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth
	                      | ImGuiTreeNodeFlags_DefaultOpen;
	if (a->children.empty())        tf |= ImGuiTreeNodeFlags_Leaf;
	if ((long)a->id.id == w.prefabSelId) tf |= ImGuiTreeNodeFlags_Selected;
	bool openNode = ImGui::TreeNodeEx(a->GetName().c_str(), tf);
	if (ImGui::IsItemClicked()) w.prefabSelId = (long)a->id.id;

	// Drag to reparent (within THIS prefab window; global pose is kept by Reparent).
	if (a != w.prefabRoot && ImGui::BeginDragDropSource())
	{
		long id = (long)a->id.id;
		ImGui::SetDragDropPayload("NUKE_PREFAB_ATOM", &id, sizeof(id));
		ImGui::TextUnformatted(a->GetName().c_str());
		ImGui::EndDragDropSource();
	}
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NUKE_PREFAB_ATOM"))
		{
			long dragId = *(const long*)p->Data;
			Atom* dragged = FindInSubtree(w.prefabRoot, dragId);
			// no-op for self / own descendant (that would detach the subtree into itself)
			bool insideDragged = dragged && FindInSubtree(dragged, (long)a->id.id) != nullptr;
			if (dragged && dragged != a && !insideDragged)
			{
				w.pv->world->Reparent(dragged, a);
				w.dirty = true; w.editedNow = true;
			}
		}
		ImGui::EndDragDropTarget();
	}

	// Structure ops (applied AFTER the walk — mutating mid-iteration corrupts the lists).
	if (ImGui::BeginPopupContextItem("##atomctx"))
	{
		if (ImGui::MenuItem(ICON_LC_PLUS " Add Child")) w.pendingAddParentId = (long)a->id.id;
		if (a != w.prefabRoot && ImGui::MenuItem(ICON_LC_TRASH_2 " Delete")) w.pendingDeleteId = (long)a->id.id;
		ImGui::EndPopup();
	}

	if (openNode)
	{
		for (Atom* ch : a->children) DrawPrefabTree(w, ch);
		ImGui::TreePop();
	}
	ImGui::PopID();
}

static nuke::Atom* FindInSubtree(nuke::Atom* a, long id)
{
	if (!a) return nullptr;
	if ((long)a->id.id == id) return a;
	for (nuke::Atom* ch : a->children)
		if (nuke::Atom* f = FindInSubtree(ch, id)) return f;
	return nullptr;
}

// Name + transform + reflected components of one prefab atom — with component
// add/remove, so the prefab is EDITABLE, not just viewable. Returns true when edited.
bool EditorUI::DrawPrefabAtomEditor(AssetEditorWin& w, Atom* a)
{
	bool edited = false;
	Transform& t = a->GetTransform();

	char nameBuf[128];
	strncpy(nameBuf, a->GetName().c_str(), 127); nameBuf[127] = 0;
	ImGui::SetNextItemWidth(240);
	if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) { a->SetName(nameBuf); edited = true; }

	if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
	{
		double pos[3] = { t.position.x, t.position.y, t.position.z };
		if (EditV3("Position", pos)) { t.position = Vector3(pos[0], pos[1], pos[2]); edited = true; }
		Vector3 e = t.EulerDeg();
		double eul[3] = { e.x, e.y, e.z };
		if (EditV3("Rotation", eul)) { t.SetEulerDeg(Vector3(eul[0], eul[1], eul[2])); edited = true; }
		double scl[3] = { t.scale.x, t.scale.y, t.scale.z };
		if (EditV3("Scale", scl)) { t.scale = Vector3(scl[0], scl[1], scl[2]); edited = true; }
	}
	Component* toRemove = nullptr;
	for (Component* c : a->components)
	{
		if (!c) continue;
		nuke::TypeInfo* ti = c->GetType();
		if (!ti) continue;
		ImGui::PushID(c);
		bool keep = true;
		bool openHdr = ImGui::CollapsingHeader(ti->name.c_str(), &keep, ImGuiTreeNodeFlags_DefaultOpen);
		if (!keep) toRemove = c;   // the header's close button = remove component
		if (openHdr)
		{
			bool en = c->enabled;
			if (ImGui::Checkbox("Enabled", &en)) { c->enabled = en; edited = true; }
			if (DrawFields(c, ti)) edited = true;
		}
		ImGui::PopID();
	}
	if (toRemove)
	{
		// Edit-time removal (same as the inspector): NOT Destroy() — that's the runtime hook.
		a->components.remove(toRemove);
		delete toRemove;
		edited = true;
	}

	// Add any registered, create-able Component type (incl. plugin ones) — same rules
	// as the scene inspector.
	ImGui::Separator();
	if (ImGui::Button(ICON_LC_PLUS " Add Component"))
		ImGui::OpenPopup("prefab_addcomp");
	if (ImGui::BeginPopup("prefab_addcomp"))
	{
		for (nuke::TypeInfo* ti : nuke::Registry_All())
		{
			if (!ti->create || ti->base != "Component")
				continue;
			if (ti->name == "PostProcess" && !a->GetComponent<nuke::Camera>())
				continue;
			if (ImGui::MenuItem(ti->name.c_str()))
			{
				a->AddComponent((nuke::Component*)ti->create());
				edited = true;
			}
		}
		ImGui::EndPopup();
	}
	return edited;
}

void EditorUI::winAssetEditors()
{
	aeFocused = -1;   // recomputed below; EditorUI::Undo/Redo route by it
	for (int i = 0; i < (int)assetEds.size(); ++i)
	{
		AssetEditorWin& w = assetEds[i];
		if (w.wantFocus) { ImGui::SetNextWindowFocus(); w.wantFocus = false; }
		// A NATIVE OS window, always: never merged into the main window's viewport.
		ImGuiWindowClass wcls;
		wcls.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
		ImGui::SetNextWindowClass(&wcls);
		ImGui::SetNextWindowSize(ImVec2(w.ext == ".nuprefab" ? 900.0f : 420.0f, 640.0f), ImGuiCond_FirstUseEver);
		const char* icon = w.ext == ".numat" ? ICON_LC_PALETTE : (w.ext == ".numesh" ? ICON_LC_BOX : ICON_LC_PACKAGE);
		std::string title = std::string(icon) + " " + bfs::path(w.path).filename().string() + "###ae:" + w.path;
		// No window scrollbars: the preview is sized to the free space, and a flickering
		// scrollbar would oscillate that size every frame (children scroll themselves).
		ImGuiWindowFlags wf = window_flags | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
		                    | (w.dirty ? ImGuiWindowFlags_UnsavedDocument : 0);
		if (ImGui::Begin(title.c_str(), &w.open, wf))
		{
			const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
			if (focused) aeFocused = i;   // Ctrl+Z/Ctrl+Y route to THIS window's history
			bool wantSave = false;
			if (w.ext != ".numesh")
			{
				if (ImGui::SmallButton(ICON_LC_SAVE " Save")) wantSave = true;
				ImGui::SameLine();
				if (ImGui::SmallButton(ICON_LC_UNDO_2 " Revert"))
				{
					// Reload the asset from disk into this editor.
					if (w.ext == ".numat" && w.mat)
					{
						delete w.mat; w.mat = nuke::Material::LoadFromFile(w.path);
						if (w.pv->mr->mat) delete w.pv->mr->mat;
						w.pv->mr->mat = w.mat ? w.mat->Clone() : nullptr;
					}
					else if (w.ext == ".nuprefab" && w.prefabRoot)
					{
						w.pv->world->RemoveAtomById((long)w.prefabRoot->id.id);
						w.prefabRoot = nuke::LoadPrefab(w.path);
						if (w.prefabRoot) { w.pv->world->Add(w.prefabRoot); w.prefabSelId = (long)w.prefabRoot->id.id; }
						FramePreview(*w.pv, w.prefabRoot);
					}
					w.dirty = false;
				}
				ImGui::SameLine();
			}
			ImGui::TextDisabled("%s", w.path.c_str());
			ImGui::Separator();

			if (w.ext == ".numat" && w.mat)
			{
				// Left: the material fields. Right: the 3D view fills EVERYTHING else.
				ImGui::BeginChild("##matfields", ImVec2(430, 0), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
				const char* meshes[] = { "Sphere", "Cube", "Plane" };
				ImGui::SetNextItemWidth(140);
				if (ImGui::Combo("Preview Mesh", &w.previewMesh, meshes, 3))
				{
					w.pv->mr->meshGuid = kPreviewMeshGuid[w.previewMesh];
					w.pv->mr->mesh = ResDB::getSingleton()->GetMesh(kPreviewMeshGuid[w.previewMesh]);
					FramePreview(*w.pv, nullptr);
				}
				ImGui::Separator();
				if (nuke::TypeInfo* ti = w.mat->GetType())
					if (DrawFields(w.mat, ti))
					{
						w.dirty = true; w.editedNow = true;
						// live preview: swap in a fresh clone of the edited material
						if (w.pv->mr->mat) delete w.pv->mr->mat;
						w.pv->mr->mat = w.mat->Clone();
					}
				ImGui::EndChild();
				ImGui::SameLine();
				DrawPreviewImage(*w.pv, ImGui::GetContentRegionAvail());
			}
			else if (w.ext == ".numesh")
			{
				if (nuke::Mesh* m = w.pv->mr->mesh)
				{
					m->EnsureBounds();
					ImGui::Text("%d vertices   %d triangles   bounds %.2f x %.2f x %.2f",
						m->numVerts, m->numVerts / 3,
						m->aabbMax[0] - m->aabbMin[0], m->aabbMax[1] - m->aabbMin[1], m->aabbMax[2] - m->aabbMin[2]);
				}
				else ImGui::TextDisabled("Mesh is not in the resource DB.");
				DrawPreviewImage(*w.pv, ImGui::GetContentRegionAvail());   // everything below the stats line
			}
			else if (w.ext == ".nuprefab" && w.prefabRoot)
			{
				// Deferred tree ops (queued by the tree's context menu last frame).
				if (w.pendingAddParentId)
				{
					if (Atom* parent = FindInSubtree(w.prefabRoot, w.pendingAddParentId))
					{
						Atom* n = new Atom("Atom");
						parent->AddChild(n);
						w.prefabSelId = (long)n->id.id;
						w.dirty = true; w.editedNow = true;
					}
					w.pendingAddParentId = 0;
				}
				if (w.pendingDeleteId)
				{
					if (w.pendingDeleteId != (long)w.prefabRoot->id.id)
					{
						w.pv->world->RemoveAtomById(w.pendingDeleteId);
						if (w.prefabSelId == w.pendingDeleteId) w.prefabSelId = (long)w.prefabRoot->id.id;
						w.dirty = true; w.editedNow = true;
					}
					w.pendingDeleteId = 0;
				}

				// Left: hierarchy tree. Right: gizmo toolbar + 3D view, atom editor below.
				ImGui::BeginChild("##ptree", ImVec2(240, 0), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
				DrawPrefabTree(w, w.prefabRoot);
				ImGui::EndChild();
				ImGui::SameLine();
				ImGui::BeginChild("##pright", ImVec2(0, 0), ImGuiChildFlags_None,
				                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
				{
					// The SAME tool buttons as the main toolbar (icons + active highlight).
					const float tbw = 34.0f;
					if (ToolBtn(ICON_LC_MOUSE_POINTER, "Select (Q)", w.gizmoOp == 0, tbw)) w.gizmoOp = 0; ImGui::SameLine();
					if (ToolBtn(ICON_LC_MOVE,          "Move (W)",   w.gizmoOp == 1, tbw)) w.gizmoOp = 1; ImGui::SameLine();
					if (ToolBtn(ICON_LC_ROTATE_3D,     "Rotate (E)", w.gizmoOp == 2, tbw)) w.gizmoOp = 2; ImGui::SameLine();
					if (ToolBtn(ICON_LC_SCALING,       "Scale (R)",  w.gizmoOp == 3, tbw)) w.gizmoOp = 3; ImGui::SameLine();
					if (ToolBtn(w.gizmoWorld ? ICON_LC_GLOBE : ICON_LC_AXIS_3D,
					            w.gizmoWorld ? "World space (X)" : "Local space (X)", false, tbw))
						w.gizmoWorld = !w.gizmoWorld;

					// Animation preview (3.1): ▶ ticks the subtree's Animators (mini-PIE for
					// this window only); ■ restores the pose snapshot taken at play start.
					if (SubtreeHasAnimator(w.prefabRoot))
					{
						ImGui::SameLine();
						if (ToolBtn(w.animPlay ? ICON_LC_SQUARE : ICON_LC_PLAY,
						            w.animPlay ? "Stop animation preview" : "Play animation preview",
						            w.animPlay, tbw))
							ToggleAnimPreview(w);
					}
					if (w.animPlay) TickAnimPreview(w);

					// The VIEWPORT's hotkeys, scoped to this window: Q/W/E/R tools, X space,
					// F frame selection, Del delete atom. Not while typing or flying (RMB+WASD).
					if ((ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
					     || ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
					    && !ImGui::GetIO().WantTextInput && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
					{
						if (ImGui::IsKeyPressed(ImGuiKey_Q)) w.gizmoOp = 0;
						if (ImGui::IsKeyPressed(ImGuiKey_W)) w.gizmoOp = 1;
						if (ImGui::IsKeyPressed(ImGuiKey_E)) w.gizmoOp = 2;
						if (ImGui::IsKeyPressed(ImGuiKey_R)) w.gizmoOp = 3;
						if (ImGui::IsKeyPressed(ImGuiKey_X)) w.gizmoWorld = !w.gizmoWorld;
						if (ImGui::IsKeyPressed(ImGuiKey_F))
						{
							Atom* fs = FindInSubtree(w.prefabRoot, w.prefabSelId);
							FramePreview(*w.pv, fs ? fs : w.prefabRoot);   // frame selection (or all)
						}
						if (ImGui::IsKeyPressed(ImGuiKey_Delete)
						    && w.prefabSelId && w.prefabSelId != (long)w.prefabRoot->id.id)
							w.pendingDeleteId = w.prefabSelId;
					}

					ImVec2 av = ImGui::GetContentRegionAvail();
					float edH = 320.0f;                                   // atom editor strip
					if (av.y - edH < 160.0f) edH = std::max(120.0f, av.y * 0.45f);
					DrawPreviewImage(*w.pv, ImVec2(av.x, av.y - edH - 8.0f));

					// Transform gizmo over the 3D view (same conventions as the scene viewport).
					Atom* sel = FindInSubtree(w.prefabRoot, w.prefabSelId);
					w.pv->gizmoBusy = false;   // set below inside the gizmo's ID scope
					if (sel && w.gizmoOp != 0 && w.pv->cam && w.pv->cam->transform
					    && w.pv->rectSize.x > 1.0f && w.pv->rectSize.y > 1.0f)
					{
						ImGuizmo::SetOrthographic(false);
						ImGuizmo::SetDrawlist();
						ImGuizmo::SetRect(w.pv->rectMin.x, w.pv->rectMin.y, w.pv->rectSize.x, w.pv->rectSize.y);
						ImGuizmo::PushID(w.path.c_str());   // several gizmos may run per frame (viewport + windows)

						float gview[16], gproj[16];
						{
							Transform* gc = w.pv->cam->transform;
							Vector3 ge = gc->globalPosition();
							Vector3 gf = gc->direction(), gu = gc->up();
							float aspect = w.pv->rectSize.x / w.pv->rectSize.y;
							float fovy   = (float)w.pv->cam->fov * 0.01745329252f;
							glm::mat4 gv = glm::lookAtLH(
								glm::vec3((float)ge.x, (float)ge.y, (float)ge.z),
								glm::vec3((float)(ge.x + gf.x), (float)(ge.y + gf.y), (float)(ge.z + gf.z)),
								glm::vec3((float)gu.x, (float)gu.y, (float)gu.z));
							glm::mat4 gp = glm::perspectiveLH_ZO(fovy, aspect, w.pv->cam->_near, w.pv->cam->_far);
							memcpy(gview, glm::value_ptr(gv), sizeof(gview));
							memcpy(gproj, glm::value_ptr(gp), sizeof(gproj));
						}
						Transform& gtt = sel->GetTransform();
						if (!ImGuizmo::IsUsing())   // resync from the atom only when NOT dragging
						{
							Vector3 gP = gtt.globalPosition(); Quaternion gR = gtt.globalRotation(); Vector3 gS = gtt.globalScale();
							glm::mat4 gm = glm::translate(glm::mat4(1.0f), glm::vec3((float)gP.x, (float)gP.y, (float)gP.z))
							             * glm::mat4_cast(glm::quat((float)gR.w, (float)gR.x, (float)gR.y, (float)gR.z))
							             * glm::scale(glm::mat4(1.0f), glm::vec3((float)gS.x, (float)gS.y, (float)gS.z));
							memcpy(w.gizmoMtx, glm::value_ptr(gm), sizeof(w.gizmoMtx));
						}
						ImGuizmo::OPERATION gop = (w.gizmoOp == 1) ? ImGuizmo::TRANSLATE
						                        : (w.gizmoOp == 2) ? ImGuizmo::ROTATE : ImGuizmo::SCALE;
						// World/local space + Ctrl-snap — the viewport's conventions exactly.
						ImGuizmo::MODE gmode = (gop != ImGuizmo::SCALE && w.gizmoWorld) ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
						float snapv   = (gop == ImGuizmo::TRANSLATE) ? 0.5f : (gop == ImGuizmo::ROTATE) ? 15.0f : 0.1f;
						float snap[3] = { snapv, snapv, snapv };
						float* snapPtr = ImGui::GetIO().KeyCtrl ? snap : nullptr;
						ImGuizmo::Manipulate(gview, gproj, gop, gmode, w.gizmoMtx, nullptr, snapPtr);
						if (ImGuizmo::IsUsing())
						{
							glm::mat4 nm = glm::make_mat4(w.gizmoMtx);
							glm::vec3 nS, nT, nSkew; glm::vec4 nPersp; glm::quat nR;
							if (glm::decompose(nm, nS, nR, nT, nSkew, nPersp) &&
							    std::isfinite(nT.x) && std::isfinite(nT.y) && std::isfinite(nT.z))
							{
								if (nS.x < 1e-3f && nS.x > -1e-3f) nS.x = 1e-3f;
								if (nS.y < 1e-3f && nS.y > -1e-3f) nS.y = 1e-3f;
								if (nS.z < 1e-3f && nS.z > -1e-3f) nS.z = 1e-3f;
								gtt.SetGlobal(Vector3(nT.x, nT.y, nT.z),
								              Quaternion(nR.x, nR.y, nR.z, nR.w),
								              Vector3(nS.x, nS.y, nS.z));
								w.dirty = true; w.editedNow = true;
							}
						}
						// Queried INSIDE the ID scope (accurate here); consumed below and
						// by the next frame's input handling.
						w.pv->gizmoBusy = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
						ImGuizmo::PopID();
					}

					// LMB picking, like the scene viewport: ray from the preview camera
					// through the click point -> nearest prefab atom with a mesh.
					if (w.pv->cam && w.pv->cam->transform && !w.pv->gizmoBusy
					    && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
					{
						const ImVec2 mp = ImGui::GetIO().MousePos;
						const ImVec2 rmin = w.pv->rectMin;
						const ImVec2 rsz  = w.pv->rectSize;
						if (rsz.x > 1.0f && rsz.y > 1.0f
						    && mp.x >= rmin.x && mp.x <= rmin.x + rsz.x
						    && mp.y >= rmin.y && mp.y <= rmin.y + rsz.y)
						{
							Transform* ct = w.pv->cam->transform;
							float ndcx = ((mp.x - rmin.x) / rsz.x) * 2.0f - 1.0f;
							float ndcy = 1.0f - ((mp.y - rmin.y) / rsz.y) * 2.0f;
							Vector3 o = ct->globalPosition();
							Vector3 f = ct->direction(), rr = ct->right(), uu = ct->up();
							float aspect = rsz.x / rsz.y;
							float thf = tanf((float)w.pv->cam->fov * 0.5f * 0.01745329252f);
							Vector3 dir(f.x + ndcx * thf * aspect * rr.x + ndcy * thf * uu.x,
							            f.y + ndcx * thf * aspect * rr.y + ndcy * thf * uu.y,
							            f.z + ndcx * thf * aspect * rr.z + ndcy * thf * uu.z);
							Atom* hit = w.pv->world->Pick(o, dir);
							// Only atoms of THIS prefab are selectable (the pick could
							// return preview-scene furniture in theory).
							if (hit && FindInSubtree(w.prefabRoot, (long)hit->id.id))
								w.prefabSelId = (long)hit->id.id;
							else if (!hit)
								w.prefabSelId = 0;   // clicked empty space = deselect
						}
					}

					ImGui::BeginChild("##patom", ImVec2(0, 0), ImGuiChildFlags_Borders);
					if (sel)
					{
						if (DrawPrefabAtomEditor(w, sel)) { w.dirty = true; w.editedNow = true; }
					}
					else ImGui::TextDisabled("Select an atom in the tree.");
					ImGui::EndChild();
				}
				ImGui::EndChild();
			}

			// Save (button or Ctrl+S while focused).
			if (focused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) wantSave = true;
			if (wantSave)
			{
				if      (w.ext == ".numat"    && w.mat)        { w.mat->SaveToFile(w.path); w.dirty = false; }
				else if (w.ext == ".nuprefab" && w.prefabRoot) { nuke::SavePrefab(w.prefabRoot, w.path); w.dirty = false; }
				// (saved .numat hot-reloads into the scene via the existing mtime watcher)
			}

			// --- per-window undo latch: an edit BURST (drag, typing) becomes ONE entry ---
			if (w.editedNow && !w.editing)
			{
				// Push the PRE-edit baseline; a new edit invalidates the redo branch.
				if (w.ext == ".nuprefab" && !w.idleP.empty())
				{
					w.undoP.push_back(w.idleP);
					if (w.undoP.size() > kAeUndoCap) w.undoP.erase(w.undoP.begin());
					w.redoP.clear();
				}
				else if (w.ext == ".numat" && w.idleM)
				{
					w.undoM.push_back(w.idleM->Clone());
					if (w.undoM.size() > kAeUndoCap) { delete w.undoM.front(); w.undoM.erase(w.undoM.begin()); }
					for (Material* m : w.redoM) delete m;
					w.redoM.clear();
				}
				w.editing = true;
			}
			if (w.editing && !w.editedNow && !ImGui::IsAnyItemActive() && !(w.pv && w.pv->gizmoBusy))
			{
				// The burst settled: the CURRENT state becomes the next baseline.
				w.editing = false;
				if      (w.ext == ".nuprefab" && w.prefabRoot) w.idleP = nuke::SaveAtomToString(w.prefabRoot);
				else if (w.ext == ".numat"    && w.mat)        { delete w.idleM; w.idleM = w.mat->Clone(); }
			}
			w.editedNow = false;
		}
		ImGui::End();
		if (!w.open && w.dirty)
		{
			w.open = true;               // keep the window until the user answers
			aeCloseConfirm = i;
		}
	}

	// Discard-changes modal for a closing dirty editor.
	if (aeCloseConfirm >= 0) ImGui::OpenPopup("Unsaved changes##asseted");
	if (ImGui::BeginPopupModal("Unsaved changes##asseted", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		if (aeCloseConfirm >= 0 && aeCloseConfirm < (int)assetEds.size())
		{
			AssetEditorWin& w = assetEds[aeCloseConfirm];
			ImGui::Text("'%s' has unsaved changes.", bfs::path(w.path).filename().string().c_str());
			ImGui::Spacing();
			if (ImGui::Button("Save & Close"))
			{
				if      (w.ext == ".numat"    && w.mat)        w.mat->SaveToFile(w.path);
				else if (w.ext == ".nuprefab" && w.prefabRoot) nuke::SavePrefab(w.prefabRoot, w.path);
				w.dirty = false; w.open = false; aeCloseConfirm = -1; ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Discard")) { w.dirty = false; w.open = false; aeCloseConfirm = -1; ImGui::CloseCurrentPopup(); }
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))  { aeCloseConfirm = -1; ImGui::CloseCurrentPopup(); }
		}
		else { aeCloseConfirm = -1; ImGui::CloseCurrentPopup(); }
		ImGui::EndPopup();
	}

	// Tear down closed editors (scene goes back to the pool).
	for (int i = (int)assetEds.size() - 1; i >= 0; --i)
		if (!assetEds[i].open)
		{
			AssetEditorWin& w = assetEds[i];
			if (w.prefabRoot && w.pv) w.pv->world->RemoveAtomById((long)w.prefabRoot->id.id);
			if (w.mat) delete w.mat;
			delete w.idleM;
			for (Material* m : w.undoM) delete m;
			for (Material* m : w.redoM) delete m;
			ReleasePreview(w.pv);
			assetEds.erase(assetEds.begin() + i);
		}
}
