// packaging panel (roadmap 3.2) — EditorUI method definitions (translation unit).
//
// "Package Project" builds the COMPLETE release under <project>/dist/: NukePlayer.exe +
// its runtime DLLs, engine shaders/fonts/config, only the USED modules, an empty mods/
// + config/mods.json, and the project itself packed into content/game.nupak (immutable,
// max compression). The dist folder must simply RUN.
//
// "Package Mod" builds a .numod overlay (editable, store by default): opened-from-.nupak
// projects pack the CRC-DIFF of the work tree against the base pak (the modder flow);
// opened-from-.numod projects repack IN PLACE; plain raw projects pack their content.
#include <editor/editorui.h>
#include <API/Model/Package.h>
#include <API/Model/Jobs.h>
#include <API/Model/StatusBar.h>
#include <API/Model/World.h>    // mod basis: merge the stack the modder authored on
#include <nlohmann/json.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <algorithm>
#include <set>
#include <sstream>
#include <cstring>   // strchr (mod-name sanitizing)
#include <cctype>    // isalnum (mod-name validation)
#ifdef _WIN32
#include <Windows.h>   // UpdateResource (game icon on the shipped exe)
#endif
#define STB_IMAGE_IMPLEMENTATION   // PNG-compressed .ico entries (this is the editor's ONLY stb TU)
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

// Does this project-relative path belong in a pak? (dev noise, outputs and nested
// archives never do; `forMod` also drops the manifest — a mod must not override it
// by accident, only game DATA; `distPrefix` = the configured build dir when it sits
// inside the project, lowercased with a trailing '/').
static bool PackWorthy(const std::string& relLower, bool forMod, const std::string& distPrefix)
{
	if (relLower.empty()) return false;
	auto starts = [&](const char* p) { return relLower.compare(0, strlen(p), p) == 0; };
	if (starts("dist/") || starts("mods/") || starts(".git")) return false;
	// managed/ is BUILD OUTPUT (compiled game scripts + csproj/obj junk), never data. The
	// game pak takes GameScripts.dll EXPLICITLY (shipExtras); a MOD must never sweep a
	// stale copy in — a mounted mod's assembly would shadow the game's and "wipe" its
	// scripts (mods layer above the base pak on reads).
	if (starts("managed/")) return false;
	if (starts(".modbasis.tmp")) return false;   // Package Mod scratch (cleaned, but never data)
	if (!distPrefix.empty() && relLower.compare(0, distPrefix.size(), distPrefix) == 0) return false;
	if (relLower == "editor_state.json" || relLower == ".nupak_base") return false;
	if (relLower == "mods.json") return false;
	// The mod manifest is GENERATED at pack time (name + requires) — never collected as
	// data (an extracted .numod work tree carries the original one on disk).
	if (relLower == "mod.json") return false;
	auto ends = [&](const char* s) { size_t n = strlen(s); return relLower.size() >= n && relLower.compare(relLower.size() - n, n, s) == 0; };
	if (ends(".log") || ends(".err") || ends(".bak") || ends(".nupak") || ends(".numod") || ends(".pdb") || ends(".tmp")) return false;
	if (relLower.find(".cache/") != std::string::npos) return false;   // materialization caches
	// Manifests never collect implicitly: the PROJECT pak stores the active one under the
	// canonical name "game.nuproj" (PackageProject adds it), and mods carry none at all.
	if (ends(".nuproj")) return false;
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

// Standard .ico layout: ICONDIR + entries + image payloads (shared by the exe stamper and
// the settings preview decoder).
#pragma pack(push, 2)
struct IcoDirEntry { uint8_t w, h, colors, reserved; uint16_t planes, bpp; uint32_t bytes, offset; };
struct GrpIconEntry { uint8_t w, h, colors, reserved; uint16_t planes, bpp; uint32_t bytes; uint16_t id; };
#pragma pack(pop)

#ifdef _WIN32
// Stamp the game's own icon onto the shipped exe (UpdateResource replaces the player's
// default icon group + images).
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
	memcpy(&grp[0], ico.data(), 6);                     // reserved/type/count header matches
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

// ---- the COOKER: only USED content ships (user requirement) -----------------------------
// Dependency closure from the MANIFEST: startupWorld (+ anything in "packInclude") roots
// the walk; every string value in an asset's JSON that matches a known asset GUID or an
// existing content-relative path pulls that file in and recurses (nested JSON strings —
// effectsData, smJson — are parsed too; Lua sources contribute their quoted literals, so
// worlds/clips a script loads are cooked WITH their own dependencies). Unreferenced
// worlds, audio, meshes etc. never enter the pak.
struct CookCtx
{
	std::string projDir, contentDir;
	std::map<std::string, std::vector<std::string>> guidFiles;   // asset guid -> file(s)
	std::set<std::string> visited;                               // lowercase disk keys (walked)
	std::set<std::string> shipped;                               // walked AND claimed -> packs
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
	// Only project files ship; engine-side paths (built-in shaders etc.) stay out.
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
	// Nested JSON payloads stored as strings (PostProcess effectsData, Animator smJson).
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

// Engine-native content the editor itself understands. JSON kinds get their serialized
// props walked; the rest are leaves (no outgoing references). Anything OUTSIDE this list
// belongs to a MODULE — it ships only if a loaded module claims it via cookContent().
static bool CookEngineType(const std::string& low, bool& isJson)
{
	auto ends = [&](const char* suf) { size_t n = strlen(suf); return low.size() > n && low.compare(low.size() - n, n, suf) == 0; };
	isJson = ends(".nuworld") || ends(".nuprefab") || ends(".numat") || ends(".nubonemap") || ends(".nuproj");
	if (isJson) return true;
	return ends(".numesh") || ends(".nutex") || ends(".nuanim")
	    || ends(".ogg") || ends(".wav") || ends(".mp3") || ends(".flac")
	    || ends(".hlsl") || ends(".ico");
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

	// A module's file type? The OWNING module claims it and reports what it uses (paths or
	// asset guids — both walk on). Only LOADED modules answer: no scripting module in the
	// project = its scripts (and everything only they referenced) never ship.
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
			for (const std::string& u : uses) CookHandleString(c, u);
			break;
		}
	}
	if (!claimed)
		std::cout << "[Package]\tunclaimed file type, not shipped: " << rel << std::endl;
}

