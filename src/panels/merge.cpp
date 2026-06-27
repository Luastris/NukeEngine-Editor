// merge panel — disk<->editor conflict resolution for the open world.
//
// Diff model: worlds are { type, version, atoms:[...] }. Atoms are matched by `id` (stable). A
// per-atom subtree of field diffs is built by recursively comparing the two atom JSON objects;
// nested objects recurse, everything else (scalars, arrays like components/position) is a leaf.
// The user picks a side per node (mutually exclusive checkboxes); "Use all" sets one side wholesale.
// Saving applies the chosen sides onto the editor atom, writes the merged world, and reloads it.
#include <editor/editorui.h>
#include <nlohmann/json.hpp>
#include <boost/filesystem/fstream.hpp>
#include <map>
#include <set>

using json = nlohmann::json;

namespace {

struct MergeNode
{
	std::string label;
	std::string path;          // json pointer relative to the atom (leaves)
	bool  isLeaf = false;
	bool  edP = false, dkP = false;
	json  ed, dk;              // leaf values; for atom nodes, the whole atom on each side
	int   choice = 0;          // 1 = editor, 2 = disk (0 = unresolved)
	long  atomId = 0;
	bool  atom = false;        // atom-level node
	bool  atomBoth = false;    // atom present on both sides (modified) -> field kids
	bool  compLeaf = false;    // whole-component add/remove leaf (matched by type+id)
	std::string compType;      // component type
	long  compId = 0;          // component id (removal matches by id, unique per atom)
	std::vector<MergeNode> kids;
};

struct MergeState
{
	json edWorld, dkWorld;
	std::vector<MergeNode> atoms;
};

std::string esc(const std::string& k)   // json-pointer-escape a key
{
	std::string o;
	for (char c : k) { if (c == '~') o += "~0"; else if (c == '/') o += "~1"; else o += c; }
	return o;
}

// Build a diff node for a value pair; returns false if identical (no node produced).
bool DiffValue(const std::string& label, const std::string& path,
               bool edP, const json& ed, bool dkP, const json& dk, MergeNode& out)
{
	if (edP && dkP && ed == dk) return false;
	if (edP && dkP && ed.is_object() && dk.is_object())
	{
		MergeNode grp; grp.label = label; grp.path = path; grp.isLeaf = false;
		std::set<std::string> keys;
		for (auto it = ed.begin(); it != ed.end(); ++it) keys.insert(it.key());
		for (auto it = dk.begin(); it != dk.end(); ++it) keys.insert(it.key());
		for (const std::string& k : keys)
		{
			bool e = ed.contains(k), d = dk.contains(k);
			MergeNode child;
			if (DiffValue(k, path + "/" + esc(k), e, e ? ed[k] : json(), d, d ? dk[k] : json(), child))
				grp.kids.push_back(std::move(child));
		}
		if (grp.kids.empty()) return false;
		out = std::move(grp);
		return true;
	}
	out.label = label; out.path = path; out.isLeaf = true;
	out.edP = edP; out.dkP = dkP; out.ed = ed; out.dk = dk;
	return true;
}

// Diff the components array by component "type" (not index), so per-component params become leaves.
// Field-leaf paths use the EDITOR component index (apply works off the editor atom).
bool DiffComponents(const json& ea, const json& da, MergeNode& out)
{
	// Key by type + component id, so several components of one type (e.g. scripts) are distinct.
	auto key   = [](const json& c){ return c.value("type", std::string()) + "#" + std::to_string(c.value("cid", 0L)); };
	auto label = [](const json& c){ long id = c.value("cid", 0L); return c.value("type", std::string()) + " #" + std::to_string(id % 100000); };
	std::map<std::string, std::pair<int, const json*>> em, dm;
	for (int i = 0; i < (int)ea.size(); ++i) em[key(ea[i])] = { i, &ea[i] };
	for (int i = 0; i < (int)da.size(); ++i) dm[key(da[i])] = { i, &da[i] };
	MergeNode grp; grp.label = "components"; grp.isLeaf = false;
	std::set<std::string> ks;
	for (auto& kv : em) ks.insert(kv.first);
	for (auto& kv : dm) ks.insert(kv.first);
	for (const std::string& kk : ks)
	{
		bool e = em.count(kk), d = dm.count(kk);
		if (e && d)
		{
			const json& ec = *em[kk].second; const json& dc = *dm[kk].second;
			if (ec == dc) continue;
			int ei = em[kk].first;
			MergeNode cn; cn.label = label(ec); cn.isLeaf = false;
			std::set<std::string> keys;
			for (auto it = ec.begin(); it != ec.end(); ++it) keys.insert(it.key());
			for (auto it = dc.begin(); it != dc.end(); ++it) keys.insert(it.key());
			for (const std::string& k : keys)
			{
				if (k == "type" || k == "cid") continue;
				bool ke = ec.contains(k), kd = dc.contains(k);
				MergeNode child;
				if (DiffValue(k, "/components/" + std::to_string(ei) + "/" + esc(k), ke, ke ? ec[k] : json(), kd, kd ? dc[k] : json(), child))
					cn.kids.push_back(std::move(child));
			}
			if (!cn.kids.empty()) grp.kids.push_back(std::move(cn));
		}
		else                                   // component on only one side
		{
			const json& c = e ? *em[kk].second : *dm[kk].second;
			MergeNode cn; cn.isLeaf = true; cn.compLeaf = true;
			cn.compType = c.value("type", std::string());
			cn.compId   = c.value("cid", 0L);
			cn.label    = label(c) + (e ? "  (editor only)" : "  (disk only)");
			cn.edP = e; cn.dkP = d;
			if (e) cn.ed = c; else cn.dk = c;
			grp.kids.push_back(std::move(cn));
		}
	}
	if (grp.kids.empty()) return false;
	out = std::move(grp);
	return true;
}

std::shared_ptr<MergeState> Build(const std::string& editorJson, const std::string& diskJson)
{
	auto st = std::make_shared<MergeState>();
	st->edWorld = json::parse(editorJson, nullptr, false);
	st->dkWorld = json::parse(diskJson, nullptr, false);
	if (st->edWorld.is_discarded() || st->dkWorld.is_discarded()) return st;
	const json& ea = st->edWorld.contains("atoms") ? st->edWorld["atoms"] : json::array();
	const json& da = st->dkWorld.contains("atoms") ? st->dkWorld["atoms"] : json::array();
	std::map<long, const json*> em, dm;
	for (const json& a : ea) em[a.value("id", 0L)] = &a;
	for (const json& a : da) dm[a.value("id", 0L)] = &a;
	std::vector<long> ids;                          // editor order first, then disk-only
	for (const json& a : ea) ids.push_back(a.value("id", 0L));
	for (const json& a : da) { long id = a.value("id", 0L); if (!em.count(id)) ids.push_back(id); }
	for (long id : ids)
	{
		bool e = em.count(id), d = dm.count(id);
		if (e && d)
		{
			if (*em[id] == *dm[id]) continue;       // identical atom — nothing to resolve
			MergeNode n; n.atom = true; n.atomBoth = true; n.atomId = id;
			n.label = em[id]->value("name", std::string("Atom"));
			n.ed = *em[id]; n.dk = *dm[id]; n.edP = n.dkP = true;
			const json& eo = *em[id]; const json& dobj = *dm[id];
			std::set<std::string> keys;
			for (auto it = eo.begin(); it != eo.end(); ++it) keys.insert(it.key());
			for (auto it = dobj.begin(); it != dobj.end(); ++it) keys.insert(it.key());
			for (const std::string& k : keys)
			{
				if (k == "id") continue;
				bool ke = eo.contains(k), kd = dobj.contains(k);
				MergeNode child;
				if (k == "components" && ke && kd && eo[k].is_array() && dobj[k].is_array())
				{
					if (DiffComponents(eo[k], dobj[k], child)) n.kids.push_back(std::move(child));   // per-component
				}
				else if (DiffValue(k, "/" + esc(k), ke, ke ? eo[k] : json(), kd, kd ? dobj[k] : json(), child))
					n.kids.push_back(std::move(child));
			}
			st->atoms.push_back(std::move(n));
		}
		else                                        // present on only one side
		{
			MergeNode n; n.atom = true; n.isLeaf = true; n.atomId = id;
			n.edP = e; n.dkP = d;
			if (e) { n.ed = *em[id]; n.label = em[id]->value("name", std::string("Atom")); }
			else   { n.dk = *dm[id]; n.label = dm[id]->value("name", std::string("Atom")); }
			st->atoms.push_back(std::move(n));
		}
	}
	return st;
}

// --- choice helpers (walk leaves) ---
void ForEachLeaf(MergeNode& n, const std::function<void(MergeNode&)>& f)
{
	if (n.isLeaf) { f(n); return; }
	for (MergeNode& k : n.kids) ForEachLeaf(k, f);
}
void SetAll(MergeState* ms, int side) { for (MergeNode& a : ms->atoms) ForEachLeaf(a, [&](MergeNode& l){ l.choice = side; }); }
int  Unresolved(MergeState* ms) { int n = 0; for (MergeNode& a : ms->atoms) ForEachLeaf(a, [&](MergeNode& l){ if (l.choice == 0) ++n; }); return n; }

void EraseAtPointer(json& root, const std::string& ptr)
{
	auto pos = ptr.find_last_of('/');
	if (pos == std::string::npos) return;
	std::string parent = ptr.substr(0, pos), key = ptr.substr(pos + 1);
	for (size_t i = 0; (i = key.find("~1", i)) != std::string::npos;) key.replace(i, 2, "/");
	for (size_t i = 0; (i = key.find("~0", i)) != std::string::npos;) key.replace(i, 2, "~");
	json::json_pointer pp(parent);
	if (root.contains(pp) && root.at(pp).is_object()) root.at(pp).erase(key);
}

// Apply a modified atom's field choices onto the editor atom (disk-chosen leaves overwrite/erase).
json ApplyAtom(MergeNode& n)
{
	json merged = n.ed;
	std::vector<long> removeIds;            // editor-only components the user chose to drop (by id)
	std::vector<json> addComps;             // disk-only components the user chose to add
	ForEachLeaf(n, [&](MergeNode& l) {
		if (l.compLeaf)                             // whole-component add/remove (deferred, by id)
		{
			if (l.choice == 2)
			{
				if (l.edP && !l.dkP) removeIds.push_back(l.compId);
				else if (l.dkP && !l.edP) addComps.push_back(l.dk);
			}
			return;
		}
		if (l.choice != 2) return;                  // keep editor
		json::json_pointer pp(l.path);
		if (l.dkP) merged[pp] = l.dk;               // take disk value (field path uses the editor index)
		else       EraseAtPointer(merged, l.path);  // disk doesn't have it -> remove the field
	});
	// Field edits done; now array-level component changes (by type, so indices can't desync the edits).
	if (merged.contains("components") && merged["components"].is_array())
	{
		json& arr = merged["components"];
		for (long rid : removeIds)
			for (auto it = arr.begin(); it != arr.end(); ++it)
				if (it->value("cid", 0L) == rid) { arr.erase(it); break; }
		for (json& c : addComps) arr.push_back(c);
	}
	return merged;
}

const char* shortVal(const json& v, bool present)
{
	static std::string s;
	if (!present) { s = "(none)"; return s.c_str(); }
	s = v.dump();
	if (s.size() > 60) s = s.substr(0, 57) + "...";
	return s.c_str();
}

void DrawLeaf(MergeNode& l)
{
	ImGui::TableNextRow();
	ImGui::PushID((void*)&l);                       // unique per leaf (compLeaf has an empty path)
	ImGui::TableSetColumnIndex(0);
	ImGui::TreeNodeEx(l.label.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanFullWidth);
	ImGui::TableSetColumnIndex(1);                  // Editor
	{
		bool e = (l.choice == 1);
		if (ImGui::Checkbox("##e", &e)) l.choice = e ? 1 : 0;
		ImGui::SameLine(); ImGui::TextUnformatted(shortVal(l.ed, l.edP));
	}
	ImGui::TableSetColumnIndex(2);                  // Disk
	{
		bool d = (l.choice == 2);
		if (ImGui::Checkbox("##d", &d)) l.choice = d ? 2 : 0;
		ImGui::SameLine(); ImGui::TextUnformatted(shortVal(l.dk, l.dkP));
	}
	ImGui::PopID();
}

void DrawGroup(MergeNode& g)
{
	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	bool open = ImGui::TreeNodeEx(g.label.c_str(), ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_DefaultOpen);
	if (open)
	{
		for (MergeNode& k : g.kids) { if (k.isLeaf) DrawLeaf(k); else DrawGroup(k); }
		ImGui::TreePop();
	}
}

void DrawAtom(MergeNode& a)
{
	if (a.isLeaf)   // atom present on only one side -> keep (editor) / take (disk)
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		std::string lbl = a.label + (a.edP ? "  (editor only)" : "  (disk only)");
		ImGui::TreeNodeEx(lbl.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanFullWidth);
		ImGui::TableSetColumnIndex(1);
		{ bool e = (a.choice == 1); if (ImGui::Checkbox(("##ae" + std::to_string(a.atomId)).c_str(), &e)) a.choice = e ? 1 : 0; ImGui::SameLine(); ImGui::TextUnformatted(a.edP ? "keep" : "remove"); }
		ImGui::TableSetColumnIndex(2);
		{ bool d = (a.choice == 2); if (ImGui::Checkbox(("##ad" + std::to_string(a.atomId)).c_str(), &d)) a.choice = d ? 2 : 0; ImGui::SameLine(); ImGui::TextUnformatted(a.dkP ? "add" : "remove"); }
		return;
	}
	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	bool open = ImGui::TreeNodeEx(a.label.c_str(), ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_DefaultOpen);
	if (open)
	{
		for (MergeNode& k : a.kids) { if (k.isLeaf) DrawLeaf(k); else DrawGroup(k); }
		ImGui::TreePop();
	}
}

} // namespace

