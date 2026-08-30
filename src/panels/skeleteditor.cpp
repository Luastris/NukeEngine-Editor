// .nuskel skeleton editor: rig preview with the bone overlay (selected bone / chain / socket
// highlighted), a bone-hierarchy tree, and CRUD panels for SOCKETS, bone GROUPS and the IK
// RIG (named chains) — the editing copy saves back to the file and hot-applies onto the
// live ResDB skeleton.
#include "editor/editorui.h"
#include "editor/animshared.h"
#include "API/Model/Animator.h"
#include "API/Model/SkinnedMeshRenderer.h"
#include "API/Model/Camera.h"
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cctype>

using namespace nuke;

namespace {

// Bone combo over the skeleton palette. Returns true on change.
bool BoneCombo(const char* label, const Skeleton* sk, std::string& bone)
{
	bool changed = false;
	if (ImGui::BeginCombo(label, bone.empty() ? "(bone)" : bone.c_str()))
	{
		for (const MeshBone& b : sk->bones)
			if (ImGui::Selectable(b.name.c_str(), bone == b.name)) { bone = b.name; changed = true; }
		ImGui::EndCombo();
	}
	return changed;
}

const ImU32 kChainColors[6] = {
	IM_COL32(110, 220, 110, 255), IM_COL32(110, 160, 240, 255), IM_COL32(230, 110, 110, 255),
	IM_COL32(220, 200, 90, 255), IM_COL32(200, 110, 220, 255), IM_COL32(90, 220, 210, 255),
};

}  // namespace

// ---- .nubonemap: clip-channel name -> skeleton bone name pairs -------------------------------

std::string EditorBoneMapJson(const BoneMap* b)
{
	nlohmann::json j;
	for (const auto& kv : b->map) j[kv.first] = kv.second;
	return j.dump();
}

void EditorBoneMapLoad(BoneMap* b, const std::string& json)
{
	try
	{
		nlohmann::json j = nlohmann::json::parse(json);
		b->map.clear();
		for (auto it = j.begin(); it != j.end(); ++it) b->map[it.key()] = it.value().get<std::string>();
	}
	catch (const std::exception&) {}
}

void EditorUI::DrawBoneMapEditor(AssetEditorWin& w)
{
	BoneMap* bm = w.bmap;
	if (!bm) { ImGui::TextDisabled("Failed to load bone map."); return; }
	bool edited = false;
	ResDB* db = ResDB::getSingleton();

	ImGui::SetNextItemWidth(220);
	AssetPicker("Source Skeleton", w.bmSrcSkel, "skeleton");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Optional: the skeleton the CLIPS were authored on (fills the left column)");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(220);
	AssetPicker("Target Skeleton", w.bmDstSkel, "skeleton");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("The skeleton the clips play on (fills the right column)");
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_SAVE " Save"))
	{
		bm->SaveToFile(w.path);
		if (BoneMap* live = db->GetBoneMap(bm->guid)) live->map = bm->map;
		w.dirty = false;
	}
	Skeleton* src = db->GetSkeleton(w.bmSrcSkel);
	Skeleton* dst = db->GetSkeleton(w.bmDstSkel);

	if (src && dst && ImGui::Button(ICON_LC_WAND_SPARKLES " Auto-fill exact names"))
	{
		for (const MeshBone& sb : src->bones)
			if (dst->BoneIndex(sb.name) >= 0 && !bm->map.count(sb.name)) bm->map[sb.name] = sb.name;
		edited = true;
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pairs bones whose names already match; the rest stay for you");

	ImGui::Separator();
	if (ImGui::BeginTable("##bmtable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
	                                      | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Clip channel");
		ImGui::TableSetupColumn("Skeleton bone");
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30.0f);
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();
		std::string killKey;
		int row = 0;
		for (auto& kv : bm->map)
		{
			ImGui::TableNextRow();
			ImGui::PushID(row++);
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(kv.first.c_str());
			ImGui::TableSetColumnIndex(1);
			ImGui::SetNextItemWidth(-1);
			if (dst)
			{
				if (BoneCombo("##dst", dst, kv.second)) edited = true;
			}
			else
			{
				char db2[64]; strncpy(db2, kv.second.c_str(), sizeof(db2)); db2[sizeof(db2) - 1] = 0;
				if (ImGui::InputText("##dsttxt", db2, sizeof(db2))) { kv.second = db2; edited = true; }
			}
			ImGui::TableSetColumnIndex(2);
			if (ImGui::SmallButton(ICON_LC_X)) killKey = kv.first;
			ImGui::PopID();
		}
		ImGui::EndTable();
		if (!killKey.empty()) { bm->map.erase(killKey); edited = true; }
	}

	// add a pair: source name (combo when a source skeleton is set) + target bone
	static std::string addSrc, addDst;
	ImGui::SetNextItemWidth(200);
	if (src)
	{
		if (ImGui::BeginCombo("##addsrc", addSrc.empty() ? "(clip channel)" : addSrc.c_str()))
		{
			for (const MeshBone& b : src->bones)
				if (!bm->map.count(b.name) && ImGui::Selectable(b.name.c_str(), addSrc == b.name)) addSrc = b.name;
			ImGui::EndCombo();
		}
	}
	else
	{
		char sb[64]; strncpy(sb, addSrc.c_str(), sizeof(sb)); sb[sizeof(sb) - 1] = 0;
		if (ImGui::InputTextWithHint("##addsrc", "clip channel name", sb, sizeof(sb))) addSrc = sb;
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(200);
	if (dst) BoneCombo("##adddst", dst, addDst);
	else
	{
		char tb[64]; strncpy(tb, addDst.c_str(), sizeof(tb)); tb[sizeof(tb) - 1] = 0;
		if (ImGui::InputTextWithHint("##adddst", "skeleton bone name", tb, sizeof(tb))) addDst = tb;
	}
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_PLUS " Pair") && !addSrc.empty() && !addDst.empty())
	{
		bm->map[addSrc] = addDst;
		addSrc.clear(); addDst.clear();
		edited = true;
	}

	if (edited) { w.dirty = true; w.editedNow = true; }
}

