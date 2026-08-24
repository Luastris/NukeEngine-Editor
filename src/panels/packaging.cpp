// packaging panel — EditorUI method definitions: Package Project (full release dist/),
// Package Mod (.numod overlay) and Package DLC.
#include <editor/editorui.h>
#include <API/Model/Package.h>
#include <API/Model/JsonDoc.h>   // CookDocFile: worlds/prefabs ship as binary documents
#include <API/Model/Texture.h>   // pre-v11 textures ship healed (v11)
#include <config.h>              // writableDir: the editor's warm shader/pipeline caches ship with the game
#include <API/Model/Jobs.h>
#include <API/Model/StatusBar.h>
#include <API/Model/World.h>    // MergeWorldLayers for the mod basis
#include <interface/Modular.h>  // PluginForType: which plugin a packed component type needs
#include <interface/Services.h> // the scripting backends answer for their own code
#include <service/iScript.h>
#include <nlohmann/json.hpp>
#include <functional>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <algorithm>
#include <set>
#include <sstream>
#include <cstring>
#include <cctype>
#ifdef _WIN32
#include <Windows.h>   // UpdateResource (game icon on the shipped exe)
#endif
#define STB_IMAGE_IMPLEMENTATION   // the editor's ONLY stb TU
#define STBI_ONLY_PNG
#include <stb_image.h>

namespace bfs = boost::filesystem;
using nuke::Package;

// Lowercased '/'-separated project-relative key.
static std::string RelKey(const bfs::path& p, const bfs::path& root)
{
	boost::system::error_code ec;
	std::string rel = bfs::relative(p, root, ec).generic_string();
	if (ec) return std::string();
	for (char& c : rel) c = (char)tolower((unsigned char)c);
	return rel;
}

// Does this project-relative path belong in a pak? `distPrefix` = the configured build dir
// when it sits inside the project, lowercased with a trailing '/'.
static bool PackWorthy(const std::string& relLower, bool forMod, const std::string& distPrefix)
{
	if (relLower.empty()) return false;
	auto starts = [&](const char* p) { return relLower.compare(0, strlen(p), p) == 0; };
	if (starts("dist/") || starts("mods/") || starts(".git")) return false;
	// managed/ is build output; the game pak takes GameScripts.dll explicitly via shipExtras.
	// A mod sweeping a stale copy in would shadow the game's assembly (mods read above the base).
	if (starts("managed/")) return false;
	if (starts(".modbasis.tmp")) return false;   // Package Mod scratch
	if (!distPrefix.empty() && relLower.compare(0, distPrefix.size(), distPrefix) == 0) return false;
	if (relLower == "editor_state.json" || relLower == ".nupak_base") return false;
	if (relLower == "mods.json") return false;
	if (relLower == "mod.json") return false;   // generated at pack time, never collected as data
	auto ends = [&](const char* s) { size_t n = strlen(s); return relLower.size() >= n && relLower.compare(relLower.size() - n, n, s) == 0; };
	if (ends(".log") || ends(".err") || ends(".bak") || ends(".nupak") || ends(".numod") || ends(".pdb") || ends(".tmp")) return false;
	if (relLower.find(".cache/") != std::string::npos) return false;   // materialization caches
	if (ends(".nuproj")) return false;   // PackageProject adds the active one as "game.nuproj"
	return true;
}

// The configured build dir as a project-relative lowercase prefix ("" when it lives
// OUTSIDE the project — nothing to exclude from the pak then).
static std::string DistPrefix(const std::string& projectDir, const std::string& distDir)
{
	boost::system::error_code ec;
	bfs::path rel = bfs::relative(bfs::path(distDir), bfs::path(projectDir), ec);
	std::string r = ec ? std::string() : rel.generic_string();
	if (r.empty() || r == "." || r.compare(0, 2, "..") == 0) return std::string();
	for (char& c : r) c = (char)tolower((unsigned char)c);
	return r + "/";
}

// Walk the project and collect (projectRelative, diskPath) pairs worth packing.
static std::vector<std::pair<std::string, std::string>> CollectProject(const std::string& projectDir, bool forMod,
                                                                       const std::string& distPrefix = std::string())
{
	std::vector<std::pair<std::string, std::string>> out;
	boost::system::error_code ec;
	bfs::path root(projectDir);
	for (bfs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec))
	{
		if (ec) break;
		if (bfs::is_directory(it->path())) continue;
		std::string key = RelKey(it->path(), root);
		if (!PackWorthy(key, forMod, distPrefix)) continue;
		std::string rel = bfs::relative(it->path(), root, ec).generic_string();
		if (!ec) out.push_back({ rel, it->path().string() });
	}
	std::sort(out.begin(), out.end());
	return out;
}

static bool CopyOne(const bfs::path& from, const bfs::path& to)
{
	boost::system::error_code ec;
	if (!bfs::exists(from, ec)) return false;
	if (to.has_parent_path()) bfs::create_directories(to.parent_path(), ec);
	bfs::copy_file(from, to, bfs::copy_options::overwrite_existing, ec);
	return !ec;
}

static void CopyTree(const bfs::path& from, const bfs::path& to)
{
	boost::system::error_code ec;
	if (!bfs::exists(from, ec)) return;
	for (bfs::recursive_directory_iterator it(from, ec), end; it != end; it.increment(ec))
	{
		if (ec) break;
		if (bfs::is_directory(it->path())) continue;
		bfs::path rel = bfs::relative(it->path(), from, ec);
		if (!ec) CopyOne(it->path(), to / rel);
	}
}

// Standard .ico layout: ICONDIR + entries + image payloads.
#pragma pack(push, 2)
struct IcoDirEntry { uint8_t w, h, colors, reserved; uint16_t planes, bpp; uint32_t bytes, offset; };
struct GrpIconEntry { uint8_t w, h, colors, reserved; uint16_t planes, bpp; uint32_t bytes; uint16_t id; };
#pragma pack(pop)

