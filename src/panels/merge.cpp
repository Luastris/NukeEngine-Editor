// Disk<->editor conflict resolution for the open world, as a structural tree with per-node side choice.
// Atoms/children are matched by id, components by type+id; Save applies the chosen sides and reloads.
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
	bool  group = false;        // has children (atom / child / component / object / array-of-...)
	bool  leaf  = false;        // a single value param
	bool  elem  = false;        // array element add/remove (a child atom or a component on one side)
	bool  atomLeaf = false;     // top-level atom present on only one side
	std::string path;           // leaf: json pointer to the value (relative to the atom)
	std::string container;      // elem: json pointer to the array (relative to the atom)
	std::string matchKey;       // elem: "id" (child atom) or "cid" (component)
	long  matchVal = 0;         // elem: the id to match for removal
	bool  edP = false, dkP = false;
	json  ed, dk;               // leaf: the two values; elem/atomLeaf: the element json (one side)
	int   choice = 0;           // 1 = editor, 2 = disk (0 = unresolved)
	bool  atom = false;         // top-level atom node
	long  atomId = 0;
	std::vector<MergeNode> kids;
};

struct MergeState { json edWorld, dkWorld; std::vector<MergeNode> atoms; };

std::string esc(const std::string& k)
{
	std::string o; for (char c : k) { if (c == '~') o += "~0"; else if (c == '/') o += "~1"; else o += c; } return o;
}

std::string human(const json& v, bool present)
{
	if (!present || v.is_null()) return "-";
	if (v.is_string())          return v.get<std::string>();
	if (v.is_boolean())         return v.get<bool>() ? "true" : "false";
	if (v.is_number_integer())  return std::to_string(v.get<long long>());
	if (v.is_number())          { char b[32]; snprintf(b, sizeof(b), "%g", v.get<double>()); return b; }
	if (v.is_array())           { std::string s; for (const auto& e : v) { if (!s.empty()) s += ", "; s += human(e, true); } return s; }
	return "...";
}

std::vector<MergeNode> DiffInto(const json& ed, const json& dk, const std::string& base);

bool DiffValue(const std::string& label, const std::string& path,
               bool edP, const json& ed, bool dkP, const json& dk, MergeNode& out)
{
	if (edP && dkP && ed == dk) return false;
	if (edP && dkP && ed.is_object() && dk.is_object())
	{
		MergeNode g; g.label = label; g.group = true;
		std::set<std::string> keys;
		for (auto it = ed.begin(); it != ed.end(); ++it) keys.insert(it.key());
		for (auto it = dk.begin(); it != dk.end(); ++it) keys.insert(it.key());
		for (const std::string& k : keys)
		{
			bool e = ed.contains(k), d = dk.contains(k);
			MergeNode c;
			if (DiffValue(k, path + "/" + esc(k), e, e ? ed[k] : json(), d, d ? dk[k] : json(), c)) g.kids.push_back(std::move(c));
		}
		if (g.kids.empty()) return false;
		out = std::move(g); return true;
	}
	out.label = label; out.leaf = true; out.path = path; out.edP = edP; out.dkP = dkP; out.ed = ed; out.dk = dk;
	return true;
}

// child atoms (array) matched by id
std::vector<MergeNode> DiffChildren(const json& ea, const json& da, const std::string& base)
{
	std::vector<MergeNode> out;
	std::map<long, std::pair<int, const json*>> em, dm;
	for (int i = 0; i < (int)ea.size(); ++i) em[ea[i].value("id", 0L)] = { i, &ea[i] };
	for (int i = 0; i < (int)da.size(); ++i) dm[da[i].value("id", 0L)] = { i, &da[i] };
	std::vector<long> ids;
	for (const json& a : ea) ids.push_back(a.value("id", 0L));
	for (const json& a : da) { long id = a.value("id", 0L); if (!em.count(id)) ids.push_back(id); }
	for (long id : ids)
	{
		bool e = em.count(id), d = dm.count(id);
		if (e && d)
		{
			if (*em[id].second == *dm[id].second) continue;
			MergeNode n; n.group = true; n.label = em[id].second->value("name", std::string("Atom"));
			n.kids = DiffInto(*em[id].second, *dm[id].second, base + "/" + std::to_string(em[id].first));
			if (!n.kids.empty()) out.push_back(std::move(n));
		}
		else
		{
			const json& a = e ? *em[id].second : *dm[id].second;
			MergeNode n; n.elem = true; n.container = base; n.matchKey = "id"; n.matchVal = id;
			n.label = a.value("name", std::string("Atom")) + (e ? "  (editor only)" : "  (disk only)");
			n.edP = e; n.dkP = d; if (e) n.ed = a; else n.dk = a;
			out.push_back(std::move(n));
		}
	}
	return out;
}

