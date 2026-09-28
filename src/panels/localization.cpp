// Localization panel (7.8): the string tables under content/localization as one grid,
// keys x languages, edited live (the tables Loc serves are the ones on screen, so PIE text
// follows), saved back as nested JSON; a missing-key report per language; live language switch.
#include <editor/editorui.h>
#include <API/Model/Loc.h>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <set>
#include <algorithm>
#include <cctype>

namespace bfs = boost::filesystem;

namespace {
// InputText over a std::string (grows with the text).
int ResizeCb(ImGuiInputTextCallbackData* d)
{
	if (d->EventFlag == ImGuiInputTextFlags_CallbackResize)
	{
		std::string* s = (std::string*)d->UserData;
		s->resize((size_t)d->BufTextLen);
		d->Buf = s->data();
	}
	return 0;
}
bool InputStd(const char* label, const char* hint, std::string& s, ImGuiInputTextFlags flags = 0)
{
	if (s.capacity() < 16) s.reserve(16);
	return ImGui::InputTextWithHint(label, hint, s.data(), s.capacity() + 1, flags | ImGuiInputTextFlags_CallbackResize, ResizeCb, &s);
}

std::string TablePath(const std::string& lang)
{
	std::string p = nuke::Loc::SourcePath(lang);
	if (!p.empty()) return p;
	const std::string& root = AppInstance::GetSingleton()->contentRoot;
	return (bfs::path(root) / "localization" / (lang + ".json")).string();
}

std::string Trim(std::string s)
{
	while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
	while (!s.empty() && isspace((unsigned char)s.front())) s.erase(s.begin());
	return s;
}
}

