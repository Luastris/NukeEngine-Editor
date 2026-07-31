// .nuinput asset editor body: CRUD over actions, contexts and bindings.
// Edits the window's model copy (w.inActions / w.inContexts); Save writes the file and applies it live.
#include <editor/editorui.h>
#include <input/Input.h>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>

using nuke::Input;
using nuke::InputAction;
using nuke::InputBinding;
using nuke::InputContext;
using nuke::InputPhase;
using nuke::ActionValueType;

// ---- press-to-bind state, keyed by window path so open editors don't cross-capture ----
static std::string s_lWin;                 // listening window ("" = nobody listening)
static int s_lC = -1, s_lB = -1, s_lSlot = -1, s_lKind = 0;   // ctx / binding / slot (-1 = append) / kind (0 ctl, 1 mod)
static std::vector<std::string> s_lBase;   // controls already active when listening began

static const char* kPhases[] = { "Pressed", "Held", "Released", "Tap", "LongPress", "DoublePress" };

static std::vector<std::string> activeControls()
{
	std::vector<std::string> out;
	for (const std::string& id : Input::ListControls())
		if (std::fabs(Input::Control(id)) >= 0.5f) out.push_back(id);
	return out;
}
static void startListen(const std::string& win, int c, int b, int slot, int kind)
{ s_lWin = win; s_lC = c; s_lB = b; s_lSlot = slot; s_lKind = kind; s_lBase = activeControls(); }
static void stopListen() { s_lWin.clear(); s_lC = s_lB = s_lSlot = -1; }
static bool listeningOn(const std::string& win, int c, int b, int slot, int kind)
{ return s_lWin == win && s_lC == c && s_lB == b && s_lSlot == slot && s_lKind == kind; }
// First control that became active since listening started; baseline controls (Mouse.X/Y) are ignored.
static std::string capturePressed()
{
	for (const std::string& id : Input::ListControls())
		if (std::fabs(Input::Control(id)) >= 0.5f &&
			std::find(s_lBase.begin(), s_lBase.end(), id) == s_lBase.end()) return id;
	return std::string();
}

// One control/modifier chip list: [Key.W x] [Key.S x] [+]. Returns true if the list changed.
static bool drawChips(const std::string& win, int ci, int bi, int kind, std::vector<std::string>& ids)
{
	bool changed = false;
	int removeAt = -1;
	for (int i = 0; i < (int)ids.size(); ++i)
	{
		ImGui::PushID(i);
		bool listening = listeningOn(win, ci, bi, i, kind);
		const std::string label = listening ? "[press a key… Esc]" : (ids[i].empty() ? "<none>" : ids[i]);
		if (ImGui::Button((label + "##chip").c_str()))
		{ if (listening) stopListen(); else startListen(win, ci, bi, i, kind); }
		if (ImGui::IsItemHovered() && !listening) ImGui::SetTooltip("Click, then press the new key/button/axis");
		ImGui::SameLine(0, 2);
		if (ImGui::Button(ICON_LC_X)) removeAt = i;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove");
		ImGui::SameLine(0, 8);
		ImGui::PopID();
	}
	if (removeAt >= 0) { ids.erase(ids.begin() + removeAt); changed = true; if (s_lWin == win) stopListen(); }
	bool addListening = listeningOn(win, ci, bi, -1, kind);
	if (ImGui::Button(addListening ? "[press…]##add" : ICON_LC_PLUS "##add"))
	{ if (addListening) stopListen(); else startListen(win, ci, bi, -1, kind); }
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add: click, then press the key/button/axis");
	return changed;
}