void EditorUI::DrawSkeletonEditor(AssetEditorWin& w)
{
	Skeleton* sk = w.skel;
	if (!sk) { ImGui::TextDisabled("Failed to load skeleton."); return; }
	bool edited = false;

	w.skRigSkel = sk->guid;   // the rig IS the skeleton being edited
	Animator* an = EditorEnsureRig(this, w.pv, w.skAtomId, w.skRigSkel);
	(void)an;   // the rig only poses the bind; no clip plays here
	Atom* rig = EditorRigAtom(w.pv, w.skAtomId);

	// F = frame
	if ((ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
	     || ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
	    && !ImGui::GetIO().WantTextInput && !ImGui::IsMouseDown(ImGuiMouseButton_Right)
	    && ImGui::IsKeyPressed(ImGuiKey_F) && w.pv)
		FramePreview(*w.pv, rig);

	// ---- toolbar ---------------------------------------------------------------------------
	ImGui::TextDisabled("%d bones", (int)sk->bones.size());
	ImGui::SameLine();
	ImGui::TextDisabled("%d meshes", (int)EditorMeshesForSkeleton(sk->guid).size());
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_SAVE " Save"))
	{
		sk->SaveToFile(w.path);
		// hot-apply the metadata onto the LIVE ResDB skeleton so IK/attachments see it now
		if (Skeleton* live = ResDB::getSingleton()->GetSkeleton(sk->guid))
		{
			live->sockets = sk->sockets;
			live->groups = sk->groups;
			live->chains = sk->chains;
		}
		w.dirty = false;
	}

	// ---- layout: preview | right panel -----------------------------------------------------
	const float panelW = 340.0f;
	ImVec2 avail = ImGui::GetContentRegionAvail();
	ImGui::BeginChild("##skview", ImVec2(avail.x - panelW - 6, 0), false,
	                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	if (w.pv && !EditorMeshesForSkeleton(w.skRigSkel).empty())
	{
		DrawPreviewImage(*w.pv, ImGui::GetContentRegionAvail());
		// overlay: bones (orange), the selected chain colored, the selected bone bright,
		// sockets cyan (selected bright).
		SkinnedMeshRenderer* smr = EditorRigSMR(rig);
		Skeleton* rsk = smr ? smr->EnsureSkeleton() : nullptr;
		EditorPvProj pj;
		if (rsk && smr->Globals().size() >= rsk->bones.size() * 16 && EditorMakeProjector(*w.pv, rig, pj))
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->PushClipRect(w.pv->rectMin,
			                 ImVec2(w.pv->rectMin.x + w.pv->rectSize.x, w.pv->rectMin.y + w.pv->rectSize.y), true);
			auto head = [&](int i) { const float* g = smr->Globals().data() + i * 16; return glm::vec3(g[12], g[13], g[14]); };
			// chain membership -> color index
			std::map<std::string, int> chainOf;
			for (int ci = 0; ci < (int)sk->chains.size(); ++ci)
				for (const std::string& bn : sk->chains[ci].bones) chainOf[bn] = ci;
			for (size_t i = 0; i < rsk->bones.size(); ++i)
			{
				ImVec2 a;
				if (!pj.Project(head((int)i), a)) continue;
				ImU32 col = IM_COL32(255, 155, 40, 200);
				auto it = chainOf.find(rsk->bones[i].name);
				if (it != chainOf.end())
				{
					col = kChainColors[it->second % 6];
					if (w.skSelChain >= 0 && it->second != w.skSelChain)
						col = (col & 0x00FFFFFF) | 0x60000000;   // other chains dimmed while one is selected
				}
				const bool selB = (w.skSelBone == (int)i);
				const int par = rsk->bones[i].parent;
				if (par >= 0)
				{
					ImVec2 b;
					if (pj.Project(head(par), b)) dl->AddLine(b, a, col, selB ? 2.5f : 1.5f);
				}
				dl->AddCircleFilled(a, selB ? 4.5f : 2.5f, selB ? IM_COL32(255, 240, 120, 255) : col);
				// click-pick a bone in the viewport
				if (!selB && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
				    && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
				{
					const ImVec2 mp = ImGui::GetIO().MousePos;
					const float dx = mp.x - a.x, dy = mp.y - a.y;
					if (dx * dx + dy * dy < 36.0f) w.skSelBone = (int)i;
				}
			}
			// sockets from the EDITING copy: bone global * socket local
			for (int si = 0; si < (int)sk->sockets.size(); ++si)
			{
				const SkeletonSocket& so = sk->sockets[si];
				const int bi = rsk->BoneIndex(so.bone);
				if (bi < 0) continue;
				const float* g = smr->Globals().data() + bi * 16;
				const glm::vec3 p = glm::vec3(glm::make_mat4(g)
				                              * glm::vec4(so.localPos[0], so.localPos[1], so.localPos[2], 1.0f));
				ImVec2 a;
				if (!pj.Project(p, a)) continue;
				const bool selS = (w.skSelSocket == si);
				dl->AddCircle(a, selS ? 6.0f : 4.0f, selS ? IM_COL32(120, 255, 255, 255) : IM_COL32(60, 200, 220, 220),
				              0, selS ? 2.5f : 1.5f);
				dl->AddText(ImVec2(a.x + 7, a.y - 7), IM_COL32(160, 230, 240, 220), so.name.c_str());
			}
			dl->PopClipRect();
		}
	}
	else ImGui::TextDisabled("No mesh is skinned to this skeleton yet — import one to preview it.");
	ImGui::EndChild();

	// ---- right panel -----------------------------------------------------------------------
	ImGui::SameLine();
	ImGui::BeginChild("##skpanel", ImVec2(panelW, 0), true);
	{
		// bone tree: filterable (ancestor paths to matches survive), edge-resizable height
		ImGui::SeparatorText("Bones");
		ImGui::SetNextItemWidth(-1);
		ImGui::InputTextWithHint("##skbonefilter", "filter bones", w.skBoneFilter, sizeof(w.skBoneFilter));
		std::string flt = w.skBoneFilter;
		std::transform(flt.begin(), flt.end(), flt.begin(), [](unsigned char c) { return (char)tolower(c); });
		ImGui::BeginChild("##skbones", ImVec2(0, w.skBonesH), ImGuiChildFlags_Borders,
		                  ImGuiWindowFlags_HorizontalScrollbar);
		{
			const int nb = (int)sk->bones.size();
			std::vector<std::vector<int>> kidsOf(nb);
			for (int i = 0; i < nb; ++i)
				if (sk->bones[i].parent >= 0 && sk->bones[i].parent < nb) kidsOf[sk->bones[i].parent].push_back(i);
			// keep = matches itself or holds a match below (the path to a match stays visible)
			std::vector<char> keep(nb, 1);
			if (!flt.empty())
			{
				std::function<bool(int)> mark = [&](int bi) -> bool
				{
					std::string nm = sk->bones[bi].name;
					std::transform(nm.begin(), nm.end(), nm.begin(), [](unsigned char c) { return (char)tolower(c); });
					bool k = nm.find(flt) != std::string::npos;
					for (int c : kidsOf[bi]) k = mark(c) || k;
					keep[bi] = k ? 1 : 0;
					return k;
				};
				for (int i = 0; i < nb; ++i)
					if (sk->bones[i].parent < 0) mark(i);
			}
			std::function<void(int)> drawBone = [&](int bi)
			{
				if (!keep[bi]) return;
				const MeshBone& b = sk->bones[bi];
				bool anyKid = false;
				for (int k : kidsOf[bi]) anyKid = anyKid || keep[k];
				ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen
				                      | ImGuiTreeNodeFlags_SpanAvailWidth
				                      | (!anyKid ? ImGuiTreeNodeFlags_Leaf : 0)
				                      | (w.skSelBone == bi ? ImGuiTreeNodeFlags_Selected : 0);
				if (!flt.empty()) ImGui::SetNextItemOpen(true);   // filtering opens the paths
				const bool open = ImGui::TreeNodeEx((void*)(intptr_t)bi, fl, ICON_LC_BONE " %s", b.name.c_str());
				if (ImGui::IsItemClicked()) w.skSelBone = bi;
				if (open)
				{
					for (int k : kidsOf[bi]) drawBone(k);
					ImGui::TreePop();
				}
			};
			for (int i = 0; i < nb; ++i)
				if (sk->bones[i].parent < 0) drawBone(i);
		}
		ImGui::EndChild();
		ImGui::InvisibleButton("##skbones_split", ImVec2(std::max(ImGui::GetContentRegionAvail().x, 1.0f), 6.0f));
		if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
		if (ImGui::IsItemActive())
		{
			w.skBonesH += ImGui::GetIO().MouseDelta.y;
			if (w.skBonesH < 80.0f)   w.skBonesH = 80.0f;
			if (w.skBonesH > 1600.0f) w.skBonesH = 1600.0f;
		}

		// sockets
		ImGui::SeparatorText("Sockets");
		for (int i = 0; i < (int)sk->sockets.size(); ++i)
		{
			ImGui::PushID(i);
			if (ImGui::Selectable(sk->sockets[i].name.c_str(), w.skSelSocket == i)) w.skSelSocket = i;
			ImGui::PopID();
		}
		if (ImGui::SmallButton(ICON_LC_PLUS " Socket"))
		{
			SkeletonSocket so;
			so.name = "socket" + std::to_string(sk->sockets.size());
			so.bone = (w.skSelBone >= 0 && w.skSelBone < (int)sk->bones.size())
			        ? sk->bones[w.skSelBone].name : (sk->bones.empty() ? "" : sk->bones[0].name);
			sk->sockets.push_back(so);
			w.skSelSocket = (int)sk->sockets.size() - 1;
			edited = true;
		}
		if (w.skSelSocket >= 0 && w.skSelSocket < (int)sk->sockets.size())
		{
			SkeletonSocket& so = sk->sockets[w.skSelSocket];
			char nb[64]; strncpy(nb, so.name.c_str(), sizeof(nb)); nb[sizeof(nb) - 1] = 0;
			if (ImGui::InputText("Name##sock", nb, sizeof(nb))) { so.name = nb; edited = true; }
			if (BoneCombo("Bone##sock", sk, so.bone)) edited = true;
			if (ImGui::DragFloat3("Position", so.localPos, 0.01f)) edited = true;
			if (ImGui::DragFloat4("Rotation", so.localRot, 0.01f)) edited = true;
			if (ImGui::DragFloat3("Scale", so.localScale, 0.01f)) edited = true;
			if (ImGui::SmallButton(ICON_LC_TRASH_2 " Remove Socket"))
			{
				sk->sockets.erase(sk->sockets.begin() + w.skSelSocket);
				w.skSelSocket = -1;
				edited = true;
			}
		}

		// groups (layer masks)
		ImGui::SeparatorText("Groups");
		for (int i = 0; i < (int)sk->groups.size(); ++i)
		{
			ImGui::PushID(100 + i);
			const std::string label = sk->groups[i].name + " (" + std::to_string(sk->groups[i].bones.size()) + ")";
			if (ImGui::Selectable(label.c_str(), w.skSelGroup == i)) w.skSelGroup = i;
			ImGui::PopID();
		}
		if (ImGui::SmallButton(ICON_LC_PLUS " Group"))
		{
			SkeletonGroup g;
			g.name = "Group" + std::to_string(sk->groups.size());
			sk->groups.push_back(g);
			w.skSelGroup = (int)sk->groups.size() - 1;
			edited = true;
		}
		if (w.skSelGroup >= 0 && w.skSelGroup < (int)sk->groups.size())
		{
			SkeletonGroup& g = sk->groups[w.skSelGroup];
			char gb[64]; strncpy(gb, g.name.c_str(), sizeof(gb)); gb[sizeof(gb) - 1] = 0;
			if (ImGui::InputText("Name##grp", gb, sizeof(gb))) { g.name = gb; edited = true; }
			ImGui::BeginChild("##grpbones", ImVec2(0, 130), ImGuiChildFlags_Borders);
			for (const MeshBone& b : sk->bones)
			{
				bool in = std::find(g.bones.begin(), g.bones.end(), b.name) != g.bones.end();
				if (ImGui::Checkbox(b.name.c_str(), &in))
				{
					if (in) g.bones.push_back(b.name);
					else g.bones.erase(std::find(g.bones.begin(), g.bones.end(), b.name));
					edited = true;
				}
			}
			ImGui::EndChild();
			if (ImGui::SmallButton(ICON_LC_TRASH_2 " Remove Group"))
			{
				sk->groups.erase(sk->groups.begin() + w.skSelGroup);
				w.skSelGroup = -1;
				edited = true;
			}
		}

		// IK rig: named chains, root -> tip
		ImGui::SeparatorText("IK Rig (chains)");
		for (int i = 0; i < (int)sk->chains.size(); ++i)
		{
			ImGui::PushID(200 + i);
			ImGui::PushStyleColor(ImGuiCol_Text, kChainColors[i % 6]);
			const std::string label = sk->chains[i].name + " (" + std::to_string(sk->chains[i].bones.size()) + ")";
			if (ImGui::Selectable(label.c_str(), w.skSelChain == i)) w.skSelChain = (w.skSelChain == i) ? -1 : i;
			ImGui::PopStyleColor();
			ImGui::PopID();
		}
		if (ImGui::SmallButton(ICON_LC_PLUS " Chain"))
		{
			SkeletonChain c;
			c.name = "Chain" + std::to_string(sk->chains.size());
			sk->chains.push_back(c);
			w.skSelChain = (int)sk->chains.size() - 1;
			edited = true;
		}
		if (w.skSelChain >= 0 && w.skSelChain < (int)sk->chains.size())
		{
			SkeletonChain& c = sk->chains[w.skSelChain];
			char cb[64]; strncpy(cb, c.name.c_str(), sizeof(cb)); cb[sizeof(cb) - 1] = 0;
			if (ImGui::InputText("Name##chain", cb, sizeof(cb))) { c.name = cb; edited = true; }
			for (int i = 0; i < (int)c.bones.size(); ++i)
			{
				ImGui::PushID(300 + i);
				ImGui::TextUnformatted(c.bones[i].c_str());
				ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70);
				if (ImGui::SmallButton(ICON_LC_ARROW_UP) && i > 0) { std::swap(c.bones[i], c.bones[i - 1]); edited = true; }
				ImGui::SameLine();
				if (ImGui::SmallButton(ICON_LC_ARROW_DOWN) && i + 1 < (int)c.bones.size()) { std::swap(c.bones[i], c.bones[i + 1]); edited = true; }
				ImGui::SameLine();
				if (ImGui::SmallButton(ICON_LC_X))
				{
					c.bones.erase(c.bones.begin() + i);
					edited = true;
					ImGui::PopID();
					break;
				}
				ImGui::PopID();
			}
			const bool haveSel = w.skSelBone >= 0 && w.skSelBone < (int)sk->bones.size();
			if (!haveSel) ImGui::BeginDisabled();
			if (ImGui::SmallButton(ICON_LC_PLUS " Add Selected Bone") && haveSel)
			{
				c.bones.push_back(sk->bones[w.skSelBone].name);
				edited = true;
			}
			if (!haveSel) ImGui::EndDisabled();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Appends the bone selected in the tree/viewport (order = root -> tip)");
			if (ImGui::SmallButton(ICON_LC_TRASH_2 " Remove Chain"))
			{
				sk->chains.erase(sk->chains.begin() + w.skSelChain);
				w.skSelChain = -1;
				edited = true;
			}
		}
	}
	ImGui::EndChild();

	if (edited) { w.dirty = true; w.editedNow = true; }
}
