// Viewport panel: EditorUI method definitions.

// Infinite camera look/pan: while an RMB/MMB drag is live the cursor hides and WRAPS at the
// viewport-image edges — it can never land on other widgets or leave the panel (works for the
// docked panel AND detached native-viewport windows); release puts it back at the press point.
static bool s_lookActive = false;
static bool s_lookWarpSkip = false;     // ignore the mouse delta on the frame after a warp
// Fly-speed multiplier: wheel while RMB is held scales it (wheel without RMB still dollies).
static float  s_flyMul = 1.0f;
static double s_flyMulShowUntil = 0.0;   // brief on-screen readout after a change
#include <editor/editorui.h>
// marquee rect-select: armed by a press on empty space, live once the drag passes threshold.
static bool  s_marqueeArm = false, s_marqueeLive = false;
static ImVec2 s_marqueeStart;
static ImVec2 s_lookPressPos;           // cursor restore point for the look wrap
#include "nukeui.h"
#include <GLFW/glfw3.h>   // look wrap: move the OS cursor of the current imgui viewport

// Move the OS cursor to `to` (imgui screen coords) inside the CURRENT imgui viewport's window
// and tell imgui, skipping the resulting delta. No-op where the panel has no GLFW window
// (GDI-blit fallback hosts).
static void WarpMouse(const ImVec2& to)
{
	ImGuiViewport* vp = ImGui::GetWindowViewport();
	GLFWwindow* wnd = vp ? (GLFWwindow*)vp->PlatformHandle : nullptr;
	if (!wnd) return;
	glfwSetCursorPos(wnd, to.x - vp->Pos.x, to.y - vp->Pos.y);
	ImGui::GetIO().AddMousePosEvent(to.x, to.y);
	s_lookWarpSkip = true;
}
#include <interface/EditorHooks.h>   // module-registered viewport overlays
#include "API/Model/Math.h"
#include "API/Model/resdb.h"
#include "API/Model/Light.h"
#include "API/Model/ReflectionProbe.h"
#include "API/Model/Environment.h"
#include "API/Model/Screen.h"
#include "API/Model/Canvas.h"
#include <interface/ComponentIcons.h>
#include <API/Model/Foliage.h>
#include <API/Model/Surface.h>
#include <API/Model/Spline.h>   // engine spline: exact polyline for hit-tests, point-edit API
#include <API/Model/WorldStream.h>   // the streaming-cell overlay reads DebugCells
#include <API/Model/Decal.h>         // decal volumes: viewport icon + editor picking
#include <reflect/Reflect.h>            // WaterRiver type lives in the water plugin — reached by name
#include <API/Model/DebugDraw.h>
#include <functional>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <cstring>   // strcmp: cross-DLL type-name match
#include <cstdio>    // snprintf: streaming overlay labels
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/type_ptr.hpp>

// Set while the canvas 2D rect gizmo is hovered/dragged — click-pick must not steal those clicks.
static bool s_canvasGizmoHot = false;

// Same for curve control-point handles (WaterRiver + Spline): mutes click-pick and the
// transform gizmo.
static bool s_riverGizmoHot = false;

// Ray-picks the atom under a screen point inside the viewport image; null when nothing is hit.
// The world point under the cursor: the first surface the ray meets, else the ground plane at
// the camera's focus height. This is where a dropped asset belongs — the drop position IS the
// user's intent, and "spawn at the origin, then hunt for it" is not a workflow.
static nuke::Vector3 DropPointAt(nuke::Camera* cam, ImVec2 rmin, ImVec2 sz, ImVec2 mp)
{
	nuke::Vector3 out(0, 0, 0);
	if (!cam || !cam->transform || sz.x <= 0.0f || sz.y <= 0.0f) return out;
	nuke::Transform* t = cam->transform;
	const float ndcx = ((mp.x - rmin.x) / sz.x) * 2.0f - 1.0f;
	const float ndcy = 1.0f - ((mp.y - rmin.y) / sz.y) * 2.0f;
	nuke::Vector3 o = t->globalPosition();
	nuke::Vector3 f = t->direction(), rr = t->right(), uu = t->up();
	const float aspect = sz.x / sz.y;
	nuke::Vector3 dir = f;
	if (cam->projBlend >= 0.5f)   // ortho: the origin slides, the direction is constant
	{
		const float halfH = (cam->orthoSize > 1e-4f) ? cam->orthoSize : 1.0f, halfW = halfH * aspect;
		o = nuke::Vector3(o.x + ndcx * halfW * rr.x + ndcy * halfH * uu.x,
		                  o.y + ndcx * halfW * rr.y + ndcy * halfH * uu.y,
		                  o.z + ndcx * halfW * rr.z + ndcy * halfH * uu.z);
	}
	else
	{
		const float thf = tanf((float)cam->fov * 0.5f * 0.01745329252f);
		dir = nuke::Vector3(f.x + ndcx * thf * aspect * rr.x + ndcy * thf * uu.x,
		                    f.y + ndcx * thf * aspect * rr.y + ndcy * thf * uu.y,
		                    f.z + ndcx * thf * aspect * rr.z + ndcy * thf * uu.z);
	}
	const double dl = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
	if (dl > 1e-9) { dir.x /= dl; dir.y /= dl; dir.z /= dl; }

	float dist = 0.0f;
	if (nuke::AppInstance::GetSingleton()->currentWorld->PickDist(o, dir, dist) && dist > 0.0f)
		return nuke::Vector3(o.x + dir.x * dist, o.y + dir.y * dist, o.z + dir.z * dist);
	// Nothing under the cursor: fall to y = 0, or 10 m ahead when the ray never gets there.
	if (std::fabs(dir.y) > 1e-4 && (-o.y / dir.y) > 0.0)
	{
		const double t0 = -o.y / dir.y;
		return nuke::Vector3(o.x + dir.x * t0, 0.0, o.z + dir.z * t0);
	}
	return nuke::Vector3(o.x + dir.x * 10.0, o.y + dir.y * 10.0, o.z + dir.z * 10.0);
}

// Landing marker drawn while the payload hovers: a ground cross + a small box, so the drop is
// aimed rather than guessed.
static void DrawDropPreview(const nuke::Vector3& p)
{
	const nuke::Color c(0.35, 0.85, 1.0, 1.0);
	nuke::DebugDraw::WireBox(nuke::Vector3(p.x, p.y + 0.5, p.z), nuke::Vector3(0.5, 0.5, 0.5),
	                         nuke::Quaternion(0, 0, 0, 1), c);
	nuke::DebugDraw::Line(nuke::Vector3(p.x - 1.0, p.y, p.z), nuke::Vector3(p.x + 1.0, p.y, p.z), c);
	nuke::DebugDraw::Line(nuke::Vector3(p.x, p.y, p.z - 1.0), nuke::Vector3(p.x, p.y, p.z + 1.0), c);
	nuke::DebugDraw::WireCircle(p, nuke::Vector3(0, 1, 0), 0.6, c);
}

static nuke::Atom* PickAtScreen(nuke::Camera* cam, ImVec2 rmin, ImVec2 sz, ImVec2 mp)
{
	if (!cam || !cam->transform || sz.x <= 0.0f || sz.y <= 0.0f) return nullptr;
	nuke::Transform* t = cam->transform;
	float ndcx = ((mp.x - rmin.x) / sz.x) * 2.0f - 1.0f;
	float ndcy = 1.0f - ((mp.y - rmin.y) / sz.y) * 2.0f;
	nuke::Vector3 o = t->globalPosition();
	nuke::Vector3 f = t->direction(), rr = t->right(), uu = t->up();
	float aspect = sz.x / sz.y;
	if (cam->projBlend >= 0.5f)   // ortho: rays are parallel — the origin slides, not the direction
	{
		float halfH = (cam->orthoSize > 1e-4f) ? cam->orthoSize : 1.0f, halfW = halfH * aspect;
		nuke::Vector3 ori(o.x + ndcx * halfW * rr.x + ndcy * halfH * uu.x,
		                  o.y + ndcx * halfW * rr.y + ndcy * halfH * uu.y,
		                  o.z + ndcx * halfW * rr.z + ndcy * halfH * uu.z);
		float pd;   // editor pick: invisible volumes (decal boxes) are selectable too
		return nuke::AppInstance::GetSingleton()->currentWorld->PickEditor(ori, f, pd);
	}
	float thf = tanf((float)cam->fov * 0.5f * 0.01745329252f);
	nuke::Vector3 dir(f.x + ndcx * thf * aspect * rr.x + ndcy * thf * uu.x,
	                  f.y + ndcx * thf * aspect * rr.y + ndcy * thf * uu.y,
	                  f.z + ndcx * thf * aspect * rr.z + ndcy * thf * uu.z);
	float pd;   // editor pick: invisible volumes (decal boxes) are selectable too
	return nuke::AppInstance::GetSingleton()->currentWorld->PickEditor(o, dir, pd);
}

// Editor camera projection (glm, LH depth 0..1), blended persp<->ortho like the renderer,
// so gizmos / icons / picking stay glued to the rendered image.
static glm::mat4 EditorCamProj(nuke::Camera* cam, float aspect)
{
	glm::mat4 persp = glm::perspectiveLH_ZO((float)cam->fov * 0.01745329252f, aspect, cam->_near, cam->_far);
	float b = cam->projBlend;
	if (b <= 0.0001f) return persp;
	float halfH = (cam->orthoSize > 1e-4f) ? cam->orthoSize : 1.0f;
	glm::mat4 orth = glm::orthoLH_ZO(-halfH * aspect, halfH * aspect, -halfH, halfH, cam->_near, cam->_far);
	if (b >= 0.9999f) return orth;
	glm::mat4 r;
	for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) r[i][j] = persp[i][j] * (1.0f - b) + orth[i][j] * b;
	return r;
}

