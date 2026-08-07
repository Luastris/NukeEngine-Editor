// Windows implementation of the editor's OS-integration seam (declared in editor/editorui.h):
// file/folder dialogs, external editor detection/launch, relaunch, .nuproj file association.
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>   // process snapshot
#include <commdlg.h>
#include <shobjidl.h>   // IFileOpenDialog (folder picker)
#include <string>
#include <cstring>
#include <interface/Importers.h>   // plugin importer extensions -> dialog filter
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")

// Native "open file" dialog for asset import (models + images). Returns "" if cancelled.
std::string EditorPickModelFile()
{
	char file[1024] = "";
	OPENFILENAMEA ofn = {};
	ofn.lStructSize = sizeof(ofn);

	// Built at runtime to include plugin importers. OPENFILENAME wants a double-null-terminated
	// "label\0pattern\0..." block, hence the embedded NULs.
	const char* kModels = "*.obj;*.fbx;*.dae;*.gltf;*.glb;*.3ds;*.ply;*.stl";
	const char* kImages = "*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.hdr;*.psd;*.gif";
	std::string pluginPats;
	for (const nuke::AssetImporter& imp : nuke::AssetImporters())
		for (const std::string& e : imp.exts) { pluginPats += "*"; pluginPats += e; pluginPats += ";"; }
	if (!pluginPats.empty()) pluginPats.pop_back();   // trailing ';'

	std::string allPats = std::string(kModels) + ";" + kImages;
	if (!pluginPats.empty()) allPats += ";" + pluginPats;

	std::string filt;
	auto add = [&](const std::string& label, const std::string& pat) { filt += label; filt.push_back('\0'); filt += pat; filt.push_back('\0'); };
	add("All supported", allPats);
	add(std::string("Models (") + kModels + ")", kModels);
	add(std::string("Images (") + kImages + ")", kImages);
	if (!pluginPats.empty()) add(std::string("Plugin formats (") + pluginPats + ")", pluginPats);
	add("All files (*.*)", "*.*");
	filt.push_back('\0');   // block terminator

	ofn.lpstrFilter = filt.c_str();
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

#include <editor/exteditor.h>
#include <boost/filesystem.hpp>

// Scan the standard install spots (VS2022, Rider, VS Code, Notepad++) for external editors,
// each with its file:line argument template. De-duplicated by name.
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
			add((std::string("Visual Studio 2022 ") + ed).c_str(), devenv,
			    "/Edit \"{file}\" /Command \"Edit.GoTo {line}\"",
			    "\"{project}\" \"{file}\" /Command \"Edit.GoTo {line}\"");
			break;
		}
	}
	// Rider: per-user Toolbox spot + any versioned dir under Program Files.
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

// Launch a new editor instance on `projectPath` (project lifecycle is bound to startup, so
// switching projects means relaunching); the caller closes this instance. False if spawn failed.
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

// Native "pick folder" dialog (build output path). Returns "" if cancelled.
std::string EditorPickFolder()
{
	std::string out;
	HRESULT ci = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);   // S_FALSE = already up; only uninit on S_OK
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
	// Archives too: opening a .nupak/.numod extracts its work tree (EditorUI::PrepareArchiveProject).
	ok = setKey("Software\\Classes\\.nupak", "NukeEngine.Package") && ok;
	ok = setKey("Software\\Classes\\NukeEngine.Package", "NukeEngine Packed Project") && ok;
	ok = setKey("Software\\Classes\\NukeEngine.Package\\shell\\open\\command", cmd.c_str()) && ok;
	ok = setKey("Software\\Classes\\.numod", "NukeEngine.Mod") && ok;
	ok = setKey("Software\\Classes\\NukeEngine.Mod", "NukeEngine Mod Package") && ok;
	ok = setKey("Software\\Classes\\NukeEngine.Mod\\shell\\open\\command", cmd.c_str()) && ok;
	return ok;
}

// Documents arrive via argv on Windows — the Apple Event plumbing is macOS-only.
void        EditorInstallOpenDocHandler() {}
std::string EditorTakeOpenDocRequest()    { return std::string(); }

#endif // _WIN32
