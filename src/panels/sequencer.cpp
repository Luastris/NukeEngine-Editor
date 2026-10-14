// .nuseq timeline editor: track rows + diamond keys on a zoomable ruler, scrub-preview onto
// the LIVE world, transport, RECORD (auto-key of the selected atom's transform), a small
// shared curve strip for the selected track, and Bake Skeletal -> .nuanim.
#include "editor/editorui.h"
#include "editor/animshared.h"
#include "API/Model/Sequence.h"
#include "API/Model/Camera.h"
#include "reflect/ReflectBind.h"
#include "API/Model/resdb.h"
#include <algorithm>
#include <functional>

using namespace nuke;
namespace bfs = boost::filesystem;

// Upsert a key at time t (replace within epsilon, else insert sorted).
void EditorUI::UpsertSharedKey(std::vector<AnimClip::Key>& keys, double t, const float v[4])
{
	AnimClip::Key k;
	k.t = (float)t;
	memcpy(k.v, v, sizeof(k.v));
	for (AnimClip::Key& e : keys)
		if (fabsf(e.t - k.t) < 1e-3f) { memcpy(e.v, v, sizeof(e.v)); return; }
	auto it = keys.begin();
	while (it != keys.end() && it->t <= k.t) ++it;
	keys.insert(it, k);
}

// Full world path of an atom ("/Root/Child") — sequences authored in the editor use
// world-absolute paths so no SequencePlayer parent is required.
static std::string WorldPathOf(Atom* a)
{
	std::string p;
	while (a)
	{
		p = "/" + a->name + p;
		a = a->parent;
	}
	return p;
}

// Stable-id lookup inside a subtree (the prefab editor's tree selection).
static Atom* FindInSubtreeOf(Atom* a, long id)
{
	if (!a) return nullptr;
	if ((long)a->id.id == id) return a;
	for (Atom* ch : a->children) if (Atom* r = FindInSubtreeOf(ch, id)) return r;
	return nullptr;
}

// One diamond key row on the timeline. Returns the interacted key index (-2 none, -1 = row
// context). Drag moves the key in time; right-click deletes.
int EditorUI::SharedKeyRow(const char* id, std::vector<AnimClip::Key>* keys, float x0, float y,
                           float pps, double dur, double* dragT, bool& edited)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	int hit = -2;
	if (!keys) return hit;
	for (size_t i = 0; i < keys->size(); ++i)
	{
		const float kx = x0 + (*keys)[i].t * pps;
		const ImVec2 c(kx, y);
		ImGui::PushID((int)(intptr_t)id + (int)i * 131);
		ImGui::SetCursorScreenPos(ImVec2(c.x - 5, c.y - 5));
		ImGui::InvisibleButton("##k", ImVec2(10, 10));
		const bool hov = ImGui::IsItemHovered();
		if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
		{
			float nt = ((ImGui::GetIO().MousePos.x - x0) / pps);
			nt = std::max(0.0f, std::min((float)dur, nt));
			(*keys)[i].t = nt;
			edited = true;
			if (dragT) *dragT = nt;
			hit = (int)i;
		}
		if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			keys->erase(keys->begin() + i);
			edited = true;
			ImGui::PopID();
			return -2;
		}
		if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) hit = (int)i;
		dl->AddQuadFilled(ImVec2(c.x, c.y - 5), ImVec2(c.x + 5, c.y), ImVec2(c.x, c.y + 5),
		                  ImVec2(c.x - 5, c.y), hov ? IM_COL32(255, 210, 80, 255) : IM_COL32(210, 170, 60, 255));
		ImGui::PopID();
	}
	if (!keys->empty())
	{
		std::sort(keys->begin(), keys->end(),
		          [](const AnimClip::Key& a, const AnimClip::Key& b) { return a.t < b.t; });
	}
	return hit;
}

