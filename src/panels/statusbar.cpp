// Status bar (roadmap 2.3) — a bottom viewport side-bar: frame stats, backend,
// scene counters, process memory, then every plugin-registered StatusBar field.
#include <editor/editorui.h>
#include <API/Model/StatusBar.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Psapi.h>   // GetProcessMemoryInfo (working set)
#pragma comment(lib, "Psapi.lib")
#endif

static int CountAtoms(bc::list<Atom*>& gos)
{
	int n = 0;
	for (Atom* go : gos)
	{
		if (!go) continue;
		++n;
		if (!go->children.empty()) n += CountAtoms(go->children);
	}
	return n;
}

void EditorUI::StatusBarPanel()
{
	ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
	float barH = ImGui::GetTextLineHeightWithSpacing() + 8.0f;
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
	if (ImGui::BeginViewportSideBar("##nuke-statusbar", vp, ImGuiDir_Down, barH, flags))
	{
		AppInstance* app = AppInstance::GetSingleton();
		iRender* r = app->render;

		// FPS / frame time — smoothed so the numbers are readable, not a blur.
		const double dt = nuke::Time::getSingleton()->delta;
		static double emaDt = 1.0 / 60.0;
		if (dt > 0.0 && dt < 1.0) emaDt = emaDt * 0.95 + dt * 0.05;
		ImGui::Text("%.0f fps", 1.0 / emaDt);
		ImGui::SameLine(); ImGui::TextDisabled("%.2f ms", emaDt * 1000.0);

		auto sep = [] { ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine(); };

		// Renderer: backend + scene geometry submitted last frame.
		sep();
		ImGui::TextUnformatted(r ? r->getEngine() : "no renderer");
		int draws = 0, tris = 0;
		if (r) r->getFrameStats(draws, tris);
		sep();
		ImGui::Text("%d draws", draws);
		ImGui::SameLine(); ImGui::TextDisabled("%d tris", tris);

		// World: atom count (+ play state marker).
		sep();
		ImGui::Text("%d atoms", app->currentScene ? CountAtoms(app->currentScene->GetHierarchy()) : 0);
		if (app->playState != 0)
		{
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.45f, 1.0f), app->playState == 1 ? "PLAY" : "PAUSE");
		}

		// Process memory (working set).
#ifdef _WIN32
		{
			static int memTick = 0; static double memMB = 0.0;
			if ((memTick++ % 30) == 0)   // cheap, but no need to query every frame
			{
				PROCESS_MEMORY_COUNTERS pmc{};
				if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
					memMB = (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
			}
			sep();
			ImGui::Text("%.0f MB", memMB);
		}
#endif

		// Plugin fields (nuke::StatusBar::Set), in first-set order.
		for (const auto& f : nuke::StatusBar::All())
		{
			if (f.second.empty()) continue;
			sep();
			ImGui::TextUnformatted(f.second.c_str());
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.first.c_str());
		}
	}
	ImGui::End();
	ImGui::PopStyleVar();
}
