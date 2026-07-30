// C++ GAME MODULES (Phase 6.0) — the native game workflow.
//
// A game on NukeEngine is a NUKEModule DLL: it links NukeEngine.lib, calls the full engine
// API directly (no marshaling — Tilemap/Path/Jobs on hot paths), and registers reflected
// components via nukegen module mode, so they appear in Add Component / the inspector /
// world serialization / Lua+C# automatically. Sources live in <project>/source/<Name>/,
// DLLs build into <project>/modules/ which the editor scans into the shared plugin pool.
//
// The REBUILD CYCLE (this file): a discovered DLL is file-locked for the session, so
// Build & Reload first UNLOADS the project modules (components collapse to inert
// UnknownComponent placeholders), then runs cmake configure+build on a Jobs worker with
// the output streaming into the Console, then re-discovers and re-enables the fresh DLLs —
// the placeholders restore into live components. Refused while playing.
#include <editor/editorui.h>
#include <interface/Modular.h>
#include <API/Model/Jobs.h>
#include <API/Model/StatusBar.h>

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <iostream>
#include <set>

#ifdef _WIN32
#include <Windows.h>
#endif

using namespace nuke;

// ---- scaffold ------------------------------------------------------------------------------

void EditorUI::CreateGameModuleScaffold(const std::string& name)
{
	boost::system::error_code ec;
	const bfs::path proj = projectDir;
	const bfs::path src  = proj / "source" / name;
	if (bfs::exists(src / "CMakeLists.txt", ec))
	{
		std::cout << "[gamemodule]\t'" << name << "' already exists at " << src.string() << std::endl;
		return;
	}
	bfs::create_directories(src, ec);
	bfs::create_directories(proj / "modules", ec);

	// The engine repo root, captured at scaffold time from the running editor
	// (<root>/NukeEngine/x64/<cfg>/NukeEngine-Editor.exe). Overridable in the CMake cache
	// (NUKE_ENGINE_ROOT) when the project moves to another machine.
	const bfs::path engineRoot = bfs::absolute(bfs::current_path(ec)).parent_path().parent_path().parent_path();

	{
		bfs::ofstream f(src / "CMakeLists.txt", std::ios::binary);
		f <<
"cmake_minimum_required(VERSION 3.20)\n"
"project(" << name << " CXX)\n"
"\n"
"set(CMAKE_CXX_STANDARD 20)\n"
"set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
"cmake_policy(SET CMP0091 NEW)\n"
"set(CMAKE_MSVC_RUNTIME_LIBRARY \"MultiThreaded$<$<CONFIG:Debug>:Debug>DLL\")\n"
"# vcpkg's MSBuild integration picks debug vs release vcpkg libs from <UseDebugLibraries>;\n"
"# CMake omits it by default and Debug builds then SILENTLY link release vcpkg libs\n"
"# (mixed CRTs -> crashes in boost/std objects). Emit it per config.\n"
"set(CMAKE_VS_USE_DEBUG_LIBRARIES \"$<CONFIG:Debug>\")\n"
"\n"
"# The engine source root (headers + import libs). Set at scaffold time; override in the\n"
"# cache when the project builds on another machine: cmake -DNUKE_ENGINE_ROOT=...\n"
"set(NUKE_ENGINE_ROOT \"" << engineRoot.generic_string() << "\" CACHE PATH \"NukeEngine repo root\")\n"
"set(VCPKG_INSTALLED \"$ENV{VCPKG_ROOT}/installed/x64-windows\")\n"
"\n"
"# Reflection prebuild (nukegen module mode): scans this module's sources for NUKE_CLASS +\n"
"# [[nuke::prop/func]] and generates " << name << ".gen.inc (#included in-TU; call\n"
"# NukeReflectInit_" << name << "() from OnLoad).\n"
"find_package(Python3 COMPONENTS Interpreter)\n"
"add_custom_command(\n"
"    OUTPUT \"${CMAKE_CURRENT_SOURCE_DIR}/" << name << ".gen.inc\"\n"
"    COMMAND ${Python3_EXECUTABLE} \"${NUKE_ENGINE_ROOT}/NukeUtils/nukegen.py\"\n"
"            --include \"${CMAKE_CURRENT_SOURCE_DIR}\" --scan-cpp --no-includes\n"
"            --out \"${CMAKE_CURRENT_SOURCE_DIR}/" << name << ".gen.inc\"\n"
"            --init NukeReflectInit_" << name << "\n"
"    DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/" << name << ".cpp\"\n"
"    COMMENT \"nukegen: " << name << " reflection\")\n"
"\n"
"add_library(" << name << " SHARED\n"
"    " << name << ".cpp\n"
"    \"${CMAKE_CURRENT_SOURCE_DIR}/" << name << ".gen.inc\"\n"
")\n"
"set_source_files_properties(\"${CMAKE_CURRENT_SOURCE_DIR}/" << name << ".gen.inc\" PROPERTIES HEADER_FILE_ONLY ON)\n"
"\n"
"target_compile_definitions(" << name << " PRIVATE\n"
"    WIN32 _WINDOWS NOMINMAX _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS\n"
"    GLM_ENABLE_EXPERIMENTAL BOOST_ALL_DYN_LINK\n"
")\n"
"\n"
"target_include_directories(" << name << " PRIVATE\n"
"    ${NUKE_ENGINE_ROOT}/NukeEngine\n"
"    ${NUKE_ENGINE_ROOT}/NukeEngine/include\n"
"    ${NUKE_ENGINE_ROOT}/NukeEngine/deps\n"
"    ${NUKE_ENGINE_ROOT}/NukeEngine/deps/glm\n"
"    ${VCPKG_INSTALLED}/include\n"
")\n"
"\n"
"target_link_directories(" << name << " PRIVATE\n"
"    ${NUKE_ENGINE_ROOT}/NukeEngine/x64/$<CONFIG>\n"
"    ${VCPKG_INSTALLED}/$<$<CONFIG:Debug>:debug/>lib\n"
")\n"
"\n"
"target_link_libraries(" << name << " PRIVATE NukeEngine)\n"
"\n"
"# The DLL lands in <project>/modules/ — the editor scans it into the plugin pool.\n"
"add_custom_command(TARGET " << name << " POST_BUILD\n"
"    COMMAND ${CMAKE_COMMAND} -E make_directory \"${CMAKE_CURRENT_SOURCE_DIR}/../../modules\"\n"
"    COMMAND ${CMAKE_COMMAND} -E copy_if_different\n"
"            \"$<TARGET_FILE:" << name << ">\" \"${CMAKE_CURRENT_SOURCE_DIR}/../../modules/\"\n"
"    COMMENT \"Deploying " << name << ".dll to the project's modules/\")\n";
	}

	{
		bfs::ofstream f(src / (name + ".cpp"), std::ios::binary);
		f <<
"// " << name << " — a NATIVE C++ game module for NukeEngine.\n"
"//\n"
"// THE PERF MODEL (why the game is C++): write SYSTEMS, not per-atom logic. One component\n"
"// (a *System) owns FLAT arrays of game data and ticks them with Jobs::ParallelFor across\n"
"// every core; atoms/sprites are only the thin presentation of that data. Engine systems\n"
"// (Tilemap, Path, Rand, Noise, Events) are direct C++ calls here — no script marshaling.\n"
"//\n"
"// Reflected components (NUKE_CLASS + [[nuke::prop]]/[[nuke::func]]) appear in the editor's\n"
"// Add Component menu, the inspector, world saves and the Lua/C# bindings automatically.\n"
"//\n"
"// DEBUGGING: attach Visual Studio to NukeEngine-Editor.exe (or NukePlayer.exe) — this\n"
"// module's PDB sits next to the DLL; breakpoints work as usual. Rebuild from the editor:\n"
"// Project > Build & Reload Game Modules (the editor unloads the DLL first, so the build\n"
"// can overwrite it, then hot-swaps it back in — no editor restart).\n"
"#include <interface/NUKEEInteface.h>\n"
"#include <interface/AppInstance.h>\n"
"#include <API/Model/World.h>\n"
"#include <API/Model/Atom.h>\n"
"#include <API/Model/Component.h>\n"
"#include <API/Model/Time.h>\n"
"#include <API/Model/Events.h>\n"
"#include <API/Model/Rand.h>\n"
"#include <API/Model/Noise.h>\n"
"#include <API/Model/Jobs.h>\n"
"#include <API/Model/CharacterController.h>\n"
"#include <API/Model/Physics.h>\n"
"#include <input/Input.h>\n"
"#include <reflect/Reflect.h>\n"
"#include <iostream>\n"
"#include <cstring>\n"
"\n"
"using namespace nuke;\n"
"\n"
"// A sample reflected game component — replace with your game systems.\n"
"class " << name << "System : public Component\n"
"{\n"
"\tNUKE_CLASS(" << name << "System, Component)\n"
"public:\n"
"\t[[nuke::prop(label=\"Tick Log\", tip=\"Log a heartbeat once per in-game hour\")]] bool tickLog = true;\n"
"\n"
"\t" << name << "System() : Component(\"" << name << "System\") {}\n"
"\tvoid Init(Atom* parent) override\n"
"\t{\n"
"\t\tatom = parent;\n"
"\t\ttransform = &parent->GetTransform();\n"
"\t\tparent->components.push_back(this);\n"
"\t\t// Native event subscription (storyteller pattern): exact name or \"\" for all.\n"
"\t\tsubId = Events::Subscribe(\"time.newHour\", [this](const std::string&, const std::string&)\n"
"\t\t{\n"
"\t\t\tif (tickLog) std::cout << \"[" << name << "]\th=\" << Time::Hour() << \" day=\" << Time::Day() << std::endl;\n"
"\t\t});\n"
"\t}\n"
"\tvoid Destroy() override { Events::Unsubscribe(subId); subId = 0; }\n"
"\tvoid Update() override\n"
"\t{\n"
"\t\t// Per-frame game logic. dt = Time::Delta() (speed-scaled). Heavy per-entity work\n"
"\t\t// belongs in flat arrays + Jobs::ParallelFor, not per-atom components.\n"
"\t}\n"
"\tvoid FixedUpdate() override {}\n"
"\tvoid Pause() override {}\n"
"\tvoid Reset() override {}\n"
"private:\n"
"\tlong long subId = 0;\n"
"};\n"
"\n"
"// A NATIVE orbit camera — follows `target` and orbits it from the \"Look\" action with\n"
"// exponential position damping (same pattern as the Lua/C# OrbitCamera examples).\n"
"// Put it on the CAMERA atom; set `target` to the followed atom's name.\n"
"class OrbitCamera : public Component\n"
"{\n"
"\tNUKE_CLASS(OrbitCamera, Component)\n"
"public:\n"
"\t// Atom REFERENCE prop: the inspector draws the picker (combo + hierarchy drag-drop),\n"
"\t// serialization travels by stable id with a post-load fixup — never a name text box.\n"
"\t[[nuke::prop(label=\"Target\")]]              Atom* target = nullptr;\n"
"\t[[nuke::prop(label=\"Distance\",  min=0.5)]]  float distance = 6.0f;\n"
"\t[[nuke::prop(label=\"Height\")]]              float height = 1.5f;\n"
"\t[[nuke::prop(label=\"Sensitivity\", min=0)]]  float sens = 0.25f;\n"
"\t[[nuke::prop(label=\"Damping\",   min=0)]]    float damping = 10.0f;\n"
"\t[[nuke::prop(label=\"Invert Y\")]]            bool invertY = false;\n"
"\t[[nuke::prop(label=\"Collision\", tip=\"Spring arm: the camera never ends up behind geometry (needs colliders).\")]] bool collision = true;\n"
"\t[[nuke::prop(label=\"Collision Margin\", min=0)]] float collisionMargin = 0.25f;\n"
"\t[[nuke::prop(label=\"Probe Radius\", min=0.05, tip=\"The boom probes with a sphere - no flicker on thin edges.\")]] float probeRadius = 0.2f;\n"
"\t[[nuke::prop(label=\"Rotate Target\", tip=\"Turn the character to face where the camera looks (yaw only).\")]] bool rotateTarget = true;\n"
"\t[[nuke::prop(label=\"Turn Damping\", min=0)]] float turnDamping = 12.0f;\n"
"\t[[nuke::prop(label=\"Lock Cursor\", tip=\"Hide + pin the cursor to the window center on start (FPS mode); Esc (the UnlockCursor action) frees it.\")]] bool lockCursor = true;\n"
"\n"
"\tOrbitCamera() : Component(\"OrbitCamera\") {}\n"
"\tvoid Init(Atom* parent) override\n"
"\t{\n"
"\t\tatom = parent;\n"
"\t\ttransform = &parent->GetTransform();\n"
"\t\tparent->components.push_back(this);\n"
"\t}\n"
"\tvoid Update() override\n"
"\t{\n"
"\t\t// cursor: lock once on the first frame (FPS mode), Esc frees it (player.nuinput\n"
"\t\t// binds the \"UnlockCursor\" action to Key.Escape)\n"
"\t\tif (lockCursor && !cursorDone) { Input::SetCursorMode(2); cursorDone = true; }\n"
"\t\tif (Input::Pressed(\"UnlockCursor\")) Input::SetCursorMode(0);\n"
"\t\tAtom* tgt = target;\n"
"\t\tif (!tgt || !transform) return;\n"
"\t\tconst double dt = Time::getSingleton()->gameDelta;\n"
"\t\tconst Vector2 look = Input::Axis2(\"Look\");\n"
"\t\tyaw += look.x * sens;\n"
"\t\tpitch += look.y * sens * (invertY ? 1.0 : -1.0);\n"
"\t\tpitch = pitch < -75 ? -75 : (pitch > 80 ? 80 : pitch);\n"
"\t\t// aim first: the camera's own forward then points through the aim point\n"
"\t\ttransform->SetEulerDeg(Vector3(pitch, yaw, 0));\n"
"\t\tconst Vector3 f = transform->forward();\n"
"\t\tconst Vector3 tp = tgt->GetTransform().globalPosition();\n"
"\t\tconst Vector3 aim(tp.x, tp.y + height, tp.z);\n"
"\t\t// SPRING ARM: cast from the aim point back along the boom, IGNORING the followed\n"
"\t\t// atom's own body; a hit pulls the camera in front of it.\n"
"\t\tdouble dist = distance;\n"
"\t\tif (collision)\n"
"\t\t{\n"
"\t\t\tif (Physics::SphereCastIgnore(aim, probeRadius, Vector3(-f.x, -f.y, -f.z), distance, tgt))\n"
"\t\t\t{\n"
"\t\t\t\tdist = Physics::HitDistance() - collisionMargin;\n"
"\t\t\t\tif (dist < 0.5) dist = 0.5;\n"
"\t\t\t}\n"
"\t\t}\n"
"\t\tconst Vector3 desired(aim.x - f.x * dist, aim.y - f.y * dist, aim.z - f.z * dist);\n"
"\t\t// exponential damping: frame-rate independent, no overshoot; the collision clamp\n"
"\t\t// also applies to the SMOOTHED position, so damping can't drag it into walls.\n"
"\t\tif (!hasPos) { pos = desired; hasPos = true; }\n"
"\t\tconst double k = 1.0 - exp(-damping * dt);\n"
"\t\tpos = Vector3(pos.x + (desired.x - pos.x) * k, pos.y + (desired.y - pos.y) * k, pos.z + (desired.z - pos.z) * k);\n"
"\t\tif (collision)\n"
"\t\t{\n"
"\t\t\tconst Vector3 off(pos.x - aim.x, pos.y - aim.y, pos.z - aim.z);\n"
"\t\t\tconst double len = sqrt(off.x * off.x + off.y * off.y + off.z * off.z);\n"
"\t\t\tif (len > dist && len > 1e-6)\n"
"\t\t\t{\n"
"\t\t\t\tconst double s = dist / len;\n"
"\t\t\t\tpos = Vector3(aim.x + off.x * s, aim.y + off.y * s, aim.z + off.z * s);\n"
"\t\t\t}\n"
"\t\t}\n"
"\t\ttransform->position = pos;\n"
"\t\t// FACE THE CAMERA: turn the character (yaw only, pitch stays level) toward the\n"
"\t\t// camera's look direction, smoothed exponentially and wrapped at +-180 deg.\n"
"\t\tif (rotateTarget)\n"
"\t\t{\n"
"\t\t\tif (!hasFyaw) { fyaw = yaw; hasFyaw = true; }\n"
"\t\t\tdouble d = fmod(yaw - fyaw, 360.0);\n"
"\t\t\tif (d > 180.0) d -= 360.0; else if (d < -180.0) d += 360.0;\n"
"\t\t\tfyaw += d * (1.0 - exp(-turnDamping * dt));\n"
"\t\t\ttgt->GetTransform().SetEulerDeg(Vector3(0, fyaw, 0));\n"
"\t\t}\n"
"\t}\n"
"\tvoid Destroy() override {}\n"
"\tvoid FixedUpdate() override {}\n"
"\tvoid Pause() override {}\n"
"\tvoid Reset() override { hasPos = false; hasFyaw = false; cursorDone = false; }\n"
"private:\n"
"\tdouble yaw = 0, pitch = 15;\n"
"\tVector3 pos; bool hasPos = false;\n"
"\tdouble fyaw = 0; bool hasFyaw = false;\n"
"\tbool cursorDone = false;\n"
"};\n"
"\n"
"// A NATIVE character controller driver — the same pattern as the Lua/C# PlayerController\n"
"// examples: CharacterController (capsule) + the action-based input system (player.nuinput:\n"
"// Move = WASD/left stick, Jump = Space/pad South, Sprint = Shift).\n"
"class PlayerController : public Component\n"
"{\n"
"\tNUKE_CLASS(PlayerController, Component)\n"
"public:\n"
"\t[[nuke::prop(label=\"Speed\",        min=0)]] float speed = 5.0f;\n"
"\t[[nuke::prop(label=\"Sprint Mult\",  min=1)]] float sprintMul = 1.8f;\n"
"\t[[nuke::prop(label=\"Jump Speed\",   min=0)]] float jumpSpeed = 5.0f;\n"
"\n"
"\tPlayerController() : Component(\"PlayerController\") {}\n"
"\tvoid Init(Atom* parent) override\n"
"\t{\n"
"\t\tatom = parent;\n"
"\t\ttransform = &parent->GetTransform();\n"
"\t\tparent->components.push_back(this);\n"
"\t}\n"
"\tvoid Update() override\n"
"\t{\n"
"\t\tCharacterController* cc = atom ? atom->GetComponent<CharacterController>() : nullptr;\n"
"\t\tif (!cc) return;\n"
"\t\tconst Vector2 m = Input::Axis2(\"Move\");   // x = A/D, y = W/S (or the stick)\n"
"\t\tconst double  s = Input::Held(\"Sprint\") ? speed * sprintMul : speed;\n"
"\t\t// CHARACTER-RELATIVE move: W walks where the character faces (the orbit camera\n"
"\t\t// turns him to its own yaw), A/D strafe. Facing projected to XZ - no pitch leak.\n"
"\t\tconst Vector3 f = transform->forward();\n"
"\t\tconst double  fl = sqrt(f.x * f.x + f.z * f.z);\n"
"\t\tdouble fx = 0.0, fz = 1.0;\n"
"\t\tif (fl > 1e-6) { fx = f.x / fl; fz = f.z / fl; }\n"
"\t\tconst double rx = fz, rz = -fx;             // right = forward rotated -90 deg on XZ\n"
"\t\tcc->SetMove(Vector3((rx * m.x + fx * m.y) * s, 0, (rz * m.x + fz * m.y) * s));\n"
"\t\tif (Input::Pressed(\"Jump\")) cc->Jump(jumpSpeed);\n"
"\t}\n"
"\tvoid Destroy() override {}\n"
"\tvoid FixedUpdate() override {}\n"
"\tvoid Pause() override {}\n"
"\tvoid Reset() override {}\n"
"};\n"
"\n"
"#include \"" << name << ".gen.inc\"   // nukegen: reflection registration (NukeReflectInit_" << name << ")\n"
"\n"
"// ---- the module ------------------------------------------------------------------------\n"
"class " << name << "Module : public NUKEModule\n"
"{\n"
"public:\n"
"\t" << name << "Module()\n"
"\t{\n"
"\t\tstrcpy(title, \"" << name << "\");\n"
"\t\tstrcpy(author, \"Luastris\");\n"
"\t\tstrcpy(version, \"0.1\");\n"
"\t\tstrcpy(description, \"Game module\");\n"
"\t}\n"
"\tvoid OnLoad() override\n"
"\t{\n"
"\t\tNukeReflectInit_" << name << "();   // register this module's component types\n"
"\t\tstd::cout << \"[" << name << "]\tloaded\" << std::endl;\n"
"\t}\n"
"\tvoid Run(AppInstance*) override {}\n"
"\tvoid Shutdown() override { stopped = true; }\n"
"\tbool HasSettings() override { return false; }\n"
"\tvoid Settings() override {}\n"
"};\n"
"\n"
"// Exported under the unmangled symbol \"plugin\" — the loader imports it via boost::dll.\n"
"extern \"C\" __declspec(dllexport) " << name << "Module plugin;\n"
<< name << "Module plugin;\n";
	}

	{
		bfs::ofstream f(src / "README.md", std::ios::binary);
		f <<
"# " << name << " — C++ game module\n\n"
"- **Build & reload**: Project > Build & Reload Game Modules (no editor restart; the DLL\n"
"  hot-swaps, world components survive as placeholders through the swap).\n"
"- **Command line**: `cmake -S . -B build && cmake --build build --config Debug` — the DLL\n"
"  deploys to `<project>/modules/`.\n"
"- **Debugging**: attach VS to `NukeEngine-Editor.exe` / `NukePlayer.exe`; the PDB sits next\n"
"  to the DLL. To launch under the debugger, set the exe as the startup command with the\n"
"  project path as its argument.\n"
"- **Perf model**: game SYSTEMS over per-atom logic — flat data + `Jobs::ParallelFor` across\n"
"  cores; atoms/sprites are the presentation layer. Engine calls are direct C++ (no\n"
"  reflection tax); reflection tags only make things visible to the editor/scripts/mods.\n"
"- **Engine root**: `NUKE_ENGINE_ROOT` in the CMake cache points at the engine repo.\n";
	}

	std::cout << "[gamemodule]\tscaffolded '" << name << "' at " << src.string()
	          << " — Project > Build & Reload Game Modules to build it" << std::endl;
}

