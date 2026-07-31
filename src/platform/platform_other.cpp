// Non-Windows implementation of the editor's OS-integration seam (declared in editor/editorui.h).
// Stubs for now.
#ifndef _WIN32

#include <string>

std::string EditorPickModelFile()
{
	// TODO(linux/mac): native file picker (GTK file chooser / zenity / NSOpenPanel).
	return std::string();
}

std::string EditorPickIconFile()
{
	// TODO(linux/mac): native file picker (see EditorPickModelFile).
	return std::string();
}

std::string EditorPickFolder()
{
	// TODO(linux/mac): native folder picker (see EditorPickModelFile).
	return std::string();
}

std::string EditorPickProjectFile()
{
	// TODO(linux/mac): native file picker (see EditorPickModelFile).
	return std::string();
}

bool EditorRelaunch(const std::string&)
{
	// TODO(linux/mac): fork/exec a new editor instance on the picked project.
	return false;
}

bool RegisterProjectFileAssociation()
{
	// TODO(linux): write ~/.local/share/applications/*.desktop + xdg-mime default for .nuproj.
	// TODO(mac):   LSSetDefaultRoleHandlerForContentType for the .nuproj UTI.
	return false;
}

std::string EditorPickExeFile()
{
	// TODO(linux/mac): native file picker (see EditorPickModelFile).
	return std::string();
}

#include <editor/exteditor.h>
std::vector<ExtEditor> EditorDetectExternalEditors()
{
	// TODO(linux/mac): scan PATH + standard spots (code, rider, subl, ...).
	return {};
}

bool EditorLaunchDetached(const std::string&, const std::string&)
{
	// TODO(linux/mac): posix_spawn / fork+exec.
	return false;
}

bool EditorProcessRunning(const std::string&)
{
	// TODO(linux/mac): scan /proc (or pgrep). No detection = always "first launch" args.
	return false;
}

#endif // !_WIN32
