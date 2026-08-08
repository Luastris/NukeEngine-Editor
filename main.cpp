#include <API/Model/Atom.h>
#include <interface/Services.h>
#include <nukeui.h>
#include "imgui.h"
#include <editor/editorui.h>
#include <input/DesktopInput.h>
#include <interface/AssetCreators.h>
#include <interface/AtomCreators.h>
#include <interface/ComponentIcons.h>
#include <input/keyboard.h>
#include <config.h>
#include <interface/Modular.h>
#ifdef EDITOR
#include <interface/AppInstance.h>
#include <import/assimporter.h>
#include <API/Model/Jobs.h>
#else
#include <interface/AppInstance.h>
#endif

#include <iostream>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>   // early Preferences read
#include <sstream>
#include <boost/dll/runtime_symbol_info.hpp>   // program_location() -> exe dir
#include <boost/thread.hpp>
#include <boost/chrono.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <crtdbg.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace bfs = boost::filesystem;
using namespace std;
using namespace nuke;

#ifdef _WIN32
// Unhandled-SEH filter: print the faulting address + a symbolized stack (raw C stdio, so it
// works while cout/cerr are rerouted), then fall through to the system dialog.
static LONG WINAPI NukeCrashTrace(EXCEPTION_POINTERS* ep)
{
	static LONG once = 0;
	if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;   // one report

	EXCEPTION_RECORD* er = ep->ExceptionRecord;
	fprintf(stderr, "\n[CRASH] exception 0x%08lX at %p (thread %lu)\n",
	        (unsigned long)er->ExceptionCode, er->ExceptionAddress, GetCurrentThreadId());
	if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
		fprintf(stderr, "[CRASH] %s address %p\n",
		        er->ExceptionInformation[0] ? "WRITING" : "READING",
		        (void*)er->ExceptionInformation[1]);

	HANDLE proc = GetCurrentProcess();
	SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
	SymInitialize(proc, nullptr, TRUE);

	CONTEXT ctx = *ep->ContextRecord;
	// RIP outside every module (call through a bad pointer) kills the walker on frame 0;
	// resume from the return address on top of the stack so the caller is named.
	if (ctx.Rip < 0x10000 && ctx.Rsp && !IsBadReadPtr((void*)ctx.Rsp, 8))
	{
		fprintf(stderr, "[CRASH] RIP invalid (call through a bad pointer) — resuming from the return address\n");
		ctx.Rip = *(DWORD64*)ctx.Rsp;
		ctx.Rsp += 8;
	}
	STACKFRAME64 sf = {};
	sf.AddrPC.Offset    = ctx.Rip; sf.AddrPC.Mode    = AddrModeFlat;
	sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
	sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
	int printed = 0;
	for (int i = 0; i < 64; ++i)
	{
		if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf, &ctx,
		                 nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
			break;
		DWORD64 pc = sf.AddrPC.Offset;
		if (!pc) break;

		char mod[MAX_PATH] = "?";
		HMODULE hm = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &hm) && hm)
		{
			GetModuleFileNameA(hm, mod, MAX_PATH);
			const char* slash = strrchr(mod, '\\');
			if (slash) memmove(mod, slash + 1, strlen(slash + 1) + 1);
		}

		char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
		SYMBOL_INFO* si = (SYMBOL_INFO*)symBuf;
		si->SizeOfStruct = sizeof(SYMBOL_INFO);
		si->MaxNameLen = 255;
		DWORD64 disp64 = 0;
		DWORD dispLine = 0;
		IMAGEHLP_LINE64 line = { sizeof(IMAGEHLP_LINE64) };
		if (SymFromAddr(proc, pc, &disp64, si))
		{
			if (SymGetLineFromAddr64(proc, pc, &dispLine, &line))
				fprintf(stderr, "[CRASH] #%02d %s!%s+0x%llx  (%s:%lu)\n",
				        i, mod, si->Name, (unsigned long long)disp64, line.FileName, (unsigned long)line.LineNumber);
			else
				fprintf(stderr, "[CRASH] #%02d %s!%s+0x%llx\n",
				        i, mod, si->Name, (unsigned long long)disp64);
		}
		else
			fprintf(stderr, "[CRASH] #%02d %s+0x%llx\n", i, mod, (unsigned long long)pc);
		++printed;
	}
	// Backstop when the walk yields nothing: raw-scan the stack for code addresses.
	if (printed < 2 && ep->ContextRecord->Rsp)
	{
		fprintf(stderr, "[CRASH] stack walk failed — raw scan:\n");
		DWORD64* sp = (DWORD64*)ep->ContextRecord->Rsp;
		for (int w = 0; w < 512 && !IsBadReadPtr(sp + w, 8); ++w)
		{
			DWORD64 pc = sp[w];
			HMODULE hm = nullptr;
			if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &hm) || !hm)
				continue;
			char mod[MAX_PATH] = "?";
			GetModuleFileNameA(hm, mod, MAX_PATH);
			const char* slash = strrchr(mod, '\\');
			if (slash) memmove(mod, slash + 1, strlen(slash + 1) + 1);
			char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
			SYMBOL_INFO* si = (SYMBOL_INFO*)symBuf;
			si->SizeOfStruct = sizeof(SYMBOL_INFO);
			si->MaxNameLen = 255;
			DWORD64 disp64 = 0;
			if (SymFromAddr(proc, pc, &disp64, si))
				fprintf(stderr, "[CRASH] rsp+0x%03x %s!%s+0x%llx\n", w * 8, mod, si->Name, (unsigned long long)disp64);
			else
				fprintf(stderr, "[CRASH] rsp+0x%03x %s+0x%llx\n", w * 8, mod, (unsigned long long)pc);
		}
	}
	fflush(stderr);
	fflush(stdout);
	return EXCEPTION_CONTINUE_SEARCH;   // let the system dialog / debugger take over
}