// ---- discovery -----------------------------------------------------------------------------

// Pull <project>/modules DLLs into the shared pool. A module seen for the FIRST time is
// auto-enabled and added to the project's plugin list (a game module is wanted by
// definition); ones already in the list follow it (the Plugins window toggle persists).
void EditorUI::DiscoverProjectModules()
{
	boost::system::error_code ec;
	const bfs::path dir = bfs::path(projectDir) / "modules";
	if (!bfs::exists(dir, ec)) return;
	nuke::DiscoverModulesIn(dir.string());

	const std::string prefix = bfs::absolute(dir, ec).generic_string();
	for (auto& m : nuke::GetModules())
	{
		if (!m || m->phase() == nuke::PHASE_BOOT) continue;
		boost::system::error_code ec2;
		const std::string mp = bfs::absolute(bfs::path(m->modulePath), ec2).generic_string();
		if (mp.rfind(prefix, 0) != 0) continue;   // not a project-local module
		const bool listed = std::find(enabledPlugins.begin(), enabledPlugins.end(), m->moduleFile) != enabledPlugins.end();
		if (!listed && pluginListLoaded)
		{
			// Diagnostic: this is the only boot-time single push into the plugin list — name
			// the module loudly (a corrupted pool entry once smuggled binary garbage into the
			// saved .nuproj here).
			std::cout << "[editor]\t\tproject module first sight: file='" << m->moduleFile
			          << "' path='" << m->modulePath << "' title='" << m->title << "'" << std::endl;
			enabledPlugins.push_back(m->moduleFile);   // first sight: wanted by definition
			SaveProject();
		}
		if (!m->loaded && (listed || pluginListLoaded))
			nuke::EnablePlugin(m.get());
	}
}

