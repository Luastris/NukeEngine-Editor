// Asset editors (user request on top of 2.2): each material / mesh / prefab opens its
// OWN window with a live 3D view and type-specific editing, saving back to the asset
// file. Built on POOLED preview scenes — tiny worlds (sky + shadowless sun + one mesh
// atom + camera into an own RT) that render through the hook BEFORE the live scene, so
// their lights/sky/TLAS never taint the viewport. Pooled because the render seam has no
// destroyRenderTarget: closing an editor returns its scene for reuse.
#include <editor/editorui.h>
#include <API/Model/Material.h>
#include <API/Model/Texture.h>   // Sprite Slicer (.nutex): grid/margin/spacing + SpriteCellRect
#include <API/Model/Light.h>
#include <API/Model/Environment.h>
#include <API/Model/Prefab.h>
#include <API/Model/Audio.h>   // audio preview transport (Preview bus)
#include <input/Input.h>       // .nuinput asset editor (ParseMapString/SerializeMap/ApplyMap)
#include <interface/AssetCreators.h>   // module-supplied asset editors (AssetEditorForExt)
#include "nukeui.h"                     // NukeUI host windows (editor-owned detached editors)
#include "imgui_internal.h"             // MovingWindow/ClearActiveID — drag-out detach (task #135)
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>                    // GetCursorPos/GetSystemMetrics — screen-edge tear-off
#endif
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <iterator>   // istreambuf_iterator (read the .nuinput file)
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>   // prefab gizmo: decompose the manipulated matrix
#include <glm/gtc/type_ptr.hpp>
namespace bfs = boost::filesystem;

// Sprite-slice metadata helpers (defined lower, near the slicer) — forward-declared so the undo/redo
// handlers above the definitions can use them.
static EditorUI::SpriteMeta SnapMeta(const nuke::Texture* t);
static void ApplyMeta(nuke::Texture* t, const EditorUI::SpriteMeta& m);
static void SlicerApplyLive(nuke::Texture* t);

// ---------------------------------------------------------------------------
// Preview-scene pool
// ---------------------------------------------------------------------------

