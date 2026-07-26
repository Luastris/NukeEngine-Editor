#include <API/Model/Atom.h>
#include <interface/Services.h>
#include <nukeui.h>
#include "imgui.h"
#include <editor/editorui.h>
#include <input/DesktopInput.h>   // gameplay input provider (keyboard/mouse)
#include <interface/AssetCreators.h>   // register the .nuinput asset type
#include <interface/AtomCreators.h>    // register engine atom templates for the "+" menu
#include <interface/ComponentIcons.h>  // register engine component icons for the viewport
#include <input/keyboard.h>
#include <config.h>
#include <interface/Modular.h>
#ifdef EDITOR
#include <interface/AppInstance.h>
#include <import/assimporter.h>
#include <API/Model/Jobs.h>                 // core job system (2.4)
#else
#include <interface/AppInstance.h>
#endif

#include <iostream>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>   // early Preferences read (editor render backend)
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
using namespace nuke;   // engine API now lives in namespace nuke

#ifdef _WIN32
// Crash telemetry: on any unhandled SEH exception (access violation, ...) print the
// faulting address + a SYMBOLIZED stack of the crashing thread to stdout/stderr (raw C
// stdio — works even while cout/cerr are rerouted into the log ring), then fall through
// to the system dialog. PDBs sit next to the binaries, so Debug builds resolve fully.
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
	// A CALL through a null/garbage pointer parks RIP outside every module — the walker
	// dies on frame 0. The return address of that call is on top of the stack: resume
	// from it so the report names the CALLER.
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
	// Noisier than a real walk (stale frames show up) but always names the neighborhood.
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

// CRT asserts (IM_ASSERT, _ASSERTE) abort without raising SEH — hook the report path and
// print the SAME symbolized stack so an assert names its caller in the captured log.
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

