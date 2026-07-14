// viewport panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>
#include "API/Model/Math.h"
#include "API/Model/resdb.h"   // RenderTexture camera preview (resolve targetTexGuid -> RT)
#include "API/Model/Light.h"             // entity icons: glyph/tint per component
#include "API/Model/ReflectionProbe.h"
#include "API/Model/Environment.h"
#include <functional>
#include <cmath>
#include <algorithm>
#include <cctype>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>   // gizmo: decompose the manipulated world matrix
#include <glm/gtc/type_ptr.hpp>

// Cast a ray from a screen point inside the viewport image and return the atom under it (null = none).
// Shared by left-click selection and asset drag&drop onto an object.
static nuke::Atom* PickAtScreen(nuke::Camera* cam, ImVec2 rmin, ImVec2 sz, ImVec2 mp)
{
	if (!cam || !cam->transform || sz.x <= 0.0f || sz.y <= 0.0f) return nullptr;
	nuke::Transform* t = cam->transform;
	float ndcx = ((mp.x - rmin.x) / sz.x) * 2.0f - 1.0f;
	float ndcy = 1.0f - ((mp.y - rmin.y) / sz.y) * 2.0f;
	nuke::Vector3 o = t->globalPosition();
	nuke::Vector3 f = t->direction(), rr = t->right(), uu = t->up();
	float aspect = sz.x / sz.y;
	if (cam->projBlend >= 0.5f)   // orthographic: rays are PARALLEL (dir = forward), the ORIGIN slides across the frame
	{
		float halfH = (cam->orthoSize > 1e-4f) ? cam->orthoSize : 1.0f, halfW = halfH * aspect;
		nuke::Vector3 ori(o.x + ndcx * halfW * rr.x + ndcy * halfH * uu.x,
		                  o.y + ndcx * halfW * rr.y + ndcy * halfH * uu.y,
		                  o.z + ndcx * halfW * rr.z + ndcy * halfH * uu.z);
		return nuke::AppInstance::GetSingleton()->currentWorld->Pick(ori, f);
	}
	float thf = tanf((float)cam->fov * 0.5f * 0.01745329252f);
	nuke::Vector3 dir(f.x + ndcx * thf * aspect * rr.x + ndcy * thf * uu.x,
	                  f.y + ndcx * thf * aspect * rr.y + ndcy * thf * uu.y,
	                  f.z + ndcx * thf * aspect * rr.z + ndcy * thf * uu.z);
	return nuke::AppInstance::GetSingleton()->currentWorld->Pick(o, dir);
}

// The editor camera's projection matrix (glm, LH depth 0..1), matching the renderer's
// persp<->ortho blend (Camera::projBlend) so gizmos / entity icons / picking stay glued to the
// rendered image in orthographic mode and during the transition.
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

// Billboard icons for invisible entities (2.1): cameras, lights, reflection probes and
// environments have no mesh — in EDIT mode each gets a screen-space Lucide glyph (the
// hierarchy's icon language) at its world position, drawn back-to-front on the viewport
// overlay. Hovering shows the atom name; the click handler picks icons before the ray.
void EditorUI::DrawEntityIcons(ImVec2 rmin, ImVec2 sz)
{
	iconHits.clear();
	AppInstance* app = AppInstance::GetSingleton();
	if (!editorCam || !editorCam->transform || !app->currentWorld) return;
	if (app->playState != 0) return;                       // edit mode only
	if (sz.x <= 1.0f || sz.y <= 1.0f) return;

	// The EXACT view/proj the gizmo uses (renderer convention: LH, depth 0..1), so the
	// icons sit on the rendered image precisely.
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
		vp = EditorCamProj(editorCam, aspect) * v;   // persp/ortho blended (matches the render)
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
				// tint with the light color, floored so a dark light stays readable
				const double fl = 0.35;
				col = IM_COL32((int)(255.0 * std::max(l->color.r, fl)),
				               (int)(255.0 * std::max(l->color.g, fl)),
				               (int)(255.0 * std::max(l->color.b, fl)), 235);
			}
			else if (atom->GetComponent<ReflectionProbe>()) { glyph = ICON_LC_APERTURE;  col = IM_COL32(200, 140, 255, 235); }
			else if (atom->GetComponent<Environment>())     { glyph = ICON_LC_CLOUD_SUN; col = IM_COL32(150, 200, 255, 235); }
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
		if (ic.atom == sel)   // selection ring, matching the gizmo's accent
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

