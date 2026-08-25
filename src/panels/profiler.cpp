// Profiler window: every phase the engine, the renderer and the modules report, as a live
// breakdown. CPU phases come from Profiler::Report (World's "update"/"fixed"/"render" plus the
// "rnd.*" sub-phases and "rnd.hook.<Type>" per component); "gpu.*" are the renderer's duration
// queries. Opened from Window > Profiler or by clicking the timings in the status bar.
#include <editor/editorui.h>
#include <API/Model/Profiler.h>
#include <algorithm>
#include <vector>

void EditorUI::winProfiler()
{
	if (!profilerOpen)
	{
		if (meshCostView)   // the cost overlay lives with the profiler: window closed = view off
		{
			meshCostView = false;
			if (nuke::AppInstance::GetSingleton()->render) nuke::AppInstance::GetSingleton()->render->setDebugView(0);
		}
		return;
	}
	if (profilerFocus) { ImGui::SetNextWindowFocus(); profilerFocus = false; }
	// Through DocPanel like every other panel: caption buttons, docking and tear-off come from
	// there — a bare ImGui::Begin window has none of it.
	NukeUI::DocPanel("panel:profiler", ICON_LC_ACTIVITY " Profiler", &profilerOpen,
	                 window_flags, 460, 420, [this]()
	{

	const double frameMs = nuke::Time::getSingleton()->delta * 1000.0;
	ImGui::Text("frame %.2f ms", frameMs);
	ImGui::SameLine(); ImGui::TextDisabled("(%.0f fps)", frameMs > 0.0 ? 1000.0 / frameMs : 0.0);
	ImGui::SameLine();
	ImGui::Checkbox("Freeze", &profilerFrozen);
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_DOWNLOAD " Capture CSV"))
		nuke::Profiler::Capture("profile.csv");   // logged with the absolute path
	ImGui::SameLine();
	{
		bool on = meshCostView;
		if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		if (ImGui::Button(ICON_LC_BOXES " Mesh cost"))
		{
			meshCostView = !meshCostView;
			if (nuke::AppInstance::GetSingleton()->render)
				nuke::AppInstance::GetSingleton()->render->setDebugView(meshCostView ? 1 : 0);
		}
		if (on) ImGui::PopStyleColor();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Color every mesh in the viewport by its triangle load\n"
			                  "(triangles of the drawn LOD/section x instances), flat fill + wireframe.");
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(140);
	ImGui::InputTextWithHint("##pfilter", ICON_LC_SEARCH " filter", profilerFilter, sizeof(profilerFilter));
	if (meshCostView)
	{
		// Legend: the renderer's log10 ramp (CostColor) — same stops, same colors.
		static const ImU32 kStops[5] = {
			IM_COL32( 26, 191,  38, 255), IM_COL32(230, 217,  13, 255), IM_COL32(255, 115,   5, 255),
			IM_COL32(255,  13,   5, 255), IM_COL32(255,   0, 230, 255) };
		static const char* kLabels[5] = { "1k", "10k", "100k", "1M", "10M" };
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetTextLineHeight();
		const ImVec2 p0 = ImGui::GetCursorScreenPos();
		for (int s = 0; s < 4; ++s)
			dl->AddRectFilledMultiColor(ImVec2(p0.x + w * s / 4.0f, p0.y), ImVec2(p0.x + w * (s + 1) / 4.0f, p0.y + h),
			                            kStops[s], kStops[s + 1], kStops[s + 1], kStops[s]);
		ImGui::Dummy(ImVec2(w, h));
		const ImVec2 lp = ImGui::GetCursorScreenPos();
		const ImU32 tcol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
		for (int s = 0; s < 5; ++s)
		{
			const float ts = ImGui::CalcTextSize(kLabels[s]).x;
			const float lx = w * s / 4.0f - (s == 0 ? 0.0f : (s == 4 ? ts : ts * 0.5f));
			dl->AddText(ImVec2(p0.x + lx, lp.y), tcol, kLabels[s]);
		}
		ImGui::Dummy(ImVec2(w, h));
		ImGui::TextDisabled("triangles drawn per mesh section x instances");
	}
	ImGui::Separator();

	// Snapshot: a frozen view keeps the numbers still while they are read.
	struct Row { std::string name; double ms; };
	static std::vector<Row> frozen;
	std::vector<Row> rows;
	if (!profilerFrozen)
	{
		std::string phases = nuke::Profiler::Phases();
		size_t st = 0;
		while (st < phases.size())
		{
			size_t nl = phases.find('\n', st);
			if (nl == std::string::npos) nl = phases.size();
			const std::string ph = phases.substr(st, nl - st);
			st = nl + 1;
			if (!ph.empty()) rows.push_back({ ph, nuke::Profiler::Ms(ph) });
		}
		frozen = rows;
	}
	else rows = frozen;

	auto lc = [](std::string v) { for (char& c : v) c = (char)tolower((unsigned char)c); return v; };
	const std::string needle = lc(profilerFilter);
	std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ms > b.ms; });

	// The heaviest phase sets the bar scale, so the shape of the frame is readable at a glance.
	double maxMs = 0.0;
	for (const Row& r : rows) maxMs = std::max(maxMs, r.ms);
	if (maxMs <= 0.0) maxMs = 1.0;

	auto section = [&](const char* title, const char* prefix, bool invert)
	{
		bool any = false;
		for (const Row& r : rows)
		{
			const bool match = prefix ? (r.name.rfind(prefix, 0) == 0) : true;
			if (invert ? match : !match) continue;
			if (!needle.empty() && lc(r.name).find(needle) == std::string::npos) continue;
			if (!any) { ImGui::SeparatorText(title); any = true; }
			ImGui::Text("%-28s %7.2f ms", r.name.c_str(), r.ms);
			ImGui::SameLine();
			const float frac = (float)(r.ms / maxMs);
			ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight()), "");
		}
		return any;
	};

	ImGui::BeginChild("##plist");
	section("GPU passes", "gpu.", true);
	section("Render sub-phases", "rnd.", true);
	// Everything else: the engine's own phases plus whatever modules report.
	{
		bool any = false;
		for (const Row& r : rows)
		{
			if (r.name.rfind("gpu.", 0) == 0 || r.name.rfind("rnd.", 0) == 0) continue;
			if (!needle.empty() && lc(r.name).find(needle) == std::string::npos) continue;
			if (!any) { ImGui::SeparatorText("Frame"); any = true; }
			ImGui::Text("%-28s %7.2f ms", r.name.c_str(), r.ms);
			ImGui::SameLine();
			ImGui::ProgressBar((float)(r.ms / maxMs), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight()), "");
		}
	}
	if (rows.empty()) ImGui::TextDisabled("No phases reported yet.");
	ImGui::EndChild();
	});
}
