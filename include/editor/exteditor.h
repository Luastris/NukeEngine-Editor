#pragma once
// External code/text editors (Preferences): shared by the neutral editor code and the platform layer.
#include <string>
#include <vector>

// Launch templates expand {file}, {line}, {project}, {projectDir}; `argsProj` is used when a
// project context resolves for the file, `args` is the plain fallback.
struct ExtEditor { std::string name, exe, args, argsProj; };

std::vector<ExtEditor> EditorDetectExternalEditors();   // scan the machine's standard install spots
bool EditorLaunchDetached(const std::string& exe, const std::string& args);   // fire-and-forget
// Is a process of this executable already running? Decides reuse (file-only args) vs first launch.
bool EditorProcessRunning(const std::string& exePath);