void EditorUI::DrawEntityIcons(ImVec2 rmin, ImVec2 sz)
{
	iconHits.clear();
	AppInstance* app = AppInstance::GetSingleton();
	if (!editorCam || !editorCam->transform || !app->currentWorld) return;
	if (app->playState != 0) return;                       // edit mode only
	if (sz.x <= 1.0f || sz.y <= 1.0f) return;

	// Must be the EXACT view/proj the gizmo uses (LH, depth 0..1) or icons drift off the image.
	glm::mat4 vp;
	{
		Transform* c = editorCam->transform;
		Vector3 e = c->globalPosition();
		Vector3 f = c->direction(), u = c->up();
		float aspect = sz.x / sz.y;
		float fovy = (float)editorCam->fov * 0.01745329252f;
		glm::mat4 v = glm::lookAtLH(
			glm::vec3((float)e.x, (float)e.y, (float)e.z),
			glm::vec3((float)(e.x + f.x), (float)(e.y + f.y), (float)(e.z + f.z)),
			glm::vec3((float)u.x, (float)u.y, (float)u.z));
		(void)fovy;
		vp = EditorCamProj(editorCam, aspect) * v;
	}

	struct Icon { float depth; ImVec2 c; const char* glyph; ImU32 col; Atom* atom; };
	std::vector<Icon> icons;
	std::function<void(bc::list<Atom*>&)> walk = [&](bc::list<Atom*>& gos)
	{
		for (Atom* atom : gos)
		{
			if (!atom) continue;
			const char* glyph = nullptr;
			ImU32 col = IM_COL32(230, 230, 230, 235);
			if (Camera* cam = atom->GetComponent<Camera>())
			{
				if (cam != editorCam) glyph = ICON_LC_VIDEO;   // the editor camera has no icon
			}
			else if (Light* l = atom->GetComponent<Light>())
			{
				glyph = l->type == 0 ? ICON_LC_SUN : (l->type == 2 ? ICON_LC_SPOTLIGHT : ICON_LC_LIGHTBULB);
				const double fl = 0.35;   // color floor so a dark light stays readable
				col = IM_COL32((int)(255.0 * std::max(l->color.r, fl)),
				               (int)(255.0 * std::max(l->color.g, fl)),
				               (int)(255.0 * std::max(l->color.b, fl)), 235);
			}
			else if (atom->GetComponent<ReflectionProbe>()) { glyph = ICON_LC_APERTURE;  col = IM_COL32(200, 140, 255, 235); }
			else if (atom->GetComponent<Environment>())     { glyph = ICON_LC_CLOUD_SUN; col = IM_COL32(150, 200, 255, 235); }
			else if (atom->GetComponent<Decal>())           { glyph = ICON_LC_STICKER;   col = IM_COL32(255, 200, 140, 235); }
			else
			{
				// Registered component icons: match by type-name CONTENT — pointer compare fails across DLLs.
				for (nuke::Component* mc : atom->components)
				{
					if (!mc || !mc->name) continue;
					for (const nuke::ComponentIcon& ci : nuke::ComponentIcons())
						if (!strcmp(mc->name, ci.component.c_str()))
						{
							glyph = ci.glyph.c_str();
							col = IM_COL32((int)(ci.color[0] * 255.f), (int)(ci.color[1] * 255.f),
							               (int)(ci.color[2] * 255.f), (int)(ci.color[3] * 255.f));
							break;
						}
					if (glyph) break;
				}
			}
			if (glyph)
			{
				Vector3 p = atom->GetTransform().globalPosition();
				glm::vec4 clip = vp * glm::vec4((float)p.x, (float)p.y, (float)p.z, 1.0f);
				if (clip.w > 0.01f)   // in front of the camera
				{
					float nx = clip.x / clip.w, ny = clip.y / clip.w;
					if (nx >= -1.02f && nx <= 1.02f && ny >= -1.02f && ny <= 1.02f)
						icons.push_back({ clip.w,
							ImVec2(rmin.x + (nx * 0.5f + 0.5f) * sz.x,
							       rmin.y + (0.5f - ny * 0.5f) * sz.y),
							glyph, col, atom });
				}
			}
			if (!atom->children.empty()) walk(atom->children);
		}
	};
	walk(app->currentWorld->GetHierarchy());
	if (icons.empty()) return;

	// Far first, so nearer icons draw ON TOP (and win the click, tested back-to-front).
	std::sort(icons.begin(), icons.end(), [](const Icon& a, const Icon& b) { return a.depth > b.depth; });

	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImFont* font = ImGui::GetFont();
	const float isz = ImGui::GetFontSize() * 1.45f;
	Atom* sel = app->selectedInHieararchy;
	for (const Icon& ic : icons)
	{
		ImVec2 ts = font->CalcTextSizeA(isz, FLT_MAX, 0.0f, ic.glyph);
		ImVec2 p0(ic.c.x - ts.x * 0.5f, ic.c.y - ts.y * 0.5f);
		if (ic.atom == sel)   // selection ring
			dl->AddCircle(ic.c, ts.x * 0.85f, IM_COL32(255, 190, 60, 220), 0, 1.5f);
		dl->AddText(font, isz, ImVec2(p0.x + 1.0f, p0.y + 1.0f), IM_COL32(0, 0, 0, 170), ic.glyph);   // shadow
		dl->AddText(font, isz, p0, ic.col, ic.glyph);
		iconHits.emplace_back(ImVec4(p0.x, p0.y, p0.x + ts.x, p0.y + ts.y), ic.atom);
	}

	// Hover tooltip: topmost icon under the mouse (list is back-to-front — walk it backwards).
	ImVec2 mp = ImGui::GetIO().MousePos;
	for (auto it = iconHits.rbegin(); it != iconHits.rend(); ++it)
		if (mp.x >= it->first.x && mp.x <= it->first.z && mp.y >= it->first.y && mp.y <= it->first.w)
		{
			ImGui::SetTooltip("%s", it->second->GetName().c_str());
			break;
		}
}

// Module-registered viewport overlays (interface/EditorHooks.h): the editor hands over the
// rect + editor-camera view-projection and knows nothing about who draws.
void EditorUI::DrawModuleOverlays(ImVec2 rmin, ImVec2 sz)
{
	if (ViewportOverlays().empty()) return;
	if (!editorCam || !editorCam->transform) return;
	if (sz.x <= 1.0f || sz.y <= 1.0f) return;
	glm::mat4 vp;
	{
		Transform* c = editorCam->transform;
		Vector3 e = c->globalPosition();
		Vector3 f = c->direction(), u = c->up();
		glm::mat4 v = glm::lookAtLH(
			glm::vec3((float)e.x, (float)e.y, (float)e.z),
			glm::vec3((float)(e.x + f.x), (float)(e.y + f.y), (float)(e.z + f.z)),
			glm::vec3((float)u.x, (float)u.y, (float)u.z));
		vp = EditorCamProj(editorCam, sz.x / sz.y) * v;
	}
	EditorViewportCtx ctx;
	ctx.x = rmin.x; ctx.y = rmin.y; ctx.w = sz.x; ctx.h = sz.y;
	std::memcpy(ctx.viewProj, &vp[0][0], sizeof(ctx.viewProj));
	for (const EditorViewportOverlay& o : ViewportOverlays()) o.draw(ctx);
}

void EditorUI::DrawStreamCells(ImVec2 rmin, ImVec2 sz)
{
	AppInstance* app = AppInstance::GetSingleton();
	nuke::World* w = app->currentWorld;
	if (!w || !w->stream || !editorCam || !editorCam->transform) return;
	if (sz.x <= 1.0f || sz.y <= 1.0f) return;

	glm::mat4 vp;
	{
		Transform* c = editorCam->transform;
		Vector3 e = c->globalPosition();
		Vector3 f = c->direction(), u = c->up();
		glm::mat4 v = glm::lookAtLH(
			glm::vec3((float)e.x, (float)e.y, (float)e.z),
			glm::vec3((float)(e.x + f.x), (float)(e.y + f.y), (float)(e.z + f.z)),
			glm::vec3((float)u.x, (float)u.y, (float)u.z));
		vp = EditorCamProj(editorCam, sz.x / sz.y) * v;
	}
	auto toScreen = [&](double wx, double wz, ImVec2& out) -> bool
	{
		glm::vec4 clip = vp * glm::vec4((float)wx, 0.0f, (float)wz, 1.0f);
		if (clip.w < 0.05f) return false;
		out = ImVec2(rmin.x + (clip.x / clip.w * 0.5f + 0.5f) * sz.x,
		             rmin.y + (0.5f - clip.y / clip.w * 0.5f) * sz.y);
		return true;
	};
	auto fmtBytes = [](uint64_t b, char* buf, size_t n)
	{
		if (b >= (10ull << 20)) std::snprintf(buf, n, "%.0f MB", b / 1048576.0);
		else if (b >= (1ull << 20)) std::snprintf(buf, n, "%.1f MB", b / 1048576.0);
		else std::snprintf(buf, n, "%.0f KB", b / 1024.0);
	};

	const double cs = std::max(8.0f, (float)w->settings.streamCellSize);
	struct Row { nuke::WorldStream::CellInfo ci; int atoms = 0; };
	std::vector<Row> rows;
	{
		std::vector<nuke::WorldStream::CellInfo> cells;
		w->stream->DebugCells(cells);
		rows.reserve(cells.size());
		for (const auto& ci : cells) rows.push_back({ ci, 0 });
	}
	// The editor FOLDS cells into the live world, so in edit mode the runtime map is empty (or
	// partial). Synthesize the PROSPECTIVE partition from the spatial roots — the cells a save
	// would split — and count atoms per cell; runtime cells just gain their atom count.
	{
		std::map<std::pair<int, int>, int> counts;
		for (Atom* a : app->currentWorld->GetHierarchy())
			if (a && nuke::WorldStream::Spatial(a))
			{
				Vector3 p = a->GetTransform().globalPosition();
				const nuke::WorldStream::CellKey k = nuke::WorldStream::CellOf(p.x, p.z, (float)cs);
				++counts[{ k.x, k.z }];
			}
		for (Row& r : rows)
		{
			auto it = counts.find({ r.ci.key.x, r.ci.key.z });
			if (it != counts.end()) { r.atoms = it->second; counts.erase(it); }
		}
		for (const auto& kv : counts)
		{
			Row r;
			r.ci.key.x = kv.first.first; r.ci.key.z = kv.first.second;
			r.ci.loaded = true;   // resident in the live world
			r.atoms = kv.second;
			rows.push_back(r);
		}
	}
	if (rows.empty()) return;

	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImFont* font = ImGui::GetFont();
	int nLoaded = 0, nParked = 0, nCold = 0, nLoading = 0, nHlod = 0;
	uint64_t parkedB = 0, diskB = 0;
	for (const Row& row : rows)
	{
		const nuke::WorldStream::CellInfo& ci = row.ci;
		if (ci.loaded) ++nLoaded;
		if (ci.parked) { ++nParked; parkedB += ci.parkedBytes; }
		if (ci.fromFile && !ci.coldLoaded) ++nCold;
		if (ci.loading) ++nLoading;
		if (ci.hlodDraw) ++nHlod;
		diskB += ci.fileBytes;

		const double x0 = ci.key.x * cs, z0 = ci.key.z * cs, x1 = x0 + cs, z1 = z0 + cs;
		ImVec2 p[4];
		bool vis[4];
		vis[0] = toScreen(x0, z0, p[0]); vis[1] = toScreen(x1, z0, p[1]);
		vis[2] = toScreen(x1, z1, p[2]); vis[3] = toScreen(x0, z1, p[3]);

		// State color (the fill stays translucent so the world reads through).
		ImU32 col;
		const char* state;
		if (ci.loading)                          { col = IM_COL32(255, 160,  40, 255); state = "loading"; }
		else if (ci.parked)                      { col = IM_COL32(235, 210,  60, 255); state = "parked"; }
		else if (ci.loaded)                      { col = IM_COL32( 90, 220, 110, 255); state = "loaded"; }
		else if (ci.fromFile && !ci.coldLoaded)  { col = IM_COL32( 90, 150, 255, 255); state = "cold"; }
		else                                     { col = IM_COL32(160, 160, 160, 255); state = ""; }
		const ImU32 fill = (col & 0x00FFFFFF) | (0x20u << 24);

		if (vis[0] && vis[1] && vis[2] && vis[3])
		{
			dl->AddQuadFilled(p[0], p[1], p[2], p[3], fill);
			dl->AddQuad(p[0], p[1], p[2], p[3], col, 2.0f);
			if (ci.hlodDraw)   // inset magenta ring: the proxy stands in for this cell right now
			{
				ImVec2 c4((p[0].x + p[2].x) * 0.5f, (p[0].y + p[2].y) * 0.5f);
				ImVec2 q[4];
				for (int i = 0; i < 4; ++i) q[i] = ImVec2(p[i].x + (c4.x - p[i].x) * 0.12f, p[i].y + (c4.y - p[i].y) * 0.12f);
				dl->AddQuad(q[0], q[1], q[2], q[3], IM_COL32(230, 90, 230, 220), 1.5f);
			}
			// Label when the cell has real screen estate.
			const float dx = p[2].x - p[0].x, dy = p[2].y - p[0].y;
			if (dx * dx + dy * dy > 70.0f * 70.0f)
			{
				char size[32] = "";
				uint64_t bytes = ci.parked ? ci.parkedBytes : ci.fileBytes;
				if (bytes) fmtBytes(bytes, size, sizeof(size));
				char atoms[24] = "";
				if (row.atoms) std::snprintf(atoms, sizeof(atoms), "  %d atom%s", row.atoms, row.atoms == 1 ? "" : "s");
				char label[120];
				std::snprintf(label, sizeof(label), "%d_%d%s%s%s%s%s%s", ci.key.x, ci.key.z,
				              *state ? "  " : "", state, ci.hlodDraw ? " +hlod" : "",
				              *size ? "  " : "", size, atoms);
				ImVec2 c4((p[0].x + p[2].x) * 0.5f, (p[0].y + p[2].y) * 0.5f);
				ImVec2 ts = font->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, label);
				ImVec2 tp(c4.x - ts.x * 0.5f, c4.y - ts.y * 0.5f);
				dl->AddText(font, ImGui::GetFontSize(), ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 200), label);
				dl->AddText(font, ImGui::GetFontSize(), tp, col, label);
			}
		}
		else
		{
			// The camera stands inside/over this cell: draw whichever edges are still projectable
			// so the current cell never just vanishes from the overlay.
			static const int e[4][2] = { {0, 1}, {1, 2}, {2, 3}, {3, 0} };
			for (int i = 0; i < 4; ++i)
				if (vis[e[i][0]] && vis[e[i][1]])
					dl->AddLine(p[e[i][0]], p[e[i][1]], col, 2.0f);
		}
	}

	// Summary (top-left of the viewport image).
	{
		char pb[32], db[32];
		fmtBytes(parkedB, pb, sizeof(pb));
		fmtBytes(diskB, db, sizeof(db));
		char sum[256];
		std::snprintf(sum, sizeof(sum),
		              "Streaming: %d cells | %d loaded | %d parked (%s) | %d cold | %d loading | %d HLOD | disk %s | cell %.0f m%s",
		              (int)rows.size(), nLoaded, nParked, pb, nCold, nLoading, nHlod, db, cs,
		              nuke::WorldStream::Active(w) ? "" : "  [edit mode: prospective partition]");
		ImVec2 ts = font->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, sum);
		ImVec2 tp(rmin.x + 8.0f, rmin.y + 8.0f);
		dl->AddRectFilled(ImVec2(tp.x - 4, tp.y - 2), ImVec2(tp.x + ts.x + 4, tp.y + ts.y + 2), IM_COL32(0, 0, 0, 140), 3.0f);
		dl->AddText(font, ImGui::GetFontSize(), tp, IM_COL32(235, 235, 235, 255), sum);
	}
}

