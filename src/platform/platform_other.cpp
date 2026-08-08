// Linux implementation of the editor's OS-integration seam (declared in editor/editorui.h).
// Windows lives in platform_win32.cpp, macOS in platform_macos.mm. Dialogs go through the
// desktop's own tool (kdialog on KDE, zenity elsewhere) as a subprocess — no GTK/Qt link
// dependency, works under X11 and Wayland alike; processes go through /proc; the .nuproj
// association through the XDG mime + .desktop machinery.
#if !defined(_WIN32) && !defined(__APPLE__)

#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <spawn.h>
#include <unistd.h>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <interface/Importers.h>   // plugin importer extensions -> dialog filter
#include <editor/exteditor.h>

extern char** environ;
namespace bfs = boost::filesystem;

// ---- dialogs ------------------------------------------------------------------------------

// First hit on PATH (plus the flatpak export dirs), or "" when absent.
static std::string ToolPath(const char* name)
{
	std::string path = getenv("PATH") ? getenv("PATH") : "";
	path += ":/var/lib/flatpak/exports/bin";
	if (const char* home = getenv("HOME"))
		path += std::string(":") + home + "/.local/share/flatpak/exports/bin";
	size_t b = 0;
	boost::system::error_code ec;
	while (b <= path.size())
	{
		const size_t e = path.find(':', b);
		const std::string dir = path.substr(b, e == std::string::npos ? e : e - b);
		if (!dir.empty())
		{
			const bfs::path cand = bfs::path(dir) / name;
			if (bfs::exists(cand, ec)) return cand.string();
		}
		if (e == std::string::npos) break;
		b = e + 1;
	}
	return std::string();
}

// Run a dialog command line, return its first stdout line ("" = cancelled / tool missing).
static std::string RunDialog(const std::string& cmd)
{
	FILE* p = popen(cmd.c_str(), "r");
	if (!p) return std::string();
	char buf[4096] = {};
	const bool got = fgets(buf, sizeof(buf), p) != nullptr;
	const int rc = pclose(p);
	if (!got || rc != 0) return std::string();
	std::string out(buf);
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
	return out;
}

// One modal open dialog. `exts` empty = any file. Prefers the desktop's own tool so the
// dialog matches the environment (KDE -> kdialog, GNOME & the rest -> zenity), and falls
// back to whichever of the two is installed.
static std::string RunOpenDialog(const char* title, const std::vector<std::string>& exts,
                                 bool files, bool dirs, const char* filterLabel = "Files")
{
	const char* desk = getenv("XDG_CURRENT_DESKTOP");
	const bool kde = desk && strstr(desk, "KDE");
	const std::string kdialog = ToolPath("kdialog"), zenity = ToolPath("zenity");

	std::string globs;                     // "*.obj *.fbx"
	for (const std::string& e : exts)
		globs += std::string(globs.empty() ? "" : " ") + "*." + (e.size() && e[0] == '.' ? e.substr(1) : e);

	auto viaKdialog = [&]() -> std::string
	{
		std::string cmd = "kdialog --title \"" + std::string(title) + "\" ";
		if (dirs && !files) cmd += "--getexistingdirectory \"$HOME\"";
		else
		{
			cmd += "--getopenfilename \"$HOME\"";
			if (!globs.empty()) cmd += " \"" + globs + "|" + filterLabel + "\"";
		}
		return RunDialog(cmd + " 2>/dev/null");
	};
	auto viaZenity = [&]() -> std::string
	{
		std::string cmd = "zenity --file-selection --title=\"" + std::string(title) + "\"";
		if (dirs && !files) cmd += " --directory";
		else if (!globs.empty())
			cmd += " --file-filter=\"" + std::string(filterLabel) + " | " + globs + "\""
			     + " --file-filter=\"All files | *\"";
		return RunDialog(cmd + " 2>/dev/null");
	};

	if (kde  && !kdialog.empty()) return viaKdialog();
	if (!zenity.empty())          return viaZenity();
	if (!kdialog.empty())         return viaKdialog();
	printf("[dialog]\t\tno zenity/kdialog on this machine — install one for native file pickers\n");
	return std::string();
}

// Native "open file" dialog for asset import (models + images + plugin importer formats).
std::string EditorPickModelFile()
{
	std::vector<std::string> exts = {
		"obj", "fbx", "dae", "gltf", "glb", "3ds", "ply", "stl",          // models
		"png", "jpg", "jpeg", "tga", "bmp", "hdr", "psd", "gif",          // images
	};
	for (const nuke::AssetImporter& imp : nuke::AssetImporters())
		for (const std::string& e : imp.exts)
			exts.push_back(e.size() && e[0] == '.' ? e.substr(1) : e);
	return RunOpenDialog("Import asset", exts, true, false, "Assets");
}

