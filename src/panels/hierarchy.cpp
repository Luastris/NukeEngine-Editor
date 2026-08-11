// hierarchy panel — tree, icons, search, drag&drop (reparent + asset instantiate), focus. EditorUI.
#include <editor/editorui.h>
#include "nukeui.h"   // DocPanel: detachable panels
#include <API/Model/Light.h>
#include <cmath>

static bool hierCI(const std::string& hay, const std::string& needle)
{
	if (needle.empty()) return true;
	auto low = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
	return low(hay).find(low(needle)) != std::string::npos;
}

const char* EditorUI::AtomIcon(Atom* atom)
{
	if (atom->folder)                       return ICON_LC_FOLDER;
	if (atom->GetComponent<Camera>())       return ICON_LC_VIDEO;
	if (Light* l = atom->GetComponent<Light>())   // light by type: sun / bulb / spotlight
		return l->type == 0 ? ICON_LC_SUN : (l->type == 2 ? ICON_LC_SPOTLIGHT : ICON_LC_LIGHTBULB);
	if (atom->GetComponent<MeshRenderer>()) return ICON_LC_BOX;
	return ICON_LC_ATOM;
}

bool EditorUI::HierMatch(Atom* atom)
{
	std::string q = hierSearch;
	if (q.empty()) return true;
	if (hierCI(atom->GetName(), q)) return true;
	for (Component* c : atom->components)
	{
		if (!c) continue;
		if (nuke::UnknownComponent* uc = dynamic_cast<nuke::UnknownComponent*>(c)) { if (hierCI(uc->typeName, q)) return true; }
		else if (nuke::TypeInfo* ti = c->GetType())                                { if (hierCI(ti->name, q)) return true; }
		else if (hierCI(c->name, q))                                                return true;
	}
	return false;
}

bool EditorUI::HierMatchDeep(Atom* atom)
{
	if (HierMatch(atom)) return true;
	for (Atom* ch : atom->children) if (ch && HierMatchDeep(ch)) return true;
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
	camFocusTarget = target - c->direction() * dist;   // keep orientation
	camFocusing    = true;
}

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