// CRT assert/error report hook: CRT asserts abort without raising SEH, so print a symbolized
// stack here too. Returns FALSE to continue to the normal assert dialog.
static int __cdecl NukeCrtReportHook(int reportType, char* message, int* /*returnValue*/)
{
	if (reportType != _CRT_ASSERT && reportType != _CRT_ERROR) return FALSE;
	fprintf(stderr, "\n[CRASH] CRT %s: %s\n",
	        reportType == _CRT_ASSERT ? "assert" : "error", message ? message : "");
	HANDLE proc = GetCurrentProcess();
	SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
	SymInitialize(proc, nullptr, TRUE);
	void* frames[64];
	int n = (int)CaptureStackBackTrace(0, 64, frames, nullptr);
	for (int i = 0; i < n; ++i)
	{
		DWORD64 pc = (DWORD64)frames[i];
		char mod[MAX_PATH] = "?";
		HMODULE hm = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &hm) && hm)
		{
			GetModuleFileNameA(hm, mod, MAX_PATH);
			const char* slash = strrchr(mod, '\\');
			if (slash) memmove(mod, slash + 1, strlen(slash + 1) + 1);
		}
		char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
		SYMBOL_INFO* si = (SYMBOL_INFO*)symBuf;
		si->SizeOfStruct = sizeof(SYMBOL_INFO);
		si->MaxNameLen = 255;
		DWORD64 disp64 = 0;
		DWORD dispLine = 0;
		IMAGEHLP_LINE64 line = { sizeof(IMAGEHLP_LINE64) };
		if (SymFromAddr(proc, pc, &disp64, si))
		{
			if (SymGetLineFromAddr64(proc, pc, &dispLine, &line))
				fprintf(stderr, "[CRASH] #%02d %s!%s+0x%llx  (%s:%lu)\n",
				        i, mod, si->Name, (unsigned long long)disp64, line.FileName, (unsigned long)line.LineNumber);
			else
				fprintf(stderr, "[CRASH] #%02d %s!%s+0x%llx\n",
				        i, mod, si->Name, (unsigned long long)disp64);
		}
		else
			fprintf(stderr, "[CRASH] #%02d %s+0x%llx\n", i, mod, (unsigned long long)pc);
	}
	fflush(stderr);
	fflush(stdout);
	return FALSE;   // continue to the normal assert dialog
}
#endif  // _WIN32

