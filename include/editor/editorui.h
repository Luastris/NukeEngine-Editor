#ifndef EDITORUI_H
#define EDITORUI_H
// Editor UI panels, ported to Dear ImGui 1.92 and the NukeUI module.
//
// All the old plumbing is GONE — it now lives elsewhere:
//   * ImGui context / font / frame (NewFrame/Render) -> NukeUI module
//   * GPU rendering of draw data                      -> renderer via iRender seam
//   * input                                            -> (wired later via iRender callbacks)
//   * OpenGL2 immediate-mode renderer + freeglut       -> deleted
//   * ImGuizmo gizmo                                   -> deferred (lib not in build yet)
// What remains here is just the panels: menu, hierarchy, inspector, about, etc.

#include "imgui.h"
#include "imgui_internal.h"   // BeginViewportSideBar (toolbar attached under the main menu bar)
#include "nukeui.h"           // NukeUI::MergeIconFont
#include "IconsLucide.h"      // ICON_LC_* toolbar icons
#include "config.h"
#include "interface/AppInstance.h"
#include "interface/Modular.h"
#include "API/Model/MeshRenderer.h"
#include <boost/container/list.hpp>
#include <boost/bind/bind.hpp>
#include <cstring>
#include <cmath>

class EditorUI
{
private:
	EditorUI() {}
	~EditorUI() {}
	struct NukeWindow* win = nullptr;
	boost::shared_ptr<NUKEModule> selectedPlugin = nullptr;
	int  selectedPluginIndex = -1;
	bool freezeWindows = true;
	ImGuiWindowFlags window_flags = 0;
	Camera* editorCam = nullptr;
	uint64_t sceneRTId = 0;   // render target the editor camera draws into
	float camYaw = 0.0f, camPitch = 0.0f;   // editor camera look angles (radians)

public:
	static EditorUI* getSingleton()
	{
		static EditorUI instance;
		return &instance;
	}

	void SetUp()
	{
		cout << "[editorui]\t\t" << "EditorUI setup (imgui 1.92 / NukeUI)..." << endl;
		ApplyStyle();

		win = &Config::getSingleton()->window;

		// The UI module owns the font atlas, but the APP chooses its font.
		ImGuiIO& io = ImGui::GetIO();
		if (!win->mainFont.empty())
		{
			cout << "[editorui]\t\t" << "Loading font: " << win->mainFont << endl;
			io.Fonts->AddFontFromFileTTF(win->mainFont.c_str(), 19.0f);
		}
		else
		{
			io.Fonts->AddFontDefault();
		}
		// Merge Lucide icons on top so toolbar/panels can use ICON_LC_* glyphs.
		// Lucide glyphs sit high in the line, so nudge them down to centre.
		NukeUI::MergeIconFont("fonts/lucide.ttf", 20.0f, 4.0f);

		InitMenu();

		AppInstance* editor = AppInstance::GetSingleton();
		editor->PushWindow("nukeeditor-about", boost::bind(&EditorUI::winAbout, this));
		editor->PushWindow("nukeeditor-browser", boost::bind(&EditorUI::winBrowser, this));
		editor->PushWindow("nukeeditor-console", boost::bind(&EditorUI::winConsole, this));
		editor->PushWindow("nukeeditor-hierarchy", boost::bind(&EditorUI::winHierarchy, this));
		editor->PushWindow("nukeeditor-inspector", boost::bind(&EditorUI::winInspector, this));
		editor->PushWindow("nukeeditor-render", boost::bind(&EditorUI::winRender, this));
		editor->PushWindow("nukeeditor-plugins", boost::bind(&EditorUI::PluginMGRWindow, this));

		GameObject* camObj = editor->currentScene->Get("Editor Camera");
		if (camObj)
			editorCam = camObj->GetComponent<Camera>();
		if (editorCam)
		{
			// Park the editor camera so it looks at the origin (camera control TODO).
			editorCam->transform->position.x = 0; editorCam->transform->position.y = 0; editorCam->transform->position.z = -5;
			// rotation defaults to identity quaternion (looks +Z, at the origin).
		}
		// Demo geometry via the spawn API so the viewport shows something.
		{
			GameObject* cube = new GameObject("Cube");
			MeshRenderer* mr = new MeshRenderer();
			cube->AddComponent(mr);
			mr->mesh = Mesh::CreateCube();
			editor->currentScene->Add(cube);
		}
		cout << "[editorui]\t\t" << "EditorUI ready." << endl;
	}

