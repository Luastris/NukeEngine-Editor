// Windows implementation of the editor's OS-integration seam (declared neutrally in
// editor/editorui.h). ALL Win32 lives here behind _WIN32 — no platform API leaks into shared code.
// Other platforms are served by platform_other.cpp (POSIX/stub).
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>   // process snapshot: reuse a RUNNING IDE instead of spawning one per file
#include <commdlg.h>
#include <shobjidl.h>   // IFileOpenDialog (folder picker)
#include <string>
#include <cstring>
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")

// Native "open file" dialog for asset import (models + images). Filter labels list the extensions so it
// is obvious what is supported. The browser dispatches by extension (AssImporter::ImportAny).
std::string EditorPickModelFile()
{
	char file[1024] = "";
	OPENFILENAMEA ofn = {};
	ofn.lStructSize = sizeof(ofn);
	ofn.lpstrFilter =
		"All supported (models + images)\0*.obj;*.fbx;*.dae;*.gltf;*.glb;*.3ds;*.ply;*.stl;*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.hdr;*.psd;*.gif\0"
		"Models (*.obj;*.fbx;*.dae;*.gltf;*.glb;*.3ds;*.ply;*.stl)\0*.obj;*.fbx;*.dae;*.gltf;*.glb;*.3ds;*.ply;*.stl\0"
		"Images (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.hdr;*.psd;*.gif)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.hdr;*.psd;*.gif\0"
		"All files (*.*)\0*.*\0";
	ofn.lpstrFile   = file;
	ofn.nMaxFile    = sizeof(file);
	ofn.lpstrTitle  = "Import asset";
	ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (GetOpenFileNameA(&ofn)) return std::string(file);
	return std::string();
}

// Native "open file" dialog for the game icon (Project Settings -> Packaging).
std::string EditorPickIconFile()
{
	char file[1024] = "";
	OPENFILENAMEA ofn = {};
	ofn.lStructSize = sizeof(ofn);
	ofn.lpstrFilter = "Icons (*.ico)\0*.ico\0All files (*.*)\0*.*\0";
	ofn.lpstrFile   = file;
	ofn.nMaxFile    = sizeof(file);
	ofn.lpstrTitle  = "Pick the game icon";
	ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (GetOpenFileNameA(&ofn)) return std::string(file);
	return std::string();
}

// Native "open file" dialog for projects: raw .nuproj, packed .nupak, mod .numod.
std::string EditorPickProjectFile()
{
	char file[1024] = "";
	OPENFILENAMEA ofn = {};
	ofn.lStructSize = sizeof(ofn);
	ofn.lpstrFilter =
		"NukeEngine projects (*.nuproj;*.nupak;*.numod)\0*.nuproj;*.nupak;*.numod\0"
		"Raw project (*.nuproj)\0*.nuproj\0"
		"Packed game (*.nupak)\0*.nupak\0"
		"Mod package (*.numod)\0*.numod\0"
		"All files (*.*)\0*.*\0";
	ofn.lpstrFile   = file;
	ofn.nMaxFile    = sizeof(file);
	ofn.lpstrTitle  = "Open project";
	ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (GetOpenFileNameA(&ofn)) return std::string(file);
	return std::string();
}

// External editor integration (Preferences). Detection scans the STANDARD install spots —
// no registry crawling: VS2022 editions, JetBrains Rider (per-user + Program Files, any
// version dir), VS Code (per-user + system), Notepad++. Each entry carries the argument
// template that makes the editor open file:line ({file}/{line} expand at launch).
#include <editor/exteditor.h>
#include <boost/filesystem.hpp>   // Boost over std, same as everywhere else in the project