// Shared curve strip: per-component polylines of a key track + draggable value points.
// Reused by the stage-10 animation editors.
bool EditorUI::DrawKeysCurve(const char* id, std::vector<AnimClip::Key>& keys, int dim, float height)
{
	bool edited = false;
	if (keys.empty() || dim <= 0) { ImGui::TextDisabled("No keys."); return false; }
	ImGui::PushID(id);
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	const ImVec2 p0 = ImGui::GetCursorScreenPos();
	const float w = std::max(80.0f, avail.x), h = height;
	ImGui::InvisibleButton("##curve", ImVec2(w, h));
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(28, 30, 34, 255));
	float vmin = 1e30f, vmax = -1e30f;
	float tmax = 0.0001f;
	for (const AnimClip::Key& k : keys)
	{
		tmax = std::max(tmax, k.t);
		for (int c = 0; c < dim; ++c)
		{
			vmin = std::min(vmin, k.v[c]);
			vmax = std::max(vmax, k.v[c]);
		}
	}
	if (vmax - vmin < 1e-4f) { vmax += 0.5f; vmin -= 0.5f; }
	const ImU32 cols[4] = { IM_COL32(230, 90, 90, 255), IM_COL32(110, 220, 110, 255),
	                        IM_COL32(110, 150, 240, 255), IM_COL32(220, 220, 120, 255) };
	auto px = [&](float t) { return p0.x + t / tmax * (w - 12) + 6; };
	auto py = [&](float v) { return p0.y + (1.0f - (v - vmin) / (vmax - vmin)) * (h - 12) + 6; };
	for (int c = 0; c < dim; ++c)
		for (size_t i = 1; i < keys.size(); ++i)
			dl->AddLine(ImVec2(px(keys[i - 1].t), py(keys[i - 1].v[c])),
			            ImVec2(px(keys[i].t), py(keys[i].v[c])), cols[c], 1.5f);
	for (int c = 0; c < dim; ++c)
		for (size_t i = 0; i < keys.size(); ++i)
		{
			const ImVec2 pt(px(keys[i].t), py(keys[i].v[c]));
			ImGui::PushID(c * 4096 + (int)i);
			ImGui::SetCursorScreenPos(ImVec2(pt.x - 4, pt.y - 4));
			ImGui::InvisibleButton("##pt", ImVec2(8, 8));
			if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
			{
				const float nv = vmin + (1.0f - (ImGui::GetIO().MousePos.y - p0.y - 6) / (h - 12)) * (vmax - vmin);
				keys[i].v[c] = nv;
				edited = true;
			}
			dl->AddCircleFilled(pt, 3.5f, ImGui::IsItemHovered() ? IM_COL32_WHITE : cols[c]);
			ImGui::PopID();
		}
	ImGui::PopID();
	return edited;
}

// ---- host-relative addressing --------------------------------------------------------------------
// A sequence edited FROM a SequencePlayer (the inspector's / prefab editor's "Edit Sequence") is
// bound to that player's atom: every track path is relative to it ("" = the atom, "A/B" its
// descendants), poses are local, so the same .nuseq animates the prefab wherever it is placed.
// A .nuseq opened from the browser keeps the world-absolute ("/Root/Child") mode.
static bool RelPathOf(Atom* host, Atom* a, std::string& out)
{
	out.clear();
	if (!host || !a) return false;
	std::vector<std::string> segs;
	Atom* x = a;
	while (x && x != host) { segs.push_back(x->name); x = x->parent; }
	if (x != host) return false;   // not under the host
	for (auto it = segs.rbegin(); it != segs.rend(); ++it) { if (!out.empty()) out += "/"; out += *it; }
	return true;
}
static Atom* ResolveRel(Atom* host, const std::string& path)
{
	if (!host) return nullptr;
	Atom* a = host;
	size_t pos = 0;
	while (pos <= path.size() && !path.empty())
	{
		const size_t s = path.find('/', pos);
		const std::string seg = path.substr(pos, s == std::string::npos ? std::string::npos : s - pos);
		Atom* next = nullptr;
		for (Atom* ch : a->children) if (ch && ch->name == seg) { next = ch; break; }
		if (!next) return nullptr;
		a = next;
		if (s == std::string::npos) break;
		pos = s + 1;
	}
	return a;
}
// The window's host atom (0 = world-absolute mode): a prefab window's sequencer hosts inside the
// prefab's preview world, a standalone one inside the live world.
static Atom* SeqHostOf(EditorUI::AssetEditorWin& w)
{
	if (!w.seqHostId) return nullptr;
	World* hw = (w.ext == ".nuprefab" && w.pv) ? w.pv->world : AppInstance::GetSingleton()->currentWorld;
	return hw ? hw->GetById(w.seqHostId) : nullptr;
}
// The atom a path addresses in this window's mode.
static Atom* SeqResolve(EditorUI::AssetEditorWin& w, Atom* host, const std::string& path)
{
	if (host) return ResolveRel(host, path);
	if (w.seqPv) { w.seqPv->atom = nullptr; }
	// world-absolute: "/Root/Child"
	World* lw = AppInstance::GetSingleton()->currentWorld;
	if (!lw || path.empty() || path[0] != '/') return nullptr;
	std::string p = path.substr(1);
	Atom* a = nullptr;
	size_t pos = 0;
	while (true)
	{
		const size_t s = p.find('/', pos);
		const std::string seg = p.substr(pos, s == std::string::npos ? std::string::npos : s - pos);
		Atom* next = nullptr;
		if (!a) { for (Atom* r : lw->GetHierarchy()) if (r && r->name == seg) { next = r; break; } }
		else    { for (Atom* ch : a->children) if (ch && ch->name == seg) { next = ch; break; } }
		if (!next) return nullptr;
		a = next;
		if (s == std::string::npos) break;
		pos = s + 1;
	}
	return a;
}