// components (array) matched by type + id
std::vector<MergeNode> DiffComps(const json& ea, const json& da, const std::string& base)
{
	std::vector<MergeNode> out;
	auto key = [](const json& c){ return c.value("type", std::string()) + "#" + std::to_string(c.value("cid", 0L)); };
	auto lab = [](const json& c){ return c.value("type", std::string()) + " #" + std::to_string(c.value("cid", 0L) % 100000); };
	std::map<std::string, std::pair<int, const json*>> em, dm;
	for (int i = 0; i < (int)ea.size(); ++i) em[key(ea[i])] = { i, &ea[i] };
	for (int i = 0; i < (int)da.size(); ++i) dm[key(da[i])] = { i, &da[i] };
	std::set<std::string> ks; for (auto& kv : em) ks.insert(kv.first); for (auto& kv : dm) ks.insert(kv.first);
	for (const std::string& kk : ks)
	{
		bool e = em.count(kk), d = dm.count(kk);
		if (e && d)
		{
			const json& ec = *em[kk].second; const json& dc = *dm[kk].second;
			if (ec == dc) continue;
			MergeNode n; n.group = true; n.label = lab(ec);
			n.kids = DiffInto(ec, dc, base + "/" + std::to_string(em[kk].first));
			if (!n.kids.empty()) out.push_back(std::move(n));
		}
		else
		{
			const json& c = e ? *em[kk].second : *dm[kk].second;
			MergeNode n; n.elem = true; n.container = base; n.matchKey = "cid"; n.matchVal = c.value("cid", 0L);
			n.label = lab(c) + (e ? "  (editor only)" : "  (disk only)");
			n.edP = e; n.dkP = d; if (e) n.ed = c; else n.dk = c;
			out.push_back(std::move(n));
		}
	}
	return out;
}

// diff two objects' keys; children/components recurse into sub-trees, everything else generic
std::vector<MergeNode> DiffInto(const json& ed, const json& dk, const std::string& base)
{
	std::vector<MergeNode> out;
	std::set<std::string> keys;
	for (auto it = ed.begin(); it != ed.end(); ++it) keys.insert(it.key());
	for (auto it = dk.begin(); it != dk.end(); ++it) keys.insert(it.key());
	for (const std::string& k : keys)
	{
		if (k == "id" || k == "cid") continue;
		bool e = ed.contains(k), d = dk.contains(k);
		if (k == "children" && e && d && ed[k].is_array() && dk[k].is_array())
		{
			auto kids = DiffChildren(ed[k], dk[k], base + "/children");
			if (!kids.empty()) { MergeNode g; g.group = true; g.label = "children"; g.kids = std::move(kids); out.push_back(std::move(g)); }
		}
		else if (k == "components" && e && d && ed[k].is_array() && dk[k].is_array())
		{
			auto kids = DiffComps(ed[k], dk[k], base + "/components");
			if (!kids.empty()) { MergeNode g; g.group = true; g.label = "components"; g.kids = std::move(kids); out.push_back(std::move(g)); }
		}
		else
		{
			MergeNode c;
			if (DiffValue(k, base + "/" + esc(k), e, e ? ed[k] : json(), d, d ? dk[k] : json(), c)) out.push_back(std::move(c));
		}
	}
	return out;
}

