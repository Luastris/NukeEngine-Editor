#ifndef EDITORUI_H
#define EDITORUI_H
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "ImGuizmo/ImGuizmo.h"
#include <GL/freeglut.h>
#include "gui/gui.h"
#include "config.h"
#include "interface/AppInstance.h"
#include "interface/Modular.h"
#include <boost/container/list.hpp>
#include <math.h>

#ifdef _MSC_VER
#pragma warning (disable: 4505) // unreferenced local function has been removed
#endif

static int g_Time = 0;
static GLuint g_FontTexture = 0;

void ImGui_ImplOpenGL2_RenderDrawData(ImDrawData* draw_data)
{
    // Avoid rendering when minimized, scale coordinates for retina displays (screen coordinates != framebuffer coordinates)
    ImGuiIO& io = ImGui::GetIO();
    int fb_width = (int)(draw_data->DisplaySize.x * io.DisplayFramebufferScale.x);
    int fb_height = (int)(draw_data->DisplaySize.y * io.DisplayFramebufferScale.y);
    if (fb_width == 0 || fb_height == 0)
        return;
    draw_data->ScaleClipRects(io.DisplayFramebufferScale);

    // We are using the OpenGL fixed pipeline to make the example code simpler to read!
    // Setup render state: alpha-blending enabled, no face culling, no depth testing, scissor enabled, vertex/texcoord/color pointers, polygon fill.
    GLint last_texture; glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture);
    GLint last_polygon_mode[2]; glGetIntegerv(GL_POLYGON_MODE, last_polygon_mode);
    GLint last_viewport[4]; glGetIntegerv(GL_VIEWPORT, last_viewport);
    GLint last_scissor_box[4]; glGetIntegerv(GL_SCISSOR_BOX, last_scissor_box);
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TRANSFORM_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnable(GL_TEXTURE_2D);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    //glUseProgram(0); // You may want this if using this code in an OpenGL 3+ context where shaders may be bound

    // Setup viewport, orthographic projection matrix
    // Our visible imgui space lies from draw_data->DisplayPps (top left) to draw_data->DisplayPos+data_data->DisplaySize (bottom right). DisplayMin is typically (0,0) for single viewport apps.
    glViewport(0, 0, (GLsizei)fb_width, (GLsizei)fb_height);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(draw_data->DisplayPos.x, draw_data->DisplayPos.x + draw_data->DisplaySize.x, draw_data->DisplayPos.y + draw_data->DisplaySize.y, draw_data->DisplayPos.y, -1.0f, +1.0f);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    // Render command lists
    ImVec2 pos = draw_data->DisplayPos;
    for (int n = 0; n < draw_data->CmdListsCount; n++)
    {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        const ImDrawVert* vtx_buffer = cmd_list->VtxBuffer.Data;
        const ImDrawIdx* idx_buffer = cmd_list->IdxBuffer.Data;
        glVertexPointer(2, GL_FLOAT, sizeof(ImDrawVert), (const GLvoid*)((const char*)vtx_buffer + IM_OFFSETOF(ImDrawVert, pos)));
        glTexCoordPointer(2, GL_FLOAT, sizeof(ImDrawVert), (const GLvoid*)((const char*)vtx_buffer + IM_OFFSETOF(ImDrawVert, uv)));
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ImDrawVert), (const GLvoid*)((const char*)vtx_buffer + IM_OFFSETOF(ImDrawVert, col)));

        for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
        {
            const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
            if (pcmd->UserCallback)
            {
                // User callback (registered via ImDrawList::AddCallback)
                pcmd->UserCallback(cmd_list, pcmd);
            }
            else
            {
                ImVec4 clip_rect = ImVec4(pcmd->ClipRect.x - pos.x, pcmd->ClipRect.y - pos.y, pcmd->ClipRect.z - pos.x, pcmd->ClipRect.w - pos.y);
                if (clip_rect.x < fb_width && clip_rect.y < fb_height && clip_rect.z >= 0.0f && clip_rect.w >= 0.0f)
                {
                    // Apply scissor/clipping rectangle
                    glScissor((int)clip_rect.x, (int)(fb_height - clip_rect.w), (int)(clip_rect.z - clip_rect.x), (int)(clip_rect.w - clip_rect.y));

                    // Bind texture, Draw
                    glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)pcmd->TextureId);
                    glDrawElements(GL_TRIANGLES, (GLsizei)pcmd->ElemCount, sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, idx_buffer);
                }
            }
            idx_buffer += pcmd->ElemCount;
        }
    }

    // Restore modified state
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glBindTexture(GL_TEXTURE_2D, (GLuint)last_texture);
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopAttrib();
    glPolygonMode(GL_FRONT, (GLenum)last_polygon_mode[0]); glPolygonMode(GL_BACK, (GLenum)last_polygon_mode[1]);
    glViewport(last_viewport[0], last_viewport[1], (GLsizei)last_viewport[2], (GLsizei)last_viewport[3]);
    glScissor(last_scissor_box[0], last_scissor_box[1], (GLsizei)last_scissor_box[2], (GLsizei)last_scissor_box[3]);
}

void TogglePluginMGR(){
    Config::getSingleton()->window.plugmgr = !Config::getSingleton()->window.plugmgr;
}

bool PlugTitleGetter(void* data, int n, const char** out_text)
{
  const bc::vector<boost::shared_ptr<NUKEModule>>* v = (bc::vector<boost::shared_ptr<NUKEModule>>*)data;
  *out_text = v->at(n).get()->title;
  return true;
}

bool HierarchyGetter(void* data, int n, const char** out_text)
{
  const bc::list<GameObject*>* v = (bc::list<GameObject*>*)data;
  auto it = v->begin();
  std::advance(it, n);
  *out_text = (*it)->GetName().c_str();
  return true;
}

