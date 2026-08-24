// C++ game modules: scaffold, discovery and the build-and-reload cycle for the
// NUKEModule DLLs in <project>/source -> <project>/modules.
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
#else
#include <cstdio>     // popen/pclose (module build spawn)
#include <sys/wait.h> // WIFEXITED/WEXITSTATUS
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

	// Engine repo root from the RUNNING editor (RunRoot, not the CWD); overridable through
	// the NUKE_ENGINE_ROOT cache entry. A staged dist carries the kit beside the exe
	// (stage_release -Sdk): the generated CMakeLists detects that layout.
	bfs::path engineRoot = nuke::RunRoot().parent_path().parent_path().parent_path();
	if (bfs::exists(nuke::RunRoot() / "sdk" / "include", ec))
		engineRoot = nuke::RunRoot() / "sdk";

	{
		bfs::ofstream f(src / "CMakeLists.txt", std::ios::binary);
		f <<
"cmake_minimum_required(VERSION 3.20)\n"
"\n"
"# The engine root: the REPO checkout (headers under NukeEngine/include, libs under\n"
"# NukeEngine/x64/<Config>) or a staged SDK (include/, lib/<Config>, bin/). Set at scaffold\n"
"# time; override in the cache when the project builds on another machine:\n"
"#   cmake -DNUKE_ENGINE_ROOT=...\n"
"set(NUKE_ENGINE_ROOT \"" << engineRoot.generic_string() << "\" CACHE PATH \"NukeEngine repo root or SDK dir\")\n"
"\n"
"# Dependencies come from vcpkg. A machine with a classic install (the engine dev setup)\n"
"# uses it directly — instant. Otherwise the vcpkg.json manifest beside this file takes\n"
"# over: the toolchain installs the engine's public dependencies into the build dir at the\n"
"# FIRST configure (long once, cached after). The toolchain must be chosen BEFORE project().\n"
"if(WIN32)\n"
"    set(NUKE_TRIPLET x64-windows)\n"
"    set(NUKE_CLASSIC_TRIPLETS x64-windows)\n"
"elseif(APPLE)\n"
"    if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL \"arm64\")\n"
"        set(NUKE_TRIPLET arm64-osx)\n"
"    else()\n"
"        set(NUKE_TRIPLET x64-osx)\n"
"    endif()\n"
"    # Engine dev machines install UNIVERSAL packages (overlay triplet) — probe those first.\n"
"    set(NUKE_CLASSIC_TRIPLETS universal-osx ${NUKE_TRIPLET})\n"
"else()\n"
"    set(NUKE_TRIPLET x64-linux)\n"
"    set(NUKE_CLASSIC_TRIPLETS x64-linux)\n"
"endif()\n"
"foreach(t ${NUKE_CLASSIC_TRIPLETS})\n"
"    if(NOT DEFINED NUKE_VCPKG_INC AND EXISTS \"$ENV{VCPKG_ROOT}/installed/${t}/include/boost\")\n"
"        set(NUKE_VCPKG_INC \"$ENV{VCPKG_ROOT}/installed/${t}/include\")\n"
"        set(NUKE_VCPKG_LIB \"$ENV{VCPKG_ROOT}/installed/${t}/$<$<CONFIG:Debug>:debug/>lib\")\n"
"    endif()\n"
"endforeach()\n"
"if(DEFINED NUKE_VCPKG_INC)\n"
"    # classic install found — resolved above\n"
"elseif(DEFINED ENV{VCPKG_ROOT})\n"
"    # Safe standalone AND as a subdirectory: the parent may have chosen the toolchain already.\n"
"    if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)\n"
"        set(CMAKE_TOOLCHAIN_FILE \"$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake\" CACHE FILEPATH \"\")\n"
"    endif()\n"
"    set(NUKE_VCPKG_INC \"${CMAKE_BINARY_DIR}/vcpkg_installed/${NUKE_TRIPLET}/include\")\n"
"    set(NUKE_VCPKG_LIB \"${CMAKE_BINARY_DIR}/vcpkg_installed/${NUKE_TRIPLET}/$<$<CONFIG:Debug>:debug/>lib\")\n"
"else()\n"
"    message(FATAL_ERROR \"Set VCPKG_ROOT (https://github.com/microsoft/vcpkg) — the engine's public headers need boost/nlohmann/glm.\")\n"
"endif()\n"
"\n"
"project(" << name << " CXX)\n"
"\n"
"set(CMAKE_CXX_STANDARD 20)\n"
"set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
"# Single-config generators (Makefiles/Ninja): $<CONFIG> resolves from CMAKE_BUILD_TYPE —\n"
"# unset it collapses the engine lib dir to nothing. Debug matches the dev editor.\n"
"if(NOT WIN32 AND NOT CMAKE_BUILD_TYPE)\n"
"    set(CMAKE_BUILD_TYPE Debug)\n"
"endif()\n"
"cmake_policy(SET CMP0091 NEW)\n"
"set(CMAKE_MSVC_RUNTIME_LIBRARY \"MultiThreaded$<$<CONFIG:Debug>:Debug>DLL\")\n"
"# vcpkg's MSBuild integration picks debug vs release vcpkg libs from <UseDebugLibraries>;\n"
"# CMake omits it by default and Debug builds then SILENTLY link release vcpkg libs\n"
"# (mixed CRTs -> crashes in boost/std objects). Emit it per config.\n"
"set(CMAKE_VS_USE_DEBUG_LIBRARIES \"$<CONFIG:Debug>\")\n"
"\n"
"# Repo checkout vs staged SDK — same content, two layouts.\n"
"# Run-dir subfolder + host-tool suffix (matches NukeEngine/cmake/NukeRunDir.cmake).\n"
"if(WIN32)\n"
"    set(NUKE_RUN_SUBDIR x64)\n"
"    set(NUKE_HOSTTOOL_SUFFIX .exe)\n"
"elseif(APPLE)\n"
"    set(NUKE_RUN_SUBDIR macos)\n"
"    set(NUKE_HOSTTOOL_SUFFIX \"\")\n"
"else()\n"
"    set(NUKE_RUN_SUBDIR linux)\n"
"    set(NUKE_HOSTTOOL_SUFFIX \"\")\n"
"endif()\n"
"if(EXISTS \"${NUKE_ENGINE_ROOT}/NukeEngine/include\")\n"
"    set(NUKE_INC \"${NUKE_ENGINE_ROOT}/NukeEngine/include\")\n"
"    set(NUKE_LIB \"${NUKE_ENGINE_ROOT}/NukeEngine/${NUKE_RUN_SUBDIR}/$<CONFIG>\")\n"
"    set(NUKE_GEN \"${NUKE_ENGINE_ROOT}/NukeUtils/bin/NukeGen${NUKE_HOSTTOOL_SUFFIX}\")\n"
"    # Typed cross-module wrappers (#include <nukesdk/NukeWater.sdk.h>): a sibling dir in the\n"
"    # repo, already inside include/ in a staged SDK.\n"
"    set(NUKE_SDKINC \"${NUKE_ENGINE_ROOT}/NukeUtils/sdk\")\n"
"else()\n"
"    set(NUKE_INC \"${NUKE_ENGINE_ROOT}/include\")\n"
"    set(NUKE_LIB \"${NUKE_ENGINE_ROOT}/lib/$<CONFIG>\")\n"
"    set(NUKE_GEN \"${NUKE_ENGINE_ROOT}/bin/NukeGen${NUKE_HOSTTOOL_SUFFIX}\")\n"
"    set(NUKE_SDKINC \"\")\n"
"endif()\n"
"\n"
"# Reflection prebuild (NukeGen module mode): scans this module's sources for NUKE_CLASS +\n"
"# [[nuke::prop/func]] and generates " << name << ".gen.inc (#included in-TU; call\n"
"# NukeReflectInit_" << name << "() from OnLoad). NukeGen is a standalone native exe — no Python.\n"
"add_custom_command(\n"
"    OUTPUT \"${CMAKE_CURRENT_SOURCE_DIR}/" << name << ".gen.inc\"\n"
"    COMMAND \"${NUKE_GEN}\"\n"
"            --include \"${CMAKE_CURRENT_SOURCE_DIR}\" --scan-cpp --no-includes\n"
"            --out \"${CMAKE_CURRENT_SOURCE_DIR}/" << name << ".gen.inc\"\n"
"            --init NukeReflectInit_" << name << "\n"
"    DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/" << name << ".cpp\" \"${NUKE_GEN}\"\n"
"    COMMENT \"nukegen: " << name << " reflection\")\n"
"\n"
"add_library(" << name << " SHARED\n"
"    " << name << ".cpp\n"
"    \"${CMAKE_CURRENT_SOURCE_DIR}/" << name << ".gen.inc\"\n"
")\n"
"set_source_files_properties(\"${CMAKE_CURRENT_SOURCE_DIR}/" << name << ".gen.inc\" PROPERTIES HEADER_FILE_ONLY ON)\n"
"\n"
"target_compile_definitions(" << name << " PRIVATE\n"
"    _USE_MATH_DEFINES GLM_ENABLE_EXPERIMENTAL\n"
")\n"
"if(WIN32)\n"
"    target_compile_definitions(" << name << " PRIVATE\n"
"        WIN32 _WINDOWS NOMINMAX _CRT_SECURE_NO_WARNINGS BOOST_ALL_DYN_LINK)\n"
"else()\n"
"    target_compile_options(" << name << " PRIVATE -Wno-c++11-narrowing -Wno-unknown-attributes)\n"
"    # Loader matches modules by file stem; the engine dylib resolves from the build-time\n"
"    # lib dir (this machine) or the run dir above the project's modules/.\n"
"    set_target_properties(" << name << " PROPERTIES\n"
"        PREFIX \"\"\n"
"        BUILD_RPATH \"${NUKE_LIB};@loader_path/..;$ORIGIN/..\")\n"
"endif()\n"
"\n"
"target_include_directories(" << name << " PRIVATE\n"
"    ${NUKE_INC}/..\n"
"    ${NUKE_INC}\n"
"    ${NUKE_SDKINC}\n"
"    ${NUKE_VCPKG_INC}\n"
")\n"
"\n"
"target_link_directories(" << name << " PRIVATE\n"
"    ${NUKE_LIB}\n"
"    ${NUKE_VCPKG_LIB}\n"
")\n"
"\n"
"target_link_libraries(" << name << " PRIVATE NukeEngine)\n"
"\n"
"# The module lands in <project>/modules/ — the editor scans it into the plugin pool.\n"
"add_custom_command(TARGET " << name << " POST_BUILD\n"
"    COMMAND ${CMAKE_COMMAND} -E make_directory \"${CMAKE_CURRENT_SOURCE_DIR}/../../modules\"\n"
"    COMMAND ${CMAKE_COMMAND} -E copy_if_different\n"
"            \"$<TARGET_FILE:" << name << ">\" \"${CMAKE_CURRENT_SOURCE_DIR}/../../modules/\"\n"
"    COMMENT \"Deploying " << name << " to the project's modules/\")\n";
	}

	// The engine's PUBLIC dependencies — what its headers expose to anyone compiling against
	// them. The manifest only fires on machines without a classic vcpkg install (see above),
	// where the toolchain auto-installs these at first configure.
	if (!bfs::exists(src / "vcpkg.json", ec))
	{
		bfs::ofstream f(src / "vcpkg.json", std::ios::binary);
		f <<
"{\n"
"  \"name\": \"" << [&]{ std::string l = name; for (char& c : l) c = (char)tolower((unsigned char)c); return l; }() << "\",\n"
"  \"version-string\": \"0.1\",\n"
"  \"dependencies\": [ \"boost\", \"nlohmann-json\", \"glm\" ]\n"
"}\n";
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
"// BOOST_SYMBOL_EXPORT is the portable dllexport (__declspec on Windows, visibility elsewhere).\n"
"extern \"C\" BOOST_SYMBOL_EXPORT " << name << "Module plugin;\n"
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

void EditorUI::DiscoverProjectModules()
{
	boost::system::error_code ec;
	const bfs::path dir = bfs::path(projectDir) / "modules";
	// A module the project HAS sources for but no built DLL for is invisible everywhere its
	// components should appear (Add Component, prop pickers, worlds that reference it) — and
	// silently so. Build it once per session instead, exactly like the script backends build
	// themselves; a failing build then reports through the normal build log.
	{
		static std::string autoBuilt;
		const bfs::path srcDir = bfs::path(projectDir) / "source";
		bool missing = false;
		if (bfs::exists(srcDir, ec))
			for (bfs::directory_iterator it(srcDir, ec), end; it != end && !ec; it.increment(ec))
			{
				if (!bfs::is_directory(it->path()) || !bfs::exists(it->path() / "CMakeLists.txt")) continue;
				const std::string name = it->path().filename().string();
#ifdef _WIN32
				const char* modExt = ".dll";
#elif defined(__APPLE__)
				const char* modExt = ".dylib";
#else
				const char* modExt = ".so";
#endif
				if (!bfs::exists(dir / (name + modExt), ec)) { missing = true; break; }
			}
		// A DLL built against an older engine ABI is refused by the loader — same end result as
		// a missing one (no components anywhere), so treat it the same and rebuild.
		if (!missing)
		{
			const std::string prefix = bfs::absolute(dir, ec).generic_string();
			for (const std::string& r : nuke::RefusedModules())
				if (bfs::absolute(bfs::path(r), ec).generic_string().rfind(prefix, 0) == 0) { missing = true; break; }
		}
		if (missing && autoBuilt != projectDir)
		{
			autoBuilt = projectDir;
			std::cout << "[gamemodule]	a module in <project>/source has no built DLL in "
			          << "<project>/modules — building it now" << std::endl;
			BuildGameModules();          // async; re-enters this function when it finishes
			return;
		}
	}
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

void EditorUI::BuildGameModules() { BuildGameModules(nullptr, nullptr); }

void EditorUI::BuildGameModules(const char* config, std::function<void(bool)> onDone)
{
	AppInstance* app = AppInstance::GetSingleton();
	if (app->playState != 0)
	{
		std::cout << "[gamemodule]\tBuild & Reload refused while playing — stop PIE first" << std::endl;
		if (onDone) onDone(false);
		return;
	}
	boost::system::error_code ec;
	const bfs::path proj   = projectDir;
	const bfs::path srcDir = proj / "source";
	if (!bfs::exists(srcDir / "CMakeLists.txt", ec) && !bfs::exists(srcDir, ec))
	{
		std::cout << "[gamemodule]\tno <project>/source — create one with Project > New C++ Game Module" << std::endl;
		if (onDone) onDone(false);
		return;
	}
	// Regenerate the top-level CMakeLists every build: source/ may gain modules at any time.
	// The vcpkg manifest/toolchain must be picked HERE, before the top-level project() — a
	// subdirectory is too late for either.
	if (!bfs::exists(srcDir / "CMakeLists.txt", ec) || true)
	{
		bfs::ofstream f(srcDir / "CMakeLists.txt", std::ios::binary);
		f << "# AUTO-GENERATED by the editor (Build & Reload Game Modules) — add_subdirectory per module.\n"
		     "cmake_minimum_required(VERSION 3.20)\n"
		     "# A classic vcpkg install (the engine dev setup) beats the manifest — instant, no\n"
		     "# from-source world rebuild. Probe the triplets the engine builds with (universal\n"
		     "# first on macOS); the manifest toolchain engages only when none is present.\n"
		     "if(WIN32)\n"
		     "    set(NUKE_CLASSIC_TRIPLETS x64-windows)\n"
		     "elseif(APPLE)\n"
		     "    set(NUKE_CLASSIC_TRIPLETS universal-osx arm64-osx x64-osx)\n"
		     "else()\n"
		     "    set(NUKE_CLASSIC_TRIPLETS x64-linux)\n"
		     "endif()\n"
		     "set(NUKE_CLASSIC FALSE)\n"
		     "foreach(t ${NUKE_CLASSIC_TRIPLETS})\n"
		     "    if(EXISTS \"$ENV{VCPKG_ROOT}/installed/${t}/include/boost\")\n"
		     "        set(NUKE_CLASSIC TRUE)\n"
		     "    endif()\n"
		     "endforeach()\n"
		     "if(NOT NUKE_CLASSIC AND DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)\n"
		     "    set(CMAKE_TOOLCHAIN_FILE \"$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake\" CACHE FILEPATH \"\")\n"
		     "endif()\n"
		     "project(GameModules)\n";
		for (bfs::directory_iterator it(srcDir, ec), end; it != end && !ec; it.increment(ec))
			if (bfs::is_directory(it->path()) && bfs::exists(it->path() / "CMakeLists.txt"))
				f << "add_subdirectory(\"" << it->path().filename().string() << "\")\n";
	}
	// Manifest for the toolchain path: the engine's PUBLIC dependencies, installed into the
	// build dir at first configure on machines without a classic vcpkg install.
	if (!bfs::exists(srcDir / "vcpkg.json", ec))
	{
		bfs::ofstream f(srcDir / "vcpkg.json", std::ios::binary);
		f << "{\n  \"name\": \"game-modules\",\n  \"version-string\": \"0.1\",\n"
		     "  \"dependencies\": [ \"boost\", \"nlohmann-json\", \"glm\" ]\n}\n";
	}

	// The project DLLs must leave the pool before the build can overwrite them — including the
	// plugin window's selection, whose held shared_ptr keeps the file locked.
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
	if (config) cfg = config;      // packaging builds Release regardless of the editor's config
	StatusBar::Set("gmbuild", std::string("Game modules: building ") + cfg + "...", StatusBar::kIndeterminate);
	const std::string srcStr = srcDir.string();
	nuke::Jobs::Schedule([this, srcStr, cfg, onDone]()
	{
		auto run = [&](const std::string& cmdLine) -> bool
		{
#ifndef _WIN32
			FILE* p = popen((cmdLine + " 2>&1").c_str(), "r");
			if (!p) { std::cout << "[gamemodule]\tcan't start: " << cmdLine << std::endl; return false; }
			std::string carry; char buf[4096]; size_t got;
			while ((got = fread(buf, 1, sizeof(buf), p)) > 0)
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
			const int st = pclose(p);
			return WIFEXITED(st) && WEXITSTATUS(st) == 0;
#else
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
#endif   // _WIN32
		};

		const std::string bld = srcStr + "/build";
		// The RUNNING editor's engine root overrides the scaffold-baked one: the module must
		// build against the engine that will load it. Passed only when this editor sits in a
		// repo/SDK layout; otherwise the project's cached value stands.
		std::string rootArg;
		{
			boost::system::error_code rec;
			const bfs::path repoRoot = nuke::RunRoot().parent_path().parent_path().parent_path();
			const bfs::path sdkRoot  = nuke::RunRoot() / "sdk";
			if (bfs::exists(repoRoot / "NukeEngine" / "include", rec))
				rootArg = " -DNUKE_ENGINE_ROOT=\"" + repoRoot.generic_string() + "\"";
			else if (bfs::exists(sdkRoot / "include", rec))
				rootArg = " -DNUKE_ENGINE_ROOT=\"" + sdkRoot.generic_string() + "\"";
		}
#ifndef _WIN32
		// Single-config generators (Makefiles/Ninja): $<CONFIG> in the scaffold resolves from
		// CMAKE_BUILD_TYPE, which nobody sets by default — the engine lib dir then loses its
		// /Debug tail and the link fails. Pin it to the running host's config (VS ignores it).
		rootArg += std::string(" -DCMAKE_BUILD_TYPE=") + cfg;
#endif
		bool ok = run("cmake -S \"" + srcStr + "\" -B \"" + bld + "\"" + rootArg);
		if (ok) ok = run("cmake --build \"" + bld + "\" --config " + cfg + " --parallel");

		nuke::Jobs::RunOnMain([this, ok, onDone]()
		{
			StatusBar::Remove("gmbuild");
			std::cout << "[gamemodule]\tbuild " << (ok ? "OK" : "FAILED") << std::endl;
			// Re-discover either way: a failed build leaves the old DLLs to reload.
			DiscoverProjectModules();
			if (onDone) onDone(ok);
		});
	});
}