#ifdef _WIN32
// Stamp the game's icon onto the shipped exe, replacing the player's icon group + images.
static bool StampExeIcon(const std::string& exePath, const std::string& icoPath)
{
	bfs::ifstream f(bfs::path(icoPath), std::ios::binary);
	if (!f) return false;
	std::string ico((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (ico.size() < 6) return false;
	uint16_t type = *(const uint16_t*)(ico.data() + 2);
	uint16_t count = *(const uint16_t*)(ico.data() + 4);
	if (type != 1 || count == 0 || ico.size() < 6 + count * sizeof(IcoDirEntry)) return false;

	HANDLE h = BeginUpdateResourceA(exePath.c_str(), FALSE);
	if (!h) return false;
	const IcoDirEntry* ent = (const IcoDirEntry*)(ico.data() + 6);
	std::string grp(6 + count * sizeof(GrpIconEntry), '\0');
	memcpy(&grp[0], ico.data(), 6);                     // reserved/type/count header is shared
	bool ok = true;
	for (uint16_t i = 0; i < count; ++i)
	{
		if ((uint64_t)ent[i].offset + ent[i].bytes > ico.size()) { ok = false; break; }
		ok &= UpdateResourceA(h, MAKEINTRESOURCEA(3) /*RT_ICON*/, MAKEINTRESOURCEA(i + 1),
		                      MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
		                      (void*)(ico.data() + ent[i].offset), ent[i].bytes) != 0;
		GrpIconEntry* g = (GrpIconEntry*)(&grp[6]) + i;
		g->w = ent[i].w; g->h = ent[i].h; g->colors = ent[i].colors; g->reserved = 0;
		g->planes = ent[i].planes; g->bpp = ent[i].bpp; g->bytes = ent[i].bytes; g->id = (uint16_t)(i + 1);
	}
	if (ok)
		ok = UpdateResourceA(h, MAKEINTRESOURCEA(14) /*RT_GROUP_ICON*/, MAKEINTRESOURCEA(1),
		                     MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
		                     (void*)grp.data(), (DWORD)grp.size()) != 0;
	return EndUpdateResourceA(h, ok ? FALSE : TRUE) != 0 && ok;
}
#endif

// Engine plugins a packed file set is built on. Each shape of dependency is read where it
// actually lives, and by whoever is entitled to read it:
//   * content — every reflected component "type" in a packed JSON asset maps back to the plugin
//               that registered it (engine built-ins map to "" and drop out). The format is the
//               engine's own, so the engine reads it;
//   * native  — a DLL the mod ships states what it links in its PE import table;
//   * scripts — the SCRIPTING BACKENDS answer. Nothing here knows Lua from C#: every registered
//               iScript service is handed the file and returns the modules its code needs.
// Detection proposes; a "modules" list already in mod.json is authoritative.
static std::vector<std::string> ModulesForFiles(const std::vector<std::pair<std::string, std::string>>& files)
{
	std::set<std::string> out;
	std::function<void(const nlohmann::json&)> walk = [&](const nlohmann::json& j)
	{
		if (j.is_object())
		{
			auto it = j.find("type");
			if (it != j.end() && it->is_string())
			{
				const char* dll = nuke::PluginForType(it->get<std::string>());
				if (dll && *dll) out.insert(bfs::path(dll).stem().string());
			}
			for (auto& kv : j.items()) walk(kv.value());
		}
		else if (j.is_array())
			for (const nlohmann::json& e : j) walk(e);
	};
	std::vector<nuke::iScript*> backends = nuke::GetServices<nuke::iScript>();

	for (const auto& fp : files)
	{
		// Native binaries name their links themselves (headers only, never the payload).
		for (const std::string& imp : nuke::ModuleImportsOf(fp.second)) out.insert(imp);

		// Engine-authored JSON content. PEEK first: a mesh or a texture must not be pulled into
		// memory just to learn it is not JSON.
		{
			bfs::ifstream f(bfs::path(fp.second), std::ios::binary);
			if (!f) continue;
			char head[16] = {};
			f.read(head, sizeof(head));
			const size_t n = (size_t)(f.gcount() > 0 ? f.gcount() : 0);
			size_t b = 0;
			while (b < n && (head[b] == ' ' || head[b] == '\t' || head[b] == '\r' || head[b] == '\n')) ++b;
			if (b < n && (head[b] == '{' || head[b] == '['))
			{
				f.clear(); f.seekg(0);
				std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
				nlohmann::json j = nlohmann::json::parse(data, nullptr, false);
				if (!j.is_discarded()) { walk(j); continue; }
			}
		}
		// Everything else is somebody's code: hand over the path, let its language read it.
		for (nuke::iScript* s : backends)
		{
			if (!s) continue;
			const int need = s->ModuleDeps(fp.second.c_str(), nullptr, 0);
			if (need <= 0) continue;
			std::vector<char> buf((size_t)need + 1, 0);
			s->ModuleDeps(fp.second.c_str(), buf.data(), need + 1);
			std::string all(buf.data());
			for (size_t p = 0; p < all.size(); )
			{
				size_t nl = all.find('\n', p);
				if (nl == std::string::npos) nl = all.size();
				std::string one = all.substr(p, nl - p);
				while (!one.empty() && (one.back() == '\r' || one.back() == ' ')) one.pop_back();
				if (!one.empty()) out.insert(one);
				p = nl + 1;
			}
		}
	}
	return std::vector<std::string>(out.begin(), out.end());
}

// Game-thread snapshot of every discovered module for the packaging workers: canonical name,
// binary path, service role, plugin-list state and its ship extras. Keyed by lowercase name.
std::map<std::string, EditorUI::PkgMod> EditorUI::SnapshotPkgMods()
{
	auto low = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
	boost::system::error_code ec;
	const std::string projLow = projectDir.empty() ? std::string()
	                          : low(bfs::weakly_canonical(bfs::path(projectDir), ec).generic_string());
	std::map<std::string, PkgMod> out;
	for (auto& m : nuke::GetModules())
	{
		if (!m) continue;
		PkgMod pm;
		pm.name       = nuke::ModuleName(m->moduleFile);
		pm.file       = m->modulePath;
		pm.provides   = m->provides() ? m->provides() : "";
		pm.shared     = m->sharedService();
		pm.editorTool = nuke::ModuleIsEditorTool(m.get());
		{
			boost::system::error_code pec;
			std::string mp = low(bfs::weakly_canonical(bfs::path(m->modulePath), pec).generic_string());
			pm.project = !projLow.empty() && mp.rfind(projLow, 0) == 0;
		}
		// Boot providers ride serviceChoices, not the plugin list — count them as wanted.
		pm.enabled = !pluginListLoaded || m->phase() == nuke::PHASE_BOOT;
		for (size_t i = 0; !pm.enabled && i < enabledPlugins.size(); ++i)
			pm.enabled = nuke::ModuleFileMatches(enabledPlugins[i], m->moduleFile);
		if (m->loaded)
		{
			m->shipExtras(projectDir.c_str(), pm.extraPak, pm.extraDist);
			if (m->sharedService() && std::string(m->provides()) == "scripting")
				pm.script = m->queryService();
		}
		out[low(pm.name)] = pm;
	}
	for (auto& kv : serviceChoices)
	{
		if (kv.second.empty()) continue;
		auto it = out.find(low(nuke::ModuleName(kv.second)));
		if (it != out.end()) it->second.service = kv.first;
	}
	return out;
}

// The manifest's authored force-ship list ("shipModules": [...]) — the escape hatch for a
// module the detection below cannot see (loaded purely by hand-written runtime logic).
static std::vector<std::string> ReadManifestShipModules(const std::string& projFile)
{
	std::vector<std::string> out;
	bfs::ifstream f{ bfs::path(projFile) };
	if (!f) return out;
	nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
	if (!j.is_discarded() && j.is_object() && j.contains("shipModules") && j["shipModules"].is_array())
		for (auto& m : j["shipModules"]) if (m.is_string()) out.push_back(m.get<std::string>());
	return out;
}

// The modules this dist ships: service providers, the project's game modules, plugins the
// shipped content/scripts actually use, the manifest's "shipModules", closed over binary
// imports. Enabled-but-unused plugins are dropped with a log.
static bool IsEditorOnlyModule(const bfs::path& dll);   // defined with the icon helpers below

static std::set<std::string> ComputeShipModules(
	const std::vector<std::pair<std::string, std::string>>& files,
	const std::map<std::string, EditorUI::PkgMod>& mods,
	const std::set<std::string>& cookClaimants,
	bool consoleOn, const std::vector<std::string>& manifestShip, bool quiet)
{
	auto low = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
	auto say = [&](const std::string& m) { if (!quiet) std::cout << "[Package]\t" << m << std::endl; };

	// Everything the packed content references.
	std::set<std::string> detected = cookClaimants;
	for (const std::string& d : ModulesForFiles(files)) detected.insert(low(d));
	// A scripting backend is used when a file its language owns ships (a .lua run by path
	// leaves no component trace) — the backend itself claims the file.
	for (auto& kv : mods)
		if (kv.second.script && !detected.count(kv.first))
			for (const auto& fp : files)
				if (((nuke::iScript*)kv.second.script)->PlatformOf(fp.second.c_str(), nullptr, 0) > 0)
					{ detected.insert(kv.first); break; }
	// Modules that registered no reflected types leave no trace any walk could find.
	std::set<std::string> hasTypes;
	for (const auto& tp : nuke::PluginOwnedTypes()) hasTypes.insert(low(tp.second));

	std::set<std::string> ship;   // canonical names
	std::vector<std::string> dropped;
	auto add = [&](const std::string& lowName, const std::string& why) -> bool
	{
		auto it = mods.find(lowName);
		if (it == mods.end() || it->second.editorTool) return false;
		if (ship.count(it->second.name)) return true;
		// The UI-bearing editor companions (importing the editor's ImGui dll) never ship —
		// refusing them HERE keeps them out of the build target list too.
		if (!it->second.file.empty() && IsEditorOnlyModule(bfs::path(it->second.file)))
			{ say("module '" + it->second.name + "' is editor-only (imports the editor UI) — not shipped"); return false; }
		ship.insert(it->second.name);
		say("module '" + it->second.name + "': " + why);
		return true;
	};
	// Services with an explicit project choice; one with NO recorded choice (legacy manifests
	// wrote only some) falls back to its enabled provider below.
	std::set<std::string> chosenSvc;
	for (auto& kv : mods) if (!kv.second.service.empty()) chosenSvc.insert(kv.second.service);
	// The console needs A gui backend, whatever the plugin list says: the chosen provider,
	// else an enabled one, else any installed one.
	if (consoleOn)
	{
		std::string gui;
		for (auto& kv : mods) if (kv.second.service == "gui") gui = kv.first;
		if (gui.empty())
			for (auto& kv : mods)
				if (kv.second.provides == "gui" && kv.second.enabled && !kv.second.editorTool) gui = kv.first;
		if (gui.empty())
			for (auto& kv : mods)
				if (kv.second.provides == "gui" && !kv.second.editorTool) gui = kv.first;
		if (!gui.empty()) add(gui, "the dev console needs the GUI backend");
		else say("WARNING: the dev console is ON but no GUI backend module is installed — the console cannot draw");
	}
	for (auto& kv : mods)
	{
		const EditorUI::PkgMod& pm = kv.second;
		if (pm.editorTool) continue;
		// Exclusive non-gui services are the runtime's core (render/physics/audio): the chosen
		// provider ships; with no recorded choice the enabled one does. The "gui" service and
		// shared services (scripting backends) stay content-driven below.
		if (!pm.provides.empty() && pm.provides != "gui" && !pm.shared)
		{
			if (!pm.service.empty())
				add(kv.first, "the project's '" + pm.service + "' provider");
			else if (pm.enabled && !chosenSvc.count(pm.provides))
				add(kv.first, "the enabled '" + pm.provides + "' provider");
			continue;
		}
		if (!pm.service.empty() && pm.shared)
			{ add(kv.first, "the project's '" + pm.service + "' provider"); continue; }
		if (!pm.enabled)
		{
			if (detected.count(kv.first))
				say("WARNING: shipped content uses module '" + pm.name
				    + "' but the plugin is DISABLED — its components will load as placeholders");
			continue;
		}
		if (pm.project)                   { add(kv.first, "the project's own game module"); continue; }
		if (detected.count(kv.first))     { add(kv.first, "used by the shipped content"); continue; }
		if (!pm.extraPak.empty())         { add(kv.first, "ships project files of its own"); continue; }
		// A PLAIN plugin with no reflected types (an input provider) leaves no trace any walk
		// could see — enabled is its only signal. Service providers never take this path: the
		// gui service ships with the console or by an explicit shipModules entry.
		if (!hasTypes.count(kv.first) && pm.provides.empty())
			{ add(kv.first, "registers no reflected types (usage is undetectable) — shipped as enabled"); continue; }
		if (pm.provides == "gui" && !consoleOn)
			{ say("module '" + pm.name + "': the 'gui' provider ships only with the dev console (or via the manifest's shipModules when the game draws runtime GUI) — not shipped"); continue; }
		dropped.push_back(pm.name);
	}
	for (const std::string& m : manifestShip)
		add(low(nuke::ModuleName(m)), "forced by the manifest's shipModules");
	// Hard binary dependencies close the set.
	{
		std::vector<std::string> work(ship.begin(), ship.end());
		while (!work.empty())
		{
			auto it = mods.find(low(work.back()));
			work.pop_back();
			if (it == mods.end() || it->second.file.empty()) continue;
			for (const std::string& imp : nuke::ModuleImportsOf(it->second.file))
			{
				auto mi = mods.find(low(imp));
				if (mi == mods.end() || mi->second.editorTool) continue;
				if (ship.insert(mi->second.name).second)
				{
					say("module '" + mi->second.name + "': imported by '" + it->second.name + "'");
					work.push_back(mi->second.name);
				}
			}
		}
	}
	if (!dropped.empty())
	{
		std::string list;
		for (auto& d : dropped) list += (list.empty() ? "" : ", ") + d;
		say("unused by the shipped content, NOT shipped: " + list);
	}
	return ship;
}

// Does this file set bind its package to one platform? Whoever owns a file answers for it: every
// scripting backend is offered each file, and one that claims it says "any" or names the platform
// its code is bound to. Only what nobody claims falls back to the crude rule — a binary that is
// not somebody's runtime is machine code. Nothing here knows what a managed assembly is.
static bool PlatformLocked(const std::vector<std::pair<std::string, std::string>>& files)
{
	std::vector<nuke::iScript*> backends = nuke::GetServices<nuke::iScript>();
	auto ends = [](const std::string& s, const char* e)
	{ const size_t n = strlen(e); return s.size() > n && s.compare(s.size() - n, n, e) == 0; };
	for (const auto& fp : files)
	{
		bool claimed = false;
		for (nuke::iScript* s : backends)
		{
			if (!s) continue;
			const int need = s->PlatformOf(fp.second.c_str(), nullptr, 0);
			if (need <= 0) continue;
			std::vector<char> buf((size_t)need + 1, 0);
			s->PlatformOf(fp.second.c_str(), buf.data(), need + 1);
			claimed = true;
			if (std::string(buf.data()) != "any")
			{
				std::cout << "[Package]\t" << fp.first << " is platform-bound (" << buf.data()
				          << ") — the package is tagged for it" << std::endl;
				return true;
			}
		}
		if (claimed) continue;   // its owner said it travels
		// Nobody's runtime claimed it. A library extension is then machine code — a crude test,
		// but an honest one: the precise answers came from the owners above.
		std::string low = fp.first;
		for (char& c : low) c = (char)tolower((unsigned char)c);
		if (ends(low, ".dll") || ends(low, ".so") || ends(low, ".dylib"))
		{
			std::cout << "[Package]\t" << fp.first << " is native code — the package is platform-bound" << std::endl;
			return true;
		}
	}
	return false;
}

// ---- the cooker: only used content ships ----
// Dependency closure rooted at the manifest (startupWorld + "packInclude"); every string in
// an asset's JSON matching a known GUID or content-relative path pulls that file in and recurses.
struct CookCtx
{
	std::string projDir, contentDir;
	std::map<std::string, std::vector<std::string>> guidFiles;   // asset guid -> file(s)
	std::set<std::string> visited;                               // lowercase disk keys (walked)
	std::set<std::string> shipped;                               // walked AND claimed
	std::set<std::string> claimants;                             // lowercase module names whose cookContent claimed a shipped file
	std::vector<std::string> reached;                            // walked but UNSHIPPED files (script sources): detection input
	std::vector<std::string> queue;
	int missed = 0;
};

static std::string DiskKey(const bfs::path& p)
{
	boost::system::error_code ec;
	std::string k = bfs::weakly_canonical(p, ec).generic_string();
	if (ec) k = p.generic_string();
	for (char& c : k) c = (char)tolower((unsigned char)c);
	return k;
}

static void CookEnqueue(CookCtx& c, const bfs::path& file)
{
	boost::system::error_code ec;
	if (!bfs::exists(file, ec) || bfs::is_directory(file, ec)) return;
	// Only project files ship; engine-side paths stay out.
	bfs::path rel = bfs::relative(file, bfs::path(c.projDir), ec);
	if (ec || rel.empty() || rel.generic_string().compare(0, 2, "..") == 0) return;
	std::string key = DiskKey(file);
	if (c.visited.count(key)) return;
	c.visited.insert(key);
	c.queue.push_back(file.string());
}

static void CookScanJson(CookCtx& c, const nlohmann::json& j);

static void CookHandleString(CookCtx& c, const std::string& s)
{
	if (s.empty() || s.size() > 512) return;
	auto git = c.guidFiles.find(s);
	if (git != c.guidFiles.end())
	{
		for (const std::string& f : git->second) CookEnqueue(c, bfs::path(f));
		return;
	}
	if (s.find_first_of("\n{}") == std::string::npos)   // path candidate (content-relative)
	{
		boost::system::error_code ec;
		bfs::path cand = bfs::path(c.contentDir) / s;
		if (bfs::exists(cand, ec) && !bfs::is_directory(cand, ec)) { CookEnqueue(c, cand); return; }
	}
	// Nested JSON payloads stored as strings (effectsData, smJson).
	if (!s.empty() && (s[0] == '{' || s[0] == '['))
	{
		nlohmann::json nested = nlohmann::json::parse(s, nullptr, false);
		if (!nested.is_discarded()) CookScanJson(c, nested);
	}
}

static void CookScanJson(CookCtx& c, const nlohmann::json& j)
{
	if (j.is_string()) CookHandleString(c, j.get<std::string>());
	else if (j.is_array())  for (const auto& e : j) CookScanJson(c, e);
	else if (j.is_object()) for (auto it = j.begin(); it != j.end(); ++it) CookScanJson(c, it.value());
}

// Engine-native content types; `isJson` marks the kinds whose props get walked. Anything
// outside this list ships only if a loaded module claims it via cookContent().
static bool CookEngineType(const std::string& low, bool& isJson)
{
	auto ends = [&](const char* suf) { size_t n = strlen(suf); return low.size() > n && low.compare(low.size() - n, n, suf) == 0; };
	isJson = ends(".nuworld") || ends(".nuprefab") || ends(".numat") || ends(".nubonemap") || ends(".nuproj")
	      || ends(".nucursor");
	if (isJson) return true;
	return ends(".numesh") || ends(".nutex") || ends(".nuanim")
	    || ends(".ogg") || ends(".wav") || ends(".mp3") || ends(".flac")
	    || ends(".hlsl") || ends(".ico")
	    || ends(".png") || ends(".jpg") || ends(".jpeg") || ends(".tga") || ends(".bmp");
}

static void CookProcess(CookCtx& c, const std::string& file)
{
	bfs::ifstream f(bfs::path(file), std::ios::binary);
	if (!f) return;
	std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

	std::string low = file;
	for (char& ch : low) ch = (char)tolower((unsigned char)ch);
	boost::system::error_code ec;
	std::string rel = bfs::relative(bfs::path(file), bfs::path(c.contentDir), ec).generic_string();

	bool isJson = false;
	if (CookEngineType(low, isJson))
	{
		c.shipped.insert(DiskKey(bfs::path(file)));
		if (isJson)
		{
			nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
			if (!j.is_discarded()) CookScanJson(c, j);
			else ++c.missed;
		}
		return;
	}

	// A module's file type: the owning module claims it and reports what it uses (paths or
	// asset guids, both walk on). Only LOADED modules answer.
	std::vector<std::string> uses;
	bool claimed = false;
	for (auto& m : nuke::GetModules())
	{
		if (!m || !m->loaded) continue;
		uses.clear();
		if (m->cookContent(rel.c_str(), text.data(), (uint64_t)text.size(), uses))
		{
			claimed = true;
			c.shipped.insert(DiskKey(bfs::path(file)));
			// A claimed file is the claimant's content: shipping it proves the module is used.
			{
				std::string cn = nuke::ModuleName(m->moduleFile);
				for (char& ch : cn) ch = (char)tolower((unsigned char)ch);
				c.claimants.insert(cn);
			}
			for (const std::string& u : uses) CookHandleString(c, u);
			break;
		}
	}
	if (!claimed)
	{
		// Referenced by shipped content, ships nothing itself — still DETECTION input: a .cs
		// source is where module usage is visible (the compiled assembly bakes the SDK in).
		c.reached.push_back(file);
		std::cout << "[Package]\tunclaimed file type, not shipped: " << rel << std::endl;
	}
}

// The closure. Returns lowercase disk keys of every file that SHIPS. `outClaimants` (optional)
// receives the lowercase names of modules whose cookContent claimed shipped files.
static std::set<std::string> CookUsedFiles(const std::string& projDir, const std::string& contentDir,
                                           const std::string& manifestFile,
                                           const std::map<std::string, std::vector<std::string>>& guidFiles,
                                           std::set<std::string>* outClaimants = nullptr,
                                           std::vector<std::string>* outReached = nullptr)
{
	CookCtx c;
	c.projDir = projDir; c.contentDir = contentDir; c.guidFiles = guidFiles;
	CookEnqueue(c, bfs::path(manifestFile));   // roots: startupWorld, packInclude, gameIcon...
	// The manifest may sit outside the project dir — force-process it if CookEnqueue skipped it.
	if (c.queue.empty()) { c.visited.insert(DiskKey(manifestFile)); c.queue.push_back(manifestFile); }
	while (!c.queue.empty())
	{
		std::string f = c.queue.back();
		c.queue.pop_back();
		CookProcess(c, f);
	}
	if (outClaimants) *outClaimants = c.claimants;
	if (outReached) *outReached = c.reached;
	return c.shipped;
}

#ifdef __APPLE__
// macOS counterpart of StampExeIcon: build Contents/Resources/game.icns for the game bundle.
// A configured icon (.ico/.png/anything sips reads) goes through sips -> iconutil; without
// one the engine's stock logo.icns (editor bundle Resources) ships, so Finder never shows a
// blank app. Returns the CFBundleIconFile stem, or "" when no icon could be produced.
static std::string StampAppIconMac(const bfs::path& contentsDir, const std::string& iconSrc)
{
	boost::system::error_code ec;
	const bfs::path resDir = contentsDir / "Resources";
	bfs::create_directories(resDir, ec);
	auto runq = [](const std::string& cmd) -> bool
	{
		FILE* p = popen((cmd + " >/dev/null 2>&1").c_str(), "r");
		if (!p) return false;
		char b[256]; while (fread(b, 1, sizeof(b), p) > 0) {}
		const int st = pclose(p);
		return WIFEXITED(st) && WEXITSTATUS(st) == 0;
	};
	if (!iconSrc.empty() && bfs::exists(bfs::path(iconSrc), ec))
	{
		const bfs::path iconset = contentsDir / "game.iconset";
		bfs::remove_all(iconset, ec);
		bfs::create_directories(iconset, ec);
		bool ok = true;
		for (int sz : { 16, 32, 64, 128, 256, 512 })
			ok = runq("sips -s format png -z " + std::to_string(sz) + " " + std::to_string(sz)
			          + " \"" + iconSrc + "\" --out \"" + (iconset / ("icon_" + std::to_string(sz) + "x" + std::to_string(sz) + ".png")).string() + "\"") && ok;
		ok = ok && runq("iconutil -c icns \"" + iconset.string() + "\" -o \"" + (resDir / "game.icns").string() + "\"");
		bfs::remove_all(iconset, ec);
		if (ok) return "game";
		std::cout << "[Package]\tgame icon conversion FAILED (sips/iconutil): " << iconSrc << " — using the stock icon" << std::endl;
	}
	// Stock fallback: the engine logo from the editor bundle's Resources — dev tree layout
	// first, then the installed bundle's own (run root = Contents/MacOS).
	for (const bfs::path& stock : { nuke::RunRoot() / "NukeEngine-Editor.app" / "Contents" / "Resources" / "logo.icns",
	                                nuke::RunRoot().parent_path() / "Resources" / "logo.icns" })
		if (bfs::exists(stock, ec))
		{
			bfs::copy_file(stock, resDir / "game.icns", bfs::copy_options::overwrite_existing, ec);
			if (!ec) return "game";
		}
	return std::string();
}
#endif

#if defined(__linux__)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
// Linux counterpart of StampExeIcon/StampAppIconMac: put <stem>.png + .DirIcon into the
// game AppDir. A .png ships as-is; a .ico decodes through DecodeIcoRGBA; without a usable
// icon the editor's stock logo ships. Returns the Icon= stem for the .desktop, or "".
static std::string StampAppIconLinux(const bfs::path& appDir, const std::string& iconSrc,
                                     const std::string& gameName)
{
	boost::system::error_code ec;
	std::string stem = gameName;
	for (char& c : stem)
		if (!isalnum((unsigned char)c) && c != '-' && c != '_') c = '-';
	const bfs::path dst = appDir / (stem + ".png");
	auto finish = [&]() -> std::string
	{
		bfs::copy_file(dst, appDir / ".DirIcon", bfs::copy_options::overwrite_existing, ec);
		return stem;
	};
	if (!iconSrc.empty() && bfs::exists(bfs::path(iconSrc), ec))
	{
		const std::string ext = bfs::path(iconSrc).extension().string();
		if (ext == ".png" || ext == ".PNG")
		{
			bfs::copy_file(iconSrc, dst, bfs::copy_options::overwrite_existing, ec);
			if (!ec) return finish();
		}
		else
		{
			std::vector<unsigned char> rgba; int w = 0, h = 0;
			if (EditorUI::DecodeIcoRGBA(iconSrc, rgba, w, h)
			 && stbi_write_png(dst.string().c_str(), w, h, 4, rgba.data(), w * 4))
				return finish();
		}
		std::cout << "[Package]\tgame icon conversion FAILED: " << iconSrc << " — using the stock icon" << std::endl;
	}
	// Stock fallback: the editor's own icon — AppImage/AppDir root first, then the dev tree.
	for (const bfs::path& stock : { nuke::RunRoot() / "nukeengine-editor.png",
	                                nuke::RunRoot().parent_path().parent_path().parent_path()
	                                    / "NukeEngine-Editor" / "res" / "logo.png" })
		if (bfs::exists(stock, ec))
		{
			bfs::copy_file(stock, dst, bfs::copy_options::overwrite_existing, ec);
			if (!ec) return finish();
		}
	return std::string();
}
#endif

// Editor-only module? A module importing NukeImGui can't load next to the Player.
// Import tables store library names as plain bytes (PE import dir / Mach-O LC_LOAD_DYLIB),
// so a contents scan is a reliable test on either format.
static bool IsEditorOnlyModule(const bfs::path& dll)
{
	bfs::ifstream f(dll, std::ios::binary);
	if (!f) return false;
	std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	return bytes.find("NukeImGui.dll") != std::string::npos
	    || bytes.find("libNukeImGui.dylib") != std::string::npos
	    || bytes.find("libNukeImGui.so") != std::string::npos;
}

bool EditorUI::DecodeIcoRGBA(const std::string& path, std::vector<unsigned char>& rgba, int& w, int& h)
{
	bfs::ifstream f(bfs::path(path), std::ios::binary);
	if (!f) return false;
	std::string ico((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (ico.size() < 6 + sizeof(IcoDirEntry)) return false;
	if (*(const uint16_t*)(ico.data() + 2) != 1) return false;
	uint16_t count = *(const uint16_t*)(ico.data() + 4);
	if (!count || ico.size() < 6 + count * sizeof(IcoDirEntry)) return false;

	const IcoDirEntry* ent = (const IcoDirEntry*)(ico.data() + 6);
	int best = 0, bestW = 0;
	for (uint16_t i = 0; i < count; ++i)
	{
		int ew = ent[i].w ? ent[i].w : 256;   // 0 means 256 in the .ico format
		if (ew > bestW) { bestW = ew; best = i; }
	}
	const IcoDirEntry& e = ent[best];
	if ((uint64_t)e.offset + e.bytes > ico.size()) return false;
	const unsigned char* img = (const unsigned char*)ico.data() + e.offset;

	// PNG-compressed entry.
	if (e.bytes > 8 && img[0] == 0x89 && img[1] == 'P' && img[2] == 'N' && img[3] == 'G')
	{
		int n = 0;
		unsigned char* px = stbi_load_from_memory(img, (int)e.bytes, &w, &h, &n, 4);
		if (!px) return false;
		rgba.assign(px, px + (size_t)w * h * 4);
		stbi_image_free(px);
		return true;
	}

	// Classic DIB: BITMAPINFOHEADER with DOUBLE height (XOR image + AND mask), bottom-up.
	if (e.bytes < 40) return false;
	uint32_t hdr = *(const uint32_t*)img;
	int32_t  bw  = *(const int32_t*)(img + 4);
	int32_t  bh2 = *(const int32_t*)(img + 8);
	uint16_t bpp = *(const uint16_t*)(img + 14);
	if (hdr < 40 || bw <= 0 || bh2 <= 0 || bpp != 32) return false;   // exotic depths: no preview
	int bhh = bh2 / 2;
	uint64_t need = (uint64_t)hdr + (uint64_t)bw * bhh * 4;
	if (need > e.bytes) return false;
	const unsigned char* px = img + hdr;
	w = bw; h = bhh;
	rgba.resize((size_t)w * h * 4);
	bool anyAlpha = false;
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
		{
			const unsigned char* s = px + ((size_t)(h - 1 - y) * w + x) * 4;   // bottom-up BGRA
			unsigned char* d = &rgba[((size_t)y * w + x) * 4];
			d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
			anyAlpha |= s[3] != 0;
		}
	if (!anyAlpha)   // opacity lives in the 1-bpp AND mask after the XOR image
	{
		const unsigned char* mask = px + (size_t)w * h * 4;
		size_t maskStride = (((size_t)w + 31) / 32) * 4;   // rows pad to 32 bits
		if ((size_t)(mask - img) + maskStride * h <= e.bytes)
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					const unsigned char* row = mask + (size_t)(h - 1 - y) * maskStride;
					bool transparent = (row[x / 8] >> (7 - x % 8)) & 1;
					rgba[((size_t)y * w + x) * 4 + 3] = transparent ? 0 : 255;
				}
		else
			for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
	}
	return true;
}

// ---- editor-driven builds ----
void EditorUI::RunEngineBuild(const std::string& config, std::function<void(bool)> onDone)
{
	RunEngineBuild(config, std::vector<std::string>(), std::move(onDone));
}

void EditorUI::RunEngineBuild(const std::string& config, const std::vector<std::string>& targets,
                              std::function<void(bool)> onDone)
{
#ifdef _DEBUG
	const char* running = "Debug";
#else
	const char* running = "Release";
#endif
	if (config == running)
	{
		std::cout << "[build]\t\tskipped: the editor is running the " << config
		          << " binaries — build them from another config or the command line" << std::endl;
		nuke::Jobs::RunOnMain([onDone]() { if (onDone) onDone(true); });   // not a failure: proceed
		return;
	}
	// Repo root from the running exe (<root>/NukeEngine/<rundir>/<cfg>), via RunRoot() — an
	// installed .app then finds no superbuild here and the check below bows out gracefully.
	boost::system::error_code ec;
	bfs::path root = nuke::RunRoot().parent_path().parent_path().parent_path();
	if (!bfs::exists(root / "CMakeLists.txt", ec))
	{
		std::cout << "[build]\t\tno root superbuild found at " << root.string()
		          << " — building nothing (see CMakeLists.txt at the repo root)" << std::endl;
		nuke::Jobs::RunOnMain([onDone]() { if (onDone) onDone(false); });
		return;
	}
	StatusBar::Set("build", "Build " + config + ": starting...", StatusBar::kIndeterminate);
	if (!targets.empty())
	{
		std::string list;
		for (const std::string& t : targets) list += (list.empty() ? "" : " ") + t;
		std::cout << "[build]\t\ttargets (what the dist needs): " << list << std::endl;
	}
	nuke::Jobs::Schedule([this, root, config, targets, onDone]()
	{
		auto runPiped = [&](const std::string& cmdLine, int& outProjects) -> bool
		{
#ifndef _WIN32
			FILE* p = popen((cmdLine + " 2>&1").c_str(), "r");
			if (!p) { std::cout << "[build]\t\tcan't start: " << cmdLine << std::endl; return false; }
			std::string carry; char buf[4096]; size_t got;
			while ((got = fread(buf, 1, sizeof(buf), p)) > 0)
			{
				carry.append(buf, got);
				size_t nl;
				while ((nl = carry.find('\n')) != std::string::npos)
				{
					std::string line = carry.substr(0, nl);
					carry.erase(0, nl + 1);
					if (line.empty()) continue;
					std::cout << "[build]\t" << line << std::endl;
					if (line.find("Built target") != std::string::npos)
					{
						++outProjects;
						StatusBar::Set("build", "Build: " + std::to_string(outProjects) + " project(s) done",
						               StatusBar::kIndeterminate);
					}
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
				std::cout << "[build]\t\tcan't start: " << cmdLine << std::endl;
				return false;
			}
			CloseHandle(wr);
			// Stream line by line; project-completion lines tick the status bar.
			std::string carry;
			char buf[4096];
			DWORD got = 0;
			while (ReadFile(rd, buf, sizeof(buf), &got, NULL) && got > 0)
			{
				carry.append(buf, got);
				size_t nl;
				while ((nl = carry.find('\n')) != std::string::npos)
				{
					std::string line = carry.substr(0, nl);
					carry.erase(0, nl + 1);
					if (!line.empty() && line.back() == '\r') line.pop_back();
					if (line.empty()) continue;
					std::cout << "[build]\t" << line << std::endl;
					if (line.find(".vcxproj ->") != std::string::npos || line.find(" -> ") != std::string::npos)
					{
						++outProjects;
						StatusBar::Set("build", "Build: " + std::to_string(outProjects) + " project(s) done",
						               StatusBar::kIndeterminate);
					}
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

		int projects = 0;
		bool ok = true;
		boost::system::error_code ec2;
#ifdef _WIN32
		const bfs::path bldDir = root / "build";
#else
		// Single-config generators: one tree per configuration (Debug default; a Release
		// request configures its own tree). Per-platform names — the same checkout can hold
		// mac and linux trees side by side (shared drives, dual-boot).
#ifdef __APPLE__
		const bfs::path bldDir = root / (config == "Debug" ? "build-mac" : "build-mac-release");
#else
		const bfs::path bldDir = root / (config == "Debug" ? "build-linux" : "build-linux-release");
#endif
#endif
		if (!bfs::exists(bldDir / "CMakeCache.txt", ec2))
		{
			std::cout << "[build]\t\tconfiguring the superbuild (first run)..." << std::endl;
			StatusBar::Set("build", "Build: configuring...", StatusBar::kIndeterminate);
#ifdef _WIN32
			ok = runPiped("cmake -S \"" + root.string() + "\" -B \"" + bldDir.string()
			              + "\" -G \"Visual Studio 17 2022\" -A x64", projects);
#else
			ok = runPiped("cmake -S \"" + root.string() + "\" -B \"" + bldDir.string()
			              + "\" -DCMAKE_BUILD_TYPE=" + config, projects);
#endif
		}
		// An explicit target list (packaging) builds the player + the shipped modules and their
		// engine dependency only — the editor and unused modules stay untouched.
		std::string tsel;
		for (const std::string& t : targets) tsel += " --target " + t;
#ifdef _WIN32
		if (ok)
			ok = runPiped("cmake --build \"" + bldDir.string() + "\" --config " + config + tsel
			              + " -- /m /v:m /nologo", projects);
#else
		if (ok)
			ok = runPiped("cmake --build \"" + bldDir.string() + "\" --parallel" + tsel, projects);
#endif
		const bool result = ok;
		nuke::Jobs::RunOnMain([this, result, config, onDone]()
		{
			StatusBar::Remove("build");
			std::cout << "[build]\t\t" << config << (result ? " build OK" : " build FAILED — see the lines above") << std::endl;
			if (onDone) onDone(result);
		});
	});
}

// Split `files` per the project's setting: what stays goes back into `files`, the rest is
// returned as (suffix, files) part sets. Worlds, the merge basis, manifests and script
// assemblies ALWAYS stay in the main pak — the loader reads them from the pak that served
// the world. splitMode: 0 none, 1 by content type, 2 greedy under a raw-size cap.
static std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>
SplitPakFiles(std::vector<std::pair<std::string, std::string>>& files, int splitMode, int splitCapMB)
{
	std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> partsOut;
	if (splitMode != 1 && (splitMode != 2 || splitCapMB <= 0)) return partsOut;

	auto low = [](const std::string& rel)
	{ std::string s = rel; for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
	auto ends = [](const std::string& s, const char* e)
	{ const size_t n = strlen(e); return s.size() > n && s.compare(s.size() - n, n, e) == 0; };
	auto mustStayMain = [&](const std::string& l)
	{
		return l.compare(0, 6, "basis/") == 0 || l.compare(0, 8, "managed/") == 0
		    || ends(l, ".nuworld") || l == "mod.json" || l == "pak.json" || l == "game.nuproj";
	};

	std::vector<std::pair<std::string, std::string>> mainFiles;
	if (splitMode == 1)
	{
		auto classOf = [&](const std::string& l) -> const char*
		{
			if (ends(l, ".nutex") || ends(l, ".png") || ends(l, ".jpg") || ends(l, ".jpeg")
			 || ends(l, ".tga") || ends(l, ".dds") || ends(l, ".bmp")) return "textures";
			if (ends(l, ".wav") || ends(l, ".ogg") || ends(l, ".mp3") || ends(l, ".flac")) return "audio";
			if (ends(l, ".numesh") || ends(l, ".nuanim") || ends(l, ".nubonemap")) return "meshes";
			return "";
		};
		std::map<std::string, std::vector<std::pair<std::string, std::string>>> byClass;
		for (auto& fp : files)
		{
			const std::string l = low(fp.first);
			const char* cls = mustStayMain(l) ? "" : classOf(l);
			if (cls[0]) byClass[cls].push_back(fp); else mainFiles.push_back(fp);
		}
		for (auto& kv : byClass) if (!kv.second.empty()) partsOut.push_back({ kv.first, kv.second });
	}
	else
	{
		const uint64_t cap = (uint64_t)splitCapMB << 20;
		std::vector<std::pair<std::string, std::string>> cur;
		uint64_t mainSz = 0, curSz = 0; int partN = 2;
		for (auto& fp : files)
		{
			boost::system::error_code fec;
			const uint64_t sz = (uint64_t)bfs::file_size(bfs::path(fp.second), fec);
			if (mustStayMain(low(fp.first)))      { mainFiles.push_back(fp); mainSz += sz; continue; }
			if (mainSz + sz <= cap)               { mainFiles.push_back(fp); mainSz += sz; continue; }
			if (!cur.empty() && curSz + sz > cap) { partsOut.push_back({ "part" + std::to_string(partN++), cur }); cur.clear(); curSz = 0; }
			cur.push_back(fp); curSz += sz;
		}
		if (!cur.empty()) partsOut.push_back({ "part" + std::to_string(partN++), cur });
	}
	files.swap(mainFiles);
	return partsOut;
}

void EditorUI::PackageProject()
{
	// Analyze BEFORE building: the same content cook the packer runs decides which modules the
	// dist ships, and only those (plus the player) get built — an unused module and the editor
	// have no business in a game build. Then the chosen config rebuilds so stale binaries never
	// ship, and the project's game modules follow from modules/<Config>/.
	const std::string cfg = gbBuildCfg == 1 ? "Debug" : "Release";
	{
		const std::string projDir = projectDir, projFile = projectFile, content = contentDir;
		const std::string distStr = distPath.empty() ? (bfs::path(projDir) / "dist").string()
		                          : (bfs::path(distPath).is_absolute() ? distPath
		                                                               : (bfs::path(projDir) / distPath).string());
		std::map<std::string, std::vector<std::string>> guidFiles;
		{
			ResDB* db = ResDB::getSingleton();
			for (auto& kv : db->pathByGuid) guidFiles[kv.first].push_back(kv.second);
		}
		auto pkgMods = SnapshotPkgMods();
		const bool consoleOn = gbWinSet && gbConsole;
		StatusBar::Set("package", "Package: analyzing used modules...", StatusBar::kIndeterminate);
		nuke::Jobs::Schedule([this, cfg, projDir, projFile, content, distStr, guidFiles, pkgMods, consoleOn]() mutable
		{
			std::set<std::string> claimants;
			std::vector<std::string> reached;
			auto all = CollectProject(projDir, false, DistPrefix(projDir, distStr));
			std::set<std::string> used = CookUsedFiles(projDir, content, projFile, guidFiles, &claimants, &reached);
			std::vector<std::pair<std::string, std::string>> files;
			for (auto& fp : all)
				if (used.count(DiskKey(bfs::path(fp.second)))) files.push_back(fp);
			files.push_back({ "game.nuproj", projFile });
			for (const std::string& r : reached) files.push_back({ r, r });   // script sources: detection-only input
			boost::system::error_code ec;
			for (auto& kv : pkgMods)
				for (const std::string& rel : kv.second.extraPak)
				{
					bfs::path src = bfs::path(projDir) / rel;
					if (bfs::exists(src, ec) && !bfs::is_directory(src, ec)) files.push_back({ rel, src.string() });
				}
			std::set<std::string> ship = ComputeShipModules(files, pkgMods, claimants, consoleOn,
			                                                ReadManifestShipModules(projFile), false);
			// The build set: the player + whichever shipped modules are superbuild targets (the
			// project's own game modules build separately, per config).
			std::vector<std::string> targets{ "NukePlayer" };
			const bfs::path root = nuke::RunRoot().parent_path().parent_path().parent_path();
			for (const std::string& s : ship)
				if (bfs::exists(root / s / "CMakeLists.txt", ec)) targets.push_back(s);
			nuke::Jobs::RunOnMain([this, cfg, targets]()
			{
				StatusBar::Remove("package");   // the build and pack stages set their own
				pkgAnalyzed = true;             // the pack worker skips the duplicate decision log
				PackageProjectBuild(cfg, targets);
			});
		});
	}
}

// The build half of Package Project: engine targets, then game modules, then the pack itself.
void EditorUI::PackageProjectBuild(const std::string& cfg, const std::vector<std::string>& targets)
{
	RunEngineBuild(cfg, targets, [this, cfg](bool ok)
	{
		if (!ok)
		{
			std::cout << "[Package]\taborted: the " << cfg << " build failed — fix it and package again" << std::endl;
			return;
		}
		// The project's own game modules must match the dist's config (a Debug Game.dll in a
		// Release dist imports the debug CRT and dies on any machine without VS). Per-config
		// module dirs mean this build never touches the editor's own loaded DLLs.
		boost::system::error_code ec;
		if (bfs::exists(bfs::path(projectDir) / "source", ec))
		{
			BuildGameModules(cfg.c_str(), [this, cfg](bool mok)
			{
				if (!mok)
				{
					std::cout << "[Package]\taborted: the game-module " << cfg << " build failed — fix it and package again" << std::endl;
					return;
				}
				PackageProjectNow();
			});
			return;
		}
		PackageProjectNow();
	});
}

void EditorUI::PackageProjectNow()
{
	// Archive-derived sessions are not the authoring project; guards the NUKE_PACKAGE dev hook.
	if (!basePakPath.empty())
	{
		std::cout << "[editor]\t\t" << "Package Project refused: this project was opened from an archive ("
		          << basePakPath << "). Package a mod instead (File -> Package Mod)." << std::endl;
		return;
	}
	// Snapshot everything the worker needs (members are game-thread state).
	const std::string projDir = projectDir;
	const std::string projFile = projectFile;
	const std::string content = contentDir;
	const bool             gbSet = gbWinSet;   // Game Build dialog ran -> its window tweaks ship
	const nuke::NukeWindow gbCfg = gbWin;
	// Game defaults are OFF: without the dialog these ship off, never the editor's own values.
	const bool gbLogS = gbSet && gbLog;
	const bool gbDbgS = gbSet && gbDebug;
	const bool gbConsS = gbSet && gbConsole;   // the in-game dev console (~) in the dist
	const std::string gameName = projectName.empty() ? std::string("NukeGame") : projectName;
	const std::string icon = gameIcon.empty() ? std::string()
	                        : AppInstance::GetSingleton()->ResolveContent(gameIcon);
	// Cooker inputs: ResDB is game-thread state, so snapshot guid -> file(s) here. Shaders
	// carry TWO files per guid (the .vs/.ps pair).
	std::map<std::string, std::vector<std::string>> guidFiles;
	{
		ResDB* db = ResDB::getSingleton();
		for (auto& kv : db->pathByGuid) guidFiles[kv.first].push_back(kv.second);
		for (nuke::Shader* sh : db->shaders)
			if (sh && !sh->vsPath.empty())
			{
				guidFiles[sh->guid].push_back(sh->vsPath);
				if (!sh->psPath.empty()) guidFiles[sh->guid].push_back(sh->psPath);
			}
			else if (sh && sh->isPost && !sh->psPath.empty())
				guidFiles[sh->guid].push_back(sh->psPath);
	}
	// Build output: "" = <project>/dist; a relative setting resolves against the project.
	std::string distStr = distPath.empty() ? (bfs::path(projDir) / "dist").string()
	                    : (bfs::path(distPath).is_absolute() ? distPath
	                                                         : (bfs::path(projDir) / distPath).string());
	const int method = pakMethod, level = pakLevel, blockMB = pakBlockMB;
	const int splitMode = modSplitMode, splitCapMB = modSplitCapMB;   // one split setting for every pak
	// Module snapshot (game thread): the candidates, their service roles and ship extras.
	// WHICH of them ship is decided on the worker from the cooked content — the plugin list is
	// the candidate pool, not the answer (see ComputeShipModules).
	auto pkgMods = SnapshotPkgMods();
	const bool quiet = pkgAnalyzed;   // PackageProject's pre-pass already logged the decisions
	pkgAnalyzed = false;

	// The dist's build configuration: which sibling run dir the binaries come from and which
	// modules/<Config>/ the project modules ship from.
	const std::string distCfg = gbBuildCfg == 1 ? "Debug" : "Release";
	StatusBar::Set("package", "Packaging project (" + distCfg + ")...", StatusBar::kIndeterminate);
	nuke::Jobs::Schedule([this, projDir, projFile, content, gameName, icon, method, level, blockMB, splitMode, splitCapMB,
	                      pkgMods, distStr, guidFiles, distCfg, quiet,
	                      gbSet, gbCfg, gbLogS, gbDbgS, gbConsS]()
	{
		Package::CreateOptions pakOpts; pakOpts.blockBytes = (uint32_t)blockMB << 20;
		boost::system::error_code ec;
		// distRoot = the user-visible build folder. On macOS the shipped layout lives INSIDE
		// <GameName>.app/Contents/MacOS (self-contained bundle: run root = exe dir), so every
		// dist-relative path below lands in the bundle; elsewhere dist == distRoot.
		const bfs::path distRoot = distStr;
#ifdef __APPLE__
		const bfs::path dist = distRoot / (gameName + ".app") / "Contents" / "MacOS";
#elif defined(__linux__)
		// Linux: the shipped layout IS the AppDir — flat, run root = exe dir = image root.
		// appimagetool squashes it into <Game>.AppImage at the end; the loose AppDir stays
		// runnable for quick testing (./AppRun).
		const bfs::path dist = distRoot / (gameName + ".AppDir");
#else
		const bfs::path dist = distRoot;
#endif
		// dist/ is a build artifact, always wiped: leftovers from an older pack would ship a
		// broken mix. mods/ and mods.json are preserved explicitly across the wipe.
		std::string modsJson;
		{
			bfs::ifstream mj(dist / "config" / "mods.json");
			if (mj) modsJson.assign(std::istreambuf_iterator<char>(mj), std::istreambuf_iterator<char>());
		}
		bfs::path modsKeep = bfs::path(projDir) / ".dist_mods_keep";
		bfs::remove_all(modsKeep, ec);
		if (bfs::exists(dist / "mods", ec)) bfs::rename(dist / "mods", modsKeep, ec);
		// Installed DLC paks survive a base repack, like mods.
		bfs::path dlcKeep = bfs::path(projDir) / ".dist_dlc_keep";
		bfs::remove_all(dlcKeep, ec);
		if (bfs::exists(dist / "content" / "dlc", ec)) bfs::rename(dist / "content" / "dlc", dlcKeep, ec);
		bfs::remove_all(distRoot, ec);   // the whole build folder: stale bundles/flat layouts too
		bfs::create_directories(dist, ec);
		if (bfs::exists(modsKeep, ec)) bfs::rename(modsKeep, dist / "mods", ec);
		if (bfs::exists(dlcKeep, ec))
		{
			bfs::create_directories(dist / "content", ec);
			bfs::rename(dlcKeep, dist / "content" / "dlc", ec);
		}
		if (!modsJson.empty())
		{
			bfs::create_directories(dist / "config", ec);
			bfs::ofstream mj(dist / "config" / "mods.json");
			if (mj) mj << modsJson;
		}

		// 1) The project -> dist/content/game.nupak, cooked: only the manifest's dependency
		// closure ships. "packInclude" force-adds extras the cooker can't see.
		std::set<std::string> cookClaimants;
		std::vector<std::string> cookReached;
		auto all = CollectProject(projDir, false, DistPrefix(projDir, distRoot.string()));
		std::set<std::string> used = CookUsedFiles(projDir, content, projFile, guidFiles, &cookClaimants, &cookReached);
		std::vector<std::pair<std::string, std::string>> files;
		files.reserve(all.size());
		int forcedShaders = 0;
		for (auto& fp : all)
		{
			if (used.count(DiskKey(bfs::path(fp.second)))) { files.push_back(fp); continue; }
			// Auto-loaded types are invisible to the reference walk, so they always ship:
			// .hlsl is bound by name (Shader.Find), .nuinput is auto-loaded by ResDB.
			std::string low = fp.first;
			for (char& c : low) c = (char)tolower((unsigned char)c);
			auto ends = [&](const char* ext) {
				const size_t n = strlen(ext);
				return low.size() > n && low.compare(low.size() - n, n, ext) == 0;
			};
			if (ends(".hlsl") || ends(".nuinput"))
			{
				files.push_back(fp);
				++forcedShaders;
			}
		}
		std::cout << "[Package]\tcooked " << files.size() << " used files ("
		          << (all.size() - files.size()) << " of " << all.size() << " unused, not shipped"
		          << (forcedShaders ? "; " + std::to_string(forcedShaders) + " shader file(s) force-shipped" : "")
		          << ")" << std::endl;
		files.push_back({ "game.nuproj", projFile });
		// Pak identity: DLCs record this name as their "base", so a DLC mounts only onto its own game.
		bfs::path pakManTmp = bfs::path(projDir) / ".pak.json.tmp";
		// Module pak extras: project files the cooker can't reach by reference. They join the
		// module-detection INPUT (a managed assembly is scanned for the modules its code uses);
		// extras of modules that end up not shipping are pulled back out below.
		std::vector<std::pair<std::string, std::string>> extraOwner;   // pak rel -> module (lowercase)
		for (auto& mkv : pkgMods)
			for (const std::string& rel : mkv.second.extraPak)
			{
				bfs::path src = bfs::path(projDir) / rel;
				if (bfs::exists(src, ec) && !bfs::is_directory(src, ec))
				{
					files.push_back({ rel, src.string() });
					extraOwner.push_back({ rel, mkv.first });
					std::cout << "[Package]\tmodule pak extra: " << rel << std::endl;
				}
				else
					std::cout << "[Package]\tmodule pak extra MISSING, skipped: " << rel << std::endl;
			}
		// The runtime source dir (Release preferred) must resolve BEFORE the pak builds:
		// engine built-ins (shaders/, fonts/) ride INSIDE game.nupak so mods can override them.
		// Anchored on RunRoot(), not the CWD — an installed .app runs with CWD elsewhere.
		bfs::path rt = nuke::RunRoot();
		{
#ifdef _WIN32
			const char* playerBin = "NukePlayer.exe";
			const char* engineBin = "NukeEngine.dll";
#elif defined(__APPLE__)
			const char* playerBin = "NukePlayer";
			const char* engineBin = "libNukeEngine.dylib";
#else
			const char* playerBin = "NukePlayer";
			const char* engineBin = "libNukeEngine.so";
#endif
			bfs::path rel = rt.parent_path() / distCfg;   // sibling config dir (dev tree only)
			if (rel != rt && bfs::exists(rel / playerBin, ec) && bfs::exists(rel / engineBin, ec))
				rt = rel;
			else if (rel != rt)
				std::cout << "[Package]\t" << distCfg << " build not found — bundling the CURRENT config's binaries" << std::endl;
		}
		for (const char* dirName : { "shaders", "fonts" })
			for (bfs::recursive_directory_iterator it(rt / dirName, ec), end; it != end && !ec; it.increment(ec))
			{
				if (bfs::is_directory(it->path())) continue;
				boost::system::error_code rec;
				std::string rel = bfs::relative(it->path(), rt, rec).generic_string();
				if (!rec) files.push_back({ rel, it->path().string() });
			}
		// Which modules ship: decided by the CONTENT (component types, cook claims, script use,
		// binary imports) — not by the plugin list alone. Detection sees MORE than the pak:
		// script SOURCES the cook reached never ship (the compiled assembly does), but the .cs
		// text is where module usage is visible. See ComputeShipModules.
		std::vector<std::pair<std::string, std::string>> detectFiles = files;
		for (const std::string& r : cookReached) detectFiles.push_back({ r, r });
		std::set<std::string> shipMods = ComputeShipModules(detectFiles, pkgMods, cookClaimants, gbConsS,
		                                                    ReadManifestShipModules(projFile), quiet);
		{
			std::string list;
			for (const std::string& s : shipMods) list += (list.empty() ? "" : ", ") + s;
			std::cout << "[Package]\t" << shipMods.size() << " module(s) ship: " << list << std::endl;
		}
		auto shipsMod = [&](const std::string& lowName)
		{
			auto sit = pkgMods.find(lowName);
			return sit != pkgMods.end() && shipMods.count(sit->second.name) != 0;
		};
		// Pull the pak extras of non-shipping modules back out.
		files.erase(std::remove_if(files.begin(), files.end(), [&](const std::pair<std::string, std::string>& fp)
		{
			for (const auto& eo : extraOwner)
				if (eo.first == fp.first && !shipsMod(eo.second))
				{
					std::cout << "[Package]\tmodule pak extra dropped with its module: " << fp.first << std::endl;
					return true;
				}
			return false;
		}), files.end());
		// Dist-tree extras of the shipped modules only.
		std::vector<std::pair<std::string, std::string>> extraDist;
		for (const auto& mkv : pkgMods)
			if (shipMods.count(mkv.second.name))
				for (const auto& ed : mkv.second.extraDist) extraDist.push_back(ed);
		// The player only loads LISTED plugins: with the console on, the GUI backend must be on
		// the shipped manifest even when the project itself never uses it.
		bfs::path projTweakTmp;
		if (gbConsS)
		{
			std::string guiName;
			for (const auto& mkv : pkgMods)
				if (mkv.second.provides == "gui" && shipMods.count(mkv.second.name)) guiName = mkv.second.name;
			if (!guiName.empty())
			{
				bfs::ifstream pf{ bfs::path(projFile) };
				nlohmann::json pj = pf ? nlohmann::json::parse(pf, nullptr, false) : nlohmann::json();
				if (!pj.is_discarded() && pj.is_object())
				{
					if (!pj.contains("plugins") || !pj["plugins"].is_array()) pj["plugins"] = nlohmann::json::array();
					bool listed = false;
					for (auto& p : pj["plugins"])
						if (p.is_string() && nuke::ModuleFileMatches(p.get<std::string>(), guiName)) listed = true;
					if (!listed)
					{
						pj["plugins"].push_back(guiName);
						projTweakTmp = bfs::path(projDir) / ".game.nuproj.tmp";
						bfs::ofstream po(projTweakTmp, std::ios::binary | std::ios::trunc);
						if (po)
						{
							po << pj.dump(2);
							po.close();
							for (auto& fp : files)
								if (fp.first == "game.nuproj") fp.second = projTweakTmp.string();
							std::cout << "[Package]\tdev console: '" << guiName
							          << "' added to the shipped manifest's plugins" << std::endl;
						}
					}
				}
			}
		}
		// Split the game's own content into side parts, then write the identity manifest with
		// the part list so the runtime mounts them alongside game.nupak.
		auto partsOut = SplitPakFiles(files, splitMode, splitCapMB);
		std::vector<std::string> partFiles;
		for (auto& pr : partsOut) partFiles.push_back("game." + pr.first + ".nupak");
		{
			nlohmann::json pj;
			pj["kind"] = "base";
			pj["name"] = gameName;
			pj["platforms"] = nlohmann::json::array({ Package::CurrentPlatform() });
			if (!partFiles.empty()) pj["parts"] = partFiles;
			bfs::ofstream pf(pakManTmp, std::ios::binary | std::ios::trunc);
			if (pf) pf << pj.dump(2);
		}
		files.push_back({ "pak.json", pakManTmp.string() });
		// Fast loading: document containers (worlds, cells, prefabs) ship COOKED — "NCBR" + CBOR,
		// a fraction of the text size and several times faster to parse; every loader sniffs the
		// magic, the project files stay text. Cooked copies live in a temp dir for the pack only.
		boost::system::error_code cookEc;
		const bfs::path cookDir = bfs::temp_directory_path(cookEc) / "nuke-cook";   // never inside the project tree
		{
			boost::system::error_code cec;
			bfs::remove_all(cookDir, cec);
			int cooked = 0;
			for (auto& fp : files)
			{
				std::string low = fp.first;
				for (char& c : low) c = (char)tolower((unsigned char)c);
				const bool doc = (low.size() > 8 && low.compare(low.size() - 8, 8, ".nuworld") == 0)
				              || (low.size() > 9 && low.compare(low.size() - 9, 9, ".nuprefab") == 0);
				const bool tex = low.size() > 6 && low.compare(low.size() - 6, 6, ".nutex") == 0;
				if (!doc && !tex) continue;
				const bfs::path out = cookDir / fp.first;
				if (doc)
				{
					if (nuke::CookDocFile(fp.second, out.string())) { fp.second = out.string(); ++cooked; }
					continue;
				}
				// Pre-v11 textures heal (re-encode) on every load: ship them healed, as v11.
				if (Texture* t = Texture::LoadFromFile(fp.second))
				{
					if (t->healedOnLoad)
					{
						boost::system::error_code tec;
						bfs::create_directories(out.parent_path(), tec);
						if (t->SaveToFile(out.string())) { fp.second = out.string(); ++cooked; }
					}
					delete t;
				}
			}
			std::cout << "[Package]\t" << cooked << " document(s) cooked to binary" << std::endl;
			// Module cook transforms (e.g. NukeAudio: lossless audio -> Vorbis): a module may
			// rewrite a shipping file under the same pak name; the manifest carries its settings.
			{
				std::string projJson;
				{
					bfs::ifstream pf(bfs::path(projFile), std::ios::binary);
					if (pf) projJson.assign(std::istreambuf_iterator<char>(pf), std::istreambuf_iterator<char>());
				}
				int transformed = 0;
				for (auto& fp : files)
				{
					std::string tout, trel;
					for (auto& m : nuke::GetModules())
					{
						if (!m || !m->loaded || nuke::ModuleAbi(m.get()) < 5) continue;
						if (!m->cookTransform(fp.first.c_str(), fp.second.c_str(), projJson.c_str(), trel, tout)) continue;
						if (!trel.empty()) fp.first = trel;   // format change ships under its own name
						const bfs::path o = cookDir / fp.first;
						boost::system::error_code tec;
						bfs::create_directories(o.parent_path(), tec);
						bfs::ofstream of(o, std::ios::binary | std::ios::trunc);
						if (of) { of.write(tout.data(), (std::streamsize)tout.size()); of.close(); fp.second = o.string(); ++transformed; }
						break;
					}
				}
				if (transformed) std::cout << "[Package]\t" << transformed << " file(s) cook-transformed by modules" << std::endl;
			}
		}
		bool ok = !files.empty()
		       && Package::Create(files, (dist / "content" / "game.nupak").string(), method, level,
		              [](int done, int total)
		              {
		                  StatusBar::Set("package", "Packaging project... " + std::to_string(done) + "/" + std::to_string(total),
		                                 total ? 0.7f * done / total : 0.0f);
		              }, &pakOpts);
		for (size_t pi = 0; ok && pi < partsOut.size(); ++pi)
		{
			nlohmann::json ppj;
			ppj["kind"] = "part";
			ppj["name"] = gameName + " (" + partsOut[pi].first + ")";
			ppj["part_of"] = gameName;
			bfs::path ptmp = bfs::path(projDir) / (".pak.part" + std::to_string(pi) + ".json.tmp");
			{ bfs::ofstream po(ptmp, std::ios::binary | std::ios::trunc); if (po) po << ppj.dump(2); }
			auto pfiles = partsOut[pi].second;
			pfiles.push_back({ "pak.json", ptmp.string() });
			ok = Package::Create(pfiles, (dist / "content" / partFiles[pi]).string(), method, level,
			         [](int done, int total)
			         {
			             StatusBar::Set("package", "Packaging part... " + std::to_string(done) + "/" + std::to_string(total),
			                            total ? 0.7f * done / total : 0.0f);
			         }, &pakOpts) && ok;
			bfs::remove(ptmp, ec);
			if (ok) std::cout << "[Package]\tgame part: " << partFiles[pi] << " ("
			                  << partsOut[pi].second.size() << " files)" << std::endl;
		}

		// 2) The runtime around it: the Player under the game's name + icon, its DLLs, the
		// config, the used modules and the mods/ socket. The two build configs never mix (CRT).
		if (ok)
		{
			StatusBar::Set("package", "Packaging: runtime files...", 0.75f);
#ifdef _WIN32
			const bfs::path gameExe = dist / (gameName + ".exe");
			ok &= CopyOne(rt / "NukePlayer.exe", gameExe);
			for (bfs::directory_iterator it(rt, ec), end; it != end && !ec; it.increment(ec))
			{
				if (bfs::is_directory(it->path())) continue;
				if (it->path().extension() != ".dll") continue;
				std::string n = it->path().filename().string();
				if (n == "NukeImGui.dll") continue;              // editor-only UI dll
				CopyOne(it->path(), dist / n);
			}
#else
			// macOS: the game IS the .app — binary + dylibs live in Contents/MacOS (dist).
			// Linux: flat dist, .so's next to the binary ($ORIGIN rpath).
			const bfs::path gameExe = dist / gameName;
			ok &= CopyOne(rt / "NukePlayer", gameExe);
			{
				boost::system::error_code pec;
				bfs::permissions(gameExe, bfs::perms::owner_all | bfs::perms::group_read | bfs::perms::group_exe
				                        | bfs::perms::others_read | bfs::perms::others_exe, pec);
			}
			for (bfs::directory_iterator it(rt, ec), end; it != end && !ec; it.increment(ec))
			{
				if (bfs::is_directory(it->path())) continue;
				std::string n = it->path().filename().string();
#ifdef __APPLE__
				if (it->path().extension() != ".dylib") continue;
				if (n == "libNukeImGui.dylib") continue;         // editor-only UI dylib
#else
				if (n.find(".so") == std::string::npos) continue;   // libFoo.so / libglfw.so.3
				if (n.rfind("libNukeImGui.so", 0) == 0) continue;   // editor-only UI so
#endif
				CopyOne(it->path(), dist / n);
			}
#ifdef __APPLE__
			// Bundle manifest + icon: the mac equivalents of the exe rename and the .ico stamp.
			{
				const std::string iconStem = StampAppIconMac(dist.parent_path(), icon);
				if (!iconStem.empty() && !icon.empty())
					std::cout << "[Package]\tgame icon stamped (icns): " << icon << std::endl;
				std::string safeId = gameName;
				for (char& c : safeId)
					if (!isalnum((unsigned char)c) && c != '-' && c != '.') c = '-';
				bfs::ofstream pl(dist.parent_path() / "Info.plist", std::ios::trunc);
				if (pl)
				{
					pl <<
					"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
					"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
					"<plist version=\"1.0\">\n<dict>\n"
					"\t<key>CFBundleExecutable</key>\t<string>" << gameName << "</string>\n"
					"\t<key>CFBundleIdentifier</key>\t<string>com.luastris.game." << safeId << "</string>\n"
					"\t<key>CFBundleName</key>\t<string>" << gameName << "</string>\n"
					"\t<key>CFBundlePackageType</key>\t<string>APPL</string>\n"
					"\t<key>CFBundleShortVersionString</key>\t<string>1.0</string>\n"
					"\t<key>CFBundleVersion</key>\t<string>1</string>\n"
					"\t<key>NSHighResolutionCapable</key>\t<true/>\n";
					if (!iconStem.empty())
						pl << "\t<key>CFBundleIconFile</key>\t<string>" << iconStem << "</string>\n";
#ifdef NUKE_BUILD_ARCHS
					// The build's architecture set — the shipped player carries the same
					// slices as the editor that packaged it (one superbuild).
					pl << "\t<key>NukeBuildArchitectures</key>\t<string>" << NUKE_BUILD_ARCHS << "</string>\n";
#endif
					pl << "</dict>\n</plist>\n";
				}
			}
#elif defined(__linux__)
			// AppImage identity: AppRun + one root .desktop + the icon it names — the Linux
			// equivalents of the exe rename and the .ico stamp.
			{
				boost::system::error_code lec;
				bfs::remove(dist / "AppRun", lec);
				bfs::create_symlink(gameName, dist / "AppRun", lec);
				const std::string iconStem = StampAppIconLinux(dist, icon, gameName);
				if (!iconStem.empty() && !icon.empty())
					std::cout << "[Package]\tgame icon stamped (png): " << icon << std::endl;
				std::string safeId = gameName;
				for (char& c : safeId)
					if (!isalnum((unsigned char)c) && c != '-' && c != '_') c = '-';
				bfs::ofstream dt(dist / (safeId + ".desktop"), std::ios::trunc);
				if (dt)
				{
					dt << "[Desktop Entry]\n"
					      "Type=Application\n"
					      "Name=" << gameName << "\n"
					      "Exec=\"" << gameName << "\"\n";
					if (!iconStem.empty()) dt << "Icon=" << iconStem << "\n";
					dt << "Terminal=false\n"
					      "Categories=Game;\n";
				}
			}
#endif
#endif
			// The game's config is formed here: the editor's current config with the window
			// block overridden by the Game Build dialog. No title key — the Player titles its
			// window from game.nuproj "name".
			try
			{
				nlohmann::json cj;
				{
					// The editor's live config (writable), else the shipped stock one.
				bfs::path cfgSrc = nuke::Config::writableDir() / "config" / "main.json";
				if (!bfs::exists(cfgSrc, ec)) cfgSrc = nuke::Config::baseDir() / "config" / "main.json";
				bfs::ifstream in(cfgSrc);
					if (in) { std::stringstream ss; ss << in.rdbuf(); cj = nlohmann::json::parse(ss.str(), nullptr, false, true); }
				}
				if (!cj.is_object()) cj = nlohmann::json::object();
				{
					// The dialog's values when it ran (gbSet), else the editor's live window config.
					const nuke::NukeWindow w = gbSet ? gbCfg : nuke::Config::getSingleton()->window;
					nlohmann::json& jw = cj["window"];
					if (!jw.is_object()) jw = nlohmann::json::object();
					jw["width"]       = w.w;
					jw["height"]      = w.h;
					if (!w.mainFont.empty()) jw["mainFont"] = w.mainFont;
					jw["decorated"]   = w.decorated;
					jw["resizable"]   = w.resizable;
					jw["floating"]    = w.floating;
					jw["maximized"]   = w.maximized;
					// Human-readable mode; the legacy `fullscreen` bool never ships.
					jw["mode"]        = w.mode == 1 ? "borderless" : w.mode == 2 ? "exclusive" : "windowed";
					jw.erase("fullscreen");
					jw["transparent"] = w.transparent;
					jw["opacity"]     = w.opacity;
					jw["backend"]     = w.backend;
					jw["rayTracing"]  = w.rayTracing;
					jw["showFps"]     = w.showFps;
					jw["vsync"]       = w.vsync;
					jw["showConsole"] = w.showConsole;
					jw["fpsLimit"]    = w.fpsLimit;
					jw.erase("title");
				}
				// Log/debug ship as the dialog set them; the editor's own values never leak in.
				cj["logToConsole"]  = gbLogS;
				cj["gpuValidation"] = gbDbgS;
				cj["devConsole"]    = gbConsS;   // the in-game ~ console (packaged default is OFF)
				boost::system::error_code cec;
				bfs::create_directories(dist / "config", cec);   // first package: config/ doesn't exist yet
				// Fast loading: ship the WARM shader bytecode caches (backend IL — GPU-agnostic) and
				// the pipeline caches, so a first launch skips the DXC/FXC front end. Every backend
				// present is copied: the shipped game may run another one than the editor.
				{
					const bfs::path cfg = bfs::path(nuke::Config::writableDir()) / "config";
					int shipped = 0;
					for (bfs::directory_iterator it(cfg, cec), end; !cec && it != end; it.increment(cec))
					{
						const std::string nm = it->path().filename().string();
						if (nm.rfind("shadercache_", 0) == 0 && bfs::is_directory(it->path())) { CopyTree(it->path(), dist / "config" / nm); ++shipped; }
						else if (nm.rfind("psocache_", 0) == 0) { CopyOne(it->path(), dist / "config" / nm); ++shipped; }
					}
					std::cout << "[Package]\t" << shipped << " shader/pipeline cache(s) shipped" << std::endl;
				}
				bfs::ofstream outc(dist / "config" / "main.json");
				if (outc) outc << cj.dump(2);
			}
			catch (...) {}
			if (!bfs::exists(dist / "config" / "mods.json", ec))
			{
				bfs::create_directories(dist / "config", ec);
				bfs::ofstream mj(dist / "config" / "mods.json");
				if (mj) mj << "{\n  \"mods\": []\n}\n";
			}
			bfs::create_directories(dist / "mods", ec);
#ifdef _WIN32
			if (ok && !icon.empty())
			{
				if (StampExeIcon(gameExe.string(), icon))
					std::cout << "[Package]\tgame icon stamped: " << icon << std::endl;
				else
					std::cout << "[Package]\tgame icon FAILED (need a real .ico): " << icon << std::endl;
			}
#endif
			StatusBar::Set("package", "Packaging: modules...", 0.9f);
			for (const std::string& m : shipMods)
			{
				// The list holds platform-neutral NAMES; the file on disk is this platform's
				// spelling ("NukeVFX" -> NukeVFX.dll / libNukeVFX.so). Legacy entries that
				// still carry a foreign extension resolve through the same helper.
				const std::string native = nuke::ModuleFileName(nuke::ModuleName(m));
				std::string shipName = native;   // the file name that lands in dist/modules
				boost::system::error_code mec;
				bfs::path src = rt / "modules" / native;
				// Project-local game modules ship too; editor modules win on name clash.
				if (!bfs::exists(src, mec)) src = bfs::path(projDir) / "modules" / distCfg / native;
				if (!bfs::exists(src, mec)) src = bfs::path(projDir) / "modules" / native;
				// Last resort: the entry verbatim (a hand-written file name).
				if (!bfs::exists(src, mec) && m != native)
				{
					src = rt / "modules" / m;
					if (!bfs::exists(src, mec)) src = bfs::path(projDir) / "modules" / distCfg / m;
					if (!bfs::exists(src, mec)) src = bfs::path(projDir) / "modules" / m;
					if (bfs::exists(src, mec)) shipName = m;
				}
				if (IsEditorOnlyModule(src))
				{
					std::cout << "[Package]\tmodule '" << m << "' is editor-only (imports NukeImGui.dll) — not shipped" << std::endl;
					continue;
				}
				// A Debug binary imports the debug CRT, which no player machine has — a dist
				// with one is dead on arrival. Shout; packaging normally rebuilt these Release.
				{
					std::string bytes;
					bfs::ifstream mf(src, std::ios::binary);
					if (mf) bytes.assign(std::istreambuf_iterator<char>(mf), std::istreambuf_iterator<char>());
					if (distCfg != "Debug" && bytes.find("ucrtbased.dll") != std::string::npos)
						std::cout << "[Package]\tWARNING: module '" << shipName << "' is a DEBUG build (imports ucrtbased.dll)"
						          << " — it will NOT load on machines without Visual Studio. Rebuild it Release." << std::endl;
				}
				if (!CopyOne(src, dist / "modules" / shipName))
					std::cout << "[Package]\tmodule missing, skipped: " << m << std::endl;
			}
			// Module dist extras: relative sources resolve against the shipped runtime dir,
			// absolute as-is; directories copy recursively.
			for (const auto& ed : extraDist)
			{
				bfs::path src = bfs::path(ed.first).is_absolute() ? bfs::path(ed.first) : rt / ed.first;
				bfs::path dst = dist / ed.second;
				if (!bfs::exists(src, ec))
				{
					std::cout << "[Package]\tmodule dist extra MISSING, skipped: " << ed.first << std::endl;
					continue;
				}
				if (bfs::is_directory(src, ec)) CopyTree(src, dst);
				else                            CopyOne(src, dst);
				std::cout << "[Package]\tmodule dist extra: " << ed.second << std::endl;
			}
		}

#ifdef __linux__
		if (ok)
		{
			// Squash the AppDir into <Game>.AppImage. The tool ships inside the editor image
			// (tools/appimagetool), with the env var and PATH as dev-tree fallbacks; without
			// it the loose AppDir still runs (./AppRun) — a notice, not a failure.
			StatusBar::Set("package", "Packaging: AppImage...", 0.95f);
			boost::system::error_code aec;
			std::string tool;
			if (bfs::exists(nuke::RunRoot() / "tools" / "appimagetool", aec))
				tool = (nuke::RunRoot() / "tools" / "appimagetool").string();
			else if (const char* e = std::getenv("NUKE_APPIMAGETOOL"))
				tool = e;
			else if (std::system("command -v appimagetool >/dev/null 2>&1") == 0)
				tool = "appimagetool";
			if (tool.empty())
				std::cout << "[Package]\tappimagetool not found — shipping the loose AppDir (run it via ./AppRun)" << std::endl;
			else
			{
				const bfs::path img = distRoot / (gameName + ".AppImage");
				const std::string cmd = "APPIMAGE_EXTRACT_AND_RUN=1 ARCH=\"$(uname -m)\" \"" + tool
				                      + "\" \"" + dist.string() + "\" \"" + img.string() + "\" >/dev/null 2>&1";
				if (std::system(cmd.c_str()) == 0)
					std::cout << "[Package]\tAppImage ready: " << img.string() << std::endl;
				else
					std::cout << "[Package]\tappimagetool FAILED — shipping the loose AppDir (run it via ./AppRun)" << std::endl;
			}
		}
#endif
		if (!projTweakTmp.empty()) bfs::remove(projTweakTmp, ec);   // served its one pack
		nuke::Jobs::RunOnMain([this, ok, distRoot]()
		{
			{
				boost::system::error_code cec;   // the cooked document copies served their one pack
				bfs::remove_all(bfs::temp_directory_path(cec) / "nuke-cook", cec);
			}
			StatusBar::Remove("package");
			if (ok)
			{
				uint64_t bytes = 0; int count = 0;
				boost::system::error_code ec2;
				for (bfs::recursive_directory_iterator it(distRoot, ec2), end; it != end; it.increment(ec2))
					if (!ec2 && !bfs::is_directory(it->path())) { bytes += bfs::file_size(it->path(), ec2); ++count; }
				std::cout << "[Package]\tdist ready: " << distRoot.string() << " (" << count << " files, "
				          << (bytes / (1024 * 1024)) << " MB)" << std::endl;
			}
			else std::cout << "[Package]\tPACKAGING FAILED — see messages above." << std::endl;
		});
	});
}

void EditorUI::PackageMod(const std::string& modNameIn)
{
	const std::string projDir = projectDir;
	const std::string base = basePakPath;
	// The mod's name is its file name: same name updates that mod, a new name creates one.
	// The fallbacks serve the NUKE_PACKAGE_MOD dev hook's no-arg call.
	std::string name = !modNameIn.empty() ? modNameIn
	                 : !modName.empty()   ? modName
	                 : (projectName.empty() ? std::string("mod") : projectName);
	for (char& c : name) if (strchr("\\/:*?\"<>|", c)) c = '_';   // filename-safe
	const int method = modMethod, level = modLevel, blockMB = pakBlockMB;
	std::string distStr = distPath.empty() ? (bfs::path(projectDir) / "dist").string()
	                    : (bfs::path(distPath).is_absolute() ? distPath
	                                                         : (bfs::path(projectDir) / distPath).string());
	// Dependencies: the mods mounted under this session are what the new mod was authored on.
	// Recorded into mod.json; the loader mounts them below it. Snapshot on the game thread.
	std::vector<std::string> requires_;
	for (const Package::ModInfo& mi : Package::Mods()) requires_.push_back(mi.name);
	// The DLCs mounted in this session: a mod authored over DLC content must not load without it.
	const std::vector<std::string> dlcDeps = Package::MountedDlcs();
	const int splitMode = modSplitMode;      // 0 = one pak, 1 = by content type, 2 = size cap
	const int splitCapMB = modSplitCapMB;

	StatusBar::Set("packmod", "Packaging mod...", StatusBar::kIndeterminate);
	nuke::Jobs::Schedule([projDir, base, name, method, level, blockMB, distStr, requires_, dlcDeps, splitMode, splitCapMB]()
	{
		Package::CreateOptions pakOpts; pakOpts.blockBytes = (uint32_t)blockMB << 20;
		boost::system::error_code ec;
		auto all = CollectProject(projDir, true, DistPrefix(projDir, distStr));

		std::string outPath;
		std::vector<std::pair<std::string, std::string>> files;
		const bool fromMod = base.size() > 6 && base.compare(base.size() - 6, 6, ".numod") == 0;
		const bool fromPak = base.size() > 6 && base.compare(base.size() - 6, 6, ".nupak") == 0;
		if (fromMod)
		{
			files = all;                    // editable mod: repack the whole work tree
			// managed/ is excluded from generic collection, so re-add the mod's own assembly.
			{
				boost::system::error_code sec;
				bfs::path bin = bfs::path(projDir) / "managed" / "bin";
				for (bfs::directory_iterator it(bin, sec), end; it != end && !sec; it.increment(sec))
				{
					std::string fn = it->path().filename().string();
					std::string low = fn;
					for (char& c : low) c = (char)tolower((unsigned char)c);
					if (it->path().extension() != ".dll" || low == "gamescripts.dll") continue;
					if (low.compare(0, 8, "scripts_") != 0) continue;
					files.push_back({ "managed/bin/" + fn, it->path().string() });
				}
			}
			// Same name -> in-place update; a new name -> save-as beside it (retargeted below).
			outPath = (bfs::path(base).parent_path() / (name + ".numod")).string();
		}
		else if (fromPak)
		{
			// Modder flow: only what changed against the session stack (new files + CRC diffs
			// vs the top mounted copy). A changed world also records its basis under
			// "basis/<rel>" so the loader can diff the mod against what its author saw.
			bfs::path basisTmp = bfs::path(projDir) / ".modbasis.tmp";
			bfs::remove_all(basisTmp, ec);
			for (auto& fp : all)
			{
				std::string under;
				if (!Package::ReadMounted(fp.first, under)) { files.push_back(fp); continue; }   // new file
				bfs::ifstream f(bfs::path(fp.second), std::ios::binary);
				std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
				if (Package::Crc32(raw.data(), raw.size()) == Package::Crc32(under.data(), under.size()))
					continue;
				files.push_back(fp);
				std::string lowRel = fp.first;
				for (char& c : lowRel) c = (char)tolower((unsigned char)c);
				if (lowRel.size() > 8 && lowRel.compare(lowRel.size() - 8, 8, ".nuworld") == 0)
				{
					// Basis = every mounted pak layer below the raw overlay, merged as the session merged them.
					std::string basisData = under;
					std::vector<std::pair<std::string, std::string>> wl;
					if (Package::ReadAllInfo(fp.first, wl) > 0)
					{
						std::vector<std::string> ls;
						for (auto& h : wl) if (!h.second.empty()) ls.push_back(h.first);
						if (ls.size() > 1)      basisData = nuke::World::MergeWorldLayers(ls);
						else if (ls.size() == 1) basisData = ls[0];
					}
					bfs::path bt = basisTmp / fp.first;
					bfs::create_directories(bt.parent_path(), ec);
					bfs::ofstream bo(bt, std::ios::binary | std::ios::trunc);
					if (bo)
					{
						bo.write(basisData.data(), (std::streamsize)basisData.size());
						bo.close();
						files.push_back({ "basis/" + fp.first, bt.string() });
					}
				}
			}
			// The session's own Scripts_<session>.dll ships with the mod: unique name, loaded
			// additively by the player. The generic managed/ exclusion otherwise stays.
			{
				boost::system::error_code sec;
				bfs::path bin = bfs::path(projDir) / "managed" / "bin";
				for (bfs::directory_iterator it(bin, sec), end; it != end && !sec; it.increment(sec))
				{
					std::string fn = it->path().filename().string();
					std::string low = fn;
					for (char& c : low) c = (char)tolower((unsigned char)c);
					if (it->path().extension() != ".dll" || low == "gamescripts.dll") continue;
					if (low.compare(0, 8, "scripts_") != 0) continue;
					files.push_back({ "managed/bin/" + fn, it->path().string() });
					std::cout << "[Package]\tmod scripts assembly: " << fn << std::endl;
				}
			}
			// The dist layout keeps the pak in content/ but mods in the ROOT mods/ dir.
			bfs::path gameRoot = bfs::path(base).parent_path();
			if (gameRoot.filename() == "content") gameRoot = gameRoot.parent_path();
			outPath = (gameRoot / "mods" / (name + ".numod")).string();
		}
		else
		{
			files = all;                    // plain raw project: pack its data as a fresh mod
			outPath = (bfs::path(projDir) / (name + ".numod")).string();
		}

		bool ok = false;
		if (files.empty())
			std::cout << "[Package]\tmod: nothing changed against the base — nothing to pack." << std::endl;
		else
		{
			const bool hasNative = PlatformLocked(files);

			auto partsOut = SplitPakFiles(files, splitMode, splitCapMB);
			std::vector<std::string> partFiles;
			for (auto& pr : partsOut) partFiles.push_back(name + "." + pr.first + ".numod");

			// The mod's manifest rides inside it; a repacked .numod keeps its authored requires.
			// Engine plugins the packed content is built on (empty for a pure built-in mod).
			// A plugin the mod SHIPS (its modules/ dir is in the pak) is not an external
			// dependency — listing it would demand a module only this very mod provides.
			std::vector<std::string> modDeps = ModulesForFiles(files);
			modDeps.erase(std::remove_if(modDeps.begin(), modDeps.end(), [&](const std::string& m)
			{
				std::string want = m;
				for (char& c : want) c = (char)tolower((unsigned char)c);
				for (const auto& fp : files)
				{
					std::string low = fp.first;
					for (char& c : low) c = (char)tolower((unsigned char)c);
					if (low.compare(0, 8, "modules/") == 0
					    && bfs::path(low).stem().string() == want) return true;
				}
				return false;
			}), modDeps.end());
			if (!modDeps.empty())
			{
				std::string list;
				for (const std::string& m : modDeps) list += (list.empty() ? "" : ", ") + m;
				std::cout << "[Package]\tmod needs module(s): " << list << std::endl;
			}

			nlohmann::json man;
			man["name"] = name;
			man["requires"] = requires_;
			man["platform"] = hasNative ? Package::CurrentPlatform() : "any";
			if (!dlcDeps.empty()) man["dlc"] = dlcDeps;
			if (!modDeps.empty()) man["modules"] = modDeps;
			if (!partFiles.empty()) man["parts"] = partFiles;
			if (fromMod)
			{
				bfs::ifstream mf(bfs::path(projDir) / "mod.json");
				if (mf)
				{
					nlohmann::json old = nlohmann::json::parse(mf, nullptr, false);
					if (!old.is_discarded() && old.contains("requires")) man["requires"] = old["requires"];
					// An authored list is AUTHORITATIVE: detection proposes, the author disposes —
					// a name can also turn up in a comment, and a repack must not undo the edit.
					// What detection found beyond it is reported, not silently added.
					if (!old.is_discarded() && old.contains("modules") && old["modules"].is_array())
					{
						std::set<std::string> authored;
						for (auto& m : old["modules"]) if (m.is_string()) authored.insert(m.get<std::string>());
						man["modules"] = std::vector<std::string>(authored.begin(), authored.end());
						std::string extra;
						for (const std::string& m : modDeps)
							if (!authored.count(m)) extra += (extra.empty() ? "" : ", ") + m;
						if (!extra.empty())
							std::cout << "[Package]\tmod.json lists its own modules; detection also saw ["
							          << extra << "] — add them there if they are needed." << std::endl;
					}
				}
			}
			bfs::path manTmp = bfs::path(projDir) / ".mod.json.tmp";
			{
				bfs::ofstream mo(manTmp, std::ios::trunc);
				if (mo) mo << man.dump(2);
			}
			files.push_back({ "mod.json", manTmp.string() });
			ok = Package::Create(files, outPath, method, level,
				[](int done, int total)
				{
					StatusBar::Set("packmod", "Packaging mod... " + std::to_string(done) + "/" + std::to_string(total),
					               total ? (float)done / total : 0.0f);
				}, &pakOpts);
			// Parts land beside the main pak with a {"part_of": ...} manifest, so neither the
			// loader nor the UI treats one as a mod of its own.
			for (size_t pi = 0; ok && pi < partsOut.size(); ++pi)
			{
				nlohmann::json pman;
				pman["name"] = name + " (" + partsOut[pi].first + ")";
				pman["part_of"] = name;
				bfs::path pmanTmp = bfs::path(projDir) / (".mod.part" + std::to_string(pi) + ".json.tmp");
				{ bfs::ofstream mo(pmanTmp, std::ios::trunc); if (mo) mo << pman.dump(2); }
				auto pfiles = partsOut[pi].second;
				pfiles.push_back({ "mod.json", pmanTmp.string() });
				const std::string ppath = (bfs::path(outPath).parent_path() / partFiles[pi]).string();
				ok = Package::Create(pfiles, ppath, method, level,
					[](int done, int total)
					{
						StatusBar::Set("packmod", "Packaging mod part... " + std::to_string(done) + "/" + std::to_string(total),
						               total ? (float)done / total : 0.0f);
					}, &pakOpts) && ok;
				bfs::remove(pmanTmp, ec);
				if (ok) std::cout << "[Package]\tmod part: " << ppath << " ("
				                  << partsOut[pi].second.size() << " files)" << std::endl;
			}
			bfs::remove(manTmp, ec);   // the manifest is inside the pak now
			bfs::remove_all(bfs::path(projDir) / ".modbasis.tmp", ec);   // basis copies are inside too
		}

		// A renamed repack retargets the session (marker + live basePakPath) so the next
		// repack updates the new file in place.
		if (ok && fromMod && outPath != base)
		{
			bfs::ofstream mark(bfs::path(projDir) / ".nupak_base", std::ios::trunc);
			if (mark) mark << outPath;
			nuke::Jobs::RunOnMain([outPath]() { EditorUI::getSingleton()->basePakPath = outPath; });
		}

		// A mod packed into a game's mods/ dir self-registers in that game's config.
		if (ok && bfs::path(outPath).parent_path().filename() == "mods")
		{
			bfs::path gameRoot2 = bfs::path(outPath).parent_path().parent_path();
			bfs::path cfg = gameRoot2 / "config" / "mods.json";
			nlohmann::json mj;
			{
				bfs::ifstream in(cfg);
				if (in) mj = nlohmann::json::parse(in, nullptr, false);
			}
			if (mj.is_discarded() || !mj.is_object()) mj = nlohmann::json::object();
			if (!mj.contains("mods") || !mj["mods"].is_array()) mj["mods"] = nlohmann::json::array();
			std::string entry = "mods/" + bfs::path(outPath).filename().string();
			bool present = false;
			for (auto& e : mj["mods"]) if (e.is_string() && e.get<std::string>() == entry) present = true;
			if (!present)
			{
				mj["mods"].push_back(entry);
				bfs::create_directories(cfg.parent_path(), ec);
				bfs::ofstream out(cfg, std::ios::trunc);
				if (out) { out << mj.dump(2); std::cout << "[Package]\tmod ENABLED in " << cfg.string() << std::endl; }
			}
		}

		nuke::Jobs::RunOnMain([ok, outPath, files]()
		{
			StatusBar::Remove("packmod");
			if (ok) std::cout << "[Package]\tmod ready: " << outPath << " (" << files.size()
			                  << " files) — already enabled in the game's config/mods.json." << std::endl;
		});
	});
}

// Native modules the mounted mods/DLCs brought: discover their cache dirs and ENABLE what is
// not running yet. A modder's component must be LIVE in the session — discovered-but-disabled
// means its components load as placeholders, which reads as "the mod is broken". Enabling is
// consent-safe (the mod being mounted IS the consent) and placeholder atoms upgrade in place:
// EnablePlugin already runs World::RestorePluginComponents. The host's own modules were
// discovered at boot, so a name clash resolves to the host.
static void EnableModCacheModules()
{
	for (const std::string& d : Package::ModuleCacheDirs()) nuke::DiscoverModulesIn(d);
	auto norm = [](const std::string& p)
	{
		boost::system::error_code ec;
		std::string s = bfs::absolute(bfs::path(p), ec).generic_string();
		for (char& c : s) c = (char)tolower((unsigned char)c);
		return s;
	};
	for (auto& m : nuke::GetModules())
	{
		if (!m || m->loaded || m->phase() == nuke::PHASE_BOOT) continue;
		const std::string mp = norm(m->modulePath);
		for (const std::string& d : Package::ModuleCacheDirs())
			if (mp.rfind(norm(d), 0) == 0) { nuke::EnablePlugin(m.get()); break; }
	}
}

// ---- Mods panel data (Project Settings "Mods" section) ----
std::string EditorUI::GameRootFromBase() const
{
	if (basePakPath.empty()) return std::string();
	bfs::path r = bfs::path(basePakPath).parent_path();
	if (r.filename() == "content") r = r.parent_path();
	return r.string();
}

void EditorUI::ScanModsUi()
{
	modsUi.clear();
	std::string root = GameRootFromBase();
	if (root.empty()) return;
	auto lower = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
	auto manifest = [](ModRow& r) -> bool {   // false = this pak is a split part, not a mod
		Package::File pf;
		std::string man;
		if (!r.path.empty() && pf.Open(r.path) && pf.Read("mod.json", man))
		{
			nlohmann::json j = nlohmann::json::parse(man, nullptr, false);
			if (!j.is_discarded() && j.is_object())
			{
				if (!j.value("part_of", std::string()).empty()) return false;   // mounted by its main mod
				r.name = j.value("name", r.name);
				if (j.contains("requires") && j["requires"].is_array())
					for (auto& q : j["requires"])
						if (q.is_string())
						{
							r.reqs.push_back(q.get<std::string>());
							r.req += (r.req.empty() ? "" : ", ") + r.reqs.back();
						}
				// Engine plugins: resolved against what is installed and switched on. One the mod
				// itself ships (modules/ in its pak) is always satisfied — it loads with the mod.
				auto shipsIt = [&](const std::string& name)
				{
					std::string want = bfs::path(name).stem().string();
					for (char& c : want) c = (char)tolower((unsigned char)c);
					for (const Package::Entry& e : pf.Entries())
					{
						std::string k = e.path;
						for (char& c : k) c = (char)tolower((unsigned char)c);
						if (k.compare(0, 8, "modules/") != 0) continue;
						std::string st = bfs::path(e.path).stem().string();
						for (char& c : st) c = (char)tolower((unsigned char)c);
						if (st == want) return true;
					}
					return false;
				};
				if (j.contains("modules") && j["modules"].is_array())
					for (auto& q : j["modules"])
						if (q.is_string())
						{
							r.mods.push_back(q.get<std::string>());
							r.modReq += (r.modReq.empty() ? "" : ", ") + r.mods.back();
							if (shipsIt(r.mods.back())) continue;
							bool on = false;
							if (!nuke::ModuleInstalled(r.mods.back(), &on)) r.modsInstalled = false;
							else if (!on) r.modsEnabled = false;
						}
			}
		}
		return true;
	};
	boost::system::error_code ec;
	std::set<std::string> seen;   // lowercase file names already listed (config entries)

	// Enabled mods, in CONFIG order (= load order among independents).
	nlohmann::json mj;
	{
		bfs::ifstream in(bfs::path(root) / "config" / "mods.json");
		if (in) mj = nlohmann::json::parse(in, nullptr, false);
	}
	if (!mj.is_discarded() && mj.contains("mods") && mj["mods"].is_array())
		for (auto& m : mj["mods"])
		{
			if (!m.is_string()) continue;
			std::string entry = m.get<std::string>();
			ModRow r;
			r.enabled = true;
			r.file = bfs::path(entry).filename().string();
			r.name = bfs::path(r.file).stem().string();
			bfs::path cand[] = { bfs::path(entry), bfs::path(root) / entry,
			                     bfs::path(root) / "mods" / entry, bfs::path(root) / "mods" / r.file };
			for (bfs::path& c : cand) if (bfs::exists(c, ec) && !bfs::is_directory(c, ec)) { r.path = c.string(); break; }
			r.found = !r.path.empty();
			if (!manifest(r)) { seen.insert(lower(r.file)); continue; }   // part: rides with its main mod
			for (const std::string& mp : Package::MountedPaks())
			{
				boost::system::error_code ec2;
				if (r.found && bfs::equivalent(bfs::path(mp), bfs::path(r.path), ec2)) { r.mounted = true; break; }
			}
			seen.insert(lower(r.file));
			modsUi.push_back(std::move(r));
		}

	// Disabled mods: .numod files in mods/ that the config doesn't list (parts stay hidden).
	for (bfs::directory_iterator it(bfs::path(root) / "mods", ec), end; it != end && !ec; it.increment(ec))
	{
		if (bfs::is_directory(it->path()) || it->path().extension() != ".numod") continue;
		if (seen.count(lower(it->path().filename().string()))) continue;
		ModRow r;
		r.path = it->path().string();
		r.file = it->path().filename().string();
		r.name = it->path().stem().string();
		if (!manifest(r)) continue;
		modsUi.push_back(std::move(r));
	}

	// The editor's own selection (editor_mods.json in the session overlay) is separate from
	// the game's config: these are the mods THIS session mounts.
	{
		std::set<std::string> ed;
		bfs::ifstream ef(bfs::path(projectDir) / "editor_mods.json");
		if (ef)
		{
			nlohmann::json ej = nlohmann::json::parse(ef, nullptr, false);
			if (!ej.is_discarded() && ej.contains("mods") && ej["mods"].is_array())
				for (auto& m : ej["mods"])
					if (m.is_string()) ed.insert(lower(bfs::path(m.get<std::string>()).filename().string()));
		}
		for (ModRow& r : modsUi) r.edMounted = ed.count(lower(r.file)) != 0;
	}
}

// Persist the editor's mod selection and remount the session stack to match. Worlds pick
// the change up on their next open.
void EditorUI::SaveEditorMods()
{
	std::string root = GameRootFromBase();
	if (root.empty() || basePakPath.empty()) return;
	nlohmann::json ej;
	ej["mods"] = nlohmann::json::array();
	std::vector<std::string> entries;
	for (const ModRow& r : modsUi)
		if (r.edMounted) { ej["mods"].push_back("mods/" + r.file); entries.push_back("mods/" + r.file); }
	boost::system::error_code ec;
	bfs::ofstream out(bfs::path(projectDir) / "editor_mods.json", std::ios::trunc);
	if (out) out << ej.dump(2);
	Package::UnmountAll();
	Package::Mount(basePakPath, 0);
	Package::MountPakParts(basePakPath, 0);
	// The DLC layer is part of the game the modder targets.
	Package::PakInfo basePi;
	Package::ReadPakInfo(basePakPath, basePi);
	Package::MountDlcs(root, basePi.name);
	int n = Package::MountModList(root, entries);   // empty list still clears the mod metadata
	EnableModCacheModules();   // mod-shipped native modules go LIVE now, not on the next restart
	std::cout << "[editor]\t\teditor mods remounted (base + " << n << ") — reopen the world to apply" << std::endl;
	modsUiTick = -1;   // rescan (mounted flags)
}

void EditorUI::SaveModsUi()
{
	std::string root = GameRootFromBase();
	if (root.empty()) return;
	nlohmann::json mj;
	mj["mods"] = nlohmann::json::array();
	for (const ModRow& r : modsUi)
		if (r.enabled) mj["mods"].push_back("mods/" + r.file);
	boost::system::error_code ec;
	bfs::create_directories(bfs::path(root) / "config", ec);
	bfs::ofstream out(bfs::path(root) / "config" / "mods.json", std::ios::trunc);
	if (out) out << mj.dump(2);
	modsUiTick = -1;   // rescan next frame (mounted flags etc.)
}

void EditorUI::PackageModCmd()
{
	std::string pre = modName;                     // last packaged name (repack updates it)
	if (pre.empty() && basePakPath.size() > 6 && basePakPath.compare(basePakPath.size() - 6, 6, ".numod") == 0)
		pre = bfs::path(basePakPath).stem().string();   // editing an existing mod: its file name
	if (pre.empty()) pre = "MyMod";
	strncpy(packModName, pre.c_str(), sizeof(packModName) - 1);
	packModName[sizeof(packModName) - 1] = 0;
	openPackageModPopup = true;
}

void EditorUI::DrawPackageModPopup()
{
	if (openPackageModPopup)
	{
		ImGui::OpenPopup("Package Mod");
		openPackageModPopup = false;
	}
	if (ImGui::BeginPopupModal("Package Mod", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::SetNextItemWidth(300);
		ImGui::InputText("Mod name", packModName, sizeof(packModName));
		std::string name = packModName;
		bool nameOk = !name.empty();
		for (char c : name) nameOk &= (std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == ' ');

		// Where it lands — the same rules PackageMod applies.
		const bool fromMod = basePakPath.size() > 6 && basePakPath.compare(basePakPath.size() - 6, 6, ".numod") == 0;
		const bool fromPak = basePakPath.size() > 6 && basePakPath.compare(basePakPath.size() - 6, 6, ".nupak") == 0;
		bfs::path target;
		if (fromMod) target = bfs::path(basePakPath).parent_path() / (name + ".numod");
		else if (fromPak)
		{
			bfs::path gameRoot = bfs::path(basePakPath).parent_path();
			if (gameRoot.filename() == "content") gameRoot = gameRoot.parent_path();
			target = gameRoot / "mods" / (name + ".numod");
		}
		else target = bfs::path(projectDir) / (name + ".numod");

		boost::system::error_code ec;
		if (nameOk) ImGui::TextDisabled("-> %s", target.string().c_str());
		if (!nameOk) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1), "Name: letters, digits, space, _ and - only.");
		else if (bfs::exists(target, ec))
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1), "A mod with this name exists - it will be updated.");

		// Same setting as Project Settings -> Packaging; edited here it persists too.
		ImGui::SetNextItemWidth(300);
		if (ImGui::Combo("Split", &modSplitMode, "None (single file)\0By content type (textures/audio/meshes)\0Size cap per file\0"))
			SaveProject();
		if (modSplitMode == 2)
		{
			ImGui::SetNextItemWidth(300);
			if (ImGui::InputInt("Cap (MB)", &modSplitCapMB, 64, 256))
			{
				if (modSplitCapMB < 16) modSplitCapMB = 16;
				SaveProject();
			}
		}

		ImGui::Separator();
		ImGui::BeginDisabled(!nameOk);
		if (ImGui::Button("Package", ImVec2(120, 0)))
		{
			modName = name;
			SaveProject();          // persist: the same name updates THIS mod next time
			PackageMod(name);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

// --- Package DLC ----
// A .nupak carrying only what is new/changed vs a shipped base pak; lands in that game's
// content/dlc/ and mounts between the base and the mods. Built from the raw project only.

// Open the modal, prefilling the DLC name and the base pak from this project's own dist.
void EditorUI::PackageDlcCmd()
{
	if (packDlcName[0] == 0)
	{
		strncpy(packDlcName, "DLC1", sizeof(packDlcName) - 1);
		packDlcName[sizeof(packDlcName) - 1] = 0;
	}
	if (packDlcBase[0] == 0)
	{
		bfs::path guess = (distPath.empty() ? bfs::path(projectDir) / "dist"
		                  : (bfs::path(distPath).is_absolute() ? bfs::path(distPath)
		                                                       : bfs::path(projectDir) / distPath))
		                  / "content" / "game.nupak";
		boost::system::error_code ec;
		if (bfs::exists(guess, ec))
		{
			strncpy(packDlcBase, guess.string().c_str(), sizeof(packDlcBase) - 1);
			packDlcBase[sizeof(packDlcBase) - 1] = 0;
		}
	}
	openPackageDlcPopup = true;
}

void EditorUI::DrawPackageDlcPopup()
{
	if (openPackageDlcPopup)
	{
		ImGui::OpenPopup("Package DLC");
		openPackageDlcPopup = false;
	}
	if (ImGui::BeginPopupModal("Package DLC", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::SetNextItemWidth(300);
		ImGui::InputText("DLC name", packDlcName, sizeof(packDlcName));
		ImGui::SetNextItemWidth(300);
		ImGui::InputText("Base game.nupak", packDlcBase, sizeof(packDlcBase));
		ImGui::SameLine();
		if (ImGui::Button("Browse..."))
		{
			std::string p = EditorPickProjectFile();
			if (!p.empty()) { strncpy(packDlcBase, p.c_str(), sizeof(packDlcBase) - 1); packDlcBase[sizeof(packDlcBase) - 1] = 0; }
		}
		std::string name = packDlcName;
		bool nameOk = !name.empty();
		for (char c : name) nameOk &= (std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == ' ');
		boost::system::error_code ec;
		const bool baseOk = packDlcBase[0] != 0 && bfs::exists(bfs::path(packDlcBase), ec)
		                 && bfs::path(packDlcBase).extension() == ".nupak";
		if (!nameOk)      ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1), "Name: letters, digits, space, _ and - only.");
		else if (!baseOk) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1), "Pick the SHIPPED base game.nupak this DLC extends.");
		else
		{
			bfs::path gameRoot = bfs::path(packDlcBase).parent_path();
			if (gameRoot.filename() == "content") gameRoot = gameRoot.parent_path();
			ImGui::TextDisabled("-> %s", (gameRoot / "content" / "dlc" / (name + ".nupak")).string().c_str());
			ImGui::TextDisabled("Ships ONLY what's new/changed vs the base. Not editable by players/modders.");
		}
		ImGui::Separator();
		ImGui::BeginDisabled(!nameOk || !baseOk);
		if (ImGui::Button("Package", ImVec2(120, 0)))
		{
			PackageDlc(name, packDlcBase);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

void EditorUI::PackageDlc(const std::string& dlcNameIn, const std::string& basePakIn)
{
	if (!basePakPath.empty())
	{
		std::cout << "[Package]\tDLC packs from the RAW developer project only." << std::endl;
		return;
	}
	std::string name = dlcNameIn.empty() ? std::string("DLC") : dlcNameIn;
	for (char& c : name) if (strchr("\\/:*?\"<>|", c)) c = '_';   // filename-safe
	// Game-thread snapshots: the cooker runs on a worker.
	const std::string projDir = projectDir, projFile = projectFile, content = contentDir;
	const std::string base = basePakIn;
	const int method = pakMethod, level = pakLevel, blockMB = pakBlockMB;
	const int splitMode = modSplitMode, splitCapMB = modSplitCapMB;
	std::string distStr = distPath.empty() ? (bfs::path(projectDir) / "dist").string()
	                    : (bfs::path(distPath).is_absolute() ? distPath
	                                                         : (bfs::path(projectDir) / distPath).string());
	std::map<std::string, std::vector<std::string>> guidFiles;
	{
		ResDB* db = ResDB::getSingleton();
		for (auto& kv : db->pathByGuid) guidFiles[kv.first].push_back(kv.second);
		for (nuke::Shader* sh : db->shaders)
			if (sh && !sh->vsPath.empty())
			{
				guidFiles[sh->guid].push_back(sh->vsPath);
				if (!sh->psPath.empty()) guidFiles[sh->guid].push_back(sh->psPath);
			}
			else if (sh && sh->isPost && !sh->psPath.empty())
				guidFiles[sh->guid].push_back(sh->psPath);
	}
	StatusBar::Set("packdlc", "Packaging DLC...", StatusBar::kIndeterminate);
	nuke::Jobs::Schedule([projDir, projFile, content, base, name, method, level, blockMB, splitMode, splitCapMB, distStr, guidFiles]()
	{
		Package::CreateOptions pakOpts; pakOpts.blockBytes = (uint32_t)blockMB << 20;
		boost::system::error_code ec;
		Package::File bf;
		if (!bf.Open(base))
		{
			std::cout << "[Package]\tDLC: base pak unreadable: " << base << std::endl;
			nuke::Jobs::RunOnMain([]() { StatusBar::Remove("packdlc"); });
			return;
		}
		Package::PakInfo basePi;
		Package::ReadPakInfo(base, basePi);   // legacy base -> empty name (folder-bound DLC)

		// Cook like Package Project, then keep only new and crc-changed files.
		auto all = CollectProject(projDir, false, DistPrefix(projDir, distStr));
		std::set<std::string> used = CookUsedFiles(projDir, content, projFile, guidFiles);
		std::vector<std::pair<std::string, std::string>> cooked;
		for (auto& fp : all)
		{
			if (used.count(DiskKey(bfs::path(fp.second)))) { cooked.push_back(fp); continue; }
			std::string low = fp.first;
			for (char& c : low) c = (char)tolower((unsigned char)c);
			auto ends = [&](const char* ext) {
				const size_t n = strlen(ext);
				return low.size() > n && low.compare(low.size() - n, n, ext) == 0;
			};
			if (ends(".hlsl") || ends(".nuinput")) cooked.push_back(fp);
		}
		cooked.push_back({ "game.nuproj", projFile });   // the manifest rides too (world lists grew)
		std::vector<std::pair<std::string, std::string>> files;
		for (auto& fp : cooked)
		{
			const Package::Entry* e = bf.Find(fp.first);
			if (!e) { files.push_back(fp); continue; }   // new
			bfs::ifstream f(bfs::path(fp.second), std::ios::binary);
			std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			if (Package::Crc32(raw.data(), raw.size()) != e->crc) files.push_back(fp);   // changed
		}

		bool ok = false;
		std::string outPath;
		if (files.empty())
			std::cout << "[Package]\tDLC: nothing new against the base — nothing to pack." << std::endl;
		else
		{
			auto partsOut = SplitPakFiles(files, splitMode, splitCapMB);
			std::vector<std::string> partFiles;
			for (auto& pr : partsOut) partFiles.push_back(name + "." + pr.first + ".nupak");
			nlohmann::json pj;
			pj["kind"] = "dlc";
			pj["name"] = name;
			pj["base"] = basePi.name;   // "" for a legacy base: folder placement binds it
			pj["platforms"] = nlohmann::json::array({ Package::CurrentPlatform() });
			// Same contract as a mod: record the engine plugins this content needs.
			{
				const std::vector<std::string> modDeps = ModulesForFiles(files);
				if (!modDeps.empty())
				{
					pj["modules"] = modDeps;
					std::string list;
					for (const std::string& m : modDeps) list += (list.empty() ? "" : ", ") + m;
					std::cout << "[Package]\tDLC needs module(s): " << list << std::endl;
				}
			}
			if (!partFiles.empty()) pj["parts"] = partFiles;
			bfs::path manTmp = bfs::path(projDir) / ".dlc.json.tmp";
			{
				bfs::ofstream mo(manTmp, std::ios::binary | std::ios::trunc);
				if (mo) mo << pj.dump(2);
			}
			files.push_back({ "pak.json", manTmp.string() });
			bfs::path gameRoot = bfs::path(base).parent_path();
			if (gameRoot.filename() == "content") gameRoot = gameRoot.parent_path();
			bfs::path outDir = gameRoot / "content" / "dlc";
			bfs::create_directories(outDir, ec);
			outPath = (outDir / (name + ".nupak")).string();
			ok = Package::Create(files, outPath, method, level,
				[](int done, int total)
				{
					StatusBar::Set("packdlc", "Packaging DLC... " + std::to_string(done) + "/" + std::to_string(total),
					               total ? (float)done / total : 0.0f);
				}, &pakOpts);
			bfs::remove(manTmp, ec);
			for (size_t pi = 0; ok && pi < partsOut.size(); ++pi)
			{
				nlohmann::json ppj;
				ppj["kind"] = "part";
				ppj["name"] = name + " (" + partsOut[pi].first + ")";
				ppj["part_of"] = name;
				bfs::path ptmp = bfs::path(projDir) / (".dlc.part" + std::to_string(pi) + ".json.tmp");
				{ bfs::ofstream po(ptmp, std::ios::binary | std::ios::trunc); if (po) po << ppj.dump(2); }
				auto pfiles = partsOut[pi].second;
				pfiles.push_back({ "pak.json", ptmp.string() });
				ok = Package::Create(pfiles, (outDir / partFiles[pi]).string(), method, level,
					[](int done, int total)
					{
						StatusBar::Set("packdlc", "Packaging DLC part... " + std::to_string(done) + "/" + std::to_string(total),
						               total ? (float)done / total : 0.0f);
					}, &pakOpts) && ok;
				bfs::remove(ptmp, ec);
				if (ok) std::cout << "[Package]\tDLC part: " << partFiles[pi] << " ("
				                  << partsOut[pi].second.size() << " files)" << std::endl;
			}
		}
		nuke::Jobs::RunOnMain([ok, outPath, name]()
		{
			StatusBar::Remove("packdlc");
			if (ok) std::cout << "[Package]\tDLC ready: " << outPath
			                  << " — mounts automatically from content/dlc/ at game boot." << std::endl;
		});
	});
}

// --- Game Build dialog (File -> Package Project) ----
// Nothing is stored in the project: the dialog only tweaks the config written into dist at
// packaging time, and pre-fills from the previous dist config so tweaks carry over.

void EditorUI::PackageProjectCmd()
{
	// Archive sessions can't package a project — reuse the standard refusal message.
	if (!basePakPath.empty()) { PackageProjectNow(); return; }

	// Dialog model: the editor's live config overlaid with the previous dist config.
	// Log/debug are GAME defaults (off), not the editor's values.
	nuke::NukeWindow w = nuke::Config::getSingleton()->window;
	gbLog = false; gbDebug = false; gbConsole = false;
	{
		bfs::path dist = distPath.empty() ? (bfs::path(projectDir) / "dist")
		               : (bfs::path(distPath).is_absolute() ? bfs::path(distPath)
		                                                    : bfs::path(projectDir) / distPath);
		try
		{
			bfs::ifstream in(dist / "config" / "main.json");
			if (in)
			{
				std::stringstream ss; ss << in.rdbuf();
				nlohmann::json p = nlohmann::json::parse(ss.str(), nullptr, false, true);
				if (p.is_object())
				{
					gbLog     = p.value("logToConsole",  false);
					gbDebug   = p.value("gpuValidation", false);
					gbConsole = p.value("devConsole",    false);
				}
				if (p.is_object() && p.contains("window") && p["window"].is_object())
				{
					const nlohmann::json& j = p["window"];
					w.w           = j.value("width",       w.w);
					w.h           = j.value("height",      w.h);
					w.resizable   = j.value("resizable",   w.resizable);
					// mode: human-readable word, with legacy number/bool configs still accepted.
					if (j.contains("mode") && j["mode"].is_string())
					{
						const std::string m = j["mode"].get<std::string>();
						w.mode = (m == "borderless") ? 1 : (m == "exclusive") ? 2 : 0;
					}
					else if (j.contains("mode") && j["mode"].is_number())
						w.mode = j["mode"].get<int>();
					else
						w.mode = j.value("fullscreen", false) ? 2 : 0;
					w.fullscreen  = w.mode != 0;
					w.transparent = j.value("transparent", w.transparent);
					w.opacity     = j.value("opacity",     w.opacity);
					w.backend     = j.value("backend",     w.backend);
					w.rayTracing  = j.value("rayTracing",  w.rayTracing);
					w.showFps     = j.value("showFps",     w.showFps);
					w.vsync       = j.value("vsync",       w.vsync);
					w.showConsole = j.value("showConsole", w.showConsole);
					w.fpsLimit    = j.value("fpsLimit",    w.fpsLimit);
				}
			}
		}
		catch (...) {}
	}
	gbWin = w;
	openPackageProjectPopup = true;
}

void EditorUI::DrawPackageProjectPopup()
{
	if (openPackageProjectPopup)
	{
		ImGui::OpenPopup("Game Build");
		openPackageProjectPopup = false;
	}
	if (!ImGui::BeginPopupModal("Game Build", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

	{
		// Full-Debug dists exist for developers: everything (engine, player, modules, the
		// project's own game modules) is bundled from the chosen configuration.
		const char* cfgModes[] = { "Release (ship)", "Debug (dev — needs the debug CRT / VS on the target)" };
		ImGui::Combo("Build", &gbBuildCfg, cfgModes, IM_ARRAYSIZE(cfgModes));
		ImGui::SameLine(); ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Which build of the game ships: Release for players,\n"
		                                              "Debug for asserts + debugging on machines with Visual Studio.");
#ifdef _WIN32
		const char* beModes[] = { "Direct3D 11", "Direct3D 12 (DirectStorage)", "Vulkan" };
		ImGui::Combo("Render Backend", &gbWin.backend, beModes, IM_ARRAYSIZE(beModes));
		ImGui::SameLine(); ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Backend of the PACKAGED game.\n"
		                                              "D3D12 additionally brings DirectStorage (GDeflate paks inflate on the GPU,\n"
		                                              "textures land straight in VRAM), window transparency and HDR10.\n"
		                                              "The EDITOR's own backend is in Preferences.");
#else
		// This dialog packages for the CURRENT platform, and off Windows that means Vulkan —
		// no dead D3D entries. The player heals a foreign config's backend at boot anyway.
		gbWin.backend = 2;
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Render Backend"); ImGui::SameLine(ImGui::GetFontSize() * 13.0f);
		ImGui::TextDisabled("Vulkan (the only backend on this OS)");
#endif
	}
	ImGui::Checkbox("Ray Tracing", &gbWin.rayTracing);
	ImGui::SameLine(); ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off = force the raster path (shadow maps/SSR) even on RT-capable GPUs.");
	{
		ImGui::SetNextItemWidth(110.0f);
		ImGui::InputInt("##gb_w", &gbWin.w, 0);
		ImGui::SameLine(); ImGui::TextUnformatted("x"); ImGui::SameLine();
		ImGui::SetNextItemWidth(110.0f);
		ImGui::InputInt("Resolution##gb_h", &gbWin.h, 0);
	}
	{
		const char* wModes[] = { "Windowed", "Borderless Fullscreen", "Exclusive Fullscreen" };
		ImGui::Combo("Display Mode", &gbWin.mode, wModes, IM_ARRAYSIZE(wModes));
	}
	ImGui::Checkbox("Resizable", &gbWin.resizable);
	ImGui::Checkbox("VSync", &gbWin.vsync);
	ImGui::SetNextItemWidth(110.0f);
	ImGui::InputInt("FPS Limit", &gbWin.fpsLimit, 0);
	ImGui::SameLine(); ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Manual frame cap for the shipped game (0 = uncapped).\n"
	                                              "VSync still applies on top; Game.SetFpsLimit overrides live.");
	ImGui::Checkbox("Show OS Console", &gbWin.showConsole);
	ImGui::SameLine(); ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("The process's own log window. Off for a shipped game.");
	ImGui::Checkbox("Show FPS", &gbWin.showFps);
	ImGui::SameLine(); ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Append an FPS readout to the game window's title.");
	ImGui::Checkbox("Log", &gbLog);
	ImGui::SameLine(); ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("logToConsole: echo the game's log to its OS console.\n"
	                                              "Costs frame time under heavy logging — off for a shipped game.");
	ImGui::Checkbox("Debug", &gbDebug);
	ImGui::SameLine(); ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("gpuValidation: the GPU debug/validation layer (Debug builds only;\n"
	                                              "can more than halve FPS). Only for diagnosing renderer crashes.");
	ImGui::Checkbox("Developer Console", &gbConsole);
	ImGui::SameLine(); ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("The in-game console on ` (grave): reflected commands, plus Lua when a\n"
	                                              "scripting backend ships. Ships the GUI backend with the game even when\n"
	                                              "the game itself has no UI. Off for a shipped game.");
	ImGui::Checkbox("Transparent Window", &gbWin.transparent);
	ImGui::SameLine(); ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Per-pixel window alpha (needs D3D12; creation-time property).");
	ImGui::SliderFloat("Window Opacity", &gbWin.opacity, 0.1f, 1.0f, "%.2f");

	ImGui::TextDisabled("Written into the dist's config/main.json at packaging.");
	ImGui::Separator();
	if (ImGui::Button("Package", ImVec2(140, 0)))
	{
		if (gbWin.w < 64) gbWin.w = 64;
		if (gbWin.h < 64) gbWin.h = 64;
		if (gbWin.opacity < 0.1f) gbWin.opacity = 0.1f;
		if (gbWin.opacity > 1.0f) gbWin.opacity = 1.0f;
		if (gbWin.fpsLimit < 0) gbWin.fpsLimit = 0;
		gbWin.fullscreen = gbWin.mode != 0;
		gbWinSet = true;   // the packaging worker overrides the shipped window block with gbWin
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		PackageProject();   // Release build first, then pack
		return;
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

// Open-with for a packed project (.nupak): mount it read-only and put the editing session
// into a "<stem>_mod" overlay beside it (the raw layer wins over mounts), carrying over only
// the manifest. Returns the work project's .nuproj, "" on failure.
std::string EditorUI::PrepareMountedProject(const std::string& pakAbs)
{
	// A DLC pak is a point diff over a base game, never editable as a project.
	{
		Package::PakInfo pi;
		if (Package::ReadPakInfo(pakAbs, pi) && pi.kind == "dlc")
		{
			std::cout << "[Package]\t'" << bfs::path(pakAbs).filename().string()
			          << "' is a DLC pak — not editable. Open the base game's pak (modding) or "
			          << "the developer's raw project instead." << std::endl;
			return std::string();
		}
	}
	if (!Package::Mount(pakAbs, 0))
	{
		std::cout << "[Package]\tcan't open archive: " << pakAbs << std::endl;
		return std::string();
	}
	Package::MountPakParts(pakAbs, 0);
	boost::system::error_code ec;
	bfs::path pak(pakAbs);
	bfs::path work = pak.parent_path() / (pak.stem().string() + "_mod");
	bfs::create_directories(work / "content", ec);
	// Base only: config/mods.json is the PLAYER's list, so the session mounts mods only per
	// the editor's own editor_mods.json.
	{
		bfs::path gameRoot = pak.parent_path();
		if (gameRoot.filename() == "content") gameRoot = gameRoot.parent_path();
		// The DLC layer is part of the game a modder authors against.
		{
			Package::PakInfo basePi;
			Package::ReadPakInfo(pakAbs, basePi);
			Package::MountDlcs(gameRoot.string(), basePi.name);
		}
		std::vector<std::string> em;
		bfs::ifstream ef(work / "editor_mods.json");
		if (ef)
		{
			nlohmann::json ej = nlohmann::json::parse(ef, nullptr, false);
			if (!ej.is_discarded() && ej.contains("mods") && ej["mods"].is_array())
				for (auto& m : ej["mods"]) if (m.is_string()) em.push_back(m.get<std::string>());
		}
		if (!em.empty())
		{
			int n = Package::MountModList(gameRoot.string(), em);
			if (n > 0) std::cout << "[Package]\tsession: base + " << n << " EDITOR-selected mod(s)" << std::endl;
			EnableModCacheModules();
		}
	}
	if (!bfs::exists(work / "game.nuproj", ec))
	{
		std::string manifest;
		if (!Package::Read("game.nuproj", manifest) || manifest.empty())
			manifest = "{\n  \"name\": \"" + pak.stem().string() + "\",\n  \"content\": \"content\"\n}\n";
		bfs::ofstream mj(work / "game.nuproj");
		if (mj) mj << manifest;
	}
	{
		bfs::ofstream mark(work / ".nupak_base", std::ios::trunc);   // base pointer (PackageMod diff)
		if (mark) mark << pakAbs;
	}
	std::cout << "[Package]\tmounted session: " << pak.filename().string()
	          << " (read-only) + overlay " << work.filename().string() << std::endl;
	return (work / "game.nuproj").string();
}

// Open-with for a mod (.numod): materialize it into "<stem>_project" beside the pak (reused
// unless the pak is newer), synthesize a manifest if it has none, and remember the base so
// Package Mod repacks it in place. Returns the work project's .nuproj, "" on failure.
std::string EditorUI::PrepareArchiveProject(const std::string& pakAbs)
{
	Package::File pf;
	if (!pf.Open(pakAbs)) { std::cout << "[Package]\tcan't open archive: " << pakAbs << std::endl; return std::string(); }

	boost::system::error_code ec;
	bfs::path pak(pakAbs);
	bfs::path work = pak.parent_path() / (pak.stem().string() + "_project");
	std::time_t pakTime = bfs::last_write_time(pak, ec);
	const bool fresh = !bfs::exists(work / "game.nuproj", ec)
	                && !bfs::exists(work / ".nupak_base", ec);
	std::time_t markTime = fresh ? 0 : bfs::last_write_time(work / ".nupak_base", ec);

	if (fresh || pakTime > markTime)
	{
		std::cout << "[Package]\textracting " << pak.filename().string() << " -> " << work.string() << std::endl;
		for (const Package::Entry& e : pf.Entries())
		{
			std::string raw;
			if (!pf.Read(e.path, raw)) { std::cout << "[Package]\tcorrupt entry: " << e.path << std::endl; return std::string(); }
			bfs::path dst = work / e.path;
			if (dst.has_parent_path()) bfs::create_directories(dst.parent_path(), ec);
			bfs::ofstream o(dst, std::ios::binary | std::ios::trunc);
			if (!o) { std::cout << "[Package]\tcan't write " << dst.string() << std::endl; return std::string(); }
			if (!raw.empty()) o.write(raw.data(), (std::streamsize)raw.size());
		}
	}
	// A mod inside a game's mods/ dir edits on top of that game: mount the base pak and only
	// this mod's own "requires" chain — not the player's whole enabled stack.
	{
		bfs::path gameRoot = pak.parent_path();
		if (gameRoot.filename() == "mods")
		{
			gameRoot = gameRoot.parent_path();
			bfs::path basePak = gameRoot / "content" / "game.nupak";
			if (bfs::exists(basePak, ec) && Package::Mount(basePak.string(), 0))
			{
				Package::MountPakParts(basePak.string(), 0);
				// The game's DLC layer is part of what this mod was authored on top of.
				{
					Package::PakInfo basePi;
					Package::ReadPakInfo(basePak.string(), basePi);
					Package::MountDlcs(gameRoot.string(), basePi.name);
				}
				// name -> "mods/<file>" map from the game's mods dir manifests.
				std::map<std::string, std::string> byName;
				std::map<std::string, std::string> fileByName;   // for reading deps' manifests
				auto lower = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
				boost::system::error_code mec;
				for (bfs::directory_iterator it(gameRoot / "mods", mec), end; it != end && !mec; it.increment(mec))
				{
					if (bfs::is_directory(it->path()) || it->path().extension() != ".numod") continue;
					std::string nm = it->path().stem().string();
					Package::File mpf;
					std::string man;
					if (mpf.Open(it->path().string()) && mpf.Read("mod.json", man))
					{
						nlohmann::json j = nlohmann::json::parse(man, nullptr, false);
						if (!j.is_discarded() && j.is_object()) nm = j.value("name", nm);
					}
					byName[lower(nm)]     = "mods/" + it->path().filename().string();
					fileByName[lower(nm)] = it->path().string();
				}
				// The opened mod's requires closure (breadth-first; missing deps just log).
				std::vector<std::string> chain;
				std::set<std::string> seen;
				std::vector<std::string> queue;
				{
					bfs::ifstream mf(work / "mod.json");
					nlohmann::json j = mf ? nlohmann::json::parse(mf, nullptr, false) : nlohmann::json();
					if (!j.is_discarded() && j.is_object() && j.contains("requires") && j["requires"].is_array())
						for (auto& r : j["requires"]) if (r.is_string()) queue.push_back(r.get<std::string>());
				}
				while (!queue.empty())
				{
					std::string name = lower(queue.back());
					queue.pop_back();
					if (seen.count(name)) continue;
					seen.insert(name);
					auto bit = byName.find(name);
					if (bit == byName.end())
					{
						std::cout << "[Package]\tmod dependency '" << name << "' not found in mods/ — skipped" << std::endl;
						continue;
					}
					chain.push_back(bit->second);
					Package::File dpf;
					std::string man;
					if (dpf.Open(fileByName[name]) && dpf.Read("mod.json", man))
					{
						nlohmann::json j = nlohmann::json::parse(man, nullptr, false);
						if (!j.is_discarded() && j.is_object() && j.contains("requires") && j["requires"].is_array())
							for (auto& r : j["requires"]) if (r.is_string()) queue.push_back(r.get<std::string>());
					}
				}
				int n = chain.empty() ? 0 : Package::MountModList(gameRoot.string(), chain);
				EnableModCacheModules();
				std::cout << "[Package]\tmod session: base game + " << n << " required dep(s) mounted under the work tree" << std::endl;
				if (!bfs::exists(work / "game.nuproj", ec))
				{
					std::string manifest;
					if (Package::Read("game.nuproj", manifest) && !manifest.empty())
					{
						bfs::ofstream mj(work / "game.nuproj");
						if (mj) mj << manifest;
					}
				}
			}
		}
	}
	if (!bfs::exists(work / "game.nuproj", ec))   // a standalone mod carries no manifest — make one
	{
		bfs::ofstream mj(work / "game.nuproj");
		if (mj) mj << "{\n  \"name\": \"" << pak.stem().string() << "\",\n  \"content\": \"content\"\n}\n";
	}
	{
		bfs::ofstream mark(work / ".nupak_base", std::ios::trunc);   // base pointer + freshness stamp
		if (mark) mark << pakAbs;
	}
	return (work / "game.nuproj").string();
}
