// Non-Windows implementation of the editor's OS-integration seam (declared in editor/editorui.h).
// Stubs for now — Linux would use a portable file dialog (GTK/zenity) and xdg-mime/.desktop for the
// project file association; macOS would use NSOpenPanel + LSSetDefaultRoleHandlerForContentType.
// The shared editor code calls these neutrally and never touches a platform API directly.
#ifndef _WIN32

#include <string>

std::string EditorPickModelFile()
{
	// TODO(linux/mac): native file picker (GTK file chooser / zenity / NSOpenPanel).
	return std::string();
}

bool RegisterProjectFileAssociation()
{
	// TODO(linux): write ~/.local/share/applications/*.desktop + xdg-mime default for .nuproj.
	// TODO(mac):   LSSetDefaultRoleHandlerForContentType for the .nuproj UTI.
	return false;
}

#endif // !_WIN32