EditorUI::PreviewWorld* EditorUI::AcquirePreview()
{
	for (PreviewWorld* s : pvPool)
		if (!s->inUse)
		{
			s->inUse = true;
			s->yaw = 0.7f; s->pitch = 0.35f; s->dist = 0.0f;
			return s;
		}
	iRender* r = AppInstance::GetSingleton()->render;
	if (!r) return nullptr;

	PreviewWorld* s = new PreviewWorld();
	s->rt = r->createRenderTarget(384, 384);
	s->world = new World();
	s->world->name = "Asset Preview";
	s->world->auxiliary = true;   // skip global heavy passes (RT TLAS) — see World::Render

	Atom* env = new Atom("PreviewEnv");
	env->AddComponent(new Environment());
	s->world->Add(env);

	Atom* sun = new Atom("PreviewSun");
	Light* l = new Light();
	l->type = Light::Directional;
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

void EditorUI::ReleasePreview(PreviewWorld* s)
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

void EditorUI::FramePreview(PreviewWorld& s, Atom* subtree)
{
	bool any = false;
	Vector3 c(0, 0, 0); float r = 1.0f;
	if (subtree) SubtreeBounds(subtree, c, r, any);
	else         SubtreeBounds(s.meshAtom, c, r, any);
	s.center = any ? c : Vector3(0, 0, 0);
	s.radius = any ? std::max(r, 0.01f) : 1.0f;
	s.dist = 0.0f;   // re-derive from radius on next draw
}

void EditorUI::DrawPreviewImage(PreviewWorld& s, ImVec2 size)
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
	else if (w.ext == ".nutex" && w.tex && !w.undoS.empty())
	{
		w.redoS.push_back(SnapMeta(w.tex));
		ApplyMeta(w.tex, w.undoS.back()); w.undoS.pop_back();
		w.idleS = SnapMeta(w.tex); SlicerApplyLive(w.tex); w.dirty = true;
	}
	else if (w.ext == ".nuinput" && !w.undoI.empty())
	{
		w.redoI.push_back(InputMapJson(w));
		LoadInputMapJson(w, w.undoI.back()); w.undoI.pop_back();
		w.idleI = InputMapJson(w); w.editing = false; w.dirty = true;
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
	else if (w.ext == ".nutex" && w.tex && !w.redoS.empty())
	{
		w.undoS.push_back(SnapMeta(w.tex));
		ApplyMeta(w.tex, w.redoS.back()); w.redoS.pop_back();
		w.idleS = SnapMeta(w.tex); SlicerApplyLive(w.tex); w.dirty = true;
	}
	else if (w.ext == ".nuinput" && !w.redoI.empty())
	{
		w.undoI.push_back(InputMapJson(w));
		LoadInputMapJson(w, w.redoI.back()); w.redoI.pop_back();
		w.idleI = InputMapJson(w); w.editing = false; w.dirty = true;
	}
}

uint64_t EditorUI::UploadTexPreview(nuke::Texture* t, int cap, int& outW, int& outH)
{
	outW = outH = 0;
	if (!t || t->renderTexture) return 0;
	iRender* r = AppInstance::GetSingleton()->render; if (!r) return 0;
	std::vector<unsigned char> rgba = t->DecodeRGBA();
	int pw = t->width, ph = t->height;
	if (rgba.empty() || pw <= 0 || ph <= 0) return 0;
	if (pw > cap || ph > cap)   // box-downsample so the longest side <= cap
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
	outW = pw; outH = ph;
	return r->createTexture2D(rgba.data(), pw, ph);
}

// Sprite-slice metadata <-> its snapshot struct (single place that lists the fields).
static EditorUI::SpriteMeta SnapMeta(const nuke::Texture* t)
{
	return { t->spriteColumns, t->spriteRows,
	         t->spriteMarginLeft, t->spriteMarginRight, t->spriteMarginTop, t->spriteMarginBottom,
	         t->spriteSpacingX, t->spriteSpacingY, t->sliceLeft, t->sliceRight, t->sliceTop, t->sliceBottom,
	         t->nineSlice };
}
static void ApplyMeta(nuke::Texture* t, const EditorUI::SpriteMeta& m)
{
	t->spriteColumns = m.cols; t->spriteRows = m.rows;
	t->spriteMarginLeft = m.ml; t->spriteMarginRight = m.mr; t->spriteMarginTop = m.mt; t->spriteMarginBottom = m.mb;
	t->spriteSpacingX = m.sx; t->spriteSpacingY = m.sy;
	t->sliceLeft = m.sl; t->sliceRight = m.sr; t->sliceTop = m.st; t->sliceBottom = m.sb;
	t->nineSlice = m.nine;
}
// Mirror the edited slice metadata onto the live ResDB texture so scene sprites re-slice immediately
// (int fields only — no pixel re-upload, no cache invalidation).
static void SlicerApplyLive(nuke::Texture* t)
{
	if (!t || t->guid.empty()) return;
	if (nuke::Texture* live = nuke::ResDB::getSingleton()->GetTexture(t->guid))
		if (live != t) ApplyMeta(live, SnapMeta(t));
}

// Call AFTER committing a slice change: records the pre-change baseline (w.idleS) then re-baselines.
void EditorUI::SlicerPushUndo(AssetEditorWin& w)
{
	if (!w.tex) return;
	w.undoS.push_back(w.idleS);
	w.redoS.clear();
	w.idleS = SnapMeta(w.tex);
	w.dirty = true;
	SlicerApplyLive(w.tex);
}

void EditorUI::OpenAssetEditor(const std::string& path)
{
	for (AssetEditorWin& w : assetEds)
		if (w.path == path) { w.wantFocus = true; return; }

	std::string ext = bfs::path(path).extension().string();
	for (char& c : ext) c = (char)std::tolower((unsigned char)c);
	const bool isAudio = (ext == ".ogg" || ext == ".wav" || ext == ".mp3" || ext == ".flac");
	// MODULE-supplied editors first: the module that registered a file type registers its
	// editor too (RegisterAssetEditor — e.g. NukeTilemapEditor owns .nutile). The editor
	// core stays format-blind about plugin types.
	if (const auto* open = nuke::AssetEditorForExt(ext)) { (*open)(path); return; }
	if (ext != ".numat" && ext != ".numesh" && ext != ".nuprefab" && ext != ".nutex" && ext != ".nuinput" && !isAudio) return;

	if (ext == ".nuinput")   // gameplay input map: pure data CRUD — no 3D scene, edits + saves the file
	{
		AssetEditorWin w;
		w.path = path; w.ext = ext; w.wantFocus = true; w.detached = detachAssetEditors;
		bfs::ifstream f{ bfs::path(path) };
		std::string js((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		nuke::Input::InputMapData m = nuke::Input::ParseMapString(js);
		w.inActions = m.actions; w.inContexts = m.contexts;
		w.idleI = nuke::Input::SerializeMap(m);   // undo baseline
		assetEds.push_back(std::move(w));
		return;
	}

	if (isAudio)   // audio preview: no 3D scene — just the file path + a Preview-bus voice
	{
		AssetEditorWin w;
		w.path = path; w.ext = ext; w.wantFocus = true; w.detached = detachAssetEditors;
		assetEds.push_back(std::move(w));
		return;
	}

	if (ext == ".nutex")   // Sprite Slicer: 2D — no preview scene, just an owned texture + a GPU preview
	{
		AssetEditorWin w;
		w.path = path; w.ext = ext; w.wantFocus = true; w.detached = detachAssetEditors;
		w.tex = nuke::Texture::LoadFromFile(path);
		if (!w.tex) return;
		if (w.tex->usage != nuke::Texture::UsageSprite) w.tex->usage = nuke::Texture::UsageSprite;  // opening it in the slicer implies it IS a sprite
		w.texPreview = UploadTexPreview(w.tex, 2048, w.texPrevW, w.texPrevH);
		// (destroyTexture2D defers destruction INSIDE the renderer now — a destroyed handle's view
		// stays alive until no in-flight draw data can reference it, so handle reuse can't collide.)
		w.slFirst = 0; w.slCount = w.tex->SpriteCount();
		w.idleS = SnapMeta(w.tex);
		assetEds.push_back(std::move(w));
		return;
	}

	AssetEditorWin w;
	w.path = path; w.ext = ext; w.wantFocus = true; w.detached = detachAssetEditors;
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

// --- dashed helpers for the slicer grid overlay ---
static void DashLine(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float th, float dash, float gap)
{
	float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
	if (len < 0.001f) return;
	float step = dash + gap;
	// HARD safety: a very long line would emit thousands of segments -> a single UI draw list past 65535
	// verts overflows the renderer's 16-bit index buffer -> GPU reads OOB -> device removed. Callers clip
	// to the canvas first, but if anything slips through, fall back to a solid line (2 verts).
	if (len / step > 4096.0f) { dl->AddLine(a, b, col, th); return; }
	float ux = dx / len, uy = dy / len;
	for (float d = 0; d < len; d += step)
	{
		float e = d + dash; if (e > len) e = len;
		dl->AddLine(ImVec2(a.x + ux * d, a.y + uy * d), ImVec2(a.x + ux * e, a.y + uy * e), col, th);
	}
}
// Dashed axis-aligned rectangle, each edge CLIPPED to [cmin,cmax] (and culled if outside) so the segment
// count is bounded by the visible canvas, never by the zoom level.
static void DashRect(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImVec2 cmin, ImVec2 cmax, ImU32 col, float th)
{
	if (p0.x > p1.x) { float t = p0.x; p0.x = p1.x; p1.x = t; }
	if (p0.y > p1.y) { float t = p0.y; p0.y = p1.y; p1.y = t; }
	if (p1.x < cmin.x || p0.x > cmax.x || p1.y < cmin.y || p0.y > cmax.y) return;   // fully outside
	float cl = p0.x < cmin.x ? cmin.x : p0.x, cr = p1.x > cmax.x ? cmax.x : p1.x;   // clamped x span
	float ct = p0.y < cmin.y ? cmin.y : p0.y, cb = p1.y > cmax.y ? cmax.y : p1.y;   // clamped y span
	if (p0.y >= cmin.y && p0.y <= cmax.y && cr > cl) DashLine(dl, ImVec2(cl, p0.y), ImVec2(cr, p0.y), col, th, 6, 4);
	if (p1.y >= cmin.y && p1.y <= cmax.y && cr > cl) DashLine(dl, ImVec2(cl, p1.y), ImVec2(cr, p1.y), col, th, 6, 4);
	if (p0.x >= cmin.x && p0.x <= cmax.x && cb > ct) DashLine(dl, ImVec2(p0.x, ct), ImVec2(p0.x, cb), col, th, 6, 4);
	if (p1.x >= cmin.x && p1.x <= cmax.x && cb > ct) DashLine(dl, ImVec2(p1.x, ct), ImVec2(p1.x, cb), col, th, 6, 4);
}

// Sprite Slicer body (mode 0): a GIMP-style 2D editor for the sheet. Left = properties (drag OR type),
// centre = the sheet under a real ruler with draggable margin edges + grid lines, right = live cell preview.
// All cell geometry comes from Texture::SpriteCellRect (shared with the runtime SpriteAnimator), so what you
// mark here is exactly what plays. View auto-fits (adapts to window resize) until you zoom/pan.
void EditorUI::DrawSpriteSlicer(AssetEditorWin& w)
{
	nuke::Texture* t = w.tex;
	if (!t) { ImGui::TextDisabled("No texture."); return; }
	ImGuiIO& io = ImGui::GetIO();

	auto clampGrid = [&]{
		if (t->spriteColumns < 1) t->spriteColumns = 1; if (t->spriteColumns > 256) t->spriteColumns = 256;
		if (t->spriteRows    < 1) t->spriteRows    = 1; if (t->spriteRows    > 256) t->spriteRows    = 256;
		int* m[] = { &t->spriteMarginLeft,&t->spriteMarginRight,&t->spriteMarginTop,&t->spriteMarginBottom,
		             &t->spriteSpacingX,&t->spriteSpacingY,&t->sliceLeft,&t->sliceRight,&t->sliceTop,&t->sliceBottom };
		for (int* p : m) { if (*p < 0) *p = 0; }
	};

	// ---- header ----
	ImGui::SetNextItemWidth(150);
	const char* modes[] = { "Sprite Slicer", "Nine-Slice" };
	ImGui::Combo("##mode", &w.slMode, modes, IM_ARRAYSIZE(modes));
	const bool nineMode = (w.slMode == 1);   // drag the 9-slice borders instead of the grid
	ImGui::SameLine(); ImGui::TextDisabled("%d x %d px", t->width, t->height);
	ImGui::SameLine(); if (ImGui::SmallButton("Fit")) w.slUserView = false;
	ImGui::Separator();

	// advance the preview clock (used by both the canvas highlight and the right preview)
	int nCells = t->SpriteCount();
	if (w.slFirst < 0) w.slFirst = 0; if (w.slFirst >= nCells) w.slFirst = nCells - 1;
	if (w.slCount < 1) w.slCount = 1; if (w.slCount > nCells - w.slFirst) w.slCount = nCells - w.slFirst;
	if (w.slPlay && w.slFps > 0 && w.slCount > 1) {
		w.slAcc += io.DeltaTime; float dur = 1.0f / w.slFps;
		while (w.slAcc >= dur) { w.slAcc -= dur; w.slCur = (w.slCur + 1) % w.slCount; }
	} else if (!w.slPlay) { w.slCur = 0; w.slAcc = 0; }
	int activeCell = w.slFirst + (w.slCount > 0 ? (w.slCur % w.slCount) : 0);
	int ax0 = 0, ay0 = 0, acw = 0, ach = 0; bool haveActive = t->SpriteCellRect(activeCell, ax0, ay0, acw, ach);

	// ===== LEFT: properties (labels left, drag-to-scrub or double-click to type) =====
	ImGui::BeginChild("sl_props", ImVec2(232, 0), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
	auto Row = [&](const char* label, int* v, int lo, int hi){
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(label);
		ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-1);
		ImGui::PushID(label);
		if (ImGui::DragInt("##d", v, 1.0f, lo, hi)) { if (*v < lo) *v = lo; if (*v > hi) *v = hi; w.dirty = true; SlicerApplyLive(t); }
		if (ImGui::IsItemDeactivatedAfterEdit()) SlicerPushUndo(w);
		ImGui::PopID();
	};
	auto Table = [&](const char* id){
		if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
		ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthFixed, 88);
		ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
		return true;
	};
	if (!nineMode)
	{
	ImGui::SeparatorText("Grid");
	if (Table("sl_grid")) {
		Row("Columns", &t->spriteColumns, 1, 256);
		Row("Rows",    &t->spriteRows,    1, 256);
		Row("Margin L", &t->spriteMarginLeft,   0, t->width);
		Row("Margin R", &t->spriteMarginRight,  0, t->width);
		Row("Margin T", &t->spriteMarginTop,    0, t->height);
		Row("Margin B", &t->spriteMarginBottom, 0, t->height);
		Row("Spacing X", &t->spriteSpacingX, 0, t->width);
		Row("Spacing Y", &t->spriteSpacingY, 0, t->height);
		ImGui::EndTable();
	}
	}
	ImGui::SeparatorText("9-Slice");
	if (Table("sl_9s")) {
		Row("Left",   &t->sliceLeft,   0, t->width);
		Row("Right",  &t->sliceRight,  0, t->width);
		Row("Top",    &t->sliceTop,    0, t->height);
		Row("Bottom", &t->sliceBottom, 0, t->height);
		ImGui::EndTable();
	}
	if (nineMode)
	{
		if (ImGui::Checkbox("Enable nine-slice", &t->nineSlice)) { w.dirty = true; SlicerApplyLive(t); SlicerPushUndo(w); }
		ImGui::TextWrapped("Drag the green border lines on the sheet. Corners never stretch; edges stretch "
		                   "along one axis, the centre along both. Applies to EVERY sprite using this texture.");
	}
	if (!nineMode) {
	ImGui::SeparatorText("Animation");
	if (Table("sl_anim")) {
		ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("First");
		ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-1); ImGui::DragInt("##first", &w.slFirst, 1.0f, 0, nCells - 1);
		ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Count");
		ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-1); ImGui::DragInt("##count", &w.slCount, 1.0f, 1, nCells);
		ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("FPS");
		ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-1); ImGui::DragFloat("##fps", &w.slFps, 0.5f, 0, 120, "%.0f");
		ImGui::EndTable();
	}
	if (ImGui::Button(w.slPlay ? "Pause" : "Play", ImVec2(70, 0))) w.slPlay = !w.slPlay;
	ImGui::SameLine(); ImGui::Checkbox("Mirror cell-0", &w.slShowMirror);
	if (w.slShowMirror && Table("sl_pad")) {
		auto PadRow = [&](const char* label, int* v){
			ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(label);
			ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-1); ImGui::PushID(label); ImGui::DragInt("##p", v, 1.0f, 0, t->width); if (*v < 0) *v = 0; ImGui::PopID();
		};
		PadRow("Pad L", &w.slPadL); PadRow("Pad R", &w.slPadR); PadRow("Pad T", &w.slPadT); PadRow("Pad B", &w.slPadB);
		ImGui::EndTable();
	}
	}   // !nineMode (animation/mirror sections)
	clampGrid();
	ImGui::EndChild();

	// ===== CENTRE: ruler + sheet =====
	ImGui::SameLine();
	ImGui::BeginChild("sl_canvas", ImVec2(-190, 0), ImGuiChildFlags_Borders,
	                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	{
		const float RT = 18.0f, RL = 34.0f;   // ruler strip sizes
		ImVec2 c0  = ImGui::GetCursorScreenPos();
		ImVec2 csz = ImGui::GetContentRegionAvail();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 ia0 = ImVec2(c0.x + RL, c0.y + RT);
		float iaW = csz.x - RL, iaH = csz.y - RT; if (iaW < 16) iaW = 16; if (iaH < 16) iaH = 16;

		float fit = 1.0f;
		if (t->width > 0 && t->height > 0) {
			fit = (iaW - 12) / t->width; float fy = (iaH - 12) / t->height; if (fy < fit) fit = fy;
			if (fit <= 0) fit = 0.01f;
		}
		float z = fit * (w.slUserView ? w.slZoomMul : 1.0f);
		ImVec2 ip0;
		if (!w.slUserView) {
			ip0 = ImVec2(ia0.x + (iaW - t->width * z) * 0.5f, ia0.y + (iaH - t->height * z) * 0.5f);
			w.slPanX = ip0.x - ia0.x; w.slPanY = ip0.y - ia0.y; w.slZoomMul = 1.0f;
		} else ip0 = ImVec2(ia0.x + w.slPanX, ia0.y + w.slPanY);

		ImGui::InvisibleButton("sl_surf", csz, ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
		bool overArea = ImGui::IsItemHovered();
		if (overArea && io.MouseWheel != 0) {
			w.slUserView = true;
			float oldz = z, nzm = (oldz / fit) * (io.MouseWheel > 0 ? 1.1f : 1.0f / 1.1f);
			if (nzm < 0.05f) nzm = 0.05f; if (nzm > 40) nzm = 40; float nz = fit * nzm;
			float tx = (io.MousePos.x - ip0.x) / oldz, ty = (io.MousePos.y - ip0.y) / oldz;
			w.slPanX = (io.MousePos.x - tx * nz) - ia0.x; w.slPanY = (io.MousePos.y - ty * nz) - ia0.y;
			w.slZoomMul = nzm; z = nz; ip0 = ImVec2(ia0.x + w.slPanX, ia0.y + w.slPanY);
		}
		if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
			w.slUserView = true; w.slPanX += io.MouseDelta.x; w.slPanY += io.MouseDelta.y; ip0 = ImVec2(ia0.x + w.slPanX, ia0.y + w.slPanY);
		}

		auto sX = [&](float tx){ return ip0.x + tx * z; };
		auto sY = [&](float ty){ return ip0.y + ty * z; };
		float exL = (float)t->spriteMarginLeft, exR = (float)(t->width  - t->spriteMarginRight);
		float eyT = (float)t->spriteMarginTop,  eyB = (float)(t->height - t->spriteMarginBottom);

		// ---- draggable handles: outer margin edges + inner grid lines (spacing) ----
		bool inArea = overArea || (w.slDrag != 0);
		auto nearX = [&](float sx){ return fabsf(io.MousePos.x - sx) < 5.0f && io.MousePos.y > c0.y && io.MousePos.y < c0.y + csz.y; };
		auto nearY = [&](float sy){ return fabsf(io.MousePos.y - sy) < 5.0f && io.MousePos.x > c0.x && io.MousePos.x < c0.x + csz.x; };
		int hover = 0;   // 1..4 outer L/R/T/B, 5 spacingX, 6 spacingY; NINE-SLICE mode: 7..10 slice L/R/T/B
		if (w.slDrag == 0 && inArea) {
			if (nineMode)
			{
				if      (nearX(sX((float)t->sliceLeft)))              hover = 7;
				else if (nearX(sX((float)(t->width  - t->sliceRight)))) hover = 8;
				else if (nearY(sY((float)t->sliceTop)))               hover = 9;
				else if (nearY(sY((float)(t->height - t->sliceBottom)))) hover = 10;
			}
			else
			{
			if      (nearX(sX(exL))) hover = 1;
			else if (nearX(sX(exR))) hover = 2;
			else if (nearY(sY(eyT))) hover = 3;
			else if (nearY(sY(eyB))) hover = 4;
			else {
				for (int c = 0; c < t->spriteColumns - 1 && !hover; ++c) { int x0,y0,cw,ch; if (t->SpriteCellRect(c, x0, y0, cw, ch) && nearX(sX((float)(x0 + cw)))) hover = 5; }
				for (int r = 0; r < t->spriteRows - 1 && !hover; ++r)    { int x0,y0,cw,ch; if (t->SpriteCellRect(r * t->spriteColumns, x0, y0, cw, ch) && nearY(sY((float)(y0 + ch)))) hover = 6; }
			}
			}
		}
		if      (hover == 1 || hover == 2 || hover == 5 || hover == 7 || hover == 8)  ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
		else if (hover == 3 || hover == 4 || hover == 6 || hover == 9 || hover == 10) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
		if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) w.slDrag = hover;
		if (w.slDrag && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
			float txp = (io.MousePos.x - ip0.x) / z, typ = (io.MousePos.y - ip0.y) / z;
			auto Rnd = [](float f){ return (int)floorf(f + 0.5f); };
			switch (w.slDrag) {
				case 1: t->spriteMarginLeft   = Rnd(txp); break;
				case 2: t->spriteMarginRight  = Rnd(t->width  - txp); break;
				case 3: t->spriteMarginTop    = Rnd(typ); break;
				case 4: t->spriteMarginBottom = Rnd(t->height - typ); break;
				case 5: t->spriteSpacingX += Rnd(io.MouseDelta.x / z); break;
				case 6: t->spriteSpacingY += Rnd(io.MouseDelta.y / z); break;
				case 7:  t->sliceLeft   = Rnd(txp); break;
				case 8:  t->sliceRight  = Rnd(t->width  - txp); break;
				case 9:  t->sliceTop    = Rnd(typ); break;
				case 10: t->sliceBottom = Rnd(t->height - typ); break;
			}
			clampGrid(); w.dirty = true; SlicerApplyLive(t);
		}
		if (w.slDrag && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) { SlicerPushUndo(w); w.slDrag = 0; }

		// ---- draw sheet + grid (clipped to the image area) ----
		dl->PushClipRect(ia0, ImVec2(ia0.x + iaW, ia0.y + iaH), true);
		ImVec2 ip1 = ImVec2(sX((float)t->width), sY((float)t->height));
		if (w.texPreview) dl->AddImage((ImTextureID)w.texPreview, ip0, ip1);
		dl->AddRect(ip0, ip1, IM_COL32(70, 70, 70, 255));
		int cnt = t->SpriteCount();
		for (int i = 0; i < cnt; ++i) {
			int x0,y0,cw,ch; if (!t->SpriteCellRect(i, x0, y0, cw, ch)) continue;
			ImVec2 p0 = ImVec2(sX((float)x0), sY((float)y0)), p1 = ImVec2(sX((float)(x0 + cw)), sY((float)(y0 + ch)));
			if (p1.x < ia0.x || p0.x > ia0.x + iaW || p1.y < ia0.y || p0.y > ia0.y + iaH) continue;   // cull off-screen cells (bounds vertex count)
			if (i == activeCell) dl->AddRect(p0, p1, IM_COL32(255, 210, 60, 255), 0, 0, 2.0f);
			else                 DashRect(dl, p0, p1, ia0, ImVec2(ia0.x + iaW, ia0.y + iaH), IM_COL32(60, 200, 255, 170), 1.0f);
			if (w.slShowMirror) {
				ImVec2 q0 = ImVec2(p0.x + w.slPadL * z, p0.y + w.slPadT * z), q1 = ImVec2(p1.x - w.slPadR * z, p1.y - w.slPadB * z);
				if (q1.x > q0.x && q1.y > q0.y) dl->AddRect(q0, q1, IM_COL32(255, 120, 120, 110), 0, 0, 1.0f);
			}
		}
		if (nineMode || t->sliceLeft || t->sliceRight || t->sliceTop || t->sliceBottom) {   // 9-slice guides
			ImU32 sc = nineMode ? IM_COL32(120, 255, 120, 255) : IM_COL32(120, 255, 120, 150);
			float th = nineMode ? 2.0f : 1.0f;
			float L = sX((float)t->sliceLeft), R = sX((float)(t->width - t->sliceRight));
			float T = sY((float)t->sliceTop),  B = sY((float)(t->height - t->sliceBottom));
			dl->AddLine(ImVec2(L, sY(0)), ImVec2(L, sY((float)t->height)), sc, th);
			dl->AddLine(ImVec2(R, sY(0)), ImVec2(R, sY((float)t->height)), sc, th);
			dl->AddLine(ImVec2(sX(0), T), ImVec2(sX((float)t->width), T), sc, th);
			dl->AddLine(ImVec2(sX(0), B), ImVec2(sX((float)t->width), B), sc, th);
		}
		ImU32 edge = IM_COL32(255, 140, 40, 220);   // outer margin frame (solid, brighter than the cell grid)
		dl->AddLine(ImVec2(sX(exL), sY(eyT)), ImVec2(sX(exL), sY(eyB)), edge, 1.5f);
		dl->AddLine(ImVec2(sX(exR), sY(eyT)), ImVec2(sX(exR), sY(eyB)), edge, 1.5f);
		dl->AddLine(ImVec2(sX(exL), sY(eyT)), ImVec2(sX(exR), sY(eyT)), edge, 1.5f);
		dl->AddLine(ImVec2(sX(exL), sY(eyB)), ImVec2(sX(exR), sY(eyB)), edge, 1.5f);
		dl->PopClipRect();

		// ---- rulers: strips OUTSIDE the image clip; tick labels live HERE, never on the sheet ----
		ImU32 rbg = IM_COL32(28, 28, 28, 255), rtick = IM_COL32(150, 150, 150, 255), rtxt = IM_COL32(205, 205, 205, 255);
		dl->AddRectFilled(c0, ImVec2(c0.x + csz.x, c0.y + RT), rbg);
		dl->AddRectFilled(c0, ImVec2(c0.x + RL, c0.y + csz.y), rbg);
		const int nice[] = { 1,2,5,10,20,25,50,100,200,250,500,1000,2000,2500,5000,10000,20000 };
		double raw = 64.0 / (z > 1e-6f ? z : 1e-6f); int step = nice[sizeof(nice) / sizeof(int) - 1];
		for (int k = 0; k < (int)(sizeof(nice) / sizeof(int)); ++k) if (nice[k] >= raw) { step = nice[k]; break; }
		char buf[16];
		dl->PushClipRect(ImVec2(c0.x + RL, c0.y), ImVec2(c0.x + csz.x, c0.y + RT), true);
		{ int a = (int)((c0.x + RL - ip0.x) / z); if (a < 0) a = 0; int b = (int)((c0.x + csz.x - ip0.x) / z) + 1; if (b > t->width) b = t->width;
		for (int px = (a / step) * step; px <= b; px += step) { float x = sX((float)px); if (x < c0.x + RL) continue; dl->AddLine(ImVec2(x, c0.y + RT - 5), ImVec2(x, c0.y + RT), rtick); snprintf(buf, 16, "%d", px); dl->AddText(ImVec2(x + 2, c0.y + 2), rtxt, buf); } }
		dl->PopClipRect();
		dl->PushClipRect(ImVec2(c0.x, c0.y + RT), ImVec2(c0.x + RL, c0.y + csz.y), true);
		{ int a = (int)((c0.y + RT - ip0.y) / z); if (a < 0) a = 0; int b = (int)((c0.y + csz.y - ip0.y) / z) + 1; if (b > t->height) b = t->height;
		for (int py = (a / step) * step; py <= b; py += step) { float y = sY((float)py); if (y < c0.y + RT) continue; dl->AddLine(ImVec2(c0.x + RL - 5, y), ImVec2(c0.x + RL, y), rtick); snprintf(buf, 16, "%d", py); dl->AddText(ImVec2(c0.x + 2, y + 1), rtxt, buf); } }
		dl->PopClipRect();
		// draggable handle marks on the rulers at the outer margin edges
		auto handleX = [&](float sx){ if (sx > c0.x + RL) dl->AddTriangleFilled(ImVec2(sx - 4, c0.y), ImVec2(sx + 4, c0.y), ImVec2(sx, c0.y + RT), edge); };
		auto handleY = [&](float sy){ if (sy > c0.y + RT) dl->AddTriangleFilled(ImVec2(c0.x, sy - 4), ImVec2(c0.x, sy + 4), ImVec2(c0.x + RL, sy), edge); };
		handleX(sX(exL)); handleX(sX(exR)); handleY(sY(eyT)); handleY(sY(eyB));
	}
	ImGui::EndChild();

	// ===== RIGHT: live cell preview (nine-slice mode: a STRETCHED preview built from the 9 patches) =====
	ImGui::SameLine();
	ImGui::BeginChild("sl_prev", ImVec2(0, 0), ImGuiChildFlags_Borders);
	if (nineMode)
	{
		ImGui::TextDisabled("Stretch preview");
		static float s_pw = 2.0f, s_ph = 1.5f;   // preview stretch factors (edit-session UI state)
		ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##pw", &s_pw, 0.25f, 4.0f, "W x%.2f");
		ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##ph", &s_ph, 0.25f, 4.0f, "H x%.2f");
		if (w.texPreview && t->width > 0 && t->height > 0)
		{
			// Fit the stretched rect into the panel, then compose 9 sub-images: corners keep their
			// pixel size (scaled by the fit), edges stretch one axis, the centre both.
			ImVec2 av = ImGui::GetContentRegionAvail();
			float outW = t->width * s_pw, outH = t->height * s_ph;
			float fitP = 1.0f;
			if (outW > 0 && outH > 0) { fitP = (av.x - 8) / outW; float fy = (av.y - 8) / outH; if (fy < fitP) fitP = fy; if (fitP <= 0) fitP = 0.01f; }
			float dw = outW * fitP, dh = outH * fitP;
			float bl = t->sliceLeft * fitP, br = t->sliceRight * fitP, bt = t->sliceTop * fitP, bb = t->sliceBottom * fitP;
			if (bl + br > dw && bl + br > 0) { float s = dw / (bl + br); bl *= s; br *= s; }
			if (bt + bb > dh && bt + bb > 0) { float s = dh / (bt + bb); bt *= s; bb *= s; }
			ImVec2 p0 = ImGui::GetCursorScreenPos();
			ImDrawList* pdl = ImGui::GetWindowDrawList();
			float xs[4] = { 0, bl, dw - br, dw };
			float ys[4] = { 0, bt, dh - bb, dh };   // screen top -> bottom
			float us[4] = { 0, (float)t->sliceLeft / t->width, 1.0f - (float)t->sliceRight / t->width, 1.0f };
			float vs[4] = { 0, (float)t->sliceTop / t->height, 1.0f - (float)t->sliceBottom / t->height, 1.0f };
			for (int cxi = 0; cxi < 3; ++cxi)
				for (int ryi = 0; ryi < 3; ++ryi)
				{
					if (xs[cxi + 1] <= xs[cxi] || ys[ryi + 1] <= ys[ryi]) continue;
					pdl->AddImage((ImTextureID)w.texPreview,
					              ImVec2(p0.x + xs[cxi], p0.y + ys[ryi]), ImVec2(p0.x + xs[cxi + 1], p0.y + ys[ryi + 1]),
					              ImVec2(us[cxi], vs[ryi]), ImVec2(us[cxi + 1], vs[ryi + 1]));
				}
			pdl->AddRect(p0, ImVec2(p0.x + dw, p0.y + dh), IM_COL32(120, 255, 120, 120));
			ImGui::Dummy(ImVec2(dw, dh));
		}
	}
	else
	{
		ImGui::TextDisabled("Preview  ·  cell %d", activeCell);
		if (w.texPreview && haveActive && acw > 0 && ach > 0) {
			ImVec2 uv0((float)ax0 / t->width, (float)ay0 / t->height), uv1((float)(ax0 + acw) / t->width, (float)(ay0 + ach) / t->height);
			ImVec2 av = ImGui::GetContentRegionAvail();
			float ar = (float)acw / ach, dw = av.x, dh = dw / ar; if (dh > av.y) { dh = av.y; dw = dh * ar; }
			ImGui::Image((ImTextureID)w.texPreview, ImVec2(dw, dh), uv0, uv1);
		}
	}
	ImGui::EndChild();
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

	// Drag to reparent (within THIS prefab window; world pose is preserved below — core Reparent
	// is pure link surgery and never touches transforms).
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
				// Keep the WORLD pose across the parent change (same behaviour as the main hierarchy).
				Transform& mt = dragged->GetTransform();
				Vector3 wp = mt.globalPosition(); Quaternion wr = mt.globalRotation(); Vector3 ws = mt.globalScale();
				w.pv->world->Reparent(dragged, a);
				mt.SetGlobal(wp, wr, ws);
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
	// NEW detachable DOCUMENT windows (text editor, module editors) follow the same
	// preference as the asset editors.
	NukeUI::DocDetachDefault(detachAssetEditors);

	// NORMAL DOCKING, tear-off half (D3D fallback only — with NATIVE viewports imgui
	// detaches windows itself): the user drags an asset editor's title bar PAST the
	// main-window edge -> the window detaches into a host OS window that keeps following
	// the cursor (the drag continues seamlessly). Releasing it back over the main window
	// re-docks it at the drop point (HostDockDrop, consumed in the loop below).
	if (!NukeUI::NativeViewportsActive())
	if (ImGuiContext* g = ImGui::GetCurrentContext())
		if (g->MovingWindow && g->MovingWindow->RootWindow)
		{
			const char* tag = strstr(g->MovingWindow->RootWindow->Name, "###ae:");
			const ImVec2 m  = ImGui::GetMousePos();
			const ImVec2 ds = ImGui::GetIO().DisplaySize;
			// Torn off when the cursor CLEARLY left the window (margin, e.g. onto another
			// monitor) — or when it is pinned against the VIRTUAL-SCREEN edge (a maximized
			// main window clamps the cursor there, so "past the edge" is unreachable).
			const float out = 12.0f;
			const bool left = m.x < -out || m.y < -out || m.x >= ds.x + out || m.y >= ds.y + out;
			bool clamped = false;
#ifdef _WIN32
			{
				POINT cp; GetCursorPos(&cp);
				const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
				const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
				clamped = cp.x <= vx || cp.y <= vy || cp.x >= vx + vw - 1 || cp.y >= vy + vh - 1;
			}
#endif
			if (tag && (left || clamped))
				for (AssetEditorWin& w : assetEds)
					if (!w.host && w.path == tag + 6)
					{
						w.detached = true; w.dragOut = true;
						ImGui::ClearActiveID();   // hand the drag over to the host window
						g->MovingWindow = nullptr;
						break;
					}
		}

	for (int i = 0; i < (int)assetEds.size(); ++i)
	{
		AssetEditorWin& w = assetEds[i];
		// "Dock back" pressed inside the host window last frame: a host can't destroy
		// itself from within its own content tick, so the request lands here.
		if (w.wantDock)
		{
			if (w.host) { NukeUI::HostDestroy(w.host); w.host = nullptr; }
			w.detached = false; w.wantDock = false; w.wantFocus = true;
		}
		// NATIVE viewports (Vulkan): imgui owns embedding/detaching — no forced modes.
		// DETACHED mode below is the D3D fallback only (PER WINDOW; the preference is the
		// default for newly opened editors): an EDITOR-OWNED OS window (NukeUI host, the
		// Godot model) — we create a borderless GLFW window ourselves, imgui draws into it
		// through its own context, pixels arrive via the GDI blit (the imgui multi-viewport
		// platform-window path raced DXGI into device removal).
		if (w.detached && !NukeUI::NativeViewportsActive())
		{
			if (!w.host)
			{
				const std::string title = bfs::path(w.path).filename().string();
				const bool isSlicerH = (w.ext == ".nutex"), isInputH = (w.ext == ".nuinput");
				const bool isAudioH = !w.pv && !isSlicerH && !isInputH;
				const int hw = isAudioH ? 460 : (w.ext == ".nuprefab" ? 900 : (isSlicerH ? 760 : (isInputH ? 820 : 640)));
				const int hh = isAudioH ? 200 : 640;
				w.host = NukeUI::HostCreate(title.c_str(), hw, hh);
				const std::string keyPath = w.path;   // vector may reallocate: look up by path
				NukeUI::HostSetContent(w.host, [this, keyPath]()
				{
					for (int k = 0; k < (int)assetEds.size(); ++k)
						if (assetEds[k].path == keyPath)
						{
							if (assetEds[k].host && NukeUI::HostFocused(assetEds[k].host)) aeFocused = k;
							DrawAssetEditorBody(k);
							return;
						}
				});
			}
			// Tear-off: the host was born mid-drag — it picks the drag up and rides the
			// cursor until the user lets go (release back over the main window = re-dock).
			if (w.dragOut) { NukeUI::HostBeginDrag(w.host, 220.0f, 12.0f); w.dragOut = false; }
			// Content-window flags mirror the docked window: dirty dot + the same
			// no-scrollbar rule for preview editors (see the docked path below).
			{
				const bool isInputH = (w.ext == ".nuinput");
				NukeUI::HostSetContentFlags(w.host, (w.dirty ? ImGuiWindowFlags_UnsavedDocument : 0)
				    | (isInputH ? 0 : (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)));
			}
			// Dropped back onto the main window: re-dock at the drop point.
			float dropX = 0, dropY = 0;
			if (NukeUI::HostDockDrop(w.host, &dropX, &dropY))
			{
				w.wantDock = true; w.hasDrop = true;
				w.dropX = dropX; w.dropY = dropY;
			}
			if (w.wantFocus) { NukeUI::HostFocus(w.host); w.wantFocus = false; }
			if (!NukeUI::HostAlive(w.host)) w.open = false;   // OS close button
			if (!w.open && w.dirty)
			{
				w.open = true;                // keep it until the user answers (modal below)
				aeCloseConfirm = i;
			}
			continue;                          // content is drawn by the host tick, not here
		}
		if (w.wantFocus) { ImGui::SetNextWindowFocus(); w.wantFocus = false; }
		if (w.hasDrop)
		{
			// Re-docked by drag: appear right where the user dropped it (title bar under
			// the cursor), floating — from there normal imgui docking (drag onto panels /
			// dock nodes with the usual preview overlays) takes over.
			w.hasDrop = false;
			ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
			ImGui::SetNextWindowPos(ImVec2(ImMax(0.0f, w.dropX - 220.0f), ImMax(0.0f, w.dropY - 10.0f)), ImGuiCond_Always);
		}
		else if (!NukeUI::NativeViewportsActive())
		{
			// PIN to the main viewport (D3D fallback only): imgui remembers a previously-
			// detached screen position and would silently auto-spawn an OS window again —
			// the fallback must never leave the main window on its own. With NATIVE
			// viewports leaving the main window is exactly what windows are ALLOWED to do.
			ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
			ImVec2 wp = ImGui::GetMainViewport()->WorkPos;
			ImGui::SetNextWindowPos(ImVec2(wp.x + 80.0f, wp.y + 80.0f), ImGuiCond_Appearing);
		}
		const bool isSlicer = (w.ext == ".nutex");   // 2D Sprite Slicer — no 3D scene, but not audio either
		const bool isInput  = (w.ext == ".nuinput"); // gameplay input map — pure data CRUD, no scene
		const bool isAudio = !w.pv && !isSlicer && !isInput;   // audio previews are the only OTHER sceneless editors
		ImGui::SetNextWindowSize(isAudio ? ImVec2(420.0f, 170.0f)
		                                 : ImVec2(w.ext == ".nuprefab" ? 900.0f : (isSlicer ? 760.0f : (isInput ? 820.0f : 420.0f)), 640.0f), ImGuiCond_FirstUseEver);
		const char* icon = isAudio ? ICON_LC_MUSIC : isSlicer ? ICON_LC_GRID_2X2 : isInput ? ICON_LC_SETTINGS_2
		                 : w.ext == ".numat" ? ICON_LC_PALETTE : (w.ext == ".numesh" ? ICON_LC_BOX : ICON_LC_PACKAGE);
		std::string title = std::string(icon) + " " + bfs::path(w.path).filename().string() + "###ae:" + w.path;
		// No window scrollbars for PREVIEW editors: the 3D/image view is sized to the free space and
		// a flickering scrollbar would oscillate that size every frame (children scroll themselves).
		// Pure-form editors (.nuinput) have flowing content and DO scroll like a normal window.
		ImGuiWindowFlags wf = window_flags | (w.dirty ? ImGuiWindowFlags_UnsavedDocument : 0)
		                    | (isInput ? 0 : (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse));
		if (ImGui::Begin(title.c_str(), &w.open, wf))
			DrawAssetEditorBody(i);
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
				else if (w.ext == ".nutex"    && w.tex)        { w.tex->SaveToFile(w.path); SlicerApplyLive(w.tex); }
				else if (w.ext == ".nuinput")                  SaveInputAsset(w);
				w.dirty = false; w.open = false; aeCloseConfirm = -1; ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Discard")) { w.dirty = false; w.open = false; aeCloseConfirm = -1; ImGui::CloseCurrentPopup(); }
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				if (w.host) NukeUI::HostCancelClose(w.host);   // un-stick the OS close request
				aeCloseConfirm = -1; ImGui::CloseCurrentPopup();
			}
		}
		else { aeCloseConfirm = -1; ImGui::CloseCurrentPopup(); }
		ImGui::EndPopup();
	}

	// Tear down closed editors (scene goes back to the pool; audio voices stop).
	for (int i = (int)assetEds.size() - 1; i >= 0; --i)
		if (!assetEds[i].open)
		{
			AssetEditorWin& w = assetEds[i];
			if (w.audioVoice) { nuke::Audio::Stop((double)w.audioVoice); w.audioVoice = 0; }
			if (w.prefabRoot && w.pv) w.pv->world->RemoveAtomById((long)w.prefabRoot->id.id);
			if (w.mat) delete w.mat;
			delete w.idleM;
			for (Material* m : w.undoM) delete m;
			for (Material* m : w.redoM) delete m;
			if (w.texPreview)   // destruction is deferred INSIDE the renderer (centralized GPU trash)
			{
				if (iRender* r = AppInstance::GetSingleton()->render) r->destroyTexture2D(w.texPreview);
				w.texPreview = 0;
			}
			if (w.tex) delete w.tex;
			if (w.pv) ReleasePreview(w.pv);
			if (w.host) { NukeUI::HostDestroy(w.host); w.host = nullptr; }
			assetEds.erase(assetEds.begin() + i);
		}
}

