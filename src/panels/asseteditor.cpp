// Per-asset editor windows (material / mesh / prefab / texture / input map), each with a
// live 3D view backed by a pooled preview world rendered before the main scene.
#include <editor/editorui.h>
#include <API/Model/Material.h>
#include <API/Model/Texture.h>
#include <API/Model/Light.h>
#include <API/Model/Environment.h>
#include <API/Model/Prefab.h>
#include <API/Model/Surface.h>   // trigger tool: preview hit reactions (Hit + DrainHits)
#include <set>
#include <API/Model/Audio.h>
#include <input/Input.h>
#include <interface/AssetCreators.h>   // module-supplied asset editors
#include "nukeui.h"                     // NukeUI host windows for detached editors
#include "imgui_internal.h"             // MovingWindow/ClearActiveID — drag-out detach
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>                    // GetCursorPos/GetSystemMetrics — screen-edge tear-off
#endif
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <iterator>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/type_ptr.hpp>
namespace bfs = boost::filesystem;

// Sprite-slice metadata helpers, defined near the slicer below.
static EditorUI::SpriteMeta SnapMeta(const nuke::Texture* t);
static void ApplyMeta(nuke::Texture* t, const EditorUI::SpriteMeta& m);
static void SlicerApplyLive(nuke::Texture* t);

// Clip metadata snapshots for the .nuanim editor's undo (animeditor.cpp).
std::string EditorAnimMetaJson(const nuke::AnimClip* c);
void        EditorAnimMetaLoad(nuke::AnimClip* c, const std::string& json);
// Bone-map snapshots for the .nubonemap editor's undo (skeleteditor.cpp).
std::string EditorBoneMapJson(const nuke::BoneMap* b);
void        EditorBoneMapLoad(nuke::BoneMap* b, const std::string& json);

// Generic reflected-field copy between two instances of the SAME type (preview-environment
// mirroring): typed through Field::addr, so module-added fields follow automatically.
static void CopyReflectedField(const nuke::Field& f, void* src, void* dst)
{
	void* sp = f.addr(src);
	void* dp = f.addr(dst);
	if (!sp || !dp) return;
	using nuke::FT;
	switch (f.type)
	{
		case FT::Bool:   *(bool*)dp = *(bool*)sp; break;
		case FT::Int:    *(int*)dp = *(int*)sp; break;
		case FT::Float:  *(float*)dp = *(float*)sp; break;
		case FT::Double: *(double*)dp = *(double*)sp; break;
		case FT::String: *(std::string*)dp = *(std::string*)sp; break;
		case FT::Color:  *(nuke::Color*)dp = *(nuke::Color*)sp; break;
		case FT::Vec2:   *(nuke::Vector2*)dp = *(nuke::Vector2*)sp; break;
		case FT::Vec3:   *(nuke::Vector3*)dp = *(nuke::Vector3*)sp; break;
		case FT::Vec4:   *(nuke::Vector4*)dp = *(nuke::Vector4*)sp; break;
		case FT::Quat:   *(nuke::Quaternion*)dp = *(nuke::Quaternion*)sp; break;
		default: break;   // lists/refs are not part of the environment mirror
	}
}

// --- Preview-scene pool ----------------------------------------------------

