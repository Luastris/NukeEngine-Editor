// Status bar (roadmap 2.3) — a bottom viewport side-bar: frame stats, backend,
// scene counters, process memory, then every plugin-registered StatusBar field.
// Fields with a progress value are background JOBS: the active one shows inline with a
// progress bar, and the right-edge button drops UP a list of all of them + pool stats.
#include <editor/editorui.h>
#include <API/Model/StatusBar.h>
#include <API/Model/Jobs.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Psapi.h>   // GetProcessMemoryInfo (working set)
#pragma comment(lib, "Psapi.lib")
#endif

static int CountAtoms(bc::list<Atom*>& gos)
{
	int n = 0;
	for (Atom* atom : gos)
	{
		if (!atom) continue;
		++n;
		if (!atom->children.empty()) n += CountAtoms(atom->children);
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
		ImGui::Text("%d atoms", app->currentWorld ? CountAtoms(app->currentWorld->GetHierarchy()) : 0);
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

		// Plugin fields (nuke::StatusBar::Set), in first-set order. Jobs (progress
		// fields) are pulled out for the inline bar + the drop-up list.
		const std::vector<nuke::StatusBar::Entry> fields = nuke::StatusBar::All();
		std::vector<const nuke::StatusBar::Entry*> jobs;
		for (const auto& e : fields)
		{
			if (e.IsJob()) { jobs.push_back(&e); continue; }
			if (e.text.empty()) continue;
			sep();
			ImGui::TextUnformatted(e.text.c_str());
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.key.c_str());
		}

		// Indeterminate bars animate (ImGui: negative moving fraction), known ones show %.
		auto jobBar = [](const nuke::StatusBar::Entry& e, float width)
		{
			const bool indet = (e.progress == nuke::StatusBar::kIndeterminate);
			const float frac = indet ? -1.0f * (float)ImGui::GetTime() : e.progress;
			ImGui::ProgressBar(frac, ImVec2(width, ImGui::GetTextLineHeight()), indet ? "" : nullptr);
		};

		// The ACTIVE job inline (the first running one; queued ones wait in the list).
		if (!jobs.empty())
		{
			const nuke::StatusBar::Entry& e = *jobs.front();
			sep();
			ImGui::TextUnformatted(e.text.c_str());
			ImGui::SameLine();
			jobBar(e, 140.0f);
		}

		// Right edge: the jobs button -> a list POPPING UP above the bar.
		{
			char btn[48];
			if (jobs.empty()) snprintf(btn, sizeof(btn), ICON_LC_ACTIVITY);
			else              snprintf(btn, sizeof(btn), ICON_LC_ACTIVITY " %d", (int)jobs.size());
			const float btnW = ImGui::CalcTextSize(btn).x + ImGui::GetStyle().FramePadding.x * 2.0f;
			ImGui::SameLine();
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - btnW);
			const bool activeJobs = !jobs.empty();
			if (activeJobs) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.85f, 0.45f, 1.0f));
			bool open = ImGui::Button(btn);
			if (activeJobs) ImGui::PopStyleColor();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Background jobs");
			const ImVec2 btnMax(ImGui::GetItemRectMax().x, ImGui::GetItemRectMin().y);
			if (open) ImGui::OpenPopup("##nuke-jobs-popup");

			// Anchor the popup's BOTTOM-RIGHT to the button's top-right: it drops UP.
			ImGui::SetNextWindowPos(btnMax, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
			if (ImGui::BeginPopup("##nuke-jobs-popup"))
			{
				ImGui::TextDisabled(ICON_LC_ACTIVITY " Background jobs");
				ImGui::Separator();
				if (jobs.empty()) ImGui::TextDisabled("Idle");
				for (const nuke::StatusBar::Entry* e : jobs)
				{
					ImGui::TextUnformatted(e->text.c_str());
					jobBar(*e, 280.0f);
					ImGui::Spacing();
				}
				ImGui::Separator();
				ImGui::TextDisabled("Pool: %d/%d workers busy, %d queued",
				                    nuke::Jobs::Busy(), nuke::Jobs::WorkerCount(), nuke::Jobs::Pending());
				ImGui::EndPopup();
			}
		}
	}
	ImGui::End();
	ImGui::PopStyleVar();
}