// ---- keying from the CURRENT state ------------------------------------------------------------
static bool KeyTransformTrack(Sequence::TransformTrack& tr, Atom* a, double t)
{
	if (!a) return false;
	Transform& tf = a->GetTransform();
	const float vp[4] = { (float)tf.position.x, (float)tf.position.y, (float)tf.position.z, 0 };
	const float vr[4] = { (float)tf.rotation.x, (float)tf.rotation.y, (float)tf.rotation.z, (float)tf.rotation.w };
	const float vs[4] = { (float)tf.scale.x, (float)tf.scale.y, (float)tf.scale.z, 0 };
	EditorUI::UpsertSharedKey(tr.pos, t, vp);
	EditorUI::UpsertSharedKey(tr.rot, t, vr);
	EditorUI::UpsertSharedKey(tr.scale, t, vs);
	return true;
}
static bool KeyPropTrack(Sequence::PropTrack& tr, Atom* a, double t)
{
	if (!a) return false;
	Component* c = Reflect_FindComponent(a, tr.comp);
	if (!c) return false;
	const Field* f = Reflect_FindField(c->GetType(), tr.prop);
	if (!f) return false;
	ReflectValue v = Reflect_GetField(c, *f);
	float k[4] = { 0, 0, 0, 0 };
	if (v.type == FT::Bool)   k[0] = v.b ? 1.0f : 0.0f;
	else if (tr.dim <= 1)     k[0] = (float)v.num;
	else for (int ci = 0; ci < tr.dim && ci < 4; ++ci) k[ci] = (float)v.v[ci];
	EditorUI::UpsertSharedKey(tr.keys, t, k);
	return true;
}

