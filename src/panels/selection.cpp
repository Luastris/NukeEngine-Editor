// Multi-selection ops (Q1/Q2/Q4): hierarchy folders, grouping, and every context op applied
// to the WHOLE selection as ONE undo step. The engine holds the selection (primary +
// selectedExtra ids); the shift anchor and row order live on EditorUI (hierarchy.cpp fills them).
#include <editor/editorui.h>
#include <algorithm>

using nlohmann::json;

// One atom's restorable placement: enough for ApplyAtomState in either direction.
struct AtomPlace { long id = 0; long parent = 0; int index = 0; std::string json; };

static AtomPlace PlaceOf(EditorUI* ui, Atom* a)
{
	World* w = AppInstance::GetSingleton()->currentWorld;
	AtomPlace p;
	p.id = a->id.id;
	p.parent = a->parent ? a->parent->id.id : 0;
	int i = 0;
	auto& lst = a->parent ? a->parent->children : w->GetHierarchy();
	for (Atom* s : lst) { if (s == a) { p.index = i; break; } ++i; }
	p.json = SaveAtomToString(a);
	(void)ui;
	return p;
}

void EditorUI::HierSelect(Atom* a)
{
	AppInstance* app = AppInstance::GetSingleton();
	app->selectedInHieararchy = a;
	app->selectedExtra.clear();
	hierAnchorId = a ? (long)a->id.id : 0;
}

void EditorUI::HierToggle(Atom* a)
{
	if (!a) return;
	AppInstance* app = AppInstance::GetSingleton();
	if (!app->selectedInHieararchy) { HierSelect(a); return; }
	auto& ex = app->selectedExtra;
	if (a == app->selectedInHieararchy)
	{
		// Deselect the primary: the first extra takes over (or nothing stays selected).
		if (ex.empty()) { HierSelect(nullptr); return; }
		app->selectedInHieararchy = app->currentWorld->GetById((long)ex.front());
		ex.erase(ex.begin());
		if (!app->selectedInHieararchy) HierSelect(nullptr);
		return;
	}
	auto it = std::find(ex.begin(), ex.end(), a->id.id);
	if (it != ex.end()) { ex.erase(it); return; }
	// New member: the old primary joins the extras, the clicked atom becomes primary+anchor.
	ex.insert(ex.begin(), app->selectedInHieararchy->id.id);
	app->selectedInHieararchy = a;
	hierAnchorId = (long)a->id.id;
}

void EditorUI::HierRange(Atom* a, bool additive)
{
	if (!a) return;
	AppInstance* app = AppInstance::GetSingleton();
	int ia = -1, ib = -1;
	for (int i = 0; i < (int)hierRowsPrev.size(); ++i)
	{
		if (hierRowsPrev[i] && (long)hierRowsPrev[i]->id.id == hierAnchorId) ia = i;
		if (hierRowsPrev[i] == a) ib = i;
	}
	if (ia < 0 || ib < 0) { HierSelect(a); return; }
	if (!additive) app->selectedExtra.clear();
	for (int i = std::min(ia, ib); i <= std::max(ia, ib); ++i)
	{
		Atom* r = hierRowsPrev[i];
		if (!r || r == a) continue;
		if (std::find(app->selectedExtra.begin(), app->selectedExtra.end(), r->id.id) == app->selectedExtra.end())
			app->selectedExtra.push_back(r->id.id);
	}
	// The clicked end becomes primary; the anchor STAYS for follow-up ranges (browser contract).
	if (app->selectedInHieararchy && app->selectedInHieararchy != a
	    && std::find(app->selectedExtra.begin(), app->selectedExtra.end(),
	                 app->selectedInHieararchy->id.id) == app->selectedExtra.end())
		app->selectedExtra.push_back(app->selectedInHieararchy->id.id);
	app->selectedInHieararchy = a;
	// Primary must not double as an extra.
	app->selectedExtra.erase(std::remove(app->selectedExtra.begin(), app->selectedExtra.end(),
	                                     (unsigned long)a->id.id), app->selectedExtra.end());
}

std::vector<Atom*> EditorUI::SelectionTopLevel()
{
	AppInstance* app = AppInstance::GetSingleton();
	std::vector<Atom*> sel = app->Selection();
	std::vector<Atom*> out;
	for (Atom* a : sel)
	{
		if (!a || a->GetName() == "Editor Camera") continue;
		bool covered = false;
		for (Atom* p = a->parent; p && !covered; p = p->parent)
			for (Atom* s : sel) if (s == p) { covered = true; break; }
		if (!covered) out.push_back(a);
	}
	return out;
}

