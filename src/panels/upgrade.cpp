// Upgrade Project: the migration report for the project's content - what an engine change would
// rewrite (documents on older formats, stale binary assets, files needing a reimport, files
// NEWER than this engine) - the backup taken first, the button that applies it, and the restore
// of any backup. Opens by itself when the project was last saved by another engine release and
// something needs upgrading, or when files are newer than this editor; File > Upgrade Project...
#include <editor/editorui.h>
#include <API/Model/Migrations.h>
#include <interface/Modular.h>
#include <boost/filesystem.hpp>

namespace {
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
bool InputStd(const char* label, const char* hint, std::string& s)
{
	if (s.capacity() < 16) s.reserve(16);
	return ImGui::InputTextWithHint(label, hint, s.data(), s.capacity() + 1, ImGuiInputTextFlags_CallbackResize, ResizeCb, &s);
}
}

void EditorUI::winUpgrade()
{
	// The automatic check: once the boot scan is done, a project stamped by another release gets a
	// dry run; only a non-empty report opens the window.
	if (upgradeCheckPending && !bootLoading && !projectHubMode)
	{
		upgradeCheckPending = false;
		upgradeReport = nuke::Migrations::UpgradeProjectContent(AppInstance::GetSingleton()->contentRoot, false);
		upgradeHaveReport = true;
		if (upgradeReport.items.empty()) { projectEngineVersion = nuke::EngineVersion(); SaveProject(); }   // nothing to do: stamp and move on
		else { upgradeOpen = true; upgradeFocus = true; }
	}
	if (!upgradeOpen) return;
	if (upgradeFocus) { ImGui::SetNextWindowFocus(); }
	NukeUI::DocPanel("panel:upgrade", ICON_LC_ARROW_UP_CIRCLE " Upgrade Project", &upgradeOpen,
	                 window_flags, 860, 560, [this]()
	{
	const std::string& root = AppInstance::GetSingleton()->contentRoot;
	if (bootLoading) { ImGui::TextDisabled("Loading project..."); return; }
	if (!upgradeHaveReport)
	{
		upgradeReport = nuke::Migrations::UpgradeProjectContent(root, false);
		upgradeHaveReport = true;
		upgradeBackups = nuke::Migrations::FindBackups(projectDir);
		upgradeBackupPick = 0;
	}
	if (upgradeBackupDir.empty()) upgradeBackupDir = nuke::Migrations::DefaultBackupDir();
	const nuke::Migrations::Report& r = upgradeReport;
	const ImVec4 green(0.55f, 0.85f, 0.55f, 1.0f), amber(1.0f, 0.75f, 0.3f, 1.0f), red(1.0f, 0.5f, 0.4f, 1.0f);

	// ---- header ----
	ImGui::TextDisabled("Engine %s  -  project last saved by %s", nuke::EngineVersion(), projectEngineVersion.empty() ? "(unstamped)" : projectEngineVersion.c_str());
	ImGui::Spacing();
	if (r.applied)
	{
		ImGui::TextColored(green, ICON_LC_CIRCLE_CHECK " Upgraded: %d document(s), %d binary asset(s) re-saved", r.upgraded, r.resaved);
		if (r.failed) { ImGui::SameLine(); ImGui::TextColored(red, "  %d failed", r.failed); }
	}
	else if (r.items.empty())
		ImGui::TextColored(green, ICON_LC_CIRCLE_CHECK " Everything is current - nothing to upgrade");
	else
	{
		ImGui::Text("%d document(s) to upgrade, %d binary asset(s) to re-save", r.upgraded, r.resaved);
		if (r.reimport) { ImGui::SameLine(); ImGui::TextColored(amber, "  %d need a reimport", r.reimport); }
	}
	if (r.newer)
	{
		// Files a NEWER engine wrote: this editor cannot read them and will not guess. The way back is
		// the backup that editor took before it upgraded them - or updating this editor.
		ImGui::TextColored(red, ICON_LC_TRIANGLE_ALERT " %d file(s) were written by a newer engine than this one.", r.newer);
		ImGui::TextWrapped("They cannot be read or downgraded here. Restore the backup the newer editor made before upgrading (below), or open the project with that editor.");
	}
	if (!upgradeStatus.empty()) { ImGui::TextColored(upgradeStatusError ? red : green, "%s", upgradeStatus.c_str()); }
	ImGui::Spacing();

	// ---- backup, then upgrade ----
	ImGui::SeparatorText("Back up, then upgrade");
	ImGui::Checkbox("Back up before upgrading", &upgradeBackupOn);
	ImGui::SameLine(0, 20);
	ImGui::BeginDisabled(!upgradeBackupOn);
	int scope = upgradeBackupWhole ? 1 : 0;
	ImGui::RadioButton("only the files this upgrade rewrites", &scope, 0); ImGui::SameLine();
	ImGui::RadioButton("the whole content folder", &scope, 1);
	upgradeBackupWhole = scope == 1;
	ImGui::SetNextItemWidth(-(ImGui::CalcTextSize(ICON_LC_FOLDER_OPEN " Browse").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x));
	InputStd("##bkdir", "backup folder", upgradeBackupDir);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Default: beside the project folder, <project>-backup-<engine>-<date>");
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_FOLDER_OPEN " Browse"))
	{
		const std::string picked = EditorPickFolder("Pick the backup folder", boost::filesystem::path(upgradeBackupDir).parent_path().string());
		if (!picked.empty()) upgradeBackupDir = (boost::filesystem::path(picked) / boost::filesystem::path(upgradeBackupDir).filename()).string();
	}
	ImGui::EndDisabled();
	ImGui::Spacing();
	const bool canApply = !r.applied && (r.upgraded || r.resaved);
	ImGui::BeginDisabled(!canApply);
	if (ImGui::Button(ICON_LC_ARROW_UP_CIRCLE " Upgrade now", ImVec2(150, 0)))
	{
		std::string err;
		bool ok = true;
		if (upgradeBackupOn)
		{
			ok = nuke::Migrations::BackupProject(root, projectFile, upgradeBackupDir, upgradeBackupWhole, &upgradeReport, &err);
			if (!ok) { upgradeStatus = "backup failed - nothing upgraded: " + err; upgradeStatusError = true; }
		}
		if (ok)
		{
			upgradeReport = nuke::Migrations::UpgradeProjectContent(root, true);
			projectEngineVersion = nuke::EngineVersion();
			SaveProject();   // stamps the project: this release has been through it
			upgradeStatus = upgradeBackupOn ? "backed up to " + upgradeBackupDir + ", then upgraded" : "upgraded";
			upgradeStatusError = false;
			upgradeBackups = nuke::Migrations::FindBackups(projectDir);
			upgradeBackupDir.clear();   // the next run gets a fresh dated folder
		}
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Rewrites the listed files in place (documents to the current format, binaries re-saved).\nThe live ResDB picks the rewritten files up through the file index.");
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_REFRESH_CW " Re-scan", ImVec2(110, 0))) { upgradeReport = nuke::Migrations::UpgradeProjectContent(root, false); upgradeBackups = nuke::Migrations::FindBackups(projectDir); }
	ImGui::SameLine();
	if (ImGui::Button(ICON_LC_COPY " Copy report", ImVec2(130, 0))) ImGui::SetClipboardText(r.Text().c_str());
	if (!r.applied && r.items.empty())
	{
		ImGui::SameLine();
		if (ImGui::Button("Mark project current", ImVec2(170, 0))) { projectEngineVersion = nuke::EngineVersion(); SaveProject(); }
	}

	// ---- restore ----
	ImGui::Spacing();
	ImGui::SeparatorText("Restore a backup");
	{
		std::string label = "no backup beside the project";
		if (!upgradeBackups.empty())
		{
			upgradeBackupPick = std::max(0, std::min((int)upgradeBackups.size() - 1, upgradeBackupPick));
			const nuke::Migrations::BackupInfo& b = upgradeBackups[upgradeBackupPick];
			label = boost::filesystem::path(b.dir).filename().string() + "  (" + b.engine + ", " + std::to_string(b.files) + " file" + (b.files == 1 ? "" : "s") + (b.whole ? ", whole" : "") + ")";
		}
		ImGui::SetNextItemWidth(-(ImGui::CalcTextSize(ICON_LC_FOLDER_OPEN " Other...").x + ImGui::CalcTextSize(ICON_LC_UNDO_2 " Restore").x + ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x * 2));
		ImGui::BeginDisabled(upgradeBackups.empty());
		if (ImGui::BeginCombo("##bk", label.c_str()))
		{
			for (int i = 0; i < (int)upgradeBackups.size(); ++i)
			{
				const nuke::Migrations::BackupInfo& b = upgradeBackups[i];
				const std::string it = boost::filesystem::path(b.dir).filename().string() + "  (" + b.engine + ", " + std::to_string(b.files) + " files" + (b.whole ? ", whole" : "") + ")##" + std::to_string(i);
				if (ImGui::Selectable(it.c_str(), i == upgradeBackupPick)) upgradeBackupPick = i;
			}
			ImGui::EndCombo();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button(ICON_LC_FOLDER_OPEN " Other..."))
		{
			const std::string picked = EditorPickFolder("Pick a backup folder (holds nubackup.json)", boost::filesystem::path(projectDir).parent_path().string());
			nuke::Migrations::BackupInfo bi;
			if (!picked.empty())
			{
				if (nuke::Migrations::ReadBackupInfo(picked, bi)) { upgradeBackups.insert(upgradeBackups.begin(), bi); upgradeBackupPick = 0; }
				else { upgradeStatus = "not a backup folder (no nubackup.json): " + picked; upgradeStatusError = true; }
			}
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(upgradeBackups.empty());
		if (ImGui::Button(ICON_LC_UNDO_2 " Restore")) ImGui::OpenPopup("Restore backup##upgrade");
		ImGui::EndDisabled();
		if (ImGui::BeginPopupModal("Restore backup##upgrade", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			const nuke::Migrations::BackupInfo& b = upgradeBackups[upgradeBackupPick];
			ImGui::Text("Put %d file(s) from\n%s\nback into the project, overwriting the current ones?", b.files, b.dir.c_str());
			ImGui::TextDisabled("Made by engine %s on %s.", b.engine.c_str(), b.date.c_str());
			ImGui::Spacing();
			if (ImGui::Button("Restore", ImVec2(120, 0)))
			{
				std::string err;
				if (nuke::Migrations::RestoreProject(b.dir, root, projectFile, &err))
				{
					projectEngineVersion = b.engine;   // the project is that release's again
					LoadProject();                     // the restored .nuproj (re-checks the stamp on the next boot)
					upgradeReport = nuke::Migrations::UpgradeProjectContent(root, false);
					upgradeStatus = "restored from " + b.dir; upgradeStatusError = false;
				}
				else { upgradeStatus = "restore failed: " + err; upgradeStatusError = true; }
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}
	ImGui::Spacing();

	// ---- the list ----
	const ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg;
	if (ImGui::BeginTable("upg", 3, tf))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 90);
		ImGui::TableSetupColumn("File",   ImGuiTableColumnFlags_WidthStretch, 1.2f);
		ImGui::TableSetupColumn("What",   ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableHeadersRow();
		for (const auto& i : r.items)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImVec4 col(0.8f, 0.8f, 0.8f, 1.0f);
			if (i.action == "NEWER" || i.action == "FAILED") col = red;
			else if (i.action == "reimport") col = amber;
			else if (i.action == "upgraded" || i.action == "re-saved") col = green;
			ImGui::TextColored(col, "%s", i.action.c_str());
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(i.path.c_str());
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i.path.c_str());
			ImGui::TableSetColumnIndex(2);
			ImGui::TextDisabled("%s", i.note.c_str());
			if (!i.note.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i.note.c_str());
		}
		ImGui::EndTable();
	}
	});
	upgradeFocus = false;
}