#ifndef _WIN32
#include <execinfo.h>
#include <csignal>
#include <unistd.h>
// POSIX crash trace (the SEH filter's counterpart): raw symbolized stack straight to fd 2
// (backtrace_symbols_fd — no malloc in a signal handler), then the default action so the OS
// crash reporter still fires.
static void NukeCrashSignal(int sig)
{
	static volatile sig_atomic_t once = 0;
	if (once++) _exit(128 + sig);
	fprintf(stderr, "\n[CRASH] signal %d (%s)\n", sig, strsignal(sig));
	void* frames[64];
	const int n = backtrace(frames, 64);
	backtrace_symbols_fd(frames, n, STDERR_FILENO);
	signal(sig, SIG_DFL);
	raise(sig);
}
static void NukeInstallCrashSignals()
{
	for (int s : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT })
		signal(s, NukeCrashSignal);
}
#endif  // !_WIN32

//void CreateDemoObjects(){
//    Atom* root = new Atom("root");
//    Atom* subroot = new Atom("subroot");
//    Atom* secsubroot = new Atom("2 subroot");
//    Atom* deepObject = new Atom("Deep object");
//    deepObject->SetParent(subroot);
//    subroot->SetParent(root);
//    secsubroot->SetParent(root);
//    AppInstance::GetSingleton()->currentWorld->GetHierarchy().push_back(root);
//}

//int main() {
//	AppInstance* app = AppInstance::GetSingleton();
//	app->setEditor(true);
//	World* nScene = new World();
//	app->currentWorld = nScene;
//    iRender* render = NukeBGFX::getSingleton();
//	Config* conf = Config::getSingleton();
//    
//	
//	if (app->currentWorld->GetHierarchy().empty())
//    {
//        Atom* edcam = new Atom("Editor Camera");
//        cout << "[main]\t\t\t" << "Camera render is: " << render << endl;
//        Camera* edcamc = new Camera(edcam, render);
//        edcamc->transform->position = { 0, 10, -10};
//        edcamc->freeMode = true;
//        edcam->layer = Layer::L_EDITOR;
//        app->currentWorld->GetHierarchy().push_back(edcam);
//    }
//	cout << "[main]\t\t\t" << "New hierarchy size: " << app->currentWorld->GetHierarchy().size() << endl;
//	Atom* editorCam = app->currentWorld->Get("Editor Camera");
//	cout << "[main]\t\t\t" << "Editor camera: " << editorCam << endl;
//	
//	render->_UIinit = editorinit;
//    render->_UIkeyaboardUp = editorkeyaboardUp;
//    render->_UIkeyboard = editorkeyboard;
//    render->_UImouse = editormouse;
//    render->_UImouseWheel = editormousewheel;
//	render->_UImove = editormove;
//	render->_UIpmove = editorpmove;
//	render->_UIreshape = editorreshape;
//	render->_UIspecial = editorspecial;
//	render->_UIspecialUp = editorspecialUp;
//
//    //render->setOnRender(RenderScene);
//    render->setOnGUI(editorDraw);
//
//	render->init(conf->window.w, conf->window.h);
//	render->loop();
//	UnloadModules();
//	render->deinit();
//	return 0;
//}

void keyboard1(unsigned char c, int x, int y)
{
    auto scene = AppInstance::GetSingleton()->currentWorld;
    for(auto atom : scene->GetHierarchy()){
        if(auto mr = (atom->GetComponent<MeshRenderer>())){
            mr->enabled = !mr->enabled;
            cout << "[main]\t\t\t" << atom->name << "." << mr->name << ".enabled = " << mr->enabled << endl;
        }
    }
    cout << "[main]\t\t\t" << "[1] key pressed! " << c << endl;
}

void keyboard2(unsigned char c, int x, int y)
{
    //cout << "[main]\t\t\t" << "[2] ( " << x << ", " << y << ")" << endl;
}

void special(int key, int x, int y){
    cout << "[main]\t\t\t" << "[special] ( " << key << ", " << x << ", " << y << ")" << endl;
}

void specialup(int key, int x, int y){
//    cout << "[main]\t\t\t" << "[special UP] ( " << key << ", " << x << ", " << y << ")" << endl;
}

//void testRender(NukeOGL *gl){
//    cout << "[main]\t\t\t" << "=========== Render callbacks addresses[" << gl << "] ==============" << endl;
//    cout << "[main]\t\t\t" << gl->_UIinit << endl;
//    cout << "[main]\t\t\t" << gl->_UIkeyaboardUp << endl;
//    cout << "[main]\t\t\t" << gl->_UIkeyboard << endl;
//    cout << "[main]\t\t\t" << gl->_UImouse << endl;
//    cout << "[main]\t\t\t" << gl->_UImove << endl;
//    cout << "[main]\t\t\t" << gl->_UIpmove << endl;
//    cout << "[main]\t\t\t" << gl->_UIreshape << endl;
//    cout << "[main]\t\t\t" << gl->_UIspecial << endl;
//    cout << "[main]\t\t\t" << gl->_UIspecialUp << endl;
//    cout << "[main]\t\t\t" << "=============================== END ================================" << endl;
//}