void EditorUI::DeleteSelection()
{
	std::vector<Atom*> top = SelectionTopLevel();
	if (top.empty()) return;
	if (top.size() == 1) { HierSelect(top[0]); DeleteSelectedAtom(); return; }   // existing single path
	std::vector<AtomPlace> places;
	for (Atom* a : top) places.push_back(PlaceOf(this, a));
	// Ascending indices: undo re-inserts siblings back at their captured slots correctly.
	std::sort(places.begin(), places.end(), [](const AtomPlace& x, const AtomPlace& y)
	          { return x.parent != y.parent ? x.parent < y.parent : x.index < y.index; });
	PushUndo("Delete " + std::to_string(places.size()) + " atoms",
		[this, places]{ for (const AtomPlace& p : places) ApplyAtomState(p.id, p.parent, p.index, p.json); },
		[this, places]{ for (const AtomPlace& p : places) ApplyAtomState(p.id, p.parent, p.index, std::string()); });
	for (const AtomPlace& p : places) pendingDeleteIds.push_back(p.id);
	HierSelect(nullptr);
}

void EditorUI::DuplicateSelection()
{
	std::vector<Atom*> top = SelectionTopLevel();
	if (top.empty()) return;
	if (top.size() == 1) { HierSelect(top[0]); DuplicateSelectedAtom(); return; }
	AppInstance* app = AppInstance::GetSingleton();
	World* w = app->currentWorld;
	std::vector<AtomPlace> added;
	std::vector<Atom*> copies;
	for (Atom* src : top)
	{
		Atom* a = CloneAtomFromString(SaveAtomToString(src));
		if (!a) continue;
		long parentId = src->parent ? src->parent->id.id : 0;
		int  index = 0;
		{ auto& lst = src->parent ? src->parent->children : w->GetHierarchy();
		  int i = 0; for (Atom* s : lst) { if (s == src) { index = i + 1; break; } ++i; } }
		w->InsertAtom(a, parentId, index);
		added.push_back(PlaceOf(this, a));
		copies.push_back(a);
	}
	if (added.empty()) return;
	PushUndo("Duplicate " + std::to_string(added.size()) + " atoms",
		[this, added]{ for (const AtomPlace& p : added) ApplyAtomState(p.id, p.parent, p.index, std::string()); },
		[this, added]{ for (const AtomPlace& p : added) ApplyAtomState(p.id, p.parent, p.index, p.json); });
	// The copies become the selection.
	HierSelect(copies[0]);
	for (size_t i = 1; i < copies.size(); ++i) app->selectedExtra.push_back(copies[i]->id.id);
}

void EditorUI::CopySelection()
{
	std::vector<Atom*> top = SelectionTopLevel();
	if (top.empty()) return;
	if (top.size() == 1)
	{
		ImGui::SetClipboardText((std::string("{\"nukeClipboard\":\"atom\",\"atom\":")
		                         + SaveAtomToString(top[0]) + "}").c_str());
		return;
	}
	json arr = json::array();
	for (Atom* a : top) arr.push_back(json::parse(SaveAtomToString(a), nullptr, false));
	json env; env["nukeClipboard"] = "atoms"; env["atoms"] = arr;
	ImGui::SetClipboardText(env.dump().c_str());
}

void EditorUI::CutSelection()
{
	CopySelection();
	DeleteSelection();
}

void EditorUI::PasteAtoms()
{
	const char* clip = ImGui::GetClipboardText();
	if (!clip || !strstr(clip, "\"nukeClipboard\"")) return;
	json j = json::parse(clip, nullptr, false);
	if (j.is_discarded()) return;
	if (j.value("nukeClipboard", std::string()) == "atom") { PasteAtom(); return; }   // single envelope
	if (j.value("nukeClipboard", std::string()) != "atoms" || !j.contains("atoms")) return;
	AppInstance* app = AppInstance::GetSingleton();
	World* w = app->currentWorld;
	std::vector<AtomPlace> added;
	std::vector<Atom*> pasted;
	for (const json& aj : j["atoms"])
	{
		Atom* a = CloneAtomFromString(aj.dump());
		if (!a) continue;
		w->InsertAtom(a, 0, -1);   // root level, at the end
		added.push_back(PlaceOf(this, a));
		pasted.push_back(a);
	}
	if (added.empty()) return;
	PushUndo("Paste " + std::to_string(added.size()) + " atoms",
		[this, added]{ for (const AtomPlace& p : added) ApplyAtomState(p.id, p.parent, p.index, std::string()); },
		[this, added]{ for (const AtomPlace& p : added) ApplyAtomState(p.id, p.parent, p.index, p.json); });
	HierSelect(pasted[0]);
	for (size_t i = 1; i < pasted.size(); ++i) app->selectedExtra.push_back(pasted[i]->id.id);
}

void EditorUI::SetSelectionEnabled(bool on)
{
	std::vector<Atom*> sel = AppInstance::GetSingleton()->Selection();
	std::vector<std::pair<long, bool>> was;
	for (Atom* a : sel) { if (a) { was.push_back({ (long)a->id.id, a->enabled }); a->enabled = on; } }
	if (was.empty()) return;
	editing = false; editAtomId = 0;   // own command: suppress the auto edit-detector
	PushUndo(on ? "Enable atoms" : "Disable atoms",
		[was]{ World* w = AppInstance::GetSingleton()->currentWorld;
		       for (auto& p : was) if (Atom* a = w->GetById(p.first)) a->enabled = p.second; },
		[was, on]{ World* w = AppInstance::GetSingleton()->currentWorld;
		           for (auto& p : was) if (Atom* a = w->GetById(p.first)) a->enabled = on; });
}

