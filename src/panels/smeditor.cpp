// .nusm node-graph editor: states as draggable nodes on a pannable canvas, transitions as
// clickable arrows, sub-machine navigation (breadcrumbs), a parameter/inspector side panel,
// and a LIVE preview rig driven by the EDITING copy (Animator::SetPreviewController) with
// the active state highlighted.
#include "editor/editorui.h"
#include "editor/animshared.h"
#include "API/Model/Animator.h"
#include "API/Model/SkinnedMeshRenderer.h"
#include "API/Model/Camera.h"
#include <algorithm>

using namespace nuke;

namespace {

// The states/transitions/entry container the canvas edits: the layer root or a sub-machine.
struct SmScope
{
	std::vector<AnimSM::State>*      states = nullptr;
	std::vector<AnimSM::Transition>* trans = nullptr;
	std::string* entry = nullptr;
};

SmScope ResolveScope(AnimSM* sm, int layer, const std::vector<std::string>& path)
{
	SmScope sc;
	if (layer < 0 || layer >= (int)sm->layers.size()) return sc;
	AnimSM::Layer& l = sm->layers[layer];
	sc.states = &l.states; sc.trans = &l.transitions; sc.entry = &l.entry;
	for (const std::string& name : path)
	{
		AnimSM::State* sub = nullptr;
		for (AnimSM::State& s : *sc.states)
			if (s.name == name && !s.states.empty()) { sub = &s; break; }
		if (!sub) break;
		sc.states = &sub->states; sc.trans = &sub->transitions; sc.entry = &sub->entry;
	}
	return sc;
}

AnimSM::State* FindState(SmScope& sc, const std::string& name)
{
	if (!sc.states) return nullptr;
	for (AnimSM::State& s : *sc.states)
		if (s.name == name) return &s;
	return nullptr;
}

// Unset (0,0) positions -> a simple column layout by BFS depth from the entry state.
void AutoLayout(SmScope& sc)
{
	if (!sc.states || sc.states->empty()) return;
	bool anySet = false;
	for (const AnimSM::State& s : *sc.states)
		if (s.nx != 0.0f || s.ny != 0.0f) { anySet = true; break; }
	if (anySet) return;
	std::map<std::string, int> depth;
	std::vector<std::string> queue;
	if (!sc.entry->empty()) { depth[*sc.entry] = 0; queue.push_back(*sc.entry); }
	for (size_t qi = 0; qi < queue.size(); ++qi)
		for (const AnimSM::Transition& t : *sc.trans)
			if (t.from == queue[qi] && !depth.count(t.to))
			{
				depth[t.to] = depth[queue[qi]] + 1;
				queue.push_back(t.to);
			}
	std::map<int, int> rowAt;
	int orphanRow = 0;
	for (AnimSM::State& s : *sc.states)
	{
		int d = depth.count(s.name) ? depth[s.name] : (3 + (orphanRow++ % 3));
		s.nx = 60.0f + d * 200.0f;
		s.ny = 60.0f + (rowAt[d]++) * 90.0f;
	}
}

// Rename a state everywhere its old name is referenced in the CURRENT scope.
void RenameStateRefs(SmScope& sc, const std::string& oldName, const std::string& newName)
{
	for (AnimSM::Transition& t : *sc.trans)
	{
		if (t.from == oldName) t.from = newName;
		if (t.to == oldName) t.to = newName;
	}
	if (*sc.entry == oldName) *sc.entry = newName;
}

float DistToSegment(ImVec2 p, ImVec2 a, ImVec2 b)
{
	const float abx = b.x - a.x, aby = b.y - a.y;
	const float len2 = abx * abx + aby * aby;
	float t = len2 > 1e-6f ? ((p.x - a.x) * abx + (p.y - a.y) * aby) / len2 : 0.0f;
	t = std::max(0.0f, std::min(1.0f, t));
	const float dx = p.x - (a.x + abx * t), dy = p.y - (a.y + aby * t);
	return sqrtf(dx * dx + dy * dy);
}

const char* kOps = "greater\0less\0equals\0not equals\0is true\0is false\0trigger set\0";

}  // namespace