// A trash button pushed to the right edge of the current line.
static bool trashRight(const char* id)
{
	ImGui::SameLine();
	float target = ImGui::GetWindowContentRegionMax().x - 30.0f;
	if (target > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(target);
	return ImGui::Button((std::string(ICON_LC_TRASH_2) + "##" + id).c_str());
}

std::string EditorUI::InputMapJson(AssetEditorWin& w)
{
	nuke::Input::InputMapData m; m.actions = w.inActions; m.contexts = w.inContexts;
	return nuke::Input::SerializeMap(m);
}
void EditorUI::LoadInputMapJson(AssetEditorWin& w, const std::string& json)
{
	nuke::Input::InputMapData m = nuke::Input::ParseMapString(json);
	w.inActions = m.actions; w.inContexts = m.contexts;
}
void EditorUI::SaveInputAsset(AssetEditorWin& w)
{
	std::string js = InputMapJson(w);
	{ bfs::ofstream f{ bfs::path(w.path) }; if (f) f << js; }
	nuke::Input::InputMapData m; m.actions = w.inActions; m.contexts = w.inContexts;
	nuke::Input::ApplyMap(m);   // a running PIE/game picks up the edited map immediately
	std::cout << "[editor]\tsaved input map -> " << w.path << std::endl;
}

void EditorUI::DrawInputEditor(AssetEditorWin& w)
{
	// Press-to-bind: capture into the listening slot of this window only.
	if (s_lWin == w.path && s_lB >= 0)
	{
		if (ImGui::IsKeyPressed(ImGuiKey_Escape)) stopListen();
		else if (std::string got = capturePressed(); !got.empty())
		{
			if (s_lC >= 0 && s_lC < (int)w.inContexts.size() &&
				s_lB >= 0 && s_lB < (int)w.inContexts[s_lC].bindings.size())
			{
				InputBinding& b = w.inContexts[s_lC].bindings[s_lB];
				std::vector<std::string>& list = (s_lKind == 0) ? b.controls : b.modifiers;
				if (s_lSlot < 0) list.push_back(got);
				else if (s_lSlot < (int)list.size()) list[s_lSlot] = got;
				w.dirty = true; w.editedNow = true;
			}
			stopListen();
		}
	}

	if (ImGui::Button(ICON_LC_PLAY " Apply to game"))   // push to live without a file write
	{ nuke::Input::InputMapData m; m.actions = w.inActions; m.contexts = w.inContexts; nuke::Input::ApplyMap(m); }
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Apply the edited map to the running game without saving the file");
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("%d action(s), %d context(s)", (int)w.inActions.size(), (int)w.inContexts.size());
	ImGui::Spacing();

	// ---- Actions: Name | Type | delete ----
	if (ImGui::CollapsingHeader("Actions", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Spacing();
		int delA = -1;
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6, 3));
		if (ImGui::BeginTable("##actions", 3, ImGuiTableFlags_RowBg, ImVec2(420.0f, 0)))
		{
			ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("##type", ImGuiTableColumnFlags_WidthFixed, 100.0f);
			ImGui::TableSetupColumn("##del",  ImGuiTableColumnFlags_WidthFixed, 30.0f);
			for (int i = 0; i < (int)w.inActions.size(); ++i)
			{
				ImGui::PushID(i);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				char name[64]; strncpy(name, w.inActions[i].name.c_str(), sizeof(name) - 1); name[sizeof(name) - 1] = 0;
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::InputText("##aname", name, sizeof(name))) { w.inActions[i].name = name; w.dirty = true; w.editedNow = true; }
				ImGui::TableNextColumn();
				int t = (int)w.inActions[i].type;
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::Combo("##atype", &t, "Bool\0Axis1\0Axis2\0")) { w.inActions[i].type = (ActionValueType)t; w.dirty = true; w.editedNow = true; }
				ImGui::TableNextColumn();
				if (ImGui::Button(ICON_LC_TRASH_2)) delA = i;
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		ImGui::PopStyleVar();
		if (delA >= 0) { w.inActions.erase(w.inActions.begin() + delA); w.dirty = true; w.editedNow = true; }
		if (ImGui::Button(ICON_LC_PLUS " Add action"))
		{ w.inActions.push_back({ "NewAction", ActionValueType::Bool }); w.dirty = true; w.editedNow = true; }
		ImGui::Spacing();
	}
	ImGui::Spacing();

	// ---- Contexts + bindings ----
	int delC = -1;
	for (int ci = 0; ci < (int)w.inContexts.size(); ++ci)
	{
		InputContext& ctx = w.inContexts[ci];
		ImGui::PushID(1000 + ci);
		bool act = ctx.active;
		if (ImGui::Checkbox("##cact", &act)) { ctx.active = act; w.dirty = true; w.editedNow = true; }
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Context active by default (scripts toggle it with Input.PushContext/PopContext)");
		ImGui::SameLine();
		std::string hdr = (ctx.name.empty() ? "<context>" : ctx.name)
		                + "   \xc2\xb7   priority " + std::to_string(ctx.priority) + "###ctxhdr";
		if (ImGui::CollapsingHeader(hdr.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Indent(6.0f);
			ImGui::Spacing();

			ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Name"); ImGui::SameLine();
			char cn[64]; strncpy(cn, ctx.name.c_str(), sizeof(cn) - 1); cn[sizeof(cn) - 1] = 0;
			ImGui::SetNextItemWidth(180);
			if (ImGui::InputText("##cn", cn, sizeof(cn))) { ctx.name = cn; w.dirty = true; w.editedNow = true; }
			ImGui::SameLine(0, 16);
			ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Priority"); ImGui::SameLine();
			ImGui::SetNextItemWidth(64);
			int prio = ctx.priority;
			if (ImGui::InputInt("##prio", &prio, 0, 0)) { ctx.priority = prio; w.dirty = true; w.editedNow = true; }
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Higher priority evaluates first; its 'consume' bindings hide controls from lower contexts");
			if (trashRight("delctx")) delC = ci;
			ImGui::Spacing();

			int delB = -1;
			for (int bi = 0; bi < (int)ctx.bindings.size(); ++bi)
			{
				InputBinding& b = ctx.bindings[bi];
				ImGui::PushID(bi);

				int phase = (int)b.phase; if (phase < 0 || phase > 5) phase = 0;
				std::string sum = b.action.empty() ? "<no action>" : b.action;
				if (!b.controls.empty())
				{
					sum += "  \xe2\x80\x94  ";
					for (size_t k = 0; k < b.controls.size(); ++k)
					{ if (k) sum += b.sequence ? " , " : " + "; sum += b.controls[k]; }
				}
				sum += std::string("  (") + kPhases[phase] + (b.sequence ? ", sequential)" : ")");

				bool open = ImGui::TreeNodeEx("##bind",
				                              ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap,
				                              "%s", sum.c_str());
				// Trash overlaps the right end of the header frame (AllowOverlap above).
				ImGui::SameLine();
				float tx = ImGui::GetWindowContentRegionMax().x - 30.0f;
				if (tx > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(tx);
				if (ImGui::SmallButton(ICON_LC_TRASH_2 "##delb")) delB = bi;

				if (open)
				{
					// The action's type drives which option columns appear below.
					ActionValueType atype = ActionValueType::Bool;
					for (const InputAction& a : w.inActions) if (a.name == b.action) { atype = a.type; break; }

					ImGui::Spacing();
					// Fixed label column instead of SameLine(x): survives any tree indent or font size.
					ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6, 3));
					if (ImGui::BeginTable("##bindrows", 2, ImGuiTableFlags_SizingFixedFit))
					{
						ImGui::TableSetupColumn("##l", ImGuiTableColumnFlags_WidthFixed, 72.0f);
						ImGui::TableSetupColumn("##c", ImGuiTableColumnFlags_WidthStretch);

						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Action");
						ImGui::TableNextColumn();
						ImGui::SetNextItemWidth(140);
						if (ImGui::BeginCombo("##act", b.action.empty() ? "<pick action>" : b.action.c_str()))
						{
							for (const InputAction& a : w.inActions)
								if (ImGui::Selectable(a.name.c_str(), a.name == b.action)) { b.action = a.name; w.dirty = true; w.editedNow = true; }
							ImGui::EndCombo();
						}
						ImGui::SameLine(0, 14);
						ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Phase"); ImGui::SameLine(0, 6);
						ImGui::SetNextItemWidth(110);
						if (ImGui::Combo("##phase", &phase, kPhases, IM_ARRAYSIZE(kPhases))) { b.phase = (InputPhase)phase; w.dirty = true; w.editedNow = true; }
						ImGui::SameLine(0, 14);
						if (ImGui::Checkbox("Sequential", &b.sequence)) { w.dirty = true; w.editedNow = true; }
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: all controls held together (chord)\nOn: controls pressed one after another (combo)");

						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Controls");
						ImGui::TableNextColumn();
						if (drawChips(w.path, ci, bi, 0, b.controls)) { w.dirty = true; w.editedNow = true; }

						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Modifiers");
						ImGui::TableNextColumn();
						if (drawChips(w.path, ci, bi, 1, b.modifiers)) { w.dirty = true; w.editedNow = true; }

						ImGui::EndTable();
					}
					ImGui::PopStyleVar();

					const bool showAxis = (atype == ActionValueType::Axis2);
					const bool showVal  = (atype != ActionValueType::Bool);
					int cols = 5 + (showAxis ? 1 : 0) + (showVal ? 3 : 0);   // Consume + 4 timings (+ Axis) (+ Scale/Dead/Invert)
					ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6, 3));
					if (ImGui::BeginTable("##opts", cols, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit))
					{
						if (showAxis) ImGui::TableSetupColumn("Axis",   ImGuiTableColumnFlags_WidthFixed, 44.0f);
						if (showVal)  ImGui::TableSetupColumn("Scale",  ImGuiTableColumnFlags_WidthFixed, 60.0f);
						if (showVal)  ImGui::TableSetupColumn("Deadzone", ImGuiTableColumnFlags_WidthFixed, 64.0f);
						if (showVal)  ImGui::TableSetupColumn("Invert",  ImGuiTableColumnFlags_WidthFixed, 46.0f);
						ImGui::TableSetupColumn("Consume", ImGuiTableColumnFlags_WidthFixed, 56.0f);
						ImGui::TableSetupColumn("Tap s",    ImGuiTableColumnFlags_WidthFixed, 54.0f);
						ImGui::TableSetupColumn("Long s",   ImGuiTableColumnFlags_WidthFixed, 54.0f);
						ImGui::TableSetupColumn("Double s", ImGuiTableColumnFlags_WidthFixed, 60.0f);
						ImGui::TableSetupColumn("Combo s",  ImGuiTableColumnFlags_WidthFixed, 58.0f);
						ImGui::TableHeadersRow();
						ImGui::TableNextRow();
						if (showAxis)
						{
							ImGui::TableNextColumn();
							int ax = (b.axis != 0) ? 1 : 0;
							ImGui::SetNextItemWidth(-FLT_MIN);
							if (ImGui::Combo("##axis", &ax, "X\0Y\0")) { b.axis = ax; w.dirty = true; w.editedNow = true; }
						}
						if (showVal)
						{
							ImGui::TableNextColumn();
							ImGui::SetNextItemWidth(-FLT_MIN);
							if (ImGui::DragFloat("##scale", &b.scale, 0.01f)) { w.dirty = true; w.editedNow = true; }
							ImGui::TableNextColumn();
							ImGui::SetNextItemWidth(-FLT_MIN);
							if (ImGui::DragFloat("##dead", &b.deadzone, 0.005f, 0.0f, 1.0f)) { w.dirty = true; w.editedNow = true; }
							ImGui::TableNextColumn();
							if (ImGui::Checkbox("##inv", &b.invert)) { w.dirty = true; w.editedNow = true; }
						}
						ImGui::TableNextColumn();
						if (ImGui::Checkbox("##cons", &b.consume)) { w.dirty = true; w.editedNow = true; }
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hide this binding's controls from lower-priority contexts while it's active");
						auto timingCell = [&](const char* id, float* v)
						{
							ImGui::TableNextColumn();
							ImGui::SetNextItemWidth(-FLT_MIN);
							if (ImGui::DragFloat(id, v, 0.01f, 0.0f, 5.0f, "%.2f")) { w.dirty = true; w.editedNow = true; }
						};
						timingCell("##tap",  &b.tapMax);
						timingCell("##long", &b.longMin);
						timingCell("##dbl",  &b.doubleWindow);
						timingCell("##seq",  &b.sequenceWindow);
						ImGui::EndTable();
					}
					ImGui::PopStyleVar();
					ImGui::Spacing();
					ImGui::TreePop();
				}
				ImGui::PopID();
			}
			if (delB >= 0) { ctx.bindings.erase(ctx.bindings.begin() + delB); w.dirty = true; w.editedNow = true; }

			ImGui::Spacing();
			if (ImGui::Button(ICON_LC_PLUS " Add binding"))
			{
				InputBinding nb; if (!w.inActions.empty()) nb.action = w.inActions[0].name;
				ctx.bindings.push_back(nb); w.dirty = true; w.editedNow = true;
			}
			ImGui::Unindent(6.0f);
			ImGui::Spacing();
		}
		ImGui::PopID();
	}
	if (delC >= 0) { w.inContexts.erase(w.inContexts.begin() + delC); w.dirty = true; w.editedNow = true; if (s_lWin == w.path) stopListen(); }

	ImGui::Spacing();
	if (ImGui::Button(ICON_LC_PLUS " Add context"))
	{ InputContext c; c.name = "NewContext"; c.active = true; w.inContexts.push_back(c); w.dirty = true; w.editedNow = true; }
}