void EditorUI::winRender()
{
	if (!win->render) return;
	ImGui::Begin("Render", &win->render, window_flags);

	// Smooth "focus selected": ease the editor camera toward the target each frame (orientation kept).
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

	// Hotkeys: fire when the viewport is focused OR hovered (mouse over it) — matching the WASD camera
	// input, which is hover-based, so tools/Delete work even if another docked panel holds focus. Not
	// while typing in a field or flying with RMB held. Q/W/E/R = tools, X = World/Local, F = frame, Del.
	if ((ImGui::IsWindowFocused() || ImGui::IsWindowHovered()) && !ImGui::GetIO().WantTextInput && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
	{
		AppInstance* a = AppInstance::GetSingleton();
		if (ImGui::IsKeyPressed(ImGuiKey_Q)) a->manipulationMode = 0;
		if (ImGui::IsKeyPressed(ImGuiKey_W)) a->manipulationMode = 1;
		if (ImGui::IsKeyPressed(ImGuiKey_E)) a->manipulationMode = 2;
		if (ImGui::IsKeyPressed(ImGuiKey_R)) a->manipulationMode = 3;
		if (ImGui::IsKeyPressed(ImGuiKey_X)) a->manipulationWorld = !a->manipulationWorld;
		if (ImGui::IsKeyPressed(ImGuiKey_F)) FocusSelected();   // frame the selected object
		// Delete the selected atom from the viewport too (same rebindable pool as the hierarchy).
		nuke::Hotkeys* hk = nuke::Hotkeys::Get();
		nuke::Hotkey* d  = hk->Find("editor.delete");
		nuke::Hotkey* df = hk->Find("editor.delete.force");
		if ((d  && d->bound  && ImGui::IsKeyChordPressed((ImGuiKeyChord)d->chord)) ||
		    (df && df->bound && ImGui::IsKeyChordPressed((ImGuiKeyChord)df->chord)))
			DeleteSelectedAtom();
	}

	ImVec2 avail = ImGui::GetContentRegionAvail();
	iRender* r = AppInstance::GetSingleton()->render;
	if (r && avail.x >= 1.0f && avail.y >= 1.0f)
	{
		if (sceneRTId == 0)
		{
			sceneRTId = r->createRenderTarget((int)avail.x, (int)avail.y);
			if (editorCam) editorCam->renderTarget = sceneRTId; // editor cam draws here
		}
		else
		{
			r->resizeRenderTarget(sceneRTId, (int)avail.x, (int)avail.y); // match the panel
		}
		uint64_t tex = r->getRenderTargetTexture(sceneRTId);
		if (tex)
		{
			ImGui::Image((ImTextureID)tex, avail); // the live scene viewport
			// Drag from the browser onto the scene: a material/texture drops ONTO the object under the
			// cursor; anything else (prefab/mesh/world) spawns into the scene.
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* dp = ImGui::AcceptDragDropPayload("NUKE_ASSET"))
				{
					std::string dpath((const char*)dp->Data), dext;
					size_t dot = dpath.find_last_of('.');
					if (dot != std::string::npos) dext = dpath.substr(dot);
					std::transform(dext.begin(), dext.end(), dext.begin(), ::tolower);
					if (dext == ".numat" || dext == ".nutex")
					{
						if (nuke::Atom* hit = PickAtScreen(editorCam, ImGui::GetItemRectMin(), ImGui::GetItemRectSize(), ImGui::GetMousePos()))
							DropAssetOnAtom(hit, dpath);
					}
					else DropAsset(dpath);
				}
				ImGui::EndDragDropTarget();
			}
			// Tell the runtime GUI (NukeGUI) to draw INTO this viewport RT + map input to its rect.
			ImVec2 imin = ImGui::GetItemRectMin();
			AppInstance* app = AppInstance::GetSingleton();
			app->uiTarget = sceneRTId;
			app->uiX = (int)imin.x; app->uiY = (int)imin.y;
			app->uiW = (int)avail.x; app->uiH = (int)avail.y;

			// Invisible-entity icons (edit mode): drawn UNDER the camera preview and the gizmo.
			DrawEntityIcons(imin, avail);
		}
		else
			ImGui::Text("No scene texture.");

		// --- selected-camera preview: a small overlay in the viewport's bottom-right ---
		if (previewCam) { previewCam->renderTarget = 0; previewCam = nullptr; }   // release last frame's
		{
			Atom* sel = AppInstance::GetSingleton()->selectedInHieararchy;
			Camera* selCam = sel ? sel->GetComponent<Camera>() : nullptr;
			if (selCam && !selCam->enabled) selCam = nullptr;   // disabled camera: no preview (it renders nothing)
			if (tex && selCam && selCam != editorCam)
			{
				uint64_t ptex = 0;
				if (!selCam->targetTexGuid.empty())
				{
					// RenderTexture camera: it already renders into its own RT (World::Render) — preview that
					// directly. Do NOT hijack renderTarget to camPreviewRT (that would steal it from the RT).
					if (nuke::Texture* rtx = ResDB::getSingleton()->GetTexture(selCam->targetTexGuid))
						if (rtx->rtId) ptex = r->getRenderTargetTexture(rtx->rtId);
					previewCam = nullptr;
				}
				else
				{
					if (camPreviewRT == 0) camPreviewRT = r->createRenderTarget(256, 144);
					selCam->renderTarget = camPreviewRT;   // World::Render draws it here next pass
					previewCam = selCam;
					ptex = r->getRenderTargetTexture(camPreviewRT);
				}

				ImVec2 imax = ImGui::GetItemRectMax();  // bottom-right of the scene image
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

		// Transform gizmo over the selected object (only when a manip tool is active).
		{
			AppInstance* gapp = AppInstance::GetSingleton();
			Atom* gsel = gapp->selectedInHieararchy;
			if (gsel && editorCam && gapp->manipulationMode != 0)
			{
				ImGuizmo::SetOrthographic(false);
				ImGuizmo::SetDrawlist();
				ImVec2 grmin = ImGui::GetItemRectMin();
				ImVec2 gsz   = ImGui::GetItemRectSize();
				ImGuizmo::SetRect(grmin.x, grmin.y, gsz.x, gsz.y);

				// Build view/proj on the editor side in glm column-major, in the SAME
				// convention as the renderer (Diligent: left-handed, depth 0..1), so the
				// gizmo overlays the rendered image and ImGuizmo gets valid input.
				// Feed ImGuizmo the renderer's EXACT view/proj (Diligent: row-major, LH,
				// depth 0..1) so screen<->world matches the image precisely — per-axis
				// scale needs an exact ray, and ImGuizmo then detects handedness right.
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
					glm::mat4 gp = EditorCamProj(editorCam, gaspect);   // persp/ortho blended (matches the render)
					// ImGuizmo uses ROW convention, glm uses COLUMN — transpose all matrices.
					memcpy(gview, glm::value_ptr(gv), sizeof(gview));   // glm passed directly (no transpose)
					memcpy(gproj, glm::value_ptr(gp), sizeof(gproj));
				}

				Transform& gtt = gsel->GetTransform();
				if (!ImGuizmo::IsUsing())   // resync from the object only when NOT dragging
				{
					// Build the gizmo model from the atom's GLOBAL transform (matches the rendered image +
					// the selection outline, which also use global). The outline/render uses globalPosition/
					// Rotation/Scale, so the gizmo must too — otherwise a parented atom's gizmo sits at its
					// LOCAL coords (wrong place). NOTE this engine's parenting is non-standard (position is
					// additive, scale component-wise) — the write-back below inverts it.
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
				float gsnapv   = (gop == ImGuizmo::TRANSLATE) ? 0.5f : (gop == ImGuizmo::ROTATE) ? 15.0f : 0.1f;
				float gsnap[3] = { gsnapv, gsnapv, gsnapv };
				float* gsnapPtr = ImGui::GetIO().KeyCtrl ? gsnap : nullptr;   // hold Ctrl to snap
				ImGuizmo::Manipulate(gview, gproj, gop, gmode, gizmoMatrix, nullptr, gsnapPtr);
				if (ImGuizmo::IsUsing())
				{
					// Decompose the manipulated GLOBAL matrix, then convert back to LOCAL using this
					// engine's parenting rules (position additive, rotation quat-composed, scale multiplied).
					glm::mat4 nm = glm::make_mat4(gizmoMatrix);
					glm::vec3 nS, nT, nSkew; glm::vec4 nPersp; glm::quat nR;
					if (glm::decompose(nm, nS, nR, nT, nSkew, nPersp) &&
					    std::isfinite(nT.x) && std::isfinite(nT.y) && std::isfinite(nT.z) &&
					    std::isfinite(nS.x) && std::isfinite(nS.y) && std::isfinite(nS.z))
					{
						if (nS.x < 1e-3f && nS.x > -1e-3f) nS.x = 1e-3f;
						if (nS.y < 1e-3f && nS.y > -1e-3f) nS.y = 1e-3f;
						if (nS.z < 1e-3f && nS.z > -1e-3f) nS.z = 1e-3f;
						// Write the manipulated WORLD pose; Transform::SetGlobal converts it to local.
						gtt.SetGlobal(Vector3(nT.x, nT.y, nT.z),
						              Quaternion(nR.x, nR.y, nR.z, nR.w),
						              Vector3(nS.x, nS.y, nS.z));
					}
				}
			}
		}

		// Viewport camera control (while hovering the image):
		//   RMB drag = orbit/look, MMB drag = pan, wheel = dolly.
		if (editorCam && editorCam->transform && ImGui::IsItemHovered())
		{
			ImGuiIO& io = ImGui::GetIO();
			Transform* t = editorCam->transform;
			const float rotSpeed = 0.005f, panSpeed = 0.01f, zoomSpeed = 0.5f;

			// Left-click: pick the object under the cursor (null = deselect).
			// Skip if the gizmo is being interacted with, so dragging it doesn't deselect.
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsUsing() && !ImGuizmo::IsOver())
			{
				ImVec2 rmin = ImGui::GetItemRectMin();
				ImVec2 sz   = ImGui::GetItemRectSize();
				ImVec2 mp   = io.MousePos;
				// Entity icons first (topmost wins): invisible entities have no mesh, the
				// scene ray can't hit them — their icon IS their clickable body.
				Atom* iconPick = nullptr;
				for (auto it = iconHits.rbegin(); it != iconHits.rend(); ++it)
					if (mp.x >= it->first.x && mp.x <= it->first.z && mp.y >= it->first.y && mp.y <= it->first.w)
					{
						iconPick = it->second;
						break;
					}
				if (iconPick)
					AppInstance::GetSingleton()->selectedInHieararchy = iconPick;
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
					AppInstance::GetSingleton()->selectedInHieararchy =
						AppInstance::GetSingleton()->currentWorld->Pick(o, dir);
				}
			}

			// Sync the orbit angles from the camera when the drag STARTS. Derive them from the FORWARD
			// vector (unambiguous) — NOT EulerDeg(), whose quat->euler recompute after a load uses a
			// different order/range and drops roll, which made the first rotation snap to a bogus angle.
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
				camYaw   += io.MouseDelta.x * rotSpeed;
				camPitch += io.MouseDelta.y * rotSpeed;
				const float lim = 1.55f; // ~89deg pitch clamp
				if (camPitch >  lim) camPitch =  lim;
				if (camPitch < -lim) camPitch = -lim;
				t->SetEulerDeg(Vector3(camPitch * 57.29578f, camYaw * 57.29578f, 0.0f));
			}
			if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
			{
				t->position += t->right() * (double)(-io.MouseDelta.x * panSpeed)
				             + t->up()    * (double)( io.MouseDelta.y * panSpeed);
			}
			if (io.MouseWheel != 0.0f)
				t->position += t->direction() * (double)(io.MouseWheel * zoomSpeed);

			// Free-flight: hold RMB + WASD (Q/E = down/up, Shift = faster), Unity/UE-style.
			if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
			{
				float fly = 5.0f * io.DeltaTime;
				if (io.KeyShift) fly *= 3.0f;
				if (ImGui::IsKeyDown(ImGuiKey_W)) t->position += t->direction() * (double) fly;
				if (ImGui::IsKeyDown(ImGuiKey_S)) t->position += t->direction() * (double)-fly;
				if (ImGui::IsKeyDown(ImGuiKey_D)) t->position += t->right()     * (double) fly;
				if (ImGui::IsKeyDown(ImGuiKey_A)) t->position += t->right()     * (double)-fly;
				if (ImGui::IsKeyDown(ImGuiKey_E)) t->position += t->up()        * (double) fly;
				if (ImGui::IsKeyDown(ImGuiKey_Q)) t->position += t->up()        * (double)-fly;
			}
		}
	}
	ImGui::End();
}
