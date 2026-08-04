// .nuseq timeline editor: track rows + diamond keys on a zoomable ruler, scrub-preview onto
// the LIVE world, transport, RECORD (auto-key of the selected atom's transform), a small
// shared curve strip for the selected track, and Bake Skeletal -> .nuanim.
#include "editor/editorui.h"
#include "editor/animshared.h"
#include "API/Model/Sequence.h"
#include "API/Model/Camera.h"
#include "reflect/ReflectBind.h"
#include <algorithm>

using namespace nuke;

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

void EditorUI::DrawSequenceEditor(AssetEditorWin& w)
{
	Sequence* s = w.seq;
	if (!s) { ImGui::TextDisabled("Failed to load sequence."); return; }
	if (!w.seqPv)
	{
		w.seqPv = new SequencePlayer();
		w.seqPv->SetSequence(s);
		// not Init()-ed: atom stays null, "/" world paths resolve from the live world
	}
	bool edited = false;
	AppInstance* app = AppInstance::GetSingleton();

	// ---- transport --------------------------------------------------------------------------
	if (ImGui::Button(w.seqPlaying ? ICON_LC_PAUSE : ICON_LC_PLAY)) w.seqPlaying = !w.seqPlaying;
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_SQUARE)) { w.seqPlaying = false; w.seqTime = 0; w.seqPv->SetTime(0); }
	ImGui::SameLine();
	ImGui::PushStyleColor(ImGuiCol_Button, w.seqRecord ? ImVec4(0.75f, 0.15f, 0.15f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
	if (ImGui::Button(ICON_LC_CIRCLE_DOT " Rec")) { w.seqRecord = !w.seqRecord; w.seqSnapValid = false; }
	ImGui::PopStyleColor();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Auto-key the SELECTED atom's transform while it changes");
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
	if (ImGui::Button(ICON_LC_SAVE " Save")) { s->SaveToFile(w.path); w.dirty = false; }
	if (!s->boneTracks.empty())
	{
		ImGui::SameLine();
		if (ImGui::Button(ICON_LC_BONE " Bake Skeletal"))
		{
			boost::system::error_code ec;
			bfs::path rel = bfs::relative(bfs::path(w.path).parent_path(), bfs::path(app->ResolveContent("")), ec);
			w.seqPv->SetSequence(s);
			w.seqPv->BakeSkeletal((rel / (bfs::path(w.path).stem().string() + "_baked.nuanim")).generic_string());
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Write the bone tracks as a .nuanim clip on the subtree's skeleton");
	}

	// playback + scrub preview onto the live world
	if (w.seqPlaying)
	{
		w.seqTime += nuke::Time::getSingleton()->delta;
		if (w.seqTime >= s->duration) w.seqTime = 0.0;   // editor preview loops
		w.seqPv->SetSequence(s);
		w.seqPv->SetTime(w.seqTime);
	}

	// ---- RECORD: auto-key the selected atom's transform -------------------------------------
	Atom* sel = app->selectedInHieararchy;
	if (w.seqRecord && sel)
	{
		Transform& t = sel->GetTransform();
		float cur[10] = { (float)t.position.x, (float)t.position.y, (float)t.position.z,
		                  (float)t.rotation.x, (float)t.rotation.y, (float)t.rotation.z, (float)t.rotation.w,
		                  (float)t.scale.x, (float)t.scale.y, (float)t.scale.z };
		if (w.seqSnapValid && memcmp(cur, w.seqSnap, sizeof(cur)) != 0)
		{
			const std::string path = WorldPathOf(sel);
			Sequence::TransformTrack* tr = nullptr;
			for (Sequence::TransformTrack& x : s->transformTracks)
				if (x.path == path) { tr = &x; break; }
			if (!tr)
			{
				s->transformTracks.push_back({});
				tr = &s->transformTracks.back();
				tr->path = path;
			}
			const float vp[4] = { cur[0], cur[1], cur[2], 0 };
			const float vr[4] = { cur[3], cur[4], cur[5], cur[6] };
			const float vs[4] = { cur[7], cur[8], cur[9], 0 };
			UpsertSharedKey(tr->pos, w.seqTime, vp);
			UpsertSharedKey(tr->rot, w.seqTime, vr);
			UpsertSharedKey(tr->scale, w.seqTime, vs);
			edited = true;
		}
		memcpy(w.seqSnap, cur, sizeof(cur));
		w.seqSnapValid = true;
	}
	else w.seqSnapValid = false;

	// ---- add-track row ----------------------------------------------------------------------
	if (ImGui::Button(ICON_LC_PLUS " Transform") && sel)
	{
		s->transformTracks.push_back({});
		s->transformTracks.back().path = WorldPathOf(sel);
		edited = true;
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Transform track for the selected atom");
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_PLUS " Event")) { s->events.push_back({ (float)w.seqTime, "event", "" }); edited = true; }
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_PLUS " Cut") && sel && sel->GetComponent<Camera>())
	{
		s->cuts.push_back({ (float)w.seqTime, WorldPathOf(sel) });
		edited = true;
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Camera cut to the selected atom's Camera at the playhead");
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_PLUS " Clip") && sel)
	{
		Sequence::ClipTrack ct;
		ct.path = WorldPathOf(sel);
		ct.entries.push_back({ (float)w.seqTime, "", 1.0f });
		s->clipTracks.push_back(ct);
		edited = true;
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fire an Animator clip on the selected atom");
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_PLUS " Audio")) { s->stings.push_back({ (float)w.seqTime, "", 1.0f, 1 }); edited = true; }
	ImGui::SameLine();
	// prop track on the selected atom: component/prop through the shared reflection registry
	{
		if (!sel) ImGui::BeginDisabled();
		if (ImGui::Button(ICON_LC_PLUS " Prop")) ImGui::OpenPopup("##seqaddprop");
		if (!sel) ImGui::EndDisabled();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Animate a reflected component prop on the selected atom");
		if (ImGui::BeginPopup("##seqaddprop"))
		{
			static std::string apComp, apProp;
			static int apDim = 1;
			EditorPropPicker("##seqpp", 130, 120, apComp, apProp, &apDim);
			if (!apComp.empty() && !apProp.empty() && sel && ImGui::Button(ICON_LC_PLUS " Add Track"))
			{
				Sequence::PropTrack tr;
				tr.path = WorldPathOf(sel);
				tr.comp = apComp;
				tr.prop = apProp;
				tr.dim = apDim;
				// seed a key with the LIVE value so the track starts where the scene is
				AnimClip::Key k;
				k.t = (float)w.seqTime;
				k.v[0] = k.v[1] = k.v[2] = k.v[3] = 0;
				if (Component* c = Reflect_FindComponent(sel, apComp))
					if (const Field* f = Reflect_FindField(c->GetType(), apProp))
					{
						ReflectValue v = Reflect_GetField(c, *f);
						if (v.type == FT::Bool)      k.v[0] = v.b ? 1.0f : 0.0f;
						else if (apDim == 1)         k.v[0] = (float)v.num;
						else for (int ci = 0; ci < apDim; ++ci) k.v[ci] = (float)v.v[ci];
					}
				tr.keys.push_back(k);
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
	const float nameW = 210.0f;
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
	auto rowLabel = [&](const std::string& text)
	{
		dl->AddText(ImVec2(org.x, y - 7), IM_COL32(220, 220, 220, 255), text.c_str());
	};
	auto rowBg = [&]()
	{
		dl->AddLine(ImVec2(org.x, y + 10), ImVec2(org.x + totalW, y + 10), IM_COL32(60, 60, 66, 120));
	};
	double dragT = -1;
	int rowId = 1;
	// transform tracks: one row per component set
	for (size_t i = 0; i < s->transformTracks.size(); ++i)
	{
		Sequence::TransformTrack& tr = s->transformTracks[i];
		rowLabel(ICON_LC_MOVE_3D "  " + tr.path);
		std::vector<AnimClip::Key>* sets[3] = { &tr.pos, &tr.rot, &tr.scale };
		for (int sIdx = 0; sIdx < 3; ++sIdx)
		{
			const int hit = SharedKeyRow((const char*)(intptr_t)(rowId++ * 977), sets[sIdx], x0, y, pps, s->duration, &dragT, edited);
			if (hit >= 0) { w.seqSelKind = 0; w.seqSelTrack = (int)i * 3 + sIdx; w.seqSelKey = hit; }
		}
		if (ImGui::IsMouseHoveringRect(ImVec2(org.x, y - 9), ImVec2(org.x + nameW, y + 9))
		    && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			s->transformTracks.erase(s->transformTracks.begin() + i);
			edited = true;
			break;
		}
		rowBg();
		y += 22.0f;
	}
	// prop tracks
	for (size_t i = 0; i < s->propTracks.size(); ++i)
	{
		Sequence::PropTrack& tr = s->propTracks[i];
		rowLabel(ICON_LC_SLIDERS_HORIZONTAL "  " + tr.path + " " + tr.comp + "." + tr.prop);
		const int hit = SharedKeyRow((const char*)(intptr_t)(rowId++ * 977), &tr.keys, x0, y, pps, s->duration, &dragT, edited);
		if (hit >= 0) { w.seqSelKind = 1; w.seqSelTrack = (int)i; w.seqSelKey = hit; }
		rowBg();
		y += 22.0f;
	}
	// bone tracks
	for (size_t i = 0; i < s->boneTracks.size(); ++i)
	{
		Sequence::BoneTrack& tr = s->boneTracks[i];
		rowLabel(ICON_LC_BONE "  " + tr.path + " : " + tr.bone);
		SharedKeyRow((const char*)(intptr_t)(rowId++ * 977), &tr.pos, x0, y, pps, s->duration, &dragT, edited);
		SharedKeyRow((const char*)(intptr_t)(rowId++ * 977), &tr.rot, x0, y, pps, s->duration, &dragT, edited);
		rowBg();
		y += 22.0f;
	}
	// clip tracks / events / cuts / stings: single markers with tiny inline editors
	for (size_t i = 0; i < s->clipTracks.size(); ++i)
	{
		Sequence::ClipTrack& tr = s->clipTracks[i];
		rowLabel(ICON_LC_PLAY "  " + tr.path);
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
		++rowId;
		rowBg();
		y += 22.0f;
	}
	auto markerRow = [&](const char* icon, auto& vec, auto editUi)
	{
		rowLabel(icon);
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
		w.dirty = true;
		w.editedNow = true;
		w.seqPv->SetSequence(s);
		w.seqPv->SetTime(w.seqTime);   // live refresh of the changed values
	}
}