// ---- build + reload cycle --------------------------------------------------------------------

void EditorUI::BuildGameModules()
{
	AppInstance* app = AppInstance::GetSingleton();
	if (app->playState != 0)
	{
		std::cout << "[gamemodule]\tBuild & Reload refused while playing — stop PIE first" << std::endl;
		return;
	}
	boost::system::error_code ec;
	const bfs::path proj   = projectDir;
	const bfs::path srcDir = proj / "source";
	if (!bfs::exists(srcDir / "CMakeLists.txt", ec) && !bfs::exists(srcDir, ec))
	{
		std::cout << "[gamemodule]\tno <project>/source — create one with Project > New C++ Game Module" << std::endl;
		return;
	}
	// source/ may hold several modules without a top-level CMakeLists: synthesize one that
	// add_subdirectory()s every child with a CMakeLists (kept up to date on every build).
	if (!bfs::exists(srcDir / "CMakeLists.txt", ec) || true)
	{
		bfs::ofstream f(srcDir / "CMakeLists.txt", std::ios::binary);
		f << "# AUTO-GENERATED by the editor (Build & Reload Game Modules) — add_subdirectory per module.\n"
		     "cmake_minimum_required(VERSION 3.20)\nproject(GameModules)\n";
		for (bfs::directory_iterator it(srcDir, ec), end; it != end && !ec; it.increment(ec))
			if (bfs::is_directory(it->path()) && bfs::exists(it->path() / "CMakeLists.txt"))
				f << "add_subdirectory(\"" << it->path().filename().string() << "\")\n";
	}

	// UNLOCK: the project DLLs must leave the pool before the build can overwrite them.
	// (Also drop the plugin window's selection — a held shared_ptr keeps the file locked.)
	selectedPlugin = nullptr; selectedPluginIndex = -1;
	std::set<std::string> unloaded;
	{
		const std::string prefix = bfs::absolute(proj / "modules", ec).generic_string();
		std::vector<std::string> files;
		for (auto& m : nuke::GetModules())
		{
			if (!m) continue;
			boost::system::error_code ec2;
			const std::string mp = bfs::absolute(bfs::path(m->modulePath), ec2).generic_string();
			if (mp.rfind(prefix, 0) == 0) files.push_back(m->moduleFile);
		}
		for (const std::string& f : files)
			if (nuke::UnloadModuleDll(f)) unloaded.insert(f);
	}

#ifdef _DEBUG
	const char* cfg = "Debug";     // the module must match the RUNNING host's CRT/engine lib
#else
	const char* cfg = "Release";
#endif
	StatusBar::Set("gmbuild", "Game modules: building...", StatusBar::kIndeterminate);
	const std::string srcStr = srcDir.string();
	nuke::Jobs::Schedule([this, srcStr, cfg]()
	{
		auto run = [&](const std::string& cmdLine) -> bool
		{
			SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
			HANDLE rd = NULL, wr = NULL;
			CreatePipe(&rd, &wr, &sa, 0);
			SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
			STARTUPINFOA si = {}; si.cb = sizeof(si);
			si.dwFlags = STARTF_USESTDHANDLES;
			si.hStdOutput = wr; si.hStdError = wr;
			PROCESS_INFORMATION pi = {};
			std::string mcmd = cmdLine;
			if (!CreateProcessA(NULL, &mcmd[0], NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
			{
				CloseHandle(rd); CloseHandle(wr);
				std::cout << "[gamemodule]\tcan't start: " << cmdLine << std::endl;
				return false;
			}
			CloseHandle(wr);
			std::string carry; char buf[4096]; DWORD got = 0;
			while (ReadFile(rd, buf, sizeof(buf), &got, NULL) && got > 0)
			{
				carry.append(buf, got);
				size_t nl;
				while ((nl = carry.find('\n')) != std::string::npos)
				{
					std::string line = carry.substr(0, nl);
					carry.erase(0, nl + 1);
					if (!line.empty() && line.back() == '\r') line.pop_back();
					if (!line.empty()) std::cout << "[gamemodule]\t" << line << std::endl;
				}
			}
			CloseHandle(rd);
			WaitForSingleObject(pi.hProcess, INFINITE);
			DWORD code = 1;
			GetExitCodeProcess(pi.hProcess, &code);
			CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
			return code == 0;
		};

		const std::string bld = srcStr + "/build";
		bool ok = run("cmake -S \"" + srcStr + "\" -B \"" + bld + "\"");
		if (ok) ok = run("cmake --build \"" + bld + "\" --config " + cfg + " --parallel");

		nuke::Jobs::RunOnMain([this, ok]()
		{
			StatusBar::Remove("gmbuild");
			std::cout << "[gamemodule]\tbuild " << (ok ? "OK" : "FAILED") << std::endl;
			// Re-discover + re-enable the fresh DLLs either way (a failed build keeps the
			// old DLLs on disk — reloading them restores the session).
			DiscoverProjectModules();
		});
	});
}