// The full content of ONE asset editor (everything the old inline Begin..End body did).
// Shared by both hosts: the docked/floating imgui window in the MAIN context, and the
// EDITOR-OWNED OS window (NukeUI host, detached mode) whose content callback calls this
// inside its own ImGui context.
void EditorUI::DrawAssetEditorBody(int i)
{
	AssetEditorWin& w = assetEds[i];
	// Type flags (recomputed; the docked path also computes them for sizing).
	const bool isSlicer = (w.ext == ".nutex");
	const bool isInput  = (w.ext == ".nuinput");
	const bool isAudio  = !w.pv && !isSlicer && !isInput;
	(void)isAudio;
			const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
			if (focused) aeFocused = i;   // Ctrl+Z/Ctrl+Y route to THIS window's history
			bool wantSave = false;
			if (w.ext != ".numesh" && !isAudio)
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
					else if (w.ext == ".nutex" && w.tex)
					{
						if (nuke::Texture* fresh = nuke::Texture::LoadFromFile(w.path))
						{
							ApplyMeta(w.tex, SnapMeta(fresh)); delete fresh;
							w.idleS = SnapMeta(w.tex); w.undoS.clear(); w.redoS.clear();
							SlicerApplyLive(w.tex);
						}
					}
					else if (w.ext == ".nuinput")
					{
						bfs::ifstream f{ bfs::path(w.path) };
						std::string js((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
						nuke::Input::InputMapData m = nuke::Input::ParseMapString(js);
						w.inActions = m.actions; w.inContexts = m.contexts;
						w.idleI = nuke::Input::SerializeMap(m); w.undoI.clear(); w.redoI.clear();
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
			else if (isSlicer && w.tex)
			{
				DrawSpriteSlicer(w);
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
			else if (isAudio)   // audio preview: transport on the Preview bus (never game-paused)
			{
				const bool playing = nuke::Audio::IsPlaying((double)w.audioVoice);
				if (!nuke::Audio::Available())
					ImGui::TextDisabled("No audio provider is enabled (Plugins -> NukeAudio).");
				else
				{
					if (ImGui::Button(playing ? ICON_LC_SQUARE " Stop" : ICON_LC_PLAY " Play", ImVec2(90, 0)))
					{
						if (playing) { nuke::Audio::Stop((double)w.audioVoice); w.audioVoice = 0; }
						else         w.audioVoice = (uint64_t)nuke::Audio::Play(w.path, w.audioVol, false, 2);
					}
					ImGui::SameLine();
					ImGui::SetNextItemWidth(140);
					if (ImGui::SliderFloat("Volume", &w.audioVol, 0.0f, 2.0f, "%.2f"))
						if (playing) nuke::Audio::SetVolume((double)w.audioVoice, w.audioVol);

					float cur = playing ? (float)nuke::Audio::Time((double)w.audioVoice) : 0.0f;
					float len = playing ? (float)nuke::Audio::Length((double)w.audioVoice) : 0.0f;
					if (playing && len > 0.01f)
					{
						float pos = cur;
						ImGui::SetNextItemWidth(-1);
						if (ImGui::SliderFloat("##seek", &pos, 0.0f, len,
						                       (std::to_string((int)pos / 60) + ":" + (pos - 60 * (int)(pos / 60) < 9.5f ? "0" : "")
						                        + std::to_string((int)(pos) % 60) + " / "
						                        + std::to_string((int)len / 60) + ":" + ((int)len % 60 < 10 ? "0" : "")
						                        + std::to_string((int)len % 60)).c_str()))
							nuke::Audio::Seek((double)w.audioVoice, (double)pos);
					}
					else ImGui::TextDisabled(playing ? "streaming..." : "stopped");
				}
			}
			else if (isInput)
				DrawInputEditor(w);   // actions / contexts / bindings CRUD + press-to-bind (edits w.in*)

			// Save (button or Ctrl+S while focused).
			if (focused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) wantSave = true;
			if (wantSave)
			{
				if      (w.ext == ".numat"    && w.mat)        { w.mat->SaveToFile(w.path); w.dirty = false; }
				else if (w.ext == ".nuprefab" && w.prefabRoot) { nuke::SavePrefab(w.prefabRoot, w.path); w.dirty = false; }
				else if (isSlicer && w.tex)                    { w.tex->SaveToFile(w.path); SlicerApplyLive(w.tex); w.dirty = false; }
				else if (isInput)                              { SaveInputAsset(w); w.dirty = false; }
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
				else if (w.ext == ".nuinput" && !w.idleI.empty())
				{
					w.undoI.push_back(w.idleI);
					if (w.undoI.size() > kAeUndoCap) w.undoI.erase(w.undoI.begin());
					w.redoI.clear();
				}
				w.editing = true;
			}
			if (w.editing && !w.editedNow && !ImGui::IsAnyItemActive() && !(w.pv && w.pv->gizmoBusy))
			{
				// The burst settled: the CURRENT state becomes the next baseline.
				w.editing = false;
				if      (w.ext == ".nuprefab" && w.prefabRoot) w.idleP = nuke::SaveAtomToString(w.prefabRoot);
				else if (w.ext == ".numat"    && w.mat)        { delete w.idleM; w.idleM = w.mat->Clone(); }
				else if (w.ext == ".nuinput")                  w.idleI = InputMapJson(w);
			}
			w.editedNow = false;
}