void CreateDemoObjects(){
    Atom* root = new Atom("root");
    Atom* subroot = new Atom("subroot");
    Atom* secsubroot = new Atom("2 subroot");
    Atom* deepObject = new Atom("Deep object");
    deepObject->SetParent(subroot);
    subroot->SetParent(root);
    secsubroot->SetParent(root);
    AppInstance::GetSingleton()->currentWorld->GetHierarchy().push_back(root);
}

std::string MultiString(std::string str, int times) {
	std::string out = "";
	for (int i = 0; i < times; i++) {
		out += str;
	}
	return out;
}

void PrintHierarchy(Atom* atom, int level) {
	if (!atom)
		return;
	//Atom* goo = atom;
	cout << "[AppInstance]\t"
		<< MultiString("\t", level)
		<< atom->GetName()
		<< "\t(parentgpos: "
		<< (atom->GetParent() ? atom->GetParent()->GetTransform().globalPosition().toStringA() : "null")
		<< ")"
		<< ";; POS: "
		<< atom->GetTransform().globalPosition().toStringA()
		<< endl;

	if (atom->children.size() > 0)
		for (auto child : atom->children)
			PrintHierarchy(child, level + 1);
	else
		cout << "[AppInstance]\t" << MultiString("\t", level + 1) << "No children" << endl;
	if (atom->components.size() > 0)
		for (auto cmp : atom->components)
			cout << "[AppInstance]\t" << MultiString("\t", level + 1) << "+" << cmp->name << endl;
}

void InitEngine()
{
	cout << "[main]\t\t\t" << "Engine initialization..." << endl;
	cout << "[main]\t\t\t" << "Editor is: " << AppInstance::GetSingleton() << endl;
	cout << "[main]\t\t\t" << "Current scene is: " << AppInstance::GetSingleton()->currentWorld << endl;
	cout << "[main]\t\t\t" << "Hierarchy: " << &AppInstance::GetSingleton()->currentWorld->GetHierarchy() << endl;
	AppInstance* instance = AppInstance::GetSingleton();
    if (instance->currentWorld->GetHierarchy().empty())
    {
        Atom* edcam = new Atom("Editor Camera");
        iRender* rnd = AppInstance::GetSingleton()->render;
		cout << "[main]\t\t\t" << "Camera render is: " << rnd << endl;
        Camera* edcamc = new Camera(edcam, rnd);
        edcamc->transform->position = { 0, 10, -10};
        edcamc->freeMode = true;
        //edcamc->Init(edcam);
        edcam->layer = 31;   // "Editor" layer (see nuke::Layers)
        edcamc->editorCamera = true;
        AppInstance::GetSingleton()->currentWorld->GetHierarchy().push_back(edcam);
        //edcamc->renderer->currentWorld = AppInstance::GetSingleton()->currentWorld;
    }
	cout << "[main]\t\t\t" << "New hierarchy size: " << instance->currentWorld->GetHierarchy().size() << endl;
	cout << "[main]\t\t\t" << "Editor camera: " << instance->currentWorld->Get("Editor Camera") << endl;
}

void Unload()
{
    boost::this_thread::sleep_for(boost::chrono::milliseconds(1000));   // let module threads wind down
    UnloadModules();
}

void RenderObject(Atom* atom){
    for(auto goc : atom->children)
    {
        goc->Update<MeshRenderer>();
    }
}

void RenderScene(){
    auto scene = AppInstance::GetSingleton()->currentWorld;
    for(auto atom : scene->GetHierarchy()){
        RenderObject(atom);
        if(auto mr = (atom->GetComponent<Camera>())){
            mr->Update();
        }
    }
}

