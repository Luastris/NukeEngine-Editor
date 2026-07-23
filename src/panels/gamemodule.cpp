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
