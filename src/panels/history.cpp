// Edit-history window: the whole undo/redo timeline of the command stack, current position
// marked; clicking an entry rolls the world back or forward to EXACTLY that point.
#include <editor/editorui.h>

void EditorUI::winHistory()
{
	if (!historyOpen) return;
	if (historyFocus) { ImGui::SetNextWindowFocus(); }
	NukeUI::DocPanel("panel:history", ICON_LC_HISTORY " Edit History", &historyOpen,
	                 window_flags, 380, 420, [this]()
	{
	const bool inPie = AppInstance::GetSingleton()->playState != 0;
	const int nu = (int)undoStack.size(), nr = (int)redoStack.size();

	ImGui::Text("%d / 200", nu + nr);
	if (undoTrimmed > 0) { ImGui::SameLine(); ImGui::TextDisabled("(+%d older, trimmed)", undoTrimmed); }
	if (inPie) { ImGui::SameLine(); ImGui::TextDisabled(ICON_LC_PLAY " PIE — history locked"); }
	ImGui::Separator();

	// Chronological rows: applied commands, then the undone ones (redoStack, newest at back).
	struct Row { const UndoCmd* c; bool undone; int redosToApply; };
	std::vector<Row> rows;
	rows.reserve((size_t)nu + nr);
	for (int i = 0; i < nu; ++i) rows.push_back({ &undoStack[i], false, 0 });
	for (int k = nr - 1; k >= 0; --k) rows.push_back({ &redoStack[k], true, nr - k });

	// The save marker sits on the LAST row of the serial recorded at the last save/load
	// (0 = the pristine world -> the baseline row).
	int saveRow = -2;
	if (savedWorldSerial == 0) saveRow = -1;
	else
		for (int r = 0; r < (int)rows.size(); ++r)
			if (rows[r].c->serial == savedWorldSerial &&
			    (r + 1 == (int)rows.size() || rows[r + 1].c->serial != savedWorldSerial))
				saveRow = r;

	// Follow the current position when it moves (edits/undo elsewhere), and on window focus.
	static int lastCur = -100;
	const int cur = nu - 1;   // -1 = baseline
	const bool follow = historyFocus || cur != lastCur;
	lastCur = cur;
	historyFocus = false;

	int jumpKeep = -1, jumpRedos = 0;   // applied after the list (the jump mutates the stacks)
	ImGui::BeginChild("##histlist");
	auto row = [&](int r, const char* icon, const char* label, bool undone)
	{
		const bool isCur = r == cur;
		if (undone) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		char id[64];
		snprintf(id, sizeof(id), "##hist%d", r);
		char text[320];
		snprintf(text, sizeof(text), "%s %s%s", icon, label, isCur ? "   " ICON_LC_CHEVRON_LEFT : "");
		const bool clicked = ImGui::Selectable((std::string(text) + id).c_str(), isCur);
		if (undone) ImGui::PopStyleColor();
		if (r == saveRow) { ImGui::SameLine(); ImGui::TextDisabled(ICON_LC_SAVE); }
		if (isCur && follow) ImGui::SetScrollHereY(0.5f);
		if (clicked && !isCur && !inPie)
		{
			if (r < 0) jumpKeep = 0;
			else if (!rows[r].undone) jumpKeep = r + 1;
			else jumpRedos = rows[r].redosToApply;
		}
	};
	row(-1, ICON_LC_FILE, "world state at history start", false);
	for (int r = 0; r < (int)rows.size(); ++r)
		row(r, rows[r].undone ? ICON_LC_UNDO_2 : ICON_LC_PENCIL, rows[r].c->label.c_str(), rows[r].undone);
	ImGui::EndChild();

	// The jump: raw stack ops (Undo()/Redo() carry focus guards that can route to an asset
	// editor's own history mid-click).
	if (!inPie)
	{
		while (jumpKeep >= 0 && (int)undoStack.size() > jumpKeep)
		{
			UndoCmd c = undoStack.back(); undoStack.pop_back();
			c.undo();
			redoStack.push_back(std::move(c));
			idleSnapValid = false;
		}
		while (jumpRedos-- > 0 && !redoStack.empty())
		{
			UndoCmd c = redoStack.back(); redoStack.pop_back();
			c.redo();
			undoStack.push_back(std::move(c));
			idleSnapValid = false;
		}
	}
	});
}
