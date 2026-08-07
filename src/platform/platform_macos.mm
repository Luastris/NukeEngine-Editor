// macOS implementation of the editor's OS-integration seam (declared in editor/editorui.h):
// NSOpenPanel file/folder dialogs, external editor detection/launch, relaunch, process checks.
// Objective-C++ (no ARC): panels run modal on the main thread — exactly where the editor's
// ImGui code calls the pickers from.
#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>   // LaunchServices (association) + Apple Event constants
#include <libproc.h>            // proc_listallpids/proc_pidpath (EditorProcessRunning)
#include <mach-o/dyld.h>        // _NSGetExecutablePath (relaunch)
#include <spawn.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <cstring>
#include <interface/Importers.h>   // plugin importer extensions -> dialog filter
#include <editor/exteditor.h>
#include <boost/filesystem.hpp>

extern char** environ;

// allowedFileTypes is deprecated in favor of UTType, but it takes plain extensions and works
// on every macOS the editor targets — the UTType route can replace it wholesale later.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

// Run one modal NSOpenPanel. `exts` empty = any file. Returns "" if cancelled.
static std::string RunOpenPanel(const char* title, const std::vector<std::string>& exts,
                                bool files, bool dirs)
{
	@autoreleasepool
	{
		NSOpenPanel* panel = [NSOpenPanel openPanel];
		panel.title                   = [NSString stringWithUTF8String:title];
		panel.canChooseFiles          = files ? YES : NO;
		panel.canChooseDirectories    = dirs ? YES : NO;
		panel.allowsMultipleSelection = NO;
		panel.resolvesAliases         = YES;
		if (!exts.empty())
		{
			NSMutableArray* types = [NSMutableArray arrayWithCapacity:exts.size()];
			for (const std::string& e : exts)
				[types addObject:[NSString stringWithUTF8String:e.c_str()]];
			panel.allowedFileTypes = types;
		}
		[NSApp activateIgnoringOtherApps:YES];   // the panel must front a GLFW-owned app
		if ([panel runModal] != NSModalResponseOK) return std::string();
		NSURL* url = panel.URL;
		if (!url || !url.path) return std::string();
		return std::string(url.path.UTF8String);
	}
}

// Native "open file" dialog for asset import (models + images + plugin importer formats).
std::string EditorPickModelFile()
{
	std::vector<std::string> exts = {
		"obj", "fbx", "dae", "gltf", "glb", "3ds", "ply", "stl",          // models
		"png", "jpg", "jpeg", "tga", "bmp", "hdr", "psd", "gif",          // images
	};
	for (const nuke::AssetImporter& imp : nuke::AssetImporters())
		for (const std::string& e : imp.exts)
			exts.push_back(e.size() && e[0] == '.' ? e.substr(1) : e);    // panel wants bare extensions
	return RunOpenPanel("Import asset", exts, true, false);
}

// Native "open file" dialog for the game icon (Project Settings -> Packaging).
std::string EditorPickIconFile()
{
	return RunOpenPanel("Pick the game icon", { "ico", "png", "icns" }, true, false);
}

// Native "pick folder" dialog (build output path).
std::string EditorPickFolder()
{
	return RunOpenPanel("Pick the build output folder", {}, false, true);
}

// Native "open file" dialog for projects: raw .nuproj, packed .nupak, mod .numod.
std::string EditorPickProjectFile()
{
	return RunOpenPanel("Open project", { "nuproj", "nupak", "numod" }, true, false);
}

// Program picker (custom external editor). A picked .app bundle resolves to its executable.
std::string EditorPickExeFile()
{
	@autoreleasepool
	{
		NSOpenPanel* panel = [NSOpenPanel openPanel];
		panel.title                   = @"Pick the editor application";
		panel.canChooseFiles          = YES;
		panel.canChooseDirectories    = NO;
		panel.allowsMultipleSelection = NO;
		panel.treatsFilePackagesAsDirectories = NO;   // .app selects as one item
		[NSApp activateIgnoringOtherApps:YES];
		if ([panel runModal] != NSModalResponseOK) return std::string();
		NSURL* url = panel.URL;
		if (!url || !url.path) return std::string();
		std::string path(url.path.UTF8String);
		if (path.size() > 4 && path.compare(path.size() - 4, 4, ".app") == 0)
		{
			NSBundle* b = [NSBundle bundleWithPath:url.path];
			if (b && b.executablePath) return std::string(b.executablePath.UTF8String);
		}
		return path;
	}
}

// ---- processes ----------------------------------------------------------------------------

// Fire-and-forget: /bin/sh expands the argument template exactly like the Windows command
// line did; POSIX_SPAWN_SETSID detaches the child from the editor's session.
bool EditorLaunchDetached(const std::string& exe, const std::string& args)
{
	const std::string cmd = "\"" + exe + "\" " + args;
	const char* argv[] = { "/bin/sh", "-c", cmd.c_str(), nullptr };
	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
	pid_t pid = 0;
	const int rc = posix_spawn(&pid, "/bin/sh", nullptr, &attr, (char* const*)argv, environ);
	posix_spawnattr_destroy(&attr);
	return rc == 0;
}