// The closure. Returns lowercase disk keys of every file that SHIPS.
static std::set<std::string> CookUsedFiles(const std::string& projDir, const std::string& contentDir,
                                           const std::string& manifestFile,
                                           const std::map<std::string, std::vector<std::string>>& guidFiles)
{
	CookCtx c;
	c.projDir = projDir; c.contentDir = contentDir; c.guidFiles = guidFiles;
	CookEnqueue(c, bfs::path(manifestFile));   // roots: startupWorld, packInclude, gameIcon...
	// The manifest may live OUTSIDE naming conventions — force-process it even if
	// CookEnqueue skipped it (e.g. opened from an odd location).
	if (c.queue.empty()) { c.visited.insert(DiskKey(manifestFile)); c.queue.push_back(manifestFile); }
	while (!c.queue.empty())
	{
		std::string f = c.queue.back();
		c.queue.pop_back();
		CookProcess(c, f);
	}
	return c.shipped;
}

// Editor-only module? Game modules must not import the EDITOR's UI dll — a module whose
// import table names NukeImGui.dll can't load next to the Player and never ships. Import
// tables store dll names as plain bytes, so a contents scan is a reliable test.
static bool IsEditorOnlyModule(const bfs::path& dll)
{
	bfs::ifstream f(dll, std::ios::binary);
	if (!f) return false;
	std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	return bytes.find("NukeImGui.dll") != std::string::npos;
}