iRender* PreInitRender(){
	cout << "[main]\t\t\t" << "Render preinit..." << endl;

    iRender * render = AppInstance::GetSingleton()->render;

	cout << "[main]\t\t\t" << "Renderer is: " << render << endl;
    // Asset preview renders first so the live scene re-pushes its own lights/sky/TLAS after it.
    render->setOnRender([]{
        iRender* r = AppInstance::GetSingleton()->render;
        // Must be first: tearing the world down mid-command-list breaks D3D12.
        EditorUI::getSingleton()->ApplyPendingWorldOpen();
        r->setWireframe(false);   // previews always render solid, whatever the toolbar says
        EditorUI::getSingleton()->RenderAssetPreview(r);
        r->setWireframe(AppInstance::GetSingleton()->wireframe);
        AppInstance::GetSingleton()->currentWorld->Render(r);
    });

	cout << "[main]\t\t\t" << "Preinit done... Next stage..." << endl;

    return render;
}

void InitInput(KeyBoard *keyboard){
	cout << "[main]\t\t\t" << "Init input..." << endl;
    *keyboard += keyboard1;
    *keyboard &= keyboard2;
    *keyboard *= special;
    *keyboard |= specialup;
	cout << "[main]\t\t\t" << "Done!... Next stage..." << endl;
}


