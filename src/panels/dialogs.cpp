// dialogs panel — EditorUI method definitions (translation unit).
#include <editor/editorui.h>

void EditorUI::winAbout()
{
	if (!win->about) return;
	ImGui::Begin("About", &win->about, window_flags);
	ImGui::TextWrapped("NukeEngine - free, modular game engine. Renderer (Diligent) and UI (ImGui) "
	                   "are loaded as independent modules and communicate only through a neutral seam.");
	ImGui::End();
}

void EditorUI::winConsole()
{
	if (!win->console) return;
	ImGui::Begin("Console", &win->console, window_flags);
	ImGui::End();
}