// Decode the BEST image of an .ico into RGBA8 for the settings preview. Picks the largest
// entry; PNG-compressed entries atom through stb_image, classic DIB entries are decoded
// manually (32-bpp BGRA bottom-up; fully-zero alpha falls back to the AND mask).
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

	// PNG-compressed entry (Vista+ 256px icons).
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
	if (hdr < 40 || bw <= 0 || bh2 <= 0 || bpp != 32) return false;   // exotic depths: no preview (stamping still works)
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
	if (!anyAlpha)   // old icons: opacity lives in the 1-bpp AND mask after the XOR image
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

// ---- editor-driven builds (build unification) --------------------------------------------
// One command, the whole tree: the root superbuild (CMakeLists.txt at the repo root)
// drives NukeEngine.sln + every present module in dependency order, msbuild /m in
// parallel. Runs on a Jobs WORKER; output streams into the Console; the status bar shows
// live progress. The config this editor is RUNNING can't be rebuilt (locked binaries).
void EditorUI::RunEngineBuild(const std::string& config, std::function<void(bool)> onDone)
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
	// Repo root from the running exe: <root>/NukeEngine/x64/<cfg>/NukeEngine-Editor.exe.
	boost::system::error_code ec;
	bfs::path root = bfs::absolute(bfs::current_path(ec)).parent_path().parent_path().parent_path();
	if (!bfs::exists(root / "CMakeLists.txt", ec))
	{
		std::cout << "[build]\t\tno root superbuild found at " << root.string()
		          << " — building nothing (see CMakeLists.txt at the repo root)" << std::endl;
		nuke::Jobs::RunOnMain([onDone]() { if (onDone) onDone(false); });
		return;
	}
	StatusBar::Set("build", "Build " + config + ": starting...", StatusBar::kIndeterminate);
	nuke::Jobs::Schedule([this, root, config, onDone]()
	{
		auto runPiped = [&](const std::string& cmdLine, int& outProjects) -> bool
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
				std::cout << "[build]\t\tcan't start: " << cmdLine << std::endl;
				return false;
			}
			CloseHandle(wr);
			// Stream line by line: every line lands in the Console; project completions
			// ("X.vcxproj ->" / "-> dll") tick the status bar.
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
		};

		int projects = 0;
		bool ok = true;
		boost::system::error_code ec2;
		if (!bfs::exists(root / "build" / "CMakeCache.txt", ec2))
		{
			std::cout << "[build]\t\tconfiguring the superbuild (first run)..." << std::endl;
			StatusBar::Set("build", "Build: configuring...", StatusBar::kIndeterminate);
			ok = runPiped("cmake -S \"" + root.string() + "\" -B \"" + (root / "build").string()
			              + "\" -G \"Visual Studio 17 2022\" -A x64", projects);
		}
		if (ok)
			ok = runPiped("cmake --build \"" + (root / "build").string() + "\" --config " + config
			              + " -- /m /v:m /nologo", projects);
		const bool result = ok;
		nuke::Jobs::RunOnMain([this, result, config, onDone]()
		{
			StatusBar::Remove("build");
			std::cout << "[build]\t\t" << config << (result ? " build OK" : " build FAILED — see the lines above") << std::endl;
			if (onDone) onDone(result);
		});
	});
}

void EditorUI::PackageProject()
{
	// Stale binaries must never ship: rebuild Release FIRST (the superbuild is incremental
	// — a fresh tree is a no-op pass), then package. This closes the classic trap where a
	// repackaged dist silently carried yesterday's modules.
	RunEngineBuild("Release", [this](bool ok)
	{
		if (!ok)
		{
			std::cout << "[Package]\taborted: the Release build failed — fix it and package again" << std::endl;
			return;
		}
		PackageProjectNow();
	});
}

