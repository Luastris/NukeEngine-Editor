// plugins panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>
#include <algorithm>
#include <cctype>
#include <set>

// Case-insensitive substring match for the plugin filter.
static bool ContainsCI(const std::string& hay, const std::string& needle)
{
	if (needle.empty()) return true;
	auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
	                      [](char a, char b) { return tolower(a) == tolower(b); });
	return it != hay.end();
}

static bool PassesFilter(nuke::NUKEModule* m, const std::string& text, const std::string& service)
{
	if (!service.empty())
	{
		if (service == "utility") { if (*m->provides()) return false; }
		else if (service != m->provides()) return false;
	}
	if (text.empty()) return true;
	if (ContainsCI(m->title, text) || ContainsCI(m->moduleFile, text)) return true;
	for (auto& t : m->tags)
		if (ContainsCI(t, text)) return true;
	return false;
}

void EditorUI::PluginMGRWindow()
{
	if (!win->plugmgr) return;
	if (ImGui::Begin("Plugins", &win->plugmgr, window_flags))
	{
		auto& mods = nuke::GetModules();

		// Distinct services present in the pool -> the filter combo (All / Utility / <service>...).
		std::set<std::string> services;
		for (auto& m : mods)
			if (m && *m->provides()) services.insert(m->provides());
		std::vector<std::string> combo = { "All", "Utility" };
		combo.insert(combo.end(), services.begin(), services.end());
		if (pluginServiceFilter >= (int)combo.size()) pluginServiceFilter = 0;

		ImGui::SetNextItemWidth(160);
		if (ImGui::BeginCombo("##svcfilter", combo[pluginServiceFilter].c_str()))
		{
			for (int i = 0; i < (int)combo.size(); ++i)
				if (ImGui::Selectable(combo[i].c_str(), pluginServiceFilter == i))
					pluginServiceFilter = i;
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1);
		ImGui::InputTextWithHint("##plugsearch", "search name / tags...", pluginFilter, sizeof(pluginFilter));

		const std::string svcSel = pluginServiceFilter == 0 ? ""
		                         : pluginServiceFilter == 1 ? "utility"
		                         : combo[pluginServiceFilter];

		// Left: the plugin list (one list for everything — service-ness is just a badge).
		ImGui::BeginChild("pluglist", ImVec2(260, 0), ImGuiChildFlags_Borders);
		int idx = 0;
		for (auto& mod : mods)   // shared pool, single instance in the engine DLL
		{
			ImGui::PushID(idx);
			const bool visible = mod && PassesFilter(mod.get(), pluginFilter, svcSel);
			if (!visible) { ImGui::PopID(); ++idx; continue; }

			const char* service = mod->provides();
			const bool  isBoot  = mod->phase() == nuke::PHASE_BOOT;
			// A boot provider "checked" state shows the PROJECT CHOICE (what runs next start),
			// not just what is live now — that's how a pending restart-switch stays visible.
			bool on = mod->loaded;
			if (isBoot && *service && serviceChoices.count(service))
				on = (serviceChoices[service] == mod->moduleFile);
			if (ImGui::Checkbox("##en", &on))   // applied + persisted after the frame
				pendingPluginToggle.push_back({ mod.get(), on });

			ImGui::SameLine();
			bool sel = (selectedPluginIndex == idx);
			if (!mod->loaded) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			if (ImGui::Selectable(mod->title, sel))
			{
				selectedPluginIndex = idx;
				selectedPlugin = mod;
			}
			if (!mod->loaded) ImGui::PopStyleColor();

			if (*service)   // service badge on the right of the row
			{
				ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(service).x - 8);
				ImGui::TextDisabled("[%s]", service);
			}
			ImGui::PopID();
			++idx;
		}
		ImGui::EndChild();

		ImGui::SameLine();

		// Right: selected plugin details + its inline settings panel.
		ImGui::BeginChild("plugdetails", ImVec2(0, 0));
		if (selectedPlugin)
		{
			ImGui::TextUnformatted(selectedPlugin->title);
			ImGui::TextUnformatted(selectedPlugin->author);
			ImGui::TextUnformatted(selectedPlugin->version);
			ImGui::Text("%s", selectedPlugin->moduleFile.c_str());
			if (*selectedPlugin->provides())
			{
				ImGui::Text("Provides: %s", selectedPlugin->provides());
				ImGui::SameLine();
				ImGui::TextDisabled(selectedPlugin->sharedService()
				                    ? "(shared service — providers load side by side)"
				                    : "(one active provider per service)");
			}
			if (!selectedPlugin->tags.empty())
			{
				std::string tags;
				for (auto& t : selectedPlugin->tags) { if (!tags.empty()) tags += ", "; tags += t; }
				ImGui::Text("Tags: %s", tags.c_str());
			}
			ImGui::TextWrapped("%s", selectedPlugin->description);

			bool on = selectedPlugin->loaded;
			const char* service = selectedPlugin->provides();
			const bool  isBoot  = selectedPlugin->phase() == nuke::PHASE_BOOT;
			if (isBoot && *service && serviceChoices.count(service))
				on = (serviceChoices[service] == selectedPlugin->moduleFile);
			if (ImGui::Checkbox("Loaded for this project", &on))
				pendingPluginToggle.push_back({ selectedPlugin.get(), on });
			if (isBoot)
			{
				ImGui::SameLine();
				if (on && !selectedPlugin->loaded)
					ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "applies after restart");
				else
					ImGui::TextDisabled("boot service — switching applies after restart");
			}
			if (selectedPlugin->loaded && selectedPlugin->HasSettings())
			{
				ImGui::SeparatorText("Settings");
				selectedPlugin->Settings();   // plugin draws its settings inline (a panel here)
			}
		}
		else
		{
			ImGui::TextWrapped("Select a plugin on the left. To install one, put its DLL in the `modules` directory.");
		}
		ImGui::EndChild();
	}
	ImGui::End();
}