	// NukeEngine dark theme (ported from the old gui.cpp to imgui 1.92 enums).
	void ApplyStyle()
	{
		ImGuiStyle* s = &ImGui::GetStyle();
		s->WindowPadding     = ImVec2(15, 15);
		s->WindowRounding    = 0.0f;
		s->FramePadding      = ImVec2(5, 5);
		s->FrameRounding     = 0.0f;
		s->ItemSpacing       = ImVec2(6, 6);
		s->ItemInnerSpacing  = ImVec2(6, 6);
		s->IndentSpacing     = 25.0f;
		s->ScrollbarSize     = 15.0f;
		s->ScrollbarRounding = 0.0f;
		s->GrabMinSize       = 5.0f;
		s->GrabRounding      = 3.0f;
		ImVec4* c = s->Colors;
		c[ImGuiCol_Text]                 = ImVec4(0.80f, 0.80f, 0.83f, 1.00f);
		c[ImGuiCol_TextDisabled]         = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
		c[ImGuiCol_WindowBg]             = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
		c[ImGuiCol_ChildBg]              = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
		c[ImGuiCol_PopupBg]              = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
		c[ImGuiCol_Border]               = ImVec4(0.80f, 0.80f, 0.80f, 0.48f);
		c[ImGuiCol_FrameBg]              = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		c[ImGuiCol_FrameBgHovered]       = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
		c[ImGuiCol_FrameBgActive]        = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		c[ImGuiCol_TitleBg]              = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		c[ImGuiCol_TitleBgActive]        = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
		c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.20f, 0.58f, 0.55f, 0.25f);
		c[ImGuiCol_MenuBarBg]            = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		c[ImGuiCol_ScrollbarBg]          = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.80f, 0.80f, 0.83f, 0.31f);
		c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
		c[ImGuiCol_CheckMark]            = ImVec4(0.80f, 0.80f, 0.83f, 0.31f);
		c[ImGuiCol_SliderGrab]           = ImVec4(0.80f, 0.80f, 0.83f, 0.31f);
		c[ImGuiCol_SliderGrabActive]     = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
		c[ImGuiCol_Button]               = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		c[ImGuiCol_ButtonHovered]        = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
		c[ImGuiCol_ButtonActive]         = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		c[ImGuiCol_Header]               = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		c[ImGuiCol_HeaderHovered]        = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		c[ImGuiCol_HeaderActive]         = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
		c[ImGuiCol_Separator]            = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		c[ImGuiCol_SeparatorHovered]     = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
		c[ImGuiCol_SeparatorActive]      = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		c[ImGuiCol_ResizeGrip]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
		c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		c[ImGuiCol_ResizeGripActive]     = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
		c[ImGuiCol_PlotLines]            = ImVec4(0.40f, 0.39f, 0.38f, 0.63f);
		c[ImGuiCol_PlotLinesHovered]     = ImVec4(0.25f, 1.00f, 0.00f, 1.00f);
		c[ImGuiCol_PlotHistogram]        = ImVec4(0.40f, 0.39f, 0.38f, 0.63f);
		c[ImGuiCol_PlotHistogramHovered] = ImVec4(0.25f, 1.00f, 0.00f, 1.00f);
		c[ImGuiCol_TextSelectedBg]       = ImVec4(0.25f, 1.00f, 0.00f, 0.43f);
		c[ImGuiCol_ModalWindowDimBg]     = ImVec4(1.00f, 0.98f, 0.95f, 0.73f);
		// Docking-branch tab / dock colors (kept dark so the light label text reads).
		c[ImGuiCol_Tab]                       = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		c[ImGuiCol_TabHovered]                = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
		c[ImGuiCol_TabSelected]               = ImVec4(0.18f, 0.17f, 0.22f, 1.00f);
		c[ImGuiCol_TabSelectedOverline]       = ImVec4(0.25f, 1.00f, 0.00f, 0.50f);
		c[ImGuiCol_TabDimmed]                 = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
		c[ImGuiCol_TabDimmedSelected]         = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
		c[ImGuiCol_DockingPreview]            = ImVec4(0.25f, 1.00f, 0.00f, 0.30f);
		c[ImGuiCol_DockingEmptyBg]            = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
	}

	// ---- menu ----
	void InitMenu()
	{
		MenuStrip* mstrip = AppInstance::GetSingleton()->menuStrip = new MenuStrip();
		mstrip->AddItem("Tools/", "Plugin manager", TogglePluginMGR);
	}

	static void TogglePluginMGR()
	{
		Config::getSingleton()->window.plugmgr = !Config::getSingleton()->window.plugmgr;
	}

	bool EditorSubMenu(MenuItem* item)
	{
		if (item->subitems.size() > 0)
		{
			if (ImGui::BeginMenu(item->name.c_str()))
			{
				for (auto subitem : item->subitems)
					EditorSubMenu(subitem);
				ImGui::EndMenu();
			}
		}
		else if (item->callback)
		{
			if (ImGui::MenuItem(item->name.c_str()))
				item->callback();
		}
		return true;
	}

	void EditorMenu()
	{
		if (ImGui::BeginMainMenuBar())
		{
			if (ImGui::BeginMenu("File"))
			{
				if (ImGui::MenuItem("New")) {}
				if (ImGui::MenuItem("Open", "Ctrl+O")) {}
				if (ImGui::MenuItem("Save", "Ctrl+S")) {}
				ImGui::Separator();
				if (ImGui::MenuItem("Quit", "Alt+F4")) {}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Edit"))
			{
				if (ImGui::MenuItem("Undo", "CTRL+Z")) {}
				if (ImGui::MenuItem("Redo", "CTRL+Y", false, false)) {}
				ImGui::Separator();
				if (ImGui::MenuItem("Cut", "CTRL+X")) {}
				if (ImGui::MenuItem("Copy", "CTRL+C")) {}
				if (ImGui::MenuItem("Paste", "CTRL+V")) {}
				ImGui::EndMenu();
			}
			for (auto rootElement : AppInstance::GetSingleton()->menuStrip->strip)
				EditorSubMenu(rootElement);
			if (ImGui::BeginMenu("Window"))
			{
				ImGui::MenuItem("Freeze windows", "F8", &freezeWindows);
				ImGui::Separator();
				ImGui::MenuItem("Hierarchy", nullptr, &win->hierarchy);
				ImGui::MenuItem("Console", nullptr, &win->console);
				ImGui::MenuItem("Browser", nullptr, &win->browser);
				ImGui::MenuItem("Inspector", nullptr, &win->inspector);
				ImGui::MenuItem("Render", nullptr, &win->render);
				ImGui::MenuItem("Plugins", nullptr, &win->plugmgr);
				ImGui::MenuItem("About", nullptr, &win->about);
				ImGui::EndMenu();
			}
			ImGui::EndMainMenuBar();
		}
	}

	// ---- panels ----
	void DisplayRecursiveGameObjectHierarchy(bc::list<GameObject*>& gos)
	{
		int i = 0;
		for (auto go : gos)
		{
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
			if (AppInstance::GetSingleton()->selectedInHieararchy == go)
				flags |= ImGuiTreeNodeFlags_Selected;
			if (go->children.size() == 0)
				flags |= ImGuiTreeNodeFlags_Leaf;

			bool opened = ImGui::TreeNodeEx((void*)(intptr_t)i, flags, "%s", go->GetName().c_str());
			if (ImGui::IsItemClicked())
				AppInstance::GetSingleton()->selectedInHieararchy = go;
			if (opened)
			{
				if (go->children.size() > 0)
					DisplayRecursiveGameObjectHierarchy(go->children);
				ImGui::TreePop();
			}
			++i;
		}
	}

	void winHierarchy()
	{
		if (!win->hierarchy) return;
		ImGui::Begin("Hierarchy", &win->hierarchy, window_flags);
		DisplayRecursiveGameObjectHierarchy(AppInstance::GetSingleton()->currentScene->GetHierarchy());
		ImGui::End();
	}

	void CamComponent(Camera* cam)
	{
		if (cam->renderer)
		{
			ImGui::InputInt("Width", &cam->renderer->width);
			ImGui::InputInt("Height", &cam->renderer->height);
		}
		float fov = cam->fov * (float)M_PI / 180.f;
		ImGui::SliderAngle("FOV", &fov, 0, 180);
		cam->fov = fov * 180.f / (float)M_PI;
		ImGui::DragFloat("Near", &cam->_near);
		ImGui::DragFloat("Far", &cam->_far);
		ImGui::Checkbox("Free mode", &cam->freeMode);
	}

	void winInspector()
	{
		if (!win->inspector) return;
		ImGui::Begin("Inspector", &win->inspector, window_flags);
		if (auto sltd = AppInstance::GetSingleton()->selectedInHieararchy)
		{
			char name[128];
			strncpy(name, sltd->GetName().c_str(), 127); name[127] = 0;
			if (ImGui::InputText("Name", name, 128)) sltd->SetName(name);

			ImGui::SeparatorText("Transform");
			Transform& t = sltd->GetTransform();
			double p[3] = { t.position.x, t.position.y, t.position.z };
			if (ImGui::InputScalarN("Position", ImGuiDataType_Double, p, 3))
			{ t.position.x = p[0]; t.position.y = p[1]; t.position.z = p[2]; }
			Vector3 er = t.EulerDeg();
			double r[3] = { er.x, er.y, er.z };
			if (ImGui::InputScalarN("Rotation (deg)", ImGuiDataType_Double, r, 3))
				t.SetEulerDeg(Vector3(r[0], r[1], r[2]));
			double s[3] = { t.scale.x, t.scale.y, t.scale.z };
			if (ImGui::InputScalarN("Scale", ImGuiDataType_Double, s, 3))
			{ t.scale.x = s[0]; t.scale.y = s[1]; t.scale.z = s[2]; }

			if (ImGui::CollapsingHeader("Components"))
			{
				for (auto cmp : sltd->components)
				{
					if (ImGui::CollapsingHeader(cmp->name))
					{
						ImGui::Checkbox("Enabled", &cmp->enabled);
						if (auto cam = dynamic_cast<Camera*>(cmp))
							CamComponent(cam);
					}
				}
			}
		}
		else
		{
			ImGui::TextWrapped("Select an object in the Hierarchy.");
		}
		ImGui::End();
	}

	void winRender()
	{
		if (!win->render) return;
		ImGui::Begin("Render", &win->render, window_flags);
		ImVec2 avail = ImGui::GetContentRegionAvail();
		iRender* r = AppInstance::GetSingleton()->render;
		if (r && avail.x >= 1.0f && avail.y >= 1.0f)
		{
			if (sceneRTId == 0)
			{
				sceneRTId = r->createRenderTarget((int)avail.x, (int)avail.y);
				if (editorCam) editorCam->renderTarget = sceneRTId; // editor cam draws here
			}
			else
			{
				r->resizeRenderTarget(sceneRTId, (int)avail.x, (int)avail.y); // match the panel
			}
			uint64_t tex = r->getRenderTargetTexture(sceneRTId);
			if (tex)
				ImGui::Image((ImTextureID)tex, avail); // the live scene viewport
			else
				ImGui::TextDisabled("No scene texture.");

			// Viewport camera control (while hovering the image):
			//   RMB drag = orbit/look, MMB drag = pan, wheel = dolly.
			if (editorCam && editorCam->transform && ImGui::IsItemHovered())
			{
				ImGuiIO& io = ImGui::GetIO();
				Transform* t = editorCam->transform;
				const float rotSpeed = 0.005f, panSpeed = 0.01f, zoomSpeed = 0.5f;
				if (ImGui::IsMouseDragging(ImGuiMouseButton_Right))
				{
					camYaw   += io.MouseDelta.x * rotSpeed;
					camPitch += io.MouseDelta.y * rotSpeed;
					const float lim = 1.55f; // ~89deg pitch clamp
					if (camPitch >  lim) camPitch =  lim;
					if (camPitch < -lim) camPitch = -lim;
					t->SetEulerDeg(Vector3(camPitch * 57.29578f, camYaw * 57.29578f, 0.0f));
				}
				if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
				{
					t->position += t->right() * (double)(-io.MouseDelta.x * panSpeed)
					             + t->up()    * (double)( io.MouseDelta.y * panSpeed);
				}
				if (io.MouseWheel != 0.0f)
					t->position += t->direction() * (double)(io.MouseWheel * zoomSpeed);
			}
		}
		ImGui::End();
	}

	void winAbout()
	{
		if (!win->about) return;
		ImGui::Begin("About", &win->about, window_flags);
		ImGui::TextWrapped("NukeEngine - free, modular game engine. Renderer (Diligent) and UI (ImGui) "
		                   "are loaded as independent modules and communicate only through a neutral seam.");
		ImGui::End();
	}

	void winConsole()
	{
		if (!win->console) return;
		ImGui::Begin("Console", &win->console, window_flags);
		ImGui::End();
	}

	void winBrowser()
	{
		if (!win->browser) return;
		ImGui::Begin("Browser", &win->browser, window_flags);
		ImGui::End();
	}

	void PluginMGRWindow()
	{
		if (!win->plugmgr) return;
		if (ImGui::Begin("Plugins", &win->plugmgr))
		{
			ImGui::TextWrapped("To install a plugin, put it in the `modules` directory.");
			ImGui::Separator();
			int idx = 0;
			for (auto& mod : modules)
			{
				bool sel = (selectedPluginIndex == idx);
				if (ImGui::Selectable(mod->title, sel))
				{
					selectedPluginIndex = idx;
					selectedPlugin = mod;
				}
				++idx;
			}
			ImGui::Separator();
			if (selectedPlugin)
			{
				ImGui::TextUnformatted(selectedPlugin->title);
				ImGui::TextUnformatted(selectedPlugin->author);
				ImGui::TextUnformatted(selectedPlugin->version);
				ImGui::TextWrapped("%s", selectedPlugin->description);
				if (selectedPlugin->HasSettings() && ImGui::Button("Settings"))
					selectedPlugin->Settings();
			}
		}
		ImGui::End();
	}

	// ---- toolbar ----
	// A flat button that stays highlighted while `active` (radio/toggle look).
	bool ToolBtn(const char* icon, const char* tip, bool active, float w)
	{
		if (active)
		{
			ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.42f, 0.30f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.55f, 0.38f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.30f, 0.65f, 0.45f, 1.0f));
		}
		bool clicked = ImGui::Button(icon, ImVec2(w, 0));
		if (active) ImGui::PopStyleColor(3);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
		return clicked;
	}

	void SpawnEmpty()
	{
		AppInstance* app = AppInstance::GetSingleton();
		GameObject* go = new GameObject("Empty");
		app->currentScene->Add(go);
		app->selectedInHieararchy = go;
	}
	void SpawnCube()
	{
		AppInstance* app = AppInstance::GetSingleton();
		GameObject* go = new GameObject("Cube");
		MeshRenderer* mr = new MeshRenderer();
		go->AddComponent(mr);
		mr->mesh = Mesh::CreateCube();
		app->currentScene->Add(go);
		app->selectedInHieararchy = go;
	}
	void SpawnCamera()
	{
		AppInstance* app = AppInstance::GetSingleton();
		GameObject* go = new GameObject("Camera");
		Camera* c = new Camera();
		c->renderer = app->render;          // share the active renderer (avoids re-init / null deref)
		go->AddComponent(c);
		app->currentScene->Add(go);
		app->selectedInHieararchy = go;
	}

	// Second row under the main menu: tools (left) | PIE (center) | viewport mode (right).
	void Toolbar()
	{
		ImGuiViewport* vp = ImGui::GetMainViewport();
		// Keep WindowPadding pushed across the WHOLE window scope (Begin..End) so the
		// top and bottom padding match — otherwise the row sticks to the top edge.
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
		float barH = ImGui::GetFrameHeight() + 12.0f;
		ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
		bool open = ImGui::BeginViewportSideBar("##nuke-toolbar", vp, ImGuiDir_Up, barH, flags);
		if (open)
		{
			AppInstance* app = AppInstance::GetSingleton();
			ImGuiStyle& st = ImGui::GetStyle();
			const float bw = ImGui::GetFrameHeight();   // square icon buttons

			// LEFT — manipulation tools + create
			if (ToolBtn(ICON_LC_MOUSE_POINTER, "Select", app->manipulationMode == 0, bw)) app->manipulationMode = 0; ImGui::SameLine();
			if (ToolBtn(ICON_LC_MOVE,          "Move",   app->manipulationMode == 1, bw)) app->manipulationMode = 1; ImGui::SameLine();
			if (ToolBtn(ICON_LC_ROTATE_3D,     "Rotate", app->manipulationMode == 2, bw)) app->manipulationMode = 2; ImGui::SameLine();
			if (ToolBtn(ICON_LC_SCALING,       "Scale",  app->manipulationMode == 3, bw)) app->manipulationMode = 3; ImGui::SameLine();
			if (ToolBtn(ICON_LC_PLUS,          "Create", false,                       bw)) ImGui::OpenPopup("##nuke-create");
			if (ImGui::BeginPopup("##nuke-create"))
			{
				if (ImGui::MenuItem("Empty"))  SpawnEmpty();
				if (ImGui::MenuItem("Cube"))   SpawnCube();
				if (ImGui::MenuItem("Camera")) SpawnCamera();
				ImGui::EndPopup();
			}

			// CENTER — PIE (Play / Pause / Stop)
			float winW = ImGui::GetWindowWidth();
			float centerW = bw * 3 + st.ItemSpacing.x * 2;
			ImGui::SameLine();
			ImGui::SetCursorPosX((winW - centerW) * 0.5f);
			if (ToolBtn(ICON_LC_PLAY,   "Play",  app->playState == 1, bw)) app->playState = 1; ImGui::SameLine();
			if (ToolBtn(ICON_LC_PAUSE,  "Pause", app->playState == 2, bw)) app->playState = 2; ImGui::SameLine();
			if (ToolBtn(ICON_LC_SQUARE, "Stop",  app->playState == 0, bw)) app->playState = 0;

			// RIGHT — viewport draw mode (Solid / Wireframe)
			float rightW = bw * 2 + st.ItemSpacing.x;
			ImGui::SameLine();
			ImGui::SetCursorPosX(winW - rightW - 8.0f);
			if (ToolBtn(ICON_LC_BOX,      "Solid",     !app->wireframe, bw)) app->wireframe = false; ImGui::SameLine();
			if (ToolBtn(ICON_LC_GRID_3X3, "Wireframe",  app->wireframe, bw)) app->wireframe = true;
		}
		ImGui::End();
		ImGui::PopStyleVar();
	}

	void Draw()
	{
		// Order matters: main menu, then the toolbar side-bar, then the dock space —
		// each reserves viewport work-area for the next, so panels sit below both bars.
		EditorMenu();
		Toolbar();
		// Full-window dock space so panels can be docked/tabbed/split (sticky).
		// PassthruCentralNode leaves the centre transparent for the scene viewport.
		ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
		window_flags = 0; // panels are dockable/movable now
		for (auto tup : *AppInstance::GetSingleton()->editorWindows)
			tup.second();
	}
};

inline void editorinit() { EditorUI::getSingleton()->SetUp(); }
inline void editorDraw() { EditorUI::getSingleton()->Draw(); }

#endif // EDITORUI_H
