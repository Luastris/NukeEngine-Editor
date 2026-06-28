// hierarchy panel — tree, icons, search, drag&drop (reparent + asset instantiate), focus. EditorUI.
#include <editor/editorui.h>
#include <API/Model/Light.h>
#include <cmath>

static bool hierCI(const std::string& hay, const std::string& needle)
{
	if (needle.empty()) return true;
	auto low = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
	return low(hay).find(low(needle)) != std::string::npos;
}

const char* EditorUI::AtomIcon(Atom* go)
{
	if (go->GetComponent<Camera>())       return ICON_LC_VIDEO;
	if (Light* l = go->GetComponent<Light>())   // light by type: sun / bulb / spotlight
		return l->type == 0 ? ICON_LC_SUN : (l->type == 2 ? ICON_LC_SPOTLIGHT : ICON_LC_LIGHTBULB);
	if (go->GetComponent<MeshRenderer>()) return ICON_LC_BOX;
	return ICON_LC_ATOM;
}

bool EditorUI::HierMatch(Atom* go)
{
	std::string q = hierSearch;
	if (q.empty()) return true;
	if (hierCI(go->GetName(), q)) return true;
	for (Component* c : go->components)
	{
		if (!c) continue;
		if (nuke::UnknownComponent* uc = dynamic_cast<nuke::UnknownComponent*>(c)) { if (hierCI(uc->typeName, q)) return true; }
		else if (nuke::TypeInfo* ti = c->GetType())                                { if (hierCI(ti->name, q)) return true; }
		else if (hierCI(c->name, q))                                                return true;
	}
	return false;
}

bool EditorUI::HierMatchDeep(Atom* go)
{
	if (HierMatch(go)) return true;
	for (Atom* ch : go->children) if (ch && HierMatchDeep(ch)) return true;
	return false;
}

void EditorUI::FocusSelected()
{
	AppInstance* app = AppInstance::GetSingleton();
	Atom* sel = app->selectedInHieararchy;
	if (!sel || !editorCam || !editorCam->transform) return;
	Transform& t = sel->GetTransform();
	Vector3 target = t.globalPosition();
	Vector3 s      = t.globalScale();
	double radius = 0.5 * std::max({ std::fabs(s.x), std::fabs(s.y), std::fabs(s.z) });
	if (radius < 0.1) radius = 0.5;
	double half = (double)editorCam->fov * 0.5 * 0.01745329252;
	double dist = radius / std::max(0.05, std::tan(half)) + radius * 2.0;
	Transform* c = editorCam->transform;
	camFocusTarget = target - c->direction() * dist;   // keep orientation, look at the target
	camFocusing    = true;                              // smoothly lerp there (see winRender)
}

// A thin "insert before" zone overlaid on the TOP EDGE of the row just drawn. Only appears while an
// atom is being dragged. It restores the cursor afterwards, so it adds NO layout height — the tree
// stays as compact as normal; only the hit region + the orange insertion line sit on the edge.
void EditorUI::HierGap(Atom* before)
{
	const ImGuiPayload* drag = ImGui::GetDragDropPayload();
	if (!drag || !drag->IsDataType("NUKE_ATOM")) return;
	ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();   // the row just submitted
	ImVec2 saved = ImGui::GetCursorScreenPos();
	ImGui::SetCursorScreenPos(ImVec2(mn.x, mn.y - 3.0f));               // straddle the top edge
	ImGui::InvisibleButton("##ins", ImVec2(mx.x - mn.x, 6.0f));
	if (ImGui::BeginDragDropTarget())
	{
		ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mn.y), ImVec2(mx.x, mn.y), IM_COL32(255, 160, 30, 255), 2.0f);
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NUKE_ATOM"))
		{
			dndAtom = *(Atom**)p->Data; dndBefore = before; dndParent = nullptr; dndPending = true;   // deferred
		}
		ImGui::EndDragDropTarget();
	}
	ImGui::SetCursorScreenPos(saved);   // no extra height
}

