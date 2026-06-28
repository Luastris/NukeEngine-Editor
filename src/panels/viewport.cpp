// viewport panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>
#include "API/Model/Math.h"
#include <cmath>

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

	// Hotkeys (when focused, not typing, and NOT flying with RMB): Q/W/E/R = tools, X = World/Local.
	if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
	{
		AppInstance* a = AppInstance::GetSingleton();
		if (ImGui::IsKeyPressed(ImGuiKey_Q)) a->manipulationMode = 0;
		if (ImGui::IsKeyPressed(ImGuiKey_W)) a->manipulationMode = 1;
		if (ImGui::IsKeyPressed(ImGuiKey_E)) a->manipulationMode = 2;
		if (ImGui::IsKeyPressed(ImGuiKey_R)) a->manipulationMode = 3;
		if (ImGui::IsKeyPressed(ImGuiKey_X)) a->manipulationWorld = !a->manipulationWorld;
		if (ImGui::IsKeyPressed(ImGuiKey_F)) FocusSelected();   // frame the selected object
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
			AcceptAssetDropTarget();               // drag assets from the browser into the scene
			// Tell the runtime GUI (NukeGUI) to draw INTO this viewport RT + map input to its rect.
			ImVec2 imin = ImGui::GetItemRectMin();
			AppInstance* app = AppInstance::GetSingleton();
			app->uiTarget = sceneRTId;
			app->uiX = (int)imin.x; app->uiY = (int)imin.y;
			app->uiW = (int)avail.x; app->uiH = (int)avail.y;
		}
		else
			ImGui::Text("No scene texture.");

		// --- selected-camera preview: a small overlay in the viewport's bottom-right ---
		if (previewCam) { previewCam->renderTarget = 0; previewCam = nullptr; }   // release last frame's
		{
			Atom* sel = AppInstance::GetSingleton()->selectedInHieararchy;
			Camera* selCam = sel ? sel->GetComponent<Camera>() : nullptr;
			if (tex && selCam && selCam != editorCam)
			{
				if (camPreviewRT == 0) camPreviewRT = r->createRenderTarget(256, 144);
				selCam->renderTarget = camPreviewRT;   // World::Render draws it here next pass
				previewCam = selCam;

				ImVec2 imax = ImGui::GetItemRectMax();  // bottom-right of the scene image
				ImVec2 pv(256, 144), pad(12, 12);
				ImVec2 p0(imax.x - pv.x - pad.x, imax.y - pv.y - pad.y);
				ImVec2 p1(p0.x + pv.x, p0.y + pv.y);
				ImDrawList* dl = ImGui::GetWindowDrawList();
				dl->AddRectFilled(ImVec2(p0.x - 2, p0.y - 16), ImVec2(p1.x + 2, p1.y + 2), IM_COL32(15, 15, 15, 220));
				if (uint64_t ptex = r->getRenderTargetTexture(camPreviewRT))
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
					float gfovy   = (float)editorCam->fov * 0.01745329252f;
					glm::mat4 gv = glm::lookAtLH(
						glm::vec3((float)ge.x, (float)ge.y, (float)ge.z),
						glm::vec3((float)(ge.x + gf.x), (float)(ge.y + gf.y), (float)(ge.z + gf.z)),
						glm::vec3((float)gu.x, (float)gu.y, (float)gu.z));
					glm::mat4 gp = glm::perspectiveLH_ZO(gfovy, gaspect, editorCam->_near, editorCam->_far);
					// ImGuizmo uses ROW convention, glm uses COLUMN — transpose all matrices.
					memcpy(gview, glm::value_ptr(gv), sizeof(gview));   // glm passed directly (no transpose)
					memcpy(gproj, glm::value_ptr(gp), sizeof(gproj));
				}

				Transform& gtt = gsel->GetTransform();
				glm::quat gq((float)gtt.rotation.w, (float)gtt.rotation.x, (float)gtt.rotation.y, (float)gtt.rotation.z);
				glm::mat4 gworld = glm::translate(glm::mat4(1.0f), glm::vec3((float)gtt.position.x, (float)gtt.position.y, (float)gtt.position.z))
				                 * glm::mat4_cast(gq)
				                 * glm::scale(glm::mat4(1.0f), glm::vec3((float)gtt.scale.x, (float)gtt.scale.y, (float)gtt.scale.z));
				(void)gworld;   // model is built via ImGuizmo's own compose below
				if (!ImGuizmo::IsUsing())   // resync from the object only when NOT dragging
				{
					// Build the model with ImGuizmo's OWN compose so it's in exactly the
					// convention ImGuizmo expects — this is what makes per-axis scale work.
					Vector3 ep = gtt.position, ee = gtt.EulerDeg(), es = gtt.scale;
					float t3[3] = { (float)ep.x, (float)ep.y, (float)ep.z };
					float r3[3] = { (float)ee.x, (float)ee.y, (float)ee.z };
					float s3[3] = { (float)es.x, (float)es.y, (float)es.z };
					ImGuizmo::RecomposeMatrixFromComponents(t3, r3, s3, gizmoMatrix);
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
					float gtr[3], gro[3], gsc[3];
					ImGuizmo::DecomposeMatrixToComponents(gizmoMatrix, gtr, gro, gsc);
					bool gok = true;
					for (int i = 0; i < 3; ++i)
						gok = gok && std::isfinite(gtr[i]) && std::isfinite(gro[i]) && std::isfinite(gsc[i]);
					if (gok)   // skip degenerate results (e.g. scale dragged through zero -> NaN)
					{
						for (int i = 0; i < 3; ++i)
							if (fabsf(gsc[i]) < 1e-3f) gsc[i] = (gsc[i] < 0.0f) ? -1e-3f : 1e-3f;
						gtt.position = Vector3(gtr[0], gtr[1], gtr[2]);
						gtt.SetEulerDeg(Vector3(gro[0], gro[1], gro[2]));
						gtt.scale = Vector3(gsc[0], gsc[1], gsc[2]);
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
					AppInstance::GetSingleton()->currentScene->Pick(o, dir);
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