Atom* EditorUI::CreateFolderAtom(Atom* parent)
{
	AppInstance* app = AppInstance::GetSingleton();
	World* w = app->currentWorld;
	Atom* f = new Atom("Folder");
	f->folder = true;
	w->InsertAtom(f, parent ? (long)parent->id.id : 0, -1);
	RecordAdd(f);
	HierSelect(f);
	return f;
}

void EditorUI::GroupSelection(bool asFolder)
{
	std::vector<Atom*> top = SelectionTopLevel();
	if (top.empty()) return;
	AppInstance* app = AppInstance::GetSingleton();
	World* w = app->currentWorld;

	// A real pivot: the selection's world-bounds center (folders stay at identity).
	Vector3 mn(1e18, 1e18, 1e18), mx(-1e18, -1e18, -1e18);
	for (Atom* a : top)
	{
		Vector3 p = a->GetTransform().globalPosition();
		mn.x = std::min(mn.x, p.x); mn.y = std::min(mn.y, p.y); mn.z = std::min(mn.z, p.z);
		mx.x = std::max(mx.x, p.x); mx.y = std::max(mx.y, p.y); mx.z = std::max(mx.z, p.z);
	}
	Vector3 center((mn.x + mx.x) * 0.5, (mn.y + mx.y) * 0.5, (mn.z + mx.z) * 0.5);

	// Common parent when every member shares one; mixed parents group at the root.
	Atom* commonParent = top[0]->parent;
	for (Atom* a : top) if (a->parent != commonParent) { commonParent = nullptr; break; }

	Atom* g = new Atom(asFolder ? "Folder" : "Group");
	g->folder = asFolder;
	w->InsertAtom(g, commonParent ? (long)commonParent->id.id : 0, -1);
	if (!asFolder) g->GetTransform().SetGlobal(center, Quaternion(0, 0, 0, 1), Vector3(1, 1, 1));
	const AtomPlace gPlace = PlaceOf(this, g);   // captured EMPTY: redo recreates the shell first

	std::vector<AtomPlace> before, after;
	for (Atom* a : top)
	{
		before.push_back(PlaceOf(this, a));
		Transform& t = a->GetTransform();
		Vector3 wp = t.globalPosition(); Quaternion wr = t.globalRotation(); Vector3 ws = t.globalScale();
		w->Reparent(a, g);
		t.SetGlobal(wp, wr, ws);
		after.push_back(PlaceOf(this, a));
	}
	PushUndo(std::string(asFolder ? "Group into folder" : "Group") + " (" + std::to_string(top.size()) + ")",
		[this, before, gPlace]{
			for (auto it = before.rbegin(); it != before.rend(); ++it)
				ApplyAtomState(it->id, it->parent, it->index, it->json);   // members back out
			ApplyAtomState(gPlace.id, gPlace.parent, gPlace.index, std::string());   // shell away
		},
		[this, after, gPlace]{
			ApplyAtomState(gPlace.id, gPlace.parent, gPlace.index, gPlace.json);     // empty shell
			for (const AtomPlace& p : after) ApplyAtomState(p.id, p.parent, p.index, p.json);
		});
	HierSelect(g);
}

void EditorUI::UngroupSelection()
{
	std::vector<Atom*> top = SelectionTopLevel();
	AppInstance* app = AppInstance::GetSingleton();
	World* w = app->currentWorld;
	bool any = false;
	for (Atom* g : top)
	{
		if (!g || g->children.empty()) continue;
		std::vector<Atom*> members(g->children.begin(), g->children.end());
		std::vector<AtomPlace> before, after;
		const long gParent = g->parent ? g->parent->id.id : 0;
		for (Atom* a : members)
		{
			before.push_back(PlaceOf(this, a));
			Transform& t = a->GetTransform();
			Vector3 wp = t.globalPosition(); Quaternion wr = t.globalRotation(); Vector3 ws = t.globalScale();
			w->Reparent(a, g->parent);
			t.SetGlobal(wp, wr, ws);
			after.push_back(PlaceOf(this, a));
		}
		const AtomPlace gPlace = PlaceOf(this, g);   // captured EMPTY (members already out)
		PushUndo("Ungroup " + g->GetName(),
			[this, before, gPlace]{
				ApplyAtomState(gPlace.id, gPlace.parent, gPlace.index, gPlace.json);   // shell back
				for (const AtomPlace& p : before) ApplyAtomState(p.id, p.parent, p.index, p.json);
			},
			[this, after, gPlace]{
				for (const AtomPlace& p : after) ApplyAtomState(p.id, p.parent, p.index, p.json);
				ApplyAtomState(gPlace.id, gPlace.parent, gPlace.index, std::string());
			});
		pendingDeleteIds.push_back(g->id.id);
		any = true;
		(void)gParent;
	}
	if (any) HierSelect(nullptr);
}