// Native "open file" dialog for the game icon (Project Settings -> Packaging).
std::string EditorPickIconFile()
{
	return RunOpenDialog("Pick the game icon", { "ico", "png", "icns" }, true, false, "Icons");
}

// Native "pick folder" dialog (build output path).
std::string EditorPickFolder()
{
	return RunOpenDialog("Pick the build output folder", {}, false, true);
}

// Native "open file" dialog for projects: raw .nuproj, packed .nupak, mod .numod.
std::string EditorPickProjectFile()
{
	return RunOpenDialog("Open project", { "nuproj", "nupak", "numod" }, true, false, "NukeEngine projects");
}

// Program picker (custom external editor) — executables carry no extension here.
std::string EditorPickExeFile()
{
	return RunOpenDialog("Pick the editor application", {}, true, false);
}

// ---- processes ----------------------------------------------------------------------------

// Fire-and-forget: /bin/sh expands the argument template exactly like the Windows command
// line did; POSIX_SPAWN_SETSID detaches the child from the editor's session.
bool EditorLaunchDetached(const std::string& exe, const std::string& args)
{
	const std::string cmd = "\"" + exe + "\" " + args;
	const char* argv[] = { "/bin/sh", "-c", cmd.c_str(), nullptr };
	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
	pid_t pid = 0;
	const int rc = posix_spawn(&pid, "/bin/sh", nullptr, &attr, (char* const*)argv, environ);
	posix_spawnattr_destroy(&attr);
	return rc == 0;
}

// Is a process of this executable already running? Decides reuse vs first-launch args.
// /proc/<pid>/exe readlinks to the real binary — same fidelity as proc_pidpath on mac.
bool EditorProcessRunning(const std::string& exePath)
{
	std::string want = bfs::path(exePath).filename().string();
	for (char& c : want) c = (char)tolower((unsigned char)c);
	boost::system::error_code ec;
	for (bfs::directory_iterator it("/proc", ec), end; it != end && !ec; it.increment(ec))
	{
		const std::string pidStr = it->path().filename().string();
		if (pidStr.empty() || pidStr.find_first_not_of("0123456789") != std::string::npos) continue;
		if (atoi(pidStr.c_str()) == (int)getpid()) continue;
		char buf[4096];
		const ssize_t n = readlink((it->path() / "exe").c_str(), buf, sizeof(buf) - 1);
		if (n <= 0) continue;
		buf[n] = 0;
		std::string name = bfs::path(buf).filename().string();
		// A replaced-on-disk binary readlinks as "name (deleted)".
		const size_t del = name.find(" (deleted)");
		if (del != std::string::npos) name.resize(del);
		for (char& c : name) c = (char)tolower((unsigned char)c);
		if (name == want) return true;
	}
	return false;
}

// Launch a new editor instance on `projectPath` (project lifecycle is bound to startup, so
// switching projects means relaunching); the caller closes this instance.
bool EditorRelaunch(const std::string& projectPath)
{
	char exe[4096];
	const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n <= 0) return false;
	exe[n] = 0;
	const char* argv[] = { exe, projectPath.c_str(), nullptr };
	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
	pid_t pid = 0;
	const int rc = posix_spawn(&pid, exe, nullptr, &attr, (char* const*)argv, environ);
	posix_spawnattr_destroy(&attr);
	return rc == 0;
}

// ---- documents & association --------------------------------------------------------------

// Documents arrive via argv here — the Apple Event plumbing is macOS-only.
void        EditorInstallOpenDocHandler() {}
std::string EditorTakeOpenDocRequest()    { return std::string(); }