// Take a free pooled preview world, creating one (RT + env/sun/mesh/camera) if needed.
EditorUI::PreviewWorld* EditorUI::AcquirePreview()
{
	for (PreviewWorld* s : pvPool)
		if (!s->inUse)
		{
			s->inUse = true;
			s->yaw = 0.7f; s->pitch = 0.35f; s->dist = 0.0f;
			s->orbit = false; s->locked = false;          // pool reuse must not leak modes
			if (s->world) s->world->editorGrid = true;    // ...or a hidden grid
			return s;
		}
	iRender* r = AppInstance::GetSingleton()->render;
	if (!r) return nullptr;

	PreviewWorld* s = new PreviewWorld();
	s->rt = r->createRenderTarget(384, 384);
	s->world = new World();
	s->world->name = "Asset Preview";
	s->world->auxiliary = true;   // skip global heavy passes (RT TLAS)

	Atom* env = new Atom("PreviewEnv");
	env->AddComponent(new Environment());
	s->world->Add(env);

	Atom* sun = new Atom("PreviewSun");
	Light* l = new Light();
	l->type = Light::Directional;
	l->castShadows = false;
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

// Return a preview world to the pool, clearing the staged mesh/material.
void EditorUI::ReleasePreview(PreviewWorld* s)
{
	if (!s) return;
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
	{
		// Lazily-resolved renderers (skinned instances before the first ApplyPose) still have
		// a null mesh — fall back to the ASSET's bind-pose bounds, or framing collapses to a
		// unit sphere at the origin (dead F-focus + microscopic camera speeds on huge models).
		nuke::Mesh* bm = mr->mesh;
		if (!bm && !mr->meshGuid.empty()) bm = nuke::ResDB::getSingleton()->GetMesh(mr->meshGuid);
		if (bm)
		{
			bm->EnsureBounds();
			const float* mn = bm->aabbMin;
			const float* mx = bm->aabbMax;
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
				const double ddx = wc.x - c.x, ddy = wc.y - c.y, ddz = wc.z - c.z;
				const float d = (float)std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
				const float nr = std::max(r, d + lr);
				c = Vector3((c.x + wc.x) * 0.5, (c.y + wc.y) * 0.5, (c.z + wc.z) * 0.5);
				r = nr;
			}
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
	s.dist = 0.0f;   // re-derived from radius on next draw
}

void EditorUI::DrawPreviewImage(PreviewWorld& s, ImVec2 size)
{
	iRender* r = AppInstance::GetSingleton()->render;
	// Camera::Init fills `transform`, not `atom`.
	if (!r || !s.cam || !s.cam->transform) return;
	if (ImGuiViewport* vp = ImGui::GetWindowViewport())
		if (vp->Flags & ImGuiViewportFlags_IsMinimized) return;   // minimized: skip, iconified sizes churn the RT

	// Auto-frame once per staging (dist == 0); afterwards the camera is driven by the controls below.
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

	float pw = size.x, ph = size.y;
	if (pw < 64.0f) pw = 64.0f;
	if (ph < 64.0f) ph = 64.0f;

	// RT size follows the rect but quantized (16px) and debounced: a per-frame resize
	// leaves the texture empty (the world fills it only next frame) and churns GPU memory.
	const int qw = std::max(64, ((int)pw + 15) / 16 * 16);
	const int qh = std::max(64, ((int)ph + 15) / 16 * 16);
	if (qw != s.rtW || qh != s.rtH)
	{
		if (qw == s.wantW && qh == s.wantH) ++s.wantFrames;
		else { s.wantW = qw; s.wantH = qh; s.wantFrames = 1; }
		if (s.wantFrames >= 5 || s.rtW <= 0)   // settled (or first size)
		{
			r->resizeRenderTarget(s.rt, qw, qh);
			s.rtW = qw; s.rtH = qh;
			s.wantFrames = 0;
		}
	}
	else s.wantFrames = 0;
	uint64_t tex = r->getRenderTargetTexture(s.rt);
	if (!tex) return;

	// Must stay a plain Image (no InvisibleButton): an interactive item here steals clicks from ImGuizmo.
	const ImVec2 p0 = ImGui::GetCursorScreenPos();
	s.rectMin = p0; s.rectSize = ImVec2(pw, ph);   // gizmo overlay targets this rect
	// Show the top-left pw x ph region of the quantized RT — 1:1 pixels.
	const ImVec2 uv1(std::min(1.0f, pw / (float)s.rtW), std::min(1.0f, ph / (float)s.rtH));
	ImGui::Image((ImTextureID)tex, ImVec2(pw, ph), ImVec2(0, 0), uv1);
	// The preview must LOOK like the scene: mirror the live world's Environment (sky, ambient,
	// exposure/white point) and its first directional light onto the preview world's pair.
	{
		World* live = AppInstance::GetSingleton()->currentWorld;
		if (live && s.world)
		{
			Environment* srcEnv = nullptr;
			Light* srcSun = nullptr;
			for (Atom* a : live->GetHierarchy())
			{
				if (!a) continue;
				if (!srcEnv) srcEnv = a->GetComponent<Environment>();
				if (!srcSun)
					if (Light* l = a->GetComponent<Light>())
						if (l->type == Light::Directional) srcSun = l;
				if (srcEnv && srcSun) break;
			}
			if (Atom* pe = s.world->Get("PreviewEnv"))
				if (Environment* dstEnv = pe->GetComponent<Environment>())
					if (srcEnv)
					{
						// generic reflected copy — module-added fields follow automatically
						if (nuke::TypeInfo* ti = srcEnv->GetType())
							for (const nuke::Field& f : ti->fields)
								CopyReflectedField(f, srcEnv, dstEnv);
						dstEnv->daySpeed = 0.0f;   // the aux world must not tick its own clock
					}
			if (Atom* ps = s.world->Get("PreviewSun"))
				if (Light* dstSun = ps->GetComponent<Light>())
					if (srcSun && srcSun->atom)
					{
						dstSun->intensity = srcSun->intensity;
						dstSun->color = srcSun->color;
						ps->GetTransform().rotation = srcSun->atom->GetTransform().globalRotation();
					}
		}
	}

	ImGuiIO& io = ImGui::GetIO();
	if (s.locked) { s.visible = true; return; }   // static shot: renders, takes no camera input
	if (ImGui::IsItemHovered() && s.orbit)
	{
		// ORBIT mode (material editor): RMB circles the subject, wheel dollies toward it,
		// MMB shifts the pivot. yaw/pitch/dist stay authoritative; the camera is re-placed
		// from them, so it can never drift away from the sample.
		Transform* t = s.cam->transform;
		const float rotSpeed = 0.008f;
		bool moved = false;
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Right))
		{
			s.yaw   += io.MouseDelta.x * rotSpeed;
			s.pitch += io.MouseDelta.y * rotSpeed;   // drag up = camera rises above the subject
			const float lim = 1.55f;
			if (s.pitch >  lim) s.pitch =  lim;
			if (s.pitch < -lim) s.pitch = -lim;
			moved = true;
		}
		if (io.MouseWheel != 0.0f)
		{
			s.dist *= 1.0f - io.MouseWheel * 0.12f;
			const float mn = s.radius * 0.25f, mx = s.radius * 40.0f;
			if (s.dist < mn) s.dist = mn;
			if (s.dist > mx) s.dist = mx;
			moved = true;
		}
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
		{
			const double pan = 0.0025 * std::max(0.2f, s.dist);
			s.center += t->right() * (-io.MouseDelta.x * pan) + t->up() * (io.MouseDelta.y * pan);
			moved = true;
		}
		if (moved)
		{
			const double cp = std::cos(s.pitch), sp = std::sin(s.pitch);
			const double sy = std::sin(s.yaw),   cy = std::cos(s.yaw);
			Vector3 dir(cp * sy, sp, cp * cy);   // center -> camera (same frame as auto-frame)
			t->position = Vector3(s.center.x + dir.x * s.dist, s.center.y + dir.y * s.dist, s.center.z + dir.z * s.dist);
			const double fp  = std::asin(dir.y);
			const double fyw = std::atan2(-dir.x, -dir.z);
			t->SetEulerDeg(Vector3(fp * 57.29577951308232, fyw * 57.29577951308232, 0.0));
		}
	}
	else if (ImGui::IsItemHovered())
	{
		// RMB = look, MMB = pan, wheel = dolly (or speed while flying), RMB + WASD/QE = fly.
		Transform* t = s.cam->transform;
		const float k = std::max(0.2f, s.radius) * s.flyMul;
		const float rotSpeed = 0.005f, panSpeed = 0.0025f * k, zoomSpeed = 0.25f * k;

		// Re-derive yaw/pitch from forward on drag start; recomputing from eulers snaps.
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
		{
			if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
			{
				// wheel while flying tunes the speed multiplier (UE-style), not the dolly
				s.flyMul = std::max(0.02f, std::min(100.0f, s.flyMul * (1.0f + io.MouseWheel * 0.15f)));
				s.flyMulHud = 1.2f;
			}
			else
				t->position += t->direction() * (double)(io.MouseWheel * zoomSpeed);
		}

		if (ImGui::IsMouseDown(ImGuiMouseButton_Right))   // free flight
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
	if (s.flyMulHud > 0.0f)   // transient speed HUD after a wheel change
	{
		s.flyMulHud -= io.DeltaTime;
		char hud[32];
		snprintf(hud, sizeof(hud), "speed x%.2f", s.flyMul);
		ImGui::GetWindowDrawList()->AddText(ImVec2(p0.x + 8, p0.y + 6), IM_COL32(255, 255, 255, 220), hud);
	}

	s.visible = true;   // ask the render hook to draw this scene this frame
}

// One material-parameter picker for EVERYTHING that targets a param (tweens, event
// set-params): built-ins + live scalars by their inspector labels, every reflected
// numeric/color prop, per-mask fields. Resolves the CURRENT value first so callers get the
// target's dimension/color-ness even when the param came from the free-text field.
struct MatParamPick { int dim = 4; bool color = false; };
static bool MaterialParamCombo(nuke::Material* m, const char* label, std::string& param, MatParamPick& out)
{
	struct TwTarget { const char* key; const char* lbl; int dim; bool color; };
	static const TwTarget kTw[] = {
		{ "uv",                "UV Scroll",          2, false },
		{ "wipe",              "Wipe",               1, false },
		{ "color",             "Base Color",         4, true  },
		{ "emissive",          "Emissive",           4, true  },
		{ "emissiveIntensity", "Emissive Intensity", 1, false },
		{ "metallic",          "Metallic",           1, false },
		{ "roughness",         "Roughness",          1, false },
		{ "specular",          "Specular",           1, false },
		{ "parallax",          "Parallax",           1, false },
		{ "dispScale",         "Disp Scale",         1, false },
		{ "dispMid",           "Disp Mid",           1, false },
		{ "varAmount",         "Variation",          1, false },
		{ "varScale",          "Var Cell",           1, false },
		{ "varHue",            "Var Hue",            1, false },
		{ "footVolume",        "Step Volume",        1, false },
		{ "ambientVolume",     "Ambient Volume",     1, false },
		{ "windVolume",        "Wind Volume",        1, false },
	};
	auto ieq = [](const std::string& a, const char* b) {
		size_t q = 0;
		for (; q < a.size() && b[q]; ++q)
			if (tolower((unsigned char)a[q]) != tolower((unsigned char)b[q])) return false;
		return q == a.size() && !b[q];
	};
	auto builtin = [&](const std::string& p) -> const TwTarget* {
		if (p.empty()) return nullptr;
		for (const TwTarget& t : kTw) if (ieq(p, t.key) || ieq(p, t.lbl)) return &t;
		return nullptr;
	};
	auto isNum = [](nuke::FT t) {
		return t == nuke::FT::Float || t == nuke::FT::Double || t == nuke::FT::Vec2
		    || t == nuke::FT::Vec3 || t == nuke::FT::Vec4 || t == nuke::FT::Color;
	};
	auto reflected = [&](const std::string& p) -> const nuke::Field* {
		if (p.empty()) return nullptr;
		if (nuke::TypeInfo* ti = m->GetType())
			for (const nuke::Field& f : ti->fields)
				if (isNum(f.type) && (ieq(p, f.name.c_str())
				 || (!f.label.empty() && ieq(p, f.label.c_str())))) return &f;
		return nullptr;
	};
	const TwTarget*    bt = builtin(param);
	const nuke::Field* pf = bt ? nullptr : reflected(param);
	out.color = (bt && bt->color) || (pf && pf->type == nuke::FT::Color);
	out.dim = bt ? bt->dim
	        : pf ? (pf->type == nuke::FT::Vec2 ? 2 : pf->type == nuke::FT::Vec3 ? 3
	              : pf->type == nuke::FT::Vec4 ? 4 : 1)
	        : (param.rfind("mask:", 0) == 0 ? 1 : 4);
	if (out.color) out.dim = 4;
	const char* shown = bt ? bt->lbl
	                  : pf ? (pf->label.empty() ? pf->name.c_str() : pf->label.c_str())
	                  : param.empty() ? "(pick)" : param.c_str();
	bool ch = false;
	if (ImGui::BeginCombo(label, shown))
	{
		for (const TwTarget& t : kTw)
			if (ImGui::Selectable(t.lbl, bt == &t)) { param = t.key; ch = true; }
		if (nuke::TypeInfo* ti = m->GetType())
		{
			ImGui::Separator();
			for (const nuke::Field& f : ti->fields)
			{
				if (!isNum(f.type) || builtin(f.name)
				 || (!f.label.empty() && builtin(f.label))) continue;
				const std::string fl = (f.label.empty() ? f.name : f.label) + "##f" + f.name;
				if (ImGui::Selectable(fl.c_str(), pf == &f)) { param = f.name; ch = true; }
			}
		}
		if (!m->liveMasks.empty())
		{
			ImGui::Separator();
			ImGui::TextDisabled("Mask params");
			static const char* kMf[] = { "scale", "repeat", "rotation", "fade", "softness", "strength", "cx", "cy", "cz" };
			for (const nuke::LiveMask& mk : m->liveMasks)
				for (const char* mf : kMf)
				{
					const std::string mkey = "mask:" + mk.name + ":" + mf;
					const std::string mlbl = "Mask " + mk.name + ": " + mf + "##mk" + mkey;
					if (ImGui::Selectable(mlbl.c_str(), param == mkey)) { param = mkey; ch = true; }
				}
		}
		ImGui::EndCombo();
	}
	return ch;
}

// Trigger-tool click: unproject the mouse through the preview camera, raycast the preview
// mesh (LOD0, identity transform, Moller-Trumbore), fire the picked event AT the hit —
// a UV or world point, depending on the space of the mask the event drives.
void EditorUI::FireEventAtPreview(AssetEditorWin& w)
{
	PreviewWorld& s = *w.pv;
	nuke::Material* pm = s.mr ? s.mr->mat : nullptr;
	nuke::Mesh* mesh = s.mr ? s.mr->mesh : nullptr;
	if (!pm || !mesh || !mesh->vertexArray || !s.cam || !s.cam->transform || w.evtSel < 0
	 || w.evtSel >= (int)(pm->liveEvents.size() + pm->liveHits.size())) return;
	const ImVec2 mp = ImGui::GetMousePos();
	const double rw = std::max(1.0f, s.rectSize.x), rh = std::max(1.0f, s.rectSize.y);
	const double ndcx = (mp.x - s.rectMin.x) / rw * 2.0 - 1.0;
	const double ndcy = 1.0 - (mp.y - s.rectMin.y) / rh * 2.0;
	Transform* ct = s.cam->transform;
	const Vector3 o = ct->globalPosition();
	const Vector3 f = ct->direction(), r = ct->right(), u = ct->up();
	const double thf = std::tan((double)s.cam->fov * 0.5 * 0.017453292519943295);
	Vector3 d(f.x + ndcx * thf * (rw / rh) * r.x + ndcy * thf * u.x,
	          f.y + ndcx * thf * (rw / rh) * r.y + ndcy * thf * u.y,
	          f.z + ndcx * thf * (rw / rh) * r.z + ndcy * thf * u.z);
	{ const double L = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z); if (L > 1e-12) { d.x /= L; d.y /= L; d.z /= L; } }
	float bestT = 1e30f, bu = 0, bv = 0; int bTri = -1;
	const int tris = mesh->TriCount();
	for (int t = 0; t < tris; ++t)
	{
		const uint32_t i0 = mesh->TriIndex(t, 0), i1 = mesh->TriIndex(t, 1), i2 = mesh->TriIndex(t, 2);
		const float* p0 = mesh->vertexArray + i0 * 3;
		const float* p1 = mesh->vertexArray + i1 * 3;
		const float* p2 = mesh->vertexArray + i2 * 3;
		const double e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
		const double e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
		const double px = d.y * e2[2] - d.z * e2[1], py = d.z * e2[0] - d.x * e2[2], pz = d.x * e2[1] - d.y * e2[0];
		const double det = e1[0] * px + e1[1] * py + e1[2] * pz;
		if (std::fabs(det) < 1e-12) continue;
		const double inv = 1.0 / det;
		const double tv[3] = { o.x - p0[0], o.y - p0[1], o.z - p0[2] };
		const double uu = (tv[0] * px + tv[1] * py + tv[2] * pz) * inv;
		if (uu < 0.0 || uu > 1.0) continue;
		const double qx = tv[1] * e1[2] - tv[2] * e1[1], qy = tv[2] * e1[0] - tv[0] * e1[2], qz = tv[0] * e1[1] - tv[1] * e1[0];
		const double vv = (d.x * qx + d.y * qy + d.z * qz) * inv;
		if (vv < 0.0 || uu + vv > 1.0) continue;
		const double tt = (e2[0] * qx + e2[1] * qy + e2[2] * qz) * inv;
		if (tt > 1e-4 && tt < bestT) { bestT = (float)tt; bu = (float)uu; bv = (float)vv; bTri = t; }
	}
	if (bTri < 0) { std::cout << "[trigger]\tclick missed the sample (no triangle hit)" << std::endl; return; }
	if (w.evtSel >= (int)pm->liveEvents.size())
	{
		// HIT REACTION: fire Surface::Hit on the preview atom at the hit point, then drain
		// the spawn queue into the PREVIEW world at once — the live world must never get it.
		const int hi = w.evtSel - (int)pm->liveEvents.size();
		if (hi >= (int)pm->liveHits.size()) return;
		const Vector3 hp(o.x + d.x * bestT, o.y + d.y * bestT, o.z + d.z * bestT);
		const uint32_t i0 = mesh->TriIndex(bTri, 0), i1 = mesh->TriIndex(bTri, 1), i2 = mesh->TriIndex(bTri, 2);
		const float* p0v = mesh->vertexArray + i0 * 3;
		const float* p1v = mesh->vertexArray + i1 * 3;
		const float* p2v = mesh->vertexArray + i2 * 3;
		const double e1[3] = { p1v[0] - p0v[0], p1v[1] - p0v[1], p1v[2] - p0v[2] };
		const double e2[3] = { p2v[0] - p0v[0], p2v[1] - p0v[1], p2v[2] - p0v[2] };
		Vector3 N(e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]);
		const double nl = std::sqrt(N.x * N.x + N.y * N.y + N.z * N.z);
		if (nl > 1e-12) { N.x /= nl; N.y /= nl; N.z /= nl; }
		if (N.x * d.x + N.y * d.y + N.z * d.z > 0.0) { N.x = -N.x; N.y = -N.y; N.z = -N.z; }   // face the ray
		const std::string& ht = pm->liveHits[hi].hitType;
		std::cout << "[trigger]\thit '" << (ht.empty() ? "(any)" : ht.c_str()) << "' at world ("
		          << hp.x << ", " << hp.y << ", " << hp.z << ")" << std::endl;
		nuke::Surface::HitIn(s.world, s.meshAtom, ht, hp, N, 1e9);
		nuke::Surface::DrainHits(s.world);
		return;
	}
	const nuke::LiveEvent& ev = pm->liveEvents[w.evtSel];
	// Which mask receives the point? Same routing as the engine: the modulating mask of a
	// started tween, or the target of a "mask:<name>:*" tween/set-param.
	auto maskNamed = [&](const std::string& n) -> const nuke::LiveMask* {
		if (n.empty()) return nullptr;
		for (const nuke::LiveMask& mk : pm->liveMasks)
			if (mk.name == n) return &mk;
		return nullptr;
	};
	auto maskOfParam = [](const std::string& p) -> std::string {
		if (p.rfind("mask:", 0) != 0) return std::string();
		const size_t c2 = p.find(':', 5);
		return c2 == std::string::npos ? std::string() : p.substr(5, c2 - 5);
	};
	const nuke::LiveMask* bound = nullptr;
	for (const std::string& tn : ev.startTweens)
		for (const nuke::LiveTween& tw : pm->liveTweens)
		{
			const std::string& nm = tw.name.empty() ? tw.param : tw.name;
			if (tn != nm) continue;
			if (!bound) bound = maskNamed(tw.mask);
			if (!bound) bound = maskNamed(maskOfParam(tw.param));
			break;
		}
	for (size_t sp = 0; !bound && sp < ev.setParams.size(); ++sp)
		bound = maskNamed(maskOfParam(ev.setParams[sp]));
	if (!bound)
		std::cout << "[trigger]\tevent '" << ev.name << "' touches no mask (started tweens have no"
		             " Mask and no mask: targets) — the click point has nowhere to land" << std::endl;
	// Both points are known here — TriggerAtHit routes each mask in its AUTHORED space, so
	// the tool shows exactly the size a gameplay hit will produce.
	const Vector3 hp(o.x + d.x * bestT, o.y + d.y * bestT, o.z + d.z * bestT);
	if (mesh->uvArray)
	{
		const uint32_t i0 = mesh->TriIndex(bTri, 0), i1 = mesh->TriIndex(bTri, 1), i2 = mesh->TriIndex(bTri, 2);
		const float* t0 = mesh->uvArray + i0 * 2;
		const float* t1 = mesh->uvArray + i1 * 2;
		const float* t2 = mesh->uvArray + i2 * 2;
		const float hu = t0[0] * (1 - bu - bv) + t1[0] * bu + t2[0] * bv;
		const float hv = t0[1] * (1 - bu - bv) + t1[1] * bu + t2[1] * bv;
		std::cout << "[trigger]\t'" << ev.name << "' at uv (" << hu << ", " << hv << ") / world ("
		          << hp.x << ", " << hp.y << ", " << hp.z << ")"
		          << (bound ? (std::string(" -> mask '") + bound->name + "'") : std::string()) << std::endl;
		pm->TriggerAtHit(ev.name, hp, hu, hv);
	}
	else
	{
		std::cout << "[trigger]\t'" << ev.name << "' at world (" << hp.x << ", " << hp.y << ", "
		          << hp.z << ") (mesh has no uv)" << std::endl;
		pm->TriggerAtWorld(ev.name, hp);
	}
}

