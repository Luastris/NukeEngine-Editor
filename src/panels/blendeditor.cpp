// .nublend canvas editor: clip points on a 1D strip / 2D field (drag to reposition), a
// draggable query marker showing the LIVE blend weights, a point inspector, and a preview
// rig driven through an owned single-state controller (previewSm + previewBlend serve the
// EDITING copies).
#include "editor/editorui.h"
#include "editor/animshared.h"
#include "API/Model/Animator.h"
#include "API/Model/SkinnedMeshRenderer.h"
#include "API/Model/Camera.h"
#include <algorithm>

using namespace nuke;

void EditorUI::DrawBlendEditor(AssetEditorWin& w)
{
	BlendSpace* b = w.blend;
	if (!b) { ImGui::TextDisabled("Failed to load blend space."); return; }
	bool edited = false;
	bool structural = false;   // point set / clip refs changed -> graph rebind

	// owned single-state controller: one state whose motion IS this blend space
	if (!w.blSm)
	{
		w.blSm = new AnimSM();
		w.blSm->guid = "preview-sm:" + b->guid;
		w.blSm->name = "BlendPreview";
		AnimSM::Layer l;
		l.name = "Base";
		AnimSM::State s;
		s.name = "Blend";
		s.motion = b->guid;
		l.states.push_back(s);
		l.entry = "Blend";
		w.blSm->layers.push_back(l);
		AnimSM::Param px; px.name = b->paramX; px.type = 0; w.blSm->params.push_back(px);
		if (b->dims >= 2) { AnimSM::Param py; py.name = b->paramY; py.type = 0; w.blSm->params.push_back(py); }
	}

	// ---- preview rig -----------------------------------------------------------------------
	if (w.blRigSkel.empty())
		for (const BlendSpace::Point& p : b->points)
			if (AnimClip* c = ResDB::getSingleton()->GetClip(p.clip))
				if (!c->skelGuid.empty())
				{
					w.blRigSkel = c->skelGuid;
					if (!w.blRigSkel.empty()) break;
				}
	Animator* an = EditorEnsureRig(this, w.pv, w.blAtomId, w.blRigSkel);
	if (an)
	{
		if (an->previewSm != w.blSm || an->previewBlend != b)   // fresh open OR undo swapped the copy
		{
			an->smGuid = w.blSm->guid;
			an->SetPreviewController(w.blSm, b);
		}
		an->playOnStart = true;
		an->SetFloat(b->paramX, w.blQX);
		if (b->dims >= 2) an->SetFloat(b->paramY, w.blQY);
		an->Update();
	}

	// F = frame the rig
	if ((ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
	     || ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
	    && !ImGui::GetIO().WantTextInput && !ImGui::IsMouseDown(ImGuiMouseButton_Right)
	    && ImGui::IsKeyPressed(ImGuiKey_F) && w.pv)
		FramePreview(*w.pv, EditorRigAtom(w.pv, w.blAtomId));

	// ---- toolbar ---------------------------------------------------------------------------
	int dims = b->dims;
	ImGui::SetNextItemWidth(70);
	if (ImGui::Combo("##bldims", &dims, "1D\0002D\0"))
	{
		b->dims = dims == 0 ? 1 : 2;
		edited = structural = true;
	}
	ImGui::SameLine();
	char pxb[64]; strncpy(pxb, b->paramX.c_str(), sizeof(pxb)); pxb[sizeof(pxb) - 1] = 0;
	ImGui::SetNextItemWidth(110);
	if (ImGui::InputText("##blpx", pxb, sizeof(pxb))) { b->paramX = pxb; edited = structural = true; }
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("X parameter name");
	if (b->dims >= 2)
	{
		ImGui::SameLine();
		char pyb[64]; strncpy(pyb, b->paramY.c_str(), sizeof(pyb)); pyb[sizeof(pyb) - 1] = 0;
		ImGui::SetNextItemWidth(110);
		if (ImGui::InputText("##blpy", pyb, sizeof(pyb))) { b->paramY = pyb; edited = structural = true; }
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Y parameter name");
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(170);
	AssetPicker("##blskel", w.blRigSkel, "skeleton");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Preview rig skeleton (every mesh of it)");
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_SAVE " Save")) { b->SaveToFile(w.path); w.dirty = false; }

	// param range from the points (padded)
	float xmin = 1e9f, xmax = -1e9f, ymin = 1e9f, ymax = -1e9f;
	for (const BlendSpace::Point& p : b->points)
	{
		xmin = std::min(xmin, p.x); xmax = std::max(xmax, p.x);
		ymin = std::min(ymin, p.y); ymax = std::max(ymax, p.y);
	}
	if (b->points.empty()) { xmin = 0; xmax = 1; ymin = 0; ymax = 1; }
	const float xpad = std::max(0.25f, (xmax - xmin) * 0.15f);
	const float ypad = std::max(0.25f, (ymax - ymin) * 0.15f);
	xmin -= xpad; xmax += xpad;
	if (b->dims >= 2) { ymin -= ypad; ymax += ypad; } else { ymin = -1; ymax = 1; }

	// live weights for the marker query
	std::vector<float> weights;
	if (!b->points.empty()) b->Weights(w.blQX, b->dims >= 2 ? w.blQY : 0.0f, weights);

	// ---- layout: [canvas over preview] | right panel ---------------------------------------
	const float panelW = 300.0f;
	ImVec2 avail = ImGui::GetContentRegionAvail();
	const float pvH = w.blRigSkel.empty() ? 0.0f : 200.0f;
	ImGui::BeginChild("##blleft", ImVec2(avail.x - panelW - 6, 0), false,
	                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::BeginChild("##blcanvas", ImVec2(0, ImGui::GetContentRegionAvail().y - pvH - (pvH > 0 ? 6 : 0)), true,
	                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 cmin = ImGui::GetWindowPos();
		const ImVec2 csz = ImGui::GetWindowSize();
		dl->AddRectFilled(cmin, ImVec2(cmin.x + csz.x, cmin.y + csz.y), IM_COL32(24, 26, 30, 255));
		const float m = 34;   // axis margin
		const ImVec2 a0(cmin.x + m, cmin.y + csz.y - m);          // origin (bottom-left)
		const ImVec2 a1(cmin.x + csz.x - m, cmin.y + m);          // top-right
		auto toX = [&](float v) { return a0.x + (v - xmin) / (xmax - xmin) * (a1.x - a0.x); };
		auto toY = [&](float v)
		{
			if (b->dims < 2) return (a0.y + a1.y) * 0.5f;
			return a0.y + (v - ymin) / (ymax - ymin) * (a1.y - a0.y);
		};
		auto fromScreen = [&](ImVec2 p, float& vx, float& vy)
		{
			vx = xmin + (p.x - a0.x) / std::max(1.0f, a1.x - a0.x) * (xmax - xmin);
			vy = b->dims >= 2 ? ymin + (p.y - a0.y) / (a1.y - a0.y) * (ymax - ymin) : 0.0f;
		};

		// axes
		dl->AddLine(ImVec2(a0.x, a0.y), ImVec2(a1.x, a0.y), IM_COL32(90, 95, 105, 255));
		char lb[64];
		snprintf(lb, sizeof(lb), "%s  [%.2f .. %.2f]", b->paramX.c_str(), xmin, xmax);
		dl->AddText(ImVec2(a0.x, a0.y + 8), IM_COL32(150, 155, 165, 255), lb);
		if (b->dims >= 2)
		{
			dl->AddLine(ImVec2(a0.x, a0.y), ImVec2(a0.x, a1.y), IM_COL32(90, 95, 105, 255));
			snprintf(lb, sizeof(lb), "%s  [%.2f .. %.2f]", b->paramY.c_str(), ymin, ymax);
			dl->AddText(ImVec2(cmin.x + 4, a1.y - 18), IM_COL32(150, 155, 165, 255), lb);
		}
		else
			dl->AddLine(ImVec2(a0.x, (a0.y + a1.y) * 0.5f), ImVec2(a1.x, (a0.y + a1.y) * 0.5f),
			            IM_COL32(60, 63, 70, 255));

		// points (weight = ring thickness/alpha)
		for (int i = 0; i < (int)b->points.size(); ++i)
		{
			BlendSpace::Point& p = b->points[i];
			const ImVec2 c(toX(p.x), toY(p.y));
			const float wgt = i < (int)weights.size() ? weights[i] : 0.0f;
			const bool sel = (w.blSel == i);
			ImGui::PushID(i);
			ImGui::SetCursorScreenPos(ImVec2(c.x - 7, c.y - 7));
			ImGui::InvisibleButton("##pt", ImVec2(14, 14));
			if (ImGui::IsItemActivated()) w.blSel = i;
			if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
			{
				float vx, vy;
				fromScreen(ImGui::GetIO().MousePos, vx, vy);
				p.x = vx;
				if (b->dims >= 2) p.y = vy;
				edited = true;   // positions are read live by Weights() — no rebind needed
			}
			if (ImGui::BeginPopupContextItem("##ptctx"))
			{
				if (ImGui::MenuItem(ICON_LC_TRASH_2 " Remove Point"))
				{
					b->points.erase(b->points.begin() + i);
					if (w.blSel == i) w.blSel = -1;
					edited = structural = true;
				}
				ImGui::EndPopup();
			}
			const ImU32 col = sel ? IM_COL32(255, 200, 90, 255) : IM_COL32(150, 190, 240, 255);
			dl->AddCircleFilled(c, 5.0f, col);
			if (wgt > 0.001f)
				dl->AddCircle(c, 8.0f + wgt * 8.0f, IM_COL32(120, 230, 140, (int)(90 + wgt * 165)), 0, 2.0f);
			AnimClip* cl = ResDB::getSingleton()->GetClip(p.clip);
			std::string nm = cl ? cl->name : p.clip;
			if (wgt > 0.001f)
			{
				char wb[80];
				snprintf(wb, sizeof(wb), "%s  %.0f%%", nm.c_str(), wgt * 100.0f);
				nm = wb;
			}
			dl->AddText(ImVec2(c.x + 10, c.y - 7), IM_COL32(210, 214, 220, 255), nm.c_str());
			ImGui::PopID();
		}

		// query marker (drives the live parameters)
		{
			const ImVec2 c(toX(w.blQX), toY(b->dims >= 2 ? w.blQY : 0.0f));
			ImGui::SetCursorScreenPos(ImVec2(c.x - 8, c.y - 8));
			ImGui::InvisibleButton("##query", ImVec2(16, 16));
			if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
			{
				float vx, vy;
				fromScreen(ImGui::GetIO().MousePos, vx, vy);
				w.blQX = std::max(xmin, std::min(xmax, vx));
				if (b->dims >= 2) w.blQY = std::max(ymin, std::min(ymax, vy));
			}
			const ImU32 qc = IM_COL32(120, 230, 140, 255);
			dl->AddQuadFilled(ImVec2(c.x, c.y - 7), ImVec2(c.x + 7, c.y), ImVec2(c.x, c.y + 7), ImVec2(c.x - 7, c.y), qc);
			dl->AddQuad(ImVec2(c.x, c.y - 7), ImVec2(c.x + 7, c.y), ImVec2(c.x, c.y + 7), ImVec2(c.x - 7, c.y),
			            IM_COL32(20, 40, 25, 255));
		}

		// empty-canvas context: add a point at the click position
		ImGui::SetCursorScreenPos(cmin);
		ImGui::SetNextItemAllowOverlap();
		ImGui::InvisibleButton("##blbg", csz);
		if (ImGui::BeginPopupContextItem("##blbgctx"))
		{
			if (ImGui::MenuItem(ICON_LC_PLUS " Add Point"))
			{
				float vx, vy;
				fromScreen(ImGui::GetMousePosOnOpeningCurrentPopup(), vx, vy);
				BlendSpace::Point p;
				p.x = vx;
				p.y = b->dims >= 2 ? vy : 0.0f;
				b->points.push_back(p);
				w.blSel = (int)b->points.size() - 1;
				edited = structural = true;
			}
			ImGui::EndPopup();
		}
	}
	ImGui::EndChild();
	if (pvH > 0)
	{
		ImGui::BeginChild("##blpv", ImVec2(0, pvH), false,
		                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		if (w.pv)
		{
			DrawPreviewImage(*w.pv, ImGui::GetContentRegionAvail());
			EditorDrawBoneOverlay(*w.pv, EditorRigAtom(w.pv, w.blAtomId));
		}
		ImGui::EndChild();
	}
	ImGui::EndChild();   // ##blleft

	// ---- right panel -----------------------------------------------------------------------
	ImGui::SameLine();
	ImGui::BeginChild("##blpanel", ImVec2(panelW, 0), true);
	{
		ImGui::SeparatorText("Query");
		ImGui::SetNextItemWidth(-1);
		ImGui::SliderFloat(("##qx" + b->paramX).c_str(), &w.blQX, xmin, xmax, (b->paramX + " %.2f").c_str());
		if (b->dims >= 2)
		{
			ImGui::SetNextItemWidth(-1);
			ImGui::SliderFloat(("##qy" + b->paramY).c_str(), &w.blQY, ymin, ymax, (b->paramY + " %.2f").c_str());
		}

		if (w.blSel >= 0 && w.blSel < (int)b->points.size())
		{
			BlendSpace::Point& p = b->points[w.blSel];
			ImGui::SeparatorText("Point");
			std::string clip = p.clip;
			if (AssetPicker("Clip", clip, "anim")) { p.clip = clip; edited = structural = true; }
			if (ImGui::DragFloat("X", &p.x, 0.02f)) edited = true;
			if (b->dims >= 2 && ImGui::DragFloat("Y", &p.y, 0.02f)) edited = true;
			if (ImGui::DragFloat("Speed", &p.speed, 0.02f, 0.05f, 5.0f)) edited = structural = true;
			if (ImGui::Checkbox("Mirror", &p.mirror)) edited = structural = true;
			if (ImGui::Button(ICON_LC_TRASH_2 " Remove Point"))
			{
				b->points.erase(b->points.begin() + w.blSel);
				w.blSel = -1;
				edited = structural = true;
			}
		}
		else ImGui::TextDisabled("Select a point (right-click the canvas to add).");

		ImGui::SeparatorText("Weights");
		for (int i = 0; i < (int)b->points.size() && i < (int)weights.size(); ++i)
		{
			AnimClip* c = ResDB::getSingleton()->GetClip(b->points[i].clip);
			ImGui::Text("%s", (c ? c->name : b->points[i].clip).c_str());
			ImGui::SameLine(170);
			ImGui::ProgressBar(weights[i], ImVec2(-1, 12));
		}
	}
	ImGui::EndChild();

	if (structural && an)
	{
		// keep the owned controller's params in step with renamed axes
		w.blSm->params.clear();
		AnimSM::Param px2; px2.name = b->paramX; px2.type = 0; w.blSm->params.push_back(px2);
		if (b->dims >= 2) { AnimSM::Param py2; py2.name = b->paramY; py2.type = 0; w.blSm->params.push_back(py2); }
		an->SetPreviewController(w.blSm, b);
	}
	if (edited) { w.dirty = true; w.editedNow = true; }
}