// Register the NukeEngine document types and this binary as their handler, user scope:
// a mime-info XML (the types), a .desktop entry (the handler), xdg-mime (the default).
// Everything lands under ~/.local/share — reversible by deleting the two files.
bool RegisterProjectFileAssociation()
{
	const char* home = getenv("HOME");
	if (!home) return false;
	char exe[4096];
	const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n <= 0) return false;
	exe[n] = 0;

	boost::system::error_code ec;
	const bfs::path share = bfs::path(home) / ".local" / "share";
	const bfs::path mimeDir = share / "mime" / "packages";
	const bfs::path appsDir = share / "applications";
	bfs::create_directories(mimeDir, ec);
	bfs::create_directories(appsDir, ec);
	if (ec) return false;

	{
		bfs::ofstream x(mimeDir / "nukeengine.xml");
		if (!x) return false;
		x << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		     "<mime-info xmlns=\"http://www.freedesktop.org/standards/shared-mime-info\">\n"
		     "  <mime-type type=\"application/x-nukeengine-project\">\n"
		     "    <comment>NukeEngine Project</comment>\n"
		     "    <glob pattern=\"*.nuproj\"/>\n"
		     "  </mime-type>\n"
		     "  <mime-type type=\"application/x-nukeengine-package\">\n"
		     "    <comment>NukeEngine Package</comment>\n"
		     "    <glob pattern=\"*.nupak\"/>\n"
		     "  </mime-type>\n"
		     "  <mime-type type=\"application/x-nukeengine-mod\">\n"
		     "    <comment>NukeEngine Mod</comment>\n"
		     "    <glob pattern=\"*.numod\"/>\n"
		     "  </mime-type>\n"
		     "</mime-info>\n";
	}

	// Ship the editor icon into hicolor when a copy is reachable (installed run dir or the
	// dev tree); the .desktop references it by name so a later install also picks it up.
	const bfs::path exeDir = bfs::path(exe).parent_path();
	std::string iconLine;
	for (const bfs::path& cand : { exeDir / "res" / "logo.png",
	                               exeDir.parent_path() / "res" / "logo.png",
	                               exeDir / ".." / ".." / "NukeEngine-Editor" / "res" / "logo.png" })
		if (bfs::exists(cand, ec))
		{
			const bfs::path iconDir = share / "icons" / "hicolor" / "256x256" / "apps";
			bfs::create_directories(iconDir, ec);
			bfs::copy_file(cand, iconDir / "nukeengine-editor.png", bfs::copy_options::overwrite_existing, ec);
			if (!ec) iconLine = "Icon=nukeengine-editor\n";
			break;
		}

	{
		bfs::ofstream d(appsDir / "nukeengine-editor.desktop");
		if (!d) return false;
		d << "[Desktop Entry]\n"
		     "Type=Application\n"
		     "Name=NukeEngine Editor\n"
		     "Exec=\"" << exe << "\" %f\n"
		  << iconLine
		  << "Terminal=false\n"
		     "Categories=Development;IDE;\n"
		     "MimeType=application/x-nukeengine-project;application/x-nukeengine-package;application/x-nukeengine-mod;\n";
	}

	// Refresh the databases + claim the default. Best-effort: a machine without the tools
	// still has the files in place for the next desktop re-index.
	std::system(("update-mime-database \"" + (share / "mime").string() + "\" >/dev/null 2>&1").c_str());
	std::system(("update-desktop-database \"" + appsDir.string() + "\" >/dev/null 2>&1").c_str());
	for (const char* m : { "application/x-nukeengine-project", "application/x-nukeengine-package",
	                       "application/x-nukeengine-mod" })
		std::system((std::string("xdg-mime default nukeengine-editor.desktop ") + m + " >/dev/null 2>&1").c_str());
	return true;
}

// ---- external editors ---------------------------------------------------------------------

// Scan PATH + the flatpak export dirs for the usual suspects, each with its file:line
// argument template. De-duplicated by name (first spot wins).
std::vector<ExtEditor> EditorDetectExternalEditors()
{
	std::vector<ExtEditor> out;
	auto add = [&](const char* name, const std::string& exe, const char* args, const char* argsProj)
	{
		if (exe.empty()) return;
		for (ExtEditor& d : out) if (d.name == name) return;
		out.push_back({ name, exe, args, argsProj });
	};
	// VS Code: native package, OSS build, or the flatpak export.
	for (const char* c : { "code", "codium", "com.visualstudio.code", "com.vscodium.codium" })
		add("VS Code", ToolPath(c), "-r -g \"{file}:{line}\"", "\"{projectDir}\" -g \"{file}:{line}\"");
	// Rider (direct install / Toolbox shim on PATH; flatpak export).
	for (const char* c : { "rider", "com.jetbrains.Rider" })
		add("Rider", ToolPath(c), "--line {line} \"{file}\"", "\"{project}\" --line {line} \"{file}\"");
	// Sublime Text: subl CLI takes file:line directly.
	for (const char* c : { "subl", "com.sublimetext.three" })
		add("Sublime Text", ToolPath(c), "\"{file}:{line}\"", "");
	// Kate: KDE's editor — the desktop default on this port's first-class distros.
	for (const char* c : { "kate", "org.kde.kate" })
		add("Kate", ToolPath(c), "--line {line} \"{file}\"", "");
	return out;
}

#endif // !_WIN32 && !__APPLE__