void EditorUI::OpenMerge(const std::string& editorJson, const std::string& diskJson)
{
	auto st = Build(editorJson, diskJson);
	mergeState = st;                 // shared_ptr<MergeState> -> shared_ptr<void> keeps the deleter
	mergeOpen  = !st->atoms.empty();
}

void EditorUI::DrawMergeWindow()
{
	if (!mergeOpen || !mergeState) return;
	MergeState* ms = static_cast<MergeState*>(mergeState.get());
	ImGui::SetNextWindowSize(ImVec2(720, 460), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Resolve conflict (editor \xE2\x86\x94 disk)", &mergeOpen))
	{
		ImGui::TextUnformatted("Pick which side each changed object/param keeps, then Save.");
		if (ImGui::Button("Use all: Editor")) SetAll(ms, 1);
		ImGui::SameLine();
		if (ImGui::Button("Use all: Disk"))   SetAll(ms, 2);
		ImGui::Separator();

		ImGuiTableFlags fl = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
		if (ImGui::BeginTable("##merge", 3, fl, ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.4f)))
		{
			ImGui::TableSetupColumn("Object / parameter", ImGuiTableColumnFlags_WidthStretch, 0.40f);
			ImGui::TableSetupColumn("Editor", ImGuiTableColumnFlags_WidthStretch, 0.30f);
			ImGui::TableSetupColumn("Disk",   ImGuiTableColumnFlags_WidthStretch, 0.30f);
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableHeadersRow();
			for (MergeNode& a : ms->atoms) DrawAtom(a);
			ImGui::EndTable();
		}

		int un = Unresolved(ms);
		ImGui::Separator();
		ImGui::BeginDisabled(un > 0);
		if (ImGui::Button("Save"))
		{
			// Build the merged world: editor base, applying per-atom field choices + add/remove.
			json out = ms->edWorld;
			out["atoms"] = json::array();
			std::map<long, MergeNode*> nodeById;
			for (MergeNode& a : ms->atoms) nodeById[a.atomId] = &a;
			for (const json& a : ms->edWorld["atoms"])
			{
				long id = a.value("id", 0L);
				auto it = nodeById.find(id);
				if (it == nodeById.end()) { out["atoms"].push_back(a); continue; }   // unchanged
				MergeNode* n = it->second;
				if (n->atomBoth) out["atoms"].push_back(ApplyAtom(*n));
				else if (n->choice != 2) out["atoms"].push_back(a);                  // editor-only: keep unless "remove"
			}
			for (MergeNode& n : ms->atoms)
				if (n.atom && n.isLeaf && n.dkP && !n.edP && n.choice == 2) out["atoms"].push_back(n.dk);   // disk-only: add

			AppInstance* app = AppInstance::GetSingleton();
			std::string merged = out.dump();
			if (!app->currentWorldPath.empty())
			{
				bfs::ofstream f{bfs::path(app->WorldFullPath(app->currentWorldPath))};
				if (f) f << out.dump(2);
			}
			app->selectedInHieararchy = nullptr;
			app->currentScene->LoadFromString(merged);
			ResetUndo();
			SyncWorldBaseline();
			mergeOpen = false;
		}
		ImGui::EndDisabled();
		if (un > 0) { ImGui::SameLine(); ImGui::Text("%d unresolved", un); }
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) mergeOpen = false;
	}
	ImGui::End();
}