// OS integration (file dialog, .nuproj association) is declared neutrally in editor/editorui.h and
// implemented per-platform in src/platform/platform_win32.cpp / platform_other.cpp — no platform API
// is baked into this shared host code.

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
        edcam->layer = 31;   // "Editor" layer: the editor's own objects (see nuke::Layers)
        edcamc->editorCamera = true;   // screen-space canvases render as editable world planes for it
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
    // World rendering lives in the engine lib (World::Render), shared by editor
    // and game. The renderer just invokes onRender each frame. The asset-preview
    // world (inspector 3D preview) renders FIRST, so the live scene re-pushes its
    // own lights/sky/TLAS afterwards and the viewport image stays untouched.
    render->setOnRender([]{
        iRender* r = AppInstance::GetSingleton()->render;
        // FIRST: apply a queued world switch at the frame boundary — tearing the world
        // down mid-command-list (from a click handler) breaks D3D12 (render safety).
        EditorUI::getSingleton()->ApplyPendingWorldOpen();
        // Viewport draw mode (toolbar Solid/Wireframe) applies to the LIVE scene only —
        // asset previews (inspector/editor 3D thumbnails) always render solid.
        r->setWireframe(false);
        EditorUI::getSingleton()->RenderAssetPreview(r);
        r->setWireframe(AppInstance::GetSingleton()->wireframe);
        AppInstance::GetSingleton()->currentWorld->Render(r);
    });
    // UI is driven by the UI module (NukeUI); it is wired in main() after the
    // renderer is initialized (NukeUI hooks the renderer's onGUI itself).

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
#endif
	// FIRST: route cout/cerr through the engine log ring — every boot line (module loads,
	// service registrations, pak mounts) must land in the Console panel, not just stdout.
	nuke::Log::CaptureStd();

	// Capture + absolutize a .nuproj / .nupak / .numod argument against the ORIGINAL cwd,
	// BEFORE we change cwd below. Archives extract into an editable work tree (3.2).
	std::string projectArg, archiveArg;
	if (argc > 1 && argv[1])
	{
		std::string a = argv[1];
		auto endsWith = [&](const char* s) { size_t n = strlen(s); return a.size() >= n && a.compare(a.size() - n, n, s) == 0; };
		boost::system::error_code ec;
		if (endsWith(".nuproj"))                        projectArg = bfs::absolute(bfs::path(a)).string();
		else if (endsWith(".nupak") || endsWith(".numod")) archiveArg = bfs::absolute(bfs::path(a)).string();
	}

	// Always run with cwd = the editor's own directory. Engine resources (config, modules, shaders,
	// fonts) are cwd-relative, so they must resolve from the install dir regardless of how we were
	// launched — double-clicking a .nuproj otherwise sets cwd to the project folder, which breaks
	// every dependency and spawns empty config/ + modules/ there.
	{
		boost::system::error_code ec;
		bfs::path exeDir = boost::dll::program_location(ec).parent_path();
		if (!ec && !exeDir.empty()) bfs::current_path(exeDir, ec);
	}

	AppInstance* instance = AppInstance::GetSingleton();
	instance->setEditor(true);
	cout << "[main]\t\t\t" << "NukeEngine starting... Welcome!" << endl;

	// "Open with" / double-click an ARCHIVE (.nupak project / .numod mod): extract it into
	// "<stem>_project" beside the pak — the editable work tree — and open THAT. The base
	// pak is remembered (".nupak_base"), so Package Mod diffs against it / repacks a mod.
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

	// "Open with" / double-click a .nuproj: point the editor at that project (absolute path; cwd stays
	// the editor dir). The project folder = where the .nuproj lives; its content resolves from there.
	if (!projectArg.empty())
	{
		cout << "[main]\t\t\t" << "Opening project: " << projectArg << endl;
		EditorUI::getSingleton()->SetProjectFile(projectArg);
	}
	else
	{
		// No explicit project: the startup choice is a MACHINE preference (%APPDATA%) —
		// "open the last project" (default) or "always ask". Nothing is auto-created:
		// with no last project on disk the editor boots into the PROJECT HUB (recent
		// list / open / create) and the picked project relaunches the editor on itself.
		int startupMode = 0;
		std::string lastProject;
		if (const char* appdata = std::getenv("APPDATA"))
		{
			try
			{
				bfs::ifstream pf(bfs::path(appdata) / "NukeEngine" / "preferences.json");
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
			// Legacy layout: a root project/ from before the hub existed keeps opening
			// (and gets recorded into the recent list once loaded).
			cout << "[main]\t\t\t" << "Opening legacy root project." << endl;
		}
		else
		{
			cout << "[main]\t\t\t" << "No project chosen - starting the project hub." << endl;
			EditorUI::getSingleton()->projectHubMode = true;
		}
	}

	// Two-phase startup, phase 1 (PHASE_BOOT). Discover the shared plugin pool first —
	// metadata only, nothing activated — then enable the project's chosen render provider
	// ("services.render" in the .nuproj, read early because the full LoadProject() runs
	// after the window/UI exist). The renderer is an ordinary plugin now; the host gets
	// its iRender through the service registry.
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
	// Optional: hide the OS console (window.showConsole=false). The in-app Console panel still
	// captures stdout; a console shared with a launching terminal is left alone (guard inside).
	Config::SetConsoleWindowVisible(config->window.showConsole);
	// Perf: logToConsole=false stops the slow conhost echo. CaptureStd (above) tee's cout into
	// the ring, so the in-app Console panel keeps showing everything — only the OS write drops.
	nuke::Log::SetConsoleEcho(config->logToConsole);

	instance->config = config;
	instance->keyboard = KeyBoard::getSingleton();
	instance->mouse = Mouse::getSingleton();

   
	InitInput(instance->keyboard);
	cout << "[main]\t\t\t" << ">> Window size: w(" << config->window.w << "), h(" << config->window.h << ")" << endl;
    WindowDesc wd;
    wd.w = config->window.w; wd.h = config->window.h;
    wd.title       = "NukeEngine Editor";   // fixed: the editor is always the editor (the game window
                                            // titles itself from the project's name — not config)
    wd.decorated   = config->window.decorated;
    wd.resizable   = config->window.resizable;
    wd.floating    = config->window.floating;
    wd.maximized   = config->window.maximized;
    wd.fullscreen  = config->window.fullscreen;
    wd.transparent = false;   // per-pixel transparency is a GAME/runtime feature — the editor window is
                              // always opaque (no DComp swap chain), whatever the config says
    wd.opacity     = config->window.opacity;
    // EDITOR backend comes from the engine-wide PREFERENCES (%APPDATA%), NOT the project
    // config: config/main.json window.backend is the RUNTIME (Player) backend and ships
    // with the packaged game. Editor default = Vulkan (native detachable windows).
    int editorBackend = 2; bool editorRT = true;
    if (const char* appdata = std::getenv("APPDATA"))
    {
        try
        {
            boost::filesystem::ifstream pf(boost::filesystem::path(appdata) / "NukeEngine" / "preferences.json");
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
    wd.gpuValidation = config->gpuValidation;   // Debug GPU validation opt-in (config, works for double-click)
    LoadBuiltinShaders(render, "shaders");   // engine loads built-in shaders + feeds the renderer
    render->init(wd);
    render->setVSync(config->window.vsync);   // honour config vsync (Game.SetVSync toggles it live)
    cout << "[main]\t\t\t" << "> Render: " << render << endl;

	// Bring up the UI module (ImGui) — it renders through the renderer's neutral
	// seam, so this works regardless of which renderer module is loaded.
	// VULKAN: native imgui multi-viewport — any panel/editor dragged out becomes a real
	// per-window swapchain OS window (the Vulkan WSI has none of the DXGI races that
	// forced the single-window model + GDI hosts on D3D, which remain the fallback).
	NukeUI::EnableNativeViewports(editorBackend == 2);
	NukeUI::Init(render);

	nuke::InstallDesktopInput(render);   // gameplay input: keyboard/mouse -> Input controls (chains the UI callbacks)

	// .nuinput = a first-class, text-editable input map asset. New-menu template = a ready Gameplay context
	// (WASD -> Move, mouse -> Look, Space -> Jump, LMB -> Fire). Any .nuinput in content auto-loads (ResDB).
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

	// ENGINE atom templates for the "+" create menu (interface/AtomCreators.h). Modules
	// register their own from OnLoad — same registry, no editor linkage.
	nuke::RegisterAtomCreator({ "Effects", "Wind Zone", "\xee\x86\xb0" /* ICON_LC_WIND */, { "WindZone" } });
	// ENGINE component icons for the viewport overlay (interface/ComponentIcons.h) — same
	// registry the modules use; the viewport draws only from it, zero per-type hardcode.
	nuke::RegisterComponentIcon({ "WindZone", "\xee\x86\xb0" /* ICON_LC_WIND */, { 0.63f, 0.9f, 0.78f, 0.92f } });

	editorinit();                       // SetUp: loads the project + activates its chosen plugins
	instance->StartFixedThread();       // fixed-frequency update thread (idles until PIE plays)
	nuke::Jobs::Init(Config::getSingleton()->jobWorkers, Config::getSingleton()->jobPinCores);   // worker pool (2.4)
	NukeUI::AddDrawCallback(editorDraw); // editor draws via the UI module each frame
	cout << "[main]\t\t\t" << "Editor UI initialized." << endl;

    // The project's default world is opened from content by editorinit() (SetUp) — after the
    // project's plugins are active, so components deserialize correctly. Nothing to restore here.

	cout << "[main]\t\t\t" << "All done. Starting render loop." << endl;

	//AppInstance::GetSingleton()->StartUpdateThread();
	cout << "[main]\t\t\t" << "Update is bootstraped. Next stage..." << endl;
    render->loop();

    cout << "[main]\t\t\t" << "shit down..." << endl;
    AppInstance::GetSingleton()->StopFixedThread();
    nuke::Jobs::Shutdown();
    EditorUI::getSingleton()->SaveEditorState();   // persist editor state (camera, selection, panels)
    Unload();   // runtime plugins first, then the render provider (its Shutdown deinits the renderer)
    return 0;
}