void EditorUI::DrawAtomNode(Atom* go)
{
	if (!go) return;
	AppInstance* app = AppInstance::GetSingleton();
	bool searching = (hierSearch[0] != 0);
	if (searching && !HierMatchDeep(go)) return;   // hide non-matching subtrees while searching

	ImGui::PushID(go);
	ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (app->selectedInHieararchy == go) fl |= ImGuiTreeNodeFlags_Selected;
	if (go->children.empty())            fl |= ImGuiTreeNodeFlags_Leaf;
	if (searching)                       ImGui::SetNextItemOpen(true);   // reveal matches

	bool open = ImGui::TreeNodeEx((std::string(AtomIcon(go)) + " " + go->GetName()).c_str(), fl);
	if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) app->selectedInHieararchy = go;
	if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { app->selectedInHieararchy = go; FocusSelected(); }

	if (ImGui::BeginDragDropSource())
	{
		ImGui::SetDragDropPayload("NUKE_ATOM", &go, sizeof(Atom*));
		ImGui::TextUnformatted(go->GetName().c_str());
		ImGui::EndDragDropSource();
	}
	// Drop ON the row body = make a child; the gaps above/below handle reorder / level changes.
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NUKE_ATOM"))
		{
			dndAtom = *(Atom**)p->Data; dndBefore = nullptr; dndParent = go; dndPending = true;   // deferred
		}
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NUKE_ASSET"))
		{
			dndAsset = std::string((const char*)p->Data); dndAssetParent = go;   // deferred
		}
		ImGui::EndDragDropTarget();
	}

	HierGap(go);   // thin "insert before `go`" zone overlaid on this row's top edge (drag only)

	if (open)
	{
		for (Atom* ch : go->children) DrawAtomNode(ch);
		ImGui::TreePop();
	}
	ImGui::PopID();
}

void EditorUI::winHierarchy()
{
	if (!win->hierarchy) return;
	ImGui::Begin("Hierarchy", &win->hierarchy, window_flags);
	AppInstance* app = AppInstance::GetSingleton();

	ImGui::SetNextItemWidth(-1);
	ImGui::InputTextWithHint("##hsearch", ICON_LC_SEARCH " Search (atom or component)", hierSearch, sizeof(hierSearch));
	ImGui::Separator();

	// Editor camera pinned at the top, separate from the scene tree (not draggable/reparentable).
	if (Atom* cam = app->currentScene->Get("Editor Camera"))
	{
		bool sel = (app->selectedInHieararchy == cam);
		if (ImGui::Selectable((std::string(ICON_LC_VIDEO) + " " + cam->GetName() + "##editorcam").c_str(), sel))
			app->selectedInHieararchy = cam;
		ImGui::Separator();
	}

	// The scene tree (excludes the editor camera).
	for (Atom* go : app->currentScene->GetHierarchy())
		if (go && go->GetName() != "Editor Camera")
			DrawAtomNode(go);

	// Empty area below: drop target for re-parenting to root / instantiating an asset at root.
	ImVec2 rest = ImGui::GetContentRegionAvail();
	if (rest.y < 24.0f) rest.y = 24.0f;
	ImGui::InvisibleButton("##hroot", rest);
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NUKE_ATOM"))
		{
			dndAtom = *(Atom**)p->Data; dndBefore = nullptr; dndParent = nullptr; dndPending = true;   // -> root
		}
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NUKE_ASSET"))
		{
			dndAsset = std::string((const char*)p->Data); dndAssetParent = nullptr;   // -> root
		}
		ImGui::EndDragDropTarget();
	}

	// F frames the selected atom (when the hierarchy is focused; the viewport handles its own F).
	if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F))
		FocusSelected();

	// Delete (or Shift+Delete) removes the selected atom while the hierarchy is focused. Behaviour is
	// per-active-window; the chords come from the shared pool (rebindable). Atom deletes are undoable.
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput)
	{
		nuke::Hotkeys* hk = nuke::Hotkeys::Get();
		nuke::Hotkey* d  = hk->Find("editor.delete");
		nuke::Hotkey* df = hk->Find("editor.delete.force");
		if ((d  && d->bound  && ImGui::IsKeyChordPressed((ImGuiKeyChord)d->chord)) ||
		    (df && df->bound && ImGui::IsKeyChordPressed((ImGuiKeyChord)df->chord)))
			DeleteSelectedAtom();
	}

	// Apply deferred DnD now that the whole tree is drawn (mutating the lists mid-iteration corrupts it).
	if (dndPending && dndAtom)
	{
		// Capture the old placement first so the move is undoable.
		long oldParent = dndAtom->parent ? dndAtom->parent->id.id : 0;
		int  oldIndex  = 0;
		{ auto& lst = dndAtom->parent ? dndAtom->parent->children : app->currentScene->GetHierarchy();
		  int i = 0; for (Atom* s : lst) { if (s == dndAtom) { oldIndex = i; break; } ++i; } }
		Atom* moved = dndAtom;
		if (dndBefore) app->currentScene->ReparentBefore(dndAtom, dndBefore);
		else           app->currentScene->Reparent(dndAtom, dndParent);
		RecordReparent(moved, oldParent, oldIndex);
	}
	dndPending = false; dndAtom = dndBefore = dndParent = nullptr;
	if (!dndAsset.empty())
	{
		if (Atom* a = DropAsset(dndAsset)) { if (dndAssetParent) app->currentScene->Reparent(a, dndAssetParent); }
		dndAsset.clear(); dndAssetParent = nullptr;
	}

	ImGui::End();
}