void EditorUI::PackageProjectNow()
{
	// Archive-derived sessions (opened from a .nupak game or a .numod) are NOT the authoring
	// project — packaging one would produce a full second game from someone's shipped content.
	// The menu item is hidden there; this guard also covers the NUKE_PACKAGE dev hook.
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
	const std::string gameName = projectName.empty() ? std::string("NukeGame") : projectName;
	const std::string icon = gameIcon.empty() ? std::string()
	                        : AppInstance::GetSingleton()->ResolveContent(gameIcon);
	// Cooker inputs (ResDB is game-thread state — snapshot guid -> file(s) here). Shaders
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
	// Build output: "" = the default <project>/dist (in the project root, beside content
	// and the manifest); a relative setting resolves against the project, absolute as-is.
	std::string distStr = distPath.empty() ? (bfs::path(projDir) / "dist").string()
	                    : (bfs::path(distPath).is_absolute() ? distPath
	                                                         : (bfs::path(projDir) / distPath).string());
	const int method = pakMethod, level = pakLevel;
	// Used modules only: every chosen service provider + the project's plugin list; no
	// list persisted -> all runtime modules (mirrors the Player's load rule). NukeImGui
	// is the EDITOR's UI dll, never a game module — it lives outside modules/ anyway.
	std::set<std::string> modules;
	for (auto& kv : serviceChoices) if (!kv.second.empty()) modules.insert(kv.second);
	if (pluginListLoaded) for (auto& p : enabledPlugins) modules.insert(p);
	else
	{
		boost::system::error_code ec;
		for (bfs::directory_iterator it(bfs::path("modules"), ec), end; it != end; it.increment(ec))
			if (!ec && it->path().extension() == ".dll") modules.insert(it->path().filename().string());
	}
	// Module SHIP EXTRAS (game-thread snapshot — modules are live state): everything a
	// module needs shipped beyond its DLL — compiled script assemblies into the pak, its
	// runtime companions (managed bridge, a private .NET) into the dist tree.
	std::vector<std::string> extraPak;
	std::vector<std::pair<std::string, std::string>> extraDist;
	for (auto& m : nuke::GetModules())
		if (m && m->loaded) m->shipExtras(projDir.c_str(), extraPak, extraDist);

	StatusBar::Set("package", "Packaging project...", StatusBar::kIndeterminate);
	nuke::Jobs::Schedule([projDir, projFile, content, gameName, icon, method, level, modules, distStr, guidFiles,
	                      extraPak, extraDist]()
	{
		boost::system::error_code ec;
		const bfs::path dist = distStr;
		// dist/ is a BUILD ARTIFACT — always rebuilt from scratch. Leftovers from an older
		// pack (renamed exe, other-config DLLs) would ship a broken mix otherwise. The
		// user's mods/ and mods.json survive by being re-created empty only when absent...
		// they don't: preserve them explicitly across the wipe.
		std::string modsJson;
		{
			bfs::ifstream mj(dist / "config" / "mods.json");
			if (mj) modsJson.assign(std::istreambuf_iterator<char>(mj), std::istreambuf_iterator<char>());
		}
		bfs::path modsKeep = bfs::path(projDir) / ".dist_mods_keep";
		bfs::remove_all(modsKeep, ec);
		if (bfs::exists(dist / "mods", ec)) bfs::rename(dist / "mods", modsKeep, ec);
		bfs::remove_all(dist, ec);
		bfs::create_directories(dist, ec);
		if (bfs::exists(modsKeep, ec)) bfs::rename(modsKeep, dist / "mods", ec);
		if (!modsJson.empty())
		{
			bfs::create_directories(dist / "config", ec);
			bfs::ofstream mj(dist / "config" / "mods.json");
			if (mj) mj << modsJson;
		}

		// 1) The project -> dist/content/game.nupak (immutable release pak, max compression).
		// COOKED: only the dependency closure of the manifest ships (startupWorld + every
		// asset it reaches + script-referenced content; "packInclude" in the .nuproj force-
		// adds extras the cooker can't see, e.g. dynamically composed paths). The active
		// manifest packs under the CANONICAL name "game.nuproj".
		auto all = CollectProject(projDir, false, DistPrefix(projDir, dist.string()));
		std::set<std::string> used = CookUsedFiles(projDir, content, projFile, guidFiles);
		std::vector<std::pair<std::string, std::string>> files;
		files.reserve(all.size());
		int forcedShaders = 0;
		for (auto& fp : all)
		{
			if (used.count(DiskKey(bfs::path(fp.second)))) { files.push_back(fp); continue; }
			// Project SHADERS always ship, referenced or not: scripts bind them BY NAME at
			// runtime (Shader.Find("unlit") / nuke.Shader.Find) — invisible to the reference
			// walk — and they are code-sized, not content-sized. Without this a shader no
			// world material happens to use silently vanishes from the build.
			std::string low = fp.first;
			for (char& c : low) c = (char)tolower((unsigned char)c);
			if (low.size() > 5 && low.compare(low.size() - 5, 5, ".hlsl") == 0)
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
		// Module pak extras: project files the cooker can't reach by reference (compiled
		// script assemblies — packed sessions read them straight from the pak).
		for (const std::string& rel : extraPak)
		{
			bfs::path src = bfs::path(projDir) / rel;
			if (bfs::exists(src, ec) && !bfs::is_directory(src, ec))
			{
				files.push_back({ rel, src.string() });
				std::cout << "[Package]\tmodule pak extra: " << rel << std::endl;
			}
			else
				std::cout << "[Package]\tmodule pak extra MISSING, skipped: " << rel << std::endl;
		}
		// The runtime source dir (Release preferred) is resolved BEFORE the pak builds:
		// engine built-ins (shaders/, fonts/) ride INSIDE game.nupak — the dist root stays
		// clean and mods can override any of them through the Package layers.
		bfs::path rt = ".";
		{
			bfs::path rel = bfs::path("..") / "Release";
			if (bfs::exists(rel / "NukePlayer.exe", ec) && bfs::exists(rel / "NukeEngine.dll", ec))
				rt = rel;
			else
				std::cout << "[Package]\tRelease build not found (x64/Release) — bundling the CURRENT config's binaries" << std::endl;
		}
		for (const char* dirName : { "shaders", "fonts" })
			for (bfs::recursive_directory_iterator it(rt / dirName, ec), end; it != end && !ec; it.increment(ec))
			{
				if (bfs::is_directory(it->path())) continue;
				boost::system::error_code rec;
				std::string rel = bfs::relative(it->path(), rt, rec).generic_string();
				if (!rec) files.push_back({ rel, it->path().string() });
			}
		bool ok = !files.empty()
		       && Package::Create(files, (dist / "content" / "game.nupak").string(), method, level,
		              [](int done, int total)
		              {
		                  StatusBar::Set("package", "Packaging project... " + std::to_string(done) + "/" + std::to_string(total),
		                                 total ? 0.7f * done / total : 0.0f);
		              });

		// 2) The runtime around it: the RELEASE Player under the game's own name + icon,
		// its runtime DLLs, the config (window title = the game), only the USED modules,
		// and the mods/ socket. Shaders/fonts are IN the pak (above). Release binaries ship
		// whenever the sibling Release build exists; otherwise the CURRENT config ships
		// with a warning (dev iteration) — the two sets never mix (CRT mismatch).
		if (ok)
		{
			StatusBar::Set("package", "Packaging: runtime files...", 0.75f);
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
			CopyOne(rt / "config" / "main.json", dist / "config" / "main.json");
			try
			{
				nlohmann::json cj;
				bfs::ifstream in(dist / "config" / "main.json");
				if (in) { std::stringstream ss; ss << in.rdbuf(); cj = nlohmann::json::parse(ss.str(), nullptr, false, true); }
				if (!cj.is_object()) cj = nlohmann::json::object();
				// The GAME's window settings live in the PROJECT (<project>/window.json — written by
				// Game.Set* from PIE scripts; the editor's own config is never touched by them). Merge
				// that block over the shipped defaults, then stamp the game's title.
				{
					bfs::ifstream pw(bfs::path(projDir) / "window.json");
					if (pw)
					{
						std::stringstream ps; ps << pw.rdbuf();
						nlohmann::json pj = nlohmann::json::parse(ps.str(), nullptr, false, true);
						if (pj.is_object() && pj.contains("window") && pj["window"].is_object())
							for (auto& kv : pj["window"].items()) cj["window"][kv.key()] = kv.value();
					}
				}
				// (No title stamp: the Player titles its window from game.nuproj "name" — packed
				// in the pak — so the game's name binds at packaging without a config field.)
				if (cj.contains("window") && cj["window"].is_object())
					cj["window"].erase("title");   // drop the legacy key if the shipped defaults had it
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
			for (const std::string& m : modules)
			{
				bfs::path src = rt / "modules" / m;
				// Project-local C++ GAME modules (<project>/modules, Phase 6.0) ship too —
				// they aren't in the editor's runtime dir. Editor modules win on name clash.
				boost::system::error_code mec;
				if (!bfs::exists(src, mec)) src = bfs::path(projDir) / "modules" / m;
				if (IsEditorOnlyModule(src))
				{
					std::cout << "[Package]\tmodule '" << m << "' is editor-only (imports NukeImGui.dll) — not shipped" << std::endl;
					continue;
				}
				if (!CopyOne(src, dist / "modules" / m))
					std::cout << "[Package]\tmodule missing, skipped: " << m << std::endl;
			}
			// Module dist extras (shipExtras): runtime companions — the managed bridge dir,
			// a private .NET runtime, whatever a module says its DLL cannot run without.
			// Relative sources resolve against the shipped runtime dir, absolute as-is;
			// directories copy recursively.
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

		nuke::Jobs::RunOnMain([ok, dist]()
		{
			StatusBar::Remove("package");
			if (ok)
			{
				uint64_t bytes = 0; int count = 0;
				boost::system::error_code ec2;
				for (bfs::recursive_directory_iterator it(dist, ec2), end; it != end; it.increment(ec2))
					if (!ec2 && !bfs::is_directory(it->path())) { bytes += bfs::file_size(it->path(), ec2); ++count; }
				std::cout << "[Package]\tdist ready: " << dist.string() << " (" << count << " files, "
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
	// The mod's name = its file name: chosen in the Package Mod modal (each mod is its OWN
	// .numod — same name updates that mod, a new name creates a separate one). The fallbacks
	// (last packaged -> project name) serve the NUKE_PACKAGE_MOD dev hook's no-arg call.
	std::string name = !modNameIn.empty() ? modNameIn
	                 : !modName.empty()   ? modName
	                 : (projectName.empty() ? std::string("mod") : projectName);
	for (char& c : name) if (strchr("\\/:*?\"<>|", c)) c = '_';   // filename-safe
	const int method = modMethod, level = modLevel;
	std::string distStr = distPath.empty() ? (bfs::path(projectDir) / "dist").string()
	                    : (bfs::path(distPath).is_absolute() ? distPath
	                                                         : (bfs::path(projectDir) / distPath).string());
	// Dependencies (mods-on-mods): the mods MOUNTED under this session are what the new
	// mod was authored on top of — a compatibility patch depends on the mods it patches.
	// Recorded into the mod's "mod.json"; the loader mounts them below it (or skips the
	// mod when one is missing). Snapshot on the game thread.
	std::vector<std::string> requires_;
	for (const Package::ModInfo& mi : Package::Mods()) requires_.push_back(mi.name);

	StatusBar::Set("packmod", "Packaging mod...", StatusBar::kIndeterminate);
	nuke::Jobs::Schedule([projDir, base, name, method, level, distStr, requires_]()
	{
		boost::system::error_code ec;
		auto all = CollectProject(projDir, true, DistPrefix(projDir, distStr));

		std::string outPath;
		std::vector<std::pair<std::string, std::string>> files;
		const bool fromMod = base.size() > 6 && base.compare(base.size() - 6, 6, ".numod") == 0;
		const bool fromPak = base.size() > 6 && base.compare(base.size() - 6, 6, ".nupak") == 0;
		if (fromMod)
		{
			files = all;                    // editable mod: repack the whole work tree
			// Keep the mod's compiled scripts assembly across repacks (managed/ is excluded
			// from generic collection — this one artifact is the mod's shipped code).
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
			// Same name -> IN PLACE update of the opened mod; a new name -> save-as (a
			// separate .numod beside it; the session retargets to it below).
			outPath = (bfs::path(base).parent_path() / (name + ".numod")).string();
		}
		else if (fromPak)
		{
			// Modder flow: only what CHANGED against the SESSION STACK — the base game plus
			// every mod mounted under it (new files + CRC diffs vs the top mounted copy).
			// A changed WORLD also records its BASIS (the stack's copy the modder authored
			// on) under "basis/<rel>": at load the merge diffs the mod against ITS OWN basis,
			// so the mod applies exactly the author's point changes — per atom, per component,
			// per PROP — no matter how the base game evolves afterwards.
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
					// The basis = the world as the author SAW it: every mounted pak layer
					// below the raw overlay, merged the same way the session merged them.
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
			// The session's OWN scripts assembly (Scripts_<session>.dll — compiled against
			// the game's GameScripts.dll) ships WITH the mod: unique name, loaded ADDITIVELY
			// by the player, so mod classes work in the shipped game too. The generic
			// managed/ exclusion (PackWorthy) stays — only this one artifact rides along.
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
			// The mod's manifest rides INSIDE it: its name + its dependencies. A repacked
			// .numod keeps the requires recorded when it was authored (its session mounts
			// nothing) — the extracted work tree carries the original mod.json.
			nlohmann::json man;
			man["name"] = name;
			man["requires"] = requires_;
			if (fromMod)
			{
				bfs::ifstream mf(bfs::path(projDir) / "mod.json");
				if (mf)
				{
					nlohmann::json old = nlohmann::json::parse(mf, nullptr, false);
					if (!old.is_discarded() && old.contains("requires")) man["requires"] = old["requires"];
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
				});
			bfs::remove(manTmp, ec);   // the manifest is inside the pak now
			bfs::remove_all(bfs::path(projDir) / ".modbasis.tmp", ec);   // basis copies are inside too
		}

		// A renamed repack (.numod save-as) retargets the session: the marker + the live
		// basePakPath now point at the NEW file, so the next repack updates it in place.
		if (ok && fromMod && outPath != base)
		{
			bfs::ofstream mark(bfs::path(projDir) / ".nupak_base", std::ios::trunc);
			if (mark) mark << outPath;
			nuke::Jobs::RunOnMain([outPath]() { EditorUI::getSingleton()->basePakPath = outPath; });
		}

		// A mod packed into a game's mods/ dir self-registers in that game's config —
		// nobody should hand-edit paths (a bare name there was the classic mistake).
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

// ---- Mods panel data (Project Settings "Mods" section, mounted-pak session) -------------------
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
	auto manifest = [](ModRow& r) {
		Package::File pf;
		std::string man;
		if (!r.path.empty() && pf.Open(r.path) && pf.Read("mod.json", man))
		{
			nlohmann::json j = nlohmann::json::parse(man, nullptr, false);
			if (!j.is_discarded() && j.is_object())
			{
				r.name = j.value("name", r.name);
				if (j.contains("requires") && j["requires"].is_array())
					for (auto& q : j["requires"])
						if (q.is_string())
						{
							r.reqs.push_back(q.get<std::string>());
							r.req += (r.req.empty() ? "" : ", ") + r.reqs.back();
						}
			}
		}
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
			manifest(r);
			for (const std::string& mp : Package::MountedPaks())
			{
				boost::system::error_code ec2;
				if (r.found && bfs::equivalent(bfs::path(mp), bfs::path(r.path), ec2)) { r.mounted = true; break; }
			}
			seen.insert(lower(r.file));
			modsUi.push_back(std::move(r));
		}

	// Disabled mods: .numod files in mods/ that the config doesn't list.
	for (bfs::directory_iterator it(bfs::path(root) / "mods", ec), end; it != end && !ec; it.increment(ec))
	{
		if (bfs::is_directory(it->path()) || it->path().extension() != ".numod") continue;
		if (seen.count(lower(it->path().filename().string()))) continue;
		ModRow r;
		r.path = it->path().string();
		r.file = it->path().filename().string();
		r.name = it->path().stem().string();
		manifest(r);
		modsUi.push_back(std::move(r));
	}

	// The EDITOR's own selection (separate from the game's config): editor_mods.json in
	// the session overlay — these are the mods THIS session actually mounts.
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

// Persist the editor's selection + REMOUNT the session stack to match (base pak + the
// selection, dependency-ordered). Worlds pick the change up on their next open.
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
	int n = Package::MountModList(root, entries);   // empty list still clears the mod metadata
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

// File -> Package Mod...: ask for the mod's name first — each mod is its OWN .numod (the
// old behavior always wrote ONE fixed name, so every pack overwrote the same file).
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

// Open-with for a PACKED PROJECT (.nupak): no extraction — the pak stays the only copy of
// the game data. Mount it read-only, put the editing session into a "<stem>_mod" overlay
// beside it (raw layer wins over mounts, so saves/imports transparently override), and
// carry over ONLY the manifest (the editor needs one to drive itself; it is config, not
// game content). Package Mod later packs exactly what the modder created.
std::string EditorUI::PrepareMountedProject(const std::string& pakAbs)
{
	if (!Package::Mount(pakAbs, 0))
	{
		std::cout << "[Package]\tcan't open archive: " << pakAbs << std::endl;
		return std::string();
	}
	boost::system::error_code ec;
	bfs::path pak(pakAbs);
	bfs::path work = pak.parent_path() / (pak.stem().string() + "_mod");
	bfs::create_directories(work / "content", ec);
	// Opening the game pak opens THE GAME — base only. config/mods.json is the PLAYER's
	// list, not the editor's: the session mounts mods only when the EDITOR's own separate
	// list says so (editor_mods.json in the overlay, managed by the Mods panel).
	{
		bfs::path gameRoot = pak.parent_path();
		if (gameRoot.filename() == "content") gameRoot = gameRoot.parent_path();
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

// Open-with for a MOD (.numod — editable by design): materialize it into "<stem>_project"
// beside the pak (reused unless the pak is newer), synthesize a minimal manifest for
// manifest-less mods, and remember the base so Package Mod repacks it IN PLACE.
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
	// A mod living in a GAME's mods/ dir edits ON TOP of that game — mount the base pak
	// and ONLY this mod's own dependency chain (mod.json "requires", walked recursively)
	// under the work tree. The game's config/mods.json is the PLAYER's list — editing THIS
	// mod does not pull the whole enabled stack in. The game's manifest drives the session.
	{
		bfs::path gameRoot = pak.parent_path();
		if (gameRoot.filename() == "mods")
		{
			gameRoot = gameRoot.parent_path();
			bfs::path basePak = gameRoot / "content" / "game.nupak";
			if (bfs::exists(basePak, ec) && Package::Mount(basePak.string(), 0))
			{
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
