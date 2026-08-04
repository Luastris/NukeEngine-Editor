#pragma once
// Shared plumbing of the animation-family asset editors (defined in animeditor.cpp):
// the preview rig, the model-space->preview-rect projector for ImGui overlays, and the
// reflection-driven component/prop picker.
#include "editor/editorui.h"
#include <glm/glm.hpp>

namespace nuke { class Animator; class SkinnedMeshRenderer; class MeshRenderer; }

// Model-space point -> screen point over a PreviewWorld's last-drawn rect.
struct EditorPvProj
{
	glm::mat4 vpw;            // proj * view * rigWorld
	ImVec2 rectMin, rectSize;
	bool Project(const glm::vec3& model, ImVec2& out) const;
};
bool EditorMakeProjector(EditorUI::PreviewWorld& pv, nuke::Atom* rig, EditorPvProj& out);

// Every mesh skinned to this skeleton (a character is usually several) / the first one.
std::vector<std::string> EditorMeshesForSkeleton(const std::string& skelGuid);
std::string EditorAutoMeshForSkeleton(const std::string& skelGuid);
// Give a renderer the materials its MESH should wear: the mesh's import-time slots (.numesh
// v7), else the first prefab in the project that uses it, else the default.
void EditorApplyMeshMaterials(nuke::MeshRenderer* mr, const std::string& meshGuid);

// The rig's posed SkinnedMeshRenderer (its first part) — overlays read the pose from it.
nuke::SkinnedMeshRenderer* EditorRigSMR(nuke::Atom* rig);

// Find (or build) the preview rig: one atom holding SkinnedMeshRenderer + a muted Animator.
nuke::Animator* EditorEnsureRig(EditorUI* ui, EditorUI::PreviewWorld* pv, long& atomId,
                                const std::string& meshGuid);
nuke::Atom* EditorRigAtom(EditorUI::PreviewWorld* pv, long atomId);
void EditorDrawBoneOverlay(EditorUI::PreviewWorld& pv, nuke::Atom* rig);

// Reflected component/prop dropdowns (script modules' classes appear automatically).
int  EditorPropDim(nuke::FT t);
bool EditorPropPicker(const char* id, float compW, float propW,
                      std::string& comp, std::string& prop, int* outDim);
// Pick a DESCENDANT of `root` from a tree (empty = the root atom itself) instead of typing
// a "Muzzle/Flash" path by hand. True when the path changed.
bool EditorAtomPathPicker(const char* id, nuke::Atom* root, std::string& path, float width);

// Clip metadata snapshots for the .nuanim editor's undo.
std::string EditorAnimMetaJson(const nuke::AnimClip* c);
void        EditorAnimMetaLoad(nuke::AnimClip* c, const std::string& json);