void EditorUI::DrawAtomNode(Atom* atom)
{
	if (!atom) return;
	AppInstance* app = AppInstance::GetSingleton();
	bool searching = (hierSearch[0] != 0);
	if (searching && !HierMatchDeep(atom)) return;   // hide non-matching subtrees while searching

	ImGui::PushID(atom);
	hierRows.push_back(atom);   // visible-row order: shift ranges resolve against last frame's list
	ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (app->IsSelected(atom)) fl |= ImGuiTreeNodeFlags_Selected;
	if (atom->children.empty())            fl |= ImGuiTreeNodeFlags_Leaf;
	if (searching)                       ImGui::SetNextItemOpen(true);   // reveal matches
	// A selection made ELSEWHERE (viewport click, script, undo) must become visible here: the
	// branch above it opens and the row scrolls into view, once per selection change.
	if (hierRevealPending)
	{
		bool isAncestor = false;
		for (Atom* p = app->selectedInHieararchy ? app->selectedInHieararchy->parent : nullptr; p; p = p->parent)
			if (p == atom) { isAncestor = true; break; }
		if (isAncestor) ImGui::SetNextItemOpen(true);
	}

	std::string rowLabel = std::string(AtomIcon(atom)) + " " + atom->GetName();
	if (!atom->modOrigin.empty()) rowLabel += "  [" + atom->modOrigin + "]";
	bool dim = !atom->enabled;
	for (Atom* p = atom->parent; !dim && p; p = p->parent) dim = !p->enabled;
	if (dim) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	bool open = ImGui::TreeNodeEx(rowLabel.c_str(), fl);
	if (dim) ImGui::PopStyleColor();
	if (hierRevealPending && app->selectedInHieararchy == atom)
	{
		ImGui::SetScrollHereY(0.5f);   // centre the row
		hierRevealPending = false;
	}
	if (!atom->modOrigin.empty() && ImGui::IsItemHovered())
		ImGui::SetTooltip("Added by mod: %s", atom->modOrigin.c_str());
	// Select on mouse RELEASE, not press: selecting on press switches the inspector mid-drag and kills
	// drag&drop into another component's field. The expand arrow toggles on PRESS — suppress its release.
	static bool s_toggleSuppress = false;
	if (ImGui::IsItemToggledOpen()) s_toggleSuppress = true;
	if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)
	    && !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left) && !s_toggleSuppress)
	{
		ImGuiIO& io = ImGui::GetIO();
		if (io.KeyShift)     HierRange(atom, io.KeyCtrl);
		else if (io.KeyCtrl) HierToggle(atom);
		else                 HierSelect(atom);
	}
	if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		s_toggleSuppress = false;   // cleared once the click fully settles
	if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { HierSelect(atom); FocusSelected(); }

	// Explorer semantics: right-clicking INSIDE the multi-selection keeps it (ops act on all).
	if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		if (!app->IsSelected(atom)) HierSelect(atom);
	if (ImGui::BeginPopupContextItem("##atomctx"))
	{
		const int nSel = (int)std::max<size_t>(app->Selection().size(), 1);
		const std::string sfx = nSel > 1 ? " (" + std::to_string(nSel) + ")" : "";
		if (ImGui::MenuItem((std::string(ICON_LC_COPY " Copy") + sfx).c_str()))           CopySelection();
		if (ImGui::MenuItem((std::string(ICON_LC_SCISSORS " Cut") + sfx).c_str()))        CutSelection();
		if (ImGui::MenuItem(ICON_LC_CLIPBOARD_PASTE " Paste", nullptr, false, AtomClipboardAvailable())) PasteAtoms();
		// Structural ops DEFER past the walk: reparent/insert here would corrupt the lists
		// the tree iteration is standing on (the same reason DnD defers).
		if (ImGui::MenuItem((std::string(ICON_LC_COPY_PLUS " Duplicate") + sfx).c_str()))
			hierPendingOps.push_back([this]{ DuplicateSelection(); });
		ImGui::Separator();
		if (ImGui::MenuItem(ICON_LC_FOLDER_PLUS " New Folder"))
			{ Atom* p = atom; hierPendingOps.push_back([this, p]{ CreateFolderAtom(p); }); }
		if (ImGui::MenuItem((std::string(ICON_LC_GROUP " Group") + sfx).c_str(), "Ctrl+G"))
			hierPendingOps.push_back([this]{ GroupSelection(false); });
		if (ImGui::MenuItem((std::string(ICON_LC_FOLDER_INPUT " Group into Folder") + sfx).c_str()))
			hierPendingOps.push_back([this]{ GroupSelection(true); });
		if (ImGui::MenuItem(ICON_LC_UNGROUP " Ungroup", "Ctrl+Shift+G", false, !atom->children.empty()))
			hierPendingOps.push_back([this]{ UngroupSelection(); });
		ImGui::Separator();
		if (ImGui::MenuItem("Enabled", nullptr, atom->enabled)) SetSelectionEnabled(!atom->enabled);
		ImGui::Separator();
		if (ImGui::MenuItem((std::string(ICON_LC_TRASH_2 " Delete") + sfx).c_str()))      DeleteSelection();
		ImGui::EndPopup();
	}

	if (ImGui::BeginDragDropSource())
	{
		// Payload stays a single Atom* (every existing consumer keeps working); dropping an atom
		// that is PART of the multi-selection moves the whole selection (deferred apply below).
		ImGui::SetDragDropPayload("NUKE_ATOM", &atom, sizeof(Atom*));
		const size_t n = app->IsSelected(atom) ? app->Selection().size() : 1;
		if (n > 1) ImGui::Text("%zu atoms", n);
		else       ImGui::TextUnformatted(atom->GetName().c_str());
		ImGui::EndDragDropSource();
	}
	// Drop ON the row body = make a child; the gaps above/below handle reorder / level changes.
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NUKE_ATOM"))
		{
			dndAtom = *(Atom**)p->Data; dndBefore = nullptr; dndParent = atom; dndPending = true;   // deferred
		}
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NUKE_ASSET"))
		{
			dndAsset = std::string((const char*)p->Data); dndAssetParent = atom;   // deferred
		}
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NUKE_COMPONENT"))
		{
			auto* cp = (const CompDragPayload*)p->Data;                               // deferred
			dndCompAtomId = cp->atomId; dndCompId = cp->compId; dndCompDst = atom;
		}
		ImGui::EndDragDropTarget();
	}

	HierGap(atom);

	if (open)
	{
		for (Atom* ch : atom->children) DrawAtomNode(ch);
		ImGui::TreePop();
	}
	ImGui::PopID();
}