// Is a process of this executable already running? Decides reuse vs first-launch args.
bool EditorProcessRunning(const std::string& exePath)
{
	std::string want = boost::filesystem::path(exePath).filename().string();
	for (char& c : want) c = (char)tolower((unsigned char)c);

	const int n = proc_listallpids(nullptr, 0);
	if (n <= 0) return false;
	std::vector<pid_t> pids((size_t)n + 64);
	const int got = proc_listallpids(pids.data(), (int)(pids.size() * sizeof(pid_t)));
	char path[PROC_PIDPATHINFO_MAXSIZE];
	for (int i = 0; i < got; ++i)
	{
		if (pids[i] <= 0 || pids[i] == getpid()) continue;
		if (proc_pidpath(pids[i], path, sizeof(path)) <= 0) continue;
		std::string name = boost::filesystem::path(path).filename().string();
		for (char& c : name) c = (char)tolower((unsigned char)c);
		if (name == want) return true;
	}
	return false;
}

// Launch a new editor instance on `projectPath` (project lifecycle is bound to startup, so
// switching projects means relaunching); the caller closes this instance.
bool EditorRelaunch(const std::string& projectPath)
{
	char exe[4096];
	uint32_t sz = sizeof(exe);
	if (_NSGetExecutablePath(exe, &sz) != 0) return false;
	const char* argv[] = { exe, projectPath.c_str(), nullptr };
	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
	pid_t pid = 0;
	const int rc = posix_spawn(&pid, exe, nullptr, &attr, (char* const*)argv, environ);
	posix_spawnattr_destroy(&attr);
	return rc == 0;
}

// ---- documents & association --------------------------------------------------------------

// LaunchServices hands a double-clicked document over as a kAEOpenDocuments Apple Event
// (never argv). The handler queues the path; the editor's toolbar polls it every frame and
// routes it through the regular project-switch flow.
static std::string  g_openDocPath;
static NSObject*    g_openDocTarget = nil;

@interface NukeOpenDocTarget : NSObject
- (void)handleOpenDoc:(NSAppleEventDescriptor*)event withReply:(NSAppleEventDescriptor*)reply;
@end
@implementation NukeOpenDocTarget
- (void)handleOpenDoc:(NSAppleEventDescriptor*)event withReply:(NSAppleEventDescriptor*)reply
{
	NSAppleEventDescriptor* docs = [event paramDescriptorForKeyword:keyDirectObject];
	if (!docs || docs.numberOfItems < 1) return;
	NSAppleEventDescriptor* item = [docs descriptorAtIndex:1];   // AE lists are 1-based
	NSURL* url = item.fileURLValue;
	if (url && url.path)
	{
		g_openDocPath = url.path.UTF8String;
		printf("[odoc]\t\t\t%s\n", g_openDocPath.c_str()); fflush(stdout);
	}
}
@end

// Modern macOS delivers document opens through the application DELEGATE
// (application:openURLs:), not the kAEOpenDocuments Apple Event; GLFW's delegate has no
// such method, so AppKit falls to NSDocumentController — "cannot open files in the
// NukeEngine Project format" alert. Wrap GLFW's delegate in a forwarding proxy that adds it.
@interface NukeAppDelegateProxy : NSObject<NSApplicationDelegate>
@property (nonatomic, strong) id inner;   // GLFW's delegate — everything else forwards to it
@end
@implementation NukeAppDelegateProxy
- (BOOL)respondsToSelector:(SEL)sel
{
	return [super respondsToSelector:sel] || [self.inner respondsToSelector:sel];
}
- (id)forwardingTargetForSelector:(SEL)sel { return self.inner; }
- (void)application:(NSApplication*)app openURLs:(NSArray<NSURL*>*)urls
{
	if (urls.count && urls.firstObject.path)
	{
		g_openDocPath = urls.firstObject.path.UTF8String;
		printf("[odoc]\t\t\t%s\n", g_openDocPath.c_str()); fflush(stdout);
	}
}
- (BOOL)application:(NSApplication*)sender openFile:(NSString*)filename
{
	if (filename) { g_openDocPath = filename.UTF8String; printf("[odoc]\t\t\t%s\n", g_openDocPath.c_str()); fflush(stdout); }
	return YES;
}
@end
static NukeAppDelegateProxy* g_delegateProxy = nil;

static void WrapAppDelegate()
{
	if (!NSApp || !NSApp.delegate || NSApp.delegate == g_delegateProxy) return;
	if (!g_delegateProxy) g_delegateProxy = [[NukeAppDelegateProxy alloc] init];
	g_delegateProxy.inner = NSApp.delegate;
	NSApp.delegate = g_delegateProxy;
}

