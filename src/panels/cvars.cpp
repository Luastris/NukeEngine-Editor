// Cvars panel: every console variable in one grid - value editable in place (Enter applies,
// bools are checkboxes), default, flags, description; filter; reset from the row's context menu.
#include <editor/editorui.h>
#include <API/Model/Cvar.h>
#include <cfloat>

namespace {
int ResizeCb(ImGuiInputTextCallbackData* d)
{
	if (d->EventFlag == ImGuiInputTextFlags_CallbackResize)
	{
		std::string* s = (std::string*)d->UserData;
		s->resize((size_t)d->BufTextLen);
		d->Buf = s->data();
	}
	return 0;
}
bool InputStd(const char* label, const char* hint, std::string& s, ImGuiInputTextFlags flags = 0)
{
	if (s.capacity() < 16) s.reserve(16);
	return ImGui::InputTextWithHint(label, hint, s.data(), s.capacity() + 1, flags | ImGuiInputTextFlags_CallbackResize, ResizeCb, &s);
}
const char* TypeName(nuke::CvarType t)
{
	switch (t) { case nuke::CvarType::Bool: return "bool"; case nuke::CvarType::Int: return "int"; case nuke::CvarType::Float: return "float"; default: return "string"; }
}
}

void EditorUI::winCvars()
{
	if (!cvarsOpen) return;
	if (cvarsFocus) { ImGui::SetNextWindowFocus(); }
	NukeUI::DocPanel("panel:cvars", ICON_LC_SLIDERS_HORIZONTAL " Cvars", &cvarsOpen,
	                 window_flags, 960, 480, [this]()
	{
	static std::string filter, status;
	static std::map<std::string, std::string> editing;   // name -> text being typed (applied on Enter)
	const ImGuiStyle& st = ImGui::GetStyle();

	// ---- header: filter + count ----
	ImGui::SetNextItemWidth(320);
	InputStd("##filter", ICON_LC_SEARCH " filter (prefix: r. g. loc. ...)", filter);
	std::vector<std::string> names = nuke::Cvars::Names();
	std::vector<nuke::CvarInfo> rows;
	for (const std::string& n : names)
	{
		nuke::CvarInfo i;
		if (!nuke::Cvars::Info(n, i)) continue;
		if (!filter.empty() && n.find(filter) == std::string::npos && i.description.find(filter) == std::string::npos) continue;
		rows.push_back(i);
	}
	ImGui::SameLine();
	ImGui::TextDisabled("%d of %d", (int)rows.size(), (int)names.size());
	if (!status.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", status.c_str()); }
	ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 150));
	ImGui::TextDisabled(ICON_LC_TERMINAL " console: name [value]");
	ImGui::Spacing();

	// ---- grid ----
	const ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg;
	if (ImGui::BeginTable("cvars", 5, tf))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Name",        ImGuiTableColumnFlags_WidthFixed, 200);
		ImGui::TableSetupColumn("Value",       ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("Default",     ImGuiTableColumnFlags_WidthFixed, 100);
		ImGui::TableSetupColumn("Type / flags", ImGuiTableColumnFlags_WidthFixed, 150);
		ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableHeadersRow();
		if (rows.empty())
		{
			ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
			ImGui::TextDisabled(names.empty() ? "No cvars registered" : "Nothing matches the filter");
		}
		for (const nuke::CvarInfo& i : rows)
		{
			ImGui::PushID(i.name.c_str());
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			const bool changed = !i.bound && i.value != i.defaultValue;
			if (changed) ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.5f, 1.0f), "%s", i.name.c_str());
			else         ImGui::TextUnformatted(i.name.c_str());
			if (ImGui::BeginPopupContextItem("ctx"))
			{
				ImGui::TextDisabled("%s", i.name.c_str());
				ImGui::Separator();
				if (ImGui::MenuItem(ICON_LC_COPY " Copy name")) ImGui::SetClipboardText(i.name.c_str());
				ImGui::BeginDisabled(i.bound || !changed);
				if (ImGui::MenuItem(ICON_LC_UNDO_2 " Reset to default")) { nuke::Cvars::Reset(i.name); editing.erase(i.name); }
				ImGui::EndDisabled();
				ImGui::EndPopup();
			}

			ImGui::TableSetColumnIndex(1);
			const bool ro = (i.flags & nuke::CvarReadOnly) != 0;
			ImGui::BeginDisabled(ro);
			if (i.type == nuke::CvarType::Bool)
			{
				bool v = i.value == "true";
				if (ImGui::Checkbox("##b", &v)) { std::string err; if (!nuke::Cvars::SetFrom(i.name, v ? "true" : "false", false, &err)) status = err; else status.clear(); }
			}
			else
			{
				// The text being typed lives in `editing` until Enter; a cvar changed elsewhere shows live.
				auto it = editing.find(i.name);
				std::string text = it != editing.end() ? it->second : i.value;
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (InputStd("##v", "", text, ImGuiInputTextFlags_EnterReturnsTrue))
				{
					std::string err;
					if (nuke::Cvars::SetFrom(i.name, text, false, &err)) { status.clear(); editing.erase(i.name); }
					else status = err;
				}
				else if (ImGui::IsItemActive()) editing[i.name] = text;
				else if (!ImGui::IsItemActive() && it != editing.end() && ImGui::IsItemDeactivated()) editing.erase(i.name);
			}
			ImGui::EndDisabled();

			ImGui::TableSetColumnIndex(2);
			ImGui::AlignTextToFramePadding();
			if (i.bound) ImGui::TextDisabled("(engine)"); else ImGui::TextDisabled("%s", i.defaultValue.empty() ? "\"\"" : i.defaultValue.c_str());

			ImGui::TableSetColumnIndex(3);
			ImGui::AlignTextToFramePadding();
			std::string fl = TypeName(i.type);
			if (i.flags & nuke::CvarArchive)  fl += "  " ICON_LC_SAVE;
			if (i.flags & nuke::CvarCheat)    fl += "  " ICON_LC_SKULL;
			if (i.flags & nuke::CvarReadOnly) fl += "  " ICON_LC_LOCK;
			ImGui::TextDisabled("%s", fl.c_str());
			if (ImGui::IsItemHovered() && i.flags)
			{
				std::string tip;
				if (i.flags & nuke::CvarArchive)  tip += "archive - saved in config/main.json [\"cvars\"]\n";
				if (i.flags & nuke::CvarCheat)    tip += "cheat - the console sets it only while enabled\n";
				if (i.flags & nuke::CvarReadOnly) tip += "read-only\n";
				if (!tip.empty()) tip.pop_back();
				ImGui::SetTooltip("%s", tip.c_str());
			}

			ImGui::TableSetColumnIndex(4);
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%s", i.description.c_str());
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	});
	cvarsFocus = false;
}