std::shared_ptr<MergeState> Build(const std::string& editorJson, const std::string& diskJson)
{
	auto st = std::make_shared<MergeState>();
	st->edWorld = json::parse(editorJson, nullptr, false);
	st->dkWorld = json::parse(diskJson, nullptr, false);
	if (st->edWorld.is_discarded() || st->dkWorld.is_discarded()) return st;
	const json ea = st->edWorld.contains("atoms") ? st->edWorld["atoms"] : json::array();
	const json da = st->dkWorld.contains("atoms") ? st->dkWorld["atoms"] : json::array();
	std::map<long, const json*> em, dm;
	for (const json& a : ea) em[a.value("id", 0L)] = &a;
	for (const json& a : da) dm[a.value("id", 0L)] = &a;
	std::vector<long> ids;
	for (const json& a : ea) ids.push_back(a.value("id", 0L));
	for (const json& a : da) { long id = a.value("id", 0L); if (!em.count(id)) ids.push_back(id); }
	for (long id : ids)
	{
		bool e = em.count(id), d = dm.count(id);
		if (e && d)
		{
			if (*em[id] == *dm[id]) continue;
			MergeNode n; n.atom = true; n.group = true; n.atomId = id;
			n.label = em[id]->value("name", std::string("Atom"));
			n.ed = *em[id]; n.kids = DiffInto(*em[id], *dm[id], "");
			if (!n.kids.empty()) st->atoms.push_back(std::move(n));
		}
		else
		{
			MergeNode n; n.atom = true; n.atomLeaf = true; n.atomId = id;
			n.edP = e; n.dkP = d;
			if (e) { n.ed = *em[id]; n.label = em[id]->value("name", std::string("Atom")); }
			else   { n.dk = *dm[id]; n.label = dm[id]->value("name", std::string("Atom")); }
			st->atoms.push_back(std::move(n));
		}
	}
	return st;
}

// --- subtree choice helpers ---
void SetSide(MergeNode& n, int side)
{
	if (n.leaf || n.elem || n.atomLeaf) n.choice = side;
	for (MergeNode& k : n.kids) SetSide(k, side);
}
bool AllSide(const MergeNode& n, int side)   // every choosable descendant is `side`
{
	if (n.leaf || n.elem || n.atomLeaf) return n.choice == side;
	if (n.kids.empty()) return false;
	for (const MergeNode& k : n.kids) if (!AllSide(k, side)) return false;
	return true;
}
int Unresolved(const MergeNode& n)
{
	if (n.leaf || n.elem || n.atomLeaf) return n.choice == 0 ? 1 : 0;
	int c = 0; for (const MergeNode& k : n.kids) c += Unresolved(k); return c;
}
void Walk(MergeNode& n, const std::function<void(MergeNode&)>& f)
{
	if (n.leaf || n.elem) f(n);
	for (MergeNode& k : n.kids) Walk(k, f);
}

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

// Build a merged atom: editor base, applying disk-chosen field values, then element add/remove.
json ApplyAtomNode(const json& base, MergeNode& node)
{
	json merged = base;
	struct ElemOp { std::string container, key; long val; bool add; json add_json; };
	std::vector<ElemOp> ops;
	for (MergeNode& top : node.kids) Walk(top, [&](MergeNode& l) {
		if (l.choice != 2) return;
		if (l.leaf)
		{
			json::json_pointer pp(l.path);
			if (l.dkP) merged[pp] = l.dk; else EraseAtPointer(merged, l.path);
		}
		else if (l.elem)
		{
			if (l.edP && !l.dkP) ops.push_back({ l.container, l.matchKey, l.matchVal, false, {} });   // remove
			else if (l.dkP && !l.edP) ops.push_back({ l.container, l.matchKey, l.matchVal, true, l.dk });// add
		}
	});
	// element ops AFTER field edits (which used editor indices); match by key so indices don't matter
	for (ElemOp& op : ops)
	{
		json::json_pointer cp(op.container);
		if (!merged.contains(cp) || !merged.at(cp).is_array()) continue;
		json& arr = merged.at(cp);
		if (op.add) arr.push_back(op.add_json);
		else for (auto it = arr.begin(); it != arr.end(); ++it) if (it->value(op.key, 0L) == op.val) { arr.erase(it); break; }
	}
	return merged;
}

