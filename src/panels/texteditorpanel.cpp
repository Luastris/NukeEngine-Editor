// Text editor panel (roadmap 2.2) — EditorUI method definitions (translation unit).
// Tabs of open text files on the vendored ImGuiColorTextEdit; the syntax language comes
// from the file-type descriptor (0.6, AssetCreatorForExt), with a fallback by extension.
// Saving writes the file to disk — shaders/materials then hot-reload through the editor's
// existing mtime watcher (ResDB::HotReloadShaders/Assets), no extra plumbing needed.
#include <editor/editorui.h>
#include "interface/AssetCreators.h"
#include "nukeui.h"   // DocWindow: detachable document windows (task #136)
#include "../textedit/TextEditor.h"
#include <boost/filesystem/fstream.hpp>

// Syntax language for a file: the registered descriptor's syntaxLanguage first, then a
// built-in extension fallback (shaders/json/etc. are editor-created, not plugin-registered).
static TextEditor::LanguageDefinitionId LangForFile(const std::string& ext)
{
	std::string lang;
	if (const nuke::AssetCreator* ac = nuke::AssetCreatorForExt(ext))
		lang = ac->syntaxLanguage;
	if (lang.empty())
	{
		if      (ext == ".lua")                                   lang = "lua";
		else if (ext == ".hlsl" || ext == ".fx")                  lang = "hlsl";
		else if (ext == ".glsl" || ext == ".vert" || ext == ".frag") lang = "glsl";
		else if (ext == ".json" || ext == ".nuproj" || ext == ".numat" || ext == ".nuworld" || ext == ".nuprefab") lang = "json";
		else if (ext == ".c" || ext == ".h" || ext == ".cpp" || ext == ".hpp" || ext == ".cs"
		         || ext == ".cc" || ext == ".inl" || ext == ".inc") lang = "cpp";
	}
	for (char& c : lang) c = (char)std::tolower((unsigned char)c);
	if (lang == "lua")  return TextEditor::LanguageDefinitionId::Lua;
	if (lang == "hlsl") return TextEditor::LanguageDefinitionId::Hlsl;
	if (lang == "glsl") return TextEditor::LanguageDefinitionId::Glsl;
	if (lang == "json") return TextEditor::LanguageDefinitionId::Json;
	if (lang == "cpp" || lang == "c++" || lang == "c") return TextEditor::LanguageDefinitionId::Cpp;
	if (lang == "cs")   return TextEditor::LanguageDefinitionId::Cs;
	if (lang == "python") return TextEditor::LanguageDefinitionId::Python;
	return TextEditor::LanguageDefinitionId::None;
}

// A file the text editor may open: the descriptor says textEditable, or it's a known
// text extension (shader sources and configs are editor-created — no descriptor).
bool EditorUI::IsTextFile(const std::string& ext)
{
	if (const nuke::AssetCreator* ac = nuke::AssetCreatorForExt(ext))
		if (ac->textEditable) return true;
	static const char* kText[] = { ".lua", ".hlsl", ".fx", ".glsl", ".vert", ".frag",
	                               ".json", ".txt", ".md", ".ini", ".cfg", ".nuproj",
	                               ".nubonemap",   // retarget map = plain JSON
	                               // C++ sources (the browser's source root, 6.0)
	                               ".cpp", ".h", ".hpp", ".c", ".cc", ".inl", ".inc", ".cmake" };
	for (const char* t : kText) if (ext == t) return true;
	return false;
}

void EditorUI::OpenTextFile(const std::string& path, int line)
{
	// Already open: focus its tab (and jump if a line was asked for).
	for (TextDoc& d : textDocs)
		if (d.path == path)
		{
			if (line > 0) d.ed->SetCursorPosition(line - 1, 0);
			d.wantFocus = true; textEditorOpen = true; return;
		}

	bfs::ifstream in(bfs::path(path), std::ios::binary);
	if (!in) { cout << "[TextEditor]\tcannot open '" << path << "'" << endl; return; }
	std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

	TextDoc d;
	d.path = path;
	d.ed = std::make_shared<TextEditor>();
	d.ed->SetText(text);
	std::string ext = bfs::path(path).extension().string();
	for (char& c : ext) c = (char)std::tolower((unsigned char)c);
	d.ed->SetLanguageDefinition(LangForFile(ext));
	if (line > 0) d.ed->SetCursorPosition(line - 1, 0);
	d.savedUndoIndex = d.ed->GetUndoIndex();
	d.wantFocus = true;
	textDocs.push_back(std::move(d));
	textEditorOpen = true;
}