void EditorUI::winHierarchy()
{
	if (!win->hierarchy) return;
	NukeUI::DocPanel("panel:hierarchy", "Hierarchy", &win->hierarchy,
	                 window_flags | ImGuiWindowFlags_HorizontalScrollbar, 300, 620, [this]()
	{
	if (bootLoading) { ImGui::TextDisabled("Loading project..."); return; }   // world still streaming in
	AppInstance* app = AppInstance::GetSingleton();

	ImGui::SetNextItemWidth(-1);
	ImGui::InputTextWithHint("##hsearch", ICON_LC_SEARCH " Search (atom or component)", hierSearch, sizeof(hierSearch));
	ImGui::Separator();

	// Editor camera pinned at the top, outside the tree: not draggable/reparentable.
	if (Atom* cam = app->currentWorld->Get("Editor Camera"))
	{
		bool sel = (app->selectedInHieararchy == cam);
		if (ImGui::Selectable((std::string(ICON_LC_VIDEO) + " " + cam->GetName() + "##editorcam").c_str(), sel))
			app->selectedInHieararchy = cam;
		ImGui::Separator();
	}

	if (app->selectedInHieararchy != hierLastSel)
	{
		hierLastSel = app->selectedInHieararchy;
		hierRevealPending = app->selectedInHieararchy != nullptr;
	}
	hierRowsPrev.swap(hierRows);   // last frame's visible order backs shift ranges
	hierRows.clear();

	for (Atom* atom : app->currentWorld->GetHierarchy())
		if (atom && atom->GetName() != "Editor Camera")
			DrawAtomNode(atom);

	// Empty area below = drop target for root.
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

	if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F))
		FocusSelected();

	// Chords act only while this panel is focused; bindings come from the shared rebindable pool.
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput)
	{
		nuke::Hotkeys* hk = nuke::Hotkeys::Get();
		nuke::Hotkey* d  = hk->Find("editor.delete");
		nuke::Hotkey* df = hk->Find("editor.delete.force");
		if ((d  && d->bound  && ImGui::IsKeyChordPressed((ImGuiKeyChord)d->chord)) ||
		    (df && df->bound && ImGui::IsKeyChordPressed((ImGuiKeyChord)df->chord)))
			DeleteSelection();
		auto chord = [&](const char* id) { nuke::Hotkey* h = hk->Find(id); return h && h->bound && ImGui::IsKeyChordPressed((ImGuiKeyChord)h->chord); };
		if (chord("editor.copy"))      CopySelection();
		if (chord("editor.cut"))       CutSelection();
		if (chord("editor.paste"))     hierPendingOps.push_back([this]{ PasteAtoms(); });
		if (chord("editor.duplicate")) hierPendingOps.push_back([this]{ DuplicateSelection(); });
		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_G))
			hierPendingOps.push_back([this]{ UngroupSelection(); });
		else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G))
			hierPendingOps.push_back([this]{ GroupSelection(false); });
	}

	// Deferred structural ops: the walk is over, the lists are safe to mutate now.
	if (!hierPendingOps.empty())
	{
		std::vector<std::function<void()>> ops;
		ops.swap(hierPendingOps);
		for (auto& op : ops) op();
	}

	// Deferred DnD applies only after the tree is drawn: mutating the lists mid-iteration corrupts it.
	if (dndPending && dndAtom)
	{
		// Dragging a member of the multi-selection moves the WHOLE selection (top-level only);
		// anything else moves just the dragged atom. Dropping onto a selected atom is a no-op.
		std::vector<Atom*> movers;
		if (app->IsSelected(dndAtom) && app->Selection().size() > 1) movers = SelectionTopLevel();
		else                                                         movers.push_back(dndAtom);
		bool badTarget = false;
		for (Atom* m : movers)
			if (m == dndParent || m == dndBefore) { badTarget = true; break; }
		if (!badTarget)
			for (Atom* moved : movers)
			{
				// Snapshot BEFORE the move: undo needs the old-parent-relative transform.
				long oldParent = moved->parent ? moved->parent->id.id : 0;
				int  oldIndex  = 0;
				{ auto& lst = moved->parent ? moved->parent->children : app->currentWorld->GetHierarchy();
				  int i = 0; for (Atom* s : lst) { if (s == moved) { oldIndex = i; break; } ++i; } }
				std::string beforeJson = SaveAtomToString(moved);
				// Keep the WORLD pose across any parent change (gap-drops reparent too).
				Atom* wasParent = moved->parent;
				Transform& mt = moved->GetTransform();
				Vector3 wp = mt.globalPosition(); Quaternion wr = mt.globalRotation(); Vector3 ws = mt.globalScale();
				if (dndBefore) app->currentWorld->ReparentBefore(moved, dndBefore);
				else           app->currentWorld->Reparent(moved, dndParent);
				if (moved->parent != wasParent) mt.SetGlobal(wp, wr, ws);
				RecordReparent(moved, oldParent, oldIndex, beforeJson);
			}
	}
	dndPending = false; dndAtom = dndBefore = dndParent = nullptr;
	// Deferred component move (inspector header dropped on a row).
	if (dndCompDst)
	{
		Atom* srcA = app->currentWorld->GetById(dndCompAtomId);
		Component* c = nullptr;
		if (srcA) for (Component* cc : srcA->components) if (cc && (long)cc->id.id == dndCompId) { c = cc; break; }
		if (srcA && c) MoveComponent(srcA, c, dndCompDst);
		dndCompAtomId = 0; dndCompId = 0; dndCompDst = nullptr;
	}
	if (!dndAsset.empty())
	{
		if (Atom* a = DropAsset(dndAsset)) { if (dndAssetParent) app->currentWorld->Reparent(a, dndAssetParent); }
		dndAsset.clear(); dndAssetParent = nullptr;
	}

	});
}