std::vector<ExtEditor> EditorDetectExternalEditors()
{
	namespace fs = boost::filesystem;
	std::vector<ExtEditor> out;
	boost::system::error_code ec;
	auto add = [&](const char* name, const fs::path& exe, const char* args, const char* argsProj) {
		if (fs::exists(exe, ec)) out.push_back({ name, exe.string(), args, argsProj });
	};
	auto env = [](const char* v) { const char* e = getenv(v); return std::string(e ? e : ""); };
	const std::string pf    = env("ProgramFiles").empty()      ? "C:\\Program Files"       : env("ProgramFiles");
	const std::string pf86  = env("ProgramFiles(x86)").empty() ? "C:\\Program Files (x86)" : env("ProgramFiles(x86)");
	const std::string local = env("LOCALAPPDATA");

	// Visual Studio 2022 (any edition; first found wins).
	for (const char* ed : { "Community", "Professional", "Enterprise" })
	{
		fs::path devenv = fs::path(pf) / "Microsoft Visual Studio" / "2022" / ed / "Common7" / "IDE" / "devenv.exe";
		if (fs::exists(devenv, ec))
		{
			// Project context: devenv <csproj> <file> loads the project (IntelliSense) AND
			// opens the file; /Command jumps to the line once the IDE is up.
			add((std::string("Visual Studio 2022 ") + ed).c_str(), devenv,
			    "/Edit \"{file}\" /Command \"Edit.GoTo {line}\"",
			    "\"{project}\" \"{file}\" /Command \"Edit.GoTo {line}\"");
			break;
		}
	}
	// JetBrains Rider: the per-user Toolbox spot + any versioned dir under Program Files.
	if (!local.empty())
		add("Rider", fs::path(local) / "Programs" / "Rider" / "bin" / "rider64.exe",
		    "--line {line} \"{file}\"", "\"{project}\" --line {line} \"{file}\"");
	{
		fs::path jb = fs::path(pf) / "JetBrains";
		if (fs::exists(jb, ec))
			for (fs::directory_iterator it(jb, ec), end; it != end && !ec; it.increment(ec))
				if (it->path().filename().string().find("Rider") != std::string::npos)
				{
					add("Rider", it->path() / "bin" / "rider64.exe",
					    "--line {line} \"{file}\"", "\"{project}\" --line {line} \"{file}\"");
					break;
				}
	}
	// VS Code: per-user install first, then system-wide (-r = reuse the current window).
	if (!local.empty())
		add("VS Code", fs::path(local) / "Programs" / "Microsoft VS Code" / "Code.exe",
		    "-r -g \"{file}:{line}\"", "\"{projectDir}\" -g \"{file}:{line}\"");
	add("VS Code", fs::path(pf) / "Microsoft VS Code" / "Code.exe",
	    "-r -g \"{file}:{line}\"", "\"{projectDir}\" -g \"{file}:{line}\"");
	// Notepad++ (64- and 32-bit spots).
	add("Notepad++", fs::path(pf)   / "Notepad++" / "notepad++.exe", "-n{line} \"{file}\"", "");
	add("Notepad++", fs::path(pf86) / "Notepad++" / "notepad++.exe", "-n{line} \"{file}\"", "");

	// Same editor found twice (per-user + system): keep the first spot only.
	std::vector<ExtEditor> dedup;
	for (ExtEditor& e : out)
	{
		bool seen = false;
		for (ExtEditor& d : dedup) seen |= d.name == e.name;
		if (!seen) dedup.push_back(std::move(e));
	}
	return dedup;
}

bool EditorLaunchDetached(const std::string& exe, const std::string& args)
{
	std::string cmd = "\"" + exe + "\" " + args;
	STARTUPINFOA si = {}; si.cb = sizeof(si);
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessA(NULL, &cmd[0], NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL, &si, &pi)) return false;
	CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
	return true;
}

bool EditorProcessRunning(const std::string& exePath)
{
	std::string want = boost::filesystem::path(exePath).filename().string();
	for (char& c : want) c = (char)tolower((unsigned char)c);
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) return false;
	PROCESSENTRY32 pe = { sizeof(pe) };
	bool found = false;
	for (BOOL ok = Process32First(snap, &pe); ok && !found; ok = Process32Next(snap, &pe))
	{
		std::string name = pe.szExeFile;
		for (char& c : name) c = (char)tolower((unsigned char)c);
		found = name == want;
	}
	CloseHandle(snap);
	return found;
}

