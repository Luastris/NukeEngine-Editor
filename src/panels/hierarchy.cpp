// hierarchy panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>

// ---- panels ----
void EditorUI::DisplayRecursiveAtomHierarchy(bc::list<Atom*>& gos)
{
	int i = 0;
	for (auto go : gos)
	{
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
		if (AppInstance::GetSingleton()->selectedInHieararchy == go)
			flags |= ImGuiTreeNodeFlags_Selected;
		if (go->children.size() == 0)
			flags |= ImGuiTreeNodeFlags_Leaf;

		bool opened = ImGui::TreeNodeEx((void*)(intptr_t)i, flags, "%s", go->GetName().c_str());
		if (ImGui::IsItemClicked())
			AppInstance::GetSingleton()->selectedInHieararchy = go;
		if (opened)
		{
			if (go->children.size() > 0)
				DisplayRecursiveAtomHierarchy(go->children);
			ImGui::TreePop();
		}
		++i;
	}
}

void EditorUI::winHierarchy()
{
	if (!win->hierarchy) return;
	ImGui::Begin("Hierarchy", &win->hierarchy, window_flags);
	DisplayRecursiveAtomHierarchy(AppInstance::GetSingleton()->currentScene->GetHierarchy());
	ImGui::End();
}
