// Windows implementation of the editor's OS-integration seam (declared neutrally in
// editor/editorui.h). ALL Win32 lives here behind _WIN32 — no platform API leaks into shared code.
// Other platforms are served by platform_other.cpp (POSIX/stub).
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
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