class EditorUI
{
private:
    EditorUI() {}
    ~EditorUI() {}
    struct NukeWindow * win;
    boost::shared_ptr<NUKEModule> selectedPlugin = nullptr;
    int selectedPluginIndex = -1;
    int selectedGameObjectIndex = -1;
    bool freezeWindows = true;
    ImGuiWindowFlags window_flags = 0;
    Camera* editorCam = nullptr;
    int prevX, prevY;
    bool moveKey, crosshair;
    float lx=0.0f,ly=0.0f,lz=0.0f;
    const float g_rotation_speed = M_PI/180*0.2;
public:
    static EditorUI* getSingleton(){
        static EditorUI instance;
        return &instance;
    }

    ImGuiContext* context;


    void SetUp(){
        cout << "[editorui]\t\t" << "EditorUI initialization..." << endl;
        context = ImGui::CreateContext();
		cout << "[editorui]\t\t" << "Context: " << context << endl;

        ImGuiIO& io = ImGui::GetIO();
		cout << "[editorui]\t\t" << "Io: " << &io << endl;
		g_Time = 0;

        // Glut has 1 function for characters and one for "special keys". We map the characters in the 0..255 range and the keys above.
        io.KeyMap[ImGuiKey_Tab]         = '\t'; // == 9 == CTRL+I
        io.KeyMap[ImGuiKey_LeftArrow]   = 256 + GLUT_KEY_LEFT;
        io.KeyMap[ImGuiKey_RightArrow]  = 256 + GLUT_KEY_RIGHT;
        io.KeyMap[ImGuiKey_UpArrow]     = 256 + GLUT_KEY_UP;
        io.KeyMap[ImGuiKey_DownArrow]   = 256 + GLUT_KEY_DOWN;
        io.KeyMap[ImGuiKey_PageUp]      = 256 + GLUT_KEY_PAGE_UP;
        io.KeyMap[ImGuiKey_PageDown]    = 256 + GLUT_KEY_PAGE_DOWN;
        io.KeyMap[ImGuiKey_Home]        = 256 + GLUT_KEY_HOME;
        io.KeyMap[ImGuiKey_End]         = 256 + GLUT_KEY_END;
        io.KeyMap[ImGuiKey_Insert]      = 256 + GLUT_KEY_INSERT;
        io.KeyMap[ImGuiKey_Delete]      = 127;
        io.KeyMap[ImGuiKey_Backspace]   = 8;  // == CTRL+H
        io.KeyMap[ImGuiKey_Space]       = ' ';
        io.KeyMap[ImGuiKey_Enter]       = 13; // == CTRL+M
        io.KeyMap[ImGuiKey_Escape]      = 27;
        io.KeyMap[ImGuiKey_A]           = 'A';
        io.KeyMap[ImGuiKey_C]           = 'C';
        io.KeyMap[ImGuiKey_V]           = 'V';
        io.KeyMap[ImGuiKey_X]           = 'X';
        io.KeyMap[ImGuiKey_Y]           = 'Y';
        io.KeyMap[ImGuiKey_Z] = 'Z';

		cout << "[editorui]\t\t" << "SetUp GUI...." << endl;
		cout << "[editorui]\t\t" << "Editor is... " << AppInstance::GetSingleton() << endl;
		cout << "[editorui]\t\t" << "Config is... " << Config::getSingleton() << endl;
        GUI::getSingleton()->setup(context);
		
		win = &Config::getSingleton()->window;

		cout << "[editorui]\t\t" << "Load font from... " << win->mainFont << endl;
        io.Fonts->AddFontFromFileTTF(Config::getSingleton()->window.mainFont.c_str(), 19.f);
        
        InitMenu();
        *KeyBoard::getSingleton() &= b::function<void(unsigned char, int, int)>(b::bind(&EditorUI::OnKeyBoardUp, this, _1, _2, _3));

//        AppInstance::GetSingleton()->render->setOnGUI(b::bind(&EditorUI::transformSelected, this));
        AppInstance* editor = AppInstance::GetSingleton();
		cout << "[editorui]\t\t" << "Editor is " << editor << ";, this is: " << this << endl;
        editor->PushWindow("nukeeditor-about", b::bind(&EditorUI::winAbout, this));
        editor->PushWindow("nukeeditor-browser", b::bind(&EditorUI::winBrowser, this));
        editor->PushWindow("nukeeditor-console", b::bind(&EditorUI::winConsole, this));
        editor->PushWindow("nukeeditor-hierarchy", b::bind(&EditorUI::winHierarchy, this));
        editor->PushWindow("nukeeditor-inspector", b::bind(&EditorUI::winInspector, this));
        editor->PushWindow("nukeeditor-render", b::bind(&EditorUI::winRender, this));
        editor->PushWindow("nukeeditor-plugins", b::bind(&EditorUI::PluginMGRWindow, this));
		GameObject* camObj = editor->currentScene->Get("Editor Camera");
		cout << "[editorui]\t\t" << "HIERARCHY SIZE: " << editor->currentScene->GetHierarchy().size() << endl;
		cout << "[editorui]\t\t" << "CAM:  " << camObj << endl;
        if(camObj)
			editorCam = camObj->GetComponent<Camera>();
		else
		{
			cout << "[editorui]\t\t" << "Editor Camera not found. Terminating." << endl;
		}
        cout << "[editorui]\t\t" << "EditorUI Initialization finished" << endl;
    }

