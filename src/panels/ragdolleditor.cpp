// .nurag ragdoll editor: the rig preview with WIREFRAME CAPSULES projected over the bind
// pose (selected body highlighted), body/joint inspectors, Add/Remove body, and a Rebuild
// that re-runs the importer's auto-fit against the skinned mesh. Saving hot-applies onto
// the live ResDB rig.
#include "editor/editorui.h"
#include "editor/animshared.h"
#include "API/Model/Animator.h"
#include "API/Model/SkinnedMeshRenderer.h"
#include "API/Model/Ragdoll.h"
#include "API/Model/Camera.h"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>

using namespace nuke;

namespace {

// Project a bone-local capsule as 3 rings + 4 side lines through the preview projector.
void DrawCapsule(ImDrawList* dl, const EditorPvProj& pj, const glm::mat4& boneG,
                 const RagdollDef::Body& b, ImU32 col, float th)
{
	const glm::vec3 axis = glm::normalize(glm::vec3(b.axis[0], b.axis[1], b.axis[2]));
	const glm::vec3 c(b.center[0], b.center[1], b.center[2]);
	glm::vec3 u = fabsf(axis.y) < 0.99f ? glm::normalize(glm::cross(axis, glm::vec3(0, 1, 0)))
	                                    : glm::vec3(1, 0, 0);
	glm::vec3 v = glm::normalize(glm::cross(axis, u));
	auto ring = [&](const glm::vec3& center)
	{
		ImVec2 prev;
		bool prevOk = false;
		for (int s = 0; s <= 12; ++s)
		{
			const float ang = (float)s / 12.0f * 6.2831853f;
			const glm::vec3 pl = center + (u * cosf(ang) + v * sinf(ang)) * b.radius;
			ImVec2 sp;
			const bool ok = pj.Project(glm::vec3(boneG * glm::vec4(pl, 1.0f)), sp);
			if (ok && prevOk) dl->AddLine(prev, sp, col, th);
			prev = sp;
			prevOk = ok;
		}
	};
	const glm::vec3 top = c + axis * b.halfHeight, bot = c - axis * b.halfHeight;
	ring(c); ring(top); ring(bot);
	for (int s = 0; s < 4; ++s)
	{
		const float ang = (float)s / 4.0f * 6.2831853f;
		const glm::vec3 off = (u * cosf(ang) + v * sinf(ang)) * b.radius;
		ImVec2 a, e;
		if (pj.Project(glm::vec3(boneG * glm::vec4(top + off, 1.0f)), a)
		    && pj.Project(glm::vec3(boneG * glm::vec4(bot + off, 1.0f)), e))
			dl->AddLine(a, e, col, th);
	}
}

}  // namespace

