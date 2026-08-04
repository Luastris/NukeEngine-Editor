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
	if (!profilerOpen) return;
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
	ImGui::SetNextItemWidth(140);
	ImGui::InputTextWithHint("##pfilter", ICON_LC_SEARCH " filter", profilerFilter, sizeof(profilerFilter));
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
