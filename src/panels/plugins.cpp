// plugins panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>

void EditorUI::PluginMGRWindow()
{
	if (!win->plugmgr) return;
	if (ImGui::Begin("Plugins", &win->plugmgr, window_flags))
	{
		// Left: the list of loaded plugins.
		ImGui::BeginChild("pluglist", ImVec2(180, 0), ImGuiChildFlags_Borders);
		int idx = 0;
		for (auto& mod : nuke::GetModules())   // shared pool, single instance in the engine DLL
		{
			ImGui::PushID(idx);
			bool on = mod->loaded;
			if (ImGui::Checkbox("##en", &on))   // load / unload (applied + persisted after the frame)
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
			ImGui::TextWrapped("%s", selectedPlugin->description);

			bool on = selectedPlugin->loaded;
			if (ImGui::Checkbox("Loaded for this project", &on))
				pendingPluginToggle.push_back({ selectedPlugin.get(), on });
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
