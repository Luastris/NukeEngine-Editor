#include <API/Model/Atom.h>
#include <interface/Services.h>
#include <nukeui.h>
#include "imgui.h"
#include <editor/editorui.h>
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
#include <boost/dll/runtime_symbol_info.hpp>   // program_location() -> exe dir
#include <boost/thread.hpp>
#include <boost/chrono.hpp>
namespace bfs = boost::filesystem;
using namespace std;
using namespace nuke;   // engine API now lives in namespace nuke

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
//    AppInstance::GetSingleton()->currentScene->GetHierarchy().push_back(root);
//}

//int main() {
//	AppInstance* app = AppInstance::GetSingleton();
//	app->setEditor(true);
//	World* nScene = new World();
//	app->currentScene = nScene;
//    iRender* render = NukeBGFX::getSingleton();
//	Config* conf = Config::getSingleton();
//    
//	
//	if (app->currentScene->GetHierarchy().empty())
//    {
//        Atom* edcam = new Atom("Editor Camera");
//        cout << "[main]\t\t\t" << "Camera render is: " << render << endl;
//        Camera* edcamc = new Camera(edcam, render);
//        edcamc->transform->position = { 0, 10, -10};
//        edcamc->freeMode = true;
//        edcam->layer = Layer::L_EDITOR;
//        app->currentScene->GetHierarchy().push_back(edcam);
//    }
//	cout << "[main]\t\t\t" << "New hierarchy size: " << app->currentScene->GetHierarchy().size() << endl;
//	Atom* editorCam = app->currentScene->Get("Editor Camera");
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
    auto scene = AppInstance::GetSingleton()->currentScene;
    for(auto go : scene->GetHierarchy()){
        if(auto mr = (go->GetComponent<MeshRenderer>())){
            mr->enabled = !mr->enabled;
            cout << "[main]\t\t\t" << go->name << "." << mr->name << ".enabled = " << mr->enabled << endl;
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
    AppInstance::GetSingleton()->currentScene->GetHierarchy().push_back(root);
}

std::string MultiString(std::string str, int times) {
	std::string out = "";
	for (int i = 0; i < times; i++) {
		out += str;
	}
	return out;
}

void PrintHierarchy(Atom* go, int level) {
	if (!go)
		return;
	//Atom* goo = go;
	cout << "[AppInstance]\t"
		<< MultiString("\t", level)
		<< go->GetName()
		<< "\t(parentgpos: "
		<< (go->GetParent() ? go->GetParent()->GetTransform().globalPosition().toStringA() : "null")
		<< ")"
		<< ";; POS: "
		<< go->GetTransform().globalPosition().toStringA()
		<< endl;

	if (go->children.size() > 0)
		for (auto child : go->children)
			PrintHierarchy(child, level + 1);
	else
		cout << "[AppInstance]\t" << MultiString("\t", level + 1) << "No children" << endl;
	if (go->components.size() > 0)
		for (auto cmp : go->components)
			cout << "[AppInstance]\t" << MultiString("\t", level + 1) << "+" << cmp->name << endl;
}

void InitEngine()
{
	cout << "[main]\t\t\t" << "Engine initialization..." << endl;
	cout << "[main]\t\t\t" << "Editor is: " << AppInstance::GetSingleton() << endl;
	cout << "[main]\t\t\t" << "Current scene is: " << AppInstance::GetSingleton()->currentScene << endl;
	cout << "[main]\t\t\t" << "Hierarchy: " << &AppInstance::GetSingleton()->currentScene->GetHierarchy() << endl;
	AppInstance* instance = AppInstance::GetSingleton();
    if (instance->currentScene->GetHierarchy().empty())
    {
        Atom* edcam = new Atom("Editor Camera");
        iRender* rnd = AppInstance::GetSingleton()->render;
		cout << "[main]\t\t\t" << "Camera render is: " << rnd << endl;
        Camera* edcamc = new Camera(edcam, rnd);
        edcamc->transform->position = { 0, 10, -10};
        edcamc->freeMode = true;
        //edcamc->Init(edcam);
        edcam->layer = NUKEE_LAYER_EDITOR;
        AppInstance::GetSingleton()->currentScene->GetHierarchy().push_back(edcam);
        //edcamc->renderer->currentScene = AppInstance::GetSingleton()->currentScene;
    }
	cout << "[main]\t\t\t" << "New hierarchy size: " << instance->currentScene->GetHierarchy().size() << endl;
	cout << "[main]\t\t\t" << "Editor camera: " << instance->currentScene->Get("Editor Camera") << endl;
}

void Unload()
{
    boost::this_thread::sleep_for(boost::chrono::milliseconds(1000));   // let module threads wind down
    UnloadModules();
}

void RenderObject(Atom* go){
    for(auto goc : go->children)
    {
        goc->Update<MeshRenderer>();
    }
}

void RenderScene(){
    auto scene = AppInstance::GetSingleton()->currentScene;
    for(auto go : scene->GetHierarchy()){
        RenderObject(go);
        if(auto mr = (go->GetComponent<Camera>())){
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
        EditorUI::getSingleton()->RenderAssetPreview(r);
        AppInstance::GetSingleton()->currentScene->Render(r);
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
	
	instance->config = config;
	instance->keyboard = KeyBoard::getSingleton();
	instance->mouse = Mouse::getSingleton();

   
	InitInput(instance->keyboard);
	cout << "[main]\t\t\t" << ">> Window size: w(" << config->window.w << "), h(" << config->window.h << ")" << endl;
    WindowDesc wd;
    wd.w = config->window.w; wd.h = config->window.h;
    wd.title       = "NukeEngine Editor";   // editor's OWN title; config.title is the game/Player window
    wd.decorated   = config->window.decorated;
    wd.resizable   = config->window.resizable;
    wd.floating    = config->window.floating;
    wd.maximized   = config->window.maximized;
    wd.fullscreen  = config->window.fullscreen;
    wd.transparent = config->window.transparent;
    wd.opacity     = config->window.opacity;
    wd.backend     = config->window.backend;   // D3D11 / D3D12 (from config.json window.backend)
    LoadBuiltinShaders(render, "shaders");   // engine loads built-in shaders + feeds the renderer
    render->init(wd);
    cout << "[main]\t\t\t" << "> Render: " << render << endl;

	// Bring up the UI module (ImGui) — it renders through the renderer's neutral
	// seam, so this works regardless of which renderer module is loaded.
	NukeUI::Init(render);

	editorinit();                       // SetUp: loads the project + activates its chosen plugins
	instance->StartFixedThread();       // fixed-frequency update thread (idles until PIE plays)
	nuke::Jobs::Init(Config::getSingleton()->jobWorkers, Config::getSingleton()->jobPinCores);   // worker pool (2.4)
	NukeUI::AddDrawCallback(editorDraw); // editor draws via the UI module each frame
	cout << "[main]\t\t\t" << "Editor UI initialized." << endl;

    //CreateDemoObjects();
    //cubepositions();
	cout << "[main]\t\t\t" << "Done! Importing model..." << endl;

    AssImporter::getSingleton()->Import("mpm_vol.09_p35.OBJ");
    if(ResDB::getSingleton()->prefabs.size() > 0)
    {
        for(auto pref : ResDB::getSingleton()->prefabs){
            AppInstance::GetSingleton()->currentScene->Add(pref);
        }
//        for(auto m : ResDB::getSingleton()->meshes){
//            Atom* go = new Atom(m->name);
//            MeshRenderer* mr = new MeshRenderer();
//            mr->mesh = m;
//            go->AddComponent(mr);//dynamic_cast<Component*>(mr));
//            cout << "[main]\t\t\t" << go->name << " : " << go->transform.position.toStringA() << endl;
//            AppInstance::GetSingleton()->currentScene->Add(go);
//        }
    }

    // The project's default world is opened from content by editorinit() (SetUp) — after the
    // project's plugins are active, so components deserialize correctly. Nothing to restore here.

	cout << "[main]\t\t\t" << "Hierarchy: " << &AppInstance::GetSingleton()->currentScene->GetHierarchy() << endl;
	/*if(AppInstance::GetSingleton()->currentScene->GetHierarchy().size() > 0)
		for(auto g : AppInstance::GetSingleton()->currentScene->GetHierarchy())
			if(g)
				PrintHierarchy(g, 0);*/

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