int main(int argc, char** argv)
{
#ifdef _WIN32
	SetUnhandledExceptionFilter(NukeCrashTrace);   // symbolized stack on any crash
#ifdef _DEBUG
	_CrtSetReportHook(NukeCrtReportHook);          // ...and on CRT asserts (IM_ASSERT)
#endif
#else
	NukeInstallCrashSignals();                     // symbolized stack on any crash/abort
#endif
	nuke::Log::CaptureStd();   // must precede any boot logging so it lands in the Console panel

	// Absolutize the project/archive argument against the ORIGINAL cwd, before the cwd change below.
	std::string projectArg, archiveArg;
	if (argc > 1 && argv[1])
	{
		std::string a = argv[1];
		auto endsWith = [&](const char* s) { size_t n = strlen(s); return a.size() >= n && a.compare(a.size() - n, n, s) == 0; };
		boost::system::error_code ec;
		if (endsWith(".nuproj"))                        projectArg = bfs::absolute(bfs::path(a)).string();
		else if (endsWith(".nupak") || endsWith(".numod")) archiveArg = bfs::absolute(bfs::path(a)).string();
	}

	// cwd = the WRITABLE root (== run root on Windows and in the dev tree): engine resources
	// are cwd-relative there, while an INSTALLED editor must aim every relative write at the
	// per-user dir — never beside or inside the bundle. Shipped assets resolve through
	// absolute run-root paths (RunRoot/baseDir) regardless of cwd.
	{
		boost::system::error_code ec;
		bfs::create_directories(nuke::Config::writableDir(), ec);
		bfs::current_path(nuke::Config::writableDir(), ec);
	}
#if !defined(_WIN32) && !defined(__APPLE__)
	// The EDITOR's display server — an explicit Preference (Interface -> Display server).
	// "auto" = X11: the multi-window UX (tear-off panels, drag-to-dock, imgui viewports)
	// needs client-side positioning and a global cursor, which native Wayland forbids by
	// design; the single-window Player keeps the engine's native-Wayland default. Read RAW
	// this early — render init happens long before LoadPreferences. An explicit
	// NUKE_DISPLAY_BACKEND env var still wins (setenv flag 0 never overwrites).
	{
		std::string db = "auto";
		try
		{
			bfs::ifstream pf(nuke::Config::userDataDir() / "NukeEngine" / "preferences.json");
			if (pf)
			{
				nlohmann::json pj = nlohmann::json::parse(pf, nullptr, false);
				if (!pj.is_discarded() && pj.is_object())
					db = pj.value("displayBackend", std::string("auto"));
			}
		}
		catch (...) {}
		setenv("NUKE_DISPLAY_BACKEND", db == "wayland" ? "wayland" : "x11", 0);
	}
#endif
#ifndef _WIN32
	// A GUI-launched .app gets launchd's minimal PATH — the toolchains the editor spawns
	// (cmake, dotnet) live in package-manager prefixes. Append the standard homes once;
	// a terminal launch keeps its richer PATH in front.
	{
		std::string path = getenv("PATH") ? getenv("PATH") : "";
		std::string added;
		std::vector<std::string> extras = { "/opt/homebrew/bin", "/usr/local/bin",
		                           "/usr/local/share/dotnet", "/Applications/CMake.app/Contents/bin",
		                           // Linux homes: user installs, distro dotnet layouts, snap/flatpak exports.
		                           "/usr/lib/dotnet", "/usr/lib64/dotnet", "/usr/share/dotnet",
		                           "/snap/bin", "/var/lib/flatpak/exports/bin" };
		if (const char* home = getenv("HOME"))
			extras.push_back(std::string(home) + "/.local/bin");
		for (const std::string& extraStr : extras)
		{
			const char* extra = extraStr.c_str();
			boost::system::error_code ec;
			if (path.find(extra) == std::string::npos && bfs::is_directory(extra, ec))
			{
				path += std::string(":") + extra;
				added += std::string(added.empty() ? "" : ", ") + extra;
			}
		}
		setenv("PATH", path.c_str(), 1);
		if (!added.empty())
			cout << "[main]\t\t\tPATH += " << added << " (GUI launch: toolchains for module builds)" << endl;
	}
	// VCPKG_ROOT likewise never reaches a GUI launch (login-shell export). Probe the
	// build-time location first (baked by the superbuild), then the common homes.
	// FOOL-PROOFING: an env var pointing at a bare vcpkg CHECKOUT (no installed/ tree —
	// cloned once, never used) would poison the next engine configure; classic-mode
	// consumers need the installed tree, so such a root is treated as unset here.
	if (const char* vr = getenv("VCPKG_ROOT"))
	{
		boost::system::error_code ec;
		if (!bfs::exists(bfs::path(vr) / "installed", ec))
		{
			cout << "[main]\t\t\tVCPKG_ROOT = " << vr << " has no installed/ tree — ignoring it "
			     << "(engine builds need a classic vcpkg install; probing the known roots)" << endl;
			unsetenv("VCPKG_ROOT");
		}
	}
	if (!getenv("VCPKG_ROOT"))
	{
		auto valid = [](const bfs::path& r)
		{
			boost::system::error_code ec;
			return bfs::exists(r / "scripts" / "buildsystems" / "vcpkg.cmake", ec);
		};
		std::vector<bfs::path> probes;
#ifdef NUKE_VCPKG_ROOT_DEF
		probes.push_back(NUKE_VCPKG_ROOT_DEF);
#endif
		if (const char* home = getenv("HOME"))
		{
			probes.push_back(bfs::path(home) / "vcpkg");
			probes.push_back(bfs::path(home) / "projects" / "vcpkg");
			probes.push_back(bfs::path(home) / "dev" / "vcpkg");
		}
		probes.push_back("/opt/vcpkg");
		probes.push_back("/usr/local/vcpkg");
		for (const bfs::path& r : probes)
			if (valid(r))
			{
				setenv("VCPKG_ROOT", r.string().c_str(), 1);
				cout << "[main]\t\t\tVCPKG_ROOT = " << r.string() << " (discovered for module builds)" << endl;
				break;
			}
	}
#endif
	// macOS: LaunchServices delivers a double-clicked document as an Apple Event, not argv —
	// catch it before the first event pump (no-op elsewhere).
	EditorInstallOpenDocHandler();

	AppInstance* instance = AppInstance::GetSingleton();
	instance->setEditor(true);
	cout << "[main]\t\t\t" << "NukeEngine starting... Welcome!" << endl;

	// Archive argument (.nupak / .numod): resolve it to a project folder and open that.
	if (!archiveArg.empty())
	{
		cout << "[main]\t\t\t" << "Opening archive: " << archiveArg << endl;
		const bool isMod = archiveArg.size() > 6 && archiveArg.compare(archiveArg.size() - 6, 6, ".numod") == 0;
		// .nupak = read-only MOUNT + overlay (never extracted); .numod = editable, extracts.
		projectArg = isMod ? EditorUI::PrepareArchiveProject(archiveArg)
		                   : EditorUI::PrepareMountedProject(archiveArg);
		if (projectArg.empty())
		{
			cout << "[main]\t\t\t" << "Archive open failed. Aborting." << endl;
			return 1;
		}
	}

	if (!projectArg.empty())
	{
		cout << "[main]\t\t\t" << "Opening project: " << projectArg << endl;
		EditorUI::getSingleton()->SetProjectFile(projectArg);
	}
	else
	{
		// No explicit project: startup choice comes from the machine preferences (userDataDir:
		// %APPDATA% / ~/Library/Application Support / XDG) — last project, or the project hub.
		int startupMode = 0;
		std::string lastProject;
		{
			try
			{
				bfs::ifstream pf(nuke::Config::userDataDir() / "NukeEngine" / "preferences.json");
				if (pf)
				{
					std::stringstream ss; ss << pf.rdbuf();
					nlohmann::json pj = nlohmann::json::parse(ss.str(), nullptr, false, true);
					if (pj.is_object())
					{
						startupMode = pj.value("startupProject", 0);
						if (pj.contains("recentProjects") && pj["recentProjects"].is_array())
							for (auto& r : pj["recentProjects"])   // newest existing entry wins
							{
								boost::system::error_code ec;
								if (r.is_string() && bfs::exists(bfs::path(r.get<std::string>()), ec))
									{ lastProject = r.get<std::string>(); break; }
							}
					}
				}
			}
			catch (...) {}
		}
		boost::system::error_code ec;
		if (startupMode == 0 && !lastProject.empty())
		{
			cout << "[main]\t\t\t" << "Opening last project: " << lastProject << endl;
			EditorUI::getSingleton()->SetProjectFile(lastProject);
		}
		else if (startupMode == 0 && bfs::exists(bfs::path("project/game.nuproj"), ec))
		{
			// Legacy layout: a root project/ predating the hub still opens.
			cout << "[main]\t\t\t" << "Opening legacy root project." << endl;
		}
		else
		{
			cout << "[main]\t\t\t" << "No project chosen - starting the project hub." << endl;
			EditorUI::getSingleton()->projectHubMode = true;
		}
	}

	// Startup phase 1 (PHASE_BOOT): discover plugins (metadata only), then enable the project's
	// render provider — the full LoadProject() only runs once the window/UI exist.
	cout << "[main]\t\t\t" << "Discovering plugins..." << endl;
	InitModules(instance);
	NUKEModule* renderPlugin = FindServiceProvider("render",
		EditorUI::getSingleton()->EarlyProjectService("render"));
	if (!renderPlugin) {
		cout << "[main]\t\t\t" << "No render provider found in modules/. Aborting." << endl;
		return 1;
	}
	EnablePlugin(renderPlugin);
	iRender* render = GetService<iRender>();
	if (!render) {
		cout << "[main]\t\t\t" << "Render provider '" << renderPlugin->title
		     << "' registered no iRender. Aborting." << endl;
		return 1;
	}
	instance->render = render;

	InitEngine();
	PreInitRender();
	cout << "[main]\t\t\t" << "Preinited render is: " << render << endl;
    Config* config = Config::getSingleton();
	Config::SetConsoleWindowVisible(config->window.showConsole);
	nuke::Log::SetConsoleEcho(config->logToConsole);   // off = skip the slow conhost write; the ring still gets it

	instance->config = config;
	instance->keyboard = KeyBoard::getSingleton();
	instance->mouse = Mouse::getSingleton();

   
	InitInput(instance->keyboard);
	cout << "[main]\t\t\t" << ">> Window size: w(" << config->window.w << "), h(" << config->window.h << ")" << endl;
    WindowDesc wd;
    wd.w = config->window.w; wd.h = config->window.h;
    wd.title       = "NukeEngine Editor";   // fixed; the game window titles itself from the project name
    wd.decorated   = config->window.decorated;
    wd.resizable   = config->window.resizable;
    wd.floating    = config->window.floating;
    wd.maximized   = config->window.maximized;
    wd.fullscreen  = config->window.fullscreen;
    wd.transparent = false;   // editor window is always opaque; per-pixel transparency is runtime-only
    wd.opacity     = config->window.opacity;
    // Editor backend comes from the preferences (userDataDir scope), not the project config —
    // config/main.json window.backend is the runtime (Player) backend. Default = Vulkan.
    int editorBackend = 2; bool editorRT = true;
    {
        try
        {
            boost::filesystem::ifstream pf(nuke::Config::userDataDir() / "NukeEngine" / "preferences.json");
            if (pf)
            {
                std::stringstream ss; ss << pf.rdbuf();
                nlohmann::json pj = nlohmann::json::parse(ss.str(), nullptr, false, true);
                if (pj.is_object())
                {
                    editorBackend = pj.value("editorBackend", 2);
                    editorRT      = pj.value("editorRayTracing", true);   // false = raster path in the editor
                }
            }
        }
        catch (...) {}
    }
    wd.backend     = editorBackend;
    wd.rayTracing  = editorRT;
    wd.gpuValidation = config->gpuValidation;
    LoadBuiltinShaders(render, "shaders");   // engine-side built-in shaders -> renderer
    render->init(wd);
    render->setVSync(config->window.vsync);
    cout << "[main]\t\t\t" << "> Render: " << render << endl;

	// Native imgui multi-viewport only on Vulkan; D3D falls back to GDI-hosted windows (DXGI races).
	NukeUI::EnableNativeViewports(editorBackend == 2);
	NukeUI::Init(render);

	nuke::InstallDesktopInput(render);   // chains the UI callbacks

	// New-menu template for the .nuinput input map asset.
	{
		nuke::AssetCreator ic;
		ic.label = "Input Map"; ic.ext = ".nuinput"; ic.baseName = "Input"; ic.category = "Input";
		ic.textEditable = true; ic.syntaxLanguage = "json";
		ic.content =
			"{\n  \"actions\": [\n"
			"    { \"name\": \"Move\", \"type\": 2 },\n"
			"    { \"name\": \"Look\", \"type\": 2 },\n"
			"    { \"name\": \"Jump\", \"type\": 0 },\n"
			"    { \"name\": \"Fire\", \"type\": 0 }\n  ],\n"
			"  \"contexts\": [\n    { \"name\": \"Gameplay\", \"priority\": 0, \"active\": true, \"bindings\": [\n"
			"      { \"action\": \"Move\", \"controls\": [\"Key.W\"], \"axis\": 1, \"scale\": 1,  \"phase\": 1 },\n"
			"      { \"action\": \"Move\", \"controls\": [\"Key.S\"], \"axis\": 1, \"scale\": -1, \"phase\": 1 },\n"
			"      { \"action\": \"Move\", \"controls\": [\"Key.A\"], \"axis\": 0, \"scale\": -1, \"phase\": 1 },\n"
			"      { \"action\": \"Move\", \"controls\": [\"Key.D\"], \"axis\": 0, \"scale\": 1,  \"phase\": 1 },\n"
			"      { \"action\": \"Look\", \"controls\": [\"Mouse.DeltaX\"], \"axis\": 0, \"phase\": 1 },\n"
			"      { \"action\": \"Look\", \"controls\": [\"Mouse.DeltaY\"], \"axis\": 1, \"phase\": 1 },\n"
			"      { \"action\": \"Jump\", \"controls\": [\"Key.Space\"], \"phase\": 0 },\n"
			"      { \"action\": \"Fire\", \"controls\": [\"Mouse.Left\"], \"phase\": 1 }\n    ] }\n  ]\n}\n";
		nuke::RegisterAssetCreator(ic);
	}

	// Engine atom templates for the "+" create menu; modules register their own from OnLoad.
	nuke::RegisterAtomCreator({ "Effects", "Wind Zone", "\xee\x86\xb0" /* ICON_LC_WIND */, { "WindZone" } });
	// Engine component icons for the viewport overlay.
	nuke::RegisterComponentIcon({ "WindZone", "\xee\x86\xb0" /* ICON_LC_WIND */, { 0.63f, 0.9f, 0.78f, 0.92f } });

	// Workers before SetUp: its heavy tail loads in the background (EditorUI::StartBootLoad).
	nuke::Jobs::Init(Config::getSingleton()->jobWorkers, Config::getSingleton()->jobPinCores);
	editorinit();                       // SetUp: loads the project + activates its chosen plugins
	instance->StartFixedThread();       // fixed-frequency update thread (idles until PIE plays)
	NukeUI::AddDrawCallback(editorDraw);
	cout << "[main]\t\t\t" << "Editor UI initialized." << endl;

	cout << "[main]\t\t\t" << "All done. Starting render loop." << endl;

	//AppInstance::GetSingleton()->StartUpdateThread();
	cout << "[main]\t\t\t" << "Update is bootstraped. Next stage..." << endl;
    render->loop();

    cout << "[main]\t\t\t" << "shit down..." << endl;
    EditorUI::getSingleton()->SaveEditorState();   // first: any later teardown step may wedge or die
    AppInstance::GetSingleton()->StopFixedThread();
    nuke::Jobs::Shutdown();
    Unload();   // runtime plugins first, then the render provider
    return 0;
}