void EditorUI::winLocalization()
{
	locOwnsSave = false;
	if (!locOpen) return;
	if (locFocus) { ImGui::SetNextWindowFocus(); }
	NukeUI::DocPanel("panel:localization", ICON_LC_LANGUAGES " Localization", &locOpen,
	                 window_flags, 960, 560, [this]()
	{
	static std::string newLang, newKey, filter, status, scrollTo;
	static bool missingOnly = false, focusKeyInput = false;
	static std::set<std::string> dirty;     // languages edited since the last save
	static int seenVersion = -1;
	static std::vector<std::string> langs, keys;
	static std::map<std::string, std::vector<std::string>> missing;
	if (nuke::Loc::Version() != seenVersion)
	{
		seenVersion = nuke::Loc::Version();
		langs = nuke::Loc::Languages();
		keys  = nuke::Loc::Keys();
		missing.clear();
		for (const std::string& l : langs) missing[l] = nuke::Loc::Missing(l);
	}
	const std::string cur = nuke::Loc::Language();
	const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	locOwnsSave = focused;
	const ImGuiStyle& st = ImGui::GetStyle();

	auto save = [&]()
	{
		int saved = 0;
		for (const std::string& l : dirty)
		{
			const std::string path = TablePath(l);
			boost::system::error_code ec;
			bfs::create_directories(bfs::path(path).parent_path(), ec);
			bfs::ofstream f(bfs::path(path), std::ios::binary);
			if (!f) { status = "cannot write " + path; continue; }
			f << nuke::Loc::ToJson(l) << "\n";
			++saved;
		}
		status = std::to_string(saved) + (saved == 1 ? " table saved" : " tables saved");
		dirty.clear();
	};
	auto addLanguage = [&]()
	{
		const std::string code = Trim(newLang);
		if (code.empty()) return;
		std::string tmp;
		if (std::find(langs.begin(), langs.end(), code) == langs.end())
		{
			nuke::Loc::LoadTable(code, "{\"_name\": \"" + code + "\"}", TablePath(code));
			dirty.insert(code);
			status = "language '" + code + "' added - fill it in and Save";
		}
		newLang.clear();
	};
	auto addKey = [&]()
	{
		const std::string key = Trim(newKey);
		if (key.empty() || langs.empty()) return;
		std::string tmp;
		if (!nuke::Loc::Lookup(cur, key, tmp)) { nuke::Loc::Set(cur, key, ""); dirty.insert(cur); }
		scrollTo = key;
		newKey.clear();
		focusKeyInput = true;   // Enter after Enter: keep typing keys
	};

	// ---- header: three groups, each one thing - TABLES | LANGUAGE | KEYS -------------------
	// Every group: a caption, a "current" row, an "add" row; the inputs fill the cell and the
	// action buttons share one width, so the rows line up across the groups.
	const bool ctrlS = focused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false);
	const float btnW = ImGui::CalcTextSize(ICON_LC_PLUS " Add").x + st.FramePadding.x * 2;
	auto caption = [&](const char* text) { ImGui::TextDisabled("%s", text); ImGui::Spacing(); };
	auto addButton = [&](const char* id) { ImGui::SameLine(); return ImGui::Button((std::string(ICON_LC_PLUS " Add##") + id).c_str(), ImVec2(btnW, 0)); };
	if (ImGui::BeginTable("lochead", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
	{
		ImGui::TableSetupColumn("tables",   ImGuiTableColumnFlags_WidthFixed, 190);
		ImGui::TableSetupColumn("language", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("keys",     ImGuiTableColumnFlags_WidthStretch, 1.6f);
		ImGui::TableNextRow();

		// TABLES: save + state
		ImGui::TableSetColumnIndex(0);
		caption("TABLES");
		ImGui::BeginDisabled(dirty.empty());
		const bool clicked = ImGui::Button(ICON_LC_SAVE " Save", ImVec2(-FLT_MIN, 0));
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Ctrl+S\nWrites the edited languages to content/localization/<code>.json");
		if ((clicked || ctrlS) && !dirty.empty()) save();
		ImGui::AlignTextToFramePadding();
		if (!dirty.empty()) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), ICON_LC_PENCIL " %d unsaved", (int)dirty.size());
		else                ImGui::TextDisabled("%s", status.empty() ? "up to date" : status.c_str());

		// LANGUAGE: the current one, then add
		ImGui::TableSetColumnIndex(1);
		caption("LANGUAGE");
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::BeginCombo("##lang", langs.empty() ? "-" : (cur + "  " + (nuke::Loc::LanguageName(cur) != cur ? nuke::Loc::LanguageName(cur) : "")).c_str()))
		{
			for (const std::string& l : langs)
				if (ImGui::Selectable((l + "  " + nuke::Loc::LanguageName(l) + "##" + l).c_str(), l == cur)) nuke::Loc::SetLanguage(l);
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("The language the engine serves right now (config window.language).\nSwitches live and fires \"loc.changed\".");
		ImGui::SetNextItemWidth(-(btnW + st.ItemSpacing.x));
		if (InputStd("##newlang", "new language code", newLang, ImGuiInputTextFlags_EnterReturnsTrue)) addLanguage();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enter - the file name: en, ru, ja, zh-CN ...");
		if (addButton("lang")) addLanguage();

		// KEYS: find, then add
		ImGui::TableSetColumnIndex(2);
		caption("KEYS");
		const float missW = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x + ImGui::CalcTextSize("Missing only").x;
		ImGui::SetNextItemWidth(-(missW + st.ItemSpacing.x));
		InputStd("##filter", ICON_LC_SEARCH " filter", filter);
		ImGui::SameLine();
		ImGui::Checkbox("Missing only", &missingOnly);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Only keys some language still lacks");
		ImGui::SetNextItemWidth(-(btnW + st.ItemSpacing.x));
		if (focusKeyInput) { ImGui::SetKeyboardFocusHere(); focusKeyInput = false; }
		ImGui::BeginDisabled(langs.empty());
		if (InputStd("##newkey", "new key, e.g. menu.start", newKey, ImGuiInputTextFlags_EnterReturnsTrue)) addKey();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Enter - goes into the current language, empty (= missing) until you fill it.\nDots nest into namespaces in the file: menu.start -> {\"menu\": {\"start\": ...}}");
		if (addButton("key")) addKey();
		ImGui::EndDisabled();
		ImGui::EndTable();
	}
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	if (langs.empty())
	{
		ImGui::Dummy(ImVec2(0, ImGui::GetContentRegionAvail().y * 0.3f));
		const char* l1 = "No string tables yet";
		const char* l2 = "Type a language code above (en, ru, ...) and press Enter. The table is saved as";
		const char* l3 = "content/localization/<code>.json";
		for (const char* l : { l1, l2, l3 })
		{
			ImGui::SetCursorPosX((ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(l).x) * 0.5f);
			if (l == l1) ImGui::Text("%s", l); else ImGui::TextDisabled("%s", l);
		}
		return;
	}

	// ---- missing-key report -------------------------------------------------------------------
	int totalMissing = 0;
	for (auto& kv : missing) totalMissing += (int)kv.second.size();
	{
		const std::string title = totalMissing ? std::string(ICON_LC_TRIANGLE_ALERT " ") + std::to_string(totalMissing) + " missing translation" + (totalMissing == 1 ? "" : "s") + "###missing"
		                                       : std::string(ICON_LC_CIRCLE_CHECK " Every key translated in every language###missing");
		if (totalMissing) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
		const bool open = ImGui::CollapsingHeader(title.c_str());
		if (totalMissing) ImGui::PopStyleColor();
		if (open)
		{
			ImGui::Indent();
			for (const std::string& l : langs)
			{
				const std::vector<std::string>& m = missing[l];
				const std::string name = l + "  " + nuke::Loc::LanguageName(l);
				if (m.empty()) { ImGui::TextDisabled("%s: complete", name.c_str()); continue; }
				if (ImGui::TreeNode((name + ": " + std::to_string(m.size()) + " missing##m" + l).c_str()))
				{
					for (const std::string& k : m)
						if (ImGui::Selectable(k.c_str())) { filter.clear(); scrollTo = k; }
					ImGui::TreePop();
				}
			}
			ImGui::Unindent();
		}
	}

	// ---- the grid -------------------------------------------------------------------------------
	std::vector<int> rows;   // indices into keys after the filter
	rows.reserve(keys.size());
	int scrollRow = -1;
	for (int i = 0; i < (int)keys.size(); ++i)
	{
		const std::string& k = keys[i];
		if (!filter.empty() && k.find(filter) == std::string::npos) continue;
		if (missingOnly)
		{
			bool any = false; std::string tmp;
			for (const std::string& l : langs) if (!nuke::Loc::Lookup(l, k, tmp) || tmp.empty()) { any = true; break; }
			if (!any) continue;
		}
		if (k == scrollTo) scrollRow = (int)rows.size();
		rows.push_back(i);
	}
	ImGui::TextDisabled("%d key%s, %d language%s%s", (int)keys.size(), keys.size() == 1 ? "" : "s", (int)langs.size(), langs.size() == 1 ? "" : "s",
	                    (int)rows.size() != (int)keys.size() ? (" - " + std::to_string(rows.size()) + " shown").c_str() : "");
	const ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg;
	if (ImGui::BeginTable("loc", 1 + (int)langs.size(), tf, ImVec2(0, 0)))
	{
		ImGui::TableSetupScrollFreeze(1, 1);
		ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 240);
		for (const std::string& l : langs)
		{
			std::string head = l + "  " + nuke::Loc::LanguageName(l);
			if (!missing[l].empty()) head += "  (" + std::to_string(missing[l].size()) + " missing)";
			if (dirty.count(l)) head += " *";
			ImGui::TableSetupColumn((head + "###" + l).c_str(), ImGuiTableColumnFlags_WidthStretch);
		}
		ImGui::TableHeadersRow();
		if (rows.empty())
		{
			ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
			ImGui::TextDisabled(keys.empty() ? "No keys yet - type one above and press Enter" : "Nothing matches the filter");
		}
		ImGuiListClipper clip;
		clip.Begin((int)rows.size());
		if (scrollRow >= 0) clip.IncludeItemByIndex(scrollRow);
		std::string removeKey;
		while (clip.Step())
			for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r)
			{
				const std::string& k = keys[rows[r]];
				ImGui::PushID(k.c_str());
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::AlignTextToFramePadding();
				ImGui::Selectable(k.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
				if (r == scrollRow) { ImGui::SetScrollHereY(0.3f); scrollTo.clear(); }
				if (ImGui::BeginPopupContextItem("keyctx"))
				{
					ImGui::TextDisabled("%s", k.c_str());
					ImGui::Separator();
					if (ImGui::MenuItem(ICON_LC_COPY " Copy key")) ImGui::SetClipboardText(k.c_str());
					if (ImGui::MenuItem(ICON_LC_TRASH_2 " Remove from every language")) removeKey = k;
					ImGui::EndPopup();
				}
				for (int c = 0; c < (int)langs.size(); ++c)
				{
					ImGui::TableSetColumnIndex(1 + c);
					const std::string& l = langs[c];
					std::string v;
					const bool have = nuke::Loc::Lookup(l, k, v) && !v.empty();
					if (!have) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, IM_COL32(150, 60, 40, 70));
					ImGui::SetNextItemWidth(-FLT_MIN);
					ImGui::PushID(c);
					if (InputStd("##v", have ? "" : "missing", v)) { nuke::Loc::Set(l, k, v); dirty.insert(l); }
					ImGui::PopID();
				}
				ImGui::PopID();
			}
		if (!removeKey.empty())
			for (const std::string& l : langs) { std::string tmp; if (nuke::Loc::Lookup(l, removeKey, tmp)) { nuke::Loc::Erase(l, removeKey); dirty.insert(l); } }
		ImGui::EndTable();
	}
	});
	locFocus = false;
}