void EditorUI::DrawRagdollEditor(AssetEditorWin& w)
{
	RagdollDef* rd = w.rag;
	if (!rd) { ImGui::TextDisabled("Failed to load ragdoll."); return; }
	bool edited = false;
	ResDB* db = ResDB::getSingleton();
	Skeleton* sk = db->GetSkeleton(rd->skelGuid);

	w.rgRigSkel = rd->skelGuid;   // the rig IS the ragdoll's skeleton
	EditorEnsureRig(this, w.pv, w.rgAtomId, w.rgRigSkel);
	Atom* rig = EditorRigAtom(w.pv, w.rgAtomId);

	// F = frame
	if ((ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
	     || ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
	    && !ImGui::GetIO().WantTextInput && !ImGui::IsMouseDown(ImGuiMouseButton_Right)
	    && ImGui::IsKeyPressed(ImGuiKey_F) && w.pv)
		FramePreview(*w.pv, rig);

	// ---- toolbar ---------------------------------------------------------------------------
	ImGui::TextDisabled("%d bodies / %d joints", (int)rd->bodies.size(), (int)rd->joints.size());
	ImGui::SameLine();
	ImGui::TextDisabled("%d meshes", (int)EditorMeshesForSkeleton(rd->skelGuid).size());
	ImGui::SameLine();
	ImGui::SetNextItemWidth(90);
	static float rebuildMass = 70.0f;
	ImGui::DragFloat("##rgmass", &rebuildMass, 0.5f, 1.0f, 500.0f, "%.0f kg");
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_REFRESH_CW " Rebuild"))
	{
		Mesh* m = db->GetMesh(EditorAutoMeshForSkeleton(rd->skelGuid));   // fit on the main body mesh
		if (sk && m)
			if (RagdollDef* fresh = RagdollDef::Build(sk, m, rebuildMass))
			{
				rd->bodies = fresh->bodies;
				rd->joints = fresh->joints;
				delete fresh;
				w.rgSelBody = -1;
				edited = true;
			}
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Re-run the importer's capsule auto-fit from the skin weights");
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_SAVE " Save"))
	{
		rd->SaveToFile(w.path);
		if (RagdollDef* live = db->GetRagdoll(rd->guid))   // hot-apply for the next Activate
		{
			live->bodies = rd->bodies;
			live->joints = rd->joints;
		}
		w.dirty = false;
	}

	// ---- layout: preview | right panel -----------------------------------------------------
	const float panelW = 320.0f;
	ImVec2 avail = ImGui::GetContentRegionAvail();
	ImGui::BeginChild("##rgview", ImVec2(avail.x - panelW - 6, 0), false,
	                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	if (w.pv && !EditorMeshesForSkeleton(w.rgRigSkel).empty())
	{
		DrawPreviewImage(*w.pv, ImGui::GetContentRegionAvail());
		EditorDrawBoneOverlay(*w.pv, rig);
		SkinnedMeshRenderer* smr = EditorRigSMR(rig);
		Skeleton* rsk = smr ? smr->EnsureSkeleton() : nullptr;
		EditorPvProj pj;
		if (rsk && smr->Globals().size() >= rsk->bones.size() * 16 && EditorMakeProjector(*w.pv, rig, pj))
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->PushClipRect(w.pv->rectMin,
			                 ImVec2(w.pv->rectMin.x + w.pv->rectSize.x, w.pv->rectMin.y + w.pv->rectSize.y), true);
			for (int i = 0; i < (int)rd->bodies.size(); ++i)
			{
				const RagdollDef::Body& b = rd->bodies[i];
				const int bi = rsk->BoneIndex(b.bone);
				if (bi < 0) continue;
				const glm::mat4 G = glm::make_mat4(smr->Globals().data() + bi * 16);
				const bool sel = (w.rgSelBody == i);
				DrawCapsule(dl, pj, G, b,
				            sel ? IM_COL32(255, 230, 90, 255) : IM_COL32(120, 220, 130, 190),
				            sel ? 2.0f : 1.0f);
			}
			dl->PopClipRect();
		}
	}
	else ImGui::TextDisabled("No mesh is skinned to this ragdoll's skeleton.");
	ImGui::EndChild();

	// ---- right panel -----------------------------------------------------------------------
	ImGui::SameLine();
	ImGui::BeginChild("##rgpanel", ImVec2(panelW, 0), true);
	{
		ImGui::SeparatorText("Bodies");
		for (int i = 0; i < (int)rd->bodies.size(); ++i)
		{
			ImGui::PushID(i);
			char lbl[96];
			snprintf(lbl, sizeof(lbl), "%s  (r %.2f, m %.1f)", rd->bodies[i].bone.c_str(),
			         rd->bodies[i].radius, rd->bodies[i].mass);
			if (ImGui::Selectable(lbl, w.rgSelBody == i)) w.rgSelBody = i;
			ImGui::PopID();
		}
		// add a body for a bone that has none yet
		if (sk)
		{
			static std::string addBone;
			ImGui::SetNextItemWidth(-70);
			if (ImGui::BeginCombo("##rgaddbone", addBone.empty() ? "(bone)" : addBone.c_str()))
			{
				for (const MeshBone& b : sk->bones)
				{
					bool has = false;
					for (const RagdollDef::Body& rb : rd->bodies) has = has || rb.bone == b.name;
					if (!has && ImGui::Selectable(b.name.c_str(), addBone == b.name)) addBone = b.name;
				}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			if (ImGui::Button(ICON_LC_PLUS " Body") && !addBone.empty())
			{
				RagdollDef::Body nb;
				nb.bone = addBone;
				rd->bodies.push_back(nb);
				RagdollDef::Joint nj;
				nj.bone = addBone;
				rd->joints.push_back(nj);
				w.rgSelBody = (int)rd->bodies.size() - 1;
				addBone.clear();
				edited = true;
			}
		}

		if (w.rgSelBody >= 0 && w.rgSelBody < (int)rd->bodies.size())
		{
			RagdollDef::Body& b = rd->bodies[w.rgSelBody];
			ImGui::SeparatorText(("Body: " + b.bone).c_str());
			if (ImGui::DragFloat("Radius", &b.radius, 0.005f, 0.005f, 5.0f)) edited = true;
			if (ImGui::DragFloat("Half Height", &b.halfHeight, 0.005f, 0.005f, 5.0f)) edited = true;
			if (ImGui::DragFloat3("Center", b.center, 0.005f)) edited = true;
			if (ImGui::DragFloat3("Axis", b.axis, 0.01f))
			{
				const float l = sqrtf(b.axis[0] * b.axis[0] + b.axis[1] * b.axis[1] + b.axis[2] * b.axis[2]);
				if (l > 1e-4f) { b.axis[0] /= l; b.axis[1] /= l; b.axis[2] /= l; }
				edited = true;
			}
			if (ImGui::DragFloat("Mass", &b.mass, 0.1f, 0.05f, 500.0f, "%.1f kg")) edited = true;
			// the joint hanging this body under its bodied ancestor
			int ji = -1;
			for (int i = 0; i < (int)rd->joints.size(); ++i)
				if (rd->joints[i].bone == b.bone) { ji = i; break; }
			if (ji >= 0)
			{
				RagdollDef::Joint& j = rd->joints[ji];
				ImGui::SeparatorText("Joint limits (rad)");
				if (ImGui::DragFloatRange2("Twist", &j.twistMin, &j.twistMax, 0.01f, -3.14f, 3.14f)) edited = true;
				if (ImGui::DragFloat("Swing 1", &j.swing1, 0.01f, 0.0f, 3.14f)) edited = true;
				if (ImGui::DragFloat("Swing 2", &j.swing2, 0.01f, 0.0f, 3.14f)) edited = true;
			}
			else ImGui::TextDisabled("Root body (no joint).");
			if (ImGui::SmallButton(ICON_LC_TRASH_2 " Remove Body"))
			{
				for (int i = (int)rd->joints.size() - 1; i >= 0; --i)
					if (rd->joints[i].bone == b.bone) rd->joints.erase(rd->joints.begin() + i);
				rd->bodies.erase(rd->bodies.begin() + w.rgSelBody);
				w.rgSelBody = -1;
				edited = true;
			}
		}
		else ImGui::TextDisabled("Select a body (list or viewport capsule).");
	}
	ImGui::EndChild();

	if (edited) { w.dirty = true; w.editedNow = true; }
}
