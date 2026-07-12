#pragma once
// External code/text editors (Preferences). Shared between the neutral editor code and the
// platform layer (platform_win32/other) without dragging the full editorui.h in.
#include <string>
#include <vector>

// Launch templates: {file}, {line}, {project} (a .csproj/.sln) and {projectDir} expand at
// launch. `argsProj` is used when a PROJECT CONTEXT resolves for the file (a C# script's
// generated GameScripts.csproj) — the IDE opens the whole project with IntelliSense, not a
// lone file; `args` is the plain fallback.
struct ExtEditor { std::string name, exe, args, argsProj; };

std::vector<ExtEditor> EditorDetectExternalEditors();   // scan the machine's standard install spots
bool EditorLaunchDetached(const std::string& exe, const std::string& args);   // fire-and-forget
// Is a process of this executable already running? Decides REUSE vs first launch: a
// running IDE gets the file-only arguments (devenv /Edit, VSCode -r, Rider forwarders all
// route into the live instance) instead of spawning a fresh one per opened file.
bool EditorProcessRunning(const std::string& exePath);