void EditorUI::DrawSequenceEditor(AssetEditorWin& w)
{
	Sequence* s = w.seq;
	if (!s) { ImGui::TextDisabled("Failed to load sequence."); return; }
	if (!w.seqPv)
	{
		w.seqPv = new SequencePlayer();
		w.seqPv->SetSequence(s);
		// not Init()-ed: the atom is set per frame to the host (relative paths) or stays null ("/" world paths)
	}
	bool edited = false;
	AppInstance* app = AppInstance::GetSingleton();
	Atom* host = SeqHostOf(w);
	w.seqPv->atom = host;
	const bool dock = w.seqDock && w.ext == ".nuprefab";
	const std::string seqFile = dock ? w.seqPath : w.path;
	auto pathLabel = [&](const std::string& p) { return host ? (p.empty() ? std::string("(") + host->name + ")" : p) : p; };

	// The target new tracks address: host mode = a picked descendant of the host (the prefab tree's
	// selection fills it with one click); world mode = the hierarchy selection.
	Atom* hierSel = app->selectedInHieararchy;
	Atom* prefabSel = (w.ext == ".nuprefab" && w.prefabRoot) ? FindInSubtreeOf(w.prefabRoot, w.prefabSelId) : nullptr;
	Atom* tgt = host ? ResolveRel(host, w.seqTarget) : hierSel;
	auto pathOfTarget = [&]() -> std::string { return host ? w.seqTarget : (hierSel ? WorldPathOf(hierSel) : std::string()); };

	// ---- transport --------------------------------------------------------------------------
	if (dock)
	{
		ImGui::TextUnformatted(ICON_LC_CLAPPERBOARD);
		ImGui::SameLine();
		ImGui::TextDisabled("%s%s", bfs::path(seqFile).filename().string().c_str(), w.seqDirty ? " *" : "");
		if (host) { ImGui::SameLine(); ImGui::TextDisabled("  bound to %s", host->name.c_str()); }
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_LC_X " Close")) { w.seqDock = false; return; }
	}
	else if (host)
	{
		ImGui::TextDisabled(ICON_LC_LINK "  bound to %s (paths relative, poses local)", host->name.c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton("Unbind")) { w.seqHostId = 0; w.seqTarget.clear(); }
	}
	if (ImGui::Button(w.seqPlaying ? ICON_LC_PAUSE : ICON_LC_PLAY)) w.seqPlaying = !w.seqPlaying;
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_SQUARE)) { w.seqPlaying = false; w.seqTime = 0; w.seqPv->SetTime(0); }
	ImGui::SameLine();
	ImGui::PushStyleColor(ImGuiCol_Button, w.seqRecord ? ImVec4(0.75f, 0.15f, 0.15f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
	if (ImGui::Button(ICON_LC_CIRCLE_DOT " Rec")) { w.seqRecord = !w.seqRecord; w.seqSnapValid = false; }
	ImGui::PopStyleColor();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Auto-key the target atom's transform while it changes (gizmo, inspector)");
	ImGui::SameLine();
	// One click: every track takes a key from the CURRENT state at the playhead.
	if (ImGui::Button(ICON_LC_DIAMOND " Key All"))
	{
		for (Sequence::TransformTrack& tr : s->transformTracks) if (KeyTransformTrack(tr, SeqResolve(w, host, tr.path), w.seqTime)) edited = true;
		for (Sequence::PropTrack& tr : s->propTracks)           if (KeyPropTrack(tr, SeqResolve(w, host, tr.path), w.seqTime)) edited = true;
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Insert a key at the playhead on EVERY track from the current values");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(90);
	float ft = (float)w.seqTime;
	if (ImGui::DragFloat("##time", &ft, 0.02f, 0.0f, (float)s->duration, "%.2fs"))
	{
		w.seqTime = ft;
		w.seqPv->SetSequence(s);
		w.seqPv->SetTime(w.seqTime);
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(90);
	float fd = (float)s->duration;
	if (ImGui::DragFloat("##dur", &fd, 0.05f, 0.2f, 3600.0f, "len %.2fs")) { s->duration = fd; edited = true; }
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_SAVE " Save")) { s->SaveToFile(seqFile); if (dock) w.seqDirty = false; else w.dirty = false; }
	if (!s->boneTracks.empty())
	{
		ImGui::SameLine();
		if (ImGui::Button(ICON_LC_BONE " Bake Skeletal"))
		{
			boost::system::error_code ec;
			bfs::path rel = bfs::relative(bfs::path(seqFile).parent_path(), bfs::path(app->ResolveContent("")), ec);
			w.seqPv->SetSequence(s);
			w.seqPv->BakeSkeletal((rel / (bfs::path(seqFile).stem().string() + "_baked.nuanim")).generic_string());
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Write the bone tracks as a .nuanim clip on the subtree's skeleton");
	}

	// playback + scrub preview onto the host (prefab sandbox) / the live world
	if (w.seqPlaying)
	{
		w.seqTime += nuke::Time::getSingleton()->delta;
		if (w.seqTime >= s->duration) w.seqTime = 0.0;   // editor preview loops
		w.seqPv->SetSequence(s);
		w.seqPv->SetTime(w.seqTime);
	}

	// ---- RECORD: auto-key the target's transform ---------------------------------------------
	Atom* recAtom = host ? tgt : hierSel;
	if (w.seqRecord && recAtom)
	{
		Transform& t = recAtom->GetTransform();
		float cur[10] = { (float)t.position.x, (float)t.position.y, (float)t.position.z,
		                  (float)t.rotation.x, (float)t.rotation.y, (float)t.rotation.z, (float)t.rotation.w,
		                  (float)t.scale.x, (float)t.scale.y, (float)t.scale.z };
		if (w.seqSnapValid && memcmp(cur, w.seqSnap, sizeof(cur)) != 0)
		{
			const std::string path = pathOfTarget();
			Sequence::TransformTrack* tr = nullptr;
			for (Sequence::TransformTrack& x : s->transformTracks)
				if (x.path == path) { tr = &x; break; }
			if (!tr)
			{
				s->transformTracks.push_back({});
				tr = &s->transformTracks.back();
				tr->path = path;
			}
			KeyTransformTrack(*tr, recAtom, w.seqTime);
			edited = true;
		}
		memcpy(w.seqSnap, cur, sizeof(cur));
		w.seqSnapValid = true;
	}
	else w.seqSnapValid = false;

	// ---- target + add-track row --------------------------------------------------------------
	if (host)
	{
		ImGui::TextUnformatted("Target");
		ImGui::SameLine();
		EditorAtomPathPicker("##seqtgt", host, w.seqTarget, 220);
		if (prefabSel)
		{
			ImGui::SameLine();
			std::string rel;
			const bool under = RelPathOf(host, prefabSel, rel);
			if (!under) ImGui::BeginDisabled();
			if (ImGui::SmallButton(ICON_LC_MOUSE_POINTER_CLICK " selected")) w.seqTarget = rel;
			if (!under) ImGui::EndDisabled();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(under ? "Target = the atom selected in the prefab tree" : "The selected atom is not under the bound atom");
		}
		else if (hierSel)
		{
			ImGui::SameLine();
			std::string rel;
			const bool under = RelPathOf(host, hierSel, rel);
			if (!under) ImGui::BeginDisabled();
			if (ImGui::SmallButton(ICON_LC_MOUSE_POINTER_CLICK " selected")) w.seqTarget = rel;
			if (!under) ImGui::EndDisabled();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(under ? "Target = the atom selected in the hierarchy" : "The selected atom is not under the bound atom");
		}
		ImGui::SameLine();
	}
	const bool haveTgt = tgt != nullptr;
	if (!haveTgt) ImGui::BeginDisabled();
	if (ImGui::Button(ICON_LC_PLUS " Transform"))
	{
		const std::string path = pathOfTarget();
		Sequence::TransformTrack* tr = nullptr;
		for (Sequence::TransformTrack& x : s->transformTracks) if (x.path == path) { tr = &x; break; }
		if (!tr) { s->transformTracks.push_back({}); tr = &s->transformTracks.back(); tr->path = path; }
		KeyTransformTrack(*tr, tgt, w.seqTime);   // starts where the atom IS
		edited = true;
	}
	if (!haveTgt) ImGui::EndDisabled();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Transform track for the target atom, keyed at the playhead from its current pose");
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_PLUS " Event")) { s->events.push_back({ (float)w.seqTime, "event", "" }); edited = true; }
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_PLUS " Cut") && tgt && tgt->GetComponent<Camera>())
	{
		s->cuts.push_back({ (float)w.seqTime, pathOfTarget() });
		edited = true;
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Camera cut to the target atom's Camera at the playhead");
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_PLUS " Clip") && tgt)
	{
		Sequence::ClipTrack ct;
		ct.path = pathOfTarget();
		ct.entries.push_back({ (float)w.seqTime, "", 1.0f });
		s->clipTracks.push_back(ct);
		edited = true;
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fire an Animator clip on the target atom");
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_PLUS " Audio")) { s->stings.push_back({ (float)w.seqTime, "", 1.0f, 1 }); edited = true; }
	ImGui::SameLine();
	// prop track on the target: component/prop through the shared reflection registry
	{
		if (!haveTgt) ImGui::BeginDisabled();
		if (ImGui::Button(ICON_LC_PLUS " Prop")) ImGui::OpenPopup("##seqaddprop");
		if (!haveTgt) ImGui::EndDisabled();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Animate a reflected component prop on the target atom");
		if (ImGui::BeginPopup("##seqaddprop"))
		{
			static std::string apComp, apProp;
			static int apDim = 1;
			EditorPropPicker("##seqpp", 130, 120, apComp, apProp, &apDim);
			if (!apComp.empty() && !apProp.empty() && tgt && ImGui::Button(ICON_LC_PLUS " Add Track"))
			{
				Sequence::PropTrack tr;
				tr.path = pathOfTarget();
				tr.comp = apComp;
				tr.prop = apProp;
				tr.dim = apDim;
				KeyPropTrack(tr, tgt, w.seqTime);   // seed with the LIVE value
				if (tr.keys.empty()) { AnimClip::Key k; k.t = (float)w.seqTime; k.v[0] = k.v[1] = k.v[2] = k.v[3] = 0; tr.keys.push_back(k); }
				s->propTracks.push_back(tr);
				edited = true;
				apComp.clear(); apProp.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(120);
	ImGui::SliderFloat("##zoom", &w.seqZoom, 20.0f, 400.0f, "zoom %.0f");

	// ---- timeline ---------------------------------------------------------------------------
	const float nameW = 230.0f;
	ImGui::BeginChild("##seqtl", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.62f), ImGuiChildFlags_Borders,
	                  ImGuiWindowFlags_HorizontalScrollbar);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float pps = w.seqZoom;
	const ImVec2 org = ImGui::GetCursorScreenPos();
	const float x0 = org.x + nameW;
	const float totalW = nameW + (float)s->duration * pps + 60.0f;

	// ruler (click/drag = scrub)
	{
		ImGui::SetCursorScreenPos(ImVec2(x0, org.y));
		ImGui::InvisibleButton("##ruler", ImVec2(std::max(40.0, s->duration * pps), 18));
		if (ImGui::IsItemActive() || (ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left)))
		{
			w.seqTime = std::max(0.0f, std::min((float)s->duration, (ImGui::GetIO().MousePos.x - x0) / pps));
			w.seqPv->SetSequence(s);
			w.seqPv->SetTime(w.seqTime);
		}
		for (double tt = 0; tt <= s->duration + 1e-6; tt += (pps > 140 ? 0.1 : pps > 45 ? 0.5 : 1.0))
		{
			const float x = x0 + (float)tt * pps;
			dl->AddLine(ImVec2(x, org.y), ImVec2(x, org.y + 16), IM_COL32(140, 140, 140, 255));
			char b[16];
			snprintf(b, sizeof(b), "%.1f", tt);
			dl->AddText(ImVec2(x + 2, org.y), IM_COL32(160, 160, 160, 255), b);
		}
	}

	float y = org.y + 26.0f;
	auto rowLabel = [&](const std::string& text, bool live)
	{
		dl->AddText(ImVec2(org.x, y - 7), live ? IM_COL32(220, 220, 220, 255) : IM_COL32(230, 120, 120, 255), text.c_str());
	};
	auto rowBg = [&]()
	{
		dl->AddLine(ImVec2(org.x, y + 10), ImVec2(org.x + totalW, y + 10), IM_COL32(60, 60, 66, 120));
	};
	// Row menu over the label: key this track now, delete it, or drop every track of its atom.
	// Returns 1 = key, 2 = delete track, 3 = delete all tracks of the path, 0 = nothing.
	auto rowMenu = [&](int id, const std::string& path, bool keyable) -> int
	{
		int act = 0;
		ImGui::SetCursorScreenPos(ImVec2(org.x, y - 9));
		ImGui::PushID(id);
		ImGui::InvisibleButton("##row", ImVec2(nameW - 4, 18));
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nright-click: key / delete", pathLabel(path).c_str());
		if (ImGui::BeginPopupContextItem("##rowctx"))
		{
			if (keyable && ImGui::MenuItem(ICON_LC_DIAMOND " Key at playhead")) act = 1;
			if (ImGui::MenuItem(ICON_LC_X " Delete track")) act = 2;
			if (ImGui::MenuItem((std::string(ICON_LC_LIST_X " Delete all tracks of ") + pathLabel(path)).c_str())) act = 3;
			ImGui::EndPopup();
		}
		ImGui::PopID();
		return act;
	};
	auto dropAllOf = [&](const std::string& path)
	{
		s->transformTracks.erase(std::remove_if(s->transformTracks.begin(), s->transformTracks.end(), [&](const Sequence::TransformTrack& t) { return t.path == path; }), s->transformTracks.end());
		s->propTracks.erase(std::remove_if(s->propTracks.begin(), s->propTracks.end(), [&](const Sequence::PropTrack& t) { return t.path == path; }), s->propTracks.end());
		s->boneTracks.erase(std::remove_if(s->boneTracks.begin(), s->boneTracks.end(), [&](const Sequence::BoneTrack& t) { return t.path == path; }), s->boneTracks.end());
		s->clipTracks.erase(std::remove_if(s->clipTracks.begin(), s->clipTracks.end(), [&](const Sequence::ClipTrack& t) { return t.path == path; }), s->clipTracks.end());
		edited = true;
	};
	double dragT = -1;
	int rowId = 1;
	bool structural = false;   // a track list changed: stop walking this frame
	// transform tracks: one row per component set
	for (size_t i = 0; i < s->transformTracks.size() && !structural; ++i)
	{
		Sequence::TransformTrack& tr = s->transformTracks[i];
		Atom* ta = SeqResolve(w, host, tr.path);
		rowLabel(ICON_LC_MOVE_3D "  " + pathLabel(tr.path), ta != nullptr);
		std::vector<AnimClip::Key>* sets[3] = { &tr.pos, &tr.rot, &tr.scale };
		for (int sIdx = 0; sIdx < 3; ++sIdx)
		{
			const int hit = SharedKeyRow((const char*)(intptr_t)(rowId++ * 977), sets[sIdx], x0, y, pps, s->duration, &dragT, edited);
			if (hit >= 0) { w.seqSelKind = 0; w.seqSelTrack = (int)i * 3 + sIdx; w.seqSelKey = hit; }
		}
		const int act = rowMenu(rowId * 7919, tr.path, ta != nullptr);
		if (act == 1) { if (KeyTransformTrack(tr, ta, w.seqTime)) edited = true; }
		else if (act == 2) { s->transformTracks.erase(s->transformTracks.begin() + i); edited = true; structural = true; }
		else if (act == 3) { dropAllOf(tr.path); structural = true; }
		rowBg();
		y += 22.0f;
	}
	// prop tracks
	for (size_t i = 0; i < s->propTracks.size() && !structural; ++i)
	{
		Sequence::PropTrack& tr = s->propTracks[i];
		Atom* ta = SeqResolve(w, host, tr.path);
		rowLabel(ICON_LC_SLIDERS_HORIZONTAL "  " + pathLabel(tr.path) + " " + tr.comp + "." + tr.prop, ta != nullptr);
		const int hit = SharedKeyRow((const char*)(intptr_t)(rowId++ * 977), &tr.keys, x0, y, pps, s->duration, &dragT, edited);
		if (hit >= 0) { w.seqSelKind = 1; w.seqSelTrack = (int)i; w.seqSelKey = hit; }
		const int act = rowMenu(rowId * 7919, tr.path, ta != nullptr);
		if (act == 1) { if (KeyPropTrack(tr, ta, w.seqTime)) edited = true; }
		else if (act == 2) { s->propTracks.erase(s->propTracks.begin() + i); edited = true; structural = true; }
		else if (act == 3) { dropAllOf(tr.path); structural = true; }
		rowBg();
		y += 22.0f;
	}
	// bone tracks
	for (size_t i = 0; i < s->boneTracks.size() && !structural; ++i)
	{
		Sequence::BoneTrack& tr = s->boneTracks[i];
		rowLabel(ICON_LC_BONE "  " + pathLabel(tr.path) + " : " + tr.bone, SeqResolve(w, host, tr.path) != nullptr);
		SharedKeyRow((const char*)(intptr_t)(rowId++ * 977), &tr.pos, x0, y, pps, s->duration, &dragT, edited);
		SharedKeyRow((const char*)(intptr_t)(rowId++ * 977), &tr.rot, x0, y, pps, s->duration, &dragT, edited);
		const int act = rowMenu(rowId * 7919, tr.path, false);
		if (act == 2) { s->boneTracks.erase(s->boneTracks.begin() + i); edited = true; structural = true; }
		else if (act == 3) { dropAllOf(tr.path); structural = true; }
		rowBg();
		y += 22.0f;
	}
	// clip tracks / events / cuts / stings: single markers with tiny inline editors
	for (size_t i = 0; i < s->clipTracks.size() && !structural; ++i)
	{
		Sequence::ClipTrack& tr = s->clipTracks[i];
		rowLabel(ICON_LC_PLAY "  " + pathLabel(tr.path), SeqResolve(w, host, tr.path) != nullptr);
		for (size_t k = 0; k < tr.entries.size(); ++k)
		{
			const float kx = x0 + tr.entries[k].t * pps;
			dl->AddTriangleFilled(ImVec2(kx, y - 6), ImVec2(kx + 10, y), ImVec2(kx, y + 6), IM_COL32(120, 220, 140, 255));
			ImGui::SetCursorScreenPos(ImVec2(kx - 4, y - 8));
			ImGui::PushID(rowId * 977 + (int)k);
			ImGui::InvisibleButton("##ck", ImVec2(14, 16));
			if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
			{
				tr.entries[k].t = std::max(0.0f, std::min((float)s->duration, (ImGui::GetIO().MousePos.x - x0) / pps));
				edited = true;
			}
			if (ImGui::BeginPopupContextItem("##cctx"))
			{
				char buf[128];
				snprintf(buf, sizeof(buf), "%s", tr.entries[k].clip.c_str());
				ImGui::SetNextItemWidth(220);
				if (ImGui::InputText("Clip (guid/name)", buf, sizeof(buf))) { tr.entries[k].clip = buf; edited = true; }
				if (ImGui::MenuItem("Delete")) { tr.entries.erase(tr.entries.begin() + k); edited = true; }
				ImGui::EndPopup();
			}
			ImGui::PopID();
		}
		const int act = rowMenu(rowId * 7919, tr.path, false);
		if (act == 2) { s->clipTracks.erase(s->clipTracks.begin() + i); edited = true; structural = true; }
		else if (act == 3) { dropAllOf(tr.path); structural = true; }
		++rowId;
		rowBg();
		y += 22.0f;
	}
	auto markerRow = [&](const char* icon, auto& vec, auto editUi)
	{
		rowLabel(icon, true);
		for (size_t k = 0; k < vec.size(); ++k)
		{
			const float kx = x0 + vec[k].t * pps;
			dl->AddCircleFilled(ImVec2(kx, y), 5.0f, IM_COL32(230, 160, 90, 255));
			ImGui::SetCursorScreenPos(ImVec2(kx - 5, y - 8));
			ImGui::PushID(rowId * 977 + (int)k);
			ImGui::InvisibleButton("##mk", ImVec2(12, 16));
			if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
			{
				vec[k].t = std::max(0.0f, std::min((float)s->duration, (ImGui::GetIO().MousePos.x - x0) / pps));
				edited = true;
			}
			if (ImGui::BeginPopupContextItem("##mctx"))
			{
				if (editUi(vec[k])) edited = true;
				if (ImGui::MenuItem("Delete")) { vec.erase(vec.begin() + k); edited = true; }
				ImGui::EndPopup();
			}
			ImGui::PopID();
		}
		++rowId;
		rowBg();
		y += 22.0f;
	};
	if (!structural)
	{
		if (!s->events.empty())
			markerRow(ICON_LC_ZAP "  Events", s->events, [](Sequence::Event& e)
			{
				bool ch = false;
				char n[96], p[192];
				snprintf(n, sizeof(n), "%s", e.name.c_str());
				snprintf(p, sizeof(p), "%s", e.payload.c_str());
				ImGui::SetNextItemWidth(180);
				if (ImGui::InputText("Name", n, sizeof(n))) { e.name = n; ch = true; }
				ImGui::SetNextItemWidth(180);
				if (ImGui::InputText("Payload", p, sizeof(p))) { e.payload = p; ch = true; }
				return ch;
			});
		if (!s->cuts.empty())
			markerRow(ICON_LC_VIDEO "  Cuts", s->cuts, [](Sequence::Cut& c)
			{
				bool ch = false;
				char n[192];
				snprintf(n, sizeof(n), "%s", c.camera.c_str());
				ImGui::SetNextItemWidth(220);
				if (ImGui::InputText("Camera path", n, sizeof(n))) { c.camera = n; ch = true; }
				return ch;
			});
		if (!s->stings.empty())
			markerRow(ICON_LC_MUSIC "  Audio", s->stings, [](Sequence::Sting& st)
			{
				bool ch = false;
				char n[192];
				snprintf(n, sizeof(n), "%s", st.clip.c_str());
				ImGui::SetNextItemWidth(220);
				if (ImGui::InputText("Sound (guid/path)", n, sizeof(n))) { st.clip = n; ch = true; }
				if (ImGui::SliderFloat("Volume", &st.volume, 0.0f, 2.0f)) ch = true;
				return ch;
			});
	}

	// playhead
	{
		const float x = x0 + (float)w.seqTime * pps;
		dl->AddLine(ImVec2(x, org.y), ImVec2(x, y), IM_COL32(255, 80, 80, 255), 2.0f);
	}
	ImGui::SetCursorScreenPos(ImVec2(org.x, y + 4));
	ImGui::Dummy(ImVec2(totalW, 1));
	ImGui::EndChild();

	// ---- shared curve strip for the selected track ------------------------------------------
	ImGui::TextDisabled("Curves (click a key to select its track; drag points to edit values)");
	std::vector<AnimClip::Key>* curveKeys = nullptr;
	int curveDim = 0;
	if (w.seqSelKind == 0)
	{
		const int ti = w.seqSelTrack / 3, si = w.seqSelTrack % 3;
		if (ti >= 0 && ti < (int)s->transformTracks.size())
		{
			Sequence::TransformTrack& tr = s->transformTracks[ti];
			curveKeys = si == 0 ? &tr.pos : si == 1 ? &tr.rot : &tr.scale;
			curveDim = si == 1 ? 4 : 3;
		}
	}
	else if (w.seqSelKind == 1 && w.seqSelTrack >= 0 && w.seqSelTrack < (int)s->propTracks.size())
	{
		curveKeys = &s->propTracks[w.seqSelTrack].keys;
		curveDim = std::max(1, s->propTracks[w.seqSelTrack].dim);
	}
	if (curveKeys)
	{
		if (DrawKeysCurve("##seqcurve", *curveKeys, curveDim, std::max(70.0f, ImGui::GetContentRegionAvail().y - 6)))
			edited = true;
	}
	else ImGui::TextDisabled("No track selected.");

	if (edited)
	{
		if (dock) { w.seqDirty = true; w.seqEditedNow = true; }   // the prefab window's own history stays the prefab's
		else      { w.dirty = true; w.editedNow = true; }
		w.seqPv->SetSequence(s);
		w.seqPv->SetTime(w.seqTime);   // live refresh of the changed values
	}
}