void EditorUI::winRender()
{
	if (!win->render) return;
	NukeUI::DocPanel("panel:render", "Render", &win->render, window_flags, 960, 620, [this]()
	{
	// Locked while booting: gizmos/picking would operate on half a world.
	if (bootLoading) { ImGui::TextDisabled("Loading project..."); return; }

	if (camFocusing && editorCam && editorCam->transform)
	{
		ImGuiIO& fio = ImGui::GetIO();
		if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle) || fio.MouseWheel != 0.0f)
			camFocusing = false;                       // any manual camera input cancels the fly-to
		else
		{
			Transform* c = editorCam->transform;
			if ((camFocusTarget - c->position).abs() < 1e-3) { c->position = camFocusTarget; camFocusing = false; }
			else c->position = Math::Lerp(c->position, camFocusTarget, 1.0 - std::exp(-12.0 * fio.DeltaTime));   // frame-rate-independent ease
		}
	}

	// Hotkeys fire on focus OR hover (matching the hover-based WASD camera), never while typing
	// in a field or flying with RMB held. Q/W/E/R = tools, X = World/Local, F = frame, Del.
	if ((ImGui::IsWindowFocused() || ImGui::IsWindowHovered()) && !ImGui::GetIO().WantTextInput && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
	{
		AppInstance* a = AppInstance::GetSingleton();
		if (ImGui::IsKeyPressed(ImGuiKey_Q)) a->manipulationMode = 0;
		if (ImGui::IsKeyPressed(ImGuiKey_W)) a->manipulationMode = 1;
		if (ImGui::IsKeyPressed(ImGuiKey_E)) a->manipulationMode = 2;
		if (ImGui::IsKeyPressed(ImGuiKey_R)) a->manipulationMode = 3;
		if (ImGui::IsKeyPressed(ImGuiKey_X)) a->manipulationWorld = !a->manipulationWorld;
		if (ImGui::IsKeyPressed(ImGuiKey_F)) FocusSelected();
		nuke::Hotkeys* hk = nuke::Hotkeys::Get();
		nuke::Hotkey* d  = hk->Find("editor.delete");
		nuke::Hotkey* df = hk->Find("editor.delete.force");
		if ((d  && d->bound  && ImGui::IsKeyChordPressed((ImGuiKeyChord)d->chord)) ||
		    (df && df->bound && ImGui::IsKeyChordPressed((ImGuiKeyChord)df->chord)))
			DeleteSelectedAtom();
		auto chord = [&](const char* id) { nuke::Hotkey* h = hk->Find(id); return h && h->bound && ImGui::IsKeyChordPressed((ImGuiKeyChord)h->chord); };
		if (chord("editor.copy"))      CopySelectedAtom();
		if (chord("editor.cut"))       CutSelectedAtom();
		if (chord("editor.paste"))     PasteAtom();
		if (chord("editor.duplicate")) DuplicateSelectedAtom();
	}

	ImVec2 avail = ImGui::GetContentRegionAvail();
	iRender* r = AppInstance::GetSingleton()->render;
	if (r && avail.x >= 1.0f && avail.y >= 1.0f)
	{
		if (sceneRTId == 0)
		{
			sceneRTId = r->createRenderTarget((int)avail.x, (int)avail.y);
			if (editorCam) editorCam->renderTarget = sceneRTId;
		}
		else
		{
			r->resizeRenderTarget(sceneRTId, (int)avail.x, (int)avail.y);
		}
		nuke::Screen::Set((int)avail.x, (int)avail.y);   // the editor's "game screen" = the viewport panel

		// PIE possess: pick the camera driving the viewport RT. Re-resolved EVERY frame from the live
		// world — scripts spawn/destroy cameras and Stop reloads it, so never cache a Camera*.
		nuke::Camera* driveCam = editorCam;
		{
			AppInstance* papp = AppInstance::GetSingleton();
			if (papp->playState != 0 && !pieUseEditorCam && papp->currentWorld)
				if (nuke::Camera* mc = papp->currentWorld->GetMainCamera())
					if (mc->targetTexGuid.empty())   // a RenderTexture camera keeps its own target
						driveCam = mc;
			// Exactly ONE camera may own the viewport RT — strip it from every other camera.
			std::function<void(bc::list<nuke::Atom*>&)> strip = [&](bc::list<nuke::Atom*>& gos)
			{
				for (nuke::Atom* at : gos)
				{
					if (!at) continue;
					if (nuke::Camera* c = at->GetComponent<nuke::Camera>())
						if (c != driveCam && c->renderTarget == sceneRTId) c->renderTarget = 0;
					strip(at->children);
				}
			};
			if (papp->currentWorld) strip(papp->currentWorld->GetHierarchy());
			if (driveCam) driveCam->renderTarget = sceneRTId;
		}
		const bool possessed = (driveCam != editorCam);
		uint64_t tex = r->getRenderTargetTexture(sceneRTId);
		if (tex)
		{
			ImGui::Image((ImTextureID)tex, avail);
			// Drop: material/texture applies to the atom under the cursor, anything else spawns
			// AT THE CURSOR — the drop point is where the object belongs, not the world origin.
			if (ImGui::BeginDragDropTarget())
			{
				const ImVec2 dropMin = ImGui::GetItemRectMin(), dropSz = ImGui::GetItemRectSize();
				// AcceptBeforeDelivery gives the payload while the mouse is still down, which is
				// what makes a live preview possible.
				if (const ImGuiPayload* dp = ImGui::AcceptDragDropPayload("NUKE_ASSET",
				                                 ImGuiDragDropFlags_AcceptBeforeDelivery))
				{
					std::string dpath((const char*)dp->Data), dext;
					size_t dot = dpath.find_last_of('.');
					if (dot != std::string::npos) dext = dpath.substr(dot);
					std::transform(dext.begin(), dext.end(), dext.begin(), ::tolower);
					const bool onAtom = (dext == ".numat" || dext == ".nutex");
					nuke::Vector3 dropPos;
					const bool spawnable = !onAtom && (dext == ".nuprefab" || dext == ".numesh");
					if (spawnable) dropPos = DropPointAt(driveCam, dropMin, dropSz, ImGui::GetMousePos());
					if (!dp->IsDelivery())
					{
						// Hovering: show WHERE it will land (wire box on the surface under the cursor).
						if (spawnable) DrawDropPreview(dropPos);
					}
					else if (onAtom)
					{
						// Must pick with the camera that ACTUALLY renders the image (game cam while possessed).
						if (nuke::Atom* hit = PickAtScreen(driveCam, dropMin, dropSz, ImGui::GetMousePos()))
							DropAssetOnAtom(hit, dpath);
					}
					else if (nuke::Atom* spawned = DropAsset(dpath))
					{
						if (spawnable)
						{
							nuke::Transform& st = spawned->GetTransform();
							st.SetGlobal(dropPos, st.globalRotation(), st.globalScale());
						}
					}
				}
				ImGui::EndDragDropTarget();
			}
			// Runtime GUI target + input rect. uiX/uiY are compared against GLFW coords relative to
			// the main window's client area, so the main viewport origin MUST be subtracted here.
			ImVec2 imin = ImGui::GetItemRectMin();
			ImVec2 vpos = ImGui::GetMainViewport()->Pos;
			AppInstance* app = AppInstance::GetSingleton();
			app->uiTarget = sceneRTId;
			app->uiX = (int)(imin.x - vpos.x); app->uiY = (int)(imin.y - vpos.y);
			app->uiW = (int)avail.x; app->uiH = (int)avail.y;

			// Entity icons must draw before the camera preview and the gizmo (edit mode only).
			if (!possessed) DrawEntityIcons(imin, avail);
			else            iconHits.clear();   // no stale clickable rects from the edit view
			// Streaming overlay: edit AND play (streaming actually runs in play — that is the
			// interesting view); hidden while possessed (the game owns the screen then).
			if (!possessed && streamVizVisible) DrawStreamCells(imin, avail);
			if (!possessed) DrawModuleOverlays(imin, avail);
		}
		else
			ImGui::Text("No scene texture.");

		// Selected-camera preview overlay (bottom-right of the viewport). Last frame's camera
		// resolves by STABLE atom id — stream parking may have deleted it since.
		if (previewCam)
		{
			nuke::World* pw = AppInstance::GetSingleton()->currentWorld;
			nuke::Atom* pa = pw ? pw->GetById(previewCamAtomId) : nullptr;
			if (nuke::Camera* pc = pa ? pa->GetComponent<nuke::Camera>() : nullptr) pc->renderTarget = 0;
			previewCam = nullptr; previewCamAtomId = 0;
		}
		{
			Atom* sel = AppInstance::GetSingleton()->selectedInHieararchy;
			Camera* selCam = sel ? sel->GetComponent<Camera>() : nullptr;
			if (selCam && !selCam->enabled) selCam = nullptr;   // disabled camera renders nothing
			if (tex && selCam && selCam != editorCam && selCam != driveCam)   // the driving camera IS the big image
			{
				uint64_t ptex = 0;
				if (!selCam->targetTexGuid.empty())
				{
					// RenderTexture camera already owns an RT — preview it directly; reassigning
					// renderTarget here would steal the target from the RenderTexture.
					if (nuke::Texture* rtx = ResDB::getSingleton()->GetTexture(selCam->targetTexGuid))
						if (rtx->rtId) ptex = r->getRenderTargetTexture(rtx->rtId);
					previewCam = nullptr;
				}
				else
				{
					if (camPreviewRT == 0) camPreviewRT = r->createRenderTarget(256, 144);
					selCam->renderTarget = camPreviewRT;   // World::Render draws it here next pass
					previewCam = selCam;
					previewCamAtomId = (long)sel->id.id;
					ptex = r->getRenderTargetTexture(camPreviewRT);
				}

				ImVec2 imax = ImGui::GetItemRectMax();
				ImVec2 pv(256, 144), pad(12, 12);
				ImVec2 p0(imax.x - pv.x - pad.x, imax.y - pv.y - pad.y);
				ImVec2 p1(p0.x + pv.x, p0.y + pv.y);
				ImDrawList* dl = ImGui::GetWindowDrawList();
				dl->AddRectFilled(ImVec2(p0.x - 2, p0.y - 16), ImVec2(p1.x + 2, p1.y + 2), IM_COL32(15, 15, 15, 220));
				if (ptex)
					dl->AddImage((ImTextureID)ptex, p0, p1);
				dl->AddRect(p0, p1, IM_COL32(180, 180, 180, 255));
				dl->AddText(ImVec2(p0.x + 3, p0.y - 15), IM_COL32_WHITE, sel->GetName().c_str());
			}
		}

		// Curve control-point handles (WaterRiver + Spline): LMB-drag moves a point (Shift =
		// world XZ plane), Ctrl+Click deletes it or appends near the curve. Must run BEFORE the
		// transform gizmo so a hot handle can mute it; the whole gesture is one undo entry.
		s_riverGizmoHot = false;
		{
			AppInstance* wapp = AppInstance::GetSingleton();
			Atom* wsel = wapp->selectedInHieararchy;
			// Candidates: the engine Spline + module types from the EditorHooks curve registry —
			// matched by type-name CONTENT (pointer compare fails across DLLs), `points` reached
			// through reflection so the undo path stays uniform.
			nuke::Component* riv = nullptr;
			const char* rvType = nullptr;
			std::vector<float>* rvPts = nullptr;
			if (wsel && wapp->playState == 0)
				for (nuke::Component* c : wsel->components)
				{
					if (!c || !c->name) continue;
					if (!strcmp(c->name, "Spline")) { riv = c; rvType = "Spline"; break; }
					if (!strcmp(c->name, "Rope"))   { riv = c; rvType = "Rope"; break; }
					for (const std::string& cc : nuke::CurveComponents())
						if (!strcmp(c->name, cc.c_str())) { riv = c; rvType = cc.c_str(); break; }
					if (riv) break;
				}
			if (riv)
				if (nuke::TypeInfo* rti = nuke::Registry_Find(rvType))
					for (nuke::Field& rf : rti->fields)
						if (rf.name == "points" && rf.type == nuke::FT::FloatList && rf.addr)
							{ rvPts = (std::vector<float>*)rf.addr(riv); break; }
			// Engine spline extras: exact resampled polyline for the near-curve test, structured
			// add/remove (Bezier keeps its anchor,handle,handle layout), atom scale applies.
			nuke::Spline* rvSp = riv && !strcmp(rvType, "Spline") ? dynamic_cast<nuke::Spline*>(riv) : nullptr;
			const bool rvBezier = rvSp && rvSp->type == 1;
			// The stored component pointer is an identity check: a selection/world change mid-drag
			// must abort instead of writing into a different river.
			static bool rvDragging = false; static int rvDragIdx = -1; static void* rvDragRiv = nullptr;
			static std::vector<float> rvBefore;    // the whole vector at drag start (undo capture)
			static Vector3 rvAnchor(0, 0, 0);      // dragged point's world pos at grab (plane anchor)
			if (!possessed && riv && rvPts && riv->transform && editorCam && editorCam->transform)
			{
				ImVec2 rmin = ImGui::GetItemRectMin();
				ImVec2 vsz  = ImGui::GetItemRectSize();
				Transform* gcam = editorCam->transform;
				Vector3 ge = gcam->globalPosition();
				Vector3 gf = gcam->direction(), gu = gcam->up(), gr = gcam->right();
				float aspect = (vsz.y > 0.0f) ? vsz.x / vsz.y : 1.0f;
				glm::mat4 gv = glm::lookAtLH(
					glm::vec3((float)ge.x, (float)ge.y, (float)ge.z),
					glm::vec3((float)(ge.x + gf.x), (float)(ge.y + gf.y), (float)(ge.z + gf.z)),
					glm::vec3((float)gu.x, (float)gu.y, (float)gu.z));
				glm::mat4 gp = EditorCamProj(editorCam, aspect);
				auto toScreen = [&](const Vector3& w, ImVec2& out) -> bool
				{
					glm::vec4 c = gp * gv * glm::vec4((float)w.x, (float)w.y, (float)w.z, 1.0f);
					if (c.w <= 1e-4f) return false;
					out = ImVec2(rmin.x + (c.x / c.w * 0.5f + 0.5f) * vsz.x,
					             rmin.y + (0.5f - c.y / c.w * 0.5f) * vsz.y);
					return true;
				};
				// Mouse ray must use the SAME view/branches as PickAtScreen.
				ImVec2 mp = ImGui::GetIO().MousePos;
				auto mouseRay = [&](Vector3& ro, Vector3& rdir)
				{
					float ndcx = ((mp.x - rmin.x) / vsz.x) * 2.0f - 1.0f;
					float ndcy = 1.0f - ((mp.y - rmin.y) / vsz.y) * 2.0f;
					ro = ge; rdir = gf;
					if (editorCam->projBlend >= 0.5f)
					{
						float oh = (editorCam->orthoSize > 1e-4f) ? editorCam->orthoSize : 1.0f, ow = oh * aspect;
						ro = Vector3(ge.x + ndcx * ow * gr.x + ndcy * oh * gu.x,
						             ge.y + ndcx * ow * gr.y + ndcy * oh * gu.y,
						             ge.z + ndcx * ow * gr.z + ndcy * oh * gu.z);
					}
					else
					{
						float thf = tanf((float)editorCam->fov * 0.5f * 0.01745329252f);
						rdir = Vector3(gf.x + ndcx * thf * aspect * gr.x + ndcy * thf * gu.x,
						               gf.y + ndcx * thf * aspect * gr.y + ndcy * thf * gu.y,
						               gf.z + ndcx * thf * aspect * gr.z + ndcy * thf * gu.z);
					}
				};

				// Control points atom-local -> world. Rivers ignore atom scale (as does their
				// Rebuild); Spline consumers build in scaled local space, so its handles apply it.
				Vector3 P = riv->transform->globalPosition();
				Quaternion Q = riv->transform->globalRotation();
				Quaternion Qc(-Q.x, -Q.y, -Q.z, Q.w);   // conjugate: world -> atom-local
				Vector3 S = rvSp ? riv->transform->globalScale() : Vector3(1, 1, 1);
				auto toWorldPt = [&](const Vector3& lp)
				{
					Vector3 rl = Q.Rotate(Vector3(lp.x * S.x, lp.y * S.y, lp.z * S.z));
					return Vector3(P.x + rl.x, P.y + rl.y, P.z + rl.z);
				};
				auto toLocalPt = [&](const Vector3& w)
				{
					Vector3 lp = Qc.Rotate(Vector3(w.x - P.x, w.y - P.y, w.z - P.z));
					return Vector3(std::fabs(S.x) > 1e-9 ? lp.x / S.x : 0.0,
					               std::fabs(S.y) > 1e-9 ? lp.y / S.y : 0.0,
					               std::fabs(S.z) > 1e-9 ? lp.z / S.z : 0.0);
				};
				const int n = (int)(rvPts->size() / 3);
				std::vector<ImVec2>  hpos(n);
				std::vector<Vector3> wpos(n);
				for (int i = 0; i < n; ++i)
				{
					Vector3 lp((*rvPts)[i * 3], (*rvPts)[i * 3 + 1], (*rvPts)[i * 3 + 2]);
					wpos[i] = toWorldPt(lp);
					if (!toScreen(wpos[i], hpos[i])) hpos[i] = ImVec2(-10000, -10000);
				}
				int hover = -1;
				if (ImGui::IsItemHovered() && !ImGuizmo::IsUsing())
					for (int i = 0; i < n; ++i)
					{
						float dx = mp.x - hpos[i].x, dy = mp.y - hpos[i].y;
						if (dx * dx + dy * dy < 8.0f * 8.0f) { hover = i; break; }
					}

				// One undo entry per gesture, resolved at undo time by atom id + type name +
				// reflected field: component pointers dangle across undo-recreated atoms.
				long aid = wsel->id.id;
				std::string rvTypeName = rvType;
				auto pushPointsUndo = [&](const std::vector<float>& before, const std::vector<float>& after)
				{
					if (before == after) return;
					auto set = [rvTypeName](long id, const std::vector<float>& v)
					{
						World* w = AppInstance::GetSingleton()->currentWorld;
						Atom* a = w ? w->GetById(id) : nullptr;
						nuke::TypeInfo* ti = a ? nuke::Registry_Find(rvTypeName.c_str()) : nullptr;
						if (!ti) return;
						for (nuke::Component* c : a->components)
							if (c && c->name && !strcmp(c->name, rvTypeName.c_str()))
								for (nuke::Field& f : ti->fields)
									if (f.name == "points" && f.type == nuke::FT::FloatList && f.addr)
										{ *(std::vector<float>*)f.addr(c) = v; return; }
					};
					PushUndo("Edit curve points",
						[set, aid, before]{ set(aid, before); },
						[set, aid, after ]{ set(aid, after ); });
					// TrackUndo must not ALSO record this gesture: drop its in-progress edit and
					// adopt the post-edit state as the idle baseline.
					editing = false; editAtomId = 0;
					idleAtomId = aid; idleSnap = SaveAtomToString(wsel); idleSnapValid = true;
				};

				bool rvClickDone = false;
				if (!rvDragging && ImGui::IsItemHovered() && !ImGuizmo::IsUsing() &&
				    ImGui::GetIO().KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				{
					if (hover >= 0 && n > 2)
					{
						std::vector<float> before = *rvPts;
						if (rvSp)
							rvSp->RemovePoint(hover);   // structured: Bezier drops anchor+handles, keeps minimums
						else if (n > 2)                 // river: keep at least 2 — below that there is no river
							rvPts->erase(rvPts->begin() + hover * 3, rvPts->begin() + hover * 3 + 3);
						pushPointsUndo(before, *rvPts);
						hover = -1; rvClickDone = true;
					}
					else if (hover < 0 && n >= 2)
					{
						bool nearSpline = false;
						if (rvSp)
						{
							// Exact: test against the component's own resampled polyline.
							const std::vector<nuke::SplineSample>& ss = rvSp->LocalSamples();
							for (size_t si = 0; si < ss.size() && !nearSpline; ++si)
							{
								ImVec2 sp;
								if (!toScreen(toWorldPt(Vector3(ss[si].p[0], ss[si].p[1], ss[si].p[2])), sp)) continue;
								float dx = mp.x - sp.x, dy = mp.y - sp.y;
								nearSpline = (dx * dx + dy * dy < 14.0f * 14.0f);
							}
						}
						else
						{
							// Hit-test against the same Catmull-Rom the water module draws (coarse
							// subdivision is enough for click precision).
							auto cpAt = [&](int i) -> const Vector3& { return wpos[std::min(std::max(i, 0), n - 1)]; };
							for (int seg = 0; seg + 1 < n && !nearSpline; ++seg)
							{
								const Vector3& c0 = cpAt(seg - 1); const Vector3& c1 = cpAt(seg);
								const Vector3& c2 = cpAt(seg + 1); const Vector3& c3 = cpAt(seg + 2);
								for (int s2 = 0; s2 <= 8 && !nearSpline; ++s2)
								{
									const double u = s2 / 8.0, u2 = u * u, u3 = u2 * u;
									Vector3 cp(0.5 * (2.0 * c1.x + (-c0.x + c2.x) * u + (2.0 * c0.x - 5.0 * c1.x + 4.0 * c2.x - c3.x) * u2 + (-c0.x + 3.0 * c1.x - 3.0 * c2.x + c3.x) * u3),
									           0.5 * (2.0 * c1.y + (-c0.y + c2.y) * u + (2.0 * c0.y - 5.0 * c1.y + 4.0 * c2.y - c3.y) * u2 + (-c0.y + 3.0 * c1.y - 3.0 * c2.y + c3.y) * u3),
									           0.5 * (2.0 * c1.z + (-c0.z + c2.z) * u + (2.0 * c0.z - 5.0 * c1.z + 4.0 * c2.z - c3.z) * u2 + (-c0.z + 3.0 * c1.z - 3.0 * c2.z + c3.z) * u3));
									ImVec2 sp;
									if (!toScreen(cp, sp)) continue;
									float dx = mp.x - sp.x, dy = mp.y - sp.y;
									nearSpline = (dx * dx + dy * dy < 14.0f * 14.0f);
								}
							}
						}
						if (nearSpline)
						{
							// Append on the horizontal plane through the LAST point.
							Vector3 ro, rd; mouseRay(ro, rd);
							if (fabs(rd.y) > 1e-8)
							{
								double tHit = (wpos[n - 1].y - ro.y) / rd.y;
								if (tHit > 0.0)
								{
									Vector3 hit(ro.x + rd.x * tHit, ro.y + rd.y * tHit, ro.z + rd.z * tHit);
									Vector3 lp = toLocalPt(hit);
									std::vector<float> before = *rvPts;
									if (rvSp)
										rvSp->AddPoint(lp);   // Bezier appends handle,handle,anchor
									else
									{
										rvPts->push_back((float)lp.x);
										rvPts->push_back((float)lp.y);
										rvPts->push_back((float)lp.z);
									}
									pushPointsUndo(before, *rvPts);
									rvClickDone = true;
								}
							}
						}
					}
				}

				if (!rvDragging && !rvClickDone && hover >= 0 && !ImGui::GetIO().KeyCtrl &&
				    ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				{
					rvDragging = true; rvDragIdx = hover; rvDragRiv = riv;
					rvBefore = *rvPts; rvAnchor = wpos[hover];
				}
				if (rvDragging && rvDragRiv != (void*)riv)
				{ rvDragging = false; rvDragIdx = -1; rvDragRiv = nullptr; }   // target gone: abort
				if (rvDragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
				{
					pushPointsUndo(rvBefore, *rvPts);
					rvDragging = false; rvDragIdx = -1; rvDragRiv = nullptr;
				}
				if (rvDragging && rvDragIdx >= 0 && rvDragIdx < n)
				{
					// Drag plane faces the camera through the grab position; Shift = world XZ.
					Vector3 ro, rd; mouseRay(ro, rd);
					Vector3 pn = ImGui::GetIO().KeyShift ? Vector3(0, 1, 0) : gf;
					double denom = rd.x * pn.x + rd.y * pn.y + rd.z * pn.z;
					if (fabs(denom) > 1e-8)
					{
						double tHit = ((rvAnchor.x - ro.x) * pn.x + (rvAnchor.y - ro.y) * pn.y + (rvAnchor.z - ro.z) * pn.z) / denom;
						if (tHit > 0.0)
						{
							Vector3 hit(ro.x + rd.x * tHit, ro.y + rd.y * tHit, ro.z + rd.z * tHit);
							Vector3 lp = toLocalPt(hit);
							(*rvPts)[rvDragIdx * 3 + 0] = (float)lp.x;
							(*rvPts)[rvDragIdx * 3 + 1] = (float)lp.y;
							(*rvPts)[rvDragIdx * 3 + 2] = (float)lp.z;
							// Bezier anchor carries its handles: shift them by the anchor's delta
							// from the drag-start snapshot so the span's shape survives the move.
							if (rvBezier && rvDragIdx % 3 == 0 && rvBefore.size() == rvPts->size())
							{
								const float dx = (*rvPts)[rvDragIdx * 3]     - rvBefore[rvDragIdx * 3];
								const float dy = (*rvPts)[rvDragIdx * 3 + 1] - rvBefore[rvDragIdx * 3 + 1];
								const float dz = (*rvPts)[rvDragIdx * 3 + 2] - rvBefore[rvDragIdx * 3 + 2];
								for (int hb = rvDragIdx - 1; hb <= rvDragIdx + 1; hb += 2)
									if (hb >= 0 && hb < n && hb % 3 != 0)
									{
										(*rvPts)[hb * 3]     = rvBefore[hb * 3] + dx;
										(*rvPts)[hb * 3 + 1] = rvBefore[hb * 3 + 1] + dy;
										(*rvPts)[hb * 3 + 2] = rvBefore[hb * 3 + 2] + dz;
									}
							}
						}
					}
				}
				s_riverGizmoHot = (hover >= 0) || rvDragging || rvClickDone;

				ImDrawList* dl = ImGui::GetWindowDrawList();
				for (int i = 0; i < n; ++i)
				{
					if (hpos[i].x < -999.0f) continue;
					const bool hot = (i == hover) || (rvDragging && i == rvDragIdx);
					const bool handle = rvBezier && (i % 3) != 0;   // Bezier tangent handle, not an anchor
					const float rad = hot ? 7.0f : (handle ? 4.5f : 6.0f);
					dl->AddCircleFilled(hpos[i], rad, hot ? IM_COL32(255, 200, 60, 255)
					                    : (handle ? IM_COL32(140, 220, 255, 210) : IM_COL32(80, 180, 255, 230)));
					dl->AddCircle(hpos[i], rad, IM_COL32(20, 20, 20, 255), 0, 1.5f);
				}
			}
			else if (rvDragging)
			{ rvDragging = false; rvDragIdx = -1; rvDragRiv = nullptr; }   // context gone (PIE/possess/deselect): abort
		}

		// The world grid is emitted ENGINE-SIDE in World::Render (in-frame with the camera —
		// lines pushed from here land a frame late and shimmer); the editor only hands over
		// the step. 0 = hidden.
		AppInstance::GetSingleton()->editorGridStep =
			(!possessed && gridVisible) ? std::max(0.01f, snapMove) : 0.0f;

		// Viewport tool feed (abi 16): cursor ray + stroke state for MODULE viewport tools
		// (terrain brushes...). Same ray math as PickAtScreen. The undo seam installs here —
		// module tools push their strokes onto the SAME editor command stack.
		{
			AppInstance* fapp = AppInstance::GetSingleton();
			if (fapp->editorUndoHook.empty())
				fapp->editorUndoHook = [this](const std::string& label,
				                              boost::function<void()> u, boost::function<void()> r)
				{ PushUndo(label, u, r, true); };
			ImVec2 fmin = ImGui::GetItemRectMin(), fsz = ImGui::GetItemRectSize();
			ImVec2 fmp  = ImGui::GetMousePos();
			const bool inRect = fsz.x > 0.0f && fsz.y > 0.0f
			               && fmp.x >= fmin.x && fmp.y >= fmin.y
			               && fmp.x < fmin.x + fsz.x && fmp.y < fmin.y + fsz.y
			               && ImGui::IsWindowHovered();
			// Game pointer gate (reset each frame in the toolbar): clicks feed gameplay input
			// only over the game view — or whenever the game owns the cursor (hidden/locked),
			// where the mouse position is meaningless for hover tests.
			fapp->gamePointerActive = inRect || (fapp->render && fapp->render->getCursorMode() != 0);
			const bool over = !possessed && editorCam && editorCam->transform
			               && inRect && !ImGuizmo::IsUsing() && !s_marqueeLive;
			fapp->editorRayValid = over;
			if (over)
			{
				Transform* ft = editorCam->transform;
				const float ndcx = ((fmp.x - fmin.x) / fsz.x) * 2.0f - 1.0f;
				const float ndcy = 1.0f - ((fmp.y - fmin.y) / fsz.y) * 2.0f;
				Vector3 fo = ft->globalPosition();
				Vector3 ff = ft->direction(), frr = ft->right(), fuu = ft->up();
				const float faspect = fsz.x / fsz.y;
				Vector3 fdir = ff;
				if (editorCam->projBlend >= 0.5f)   // ortho: the origin slides, rays stay parallel
				{
					const float halfH = (editorCam->orthoSize > 1e-4f) ? editorCam->orthoSize : 1.0f;
					const float halfW = halfH * faspect;
					fo = Vector3(fo.x + ndcx * halfW * frr.x + ndcy * halfH * fuu.x,
					             fo.y + ndcx * halfW * frr.y + ndcy * halfH * fuu.y,
					             fo.z + ndcx * halfW * frr.z + ndcy * halfH * fuu.z);
				}
				else
				{
					const float thf = tanf((float)editorCam->fov * 0.5f * 0.01745329252f);
					fdir = Vector3(ff.x + ndcx * thf * faspect * frr.x + ndcy * thf * fuu.x,
					               ff.y + ndcx * thf * faspect * frr.y + ndcy * thf * fuu.y,
					               ff.z + ndcx * thf * faspect * frr.z + ndcy * thf * fuu.z);
				}
				const float fl = sqrtf(fdir.x * fdir.x + fdir.y * fdir.y + fdir.z * fdir.z);
				fapp->editorRayOrigin[0] = (float)fo.x; fapp->editorRayOrigin[1] = (float)fo.y;
				fapp->editorRayOrigin[2] = (float)fo.z;
				fapp->editorRayDir[0] = (float)(fdir.x / fl); fapp->editorRayDir[1] = (float)(fdir.y / fl);
				fapp->editorRayDir[2] = (float)(fdir.z / fl);
			}
			fapp->editorMouseWentDown = over && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
			fapp->editorMouseDown     = over && ImGui::IsMouseDown(ImGuiMouseButton_Left);
		}

		// Transform gizmo over the selected object (only when a manip tool is active).
		{
			AppInstance* gapp = AppInstance::GetSingleton();
			Atom* gsel = gapp->selectedInHieararchy;
			// folders are pure organization — no gizmo, their transform stays identity.
			if (!possessed && gsel && !gsel->folder && editorCam && gapp->manipulationMode != 0)
			{
				ImGuizmo::SetOrthographic(false);
				ImGuizmo::SetDrawlist();
				ImVec2 grmin = ImGui::GetItemRectMin();
				ImVec2 gsz   = ImGui::GetItemRectSize();
				ImGuizmo::SetRect(grmin.x, grmin.y, gsz.x, gsz.y);

				// ImGuizmo needs the renderer's EXACT view/proj (LH, depth 0..1) — per-axis
				// scale needs an exact ray, and handedness detection depends on it.
				float gview[16], gproj[16];
				{
					Transform* gcam = editorCam->transform;
					Vector3 ge = gcam->globalPosition();
					Vector3 gf = gcam->direction(), gu = gcam->up();
					float gaspect = (gsz.y > 0.0f) ? gsz.x / gsz.y : 1.0f;
					glm::mat4 gv = glm::lookAtLH(
						glm::vec3((float)ge.x, (float)ge.y, (float)ge.z),
						glm::vec3((float)(ge.x + gf.x), (float)(ge.y + gf.y), (float)(ge.z + gf.z)),
						glm::vec3((float)gu.x, (float)gu.y, (float)gu.z));
					glm::mat4 gp = EditorCamProj(editorCam, gaspect);
					memcpy(gview, glm::value_ptr(gv), sizeof(gview));   // glm passed directly (no transpose)
					memcpy(gproj, glm::value_ptr(gp), sizeof(gproj));
				}

				Transform& gtt = gsel->GetTransform();
				if (!ImGuizmo::IsUsing())   // resync from the object only when NOT dragging
				{
					// Gizmo model must come from the GLOBAL transform (as the render/outline do) or a
					// parented atom's gizmo lands at local coords; the write-back below inverts it.
					Vector3 gP = gtt.globalPosition(); Quaternion gR = gtt.globalRotation(); Vector3 gS = gtt.globalScale();
					glm::mat4 gm = glm::translate(glm::mat4(1.0f), glm::vec3((float)gP.x, (float)gP.y, (float)gP.z))
					             * glm::mat4_cast(glm::quat((float)gR.w, (float)gR.x, (float)gR.y, (float)gR.z))
					             * glm::scale(glm::mat4(1.0f), glm::vec3((float)gS.x, (float)gS.y, (float)gS.z));
					memcpy(gizmoMatrix, glm::value_ptr(gm), sizeof(float) * 16);
				}

				ImGuizmo::OPERATION gop = (gapp->manipulationMode == 1) ? ImGuizmo::TRANSLATE
				                        : (gapp->manipulationMode == 2) ? ImGuizmo::ROTATE
				                                                        : ImGuizmo::SCALE;
				ImGuizmo::MODE gmode = (gop != ImGuizmo::SCALE && gapp->manipulationWorld != 0) ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
				// project snap settings drive the increments; holding Ctrl INVERTS the toggle
				// (temporary snap when off, temporary free when on). Rotate/scale snap RELATIVE
				// increments (ImGuizmo); translate snaps ABSOLUTE to the world grid below.
				float gsnapv   = (gop == ImGuizmo::TRANSLATE) ? snapMove : (gop == ImGuizmo::ROTATE) ? snapRot : snapScale;
				float gsnap[3] = { gsnapv, gsnapv, gsnapv };
				const bool snapNow = (snapEnabled != ImGui::GetIO().KeyCtrl) && gsnapv > 0.0f;
				float* gsnapPtr = (snapNow && gop != ImGuizmo::TRANSLATE) ? gsnap : nullptr;
				const glm::mat4 prevM = glm::make_mat4(gizmoMatrix);   // per-atom delta base
				// ImGuizmo::Enable is sticky — restore it right after Manipulate.
				ImGuizmo::Enable(!s_riverGizmoHot);
				ImGuizmo::Manipulate(gview, gproj, gop, gmode, gizmoMatrix, nullptr, gsnapPtr);
				ImGuizmo::Enable(true);

				// multi-drag undo: one composite step for the WHOLE selection (world poses),
				// captured at drag start and pushed on release; the single-atom auto-detector
				// is suppressed while a multi drag is running.
				struct GizPose { long id; Vector3 p; Quaternion r; Vector3 s; };
				static std::vector<GizPose> s_dragBefore;
				static bool s_dragging = false;
				auto poseOf = [](Atom* a) {
					Transform& t = a->GetTransform();
					return GizPose{ (long)a->id.id, t.globalPosition(), t.globalRotation(), t.globalScale() };
				};
				const bool multiDrag = !gapp->selectedExtra.empty();

				if (ImGuizmo::IsUsing())
				{
					if (multiDrag && !s_dragging)
					{
						s_dragging = true;
						s_dragBefore.clear();
						for (Atom* a : gapp->Selection()) if (a && !a->folder) s_dragBefore.push_back(poseOf(a));
					}
					if (multiDrag) { editing = false; editAtomId = 0; }   // composite replaces the detector

					glm::mat4 nm = glm::make_mat4(gizmoMatrix);
					glm::vec3 nS, nT, nSkew; glm::vec4 nPersp; glm::quat nR;
					if (glm::decompose(nm, nS, nR, nT, nSkew, nPersp) &&
					    std::isfinite(nT.x) && std::isfinite(nT.y) && std::isfinite(nT.z) &&
					    std::isfinite(nS.x) && std::isfinite(nS.y) && std::isfinite(nS.z))
					{
						if (nS.x < 1e-3f && nS.x > -1e-3f) nS.x = 1e-3f;
						if (nS.y < 1e-3f && nS.y > -1e-3f) nS.y = 1e-3f;
						if (nS.z < 1e-3f && nS.z > -1e-3f) nS.z = 1e-3f;
						// GLOBAL grid snap: the position quantizes to the WORLD grid (multiples of the
						// step), not to increments from the drag start - objects land ON the lines.
						if (snapNow && gop == ImGuizmo::TRANSLATE)
						{
							nT.x = roundf(nT.x / snapMove) * snapMove;
							nT.y = roundf(nT.y / snapMove) * snapMove;
							nT.z = roundf(nT.z / snapMove) * snapMove;
							gizmoMatrix[12] = nT.x; gizmoMatrix[13] = nT.y; gizmoMatrix[14] = nT.z;
							nm[3][0] = nT.x; nm[3][1] = nT.y; nm[3][2] = nT.z;
						}
						// object-to-object: while V is held during a move, the selection lands on
						// the surface UNDER THE CURSOR (selection hidden from the ray so it can't
						// hit itself), with closest-vertex magnetism on the hit mesh (kit-bashing).
						if (gop == ImGuizmo::TRANSLATE && ImGui::IsKeyDown(ImGuiKey_V))
						{
							std::vector<Atom*> hide = SelectionTopLevel();
							std::vector<char> hwas;
							for (Atom* h : hide) { hwas.push_back(h->enabled ? 1 : 0); h->enabled = false; }
							ImVec2 vmp = ImGui::GetIO().MousePos;
							const float vndx = ((vmp.x - grmin.x) / gsz.x) * 2.0f - 1.0f;
							const float vndy = 1.0f - ((vmp.y - grmin.y) / gsz.y) * 2.0f;
							Transform* vct = editorCam->transform;
							Vector3 vo = vct->globalPosition(), vfw = vct->direction(), vrt = vct->right(), vup = vct->up();
							const float vthf = tanf((float)editorCam->fov * 0.5f * 0.01745329252f);
							const float vasp = (gsz.y > 0.0f) ? gsz.x / gsz.y : 1.0f;
							Vector3 vdir(vfw.x + vndx * vthf * vasp * vrt.x + vndy * vthf * vup.x,
							             vfw.y + vndx * vthf * vasp * vrt.y + vndy * vthf * vup.y,
							             vfw.z + vndx * vthf * vasp * vrt.z + vndy * vthf * vup.z);
							float vdist = 0.0f;
							Atom* vhit = gapp->currentWorld->PickDist(vo, vdir, vdist);
							for (size_t hi = 0; hi < hide.size(); ++hi) hide[hi]->enabled = hwas[hi] != 0;
							if (vhit && vdist > 0.0f)
							{
								const double vlen = std::sqrt(vdir.x * vdir.x + vdir.y * vdir.y + vdir.z * vdir.z);
								Vector3 hp(vo.x + vdir.x / vlen * vdist,
								           vo.y + vdir.y / vlen * vdist,
								           vo.z + vdir.z / vlen * vdist);
								// The nearest mesh vertex within half a grid step beats the raw point.
								if (MeshRenderer* vmr = vhit->GetComponent<MeshRenderer>())
									if (vmr->mesh && vmr->mesh->vertexArray && vmr->mesh->numVerts > 0)
									{
										Transform& ht = vhit->GetTransform();
										Vector3 hP = ht.globalPosition(); Quaternion hR = ht.globalRotation(); Vector3 hS = ht.globalScale();
										glm::mat4 hm = glm::translate(glm::mat4(1.0f), glm::vec3((float)hP.x, (float)hP.y, (float)hP.z))
										             * glm::mat4_cast(glm::quat((float)hR.w, (float)hR.x, (float)hR.y, (float)hR.z))
										             * glm::scale(glm::mat4(1.0f), glm::vec3((float)hS.x, (float)hS.y, (float)hS.z));
										const float magnet = std::max(0.05f, snapMove * 0.5f);
										float bestD2 = magnet * magnet;
										glm::vec3 best(0.0f); bool haveBest = false;
										const int nv = std::min(vmr->mesh->numVerts, 200000);   // editor-side cap
										for (int vi = 0; vi < nv; ++vi)
										{
											glm::vec4 wv = hm * glm::vec4(vmr->mesh->vertexArray[vi * 3],
																	      vmr->mesh->vertexArray[vi * 3 + 1],
																	      vmr->mesh->vertexArray[vi * 3 + 2], 1.0f);
											const float dx = wv.x - (float)hp.x, dy = wv.y - (float)hp.y, dz = wv.z - (float)hp.z;
											const float d2 = dx * dx + dy * dy + dz * dz;
											if (d2 < bestD2) { bestD2 = d2; best = glm::vec3(wv); haveBest = true; }
										}
										if (haveBest) hp = Vector3(best.x, best.y, best.z);
									}
								nT = glm::vec3((float)hp.x, (float)hp.y, (float)hp.z);
								// The gizmo matrix follows, so the extras' delta carries the snap too.
								gizmoMatrix[12] = nT.x; gizmoMatrix[13] = nT.y; gizmoMatrix[14] = nT.z;
								nm[3][0] = nT.x; nm[3][1] = nT.y; nm[3][2] = nT.z;
							}
						}
						gtt.SetGlobal(Vector3(nT.x, nT.y, nT.z),
						              Quaternion(nR.x, nR.y, nR.z, nR.w),
						              Vector3(nS.x, nS.y, nS.z));
						// The gizmo drives the PRIMARY; every other selected atom follows by the
						// same world-space delta (the common pivot is the primary's frame).
						if (multiDrag)
						{
							const glm::mat4 delta = nm * glm::inverse(prevM);
							for (unsigned long id : gapp->selectedExtra)
							{
								Atom* ex = gapp->currentWorld->GetById((long)id);
								if (!ex || ex->folder || ex == gsel) continue;
								bool covered = false;   // a selected ancestor already carries it
								for (Atom* p = ex->parent; p && !covered; p = p->parent)
									if (gapp->IsSelected(p)) covered = true;
								if (covered) continue;
								Transform& et = ex->GetTransform();
								Vector3 eP = et.globalPosition(); Quaternion eR = et.globalRotation(); Vector3 eS = et.globalScale();
								glm::mat4 ew = glm::translate(glm::mat4(1.0f), glm::vec3((float)eP.x, (float)eP.y, (float)eP.z))
								             * glm::mat4_cast(glm::quat((float)eR.w, (float)eR.x, (float)eR.y, (float)eR.z))
								             * glm::scale(glm::mat4(1.0f), glm::vec3((float)eS.x, (float)eS.y, (float)eS.z));
								glm::mat4 nw = delta * ew;
								glm::vec3 wS, wT, wSk; glm::vec4 wPp; glm::quat wR;
								if (glm::decompose(nw, wS, wR, wT, wSk, wPp) &&
								    std::isfinite(wT.x) && std::isfinite(wT.y) && std::isfinite(wT.z))
									if (snapNow && gop == ImGuizmo::TRANSLATE)
									{
										wT.x = roundf(wT.x / snapMove) * snapMove;
										wT.y = roundf(wT.y / snapMove) * snapMove;
										wT.z = roundf(wT.z / snapMove) * snapMove;
									}
									et.SetGlobal(Vector3(wT.x, wT.y, wT.z),
									             Quaternion(wR.x, wR.y, wR.z, wR.w),
									             Vector3(wS.x, wS.y, wS.z));
							}
						}
					}
				}
				else if (s_dragging)
				{
					s_dragging = false;
					std::vector<GizPose> before = s_dragBefore, after;
					for (const GizPose& b : before)
						if (Atom* a = gapp->currentWorld->GetById(b.id)) after.push_back(poseOf(a));
					s_dragBefore.clear();
					editing = false; editAtomId = 0;   // own command: keep the detector out
					auto apply = [](const std::vector<GizPose>& v) {
						World* w = AppInstance::GetSingleton()->currentWorld;
						for (const GizPose& g : v)
							if (Atom* a = w->GetById(g.id)) a->GetTransform().SetGlobal(g.p, g.r, g.s);
					};
					PushUndo("Move " + std::to_string(before.size()) + " atoms",
						[apply, before]{ apply(before); },
						[apply, after] { apply(after); });
				}
			}
		}

		// Canvas 2D rect gizmo: corner + edge handles write Canvas width/height and shift the
		// centre so the opposite side stays put (screen-space canvases scale 1 ref px = 1/ppu).
		s_canvasGizmoHot = false;
		{
			AppInstance* capp = AppInstance::GetSingleton();
			Atom* csel = capp->selectedInHieararchy;
			nuke::Canvas* cv = csel ? csel->GetComponent<nuke::Canvas>() : nullptr;
			if (!possessed && cv && cv->transform && editorCam && editorCam->transform)
			{
				ImVec2 rmin = ImGui::GetItemRectMin();
				ImVec2 vsz  = ImGui::GetItemRectSize();
				Transform* gcam = editorCam->transform;
				Vector3 ge = gcam->globalPosition();
				Vector3 gf = gcam->direction(), gu = gcam->up(), gr = gcam->right();
				float aspect = (vsz.y > 0.0f) ? vsz.x / vsz.y : 1.0f;
				glm::mat4 gv = glm::lookAtLH(
					glm::vec3((float)ge.x, (float)ge.y, (float)ge.z),
					glm::vec3((float)(ge.x + gf.x), (float)(ge.y + gf.y), (float)(ge.z + gf.z)),
					glm::vec3((float)gu.x, (float)gu.y, (float)gu.z));
				glm::mat4 gp = EditorCamProj(editorCam, aspect);
				auto toScreen = [&](const Vector3& w, ImVec2& out) -> bool
				{
					glm::vec4 c = gp * gv * glm::vec4((float)w.x, (float)w.y, (float)w.z, 1.0f);
					if (c.w <= 1e-4f) return false;
					out = ImVec2(rmin.x + (c.x / c.w * 0.5f + 0.5f) * vsz.x,
					             rmin.y + (0.5f - c.y / c.w * 0.5f) * vsz.y);
					return true;
				};

				const float es = (cv->mode == nuke::CanvasMode::WorldSpace) ? 1.0f : cv->PxToWorld();
				Transform* ct = cv->transform;
				Vector3 P = ct->globalPosition(), R = ct->right(), U = ct->up();
				float hw = cv->width * 0.5f * es, hh = cv->height * 0.5f * es;

				// 8 handles (corners + edge midpoints), plane-signed codes.
				ImVec2 hpos[8]; int codes[8][2]; int k = 0;
				for (int sy = -1; sy <= 1; ++sy)
					for (int sx = -1; sx <= 1; ++sx)
					{
						if (!sx && !sy) continue;
						Vector3 wp(P.x + R.x * sx * hw + U.x * sy * hh,
						           P.y + R.y * sx * hw + U.y * sy * hh,
						           P.z + R.z * sx * hw + U.z * sy * hh);
						ImVec2 s2;
						hpos[k]  = toScreen(wp, s2) ? s2 : ImVec2(-10000, -10000);
						codes[k][0] = sx; codes[k][1] = sy;
						++k;
					}
				ImVec2 mp = ImGui::GetIO().MousePos;
				int hover = -1;
				for (int i = 0; i < 8; ++i)
				{
					float dx = mp.x - hpos[i].x, dy = mp.y - hpos[i].y;
					if (dx * dx + dy * dy < 8.0f * 8.0f) { hover = i; break; }
				}

				static bool dragging = false; static int dragIdx = -1; static void* dragCv = nullptr;
				if (!dragging && hover >= 0 && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				{ dragging = true; dragIdx = hover; dragCv = cv; }
				if (dragging && (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || dragCv != (void*)cv))
				{ dragging = false; dragIdx = -1; dragCv = nullptr; }
				s_canvasGizmoHot = (hover >= 0) || dragging;

				if (dragging && dragCv == (void*)cv && dragIdx >= 0)
				{
					// Mouse ray (same math as PickAtScreen) -> canvas plane -> plane coords (du, dv).
					float ndcx = ((mp.x - rmin.x) / vsz.x) * 2.0f - 1.0f;
					float ndcy = 1.0f - ((mp.y - rmin.y) / vsz.y) * 2.0f;
					Vector3 ro = ge, rdir = gf;
					if (editorCam->projBlend >= 0.5f)
					{
						float oh = (editorCam->orthoSize > 1e-4f) ? editorCam->orthoSize : 1.0f, ow = oh * aspect;
						ro = Vector3(ge.x + ndcx * ow * gr.x + ndcy * oh * gu.x,
						             ge.y + ndcx * ow * gr.y + ndcy * oh * gu.y,
						             ge.z + ndcx * ow * gr.z + ndcy * oh * gu.z);
					}
					else
					{
						float thf = tanf((float)editorCam->fov * 0.5f * 0.01745329252f);
						rdir = Vector3(gf.x + ndcx * thf * aspect * gr.x + ndcy * thf * gu.x,
						               gf.y + ndcx * thf * aspect * gr.y + ndcy * thf * gu.y,
						               gf.z + ndcx * thf * aspect * gr.z + ndcy * thf * gu.z);
					}
					Vector3 n(R.y * U.z - R.z * U.y, R.z * U.x - R.x * U.z, R.x * U.y - R.y * U.x);
					double denom = rdir.x * n.x + rdir.y * n.y + rdir.z * n.z;
					if (fabs(denom) > 1e-8)
					{
						double tHit = ((P.x - ro.x) * n.x + (P.y - ro.y) * n.y + (P.z - ro.z) * n.z) / denom;
						Vector3 hit(ro.x + rdir.x * tHit, ro.y + rdir.y * tHit, ro.z + rdir.z * tHit);
						Vector3 dv3(hit.x - P.x, hit.y - P.y, hit.z - P.z);
						float du = (float)(dv3.x * R.x + dv3.y * R.y + dv3.z * R.z);
						float dvv = (float)(dv3.x * U.x + dv3.y * U.y + dv3.z * U.z);
						const int sx = codes[dragIdx][0], sy = codes[dragIdx][1];
						Vector3 shift(0, 0, 0);
						if (sx)   // horizontal side follows the mouse; the opposite side stays fixed
						{
							float fixedE = -sx * hw, newE = du;
							float newW = fabsf(newE - fixedE); if (newW < 0.01f * es) newW = 0.01f * es;
							float cOff = (fixedE + newE) * 0.5f;
							cv->width = newW / es;
							shift = Vector3(shift.x + R.x * cOff, shift.y + R.y * cOff, shift.z + R.z * cOff);
						}
						if (sy)   // vertical side
						{
							float fixedE = -sy * hh, newE = dvv;
							float newH = fabsf(newE - fixedE); if (newH < 0.01f * es) newH = 0.01f * es;
							float cOff = (fixedE + newE) * 0.5f;
							cv->height = newH / es;
							shift = Vector3(shift.x + U.x * cOff, shift.y + U.y * cOff, shift.z + U.z * cOff);
						}
						if (shift.x || shift.y || shift.z)
							ct->SetGlobal(Vector3(P.x + shift.x, P.y + shift.y, P.z + shift.z),
							              ct->globalRotation(), ct->globalScale());
					}
				}

				ImDrawList* dl = ImGui::GetWindowDrawList();
				for (int i = 0; i < 8; ++i)
				{
					if (hpos[i].x < -999.0f) continue;
					const bool hot = (i == hover) || (dragging && i == dragIdx);
					const ImU32 col = hot ? IM_COL32(255, 200, 60, 255) : IM_COL32(80, 180, 255, 255);
					const bool corner = codes[i][0] && codes[i][1];
					if (corner) dl->AddRectFilled(ImVec2(hpos[i].x - 4, hpos[i].y - 4), ImVec2(hpos[i].x + 4, hpos[i].y + 4), col);
					else        dl->AddCircleFilled(hpos[i], 4.0f, col);
				}
			}
		}

		// Camera control while hovering the image: RMB = orbit/look, MMB = pan, wheel = dolly.
		// Suspended while possessed — the game owns the mouse and the editor camera isn't on screen.
		if (!possessed && editorCam && editorCam->transform && (ImGui::IsItemHovered() || s_lookActive))
		{
			ImGuiIO& io = ImGui::GetIO();
			Transform* t = editorCam->transform;
			const float rotSpeed = 0.005f, panSpeed = 0.01f, zoomSpeed = 0.5f;

			// Foliage paint brush, armed from the Foliage inspector. LMB paints/erases along the
			// stroke, stepped by half the radius so holding still doesn't stack instances; it also
			// consumes the click so painting never deselects.
			bool foliagePainting = false;
			if (foliageBrush != 0)
			{
				Atom* selA = AppInstance::GetSingleton()->selectedInHieararchy;
				nuke::Foliage* fol = selA ? selA->GetComponent<nuke::Foliage>() : nullptr;
				if (!fol) foliageBrush = 0;   // selection left the layer: disarm
				else if (ImGui::IsItemHovered() && !ImGuizmo::IsUsing())
				{
					foliagePainting = true;
					ImVec2 rmin = ImGui::GetItemRectMin(), szv = ImGui::GetItemRectSize(), mp = io.MousePos;
					float ndcx = ((mp.x - rmin.x) / szv.x) * 2.0f - 1.0f;
					float ndcy = 1.0f - ((mp.y - rmin.y) / szv.y) * 2.0f;
					Vector3 o = t->globalPosition();
					Vector3 f = t->direction(), rr = t->right(), uu = t->up();
					float aspect = (szv.y > 0.0f) ? szv.x / szv.y : 1.0f;
					float thf = tanf((float)editorCam->fov * 0.5f * 0.01745329252f);
					Vector3 dir(f.x + ndcx * thf * aspect * rr.x + ndcy * thf * uu.x,
					            f.y + ndcx * thf * aspect * rr.y + ndcy * thf * uu.y,
					            f.z + ndcx * thf * aspect * rr.z + ndcy * thf * uu.z);
					double L = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
					if (L > 1e-9) { dir.x /= L; dir.y /= L; dir.z /= L; }
					float dist = 0.0f;
					Atom* hit = AppInstance::GetSingleton()->currentWorld->PickDist(o, dir, dist);
					static Vector3 lastApply(1e9, 1e9, 1e9);
					if (hit && dist > 0.0f && dist < 1e29f)
					{
						Vector3 hp(o.x + dir.x * dist, o.y + dir.y * dist, o.z + dir.z * dist);
						// Brush ring is projected onto the surface by a short down-ray per segment.
						{
							const Color bc = foliageBrush == 1 ? Color(0.4, 1.0, 0.5, 1.0) : Color(1.0, 0.45, 0.35, 1.0);
							const int kSeg = 32;
							const float rr2 = foliageBrushRadius;
							const double castUp = rr2 + 2.0;
							Vector3 prev(0, 0, 0); bool hasPrev = false; Vector3 firstPt(0, 0, 0);
							World* wld = AppInstance::GetSingleton()->currentWorld;
							for (int s2 = 0; s2 <= kSeg; ++s2)
							{
								const float a2 = (float)s2 / kSeg * 6.2831853f;
								Vector3 p(hp.x + cosf(a2) * rr2, hp.y, hp.z + sinf(a2) * rr2);
								float d2 = 0.0f;
								Vector3 ro(p.x, p.y + castUp, p.z);
								Atom* h2 = wld->PickDist(ro, Vector3(0, -1, 0), d2);
								if (h2 && d2 > 0.0f && d2 < castUp * 2.0 + 50.0)
									p.y = ro.y - d2 + 0.02;
								if (hasPrev) DebugDraw::Line(prev, p, bc);
								else firstPt = p;
								prev = p; hasPrev = true;
							}
						}
						if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
						{
							double dx = hp.x - lastApply.x, dy = hp.y - lastApply.y, dz = hp.z - lastApply.z;
							const double step = foliageBrushRadius * 0.5;
							if (dx * dx + dy * dy + dz * dz > step * step)
							{
								if (foliageBrush == 1) fol->PaintAt(hp, foliageBrushRadius, foliageBrushDensity);
								else                   fol->EraseAt(hp, foliageBrushRadius);
								lastApply = hp;
								worldDirty = true;
							}
						}
						else lastApply = Vector3(1e9, 1e9, 1e9);   // stroke ended: next press applies at once
					}
				}
			}

			// SurfaceMask condition brush, armed from the SurfaceMask inspector. LMB paints the
			// selected channel continuously (strength/sec); consumes the click like foliage.
			bool maskPainting = false;
			if (maskBrush != 0)
			{
				Atom* selA = AppInstance::GetSingleton()->selectedInHieararchy;
				nuke::SurfaceMask* msk = selA ? selA->GetComponent<nuke::SurfaceMask>() : nullptr;
				if (!msk) maskBrush = 0;   // selection left the mask: disarm
				else if (ImGui::IsItemHovered() && !ImGuizmo::IsUsing())
				{
					maskPainting = true;
					ImVec2 rmin = ImGui::GetItemRectMin(), szv = ImGui::GetItemRectSize(), mp = io.MousePos;
					float ndcx = ((mp.x - rmin.x) / szv.x) * 2.0f - 1.0f;
					float ndcy = 1.0f - ((mp.y - rmin.y) / szv.y) * 2.0f;
					Vector3 o = t->globalPosition();
					Vector3 f = t->direction(), rr = t->right(), uu = t->up();
					float aspect = (szv.y > 0.0f) ? szv.x / szv.y : 1.0f;
					float thf = tanf((float)editorCam->fov * 0.5f * 0.01745329252f);
					Vector3 dir(f.x + ndcx * thf * aspect * rr.x + ndcy * thf * uu.x,
					            f.y + ndcx * thf * aspect * rr.y + ndcy * thf * uu.y,
					            f.z + ndcx * thf * aspect * rr.z + ndcy * thf * uu.z);
					double L = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
					if (L > 1e-9) { dir.x /= L; dir.y /= L; dir.z /= L; }
					float dist = 0.0f;
					Atom* hit = AppInstance::GetSingleton()->currentWorld->PickDist(o, dir, dist);
					if (hit && dist > 0.0f && dist < 1e29f)
					{
						Vector3 hp(o.x + dir.x * dist, o.y + dir.y * dist, o.z + dir.z * dist);
						const Color bc = maskBrush == 1 ? Color(0.45, 0.75, 1.0, 1.0) : Color(1.0, 0.45, 0.35, 1.0);
						DebugDraw::WireSphere(hp, maskBrushRadius, bc);
						if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
						{
							const double amt = (maskBrush == 1 ? 1.0 : -1.0) * maskBrushStrength * io.DeltaTime;
							msk->Paint(hp, maskBrushRadius, maskBrushChannel, amt);
							worldDirty = true;
						}
					}
				}
			}

			// Left-click picks (null = deselect); skipped while any gizmo/handle owns the mouse
			// or a MODULE viewport tool claimed the cursor (terrain brush strokes must not select).
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsUsing() && !ImGuizmo::IsOver() && !s_canvasGizmoHot && !s_riverGizmoHot && !foliagePainting && !maskPainting
			    && !AppInstance::GetSingleton()->editorToolActive)
			{
				ImVec2 rmin = ImGui::GetItemRectMin();
				ImVec2 sz   = ImGui::GetItemRectSize();
				ImVec2 mp   = io.MousePos;
				// Icons are tested before the ray: mesh-less entities have no other clickable body.
				Atom* iconPick = nullptr;
				for (auto it = iconHits.rbegin(); it != iconHits.rend(); ++it)
					if (mp.x >= it->first.x && mp.x <= it->first.z && mp.y >= it->first.y && mp.y <= it->first.w)
					{
						iconPick = it->second;
						break;
					}
				if (iconPick)
				{
					if (io.KeyCtrl) HierToggle(iconPick); else HierSelect(iconPick);   // ctrl adds
				}
				else
				{
					float ndcx = ((mp.x - rmin.x) / sz.x) * 2.0f - 1.0f;
					float ndcy = 1.0f - ((mp.y - rmin.y) / sz.y) * 2.0f;
					Vector3 o = t->globalPosition();
					Vector3 f = t->direction(), rr = t->right(), uu = t->up();
					float aspect = (sz.y > 0.0f) ? sz.x / sz.y : 1.0f;
					float thf = tanf((float)editorCam->fov * 0.5f * 0.01745329252f);
					Vector3 dir(f.x + ndcx * thf * aspect * rr.x + ndcy * thf * uu.x,
					            f.y + ndcx * thf * aspect * rr.y + ndcy * thf * uu.y,
					            f.z + ndcx * thf * aspect * rr.z + ndcy * thf * uu.z);
					Atom* hit = AppInstance::GetSingleton()->currentWorld->Pick(o, dir);
					if (io.KeyCtrl)   // ctrl-click toggles the hit object in the multi-selection
					{
						if (hit)
						{
							Atom* root = hit;
							while (root->GetParent()) root = root->GetParent();
							HierToggle(root);
						}
					}
					else
					{
						// A model is a SUBTREE: the first click grabs the whole thing (its root), the
						// next one drills into the part actually under the cursor, and a third returns
						// to the root — so clicking never fights the hierarchy.
						Atom* sel = AppInstance::GetSingleton()->selectedInHieararchy;
						if (!hit) HierSelect(nullptr);
						else
						{
							Atom* root = hit;
							while (root->GetParent()) root = root->GetParent();
							if (hit == root)          HierSelect(root);          // single-atom object
							else if (sel == root)     HierSelect(hit);           // drill into the part
							else if (sel == hit)      HierSelect(root);          // back out to the whole
							else                      HierSelect(root);          // new object: whole first
						}
					}
					// marquee: a drag that starts on EMPTY space rubber-bands a rect; every
					// pickable root whose world position projects inside gets selected on release.
					if (!hit && !io.KeyCtrl) { s_marqueeArm = true; s_marqueeStart = mp; }
				}
			}

			// marquee select: armed by an empty-space press, active past the drag threshold.
			if (s_marqueeArm && ImGui::IsMouseDown(ImGuiMouseButton_Left)
			    && ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left) && !ImGuizmo::IsUsing())
			{
				ImVec2 mp = io.MousePos;
				ImVec2 a(std::min(s_marqueeStart.x, mp.x), std::min(s_marqueeStart.y, mp.y));
				ImVec2 b(std::max(s_marqueeStart.x, mp.x), std::max(s_marqueeStart.y, mp.y));
				ImDrawList* dl = ImGui::GetWindowDrawList();
				dl->AddRectFilled(a, b, IM_COL32(80, 160, 255, 28));
				dl->AddRect(a, b, IM_COL32(80, 160, 255, 200));
				s_marqueeLive = true;
			}
			if (s_marqueeArm && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
			{
				if (s_marqueeLive)
				{
					ImVec2 mp = io.MousePos;
					ImVec2 ra(std::min(s_marqueeStart.x, mp.x), std::min(s_marqueeStart.y, mp.y));
					ImVec2 rb(std::max(s_marqueeStart.x, mp.x), std::max(s_marqueeStart.y, mp.y));
					ImVec2 rmin = ImGui::GetItemRectMin();
					ImVec2 sz   = ImGui::GetItemRectSize();
					AppInstance* app = AppInstance::GetSingleton();
					Transform* ct = editorCam->transform;
					Vector3 co = ct->globalPosition(), cf = ct->direction(), cr = ct->right(), cu = ct->up();
					const float thf = tanf((float)editorCam->fov * 0.5f * 0.01745329252f);
					const float aspect = (sz.y > 0.0f) ? sz.x / sz.y : 1.0f;
					HierSelect(nullptr);
					Atom* first = nullptr;
					for (Atom* atom : app->currentWorld->GetHierarchy())
					{
						if (!atom || !atom->enabled || atom->GetName() == "Editor Camera") continue;
						Vector3 p = atom->GetTransform().globalPosition();
						Vector3 d(p.x - co.x, p.y - co.y, p.z - co.z);
						const double fz = d.x * cf.x + d.y * cf.y + d.z * cf.z;   // camera-space depth
						if (fz <= 0.0) continue;                                  // behind the camera
						const double lx = (d.x * cr.x + d.y * cr.y + d.z * cr.z) / (fz * thf * aspect);
						const double ly = (d.x * cu.x + d.y * cu.y + d.z * cu.z) / (fz * thf);
						const float px = rmin.x + (float)((lx + 1.0) * 0.5) * sz.x;
						const float py = rmin.y + (float)((1.0 - ly) * 0.5) * sz.y;
						if (px < ra.x || px > rb.x || py < ra.y || py > rb.y) continue;
						if (!first) { first = atom; HierSelect(atom); }
						else HierToggle(atom);
					}
				}
				s_marqueeArm = s_marqueeLive = false;
			}
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseReleased(ImGuiMouseButton_Left))
				s_marqueeArm = s_marqueeLive = false;

			// Look/pan begins: latch the wrap mode + remember where to put the cursor back.
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
			{
				s_lookActive = true;
				s_lookPressPos = io.MousePos;
			}
			// A warp lands as one giant delta the next frame — swallow that one.
			const ImVec2 lookDelta = s_lookWarpSkip ? ImVec2(0, 0) : io.MouseDelta;
			s_lookWarpSkip = false;
			// Sync orbit angles at drag start from the FORWARD vector, never EulerDeg(): its
			// quat->euler recompute uses a different order/range and drops roll.
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
			{
				Vector3 f = t->direction();
				double len = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
				if (len > 1e-6) { f.x /= len; f.y /= len; f.z /= len; }
				double py = f.y; if (py > 1.0) py = 1.0; if (py < -1.0) py = -1.0;
				camPitch = (float)std::asin(-py);          // forward = (cosP sinY, -sinP, cosP cosY)
				camYaw   = (float)std::atan2(f.x, f.z);
			}
			if (ImGui::IsMouseDragging(ImGuiMouseButton_Right))
			{
				camYaw   += lookDelta.x * rotSpeed;
				camPitch += lookDelta.y * rotSpeed;
				const float lim = 1.55f; // ~89deg pitch clamp
				if (camPitch >  lim) camPitch =  lim;
				if (camPitch < -lim) camPitch = -lim;
				t->SetEulerDeg(Vector3(camPitch * 57.29578f, camYaw * 57.29578f, 0.0f));
			}
			if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
			{
				t->position += t->right() * (double)(-lookDelta.x * panSpeed)
				             + t->up()    * (double)( lookDelta.y * panSpeed);
			}
			// Edge wrap: the hidden cursor teleports to the opposite side just before it could
			// leave the image, so the drag never dies and no other widget ever sees it.
			if (s_lookActive && (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle)))
			{
				ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
				if (mx.x - mn.x > 24.0f && mx.y - mn.y > 24.0f)
				{
					ImVec2 p = io.MousePos;
					bool wrap = false;
					if      (p.x <= mn.x + 2.0f) { p.x = mx.x - 8.0f; wrap = true; }
					else if (p.x >= mx.x - 2.0f) { p.x = mn.x + 8.0f; wrap = true; }
					if      (p.y <= mn.y + 2.0f) { p.y = mx.y - 8.0f; wrap = true; }
					else if (p.y >= mx.y - 2.0f) { p.y = mn.y + 8.0f; wrap = true; }
					if (wrap) WarpMouse(p);
				}
			}
			if (io.MouseWheel != 0.0f)
			{
				// Wheel while flying (RMB held) scales the fly speed; otherwise it dollies.
				if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
				{
					s_flyMul = std::max(0.02f, std::min(100.0f, s_flyMul * (1.0f + io.MouseWheel * 0.15f)));
					s_flyMulShowUntil = ImGui::GetTime() + 1.2;
				}
				else
					t->position += t->direction() * (double)(io.MouseWheel * zoomSpeed);
			}

			// Free-flight: hold RMB + WASD (Q/E = down/up, Shift = faster).
			if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
			{
				float fly = 5.0f * s_flyMul * io.DeltaTime;
				if (io.KeyShift) fly *= 3.0f;
				if (ImGui::IsKeyDown(ImGuiKey_W)) t->position += t->direction() * (double) fly;
				if (ImGui::IsKeyDown(ImGuiKey_S)) t->position += t->direction() * (double)-fly;
				if (ImGui::IsKeyDown(ImGuiKey_D)) t->position += t->right()     * (double) fly;
				if (ImGui::IsKeyDown(ImGuiKey_A)) t->position += t->right()     * (double)-fly;
				if (ImGui::IsKeyDown(ImGuiKey_E)) t->position += t->up()        * (double) fly;
				if (ImGui::IsKeyDown(ImGuiKey_Q)) t->position += t->up()        * (double)-fly;
			}

			// Fly-speed readout, briefly after a wheel change (top-left of the image).
			if (ImGui::GetTime() < s_flyMulShowUntil)
			{
				char spd[48];
				snprintf(spd, sizeof(spd), "Fly speed x%.2f", s_flyMul);
				const ImVec2 rm = ImGui::GetItemRectMin();
				ImDrawList* dl = ImGui::GetWindowDrawList();
				dl->AddText(ImVec2(rm.x + 11, rm.y + 9), IM_COL32(0, 0, 0, 200), spd);
				dl->AddText(ImVec2(rm.x + 10, rm.y + 8), IM_COL32(255, 255, 255, 230), spd);
			}
		}
	}

	// Must run EVERY viewport frame (outside the hover gates) or a release is missed.
	{
		const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle);
		if (s_lookActive)
		{
			if (!down)
			{
				s_lookActive = false;
				WarpMouse(s_lookPressPos);   // reappear where the look began
			}
			else
				ImGui::SetMouseCursor(ImGuiMouseCursor_None);   // hidden while looking
		}
	}
	});
}
