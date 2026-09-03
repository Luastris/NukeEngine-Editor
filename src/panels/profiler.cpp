// Profiler window: every phase the engine, the renderer and the modules report, as a live
// breakdown. CPU phases come from Profiler::Report (World's "update"/"fixed"/"render" plus the
// "rnd.*" sub-phases and "rnd.hook.<Type>" per component); "gpu.*" are the renderer's duration
// queries. Opened from Window > Profiler or by clicking the timings in the status bar.
#include <editor/editorui.h>
#include <API/Model/Profiler.h>
#include <algorithm>
#include <vector>

// ---- frame-time history: five curves, ring of the last kHist frames ------------------------
static const int kHist = 240;
struct PerfCurve { const char* name; ImU32 col; float v[kHist]; };
static PerfCurve s_curves[5] = {
	{ "frame",  IM_COL32(235, 235, 235, 255), {} },
	{ "update", IM_COL32( 80, 200, 255, 255), {} },
	{ "fixed",  IM_COL32(255, 200,  70, 255), {} },
	{ "render", IM_COL32(170, 120, 255, 255), {} },
	{ "gpu",    IM_COL32(110, 230, 130, 255), {} },
};
static int  s_histHead  = 0;
static int  s_histCount = 0;

// Once per editor frame: sample the engine's phase timings into the ring.
void EditorUI::ProfilerHistoryTick()
{
	float gpu = 0.0f;
	{
		// Sum the renderer's duration queries ("gpu.*").
		std::string phases = nuke::Profiler::Phases();
		size_t st = 0;
		while (st < phases.size())
		{
			size_t nl = phases.find('\n', st);
			if (nl == std::string::npos) nl = phases.size();
			const std::string ph = phases.substr(st, nl - st);
			st = nl + 1;
			if (ph.rfind("gpu.", 0) == 0) gpu += (float)nuke::Profiler::Ms(ph);
		}
	}
	s_curves[0].v[s_histHead] = (float)(nuke::Time::getSingleton()->delta * 1000.0);
	s_curves[1].v[s_histHead] = (float)nuke::Profiler::Ms("update");
	s_curves[2].v[s_histHead] = (float)nuke::Profiler::Ms("fixed");
	s_curves[3].v[s_histHead] = (float)nuke::Profiler::Ms("render");
	s_curves[4].v[s_histHead] = gpu;
	s_histHead = (s_histHead + 1) % kHist;
	if (s_histCount < kHist) ++s_histCount;
}

// The curves + a legend with current values, drawn into the current window's draw list.
void EditorUI::DrawPerfGraph(float x, float y, float w, float h)
{
	if (s_histCount < 2) return;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(0, 0, 0, 150), 4.0f);
	// Scale from the visible peak, never below a 60 Hz frame so idle stays readable.
	float peak = 16.7f;
	for (const PerfCurve& c : s_curves)
		for (int i = 0; i < s_histCount; ++i) peak = std::max(peak, c.v[i]);
	// 16.6 / 33.3 ms guides
	for (float guide : { 16.6f, 33.3f })
		if (guide < peak)
		{
			const float gy = y + h - h * (guide / peak);
			dl->AddLine(ImVec2(x, gy), ImVec2(x + w, gy), IM_COL32(255, 255, 255, 40));
		}
	for (const PerfCurve& c : s_curves)
	{
		ImVec2 prev;
		for (int i = 0; i < s_histCount; ++i)
		{
			const int idx = (s_histHead - s_histCount + i + 2 * kHist) % kHist;
			const ImVec2 p(x + w * (float)i / (float)(kHist - 1),
			               y + h - h * std::min(1.0f, c.v[idx] / peak));
			if (i > 0) dl->AddLine(prev, p, c.col, 1.0f);
			prev = p;
		}
	}
	// Legend: name + the freshest value.
	float lx = x + 6, ly = y + 4;
	for (const PerfCurve& c : s_curves)
	{
		const int last = (s_histHead - 1 + kHist) % kHist;
		char buf[48];
		snprintf(buf, sizeof(buf), "%s %.1f", c.name, c.v[last]);
		dl->AddText(ImVec2(lx, ly), c.col, buf);
		lx += ImGui::CalcTextSize(buf).x + 12;
	}
}

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
			meshCostView = !meshCostView; if (meshCostView) aoView = false;   // one debug view at a time
			if (nuke::AppInstance::GetSingleton()->render)
				nuke::AppInstance::GetSingleton()->render->setDebugView(meshCostView ? 1 : 0);
		}
		if (on) ImGui::PopStyleColor();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Color every mesh in the viewport by its triangle load\n"
			                  "(triangles of the drawn LOD/section x instances), flat fill + wireframe.");
	}
	ImGui::SameLine();
	{
		bool on = perfOverlay;
		if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		if (ImGui::Button(ICON_LC_CHART_LINE " Overlay")) perfOverlay = !perfOverlay;
		if (on) ImGui::PopStyleColor();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Draw the frame-time curves over the viewport");
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(140);
	ImGui::InputTextWithHint("##pfilter", ICON_LC_SEARCH " filter", profilerFilter, sizeof(profilerFilter));

	// History graph: the five engine curves over the last kHist frames.
	{
		const float gw = ImGui::GetContentRegionAvail().x;
		const float gh = 84.0f;
		const ImVec2 gp = ImGui::GetCursorScreenPos();
		DrawPerfGraph(gp.x, gp.y, gw, gh);
		ImGui::Dummy(ImVec2(gw, gh + 4));
	}
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

	// One row: name, ms, bar against the heaviest phase.
	auto row = [&](const Row& r)
	{
		ImGui::Text("%-28s %7.2f ms", r.name.c_str(), r.ms);
		ImGui::SameLine();
		ImGui::ProgressBar((float)(r.ms / maxMs), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight()), "");
	};
	// A section holds ONLY the rows matching its prefix (rows are ms-sorted, zeros sink).
	auto section = [&](const char* title, const char* prefix)
	{
		bool any = false;
		for (const Row& r : rows)
		{
			if (r.name.rfind(prefix, 0) != 0) continue;
			if (!needle.empty() && lc(r.name).find(needle) == std::string::npos) continue;
			if (!any) { ImGui::SeparatorText(title); any = true; }
			row(r);
		}
		return any;
	};

	ImGui::BeginChild("##plist");
	// CPU first: the engine's own phases (update/fixed/render) plus whatever modules report.
	{
		bool any = false;
		for (const Row& r : rows)
		{
			if (r.name.rfind("gpu.", 0) == 0 || r.name.rfind("rnd.", 0) == 0) continue;
			if (!needle.empty() && lc(r.name).find(needle) == std::string::npos) continue;
			if (!any) { ImGui::SeparatorText("CPU phases"); any = true; }
			row(r);
		}
	}
	section("GPU passes", "gpu.");
	section("Render sub-phases (CPU)", "rnd.");
	if (rows.empty()) ImGui::TextDisabled("No phases reported yet.");
	ImGui::EndChild();
	});
}