// Open the sequencer bound to a SequencePlayer's atom: docked into a prefab window (dockIn) or
// as the standalone window on the live world. Paths author relative to the atom from then on.
void EditorUI::OpenSequenceForPlayer(Atom* host, const std::string& seqGuid, AssetEditorWin* dockIn)
{
	if (!host || seqGuid.empty()) return;
	const std::string path = ResDB::getSingleton()->PathForGuid(seqGuid);
	if (path.empty()) return;
	if (dockIn)
	{
		if (dockIn->seq && dockIn->seqPath != path)
		{
			delete dockIn->seqPv; dockIn->seqPv = nullptr;
			delete dockIn->seq;   dockIn->seq = nullptr;
		}
		if (!dockIn->seq)
		{
			dockIn->seq = Sequence::LoadFromFile(path);
			if (!dockIn->seq) return;
			dockIn->seqPath = path;
			dockIn->idleQ = dockIn->seq->ToString();
			dockIn->undoQ.clear(); dockIn->redoQ.clear();
			dockIn->seqDirty = false; dockIn->seqTime = 0.0; dockIn->seqPlaying = false;
		}
		dockIn->seqHostId = (long)host->id.id;
		dockIn->seqTarget.clear();
		dockIn->seqDock = true;
		return;
	}
	OpenAssetEditor(path);
	boost::system::error_code ec;
	const bfs::path want = bfs::weakly_canonical(bfs::path(path), ec);
	for (AssetEditorWin& w : assetEds)
		if (w.ext == ".nuseq" && w.seq)
		{
			boost::system::error_code ec2;
			if (bfs::weakly_canonical(bfs::path(w.path), ec2) == want) { w.seqHostId = (long)host->id.id; w.seqTarget.clear(); w.wantFocus = true; }
		}
}