void EditorUI::SaveTextDoc(TextDoc& d)
{
	bfs::ofstream out(bfs::path(d.path), std::ios::binary | std::ios::trunc);
	if (!out) { cout << "[TextEditor]\tcannot write '" << d.path << "'" << endl; return; }
	std::string text = d.ed->GetText();
	out.write(text.data(), (std::streamsize)text.size());
	out.close();
	d.savedUndoIndex = d.ed->GetUndoIndex();
	// Shaders/materials/textures reload via the existing mtime watcher (Draw()'s
	// HotReloadShaders/HotReloadAssets); Lua re-runs on the next Play.
}

void EditorUI::winTextEditor()
{
	textFocused = -1;   // recomputed below; EditorUI::Undo/Redo route by it
	// EACH open file is its OWN window (floating by default; dockable anywhere like any
	// panel). The window id is keyed by the file path, so re-opening a file focuses it.
	for (int i = 0; i < (int)textDocs.size(); ++i)
	{
		TextDoc& d = textDocs[i];
		const bool dirty = d.ed->GetUndoIndex() != d.savedUndoIndex;
		const std::string docId = "txt:" + d.path;
		if (d.wantFocus) { NukeUI::DocFocus(docId.c_str()); d.wantFocus = false; }
		std::string title = std::string(ICON_LC_FILE_PEN " ") + bfs::path(d.path).filename().string()
		                  + "###" + docId;   // stable identity: the dirty flag changes, the id doesn't
		ImGuiWindowFlags wf = window_flags | (dirty ? ImGuiWindowFlags_UnsavedDocument : 0);
		// DETACHABLE document window (NukeUI): docked = a normal imgui window, detached =
		// its own OS window; tear-off/dock-back by dragging like every asset editor. The
		// content lambda looks the doc up BY PATH — it may run in the host pass, and the
		// vector can shift under it.
		const std::string keyPath = d.path;
		NukeUI::DocWindow(docId.c_str(), title.c_str(), &d.open, wf, 720, 520, [this, keyPath]()
		{
			for (int k = 0; k < (int)textDocs.size(); ++k)
			{
				TextDoc& dd = textDocs[k];
				if (dd.path != keyPath) continue;
				if (ImGui::SmallButton(ICON_LC_SAVE " Save")) SaveTextDoc(dd);
				ImGui::SameLine(); ImGui::TextDisabled("%s", dd.path.c_str());
				ImGui::SameLine();
				int ln, col; dd.ed->GetCursorPosition(ln, col);
				ImGui::TextDisabled("  %d:%d  %s", ln + 1, col + 1, dd.ed->GetLanguageDefinitionName());
				const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
				if (focused) textFocused = k;   // the widget owns Ctrl+Z; scene undo must stay away
				dd.ed->Render("##text", focused, ImGui::GetContentRegionAvail());
				// Ctrl+S saves this document while its window is focused.
				if (focused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
					SaveTextDoc(dd);
				return;
			}
		});
		if (!d.open && dirty)
		{
			d.open = true;               // keep the window until the user answers
			textCloseConfirm = i;
		}
	}

	// Discard-changes modal for a closing dirty document (one at a time, global).
	if (textCloseConfirm >= 0) ImGui::OpenPopup("Unsaved changes##textedit");
	if (ImGui::BeginPopupModal("Unsaved changes##textedit", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		if (textCloseConfirm >= 0 && textCloseConfirm < (int)textDocs.size())
		{
			TextDoc& d = textDocs[textCloseConfirm];
			ImGui::Text("'%s' has unsaved changes.", bfs::path(d.path).filename().string().c_str());
			ImGui::Spacing();
			if (ImGui::Button("Save & Close")) { SaveTextDoc(d); d.open = false; textCloseConfirm = -1; ImGui::CloseCurrentPopup(); }
			ImGui::SameLine();
			if (ImGui::Button("Discard"))      { d.savedUndoIndex = d.ed->GetUndoIndex(); d.open = false; textCloseConfirm = -1; ImGui::CloseCurrentPopup(); }
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))       { textCloseConfirm = -1; ImGui::CloseCurrentPopup(); }
		}
		else { textCloseConfirm = -1; ImGui::CloseCurrentPopup(); }
		ImGui::EndPopup();
	}

	// Drop closed documents.
	for (int i = (int)textDocs.size() - 1; i >= 0; --i)
		if (!textDocs[i].open)
			textDocs.erase(textDocs.begin() + i);
}