void EditorInstallOpenDocHandler()
{
	@autoreleasepool
	{
		if (!g_openDocTarget) g_openDocTarget = [[NukeOpenDocTarget alloc] init];
		void (^install)(void) = ^{
			[[NSAppleEventManager sharedAppleEventManager]
				setEventHandler:g_openDocTarget
				    andSelector:@selector(handleOpenDoc:withReply:)
				  forEventClass:kCoreEventClass
				     andEventID:kAEOpenDocuments];
		};
		install();
		WrapAppDelegate();   // in case the app already finished launching
		// AppKit resolves the launch document INSIDE finishLaunching (GLFW init) and checks
		// the delegate right there — the wrap must already be in place at its START (Will),
		// and the AE registration re-done after AppKit re-registers its own (Did).
		for (NSNotificationName when : @[ NSApplicationWillFinishLaunchingNotification,
		                                  NSApplicationDidFinishLaunchingNotification ])
			[[NSNotificationCenter defaultCenter]
				addObserverForName:when object:nil queue:nil
				usingBlock:^(NSNotification*) { install(); WrapAppDelegate(); }];
	}
}

std::string EditorTakeOpenDocRequest()
{
	std::string p;
	p.swap(g_openDocPath);
	return p;
}

// The bundle to register: the one this exe runs from, else NukeEngine-Editor.app in the run
// dir (a bare-binary dev launch still registers the deployed bundle).
static NSURL* EditorBundleURL()
{
	NSBundle* main = [NSBundle mainBundle];
	if (main && main.bundleURL && [main.bundleURL.path hasSuffix:@".app"])
		return main.bundleURL;
	char exe[4096];
	uint32_t sz = sizeof(exe);
	if (_NSGetExecutablePath(exe, &sz) != 0) return nil;
	boost::filesystem::path p = boost::filesystem::path(exe).parent_path();
	// .../Foo.app/Contents/MacOS/exe → Foo.app
	if (p.filename() == "MacOS" && p.parent_path().filename() == "Contents")
		return [NSURL fileURLWithPath:[NSString stringWithUTF8String:p.parent_path().parent_path().c_str()]];
	boost::filesystem::path app = p / "NukeEngine-Editor.app";
	boost::system::error_code ec;
	if (boost::filesystem::exists(app, ec))
		return [NSURL fileURLWithPath:[NSString stringWithUTF8String:app.c_str()]];
	return nil;
}

// Register the editor bundle with LaunchServices and claim the NukeEngine document types
// (the bundle's Info.plist declares the UTIs). User scope, reversible in Finder.
bool RegisterProjectFileAssociation()
{
	@autoreleasepool
	{
		NSURL* app = EditorBundleURL();
		if (!app)
		{
			NSLog(@"[assoc] no NukeEngine-Editor.app found — build the bundle first");
			return false;
		}
		LSRegisterURL((__bridge CFURLRef)app, true);   // (re)index the bundle + its UTIs
		NSString* bundleId = [NSBundle bundleWithURL:app].bundleIdentifier;
		if (!bundleId) return false;
		bool ok = true;
		for (NSString* uti in @[ @"com.luastris.nukeengine.project",
		                         @"com.luastris.nukeengine.package",
		                         @"com.luastris.nukeengine.mod" ])
			ok &= LSSetDefaultRoleHandlerForContentType((__bridge CFStringRef)uti, kLSRolesAll,
			                                            (__bridge CFStringRef)bundleId) == noErr;
		return ok;
	}
}

// ---- external editors ---------------------------------------------------------------------

// Scan the standard install spots (VS Code, Rider, Sublime, TextMate) for external editors,
// each with its file:line argument template. De-duplicated by name.
std::vector<ExtEditor> EditorDetectExternalEditors()
{
	namespace fs = boost::filesystem;
	std::vector<ExtEditor> out;
	boost::system::error_code ec;
	auto add = [&](const char* name, const fs::path& exe, const char* args, const char* argsProj) {
		if (fs::exists(exe, ec)) out.push_back({ name, exe.string(), args, argsProj });
	};
	const char* home = getenv("HOME");
	const std::vector<fs::path> roots = { "/Applications",
	                                      home ? fs::path(home) / "Applications" : fs::path() };
	for (const fs::path& r : roots)
	{
		if (r.empty()) continue;
		// VS Code: the bundled `code` CLI (-r = reuse the current window).
		add("VS Code", r / "Visual Studio Code.app" / "Contents" / "Resources" / "app" / "bin" / "code",
		    "-r -g \"{file}:{line}\"", "\"{projectDir}\" -g \"{file}:{line}\"");
		// Rider (direct install; Toolbox spots vary per version and are picked via Custom).
		add("Rider", r / "Rider.app" / "Contents" / "MacOS" / "rider",
		    "--line {line} \"{file}\"", "\"{project}\" --line {line} \"{file}\"");
		// Sublime Text: subl CLI takes file:line directly.
		add("Sublime Text", r / "Sublime Text.app" / "Contents" / "SharedSupport" / "bin" / "subl",
		    "\"{file}:{line}\"", "");
		// TextMate: mate CLI.
		add("TextMate", r / "TextMate.app" / "Contents" / "Resources" / "mate",
		    "-l {line} \"{file}\"", "");
	}

	// Same editor found twice (per-user + system): keep the first spot only.
	std::vector<ExtEditor> dedup;
	for (ExtEditor& e : out)
	{
		bool seen = false;
		for (ExtEditor& d : dedup) seen |= d.name == e.name;
		if (!seen) dedup.push_back(std::move(e));
	}
	return dedup;
}

#endif // __APPLE__