// --- Asset editor windows --------------------------------------------------

static const char* kPreviewMeshGuid[] = { "builtin:sphere", "builtin:cube", "builtin:plane" };

static nuke::Atom* FindInSubtree(nuke::Atom* a, long id);

// Per-window undo/redo: prefabs use subtree JSON snapshots, materials use clones.
static const size_t kAeUndoCap = 64;

// Replace the live prefab subtree with a snapshot; atom ids survive, so the selection does.
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
	w.dirty = true;   // editedNow deliberately not raised: this IS undo
}

// --- Animation preview: a per-window mini-PIE ticking the subtree's Animators.

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
		w.animSnap = nuke::SaveAtomToString(w.prefabRoot);   // pose + structure before play
		w.animPlay = true;
	}
	else
	{
		w.animPlay = false;
		if (!w.animSnap.empty())
		{
			const bool wasDirty = w.dirty;   // restoring the pre-play pose is not an edit
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
	w.dirty = true;   // editedNow deliberately not raised: this IS undo/redo
}

// LiveMaterial sections of the .numat editor: condition-state responses, typed hit reactions,
// the surface's sound identity and shape/variation. Returns true when anything changed —
// the caller marks the window dirty and refreshes the preview clone.
bool EditorUI::DrawLiveMaterialSections(nuke::Material* m)
{
	bool ch = false;
	ImGui::Separator();
	ImGui::TextDisabled("LiveMaterial — surface behaviour (all sections optional)");
	// Labels sit to the RIGHT of widgets: reserve room for them or they clip past the window.
	ImGui::PushItemWidth(-150);

	if (ImGui::CollapsingHeader("Condition States"))
	{
		ImGui::TextDisabled("Response to wet/snow/dust/rust/... driven by world, atom or masks.");
		int kill = -1;
		for (int i = 0; i < (int)m->liveStates.size(); ++i)
		{
			nuke::LiveState& s = m->liveStates[i];
			ImGui::PushID(i);
			bool open = ImGui::TreeNode("##state", "%s", s.state.empty() ? "(unnamed state)" : s.state.c_str());
			ImGui::SameLine(ImGui::GetContentRegionAvail().x - 18);
			if (ImGui::SmallButton(ICON_LC_TRASH_2)) kill = i;
			if (open)
			{
				char buf[64]; strncpy(buf, s.state.c_str(), 63); buf[63] = 0;
				if (ImGui::InputTextWithHint("State", "wet / snow / dust / rust / mud ...", buf, sizeof(buf)))
				{ s.state = buf; ch = true; }
				ch |= AssetPicker("Albedo", s.albedoGuid, "texture");
				ch |= AssetPicker("Normal", s.normalGuid, "texture");
				ch |= AssetPicker("Metal-Rough", s.mrGuid, "texture");
				float col[4] = { (float)s.color.r, (float)s.color.g, (float)s.color.b, (float)s.color.a };
				if (ImGui::ColorEdit4("Tint", col))
				{ s.color = nuke::Color(col[0], col[1], col[2], col[3]); ch = true; }
				ch |= ImGui::DragFloat("Metallic", &s.metallic, 0.01f, -1.0f, 1.0f, "%.2f (-1 keep)", ImGuiSliderFlags_AlwaysClamp);
				ch |= ImGui::DragFloat("Roughness", &s.roughness, 0.01f, -1.0f, 1.0f, "%.2f (-1 keep)", ImGuiSliderFlags_AlwaysClamp);
				ch |= ImGui::DragFloat("Threshold", &s.threshold, 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				ch |= ImGui::DragFloat("Feather", &s.feather, 0.01f, 0.01f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				ch |= ImGui::DragFloat("Top Only", &s.topOnly, 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("1 = the state settles on up-facing surfaces only (snow, dust)");
				ch |= ImGui::DragFloat("Displace", &s.displace, 0.005f, 0.0f, 1.0f, "%.3f m", ImGuiSliderFlags_AlwaysClamp);
				ImGui::SeparatorText("Spatial (terrain)");
				ch |= ImGui::DragFloat("Height Min", &s.hMin, 0.5f, -10000.0f, 10000.0f, "%.1f");
				ch |= ImGui::DragFloat("Height Max", &s.hMax, 0.5f, -10000.0f, 10000.0f, "%.1f");
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("World-Y band the state lives in (snowline). Max <= Min = everywhere");
				ch |= ImGui::DragFloat("Height Feather", &s.hFeather, 0.25f, 0.1f, 200.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
				ch |= ImGui::DragFloat("Windward", &s.windward, 0.01f, -2.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Wind-facing bias: >0 = leeward slopes first (snow/dust drifts), <0 = windward, 0 = off");
				{
					char cbuf[64]; strncpy(cbuf, s.couple.c_str(), 63); cbuf[63] = 0;
					if (ImGui::InputTextWithHint("Couple To", "wet (mud grows where wet)", cbuf, sizeof(cbuf)))
					{ s.couple = cbuf; ch = true; }
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("This state's weight rides ANOTHER state's local weight (mud couples to wet)");
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		if (kill >= 0) { m->liveStates.erase(m->liveStates.begin() + kill); ch = true; }
		if (ImGui::Button(ICON_LC_PLUS " Add State", ImVec2(-1, 0))) { m->liveStates.push_back({}); ch = true; }
	}

	if (ImGui::CollapsingHeader("Static Layers"))
	{
		ImGui::TextDisabled("Always-on overlays blended through a mask map (rust, moss, grime).");
		int kill = -1;
		for (int i = 0; i < (int)m->liveLayers.size(); ++i)
		{
			nuke::LiveLayer& ly = m->liveLayers[i];
			ImGui::PushID(6000 + i);
			bool open = ImGui::TreeNode("##layer", "Layer %d", i);
			ImGui::SameLine(ImGui::GetContentRegionAvail().x - 18);
			if (ImGui::SmallButton(ICON_LC_TRASH_2)) kill = i;
			if (open)
			{
				ch |= AssetPicker("Albedo", ly.albedoGuid, "texture");
				ch |= AssetPicker("Normal", ly.normalGuid, "texture");
				ch |= AssetPicker("Metal-Rough", ly.mrGuid, "texture");
				ch |= AssetPicker("Mask", ly.maskGuid, "texture");
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Grayscale blend mask in the material's UV space (empty = uniform)");
				float col[4] = { (float)ly.color.r, (float)ly.color.g, (float)ly.color.b, (float)ly.color.a };
				if (ImGui::ColorEdit4("Tint", col))
				{ ly.color = nuke::Color(col[0], col[1], col[2], col[3]); ch = true; }
				ch |= ImGui::DragFloat("Metallic", &ly.metallic, 0.01f, -1.0f, 1.0f, "%.2f (-1 keep)", ImGuiSliderFlags_AlwaysClamp);
				ch |= ImGui::DragFloat("Roughness", &ly.roughness, 0.01f, -1.0f, 1.0f, "%.2f (-1 keep)", ImGuiSliderFlags_AlwaysClamp);
				ch |= ImGui::DragFloat("Weight", &ly.value, 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				ch |= ImGui::DragFloat("Threshold", &ly.threshold, 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				ch |= ImGui::DragFloat("Feather", &ly.feather, 0.01f, 0.01f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				ch |= ImGui::DragFloat("Top Only", &ly.topOnly, 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		if (kill >= 0) { m->liveLayers.erase(m->liveLayers.begin() + kill); ch = true; }
		if (ImGui::Button(ICON_LC_PLUS " Add Layer", ImVec2(-1, 0))) { m->liveLayers.push_back({}); ch = true; }
		if ((int)m->liveLayers.size() + (int)m->liveStates.size() > nuke::Material::kOverlaySlots)
			ImGui::TextDisabled("%d GPU slots render at once (layers first, then ACTIVE states; dormant states take none).",
			                    nuke::Material::kOverlaySlots);
	}

	if (ImGui::CollapsingHeader("Hit Reactions"))
	{
		ImGui::TextDisabled("Typed hits spawn the surface's own particles/decals/sounds.");
		int kill = -1;
		for (int i = 0; i < (int)m->liveHits.size(); ++i)
		{
			nuke::LiveHit& h = m->liveHits[i];
			ImGui::PushID(1000 + i);
			bool open = ImGui::TreeNode("##hit", "%s", h.hitType.empty() ? "(any hit)" : h.hitType.c_str());
			ImGui::SameLine(ImGui::GetContentRegionAvail().x - 18);
			if (ImGui::SmallButton(ICON_LC_TRASH_2)) kill = i;
			if (open)
			{
				char buf[64]; strncpy(buf, h.hitType.c_str(), 63); buf[63] = 0;
				if (ImGui::InputTextWithHint("Hit Type", "bullet / blunt / slash / explosion ... (empty = any)", buf, sizeof(buf)))
				{ h.hitType = buf; ch = true; }
				ch |= AssetPicker("Spawn Prefab", h.prefabGuid, "file:.nuprefab");
				ch |= AssetPicker("Decal", h.decalGuid, "texture");
				ch |= AssetPicker("Sound", h.soundGuid, "audio");
				ch |= ImGui::DragFloat("Min Impulse", &h.minImpulse, 0.1f, 0.0f, 10000.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scripted hits gate by their impulse; automatic contact hits gate by closing speed (m/s)");
				ch |= ImGui::DragFloat("Lifetime", &h.lifetime, 0.1f, 0.0f, 120.0f, "%.1f s (0 keep)", ImGuiSliderFlags_AlwaysClamp);
				ch |= ImGui::DragFloat("Decal Size", &h.decalSize, 0.05f, 0.05f, 8.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
				static const char* kDMode[] = { "Albedo (on top)", "Light Projector", "Stain (lit)" };
				if (h.decalMode < 0 || h.decalMode > 2) h.decalMode = 2;
				if (ImGui::BeginCombo("Decal Mode", kDMode[h.decalMode]))
				{
					for (int dm = 0; dm < 3; ++dm)
						if (ImGui::Selectable(kDMode[dm], dm == h.decalMode)) { h.decalMode = dm; ch = true; }
					ImGui::EndCombo();
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Albedo: texture as-is on top (bright decals stay bright);\nLight Projector: additive glow;\nStain: tints the lit surface — shadows show through (holes, scorch, dirt)");
				{
					float dt4[4] = { (float)h.decalTint.r, (float)h.decalTint.g, (float)h.decalTint.b, (float)h.decalTint.a };
					if (ImGui::ColorEdit4("Decal Tint", dt4))
					{ h.decalTint = nuke::Color(dt4[0], dt4[1], dt4[2], dt4[3]); ch = true; }
				}
				ch |= ImGui::DragFloat("Decal Intensity", &h.decalIntensity, 0.02f, 0.0f, 8.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				ch |= ImGui::DragFloat("Decal Fade", &h.decalFade, 0.02f, 0.0f, 5.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("The decal SPREADS in over this time (dense core first, thin edges last — blood creep).\n0 = instant stamp. Timed decals also dissolve before their Lifetime ends.");
				// The hit can fire a material event AT the hit point: masked reactions/ripples
				// run from real gameplay hits; impulse+normal land in g_Hit for shaders.
				if (ImGui::BeginCombo("Event", h.eventName.empty() ? "(none)" : h.eventName.c_str()))
				{
					if (ImGui::Selectable("(none)", h.eventName.empty())) { h.eventName.clear(); ch = true; }
					for (const nuke::LiveEvent& hev : m->liveEvents)
						if (ImGui::Selectable(hev.name.c_str(), h.eventName == hev.name)) { h.eventName = hev.name; ch = true; }
					ImGui::EndCombo();
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		if (kill >= 0) { m->liveHits.erase(m->liveHits.begin() + kill); ch = true; }
		if (ImGui::Button(ICON_LC_PLUS " Add Reaction", ImVec2(-1, 0))) { m->liveHits.push_back({}); ch = true; }
	}

	if (ImGui::CollapsingHeader("Foliage"))
	{
		ImGui::TextDisabled("Grown automatically on every surface using this material.");
		int kill = -1;
		for (int i = 0; i < (int)m->liveFoliage.size(); ++i)
		{
			nuke::LiveFoliage& f = m->liveFoliage[i];
			ImGui::PushID(3000 + i);
			bool open = ImGui::TreeNode("##fol", "Layer %d", i);
			ImGui::SameLine(ImGui::GetContentRegionAvail().x - 18);
			if (ImGui::SmallButton(ICON_LC_TRASH_2)) kill = i;
			if (open)
			{
				ch |= AssetPicker("Mesh", f.meshGuid, "mesh");
				ch |= AssetPicker("Material", f.matGuid, "material");
				ch |= ImGui::DragFloat("Density", &f.density, 0.05f, 0.01f, 64.0f, "%.2f /m2", ImGuiSliderFlags_AlwaysClamp);
				ch |= ImGui::DragFloat("Scale Min", &f.scaleMin, 0.01f, 0.01f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				ch |= ImGui::DragFloat("Scale Max", &f.scaleMax, 0.01f, 0.01f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				ch |= ImGui::DragFloat("Max Slope", &f.maxSlope, 0.5f, 0.0f, 90.0f, "%.0f deg", ImGuiSliderFlags_AlwaysClamp);
				ch |= ImGui::DragFloat("Align To Normal", &f.align, 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				ch |= ImGui::DragFloat("Wind Bend", &f.windBend, 0.01f, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				ch |= ImGui::DragFloat("Interaction Bend", &f.interBend, 0.01f, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				ch |= ImGui::DragInt("Seed", &f.seed, 1.0f, 0, 1000000, "%d", ImGuiSliderFlags_AlwaysClamp);
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		if (kill >= 0) { m->liveFoliage.erase(m->liveFoliage.begin() + kill); ch = true; }
		if (ImGui::Button(ICON_LC_PLUS " Add Foliage Layer", ImVec2(-1, 0))) { m->liveFoliage.push_back({}); ch = true; }
	}

	if (ImGui::CollapsingHeader("Tweens"))
	{
		ImGui::TextDisabled("Parameter animations from game time (stateless): value curves / gradient on one 0..1 timeline (x Duration).");
		int kill = -1;
		for (int i = 0; i < (int)m->liveTweens.size(); ++i)
		{
			nuke::LiveTween& tw = m->liveTweens[i];
			ImGui::PushID(4000 + i);
			bool open = ImGui::TreeNode("##tw", "%s", tw.param.empty() ? "(no target)" : tw.param.c_str());
			ImGui::SameLine(ImGui::GetContentRegionAvail().x - 18);
			if (ImGui::SmallButton(ICON_LC_TRASH_2)) kill = i;
			if (open)
			{
				{
					char nbuf[64]; strncpy(nbuf, tw.name.c_str(), 63); nbuf[63] = 0;
					if (ImGui::InputTextWithHint("Name", "(events start tweens by name)", nbuf, sizeof(nbuf)))
					{ tw.name = nbuf; ch = true; }
				}
				MatParamPick twPick;
				ch |= MaterialParamCombo(m, "Param", tw.param, twPick);
				char pbuf[64]; strncpy(pbuf, tw.param.c_str(), 63); pbuf[63] = 0;
				if (ImGui::InputTextWithHint("Custom", "or a shader MatCB prop (g_...)", pbuf, sizeof(pbuf)))
				{ tw.param = pbuf; ch = true; }
				ch |= ImGui::DragFloat("Duration", &tw.duration, 0.01f, 0.01f, 600.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
				static const char* kLoop[] = { "Once", "Loop", "Ping-Pong" };
				if (tw.loop < 0 || tw.loop > 2) tw.loop = 1;
				if (ImGui::BeginCombo("Loop", kLoop[tw.loop]))
				{
					for (int l = 0; l < 3; ++l)
						if (ImGui::Selectable(kLoop[l], l == tw.loop)) { tw.loop = l; ch = true; }
					ImGui::EndCombo();
				}
				static const char* kRun[] = { "Auto", "On Event" };
				if (tw.runMode < 0 || tw.runMode > 1) tw.runMode = 0;
				if (ImGui::BeginCombo("Run", kRun[tw.runMode]))
				{
					for (int rr = 0; rr < 2; ++rr)
						if (ImGui::Selectable(kRun[rr], rr == tw.runMode)) { tw.runMode = rr; ch = true; }
					ImGui::EndCombo();
				}
				if (!m->liveMasks.empty())   // spatial mask modulating this tween's effect
					if (ImGui::BeginCombo("Mask", tw.mask.empty() ? "(whole surface)" : tw.mask.c_str()))
					{
						if (ImGui::Selectable("(whole surface)", tw.mask.empty())) { tw.mask.clear(); ch = true; }
						for (const nuke::LiveMask& mk : m->liveMasks)
							if (ImGui::Selectable(mk.name.c_str(), tw.mask == mk.name)) { tw.mask = mk.name; ch = true; }
						ImGui::EndCombo();
					}
				const bool twCol = twPick.color;
				int twDim = twPick.dim;
				// triggers ride the KEYFRAMES: select a key/stop and pick the event it fires
				std::vector<std::string> evNames;
				for (const nuke::LiveEvent& ev : m->liveEvents)
					if (!ev.name.empty()) evNames.push_back(ev.name);
				if (twCol)
				{
					for (auto& cc : tw.chan) if (!cc.empty()) { cc.clear(); ch = true; }
					if (tw.grad.empty()) { tw.grad = { 0, 0, 0, 0, 1,  1, 1, 1, 1, 1 }; ch = true; }
					ch |= GradientStopsEditor(tw.grad, true, &tw.trigT, &tw.trigEvent, &evNames);
				}
				else
				{
					if (!tw.grad.empty()) { tw.grad.clear(); ch = true; }
					int curCh = 0;
					if (twDim > 1)
					{
						static const char* kChan[4] = { "X", "Y", "Z", "W" };
						ImGuiStorage* stg = ImGui::GetStateStorage();
						const ImGuiID chId = ImGui::GetID("##twchan");
						curCh = stg->GetInt(chId, 0); if (curCh >= twDim) curCh = 0;
						for (int c = 0; c < twDim; ++c)
						{
							if (c) ImGui::SameLine();
							if (ImGui::RadioButton(kChan[c], curCh == c)) { curCh = c; stg->SetInt(chId, c); }
						}
					}
					if (tw.chan[curCh].empty()) { tw.chan[curCh] = { 0, 0, 1, 1,  1, 1, 1, 1 }; ch = true; }
					ch |= CurveKeysEditor(tw.chan[curCh], -1e30f, 1e30f, nullptr, &tw.trigT, &tw.trigEvent, &evNames);
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		if (kill >= 0) { m->liveTweens.erase(m->liveTweens.begin() + kill); ch = true; }
		if (ImGui::Button(ICON_LC_PLUS " Add Tween", ImVec2(-1, 0))) { m->liveTweens.push_back({}); ch = true; }
		ImGui::TextDisabled("\"uv\" scroll + \"wipe\" dissolve draw with the LM render pass (LM-3).");
	}

	if (ImGui::CollapsingHeader("Masks"))
	{
		ImGui::TextDisabled("Spatial shapes that localize tween effects; point events move their centers.");
		int killM = -1;
		for (int i = 0; i < (int)m->liveMasks.size(); ++i)
		{
			nuke::LiveMask& mk = m->liveMasks[i];
			ImGui::PushID(5200 + i);
			bool open = ImGui::TreeNode("##mk", "%s", mk.name.empty() ? "(unnamed)" : mk.name.c_str());
			ImGui::SameLine(ImGui::GetContentRegionAvail().x - 18);
			if (ImGui::SmallButton(ICON_LC_TRASH_2)) killM = i;
			if (open)
			{
				char nbuf[64]; strncpy(nbuf, mk.name.c_str(), 63); nbuf[63] = 0;
				if (ImGui::InputText("Name", nbuf, sizeof(nbuf))) { mk.name = nbuf; ch = true; }
				static const char* kSpace[] = { "UV", "World" };
				if (ImGui::BeginCombo("Space", kSpace[mk.space & 1]))
				{
					for (int q = 0; q < 2; ++q)
						if (ImGui::Selectable(kSpace[q], q == mk.space)) { mk.space = q; ch = true; }
					ImGui::EndCombo();
				}
				static const char* kShape[] = { "Circle", "Ring", "Stamp" };
				if (ImGui::BeginCombo("Shape", kShape[mk.shape % 3]))
				{
					for (int q = 0; q < 3; ++q)
						if (ImGui::Selectable(kShape[q], q == mk.shape)) { mk.shape = q; ch = true; }
					ImGui::EndCombo();
				}
				if (mk.shape == 2) ch |= AssetPicker("Stamp", mk.stampGuid, "texture");
				float c3[3] = { mk.cx, mk.cy, mk.cz };
				if (mk.space == 0 ? ImGui::DragFloat2("Center", c3, 0.005f) : ImGui::DragFloat3("Center", c3, 0.01f))
				{ mk.cx = c3[0]; mk.cy = c3[1]; mk.cz = c3[2]; ch = true; }
				ch |= ImGui::DragFloat("Scale", &mk.scale, 0.005f, 0.0f, 1e6f, mk.space ? "%.3f m" : "%.3f uv");
				ch |= ImGui::DragFloat("Repeat", &mk.repeat, 0.05f, 0.0f, 64.0f, "%.1f");
				ch |= ImGui::DragFloat("Rotation", &mk.rotation, 0.5f, -360.0f, 360.0f, "%.0f deg");
				ch |= ImGui::DragFloat("Fade", &mk.fade, 0.01f, 0.0f, 1.0f, "%.2f");
				ch |= ImGui::DragFloat("Softness", &mk.softness, 0.01f, 0.0f, 1.0f, "%.2f");
				ch |= ImGui::DragFloat("Strength", &mk.strength, 0.01f, 0.0f, 1.0f, "%.2f");
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		if (killM >= 0) { m->liveMasks.erase(m->liveMasks.begin() + killM); ch = true; }
		if (ImGui::Button(ICON_LC_PLUS " Add Mask", ImVec2(-1, 0)))
		{
			nuke::LiveMask mk;
			mk.name = "mask" + std::to_string((int)m->liveMasks.size());
			m->liveMasks.push_back(mk); ch = true;
		}
	}

	if (ImGui::CollapsingHeader("Events"))
	{
		ImGui::TextDisabled("Named reactions the triggers fire (tween marks, gameplay, the Trigger Tool).");
		int killE = -1;
		for (int i = 0; i < (int)m->liveEvents.size(); ++i)
		{
			nuke::LiveEvent& ev = m->liveEvents[i];
			ImGui::PushID(5400 + i);
			bool open = ImGui::TreeNode("##ev", "%s", ev.name.empty() ? "(unnamed)" : ev.name.c_str());
			ImGui::SameLine(ImGui::GetContentRegionAvail().x - 18);
			if (ImGui::SmallButton(ICON_LC_TRASH_2)) killE = i;
			if (open)
			{
				char nbuf[64]; strncpy(nbuf, ev.name.c_str(), 63); nbuf[63] = 0;
				if (ImGui::InputText("Name", nbuf, sizeof(nbuf))) { ev.name = nbuf; ch = true; }
				ImGui::TextDisabled("Start Tweens");
				int killS = -1;
				for (int si = 0; si < (int)ev.startTweens.size(); ++si)
				{
					ImGui::PushID(100 + si);
					ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - 30.0f));
					if (ImGui::BeginCombo("##st", ev.startTweens[si].empty() ? "(tween)" : ev.startTweens[si].c_str()))
					{
						for (const nuke::LiveTween& tt : m->liveTweens)
						{
							const std::string& nm = tt.name.empty() ? tt.param : tt.name;
							if (nm.empty()) continue;
							if (ImGui::Selectable(nm.c_str(), ev.startTweens[si] == nm)) { ev.startTweens[si] = nm; ch = true; }
						}
						ImGui::EndCombo();
					}
					ImGui::SameLine();
					if (ImGui::SmallButton(ICON_LC_TRASH_2)) killS = si;
					ImGui::PopID();
				}
				if (killS >= 0) { ev.startTweens.erase(ev.startTweens.begin() + killS); ch = true; }
				if (ImGui::SmallButton(ICON_LC_PLUS " Add Tween Start")) { ev.startTweens.push_back(""); ch = true; }
				ImGui::TextDisabled("Set Params (instant)");
				int killP = -1;
				for (int si = 0; si < (int)ev.setParams.size(); ++si)
				{
					ImGui::PushID(200 + si);
					// target picked the same way tweens pick theirs; value widget follows
					// the target's dimension (one field for scalars, a picker for colors)
					MatParamPick pk;
					ImGui::SetNextItemWidth(std::max(140.0f, ImGui::GetContentRegionAvail().x * 0.42f));
					ch |= MaterialParamCombo(m, "##sp", ev.setParams[si], pk);
					if ((int)ev.setValues.size() < (si + 1) * 4) ev.setValues.resize((si + 1) * 4, 0.0f);
					float* spv = &ev.setValues[si * 4];
					ImGui::SameLine();
					const float trashW = ImGui::CalcTextSize(ICON_LC_TRASH_2).x
					                   + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
					ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - trashW));
					if      (pk.color)    ch |= ImGui::ColorEdit4("##sv", spv);
					else if (pk.dim == 1) ch |= ImGui::DragFloat("##sv", spv, 0.01f);
					else if (pk.dim == 2) ch |= ImGui::DragFloat2("##sv", spv, 0.01f);
					else if (pk.dim == 3) ch |= ImGui::DragFloat3("##sv", spv, 0.01f);
					else                  ch |= ImGui::DragFloat4("##sv", spv, 0.01f);
					ImGui::SameLine();
					if (ImGui::SmallButton(ICON_LC_TRASH_2)) killP = si;
					ImGui::PopID();
				}
				if (killP >= 0)
				{
					ev.setParams.erase(ev.setParams.begin() + killP);
					if ((int)ev.setValues.size() >= (killP + 1) * 4)
						ev.setValues.erase(ev.setValues.begin() + killP * 4, ev.setValues.begin() + killP * 4 + 4);
					ch = true;
				}
				if (ImGui::SmallButton(ICON_LC_PLUS " Add Set Param"))
				{ ev.setParams.push_back(""); ev.setValues.resize(ev.setParams.size() * 4, 0.0f); ch = true; }
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		if (killE >= 0) { m->liveEvents.erase(m->liveEvents.begin() + killE); ch = true; }
		if (ImGui::Button(ICON_LC_PLUS " Add Event", ImVec2(-1, 0)))
		{
			nuke::LiveEvent ev;
			ev.name = "event" + std::to_string((int)m->liveEvents.size());
			m->liveEvents.push_back(ev); ch = true;
		}
	}

	if (ImGui::CollapsingHeader("Sound"))
	{
		ImGui::TextDisabled("The surface's sound identity: footsteps, ambient, wind.");
		int kill = -1;
		for (int i = 0; i < (int)m->liveSound.footsteps.size(); ++i)
		{
			ImGui::PushID(2000 + i);
			// Shrink the picker so the trash button fits on the row (icon width + padding).
			ImGui::SetNextItemWidth(std::max(80.0f, ImGui::CalcItemWidth()
				- ImGui::CalcTextSize(ICON_LC_TRASH_2).x - ImGui::GetStyle().FramePadding.x * 2.0f
				- ImGui::GetStyle().ItemSpacing.x));
			ch |= AssetPicker(("Step " + std::to_string(i)).c_str(), m->liveSound.footsteps[i], "audio");
			ImGui::SameLine();
			if (ImGui::SmallButton(ICON_LC_TRASH_2)) kill = i;
			ImGui::PopID();
		}
		if (kill >= 0) { m->liveSound.footsteps.erase(m->liveSound.footsteps.begin() + kill); ch = true; }
		if (ImGui::Button(ICON_LC_PLUS " Add Footstep", ImVec2(-1, 0))) { m->liveSound.footsteps.push_back(""); ch = true; }
		ch |= ImGui::DragFloat("Step Volume", &m->liveSound.footVolume, 0.01f, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ch |= AssetPicker("Ambient Loop", m->liveSound.ambientGuid, "audio");
		ch |= ImGui::DragFloat("Ambient Volume", &m->liveSound.ambientVolume, 0.01f, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ch |= AssetPicker("Wind Loop", m->liveSound.windGuid, "audio");
		ch |= ImGui::DragFloat("Wind Volume", &m->liveSound.windVolume, 0.01f, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	}

	if (ImGui::CollapsingHeader("Surface Shape"))
	{
		ImGui::TextDisabled("Displacement height + anti-tiling variation.");
		ch |= AssetPicker("Height Map", m->liveSurface.heightGuid, "texture");
		ch |= ImGui::DragFloat("Parallax", &m->liveSurface.parallax, 0.002f, 0.0f, 0.3f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Parallax occlusion depth (UV space); 0 = off, typical 0.02-0.1");
		ch |= ImGui::DragFloat("Disp Scale", &m->liveSurface.dispScale, 0.005f, 0.0f, 2.0f, "%.3f m", ImGuiSliderFlags_AlwaysClamp);
		ch |= ImGui::DragFloat("Disp Mid", &m->liveSurface.dispMid, 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ch |= ImGui::DragFloat("Variation", &m->liveSurface.varAmount, 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Anti-tiling: per-cell UV jitter/rotation strength");
		ch |= ImGui::DragFloat("Var Cell", &m->liveSurface.varScale, 0.1f, 0.5f, 64.0f, "%.1f repeats", ImGuiSliderFlags_AlwaysClamp);
		ch |= ImGui::DragFloat("Var Hue", &m->liveSurface.varHue, 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	}

	ImGui::PopItemWidth();
	if (ch)
	{
		m->Resolve();          // rebind state/height textures after guid edits
		m->PushRenderProps();  // g_Disp/g_Var/g_UVT track the inspector live in the preview
	}
	return ch;
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
	else if (w.ext == ".nuseq" && w.seq && !w.undoQ.empty())
	{
		w.redoQ.push_back(w.seq->ToString());
		if (nuke::Sequence* r = nuke::Sequence::FromString(w.undoQ.back()))
		{
			delete w.seq;
			w.seq = r;
			if (w.seqPv) w.seqPv->SetSequence(r);
		}
		w.undoQ.pop_back();
		w.idleQ = w.seq->ToString(); w.editing = false; w.dirty = true;
	}
	else if (w.ext == ".nuanim" && w.anim && !w.undoAn.empty())
	{
		w.redoAn.push_back(EditorAnimMetaJson(w.anim));
		EditorAnimMetaLoad(w.anim, w.undoAn.back()); w.undoAn.pop_back();
		w.idleAn = EditorAnimMetaJson(w.anim); w.editing = false; w.dirty = true;
	}
	else if (w.ext == ".nusm" && w.sm && !w.undoSm.empty())
	{
		w.redoSm.push_back(w.sm->ToString());
		if (nuke::AnimSM* r = nuke::AnimSM::FromString(w.undoSm.back())) { delete w.sm; w.sm = r; }
		w.undoSm.pop_back();
		w.idleSm = w.sm->ToString(); w.editing = false; w.dirty = true;
	}
	else if (w.ext == ".nublend" && w.blend && !w.undoB.empty())
	{
		w.redoB.push_back(w.blend->ToString());
		if (nuke::BlendSpace* r = nuke::BlendSpace::FromString(w.undoB.back())) { delete w.blend; w.blend = r; }
		w.undoB.pop_back();
		w.idleB = w.blend->ToString(); w.editing = false; w.dirty = true;
	}
	else if (w.ext == ".nuskel" && w.skel && !w.undoSk.empty())
	{
		w.redoSk.push_back(w.skel->ToString());
		if (nuke::Skeleton* r = nuke::Skeleton::FromString(w.undoSk.back())) { delete w.skel; w.skel = r; }
		w.undoSk.pop_back();
		w.idleSk = w.skel->ToString(); w.editing = false; w.dirty = true;
	}
	else if (w.ext == ".nurag" && w.rag && !w.undoRg.empty())
	{
		w.redoRg.push_back(w.rag->ToString());
		if (nuke::RagdollDef* r = nuke::RagdollDef::FromString(w.undoRg.back())) { delete w.rag; w.rag = r; }
		w.undoRg.pop_back();
		w.idleRg = w.rag->ToString(); w.editing = false; w.dirty = true;
	}
	else if (w.ext == ".nubonemap" && w.bmap && !w.undoBm.empty())
	{
		w.redoBm.push_back(EditorBoneMapJson(w.bmap));
		EditorBoneMapLoad(w.bmap, w.undoBm.back()); w.undoBm.pop_back();
		w.idleBm = EditorBoneMapJson(w.bmap); w.editing = false; w.dirty = true;
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
	else if (w.ext == ".nuseq" && w.seq && !w.redoQ.empty())
	{
		w.undoQ.push_back(w.seq->ToString());
		if (nuke::Sequence* r = nuke::Sequence::FromString(w.redoQ.back()))
		{
			delete w.seq;
			w.seq = r;
			if (w.seqPv) w.seqPv->SetSequence(r);
		}
		w.redoQ.pop_back();
		w.idleQ = w.seq->ToString(); w.editing = false; w.dirty = true;
	}
	else if (w.ext == ".nuanim" && w.anim && !w.redoAn.empty())
	{
		w.undoAn.push_back(EditorAnimMetaJson(w.anim));
		EditorAnimMetaLoad(w.anim, w.redoAn.back()); w.redoAn.pop_back();
		w.idleAn = EditorAnimMetaJson(w.anim); w.editing = false; w.dirty = true;
	}
	else if (w.ext == ".nusm" && w.sm && !w.redoSm.empty())
	{
		w.undoSm.push_back(w.sm->ToString());
		if (nuke::AnimSM* r = nuke::AnimSM::FromString(w.redoSm.back())) { delete w.sm; w.sm = r; }
		w.redoSm.pop_back();
		w.idleSm = w.sm->ToString(); w.editing = false; w.dirty = true;
	}
	else if (w.ext == ".nublend" && w.blend && !w.redoB.empty())
	{
		w.undoB.push_back(w.blend->ToString());
		if (nuke::BlendSpace* r = nuke::BlendSpace::FromString(w.redoB.back())) { delete w.blend; w.blend = r; }
		w.redoB.pop_back();
		w.idleB = w.blend->ToString(); w.editing = false; w.dirty = true;
	}
	else if (w.ext == ".nuskel" && w.skel && !w.redoSk.empty())
	{
		w.undoSk.push_back(w.skel->ToString());
		if (nuke::Skeleton* r = nuke::Skeleton::FromString(w.redoSk.back())) { delete w.skel; w.skel = r; }
		w.redoSk.pop_back();
		w.idleSk = w.skel->ToString(); w.editing = false; w.dirty = true;
	}
	else if (w.ext == ".nurag" && w.rag && !w.redoRg.empty())
	{
		w.undoRg.push_back(w.rag->ToString());
		if (nuke::RagdollDef* r = nuke::RagdollDef::FromString(w.redoRg.back())) { delete w.rag; w.rag = r; }
		w.redoRg.pop_back();
		w.idleRg = w.rag->ToString(); w.editing = false; w.dirty = true;
	}
	else if (w.ext == ".nubonemap" && w.bmap && !w.redoBm.empty())
	{
		w.undoBm.push_back(EditorBoneMapJson(w.bmap));
		EditorBoneMapLoad(w.bmap, w.redoBm.back()); w.redoBm.pop_back();
		w.idleBm = EditorBoneMapJson(w.bmap); w.editing = false; w.dirty = true;
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

// Sprite-slice metadata <-> snapshot struct (the one place that lists the fields).
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
// Mirror edited slice metadata onto the live ResDB texture so sprites re-slice immediately.
static void SlicerApplyLive(nuke::Texture* t)
{
	if (!t || t->guid.empty()) return;
	if (nuke::Texture* live = nuke::ResDB::getSingleton()->GetTexture(t->guid))
		if (live != t) ApplyMeta(live, SnapMeta(t));
}

// Call after committing a slice change: records the pre-change baseline, then re-baselines.
void EditorUI::SlicerPushUndo(AssetEditorWin& w)
{
	if (!w.tex) return;
	w.undoS.push_back(w.idleS);
	w.redoS.clear();
	w.idleS = SnapMeta(w.tex);
	w.dirty = true;
	SlicerApplyLive(w.tex);
}

// Open (or focus) the editor window for an asset file, dispatching on its extension.
void EditorUI::OpenAssetEditor(const std::string& path)
{
	for (AssetEditorWin& w : assetEds)
		if (w.path == path) { w.wantFocus = true; return; }

	std::string ext = bfs::path(path).extension().string();
	for (char& c : ext) c = (char)std::tolower((unsigned char)c);
	const bool isAudio = (ext == ".ogg" || ext == ".wav" || ext == ".mp3" || ext == ".flac");
	// Module-supplied editors win: a module that registers a file type also registers its editor.
	if (const auto* open = nuke::AssetEditorForExt(ext)) { (*open)(path); return; }
	if (ext != ".numat" && ext != ".numesh" && ext != ".nuprefab" && ext != ".nutex" && ext != ".nuinput"
	    && ext != ".nuseq" && ext != ".nuanim" && ext != ".nusm" && ext != ".nublend"
	    && ext != ".nuskel" && ext != ".nurag" && ext != ".nubonemap" && !isAudio) return;

	if (ext == ".nuskel")
	{
		AssetEditorWin w;
		w.path = path; w.ext = ext; w.wantFocus = true; w.detached = detachAssetEditors;
		w.skel = nuke::Skeleton::LoadFromFile(path);
		if (!w.skel) return;
		w.pv = AcquirePreview();
		if (w.pv) w.pv->mr->mesh = nullptr;
		w.idleSk = w.skel->ToString();
		assetEds.push_back(std::move(w));
		return;
	}
	if (ext == ".nurag")
	{
		AssetEditorWin w;
		w.path = path; w.ext = ext; w.wantFocus = true; w.detached = detachAssetEditors;
		w.rag = nuke::RagdollDef::LoadFromFile(path);
		if (!w.rag) return;
		w.pv = AcquirePreview();
		if (w.pv) w.pv->mr->mesh = nullptr;
		w.idleRg = w.rag->ToString();
		assetEds.push_back(std::move(w));
		return;
	}
	if (ext == ".nubonemap")   // pure name pairs: no 3D scene
	{
		AssetEditorWin w;
		w.path = path; w.ext = ext; w.wantFocus = true; w.detached = detachAssetEditors;
		w.bmap = nuke::BoneMap::LoadFromFile(path);
		if (!w.bmap) return;
		w.idleBm = EditorBoneMapJson(w.bmap);
		assetEds.push_back(std::move(w));
		return;
	}

	// stage-10 animation editors: an owned editing copy + a pooled preview world for the rig
	if (ext == ".nuanim")
	{
		AssetEditorWin w;
		w.path = path; w.ext = ext; w.wantFocus = true; w.detached = detachAssetEditors;
		w.anim = nuke::AnimClip::LoadFromFile(path);
		if (!w.anim) return;
		w.pv = AcquirePreview();
		if (w.pv) w.pv->mr->mesh = nullptr;
		w.idleAn = EditorAnimMetaJson(w.anim);   // undo baseline (metadata JSON)
		assetEds.push_back(std::move(w));
		return;
	}
	if (ext == ".nusm")
	{
		AssetEditorWin w;
		w.path = path; w.ext = ext; w.wantFocus = true; w.detached = detachAssetEditors;
		w.sm = nuke::AnimSM::LoadFromFile(path);
		if (!w.sm) return;
		w.pv = AcquirePreview();
		if (w.pv) w.pv->mr->mesh = nullptr;
		w.idleSm = w.sm->ToString();
		assetEds.push_back(std::move(w));
		return;
	}
	if (ext == ".nublend")
	{
		AssetEditorWin w;
		w.path = path; w.ext = ext; w.wantFocus = true; w.detached = detachAssetEditors;
		w.blend = nuke::BlendSpace::LoadFromFile(path);
		if (!w.blend) return;
		w.pv = AcquirePreview();
		if (w.pv) w.pv->mr->mesh = nullptr;
		w.idleB = w.blend->ToString();
		assetEds.push_back(std::move(w));
		return;
	}

	if (ext == ".nuseq")   // sequencer: timeline over the LIVE world, no preview scene
	{
		AssetEditorWin w;
		w.path = path; w.ext = ext; w.wantFocus = true; w.detached = detachAssetEditors;
		w.seq = nuke::Sequence::LoadFromFile(path);
		if (!w.seq) return;
		w.idleQ = w.seq->ToString();   // undo baseline
		assetEds.push_back(std::move(w));
		return;
	}

	if (ext == ".nuinput")   // input map: pure data CRUD, no 3D scene
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

	if (isAudio)   // audio preview: file path + a Preview-bus voice
	{
		AssetEditorWin w;
		w.path = path; w.ext = ext; w.wantFocus = true; w.detached = detachAssetEditors;
		assetEds.push_back(std::move(w));
		return;
	}

	if (ext == ".nutex")   // Sprite Slicer: 2D, no preview scene
	{
		AssetEditorWin w;
		w.path = path; w.ext = ext; w.wantFocus = true; w.detached = detachAssetEditors;
		w.tex = nuke::Texture::LoadFromFile(path);
		if (!w.tex) return;
		if (w.tex->usage != nuke::Texture::UsageSprite) w.tex->usage = nuke::Texture::UsageSprite;  // slicing implies sprite usage
		w.texPreview = UploadTexPreview(w.tex, 2048, w.texPrevW, w.texPrevH);
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
		w.pv->orbit = true;   // the camera circles the sample, it never flies off it
		w.pv->world->editorGrid = false;   // clean sky backdrop — no grid through the sample
		w.pv->mr->meshGuid = kPreviewMeshGuid[0];
		w.pv->mr->mesh = db->GetMesh(kPreviewMeshGuid[0]);
		w.pv->mr->matGuid.clear();                 // preview draws our editing copy
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
		w.pv->mr->mesh = nullptr;                  // skeleton mesh atom stays empty
		w.pv->world->Add(w.prefabRoot);
		w.prefabSelId = (long)w.prefabRoot->id.id;
		w.idleP = nuke::SaveAtomToString(w.prefabRoot);   // undo baseline
		FramePreview(*w.pv, w.prefabRoot);
	}
	assetEds.push_back(std::move(w));
}

// Dashed line for the slicer grid overlay.
static void DashLine(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float th, float dash, float gap)
{
	float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
	if (len < 0.001f) return;
	float step = dash + gap;
	// Too many segments would push the draw list past 65535 verts and overflow ImGui's 16-bit indices.
	if (len / step > 4096.0f) { dl->AddLine(a, b, col, th); return; }
	float ux = dx / len, uy = dy / len;
	for (float d = 0; d < len; d += step)
	{
		float e = d + dash; if (e > len) e = len;
		dl->AddLine(ImVec2(a.x + ux * d, a.y + uy * d), ImVec2(a.x + ux * e, a.y + uy * e), col, th);
	}
}
// Dashed AABB rect with each edge clipped to [cmin,cmax], bounding segment count by the canvas, not the zoom.
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

// Sprite Slicer body: properties on the left, ruler + sheet canvas in the centre, cell preview on the right.
// Cell geometry comes from Texture::SpriteCellRect, shared with the runtime SpriteAnimator.
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
	const bool nineMode = (w.slMode == 1);   // drag 9-slice borders instead of the grid
	ImGui::SameLine(); ImGui::TextDisabled("%d x %d px", t->width, t->height);
	ImGui::SameLine(); if (ImGui::SmallButton("Fit")) w.slUserView = false;
	ImGui::Separator();

	// advance the preview clock
	int nCells = t->SpriteCount();
	if (w.slFirst < 0) w.slFirst = 0; if (w.slFirst >= nCells) w.slFirst = nCells - 1;
	if (w.slCount < 1) w.slCount = 1; if (w.slCount > nCells - w.slFirst) w.slCount = nCells - w.slFirst;
	if (w.slPlay && w.slFps > 0 && w.slCount > 1) {
		w.slAcc += io.DeltaTime; float dur = 1.0f / w.slFps;
		while (w.slAcc >= dur) { w.slAcc -= dur; w.slCur = (w.slCur + 1) % w.slCount; }
	} else if (!w.slPlay) { w.slCur = 0; w.slAcc = 0; }
	int activeCell = w.slFirst + (w.slCount > 0 ? (w.slCur % w.slCount) : 0);
	int ax0 = 0, ay0 = 0, acw = 0, ach = 0; bool haveActive = t->SpriteCellRect(activeCell, ax0, ay0, acw, ach);

	// ===== LEFT: properties =====
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
	}   // !nineMode
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

		// ---- draggable handles: margin edges + grid lines ----
		bool inArea = overArea || (w.slDrag != 0);
		auto nearX = [&](float sx){ return fabsf(io.MousePos.x - sx) < 5.0f && io.MousePos.y > c0.y && io.MousePos.y < c0.y + csz.y; };
		auto nearY = [&](float sy){ return fabsf(io.MousePos.y - sy) < 5.0f && io.MousePos.x > c0.x && io.MousePos.x < c0.x + csz.x; };
		int hover = 0;   // 1..4 margin L/R/T/B, 5 spacingX, 6 spacingY, 7..10 nine-slice L/R/T/B
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

		// ---- draw sheet + grid ----
		dl->PushClipRect(ia0, ImVec2(ia0.x + iaW, ia0.y + iaH), true);
		ImVec2 ip1 = ImVec2(sX((float)t->width), sY((float)t->height));
		if (w.texPreview) dl->AddImage((ImTextureID)w.texPreview, ip0, ip1);
		dl->AddRect(ip0, ip1, IM_COL32(70, 70, 70, 255));
		int cnt = t->SpriteCount();
		for (int i = 0; i < cnt; ++i) {
			int x0,y0,cw,ch; if (!t->SpriteCellRect(i, x0, y0, cw, ch)) continue;
			ImVec2 p0 = ImVec2(sX((float)x0), sY((float)y0)), p1 = ImVec2(sX((float)(x0 + cw)), sY((float)(y0 + ch)));
			if (p1.x < ia0.x || p0.x > ia0.x + iaW || p1.y < ia0.y || p0.y > ia0.y + iaH) continue;   // cull off-screen cells: bounds the vertex count
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
		ImU32 edge = IM_COL32(255, 140, 40, 220);   // outer margin frame
		dl->AddLine(ImVec2(sX(exL), sY(eyT)), ImVec2(sX(exL), sY(eyB)), edge, 1.5f);
		dl->AddLine(ImVec2(sX(exR), sY(eyT)), ImVec2(sX(exR), sY(eyB)), edge, 1.5f);
		dl->AddLine(ImVec2(sX(exL), sY(eyT)), ImVec2(sX(exR), sY(eyT)), edge, 1.5f);
		dl->AddLine(ImVec2(sX(exL), sY(eyB)), ImVec2(sX(exR), sY(eyB)), edge, 1.5f);
		dl->PopClipRect();

		// ---- rulers: strips outside the image clip; tick labels live here, not on the sheet ----
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
		// handle marks on the rulers at the margin edges
		auto handleX = [&](float sx){ if (sx > c0.x + RL) dl->AddTriangleFilled(ImVec2(sx - 4, c0.y), ImVec2(sx + 4, c0.y), ImVec2(sx, c0.y + RT), edge); };
		auto handleY = [&](float sy){ if (sy > c0.y + RT) dl->AddTriangleFilled(ImVec2(c0.x, sy - 4), ImVec2(c0.x, sy + 4), ImVec2(c0.x + RL, sy), edge); };
		handleX(sX(exL)); handleX(sX(exR)); handleY(sY(eyT)); handleY(sY(eyB));
	}
	ImGui::EndChild();

	// ===== RIGHT: live cell preview (nine-slice mode: stretched preview from the 9 patches) =====
	ImGui::SameLine();
	ImGui::BeginChild("sl_prev", ImVec2(0, 0), ImGuiChildFlags_Borders);
	if (nineMode)
	{
		ImGui::TextDisabled("Stretch preview");
		static float s_pw = 2.0f, s_ph = 1.5f;   // preview stretch factors
		ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##pw", &s_pw, 0.25f, 4.0f, "W x%.2f");
		ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##ph", &s_ph, 0.25f, 4.0f, "H x%.2f");
		if (w.texPreview && t->width > 0 && t->height > 0)
		{
			// Corners keep their pixel size, edges stretch on one axis, the centre on both.
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
			float ys[4] = { 0, bt, dh - bb, dh };
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

	// Drag to reparent within this prefab window; the world pose is preserved.
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
			// reparenting onto self or a descendant would detach the subtree into itself
			bool insideDragged = dragged && FindInSubtree(dragged, (long)a->id.id) != nullptr;
			if (dragged && dragged != a && !insideDragged)
			{
				Transform& mt = dragged->GetTransform();
				Vector3 wp = mt.globalPosition(); Quaternion wr = mt.globalRotation(); Vector3 ws = mt.globalScale();
				w.pv->world->Reparent(dragged, a);
				mt.SetGlobal(wp, wr, ws);
				w.dirty = true; w.editedNow = true;
			}
		}
		ImGui::EndDragDropTarget();
	}

	// Structure ops are deferred: mutating the atom lists mid-walk corrupts them.
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

// Name, transform and reflected components of one prefab atom, with add/remove. True when edited.
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
		if (!keep) toRemove = c;   // header close button = remove component
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
		// Edit-time removal: not Destroy(), which is the runtime hook.
		a->components.remove(toRemove);
		delete toRemove;
		edited = true;
	}

	// Add any registered, create-able Component type, plugin types included.
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
	NukeUI::DocDetachDefault(detachAssetEditors);

	// Tear-off (D3D fallback only; native viewports let imgui detach): dragging a title bar
	// past the main-window edge hands the drag to a host OS window. Re-dock lands in HostDockDrop.
	if (!NukeUI::NativeViewportsActive())
	if (ImGuiContext* g = ImGui::GetCurrentContext())
		if (g->MovingWindow && g->MovingWindow->RootWindow)
		{
			const char* tag = strstr(g->MovingWindow->RootWindow->Name, "###ae:");
			const ImVec2 m  = ImGui::GetMousePos();
			const ImVec2 ds = ImGui::GetIO().DisplaySize;
			// Tear off when the cursor clearly left the window, or is pinned against the
			// virtual-screen edge (a maximized main window makes "past the edge" unreachable).
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
#else
			{
				// Virtual-screen bounds from the imgui monitor list (global coords with
				// viewports on); the mouse pos is already in that space.
				const ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
				if (pio.Monitors.Size > 0)
				{
					float vx = FLT_MAX, vy = FLT_MAX, vr = -FLT_MAX, vb = -FLT_MAX;
					for (int mi = 0; mi < pio.Monitors.Size; ++mi)
					{
						const ImGuiPlatformMonitor& mon = pio.Monitors[mi];
						vx = std::min(vx, mon.MainPos.x);
						vy = std::min(vy, mon.MainPos.y);
						vr = std::max(vr, mon.MainPos.x + mon.MainSize.x);
						vb = std::max(vb, mon.MainPos.y + mon.MainSize.y);
					}
					clamped = m.x <= vx || m.y <= vy || m.x >= vr - 1.0f || m.y >= vb - 1.0f;
				}
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
		// "Dock back" from the host window: a host cannot destroy itself inside its own content tick.
		if (w.wantDock)
		{
			if (w.host) { NukeUI::HostDestroy(w.host); w.host = nullptr; }
			w.detached = false; w.wantDock = false; w.wantFocus = true;
		}
		// Detached mode is the D3D fallback only: an editor-owned borderless GLFW window with its
		// own ImGui context, blitted via GDI (imgui's multi-viewport path races DXGI into device removal).
		if (w.detached && !NukeUI::NativeViewportsActive())
		{
			if (!w.host)
			{
				const std::string title = bfs::path(w.path).filename().string();
				const bool isSlicerH = (w.ext == ".nutex"), isInputH = (w.ext == ".nuinput");
				const bool isAudioH = !w.pv && !isSlicerH && !isInputH
				                    && w.ext != ".nuseq" && w.ext != ".nubonemap";
				const int hw = isAudioH ? 460 : (w.ext == ".nuprefab" ? 900 : (isSlicerH ? 760 : (isInputH ? 820
				             : w.ext == ".nuanim" ? 1000 : w.ext == ".nusm" ? 1100 : w.ext == ".nublend" ? 900
				             : w.ext == ".nuskel" ? 960 : w.ext == ".nurag" ? 900 : w.ext == ".nubonemap" ? 700 : 640)));
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
			// Host born mid-drag: it picks the drag up and rides the cursor until release.
			if (w.dragOut) { NukeUI::HostBeginDrag(w.host, 220.0f, 12.0f); w.dragOut = false; }
			// Content-window flags mirror the docked window.
			{
				// NoScrollbar only: the WINDOW never scrolls (size-oscillation flicker), but the
				// wheel must still reach scrollable children (the prefab hierarchy tree).
				const bool isInputH = (w.ext == ".nuinput");
				NukeUI::HostSetContentFlags(w.host, (w.dirty ? ImGuiWindowFlags_UnsavedDocument : 0)
				    | (isInputH ? 0 : ImGuiWindowFlags_NoScrollbar));
			}
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
				w.open = true;                // keep it until the modal below is answered
				aeCloseConfirm = i;
			}
			continue;                          // content is drawn by the host tick
		}
		if (w.wantFocus) { ImGui::SetNextWindowFocus(); w.wantFocus = false; }
		if (w.hasDrop)
		{
			// Re-docked by drag: appear floating at the drop point, then normal imgui docking takes over.
			w.hasDrop = false;
			ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
			ImGui::SetNextWindowPos(ImVec2(ImMax(0.0f, w.dropX - 220.0f), ImMax(0.0f, w.dropY - 10.0f)), ImGuiCond_Always);
		}
		else if (!NukeUI::NativeViewportsActive())
		{
			// Pin to the main viewport: without native viewports imgui would re-spawn an OS
			// window from a remembered detached position, which the D3D fallback must never do.
			ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
			ImVec2 wp = ImGui::GetMainViewport()->WorkPos;
			ImGui::SetNextWindowPos(ImVec2(wp.x + 80.0f, wp.y + 80.0f), ImGuiCond_Appearing);
		}
		const bool isSlicer = (w.ext == ".nutex");
		const bool isInput  = (w.ext == ".nuinput");
		const bool isSeq    = (w.ext == ".nuseq");
		const bool isAnim   = (w.ext == ".nuanim");
		const bool isSm     = (w.ext == ".nusm");
		const bool isBlend  = (w.ext == ".nublend");
		const bool isSkel  = (w.ext == ".nuskel");
		const bool isRag   = (w.ext == ".nurag");
		const bool isBmap  = (w.ext == ".nubonemap");
		const bool isAudio = !w.pv && !isSlicer && !isInput && !isSeq && !isBmap;
		const float edW = w.ext == ".nuprefab" ? 900.0f : isSlicer ? 760.0f : isInput ? 820.0f
		                : isSeq ? 980.0f : isAnim ? 1000.0f : isSm ? 1100.0f : isBlend ? 900.0f
		                : isSkel ? 960.0f : isRag ? 900.0f : isBmap ? 700.0f : 420.0f;
		ImGui::SetNextWindowSize(isAudio ? ImVec2(420.0f, 170.0f) : ImVec2(edW, 640.0f), ImGuiCond_FirstUseEver);
		const char* icon = isSeq ? ICON_LC_FILM : isAnim ? ICON_LC_PLAY : isSm ? ICON_LC_WORKFLOW
		                 : isBlend ? ICON_LC_BLEND
		                 : (isSkel || isBmap) ? ICON_LC_BONE : isRag ? ICON_LC_PERSON_STANDING
		                 : isAudio ? ICON_LC_MUSIC : isSlicer ? ICON_LC_GRID_2X2 : isInput ? ICON_LC_SETTINGS_2
		                 : w.ext == ".numat" ? ICON_LC_PALETTE : (w.ext == ".numesh" ? ICON_LC_BOX : ICON_LC_PACKAGE);
		std::string title = std::string(icon) + " " + bfs::path(w.path).filename().string() + "###ae:" + w.path;
		// Preview editors get no WINDOW scrollbar: they size to free space, so a flickering
		// scrollbar would oscillate that size every frame. The wheel still routes to scrollable
		// children (hierarchy tree). Form editors (.nuinput/.nuseq) scroll normally.
		// NoCollapse: a title double-click must MAXIMIZE (WindowCaptionButtons), never roll the
		// window up into a strip — imgui would eat the double-click inside Begin otherwise.
		ImGuiWindowFlags wf = window_flags | ImGuiWindowFlags_NoCollapse
		                    | (w.dirty ? ImGuiWindowFlags_UnsavedDocument : 0)
		                    | ((isInput || isSeq || isBmap) ? 0 : ImGuiWindowFlags_NoScrollbar);
		if (ImGui::Begin(title.c_str(), &w.open, wf))
		{
			// minimize/maximize next to imgui's X once this window owns an OS window (native
			// viewports) — no-op while it is docked inside the main one
			NukeUI::WindowCaptionButtons();
			DrawAssetEditorBody(i);
		}
		ImGui::End();
		if (!w.open && w.dirty)
		{
			w.open = true;               // keep the window until the modal is answered
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
				else if (w.ext == ".nuseq"    && w.seq)        w.seq->SaveToFile(w.path);
				else if (w.ext == ".nuanim"   && w.anim)       w.anim->SaveToFile(w.path);
				else if (w.ext == ".nusm"     && w.sm)         w.sm->SaveToFile(w.path);
				else if (w.ext == ".nublend"  && w.blend)      w.blend->SaveToFile(w.path);
				else if (w.ext == ".nuskel"   && w.skel)       w.skel->SaveToFile(w.path);
				else if (w.ext == ".nurag"    && w.rag)        w.rag->SaveToFile(w.path);
				else if (w.ext == ".nubonemap" && w.bmap)      w.bmap->SaveToFile(w.path);
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
			if (w.texPreview)   // destruction is deferred inside the renderer's GPU trash
			{
				if (iRender* r = AppInstance::GetSingleton()->render) r->destroyTexture2D(w.texPreview);
				w.texPreview = 0;
			}
			if (w.tex) delete w.tex;
			delete w.seqPv;
			delete w.seq;
			// stage-10 editors: rig atoms out of their worlds FIRST — a live Animator must not
			// outlive the editing copies its preview pointers reference.
			if (w.anAtomId && w.pv)   w.pv->world->RemoveAtomById(w.anAtomId);
			if (w.smAtomId && w.pv)   w.pv->world->RemoveAtomById(w.smAtomId);
			if (w.blAtomId && w.pv)   w.pv->world->RemoveAtomById(w.blAtomId);
			if (w.skAtomId && w.pv)   w.pv->world->RemoveAtomById(w.skAtomId);
			if (w.rgAtomId && w.pv)   w.pv->world->RemoveAtomById(w.rgAtomId);
			if (w.anPv2)
			{
				if (w.anAtomId2) w.anPv2->world->RemoveAtomById(w.anAtomId2);
				ReleasePreview(w.anPv2);
				w.anPv2 = nullptr;
			}
			delete w.anim;
			delete w.sm;
			delete w.blend;
			delete w.blSm;
			delete w.skel;
			delete w.rag;
			delete w.bmap;
			if (w.pv) ReleasePreview(w.pv);
			if (w.host) { NukeUI::HostDestroy(w.host); w.host = nullptr; }
			assetEds.erase(assetEds.begin() + i);
		}
}

// Body of one asset editor window, shared by the docked imgui window and the detached host window.
void EditorUI::DrawAssetEditorBody(int i)
{
	AssetEditorWin& w = assetEds[i];
	const bool isSlicer = (w.ext == ".nutex");
	const bool isInput  = (w.ext == ".nuinput");
	const bool isSeq    = (w.ext == ".nuseq");
	const bool isAnim   = (w.ext == ".nuanim");
	const bool isSm     = (w.ext == ".nusm");
	const bool isBlend  = (w.ext == ".nublend");
	const bool isSkel   = (w.ext == ".nuskel");
	const bool isRag    = (w.ext == ".nurag");
	const bool isBmap   = (w.ext == ".nubonemap");
	const bool isAudio  = !w.pv && !isSlicer && !isInput && !isSeq && !isBmap;
	(void)isAudio;
	if (isSeq || isAnim || isSm || isBlend || isSkel || isRag || isBmap)   // asset editors with their own body + burst-latched undo
	{
		const bool edFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		if (edFocused) aeFocused = i;
		if (isSeq)        DrawSequenceEditor(w);
		else if (isAnim)  DrawAnimEditor(w);
		else if (isSm)    DrawSMEditor(w);
		else if (isBlend) DrawBlendEditor(w);
		else if (isSkel)  DrawSkeletonEditor(w);
		else if (isRag)   DrawRagdollEditor(w);
		else              DrawBoneMapEditor(w);
		if (edFocused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
		{
			if (isSeq && w.seq)          w.seq->SaveToFile(w.path);
			else if (isAnim && w.anim)   w.anim->SaveToFile(w.path);
			else if (isSm && w.sm)       w.sm->SaveToFile(w.path);
			else if (isBlend && w.blend) w.blend->SaveToFile(w.path);
			else if (isSkel && w.skel)   w.skel->SaveToFile(w.path);
			else if (isRag && w.rag)     w.rag->SaveToFile(w.path);
			else if (isBmap && w.bmap)   w.bmap->SaveToFile(w.path);
			w.dirty = false;
		}
		// per-window undo latch (one edit burst = one entry), .nuinput pattern
		std::vector<std::string>& undo = isSeq ? w.undoQ : isAnim ? w.undoAn : isSm ? w.undoSm
		                               : isBlend ? w.undoB : isSkel ? w.undoSk : isRag ? w.undoRg : w.undoBm;
		std::vector<std::string>& redo = isSeq ? w.redoQ : isAnim ? w.redoAn : isSm ? w.redoSm
		                               : isBlend ? w.redoB : isSkel ? w.redoSk : isRag ? w.redoRg : w.redoBm;
		std::string& idle = isSeq ? w.idleQ : isAnim ? w.idleAn : isSm ? w.idleSm
		                  : isBlend ? w.idleB : isSkel ? w.idleSk : isRag ? w.idleRg : w.idleBm;
		if (w.editedNow && !w.editing && !idle.empty())
		{
			undo.push_back(idle);
			if (undo.size() > 64) undo.erase(undo.begin());
			redo.clear();
		}
		w.editing = w.editedNow;
		w.editedNow = false;
		if (!w.editing)
		{
			if (isSeq && w.seq)          idle = w.seq->ToString();
			else if (isAnim && w.anim)   idle = EditorAnimMetaJson(w.anim);
			else if (isSm && w.sm)       idle = w.sm->ToString();
			else if (isBlend && w.blend) idle = w.blend->ToString();
			else if (isSkel && w.skel)   idle = w.skel->ToString();
			else if (isRag && w.rag)     idle = w.rag->ToString();
			else if (isBmap && w.bmap)   idle = EditorBoneMapJson(w.bmap);
		}
		return;
	}
			const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
			if (focused) aeFocused = i;   // Ctrl+Z/Ctrl+Y route to this window's history
			bool wantSave = false;
			if (w.ext != ".numesh" && !isAudio)
			{
				if (ImGui::SmallButton(ICON_LC_SAVE " Save")) wantSave = true;
				ImGui::SameLine();
				if (ImGui::SmallButton(ICON_LC_UNDO_2 " Revert"))
				{
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
				// Left: material fields. Right: the 3D view fills the rest.
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
				bool matCh = false;
				if (nuke::TypeInfo* ti = w.mat->GetType()) matCh |= DrawFields(w.mat, ti);
				matCh |= DrawLiveMaterialSections(w.mat);
				if (matCh)
				{
					w.dirty = true; w.editedNow = true;
					if (w.pv->mr->mat) delete w.pv->mr->mat;   // live preview: fresh clone
					w.pv->mr->mat = w.mat->Clone();
				}
				ImGui::EndChild();
				ImGui::SameLine();
				ImGui::BeginGroup();
				// Toolbar over the preview (same idiom as the prefab gizmo bar): the trigger
				// tool is a toggled icon button, the event picker sits next to it while armed.
				// The trigger tool fires EVENTS and HIT REACTIONS alike: one list, one click.
				std::vector<std::string> fireList;
				for (const nuke::LiveEvent& fe : w.mat->liveEvents) fireList.push_back(fe.name);
				for (const nuke::LiveHit& fh : w.mat->liveHits)
					fireList.push_back("Hit: " + (fh.hitType.empty() ? std::string("(any)") : fh.hitType));
				if (!fireList.empty())
				{
					// width from the GLYPH, not a magic number — the icon must never clip
					const float tbw = std::max(34.0f, ImGui::CalcTextSize(ICON_LC_CROSSHAIR).x
					                                + ImGui::GetStyle().FramePadding.x * 2.0f);
					if (ToolBtn(ICON_LC_CROSSHAIR, "Trigger tool: click the sample to fire the picked event/hit at that spot",
					            w.evtTool, tbw))
						w.evtTool = !w.evtTool;
					if (w.evtTool)
					{
						ImGui::SameLine();
						if (w.evtSel < 0 || w.evtSel >= (int)fireList.size()) w.evtSel = 0;
						ImGui::SetNextItemWidth(200);
						if (ImGui::BeginCombo("##evtsel", fireList[w.evtSel].c_str()))
						{
							for (int e = 0; e < (int)fireList.size(); ++e)
								if (ImGui::Selectable((fireList[e] + "##ev" + std::to_string(e)).c_str(), e == w.evtSel))
									w.evtSel = e;
							ImGui::EndCombo();
						}
					}
				}
				else w.evtTool = false;
				// State simulator: toolbar button (next to the trigger tool) — sliders drive
				// the GLOBAL Surface conditions live; preview + open world react immediately.
				if (!w.mat->liveStates.empty())
				{
					ImGui::SameLine();
					const float sbw = std::max(34.0f, ImGui::CalcTextSize(ICON_LC_CLOUD_DRIZZLE).x
					                                + ImGui::GetStyle().FramePadding.x * 2.0f);
					if (ImGui::Button(ICON_LC_CLOUD_DRIZZLE, ImVec2(sbw, 0)))
						ImGui::OpenPopup("##condsim");
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("State simulator: drive the wet/snow/... conditions live");
					if (ImGui::BeginPopup("##condsim"))
					{
						// PREVIEW channel: overrides reads, never serialized with the world.
						std::set<std::string> seen;
						for (const nuke::LiveState& s : w.mat->liveStates)
						{
							if (s.state.empty() || !seen.insert(s.state).second) continue;
							float v = (float)nuke::Surface::Condition(s.state);
							ImGui::SetNextItemWidth(160);
							if (ImGui::SliderFloat(s.state.c_str(), &v, 0.0f, 1.0f, "%.2f"))
								nuke::Surface::SetConditionPreview(s.state, v);
						}
						if (ImGui::Button("Clear All", ImVec2(-1, 0)))
							nuke::Surface::ClearConditionPreviews();
						ImGui::EndPopup();
					}
				}
				DrawPreviewImage(*w.pv, ImGui::GetContentRegionAvail());
				if (w.evtTool && ImGui::IsItemHovered() && ImGui::IsMouseClicked(0))
					FireEventAtPreview(w);
				ImGui::EndGroup();
			}
			else if (w.ext == ".numesh")
			{
				if (nuke::Mesh* m = w.pv->mr->mesh)
				{
					m->EnsureBounds();
					ImGui::Text("%d vertices   %d triangles   bounds %.2f x %.2f x %.2f",
						m->numVerts, m->TriCount(),
						m->aabbMax[0] - m->aabbMin[0], m->aabbMax[1] - m->aabbMin[1], m->aabbMax[2] - m->aabbMin[2]);
				}
				else ImGui::TextDisabled("Mesh is not in the resource DB.");
				DrawPreviewImage(*w.pv, ImGui::GetContentRegionAvail());
			}
			else if (isSlicer && w.tex)
			{
				DrawSpriteSlicer(w);
			}
			else if (w.ext == ".nuprefab" && w.prefabRoot)
			{
				// Deferred tree ops queued by the tree's context menu.
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
				ImGui::BeginChild("##ptree", ImVec2(240, 0), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders,
				                  ImGuiWindowFlags_HorizontalScrollbar);   // deep rigs overflow to the right
				DrawPrefabTree(w, w.prefabRoot);
				ImGui::EndChild();
				ImGui::SameLine();
				ImGui::BeginChild("##pright", ImVec2(0, 0), ImGuiChildFlags_None,
				                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
				{
					const float tbw = 34.0f;
					if (ToolBtn(ICON_LC_MOUSE_POINTER, "Select (Q)", w.gizmoOp == 0, tbw)) w.gizmoOp = 0; ImGui::SameLine();
					if (ToolBtn(ICON_LC_MOVE,          "Move (W)",   w.gizmoOp == 1, tbw)) w.gizmoOp = 1; ImGui::SameLine();
					if (ToolBtn(ICON_LC_ROTATE_3D,     "Rotate (E)", w.gizmoOp == 2, tbw)) w.gizmoOp = 2; ImGui::SameLine();
					if (ToolBtn(ICON_LC_SCALING,       "Scale (R)",  w.gizmoOp == 3, tbw)) w.gizmoOp = 3; ImGui::SameLine();
					if (ToolBtn(w.gizmoWorld ? ICON_LC_GLOBE : ICON_LC_AXIS_3D,
					            w.gizmoWorld ? "World space (X)" : "Local space (X)", false, tbw))
						w.gizmoWorld = !w.gizmoWorld;

					// Animation preview: play ticks the subtree's Animators, stop restores the snapshot.
					if (SubtreeHasAnimator(w.prefabRoot))
					{
						ImGui::SameLine();
						if (ToolBtn(w.animPlay ? ICON_LC_SQUARE : ICON_LC_PLAY,
						            w.animPlay ? "Stop animation preview" : "Play animation preview",
						            w.animPlay, tbw))
							ToggleAnimPreview(w);
					}
					if (w.animPlay) TickAnimPreview(w);

					// Viewport hotkeys scoped to this window; suppressed while typing or flying.
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
							FramePreview(*w.pv, fs ? fs : w.prefabRoot);   // frame selection, else all
						}
						if (ImGui::IsKeyPressed(ImGuiKey_Delete)
						    && w.prefabSelId && w.prefabSelId != (long)w.prefabRoot->id.id)
							w.pendingDeleteId = w.prefabSelId;
					}

					ImVec2 av = ImGui::GetContentRegionAvail();
					float edH = 320.0f;                                   // atom editor strip
					if (av.y - edH < 160.0f) edH = std::max(120.0f, av.y * 0.45f);
					DrawPreviewImage(*w.pv, ImVec2(av.x, av.y - edH - 8.0f));

					// Transform gizmo over the 3D view.
					Atom* sel = FindInSubtree(w.prefabRoot, w.prefabSelId);
					w.pv->gizmoBusy = false;   // set below, inside the gizmo's ID scope
					if (sel && w.gizmoOp != 0 && w.pv->cam && w.pv->cam->transform
					    && w.pv->rectSize.x > 1.0f && w.pv->rectSize.y > 1.0f)
					{
						ImGuizmo::SetOrthographic(false);
						ImGuizmo::SetDrawlist();
						ImGuizmo::SetRect(w.pv->rectMin.x, w.pv->rectMin.y, w.pv->rectSize.x, w.pv->rectSize.y);
						ImGuizmo::PushID(w.path.c_str());   // several gizmos may run per frame

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
						if (!ImGuizmo::IsUsing())   // resync from the atom only when not dragging
						{
							Vector3 gP = gtt.globalPosition(); Quaternion gR = gtt.globalRotation(); Vector3 gS = gtt.globalScale();
							glm::mat4 gm = glm::translate(glm::mat4(1.0f), glm::vec3((float)gP.x, (float)gP.y, (float)gP.z))
							             * glm::mat4_cast(glm::quat((float)gR.w, (float)gR.x, (float)gR.y, (float)gR.z))
							             * glm::scale(glm::mat4(1.0f), glm::vec3((float)gS.x, (float)gS.y, (float)gS.z));
							memcpy(w.gizmoMtx, glm::value_ptr(gm), sizeof(w.gizmoMtx));
						}
						ImGuizmo::OPERATION gop = (w.gizmoOp == 1) ? ImGuizmo::TRANSLATE
						                        : (w.gizmoOp == 2) ? ImGuizmo::ROTATE : ImGuizmo::SCALE;
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
						// Must be queried inside the ID scope to be accurate.
						w.pv->gizmoBusy = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
						ImGuizmo::PopID();
					}

					// LMB picking: ray from the preview camera through the click point. The window-
					// hover gate keeps clicks landing in OVERLAYS (the Add Component popup opens
					// over the 3D view) from re-picking/deselecting under the popup.
					if (w.pv->cam && w.pv->cam->transform && !w.pv->gizmoBusy
					    && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
					    && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
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
							// Only atoms of this prefab are selectable; the pick can return scene furniture.
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
			else if (isAudio)   // transport on the Preview bus, never game-paused
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
				DrawInputEditor(w);   // actions / contexts / bindings CRUD, edits w.in*

			if (focused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) wantSave = true;
			if (wantSave)
			{
				if      (w.ext == ".numat"    && w.mat)        { w.mat->SaveToFile(w.path); w.dirty = false; }
				else if (w.ext == ".nuprefab" && w.prefabRoot) { nuke::SavePrefab(w.prefabRoot, w.path); w.dirty = false; }
				else if (isSlicer && w.tex)                    { w.tex->SaveToFile(w.path); SlicerApplyLive(w.tex); w.dirty = false; }
				else if (isInput)                              { SaveInputAsset(w); w.dirty = false; }
			}

			// Per-window undo latch: one edit burst (drag, typing) becomes one entry.
			if (w.editedNow && !w.editing)
			{
				// Push the pre-edit baseline; a new edit invalidates the redo branch.
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
				// Burst settled: the current state becomes the next baseline.
				w.editing = false;
				if      (w.ext == ".nuprefab" && w.prefabRoot) w.idleP = nuke::SaveAtomToString(w.prefabRoot);
				else if (w.ext == ".numat"    && w.mat)        { delete w.idleM; w.idleM = w.mat->Clone(); }
				else if (w.ext == ".nuinput")                  w.idleI = InputMapJson(w);
			}
			w.editedNow = false;
}