void EditorUI::DrawSMEditor(AssetEditorWin& w)
{
	AnimSM* sm = w.sm;
	if (!sm) { ImGui::TextDisabled("Failed to load controller."); return; }
	bool edited = false;         // metadata edit (undo latch)
	bool structural = false;     // graph must rebind (SetPreviewController)

	if (sm->layers.empty()) { sm->layers.push_back(AnimSM::Layer{ "Base" }); structural = true; }
	w.smLayer = std::max(0, std::min(w.smLayer, (int)sm->layers.size() - 1));
	SmScope sc = ResolveScope(sm, w.smLayer, w.smPath);
	if (!sc.states) { ImGui::TextDisabled("Bad layer."); return; }
	AutoLayout(sc);

	// ---- preview rig ----------------------------------------------------------------------
	if (w.smRigSkel.empty())
		for (const AnimSM::Layer& l : sm->layers)
		{
			std::function<std::string(const std::vector<AnimSM::State>&)> firstClipSkel =
				[&](const std::vector<AnimSM::State>& states) -> std::string
			{
				for (const AnimSM::State& s : states)
				{
					if (!s.motion.empty())
					{
						if (AnimClip* c = ResDB::getSingleton()->GetClip(s.motion))
							if (!c->skelGuid.empty()) return c->skelGuid;
						if (BlendSpace* b = ResDB::getSingleton()->GetBlendSpace(s.motion))
							for (const BlendSpace::Point& p : b->points)
								if (AnimClip* c = ResDB::getSingleton()->GetClip(p.clip))
									if (!c->skelGuid.empty()) return c->skelGuid;
					}
					if (!s.states.empty())
					{
						std::string r = firstClipSkel(s.states);
						if (!r.empty()) return r;
					}
				}
				return "";
			};
			const std::string m = firstClipSkel(l.states);
			if (!m.empty()) { w.smRigSkel = m; break; }
		}
	Animator* an = EditorEnsureRig(this, w.pv, w.smAtomId, w.smRigSkel);
	if (an)
	{
		if (an->previewSm != sm) { an->smGuid = sm->guid; an->SetPreviewController(sm); }
		an->playOnStart = true;
		an->Update();   // live controller: parameters drive transitions in real time
	}
	const std::string liveState = an ? an->State() : std::string();

	// F = frame the rig
	if ((ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
	     || ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
	    && !ImGui::GetIO().WantTextInput && !ImGui::IsMouseDown(ImGuiMouseButton_Right)
	    && ImGui::IsKeyPressed(ImGuiKey_F) && w.pv)
		FramePreview(*w.pv, EditorRigAtom(w.pv, w.smAtomId));

	// ---- toolbar --------------------------------------------------------------------------
	if ((int)sm->layers.size() > 1 || true)
	{
		ImGui::SetNextItemWidth(130);
		if (ImGui::BeginCombo("##smlayer", ("Layer: " + sm->layers[w.smLayer].name).c_str()))
		{
			for (int i = 0; i < (int)sm->layers.size(); ++i)
				if (ImGui::Selectable(sm->layers[i].name.c_str(), i == w.smLayer))
				{
					w.smLayer = i;
					w.smPath.clear();
					w.smSelState.clear(); w.smSelTrans = -1;
				}
			ImGui::EndCombo();
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_PLUS " Layer"))
	{
		AnimSM::Layer l;
		l.name = "Layer" + std::to_string(sm->layers.size());
		sm->layers.push_back(l);
		w.smLayer = (int)sm->layers.size() - 1;
		w.smPath.clear();
		edited = structural = true;
	}
	// breadcrumbs
	ImGui::SameLine();
	if (ImGui::SmallButton(sm->layers[w.smLayer].name.c_str())) { w.smPath.clear(); w.smSelState.clear(); w.smSelTrans = -1; }
	for (size_t i = 0; i < w.smPath.size(); ++i)
	{
		ImGui::SameLine(); ImGui::TextDisabled(">"); ImGui::SameLine();
		if (ImGui::SmallButton(w.smPath[i].c_str()))
		{
			w.smPath.resize(i + 1);
			w.smSelState.clear(); w.smSelTrans = -1;
		}
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(170);
	AssetPicker("##smskel", w.smRigSkel, "skeleton");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Preview rig skeleton (every mesh of it)");
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_SAVE " Save")) { sm->SaveToFile(w.path); w.dirty = false; }

	// ---- layout: [canvas over preview] | right panel ---------------------------------------
	const float panelW = 320.0f;
	ImVec2 avail = ImGui::GetContentRegionAvail();
	const float pvH = w.smRigSkel.empty() ? 0.0f : 170.0f;
	ImGui::BeginChild("##smleft", ImVec2(avail.x - panelW - 6, 0), false,
	                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::BeginChild("##smcanvas", ImVec2(0, ImGui::GetContentRegionAvail().y - pvH - (pvH > 0 ? 6 : 0)), true,
	                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 cmin = ImGui::GetWindowPos();
		const ImVec2 csz = ImGui::GetWindowSize();
		dl->AddRectFilled(cmin, ImVec2(cmin.x + csz.x, cmin.y + csz.y), IM_COL32(24, 26, 30, 255));
		// grid
		const float grid = 24.0f * w.smZoom;
		for (float gx = fmodf(w.smPanX, grid); gx < csz.x; gx += grid)
			dl->AddLine(ImVec2(cmin.x + gx, cmin.y), ImVec2(cmin.x + gx, cmin.y + csz.y), IM_COL32(38, 40, 46, 255));
		for (float gy = fmodf(w.smPanY, grid); gy < csz.y; gy += grid)
			dl->AddLine(ImVec2(cmin.x, cmin.y + gy), ImVec2(cmin.x + csz.x, cmin.y + gy), IM_COL32(38, 40, 46, 255));

		auto toScreen = [&](float nx, float ny)
		{ return ImVec2(cmin.x + w.smPanX + nx * w.smZoom, cmin.y + w.smPanY + ny * w.smZoom); };
		auto toCanvas = [&](ImVec2 p)
		{ return ImVec2((p.x - cmin.x - w.smPanX) / w.smZoom, (p.y - cmin.y - w.smPanY) / w.smZoom); };
		const ImVec2 nodeSz(150.0f * w.smZoom, 46.0f * w.smZoom);

		auto nodeRect = [&](const AnimSM::State& s, ImVec2& a, ImVec2& b)
		{
			a = toScreen(s.nx, s.ny);
			b = ImVec2(a.x + nodeSz.x, a.y + nodeSz.y);
		};
		auto nodeCenter = [&](const std::string& name, ImVec2& c) -> bool
		{
			if (name.empty() || name == "*")   // Any State pseudo-node
			{
				c = ImVec2(cmin.x + 70, cmin.y + 34);
				return true;
			}
			if (AnimSM::State* s = FindState(sc, name))
			{
				ImVec2 a, b; nodeRect(*s, a, b);
				c = ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
				return true;
			}
			return false;
		};

		// transitions (arrows); offset doubled edges so A->B / B->A both show
		int transHit = -1;
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		for (int i = 0; i < (int)sc.trans->size(); ++i)
		{
			const AnimSM::Transition& t = (*sc.trans)[i];
			ImVec2 a, b;
			if (!nodeCenter(t.from, a) || !nodeCenter(t.to, b)) continue;
			// perpendicular offset (stacked edges fan out)
			int stack = 0;
			for (int j = 0; j < i; ++j)
				if (((*sc.trans)[j].from == t.from && (*sc.trans)[j].to == t.to)
				    || ((*sc.trans)[j].from == t.to && (*sc.trans)[j].to == t.from)) ++stack;
			ImVec2 d(b.x - a.x, b.y - a.y);
			const float len = sqrtf(d.x * d.x + d.y * d.y);
			if (len < 1.0f) continue;
			const ImVec2 n(-d.y / len, d.x / len);
			const float off = 8.0f + stack * 10.0f;
			a.x += n.x * off; a.y += n.y * off;
			b.x += n.x * off; b.y += n.y * off;
			const bool sel = (w.smSelTrans == i);
			const ImU32 col = sel ? IM_COL32(255, 200, 90, 255) : IM_COL32(150, 155, 165, 210);
			dl->AddLine(a, b, col, sel ? 2.5f : 1.5f);
			// arrowhead at the middle
			const ImVec2 mid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
			const ImVec2 dir(d.x / len, d.y / len);
			const ImVec2 p1(mid.x - dir.x * 8 + n.x * 5, mid.y - dir.y * 8 + n.y * 5);
			const ImVec2 p2(mid.x - dir.x * 8 - n.x * 5, mid.y - dir.y * 8 - n.y * 5);
			dl->AddTriangleFilled(mid, p1, p2, col);
			if (DistToSegment(mouse, a, b) < 6.0f) transHit = i;
		}

		// Any State pseudo-node
		{
			const ImVec2 c(cmin.x + 70, cmin.y + 34);
			dl->AddRectFilled(ImVec2(c.x - 55, c.y - 16), ImVec2(c.x + 55, c.y + 16), IM_COL32(70, 55, 40, 255), 6);
			dl->AddRect(ImVec2(c.x - 55, c.y - 16), ImVec2(c.x + 55, c.y + 16), IM_COL32(190, 150, 90, 255), 6);
			dl->AddText(ImVec2(c.x - 34, c.y - 8), IM_COL32(230, 205, 160, 255), "Any State");
			ImGui::SetCursorScreenPos(ImVec2(c.x - 55, c.y - 16));
			ImGui::InvisibleButton("##anystate", ImVec2(110, 32));
			if (ImGui::BeginPopupContextItem("##anystatectx"))
			{
				if (ImGui::MenuItem(ICON_LC_ARROW_RIGHT " Add Transition")) { w.smLink = true; w.smLinkFrom = "*"; }
				ImGui::EndPopup();
			}
		}

		// nodes
		std::string dblOpen;
		for (AnimSM::State& s : *sc.states)
		{
			ImVec2 a, b; nodeRect(s, a, b);
			const bool sel = (w.smSelState == s.name);
			const bool live = (!liveState.empty() && liveState == s.name && w.smPath.empty());
			const bool isSub = !s.states.empty();
			ImU32 fill = isSub ? IM_COL32(45, 52, 70, 255) : IM_COL32(48, 52, 60, 255);
			if (live) fill = IM_COL32(45, 80, 52, 255);
			dl->AddRectFilled(a, b, fill, 6);
			dl->AddRect(a, b, sel ? IM_COL32(255, 200, 90, 255)
			                      : live ? IM_COL32(110, 230, 130, 255) : IM_COL32(90, 95, 105, 255), 6,
			            0, sel ? 2.5f : 1.5f);
			if (*sc.entry == s.name)
				dl->AddTriangleFilled(ImVec2(a.x - 14, (a.y + b.y) * 0.5f - 7), ImVec2(a.x - 14, (a.y + b.y) * 0.5f + 7),
				                      ImVec2(a.x - 3, (a.y + b.y) * 0.5f), IM_COL32(110, 230, 130, 255));
			dl->AddText(ImVec2(a.x + 8, a.y + 6), IM_COL32(235, 235, 240, 255), s.name.c_str());
			std::string sub;
			if (isSub) sub = ICON_LC_WORKFLOW " sub-machine";
			else if (!s.motion.empty())
			{
				if (AnimClip* c = ResDB::getSingleton()->GetClip(s.motion)) sub = ICON_LC_PLAY " " + c->name;
				else if (BlendSpace* bsp = ResDB::getSingleton()->GetBlendSpace(s.motion)) sub = ICON_LC_BLEND " " + bsp->name;
				else sub = s.motion;
			}
			else sub = "(no motion)";
			dl->AddText(ImVec2(a.x + 8, a.y + 24 * w.smZoom), IM_COL32(160, 165, 175, 255), sub.c_str());

			ImGui::SetCursorScreenPos(a);
			ImGui::PushID(s.name.c_str());
			ImGui::InvisibleButton("##node", ImVec2(nodeSz.x, nodeSz.y));
			if (ImGui::IsItemActivated())
			{
				if (w.smLink)   // complete a pending link onto this node
				{
					AnimSM::Transition t;
					t.from = (w.smLinkFrom == "*") ? "" : w.smLinkFrom;
					t.to = s.name;
					sc.trans->push_back(t);
					w.smLink = false;
					w.smSelTrans = (int)sc.trans->size() - 1;
					w.smSelState.clear();
					edited = structural = true;
				}
				else { w.smSelState = s.name; w.smSelTrans = -1; }
			}
			if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) && !w.smLink)
			{
				s.nx += ImGui::GetIO().MouseDelta.x / w.smZoom;
				s.ny += ImGui::GetIO().MouseDelta.y / w.smZoom;
				edited = true;
			}
			if (isSub && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				dblOpen = s.name;
			if (ImGui::BeginPopupContextItem("##nodectx"))
			{
				if (ImGui::MenuItem(ICON_LC_ARROW_RIGHT " Add Transition")) { w.smLink = true; w.smLinkFrom = s.name; }
				if (ImGui::MenuItem(ICON_LC_LOG_IN " Set as Entry")) { *sc.entry = s.name; edited = structural = true; }
				if (ImGui::MenuItem(ICON_LC_TRASH_2 " Delete State"))
				{
					const std::string dead = s.name;
					for (int i = (int)sc.trans->size() - 1; i >= 0; --i)
						if ((*sc.trans)[i].from == dead || (*sc.trans)[i].to == dead)
							sc.trans->erase(sc.trans->begin() + i);
					for (int i = 0; i < (int)sc.states->size(); ++i)
						if ((*sc.states)[i].name == dead) { sc.states->erase(sc.states->begin() + i); break; }
					if (*sc.entry == dead) *sc.entry = sc.states->empty() ? "" : sc.states->front().name;
					if (w.smSelState == dead) w.smSelState.clear();
					w.smSelTrans = -1;
					edited = structural = true;
					ImGui::EndPopup();
					ImGui::PopID();
					break;   // container mutated: stop this walk
				}
				ImGui::EndPopup();
			}
			ImGui::PopID();
		}
		if (!dblOpen.empty())
		{
			w.smPath.push_back(dblOpen);
			w.smSelState.clear(); w.smSelTrans = -1;
		}

		// pending link follows the cursor
		if (w.smLink)
		{
			ImVec2 a;
			if (nodeCenter(w.smLinkFrom, a))
				dl->AddLine(a, mouse, IM_COL32(255, 220, 120, 255), 1.5f);
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape))
				w.smLink = false;
		}

		// canvas interactions: empty-click select transition / clear, MMB pan, ctrl+wheel zoom
		ImGui::SetCursorScreenPos(cmin);
		ImGui::SetNextItemAllowOverlap();
		ImGui::InvisibleButton("##canvasbg", csz);
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
		{
			if (transHit >= 0) { w.smSelTrans = transHit; w.smSelState.clear(); }
			else { w.smSelTrans = -1; w.smSelState.clear(); }
		}
		if (ImGui::BeginPopupContextItem("##canvasctx"))
		{
			const ImVec2 cp = toCanvas(ImGui::GetMousePosOnOpeningCurrentPopup());
			if (ImGui::MenuItem(ICON_LC_PLUS " Add State"))
			{
				AnimSM::State s;
				int n = (int)sc.states->size();
				do { s.name = "State" + std::to_string(n++); } while (FindState(sc, s.name));
				s.nx = cp.x; s.ny = cp.y;
				sc.states->push_back(s);
				if (sc.entry->empty()) *sc.entry = s.name;
				w.smSelState = s.name; w.smSelTrans = -1;
				edited = structural = true;
			}
			if (ImGui::MenuItem(ICON_LC_WORKFLOW " Add Sub-Machine"))
			{
				AnimSM::State s;
				int n = (int)sc.states->size();
				do { s.name = "Sub" + std::to_string(n++); } while (FindState(sc, s.name));
				s.nx = cp.x; s.ny = cp.y;
				s.states.push_back(AnimSM::State{ "State0" });
				s.entry = "State0";
				sc.states->push_back(s);
				w.smSelState = s.name; w.smSelTrans = -1;
				edited = structural = true;
			}
			ImGui::EndPopup();
		}
		if (ImGui::IsWindowHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))
		{
			w.smPanX += ImGui::GetIO().MouseDelta.x;
			w.smPanY += ImGui::GetIO().MouseDelta.y;
		}
		if (ImGui::IsWindowHovered() && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0.0f)
			w.smZoom = std::max(0.4f, std::min(2.0f, w.smZoom * (1.0f + ImGui::GetIO().MouseWheel * 0.1f)));
	}
	ImGui::EndChild();

	// preview strip under the canvas
	if (pvH > 0)
	{
		ImGui::BeginChild("##smpv", ImVec2(0, pvH), false,
		                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		if (w.pv) DrawPreviewImage(*w.pv, ImGui::GetContentRegionAvail());
		if (w.pv && !liveState.empty())
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->AddText(ImVec2(w.pv->rectMin.x + 6, w.pv->rectMin.y + 4),
			            IM_COL32(140, 240, 150, 255), (ICON_LC_PLAY " " + liveState).c_str());
		}
		ImGui::EndChild();
	}
	ImGui::EndChild();   // ##smleft

	// ---- right panel -----------------------------------------------------------------------
	ImGui::SameLine();
	ImGui::BeginChild("##smpanel", ImVec2(panelW, 0), true);
	{
		// parameters + live controls
		ImGui::SeparatorText("Parameters");
		for (int i = 0; i < (int)sm->params.size(); ++i)
		{
			AnimSM::Param& p = sm->params[i];
			ImGui::PushID(i);
			char nb[64]; strncpy(nb, p.name.c_str(), sizeof(nb)); nb[sizeof(nb) - 1] = 0;
			ImGui::SetNextItemWidth(100);
			if (ImGui::InputText("##pname", nb, sizeof(nb))) { p.name = nb; edited = structural = true; }
			ImGui::SameLine();
			ImGui::SetNextItemWidth(64);
			if (ImGui::Combo("##ptype", &p.type, "float\0bool\0trigger\0")) edited = structural = true;
			ImGui::SameLine();
			if (an && p.type == 0)
			{
				float v = (float)an->GetFloat(p.name);
				ImGui::SetNextItemWidth(76);
				if (ImGui::DragFloat("##pv", &v, 0.02f)) an->SetFloat(p.name, v);
			}
			else if (an && p.type == 1)
			{
				bool v = an->GetBool(p.name);
				if (ImGui::Checkbox("##pb", &v)) an->SetBool(p.name, v);
			}
			else if (an && p.type == 2)
			{
				if (ImGui::SmallButton("fire")) an->SetTrigger(p.name);
			}
			ImGui::SameLine();
			if (ImGui::SmallButton(ICON_LC_X))
			{
				sm->params.erase(sm->params.begin() + i);
				edited = structural = true;
				ImGui::PopID();
				break;
			}
			ImGui::PopID();
		}
		if (ImGui::SmallButton(ICON_LC_PLUS " Parameter"))
		{
			AnimSM::Param p;
			p.name = "param" + std::to_string(sm->params.size());
			sm->params.push_back(p);
			edited = structural = true;
		}

		// selected layer
		ImGui::SeparatorText("Layer");
		{
			AnimSM::Layer& l = sm->layers[w.smLayer];
			char lb[64]; strncpy(lb, l.name.c_str(), sizeof(lb)); lb[sizeof(lb) - 1] = 0;
			if (ImGui::InputText("Name##layer", lb, sizeof(lb))) { l.name = lb; edited = true; }
			char mb[64]; strncpy(mb, l.mask.c_str(), sizeof(mb)); mb[sizeof(mb) - 1] = 0;
			if (ImGui::InputText("Mask Group", mb, sizeof(mb))) { l.mask = mb; edited = structural = true; }
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Skeleton bone GROUP name; empty = full body");
			if (ImGui::Checkbox("Additive", &l.additive)) edited = structural = true;
			if (ImGui::DragFloat("Weight", &l.weight, 0.02f, 0.0f, 1.0f)) edited = true;
		}

		// selected state
		if (AnimSM::State* s = FindState(sc, w.smSelState))
		{
			ImGui::SeparatorText(("State: " + s->name).c_str());
			char nb[64]; strncpy(nb, s->name.c_str(), sizeof(nb)); nb[sizeof(nb) - 1] = 0;
			if (ImGui::InputText("Name##st", nb, sizeof(nb), ImGuiInputTextFlags_EnterReturnsTrue))
			{
				std::string nn = nb;
				if (!nn.empty() && !FindState(sc, nn))
				{
					RenameStateRefs(sc, s->name, nn);
					s->name = nn;
					w.smSelState = nn;
					edited = structural = true;
				}
			}
			if (s->states.empty())
			{
				std::string motion = s->motion;
				if (AssetPicker("Clip", motion, "anim")) { s->motion = motion; edited = structural = true; }
				std::string bsGuid = s->motion;
				if (AssetPicker("Blend Space", bsGuid, "blendspace")) { s->motion = bsGuid; edited = structural = true; }
			}
			else if (ImGui::Button(ICON_LC_LOG_IN " Open Sub-Machine"))
			{
				w.smPath.push_back(s->name);
				w.smSelState.clear(); w.smSelTrans = -1;
			}
			if (ImGui::Checkbox("Loop", &s->loop)) edited = structural = true;
			if (ImGui::DragFloat("Speed", &s->speed, 0.02f, 0.05f, 5.0f)) edited = structural = true;
			// speed parameter: combo over float params
			{
				const char* cur = s->speedParam.empty() ? "(none)" : s->speedParam.c_str();
				if (ImGui::BeginCombo("Speed Param", cur))
				{
					if (ImGui::Selectable("(none)", s->speedParam.empty())) { s->speedParam.clear(); edited = structural = true; }
					for (const AnimSM::Param& p : sm->params)
						if (p.type == 0 && ImGui::Selectable(p.name.c_str(), s->speedParam == p.name))
						{
							s->speedParam = p.name;
							edited = structural = true;
						}
					ImGui::EndCombo();
				}
			}
			if (ImGui::Checkbox("Mirror", &s->mirror)) edited = structural = true;
			char sb[64]; strncpy(sb, s->sync.c_str(), sizeof(sb)); sb[sizeof(sb) - 1] = 0;
			if (ImGui::InputText("Sync Group", sb, sizeof(sb))) { s->sync = sb; edited = structural = true; }
		}

		// selected transition
		if (w.smSelTrans >= 0 && w.smSelTrans < (int)sc.trans->size())
		{
			AnimSM::Transition& t = (*sc.trans)[w.smSelTrans];
			const std::string from = t.from.empty() ? "Any" : t.from;
			ImGui::SeparatorText((from + " " ICON_LC_ARROW_RIGHT " " + t.to).c_str());
			// priority reorder
			if (ImGui::SmallButton(ICON_LC_ARROW_UP) && w.smSelTrans > 0)
			{
				std::swap((*sc.trans)[w.smSelTrans], (*sc.trans)[w.smSelTrans - 1]);
				--w.smSelTrans;
				edited = structural = true;
			}
			ImGui::SameLine();
			if (ImGui::SmallButton(ICON_LC_ARROW_DOWN) && w.smSelTrans + 1 < (int)sc.trans->size())
			{
				std::swap((*sc.trans)[w.smSelTrans], (*sc.trans)[w.smSelTrans + 1]);
				++w.smSelTrans;
				edited = structural = true;
			}
			ImGui::SameLine();
			ImGui::TextDisabled("priority");
			ImGui::SameLine();
			if (ImGui::SmallButton(ICON_LC_TRASH_2 " Delete"))
			{
				sc.trans->erase(sc.trans->begin() + w.smSelTrans);
				w.smSelTrans = -1;
				edited = structural = true;
			}
			if (w.smSelTrans >= 0)
			{
				AnimSM::Transition& tr = (*sc.trans)[w.smSelTrans];
				if (ImGui::Checkbox("Has Exit Time", &tr.hasExit)) edited = structural = true;
				if (tr.hasExit && ImGui::DragFloat("Exit Time", &tr.exitTime, 0.01f, 0.0f, 4.0f, "%.2f (norm.)"))
					edited = structural = true;
				if (ImGui::DragFloat("Blend", &tr.duration, 0.01f, 0.0f, 3.0f, "%.2fs")) edited = structural = true;
				if (ImGui::Combo("Mode", &tr.mode, "inertialize\0crossfade\0")) edited = structural = true;
				if (ImGui::Combo("Interrupt", &tr.interrupt, "finish first\0can cut\0")) edited = structural = true;
				ImGui::TextDisabled("Conditions");
				for (int i = 0; i < (int)tr.conds.size(); ++i)
				{
					AnimSM::Cond& c = tr.conds[i];
					ImGui::PushID(2000 + i);
					// param combo
					ImGui::SetNextItemWidth(90);
					if (ImGui::BeginCombo("##cparam", c.param.empty() ? "(param)" : c.param.c_str()))
					{
						for (const AnimSM::Param& p : sm->params)
							if (ImGui::Selectable(p.name.c_str(), c.param == p.name))
							{
								c.param = p.name;
								c.op = p.type == 1 ? 4 : p.type == 2 ? 6 : 0;
								edited = structural = true;
							}
						ImGui::EndCombo();
					}
					ImGui::SameLine();
					ImGui::SetNextItemWidth(88);
					if (ImGui::Combo("##cop", &c.op, kOps)) edited = structural = true;
					if (c.op <= 3)
					{
						ImGui::SameLine();
						ImGui::SetNextItemWidth(64);
						if (ImGui::DragFloat("##cval", &c.value, 0.02f)) edited = structural = true;
					}
					ImGui::SameLine();
					if (ImGui::SmallButton(ICON_LC_X))
					{
						tr.conds.erase(tr.conds.begin() + i);
						edited = structural = true;
						ImGui::PopID();
						break;
					}
					ImGui::PopID();
				}
				if (ImGui::SmallButton(ICON_LC_PLUS " Condition"))
				{
					tr.conds.push_back(AnimSM::Cond());
					edited = structural = true;
				}
			}
		}
		if (w.smLink) ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "Click a node to finish the transition (Esc cancels).");
	}
	ImGui::EndChild();

	if (structural && an) an->SetPreviewController(sm);
	if (edited) { w.dirty = true; w.editedNow = true; }
}