    bool ImGui_ImplOpenGL2_CreateFontsTexture()
    {
        // Build texture atlas
        ImGuiIO& io = ImGui::GetIO();
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);   // Load as RGBA 32-bits (75% of the memory is wasted, but default font is so small) because it is more likely to be compatible with user's existing shaders. If your ImTextureId represent a higher-level concept than just a GL texture id, consider calling GetTexDataAsAlpha8() instead to save on GPU memory.

        // Upload texture to graphics system
        GLint last_texture;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture);
        glGenTextures(1, &g_FontTexture);
        glBindTexture(GL_TEXTURE_2D, g_FontTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

        // Store our identifier
        io.Fonts->TexID = (void *)(intptr_t)g_FontTexture;

        // Restore state
        glBindTexture(GL_TEXTURE_2D, last_texture);

        return true;
    }

    void ImGui_ImplOpenGL2_DestroyFontsTexture()
    {
        if (g_FontTexture)
        {
            ImGuiIO& io = ImGui::GetIO();
            glDeleteTextures(1, &g_FontTexture);
            io.Fonts->TexID = 0;
            g_FontTexture = 0;
        }
    }

    bool    ImGui_ImplOpenGL2_CreateDeviceObjects()
    {
        return ImGui_ImplOpenGL2_CreateFontsTexture();
    }

    void    ImGui_ImplOpenGL2_DestroyDeviceObjects()
    {
        ImGui_ImplOpenGL2_DestroyFontsTexture();
    }

    void preDraw(){
//        cout << "[editorui]\t\t" << "PreDraw" << endl;
        ImGuiIO& io = ImGui::GetIO();
        int current_time = glutGet(GLUT_ELAPSED_TIME);
        io.DeltaTime = (current_time - g_Time) / 1000.0f;
        g_Time = current_time;
        if (!g_FontTexture)
            ImGui_ImplOpenGL2_CreateDeviceObjects();
        // Start the frame
        ImGui::NewFrame();
//        cout << "[editorui]\t\t" << "End PreDraw" << endl;
    }

    void postDraw(){
//        cout << "[editorui]\t\t" << "PostDraw" << endl;
        ImGui::Render();
        ImGuiIO& io = ImGui::GetIO();
        glViewport(0, 0, (GLsizei)io.DisplaySize.x, (GLsizei)io.DisplaySize.y);
        glClearColor(0.0, 0.0, 0.0, 0.0);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
//        cout << "[editorui]\t\t" << "End PostDraw" << endl;
    }


    void InitMenu()
    {
        MenuStrip* mstrip = AppInstance::GetSingleton()->menuStrip = new MenuStrip();
        mstrip->AddItem("Tools/", "Plugin manager", TogglePluginMGR);
        mstrip->AddItem("Tools/Other", "Deep tools", TogglePluginMGR);
        mstrip->AddItem("Tools/Other/One more level", "...", TogglePluginMGR);
        mstrip->AddItem("Tools/Other/One more level/Last", "Wow", TogglePluginMGR);
    }

    bool EditorSubMenu(MenuItem* item)
    {
        if (item->subitems.size() > 0) {

            if(ImGui::BeginMenu(item->name.c_str())){
                for (auto subitem : item->subitems)
                {
                    EditorSubMenu(subitem);
                }
                ImGui::EndMenu();
            }
        }else
            if(item->callback)
            {
                if(ImGui::MenuItem(item->name.c_str()))
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
                    ShowMenuFile();
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Edit"))
                {
                    if (ImGui::MenuItem("Undo", "CTRL+Z")) {}
                    if (ImGui::MenuItem("Redo", "CTRL+Y", false, false)) {}  // Disabled item
                    ImGui::Separator();
                    if (ImGui::MenuItem("Cut", "CTRL+X")) {}
                    if (ImGui::MenuItem("Copy", "CTRL+C")) {}
                    if (ImGui::MenuItem("Paste", "CTRL+V")) {}
                    ImGui::EndMenu();
                }
                for (auto rootElement : AppInstance::GetSingleton()->menuStrip->strip)
                {
                    EditorSubMenu(rootElement);
                }
                if (ImGui::BeginMenu("Window"))
                {
                    ShowMenuWindow();
                    ImGui::EndMenu();
                }

                ImGui::EndMainMenuBar();
            }
    }

    bool TreeNodeV(const void* ptr_id, bool isSelected, bool isLeaf, const char* fmt, va_list args)
    {
        bool opened = false;

        if (isSelected)
        {
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImU32 col = ImColor(ImGui::GetStyle().Colors[ImGuiCol_HeaderHovered]);
            ImGui::RenderFrame(
                pos,
                ImVec2(pos.x + ImGui::GetContentRegionMax().x, pos.y + ImGui::GetTextLineHeight()),
                col,
                false);
        }

        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return false;

        auto& g = *GImGui;
        ImFormatStringV(g.TempBuffer, IM_ARRAYSIZE(g.TempBuffer), fmt, args);

        if (!ptr_id)
            ptr_id = fmt;

        if (!isLeaf)
        {
            ImGui::PushID(ptr_id);
            opened = ImGui::CollapsingHeader(g.TempBuffer, "", false);
            ImGui::PopID();
        }
        else
        {
            ImGui::Text(g.TempBuffer);
        }

        bool result = !isLeaf && opened;

        if (result)
        {
            ImGui::TreePush(ptr_id);
        }

        return result;
    }

    bool TreeNode(const void* ptr_id, bool isSelected, bool isLeaf, const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        bool s = TreeNodeV(ptr_id, isSelected, isLeaf, fmt, args);
        va_end(args);
        return s;
    }

    void mainMenu(){
        EditorMenu();
    }

    void ShowMenuFile()
    {
        if (ImGui::MenuItem("New")) {}
        if (ImGui::MenuItem("Open", "Ctrl+O")) {}
        if (ImGui::BeginMenu("Open Recent"))
        {
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Save", "Ctrl+S")) {}
        if (ImGui::MenuItem("Save As..")) {}
        ImGui::Separator();
        if (ImGui::BeginMenu("Options"))
        {
            static bool enabled = true;
            ImGui::MenuItem("Enabled", "", &enabled);
            ImGui::BeginChild("child", ImVec2(0, 60), true);
            for (int i = 0; i < 10; i++)
                ImGui::Text("Scrolling Text %d", i);
            ImGui::EndChild();
            static float f = 0.5f;
            static int n = 0;
            static bool b = true;
            ImGui::SliderFloat("Value", &f, 0.0f, 1.0f);
            ImGui::InputFloat("Input", &f, 0.1f);
            ImGui::Combo("Combo", &n, "Yes\0No\0Maybe\0\0");
            ImGui::Checkbox("Check", &b);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Quit", "Alt+F4")) {
            glutLeaveMainLoop();
        }
    }

    void ShowMenuWindow()
    {
        if(ImGui::MenuItem("Fullscreen", "F11"))
            glutFullScreenToggle();
        ImGui::MenuItem("Freeze windows", "F8", &freezeWindows);

        ImGui::Separator();
        ImGui::MenuItem("Hierarchy", "Ctrl+Alt+H", &win->hierarchy);
        ImGui::MenuItem("Console", "Ctrl+Alt+C", &win->console);
        ImGui::MenuItem("Browser", "Ctrl+Alt+B", &win->browser);
        ImGui::MenuItem("Render", "Ctrl+Alt+R", &win->render);
        ImGui::MenuItem("Inspector", "Ctrl+Alt+I", &win->inspector);

        ImGui::MenuItem("About", "", &win->about);
    }

    void DisplayRecursiveGameObjectHierarchy(bc::list<GameObject*> &gos){
        //ShowHelpMarker("Click to select, CTRL+Click to toggle, double-click to open");
        static int selection_mask = 0x02;   // Dumb representation of what may be user-side selection state. You may carry selection state inside or outside your objects in whatever format you see fit.

        int node_clicked = -1;

        for (int i = 0; i < ((int)gos.size()); i++)
        {
            auto it = gos.begin(); //AppInstance::GetSingleton()->currentScene->hierarchy.begin();
            std::advance(it, i);
            ImGuiTreeNodeFlags node_flags = ((selection_mask & (1 << i)) ? ImGuiTreeNodeFlags_Selected : 0) | ImGuiTreeNodeFlags_OpenOnDoubleClick;
            bool opened = ImGui::TreeNodeEx((void*)(intptr_t)i, node_flags, (*it)->GetName().c_str(), i);
            if (ImGui::IsItemClicked())
            {
                node_clicked = i;
                selectedGameObjectIndex = i;
                AppInstance::GetSingleton()->selectedInHieararchy = (*it);
//                cout << "[editorui]\t\t" << "SELECTED: " << AppInstance::GetSingleton()->selectedInHieararchy << endl;
            }
            if (opened)
            {
                if((*it)->children.size() > 0)
                    DisplayRecursiveGameObjectHierarchy((*it)->children);
                else
                    ImGui::Text("No children");

                ImGui::TreePop();
            }
        }
        if (node_clicked != -1)
        {
            // Update selection state. Process outside of tree loop to avoid visual inconsistencies during the clicking-frame.
            if (ImGui::GetIO().KeyCtrl)
                selection_mask ^= (1 << node_clicked);  // CTRL+click to toggle
            else
                selection_mask = (1 << node_clicked);   // Click to single-select
        }



    }

    void CamComponent(Camera* cam){
        ImGui::InputInt("Width", &cam->renderer->width);
        ImGui::InputInt("Height", &cam->renderer->height);
        float __fov = cam->fov * M_PI / 180;
        ImGui::SliderAngle("FOV", &__fov, 0, 180);
        cam->fov = __fov * 180 / M_PI;
        ImGui::DragFloat("Near", &cam->_near);
        ImGui::DragFloat("Far", &cam->_far);
        ImGui::Checkbox("Free mode", &cam->freeMode);
    }

    void OnKeyBoardUp(unsigned char c, int x, int y){
        switch(c){
            case 'f' | 'F':
            if(auto selected = AppInstance::GetSingleton()->selectedInHieararchy){
                auto cam = AppInstance::GetSingleton()->currentScene->Get("Editor Camera");
                if(cam && (&cam->GetTransform() != &selected->GetTransform())){
                    Vector3 distance = (cam->GetTransform().globalPosition() - selected->GetTransform().globalPosition());
                    cout << "[editorui]\t\t" << "DIST: " << distance.toStringA() << endl;
                    double length = 100;
                    cout << "[editorui]\t\t" << "LEN: " << length << endl;
                    cam->GetTransform().position = selected->GetTransform().position + (distance.normalize()) * (length);
                    // TODO: fix look at!
                    cam->GetTransform().rotation = Vector3(cam->GetTransform().rotation.x - distance.normalize().x, cam->GetTransform().rotation.y - distance.normalize().y, cam->GetTransform().rotation.y);
                }
            }
            break;
        }
    }

    void transformSelected(ImVec2 minPos, ImVec2 maxPos){
        if(auto tg = AppInstance::GetSingleton()->selectedInHieararchy)
            EditTransform(&tg->GetTransform(), minPos, maxPos);
    }

    void EditTransform(Transform* t, ImVec2 minPos, ImVec2 maxPos)
    {
        //static mat4 transform;
        //cout << "[editorui]\t\t" << "EDIT TRANSFORM" << endl;
        auto cam = AppInstance::GetSingleton()->currentScene->Get("Editor Camera");
        auto camCmp = cam->GetComponent<Camera>();

        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist();
        ImGuizmo::Enable(true);


        ImGuiIO& io = ImGui::GetIO();
//        cout << "[editorui]\t\t" << "IO SIZE: " << io.DisplaySize.x << ", " << io.DisplaySize.y << endl;
        ImGuizmo::SetRect(minPos.x, minPos.y, maxPos.x, maxPos.y);

        float* _view = new float[16];
        glm::mat4 mmat = glm::mat4();
        glm::scale(mmat, glm::vec3{t->globalScale().x, t->globalScale().y, t->globalScale().z});
        glm::rotate(mmat, (float)t->globalRotation().x , glm::vec3{1, 0, 0});
        glm::rotate(mmat, (float)t->globalRotation().y , glm::vec3{0, 1, 0});
        glm::rotate(mmat, (float)t->globalRotation().z , glm::vec3{0, 0, 1});
        glm::translate(mmat, glm::vec3
                        {
                           t->globalPosition().x,
                           t->globalPosition().y,
                           t->globalPosition().z
                       });
        _view =(float*)glm::value_ptr(mmat);
//        glGetFloatv(GL_MODELVIEW_MATRIX, _view);
        glm::fmat4 view = glm::mat4(*_view);

        float* _proj = new float[16];
        glm::mat4 pmat = glm::perspectiveFov(camCmp->fov, io.DisplaySize.x, io.DisplaySize.y, camCmp->_near, camCmp->_far);
        _proj = (float*)glm::value_ptr(pmat);
//        glGetFloatv(GL_PROJECTION_MATRIX, _proj);
        glm::fmat4 projection = pmat; //glm::mat4(*_proj);


        auto matrix = //glm::mat4();
        glm::lookAt(
            glm::vec3 { cam->GetTransform().globalPosition().x, cam->GetTransform().globalPosition().y, cam->GetTransform().globalPosition().z },
            //{ t->globalPosition().x, t->globalPosition().y, t->globalPosition().z },
            { cam->GetTransform().globalPosition().x + cam->GetTransform().direction().x, cam->GetTransform().globalPosition().y + cam->GetTransform().direction().y, cam->GetTransform().globalPosition().z + cam->GetTransform().direction().z },
            { 0, 1, 0});

//        glutSolidSphere(1.0, 50, 50);
//ssssss


        float globalDelta[] = {
            (float)t->globalPosition().x - (float)t->position.x,
            (float)t->globalPosition().y - (float)t->position.y,
            (float)t->globalPosition().z - (float)t->position.z
        };
        float globalDeltaRot[] = {
            (float)t->globalRotation().x - (float)t->rotation.x,
            (float)t->globalRotation().y - (float)t->rotation.y,
            (float)t->globalRotation().z - (float)t->rotation.z
        };
        float globalDeltaScale[] = {
            (float)t->globalScale().x / (float)t->scale.x,
            (float)t->globalScale().y / (float)t->scale.y,
            (float)t->globalScale().z / (float)t->scale.z
        };
        float translation[] = { (float)t->globalPosition().x, (float)t->globalPosition().y, (float)t->globalPosition().z };
        float rotation[] = { (float)t->rotation.x, (float)t->rotation.y, (float)t->rotation.z }; //{ (float)t->globalRotation().x, (float)t->globalRotation().y, (float)t->globalRotation().z };
        float scale[] = { (float)t->scale.x, (float)t->scale.y, (float)t->scale.z };//{ (float)t->globalScale().x, (float)t->globalScale().y, (float)t->globalScale().z };

        ImGuizmo::RecomposeMatrixFromComponents(&translation[0], &rotation[0], &scale[0], &view[0][0]);
//        ImGuizmo::DrawCube(&matrix[0][0], &projection[0][0], &view[0][0]);
//        ImGuizmo::DrawGrid(&matrix[0][0], &projection[0][0], &view[0][0], 200.0f);
        //ImGuizmo::Manipulate(&view[0][0], &projection[0][0], ImGuizmo::ROTATE, ImGuizmo::WORLD, &matrix[0][0]);
        ImGuizmo::Manipulate(
                    &matrix[0][0],
                &projection[0][0],
                (ImGuizmo::OPERATION)AppInstance::GetSingleton()->manipulationMode,
                (ImGuizmo::MODE)AppInstance::GetSingleton()->manipulationWorld,
                &view[0][0],
                NULL,
                NULL);
        ImGuizmo::DecomposeMatrixToComponents(&view[0][0], &translation[0], &rotation[0], &scale[0]);
        t->position.x = translation[0] - globalDelta[0];
        t->position.y = translation[1] - globalDelta[1];
        t->position.z = translation[2] - globalDelta[2];
        t->rotation.x = rotation[0] - globalDeltaRot[0];
        t->rotation.y = rotation[1] - globalDeltaRot[1];
        t->rotation.z = rotation[2] - globalDeltaRot[2];
        t->scale.x = scale[0];// / globalDeltaScale[0];
        t->scale.y = scale[1];// / globalDeltaScale[1];
        t->scale.z = scale[2];// / globalDeltaScale[2];

    }

    void winHierarchy(){
        if(!win->hierarchy)
            return;

        ImGui::Begin("Hierarchy", &win->hierarchy, window_flags);
        //ImGui::ListBox("", &selectedGameObjectIndex, HierarchyGetter, static_cast<void*>(&AppInstance::GetSingleton()->currentScene->hierarchy), AppInstance::GetSingleton()->currentScene->hierarchy.size());
        DisplayRecursiveGameObjectHierarchy(AppInstance::GetSingleton()->currentScene->GetHierarchy());
        ImGui::End();
    }

    void winInspector(){
        if(!win->inspector)
            return;

        ImGui::Begin("Inspector", &win->inspector, window_flags);
        ImGui::Text("Manipulation space:");
        if (ImGui::RadioButton("Local", AppInstance::GetSingleton()->manipulationWorld == ImGuizmo::LOCAL))
            AppInstance::GetSingleton()->manipulationWorld = ImGuizmo::LOCAL;
        ImGui::SameLine();
        if (ImGui::RadioButton("World", AppInstance::GetSingleton()->manipulationWorld == ImGuizmo::WORLD))
            AppInstance::GetSingleton()->manipulationWorld = ImGuizmo::WORLD;

        ImGui::Text("Manipulation mode:");
        if (ImGui::RadioButton("Translate", AppInstance::GetSingleton()->manipulationMode == ImGuizmo::TRANSLATE))
            AppInstance::GetSingleton()->manipulationMode = ImGuizmo::TRANSLATE;
        ImGui::SameLine();
        if (ImGui::RadioButton("Rotate", AppInstance::GetSingleton()->manipulationMode == ImGuizmo::ROTATE))
            AppInstance::GetSingleton()->manipulationMode = ImGuizmo::ROTATE;
        ImGui::SameLine();
        if (ImGui::RadioButton("Scale", AppInstance::GetSingleton()->manipulationMode == ImGuizmo::SCALE))
            AppInstance::GetSingleton()->manipulationMode = ImGuizmo::SCALE;
        if(auto sltd = AppInstance::GetSingleton()->selectedInHieararchy){
            char *__name = new char[128];
            strcpy(__name, sltd->GetName().c_str());
            ImGui::InputText("Name", __name, 128);
            sltd->SetName(__name);

            char *__tag= new char[128];
            strcpy(__tag, sltd->GetTag().c_str());
            ImGui::InputText("Tag", __tag, 128);
            sltd->SetTag(__tag);

            ImGui::BeginGroup();
            ImGui::Text("Transform");
            ImGui::Text("Position");
            double posx = sltd->GetTransform().position.x,
                    posy = sltd->GetTransform().position.y,
                    posz = sltd->GetTransform().position.z;
            ImGui::Columns(3);
            ImGui::InputDouble("xp",&posx); ImGui::NextColumn();
            ImGui::InputDouble("yp",&posy); ImGui::NextColumn();
            ImGui::InputDouble("zp",&posz);
            sltd->GetTransform().position.x = posx;
            sltd->GetTransform().position.y = posy;
            sltd->GetTransform().position.z = posz;
            ImGui::EndColumns();

            ImGui::Text("Scale");
            double scx = sltd->GetTransform().scale.x,
                    scy = sltd->GetTransform().scale.y,
                    scz = sltd->GetTransform().scale.z;
            ImGui::Columns(3);
            ImGui::InputDouble("xs",&scx); ImGui::NextColumn();
            ImGui::InputDouble("ys",&scy); ImGui::NextColumn();
            ImGui::InputDouble("zs",&scz);
            sltd->GetTransform().scale.x = scx;
            sltd->GetTransform().scale.y = scy;
            sltd->GetTransform().scale.z = scz;
            ImGui::EndColumns();

            ImGui::Text("Rotation");
            ImGui::Columns(3);
            ImGui::InputDouble("xr",&sltd->GetTransform().rotation.x);
            ImGui::NextColumn();
            ImGui::InputDouble("yr",&sltd->GetTransform().rotation.y);
            ImGui::NextColumn();
            ImGui::InputDouble("zr",&sltd->GetTransform().rotation.z);
            //ImGui::InputDouble("wr",&sltd->transform.rotation.w);
            ImGui::EndColumns();

            ImGui::EndGroup();

            if(ImGui::CollapsingHeader("Components")){
                for(auto cmp : sltd->components){
                    if(ImGui::CollapsingHeader(cmp->name)){
                        ImGui::Checkbox("Enabled", &cmp->enabled);
                        if(auto cam = dynamic_cast<Camera*>(cmp)){
                            CamComponent(cam);
                        }
                    }
                }
            }
            //DisplayRecursiveGameObjectHierarchy(AppInstance::GetSingleton()->currentScene->hierarchy);
        }
        ImGui::End();
    }

    void winRender(){
        if(!win->render)
            return;

        ImGui::Begin("Render", &win->render, window_flags);
        ImVec2 pos = ImGui::GetWindowPos();
// TODO: bring it back
//        auto tex = dynamic_cast<NukeOGL*>(AppInstance::GetSingleton()->render)->getRenderTexture();
        ImVec2 maxPos = ImVec2(pos.x + ImGui::GetWindowSize().x, pos.y + ImGui::GetWindowSize().y + 2);
//        ImGui::GetWindowDrawList()->AddImage((void *)tex,
//                                             ImVec2(ImGui::GetItemRectMin().x + 0,
//                                            ImGui::GetItemRectMin().y - 2),
//                                             maxPos, ImVec2(0,1), ImVec2(1,0));


//        if(AppInstance::GetSingleton()->selectedInHieararchy){
//            EditTransform(&AppInstance::GetSingleton()->selectedInHieararchy->transform);
//        }

        transformSelected(pos, maxPos);
        ImGui::End();
    }

    void winAbout(){
        if(!win->about)
            return;

        ImGui::Begin("About", &win->about, window_flags);
        ImGui::TextWrapped("NukeEngine - free open source game engine, developed by ExQDev team and people:). It was made to help developerss make their games as they like, easy and fast. NukeEngine is modular, and allows you to extend it. It allows your games to be modded too. And looks like Unity and has coder-friendly API:) \nEnjoy!");
        ImGui::End();
    }

    void winConsole(){
        if(!win->console)
            return;

        ImGui::Begin("Console", &win->console, window_flags);

        ImGui::End();
    }

    void winBrowser(){
        if(!win->browser)
            return;

        ImGui::Begin("Browser", &win->browser, window_flags);

        ImGui::End();
    }

    void AddUnderLine( ImColor col_ )
        {
            ImVec2 min = ImGui::GetItemRectMin();
            ImVec2 max = ImGui::GetItemRectMax();
            min.y = max.y;
            ImGui::GetWindowDrawList()->AddLine( min, max, col_, 1.0f );
        }

        // hyperlink urls
    void TextURL( const char* name_, const char* URL_, uint8_t SameLineBefore_, uint8_t SameLineAfter_ )
        {
            if( 1 == SameLineBefore_ ){ ImGui::SameLine( 0.0f, ImGui::GetStyle().ItemInnerSpacing.x ); }
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
            ImGui::Text( name_ );
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
            {
                if( ImGui::IsMouseClicked(0) )
                {
                    std::system( (string("start ") + URL_).c_str() );
                }
                AddUnderLine( ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered] );
                ImGui::SetTooltip( "Open in browser\n%s", URL_ );
            }
            else
            {
                AddUnderLine( ImGui::GetStyle().Colors[ImGuiCol_Button] );
            }
            if( 1 == SameLineAfter_ ){ ImGui::SameLine( 0.0f, ImGui::GetStyle().ItemInnerSpacing.x ); }
    }

    void PluginMGRWindow()
    {
        if(!win->plugmgr)
            return;

        if (ImGui::Begin("Plugins", &win->plugmgr))
        {
            ImGui::TextWrapped("To install plugin put it at `modules` directory.");
            ImGui::Columns(2);
            ImGui::ListBox("", &selectedPluginIndex, PlugTitleGetter, static_cast<void*>(&modules), modules.size());
            if(selectedPluginIndex >= 0)
                if(selectedPlugin != modules.at(selectedPluginIndex))
                    selectedPlugin = modules.at(selectedPluginIndex);

            ImGui::NextColumn();
            if (ImGui::CollapsingHeader("PluginDetailes"))
            {
                if (selectedPlugin)
                {
                    ImGui::Text(selectedPlugin->title);
                    ImGui::Text(selectedPlugin->author);
                    ImGui::Text(selectedPlugin->version);
                    TextURL(selectedPlugin->site, selectedPlugin->site, 0, 0);
                    ImGui::TextWrapped(selectedPlugin->description);

                    if(selectedPlugin.get()->HasSettings())
                    {
                        if (ImGui::Button("Settings"))
                        {
                            printf("Opening settings... [%s]\n", selectedPlugin.get()->title);
                            selectedPlugin->Settings();
                        }
                    }
                    else{
                        ImGui::Text("No settings");
                    }
                    ImGui::SameLine();
                    if (!selectedPlugin.get()->stopped)
                    {
                        if (ImGui::Button("Shutdown"))
                        {
                            printf("Shutting down... [%s]\n", selectedPlugin.get()->title);
                            selectedPlugin->Shutdown();
                        }
                    }
                    else
                    {
                        if (ImGui::Button("Run"))
                        {
                            printf("Starting... [%s]\n", selectedPlugin.get()->title);
                            selectedPlugin->Run(AppInstance::GetSingleton());
                        }
                    }
                }
            }
            ImGui::End();
        }
    }

    void Draw(){
        preDraw();

        if(freezeWindows)
            window_flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
        else
            window_flags &= false;

//        cout << "[editorui]\t\t" << "Draw" << endl;
        mainMenu();

        for(auto tup: *AppInstance::GetSingleton()->editorWindows){
            tup.second();
        }
//        if(win->about)
//            winAbout();
//        if(win->hierarchy)
//            winHierarchy();
//        if(win->browser)
//            winBrowser();
//        if(win->console)
//            winConsole();
//        if(win->render)
//            winRender();
//        if(win->inspector)
//            winInspector();
//        if(win->plugmgr)
//            PluginMGRWindow();

//        cout << "[editorui]\t\t" << "End Draw" << endl;

        postDraw();
    }

    void Reshape(int w, int h){
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)w, (float)h);
    }

    void MouseWheel(int button, int dir, int x, int y){
        ImGuiIO& io = ImGui::GetIO();
            io.MousePos = ImVec2((float)x, (float)y);
            if (dir > 0)
                io.MouseWheel += 1.0;
            else if (dir < 0)
                io.MouseWheel -= 1.0;
        (void)button; // Unused

        editorCam->transform->position += editorCam->transform->direction() * dir * 3;
    }

    void motion(int x, int y){
        ImGuiIO& io = ImGui::GetIO();
        io.MousePos = ImVec2((float)x, (float)y);

        if(!editorCam->freeMode)
            return;

        static bool just_warped = false;

        if(just_warped) {
            just_warped = false;
            return;
        }

        if(moveKey) {
            const double limit = 89.0 * M_PI / 180.0;

//            glutWarpPointer(renderer->width/2, renderer->height/2);

            if(prevX == 0 && prevY == 0)
            {
                prevX = x;
                prevY = y;
            }

            int dx = x - prevX,
                    dy = y - prevY;


            //cout << "[editorui]\t\t" << "CAM ROT [ " << transform->rotation.toStringA() << " ]" << endl;
            prevX = x;
            prevY = y;
            editorCam->transform->rotation.x += ((editorCam->invertMouse) ? 1 : -1) * dy * g_rotation_speed;// > 0 ? 0.5 : -0.5;
            editorCam->transform->rotation.y += dx * g_rotation_speed;// > 0 ? 0.5 : -0.5;
            if(editorCam->transform->rotation.x < -limit)
                editorCam->transform->rotation.x = -limit;

            if(editorCam->transform->rotation.x > limit)
                editorCam->transform->rotation.x = limit;
            just_warped = true;
        }


        if(crosshair)
            glutSetCursor(GLUT_CURSOR_FULL_CROSSHAIR);
        else
            glutSetCursor(GLUT_CURSOR_LEFT_ARROW);
    }

    void mouse(int glut_button, int state, int x, int y)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.MousePos = ImVec2((float)x, (float)y);
        int button = -1;
        if (glut_button == GLUT_LEFT_BUTTON) button = 0;
        if (glut_button == GLUT_RIGHT_BUTTON) button = 1;
        if (glut_button == GLUT_MIDDLE_BUTTON) button = 2;
        if (button != -1 && state == GLUT_DOWN)
            io.MouseDown[button] = true;
        if (button != -1 && state == GLUT_UP)
            io.MouseDown[button] = false;

        prevX = x;
        prevY = y;
        if(glut_button == GLUT_RIGHT_BUTTON)
        {
            if(state == GLUT_DOWN)
            {
                moveKey = true;
                crosshair = true;
            }
            if(state == GLUT_UP){
                moveKey = false;
                crosshair = false;
            }
        }
    }


    static void ImGui_ImplFreeGLUT_UpdateKeyboardMods()
    {
        ImGuiIO& io = ImGui::GetIO();
        int mods = glutGetModifiers();
        io.KeyCtrl = (mods & GLUT_ACTIVE_CTRL) != 0;
        io.KeyShift = (mods & GLUT_ACTIVE_SHIFT) != 0;
        io.KeyAlt = (mods & GLUT_ACTIVE_ALT) != 0;
    }

    void keyboard(unsigned char c, int x, int y)
    {
        // Send character to imgui
        //printf("char_down_func %d '%c'\n", c, c);
        ImGuiIO& io = ImGui::GetIO();
        if (c >= 32)
            io.AddInputCharacter(c);

        // Store letters in KeysDown[] array as both uppercase and lowercase + Handle GLUT translating CTRL+A..CTRL+Z as 1..26.
        // This is a hacky mess but GLUT is unable to distinguish e.g. a TAB key from CTRL+I so this is probably the best we can do here.
        if (c >= 1 && c <= 26)
            io.KeysDown[c] = io.KeysDown[c - 1 + 'a'] = io.KeysDown[c - 1 + 'A'] = true;
        else if (c >= 'a' && c <= 'z')
            io.KeysDown[c] = io.KeysDown[c - 'a' + 'A'] = true;
        else if (c >= 'A' && c <= 'Z')
            io.KeysDown[c] = io.KeysDown[c - 'A' + 'a'] = true;
        else
            io.KeysDown[c] = true;
        ImGui_ImplFreeGLUT_UpdateKeyboardMods();
        (void)x; (void)y; // Unused
    }

    void KeyboardUpFunc(unsigned char c, int x, int y)
    {
        //printf("char_up_func %d '%c'\n", c, c);
        ImGuiIO& io = ImGui::GetIO();
        if (c >= 1 && c <= 26)
            io.KeysDown[c] = io.KeysDown[c - 1 + 'a'] = io.KeysDown[c - 1 + 'A'] = false;
        else if (c >= 'a' && c <= 'z')
            io.KeysDown[c] = io.KeysDown[c - 'a' + 'A'] = false;
        else if (c >= 'A' && c <= 'Z')
            io.KeysDown[c] = io.KeysDown[c - 'A' + 'a'] = false;
        else
            io.KeysDown[c] = false;
        ImGui_ImplFreeGLUT_UpdateKeyboardMods();
        (void)x; (void)y; // Unused
    }

    void SpecialFunc(int key, int x, int y)
    {
        //printf("key_down_func %d\n", key);
        ImGuiIO& io = ImGui::GetIO();
        if (key + 256 < IM_ARRAYSIZE(io.KeysDown))
            io.KeysDown[key + 256] = true;
        ImGui_ImplFreeGLUT_UpdateKeyboardMods();
        (void)x; (void)y; // Unused
    }

    void SpecialUpFunc(int key, int x, int y)
    {
        //printf("key_up_func %d\n", key);
        ImGuiIO& io = ImGui::GetIO();
        if (key + 256 < IM_ARRAYSIZE(io.KeysDown))
            io.KeysDown[key + 256] = false;
        ImGui_ImplFreeGLUT_UpdateKeyboardMods();
        (void)x; (void)y; // Unused
    }

};

void editorinit(){
    EditorUI::getSingleton()->SetUp();
}
void editorkeyboard(unsigned char c, int x, int y){
    EditorUI::getSingleton()->keyboard(c,x,y);
}
void editorkeyaboardUp(unsigned char c, int x, int y){
    EditorUI::getSingleton()->KeyboardUpFunc(c,x,y);
}
void editorspecial(int key, int x, int y){
    EditorUI::getSingleton()->SpecialFunc(key, x, y);
}
void editorspecialUp(int key, int x, int y){
    EditorUI::getSingleton()->SpecialUpFunc(key, x, y);
}
void editormouse(int button, int state, int x, int y){
    EditorUI::getSingleton()->mouse(button, state, x, y);
}
void editormousewheel(int button, int dir, int x, int y){
    EditorUI::getSingleton()->MouseWheel(button, dir, x, y);
}
void editormove(int x, int y){
    EditorUI::getSingleton()->motion(x, y);
}
void editorpmove(int x, int y){
    EditorUI::getSingleton()->motion(x, y);
}
void editorreshape(int w, int h){
    EditorUI::getSingleton()->Reshape(w, h);
}

void editorDraw(){
    EditorUI::getSingleton()->Draw();
}
#endif // EDITORUI_H