std::string EditorPickExeFile()
{
	char file[1024] = "";
	OPENFILENAMEA ofn = {};
	ofn.lStructSize = sizeof(ofn);
	ofn.lpstrFilter = "Programs (*.exe)\0*.exe\0All files (*.*)\0*.*\0";
	ofn.lpstrFile   = file;
	ofn.nMaxFile    = sizeof(file);
	ofn.lpstrTitle  = "Pick the editor executable";
	ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (GetOpenFileNameA(&ofn)) return std::string(file);
	return std::string();
}

// Launch a NEW editor instance on `projectPath` (New/Open Project switch projects by
// relaunch: the project lifecycle — PHASE_BOOT renderer, plugin set, ResDB — is bound to
// startup and cannot be swapped live). The caller closes THIS instance afterwards.
bool EditorRelaunch(const std::string& projectPath)
{
	char exe[MAX_PATH] = "";
	GetModuleFileNameA(NULL, exe, MAX_PATH);
	std::string cmd = std::string("\"") + exe + "\" \"" + projectPath + "\"";
	STARTUPINFOA si = {}; si.cb = sizeof(si);
	PROCESS_INFORMATION pi = {};
	std::string mutableCmd = cmd;
	if (!CreateProcessA(NULL, &mutableCmd[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return false;
	CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
	return true;
}

// Native "pick folder" dialog (build output path). Modern IFileOpenDialog with
// FOS_PICKFOLDERS; COM is initialized locally (S_FALSE = already up, fine either way).
std::string EditorPickFolder()
{
	std::string out;
	HRESULT ci = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	IFileOpenDialog* dlg = nullptr;
	if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
	{
		DWORD opts = 0;
		dlg->GetOptions(&opts);
		dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
		dlg->SetTitle(L"Pick the build output folder");
		if (SUCCEEDED(dlg->Show(NULL)))
		{
			IShellItem* item = nullptr;
			if (SUCCEEDED(dlg->GetResult(&item)))
			{
				PWSTR w = nullptr;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &w)) && w)
				{
					int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
					if (n > 1) { out.resize(n - 1); WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, NULL, NULL); }
					CoTaskMemFree(w);
				}
				item->Release();
			}
		}
		dlg->Release();
	}
	if (ci == S_OK) CoUninitialize();
	return out;
}

// Register .nuproj -> this editor under HKEY_CURRENT_USER (user scope, no admin, reversible).
bool RegisterProjectFileAssociation()
{
	char exe[MAX_PATH] = "";
	GetModuleFileNameA(NULL, exe, MAX_PATH);
	std::string cmd = std::string("\"") + exe + "\" \"%1\"";
	auto setKey = [](const char* sub, const char* val) -> bool {
		HKEY k;
		if (RegCreateKeyExA(HKEY_CURRENT_USER, sub, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return false;
		LONG r = RegSetValueExA(k, NULL, 0, REG_SZ, (const BYTE*)val, (DWORD)strlen(val) + 1);
		RegCloseKey(k);
		return r == ERROR_SUCCESS;
	};
	bool ok = setKey("Software\\Classes\\.nuproj", "NukeEngine.Project");
	ok = setKey("Software\\Classes\\NukeEngine.Project", "NukeEngine Project") && ok;
	ok = setKey("Software\\Classes\\NukeEngine.Project\\shell\\open\\command", cmd.c_str()) && ok;
	// Archives too (3.2): double-clicking a packed project / mod extracts its work tree
	// beside the pak and opens it (see EditorUI::PrepareArchiveProject).
	ok = setKey("Software\\Classes\\.nupak", "NukeEngine.Package") && ok;
	ok = setKey("Software\\Classes\\NukeEngine.Package", "NukeEngine Packed Project") && ok;
	ok = setKey("Software\\Classes\\NukeEngine.Package\\shell\\open\\command", cmd.c_str()) && ok;
	ok = setKey("Software\\Classes\\.numod", "NukeEngine.Mod") && ok;
	ok = setKey("Software\\Classes\\NukeEngine.Mod", "NukeEngine Mod Package") && ok;
	ok = setKey("Software\\Classes\\NukeEngine.Mod\\shell\\open\\command", cmd.c_str()) && ok;
	return ok;
}

#endif // _WIN32