// --- tree rendering ---
void DrawNode(MergeNode& n)
{
	ImGui::TableNextRow();
	ImGui::PushID((void*)&n);
	ImGui::TableSetColumnIndex(0);
	bool open = false;
	if (n.group && !n.kids.empty())
		open = ImGui::TreeNodeEx(n.label.c_str(), ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_DefaultOpen);
	else
		ImGui::TreeNodeEx(n.label.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanFullWidth);

	for (int side = 1; side <= 2; ++side)
	{
		ImGui::TableSetColumnIndex(side);
		bool checked = (n.group && !n.kids.empty()) ? AllSide(n, side) : (n.choice == side);
		bool before  = checked;
		ImGui::Checkbox(side == 1 ? "##e" : "##d", &checked);
		if (checked != before)
		{
			if (n.group && !n.kids.empty()) SetSide(n, checked ? side : 0);
			else n.choice = checked ? side : 0;
		}
		if (n.leaf)        { ImGui::SameLine(); ImGui::TextUnformatted(human(side == 1 ? n.ed : n.dk, side == 1 ? n.edP : n.dkP).c_str()); }
		else if (n.elem)   { ImGui::SameLine(); ImGui::TextUnformatted(side == 1 ? (n.edP ? "keep" : "-") : (n.dkP ? "add" : "remove")); }
		else if (n.atomLeaf){ ImGui::SameLine(); ImGui::TextUnformatted(side == 1 ? (n.edP ? "keep" : "-") : (n.dkP ? "add" : "remove")); }
	}

	if (open) { for (MergeNode& k : n.kids) DrawNode(k); ImGui::TreePop(); }
	ImGui::PopID();
}

} // namespace

void EditorUI::OpenMerge(const std::string& editorJson, const std::string& diskJson)
{
	auto st = Build(editorJson, diskJson);
	mergeState = st;
	mergeOpen  = !st->atoms.empty();
}

void EditorUI::DrawMergeWindow()
{
	if (!mergeOpen || !mergeState) return;
	MergeState* ms = static_cast<MergeState*>(mergeState.get());
	ImGui::SetNextWindowSize(ImVec2(760, 480), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Resolve conflict (editor / disk)", &mergeOpen))
	{
		ImGui::TextUnformatted("Tick the side to keep on each node; ticking a parent applies to its whole subtree.");
		if (ImGui::Button("Use all: Editor")) for (MergeNode& a : ms->atoms) SetSide(a, 1);
		ImGui::SameLine();
		if (ImGui::Button("Use all: Disk"))   for (MergeNode& a : ms->atoms) SetSide(a, 2);
		ImGui::Separator();

		ImGuiTableFlags fl = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
		if (ImGui::BeginTable("##merge", 3, fl, ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.4f)))
		{
			ImGui::TableSetupColumn("Object / parameter", ImGuiTableColumnFlags_WidthStretch, 0.5f);
			ImGui::TableSetupColumn("Editor", ImGuiTableColumnFlags_WidthStretch, 0.25f);
			ImGui::TableSetupColumn("Disk",   ImGuiTableColumnFlags_WidthStretch, 0.25f);
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableHeadersRow();
			for (MergeNode& a : ms->atoms) DrawNode(a);
			ImGui::EndTable();
		}

		int un = 0; for (MergeNode& a : ms->atoms) un += Unresolved(a);
		ImGui::Separator();
		ImGui::BeginDisabled(un > 0);
		if (ImGui::Button("Save"))
		{
			json out = ms->edWorld; out["atoms"] = json::array();
			std::map<long, MergeNode*> byId; for (MergeNode& a : ms->atoms) byId[a.atomId] = &a;
			for (const json& a : ms->edWorld["atoms"])
			{
				long id = a.value("id", 0L);
				auto it = byId.find(id);
				if (it == byId.end()) { out["atoms"].push_back(a); continue; }     // unchanged
				MergeNode* n = it->second;
				if (n->atomLeaf) { if (n->choice != 2) out["atoms"].push_back(a); }// editor-only: keep unless "remove"
				else out["atoms"].push_back(ApplyAtomNode(a, *n));
			}
			for (MergeNode& n : ms->atoms)
				if (n.atomLeaf && n.dkP && !n.edP && n.choice == 2) out["atoms"].push_back(n.dk);   // disk-only: add

			AppInstance* app = AppInstance::GetSingleton();
			std::string merged = out.dump();
			if (!app->currentWorldPath.empty())
			{
				bfs::ofstream f{bfs::path(app->WorldFullPath(app->currentWorldPath))};
				if (f) f << out.dump(2);
			}
			app->selectedInHieararchy = nullptr;
			app->currentWorld->LoadFromString(merged);
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
