// Windows implementation of the editor's OS-integration seam (declared neutrally in
// editor/editorui.h). ALL Win32 lives here behind _WIN32 — no platform API leaks into shared code.
// Other platforms are served by platform_other.cpp (POSIX/stub).
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <string>
#include <cstring>
#pragma comment(lib, "comdlg32.lib")

// Native "open file" dialog for model import.
std::string EditorPickModelFile()
{
	char file[1024] = "";
	OPENFILENAMEA ofn = {};
	ofn.lStructSize = sizeof(ofn);
	ofn.lpstrFilter = "Models\0*.obj;*.fbx;*.dae;*.gltf;*.glb;*.3ds;*.ply;*.stl\0All files\0*.*\0";
	ofn.lpstrFile   = file;
	ofn.nMaxFile    = sizeof(file);
	ofn.lpstrTitle  = "Import model";
	ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (GetOpenFileNameA(&ofn)) return std::string(file);
	return std::string();
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
	return ok;
}

#endif // _WIN32
