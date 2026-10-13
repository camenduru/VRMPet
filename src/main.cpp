// ---------------------------------------------------------------------------
//  VRMPet - prototype capture tool
//
//  One question, one answer: when a VRM avatar is composited over the Windows
//  desktop using a stencil-derived alpha mask, do the alphaMode=BLEND facial
//  materials (eyelashes, brows, eye highlights) survive - or does the mask
//  clip them off, because blended geometry never writes the stencil buffer?
//
//  Writes into the output directory:
//    engine.log        the engine's own backlog, so failures are visible
//    color.png         the raw render result, no alpha (sanity: did it draw?)
//    mask_default.png  alpha where stencil != EMPTY (background/sky removed)
//    mask_user.png     alpha where the character's user stencil ref matches
//
//  Uses only public WickedEngine API. Nothing in the engine is modified.
//
//  usage: VRMPet.exe [out-dir] [model.vrm] [azimuth-deg] [focus-mesh] [isolate-mesh] [msaa]
// ---------------------------------------------------------------------------

#include "WickedEngine.h"

#include <shellapi.h>

#include <cmath>
#include <cstdint>
#include <string>

// Declared in WickedEngine/Editor/ModelImporter.h. The engine library exports no
// model importer - wi::scene::LoadModel() only understands the .wiscene Archive
// format - so the editor's glTF/VRM importer is compiled into this target
// instead (see CMakeLists.txt).
void ImportModel_GLTF(const std::string& fileName, wi::scene::Scene& scene);

namespace
{
    constexpr int     kWindowSize     = 1024;
    constexpr int     kFrameFrames    = 8;   // frames to let scene bounds settle
    constexpr int     kSettleFrames   = 30;  // frames to render before capturing
    constexpr uint8_t kUserStencilRef = 0x01;

    // The avatar is ~1.6 units tall, so a 0.05/100.0 reverse-Z range is ample and
    // keeps depth precision high. WickedEngine's CreatePerspective() swaps the two
    // internally, so pass them in the ordinary near/far order here.
    constexpr float   kNearPlane      = 0.05f;
    constexpr float   kFarPlane       = 100.0f;

    std::string g_outDir     = ".";
    std::string g_modelPath  = "C:/Users/PC/Documents/fish.audio/web/vrm/AvatarSample_B.vrm";
    float       g_azimuthDeg = 0.0f;
    int         g_focusMesh  = -1;   // -1: frame the whole body, else one mesh index
    int         g_isolateMesh = -1;  // -1: show everything, else hide all but this mesh
    int         g_msaa        = 1;   // MSAA sample count for the main render pass

    // Sampled at process entry, before any engine initialisation, and reported
    // later once a log file exists. Distinguishes "zero from the start" (static
    // initialisation never ran) from "zeroed by engine init".
    float g_bootNear = -1.0f;
    float g_bootFar  = -1.0f;
    float g_bootFov  = -1.0f;

    std::string Narrow(const wchar_t* wide)
    {
        if (wide == nullptr)
            return {};
        const int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 1)
            return {};
        std::string out(size_t(len - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
        return out;
    }

    std::string Fmt(float v)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.4f", v);
        return buf;
    }
}

class PetApp : public wi::Application
{
public:
    enum class Stage
    {
        WaitForInit,
        Framing,
        Settling,
        CaptureUserMask,
        HideForBackground,
        CaptureBackground,
        Finished,
    };

    wi::RenderPath3D* path  = nullptr;
    Stage             stage = Stage::WaitForInit;
    int               counter = 0;

    void Initialize() override
    {
        wi::Application::Initialize();

        // Route the engine's own log to disk. Without this a standalone GUI app
        // has no visible output at all and failures are silent.
        wi::backlog::SetLogFile(g_outDir + "/engine.log");
        wi::backlog::SetLogLevel(wi::backlog::LogLevel::Default);
        wi::backlog::post("VRMPet: === initialize ===");
        wi::backlog::post("VRMPet: shader binary path = " + wi::renderer::GetShaderPath());
        wi::backlog::post("VRMPet: shader source path = " + wi::renderer::GetShaderSourcePath());
        wi::backlog::Flush();

        // The projection on a bare RenderPath3D turned out to be all-NaN, so record
        // the global camera's state on both sides of render-path construction to
        // pin down where zNearP/zFarP stop being their documented defaults.
        //
        // Also construct one on the stack: wiScene_Components.h declares
        //   float zNearP = 0.1f;  float zFarP = 5000.0f;  float fov = XM_PI / 3.0f;
        // If the stack copy gets those values but the static does not, the header
        // is fine and the static is the problem - which points at static
        // initialisation, not at the header this translation unit compiled against.
        //
        // Value-initialised, not default-initialised: CameraComponent has members
        // with no default member initializer (View, Projection, frustum, ...), so
        // it is not const-default-constructible and a bare `const T x;` is
        // ill-formed (MSVC C2737). `{}` is well-defined either way, and the
        // resulting values still distinguish "default member initializers ran"
        // from "they did not".
        {
            const wi::scene::CameraComponent local{};
            wi::backlog::post("VRMPet: probe local ctor near=" + Fmt(local.zNearP)
                + " far=" + Fmt(local.zFarP)
                + " fov=" + Fmt(local.fov)
                + " sizeof=" + std::to_string(sizeof(wi::scene::CameraComponent)));
        }

        wi::backlog::post("VRMPet: probe boot (sampled at wWinMain entry, pre-engine) near="
            + Fmt(g_bootNear) + " far=" + Fmt(g_bootFar) + " fov=" + Fmt(g_bootFov));

        wi::backlog::post("VRMPet: probe pre-path global cam near="
            + Fmt(wi::scene::GetCamera().zNearP)
            + " far=" + Fmt(wi::scene::GetCamera().zFarP)
            + " fov=" + Fmt(wi::scene::GetCamera().fov)
            + " w=" + Fmt(wi::scene::GetCamera().width)
            + " h=" + Fmt(wi::scene::GetCamera().height));

        path = new wi::RenderPath3D;

        // The stencil test is evaluated per sample. With MSAA on, the screenshot
        // helper resolves a multisampled target, so a pixel the avatar only partly
        // covers comes out partially transparent instead of binary - which is what
        // makes the composite over the desktop look antialiased.
        if (g_msaa > 1)
        {
            path->setMSAASampleCount(uint32_t(g_msaa));
        }

        ActivatePath(path, 0.0f);

        wi::backlog::post("VRMPet: probe post-path path cam near=" + Fmt(path->camera->zNearP)
            + " far=" + Fmt(path->camera->zFarP)
            + " fov=" + Fmt(path->camera->fov)
            + " same_object=" + std::to_string(path->camera == &wi::scene::GetCamera() ? 1 : 0)
            + " msaa=" + std::to_string(path->getMSAASampleCount()));
        wi::backlog::Flush();
    }

    void Update(float dt) override
    {
        wi::Application::Update(dt);

        switch (stage)
        {
        case Stage::WaitForInit:
            if (wi::initializer::IsInitializeFinished())
            {
                LoadModel();
                stage = Stage::Framing;
            }
            break;

        case Stage::Framing:
            // Scene::Update computes scene.bounds; give it a few frames.
            if (++counter >= kFrameFrames)
            {
                FrameCamera();
                counter = 0;
                stage = Stage::Settling;
            }
            break;

        case Stage::Settling:
            if (++counter >= kSettleFrames)
            {
                DumpSceneState();
                // No-stencil control: STENCILMODE_ALWAYS lets every pixel pass, so
                // the result is the raw post-process output. If this is empty too,
                // the frame itself is blank and the mask logic is innocent.
                SaveMask("mask_nostencil.png", wi::enums::STENCILREF_EMPTY,
                         wi::image::STENCILMODE_ALWAYS, wi::image::STENCILREFMODE_ALL);
                SaveColor();
                SaveMask("mask_default.png");
                TagCharacterWithStencil();
                // The scene render for this frame already used the old stencil
                // refs, so the tagged mask can only be captured next frame.
                stage = Stage::CaptureUserMask;
            }
            break;

        case Stage::CaptureUserMask:
            SaveMask("mask_user.png", kUserStencilRef,
                     wi::image::STENCILMODE_EQUAL, wi::image::STENCILREFMODE_USER);
            HideAllObjects();
            counter = 0;
            stage = Stage::HideForBackground;
            break;

        case Stage::HideForBackground:
            // The screenshot helper re-draws the PREVIOUS frame's postprocess
            // result, so the objects must be hidden for a whole frame before the
            // background can be read back. Two frames of margin costs nothing.
            if (++counter >= 2)
            {
                counter = 0;
                stage = Stage::CaptureBackground;
            }
            break;

        case Stage::CaptureBackground:
            // Same call as mask_nostencil.png - STENCILMODE_ALWAYS lets every
            // pixel through - but with no geometry in the scene. This is the pure
            // background, produced through an identical code path, so it can be
            // differenced against mask_nostencil.png to recover the avatar's true
            // screen-space footprint, independent of the stencil buffer.
            SaveMask("background.png", wi::enums::STENCILREF_EMPTY,
                     wi::image::STENCILMODE_ALWAYS, wi::image::STENCILREFMODE_ALL);
            stage = Stage::Finished;
            wi::backlog::post("VRMPet: === done ===");
            wi::backlog::Flush();
            PostQuitMessage(0);
            break;

        case Stage::Finished:
            break;
        }
    }

private:
    void LoadModel()
    {
        wi::scene::Scene& scene = wi::scene::GetScene();

        wi::backlog::post("VRMPet: loading " + g_modelPath);
        ImportModel_GLTF(g_modelPath, scene);
        wi::backlog::post("VRMPet: import returned");

        wi::backlog::post("VRMPet: objects=" + std::to_string(scene.objects.GetCount())
            + " meshes="    + std::to_string(scene.meshes.GetCount())
            + " materials=" + std::to_string(scene.materials.GetCount())
            + " humanoids=" + std::to_string(scene.humanoids.GetCount())
            + " springs="   + std::to_string(scene.springs.GetCount()));

        for (size_t i = 0; i < scene.meshes.GetCount(); ++i)
        {
            const wi::scene::MeshComponent& mesh = scene.meshes[i];
            const wi::primitive::AABB& b = mesh.aabb;
            wi::backlog::post("VRMPet:   mesh[" + std::to_string(i) + "] subsets="
                + std::to_string(mesh.subsets.size())
                + " aabb=" + Fmt(b.getMin().x) + "," + Fmt(b.getMin().y) + "," + Fmt(b.getMin().z)
                + " .. "  + Fmt(b.getMax().x) + "," + Fmt(b.getMax().y) + "," + Fmt(b.getMax().z));

            for (size_t j = 0; j < mesh.subsets.size(); ++j)
            {
                const wi::scene::MeshComponent::MeshSubset& sub = mesh.subsets[j];
                std::string mname;
                if (size_t(sub.materialIndex) < scene.materials.GetCount())
                {
                    const wi::scene::NameComponent* nc =
                        scene.names.GetComponent(scene.materials.GetEntity(sub.materialIndex));
                    if (nc != nullptr)
                        mname = nc->name;
                }
                wi::backlog::post("VRMPet:      subset[" + std::to_string(j) + "] matIndex="
                    + std::to_string(sub.materialIndex)
                    + " indices=" + std::to_string(sub.indexCount)
                    + " mat='" + mname + "'");
            }
        }

        // Which materials are alphaMode=BLEND is the crux of this prototype: the
        // importer maps glTF "BLEND" onto BLENDMODE_ALPHA, and "MASK" onto a
        // reduced alphaRef (0.5, or 1 - alphaCutoff). MaterialComponent carries no
        // name of its own - it lives in a NameComponent on the material's entity.
        for (size_t i = 0; i < scene.materials.GetCount(); ++i)
        {
            const wi::scene::MaterialComponent& mat = scene.materials[i];
            std::string mname;
            if (const wi::scene::NameComponent* nc =
                    scene.names.GetComponent(scene.materials.GetEntity(i)))
            {
                mname = nc->name;
            }
            const char* kind = "OPAQUE";
            if (mat.userBlendMode == wi::enums::BLENDMODE_ALPHA)
                kind = "BLEND";
            else if (mat.alphaRef < 1.0f)
                kind = "MASK";
            wi::backlog::post("VRMPet:   mat[" + std::to_string(i) + "] '" + mname
                + "' " + kind
                + " blendMode=" + std::to_string((int)mat.userBlendMode)
                + " alphaRef=" + Fmt(mat.alphaRef)
                + " baseColorW=" + Fmt(mat.baseColor.w));
        }

        scene.Entity_CreateLight("key",
            XMFLOAT3(1.6f, 2.2f, -2.0f), XMFLOAT3(1.0f, 1.0f, 1.0f),
            6.0f, 30.0f, wi::scene::LightComponent::POINT);
        scene.Entity_CreateLight("fill",
            XMFLOAT3(-2.2f, 1.4f, -1.6f), XMFLOAT3(0.70f, 0.78f, 1.0f),
            3.0f, 30.0f, wi::scene::LightComponent::POINT);

        // Control object: an unskinned primitive with a default material, parked
        // beside the avatar's head. If the probe shows up in the stencil mask but
        // the avatar does not, the render path is fine and the fault is specific
        // to the imported skinned meshes.
        const wi::ecs::Entity probe = scene.Entity_CreateSphere("stencil_probe", 0.12f);
        if (wi::scene::TransformComponent* tr = scene.transforms.GetComponent(probe))
        {
            tr->translation_local = XMFLOAT3(0.75f, 1.25f, 0.25f);
            tr->SetDirty();
        }

        // Isolation mode. mesh[0] is the face, and its 8 subsets are exactly the 8
        // face materials - including all five alphaMode=BLEND ones (EyeIris,
        // EyeHighlight, FaceBrow, FaceEyelash, FaceEyeline). Hiding the body and
        // the hair exposes them, which the default hair-fringe-covered pose does
        // not. Everything not in the chosen mesh - including the probe - is hidden
        // so the mask can be attributed to one mesh with no ambiguity.
        if (g_isolateMesh >= 0 && size_t(g_isolateMesh) < scene.meshes.GetCount())
        {
            const wi::ecs::Entity keep = scene.meshes.GetEntity(g_isolateMesh);
            size_t hidden = 0;
            for (size_t i = 0; i < scene.objects.GetCount(); ++i)
            {
                wi::scene::ObjectComponent& obj = scene.objects[i];
                if (obj.meshID != keep)
                {
                    obj.SetRenderable(false);
                    ++hidden;
                }
            }
            wi::backlog::post("VRMPet: isolate mesh " + std::to_string(g_isolateMesh)
                + " - hid " + std::to_string(hidden) + " of "
                + std::to_string(scene.objects.GetCount()) + " objects");
        }

        wi::backlog::Flush();
    }

    void DumpSceneState()
    {
        wi::scene::Scene& scene = wi::scene::GetScene();
        wi::backlog::post("VRMPet: --- scene state at capture ---");

        if (const wi::graphics::Texture* pp = path->GetLastPostprocessRT())
        {
            wi::backlog::post("VRMPet: postprocessRT valid=" + std::to_string(pp->IsValid() ? 1 : 0)
                + " " + std::to_string(pp->desc.width) + "x" + std::to_string(pp->desc.height)
                + " fmt=" + std::to_string((int)pp->desc.format));
        }
        else
        {
            wi::backlog::post("VRMPet: postprocessRT is NULL");
        }

        wi::backlog::post("VRMPet: activePath=" + std::to_string(GetActivePath() != nullptr ? 1 : 0));
        wi::backlog::post("VRMPet: gameSpeed=" + Fmt(wi::renderer::GetGameSpeed()));
        wi::backlog::post("VRMPet: scene camera entities=" + std::to_string(scene.cameras.GetCount()));

        {
            const wi::scene::CameraComponent& cam = *path->camera;
            // Address identity: if path->camera is not the global GetCamera()
            // static, then the render path is rendering with a different camera
            // than the one this tool configures, which would explain a blank frame.
            wi::backlog::post("VRMPet: camaddr path=" + std::to_string((uintptr_t)&cam)
                + " global=" + std::to_string((uintptr_t)&wi::scene::GetCamera())
                + " scene=" + std::to_string((uintptr_t)&scene.camera));
            wi::backlog::post("VRMPet: cam w=" + Fmt(cam.width) + " h=" + Fmt(cam.height)
                + " near=" + Fmt(cam.zNearP) + " far=" + Fmt(cam.zFarP)
                + " fov=" + Fmt(cam.fov)
                + " VP[0]=" + Fmt(cam.VP._11) + "," + Fmt(cam.VP._22) + "," + Fmt(cam.VP._33)
                + " VP[3]=" + Fmt(cam.VP._41) + "," + Fmt(cam.VP._42) + "," + Fmt(cam.VP._43));
        }

        for (size_t i = 0; i < scene.objects.GetCount(); ++i)
        {
            const wi::scene::ObjectComponent& obj = scene.objects[i];
            const wi::ecs::Entity e = scene.objects.GetEntity(i);

            wi::backlog::post("VRMPet:   obj[" + std::to_string(i) + "] entity=" + std::to_string(e)
                + " mesh=" + std::to_string(obj.meshID)
                + " renderable=" + std::to_string(obj.IsRenderable() ? 1 : 0)
                + " transparency=" + Fmt(obj.GetTransparency())
                + " colorW=" + Fmt(obj.color.w)
                + " filterMask=" + std::to_string(obj.GetFilterMask())
                + " notVisibleMain=" + std::to_string(obj.IsNotVisibleInMainCamera() ? 1 : 0)
                + " center=" + Fmt(obj.center.x) + "," + Fmt(obj.center.y) + "," + Fmt(obj.center.z)
                + " radius=" + Fmt(obj.radius));

            if (const wi::scene::TransformComponent* tr = scene.transforms.GetComponent(e))
            {
                wi::backlog::post("VRMPet:      translation="
                    + Fmt(tr->translation_local.x) + "," + Fmt(tr->translation_local.y) + "," + Fmt(tr->translation_local.z)
                    + " scale=" + Fmt(tr->scale_local.x) + "," + Fmt(tr->scale_local.y) + "," + Fmt(tr->scale_local.z));
            }
        }
        wi::backlog::Flush();
    }

    // WickedEngine's render path points its `CameraComponent* camera` member at the
    // global wi::scene::GetCamera() static, but the render path itself only ever
    // writes camera->width/height into it (RenderPath3D::ResizeBuffers and
    // RenderPath3D::Render). Nothing in the render path calls CreatePerspective() -
    // establishing the projection is left to the host application; the Editor does
    // it from its camera window.
    //
    // A bare RenderPath3D therefore renders with zNearP == zFarP == 0. Reverse-Z
    // feeds those straight into XMMatrixPerspectiveFovLH(fov, aspect, zFarP,
    // zNearP), where NearZ and FarZ are both 0, so fRange = FarZ / (NearZ - FarZ)
    // becomes 0/0 -> NaN. The whole projection is NaN, the frustum is degenerate,
    // every object is culled, and the frame - and therefore the stencil mask -
    // comes out empty.
    //
    // So the host has to set the projection explicitly. Do it every frame so a
    // resize or a render-path reset can never leave it stale.
    void ApplyProjection()
    {
        wi::scene::CameraComponent& cam = *path->camera;

        float w = cam.width;
        float h = cam.height;
        if (!(w > 0.0f) || !std::isfinite(w)) w = float(kWindowSize);
        if (!(h > 0.0f) || !std::isfinite(h)) h = float(kWindowSize);

        float fov = cam.fov;
        if (!(fov > 0.01f) || !(fov < XM_PI) || !std::isfinite(fov)) fov = XM_PI / 3.0f;

        cam.CreatePerspective(w, h, kNearPlane, kFarPlane, fov);
    }

    void FrameCamera()
    {
        wi::scene::Scene& scene = wi::scene::GetScene();

        // scene.bounds also covers spring colliders, which inflate it well past
        // the visible body. Merge the mesh boxes instead for tight framing.
        wi::primitive::AABB box;
        if (g_focusMesh >= 0 && size_t(g_focusMesh) < scene.meshes.GetCount())
        {
            // Frame a single mesh - used to zoom in on the head, where the
            // alphaMode=BLEND facial materials live.
            box = scene.meshes[g_focusMesh].aabb;
        }
        else
        {
            for (size_t i = 0; i < scene.meshes.GetCount(); ++i)
            {
                box = wi::primitive::AABB::Merge(box, scene.meshes[i].aabb);
            }
        }
        if (!box.IsValid())
        {
            box = scene.bounds;
        }

        wi::backlog::post("VRMPet: frame box valid=" + std::to_string(box.IsValid() ? 1 : 0)
            + " min=" + Fmt(box.getMin().x) + "," + Fmt(box.getMin().y) + "," + Fmt(box.getMin().z)
            + " max=" + Fmt(box.getMax().x) + "," + Fmt(box.getMax().y) + "," + Fmt(box.getMax().z));

        wi::scene::CameraComponent& cam = *path->camera;

        XMFLOAT3 center = XMFLOAT3(0.0f, 0.8f, 0.0f);
        float radius = 1.0f;
        if (box.IsValid())
        {
            center = box.getCenter();
            radius = box.getRadius();
        }
        if (!(radius > 0.01f) || !std::isfinite(radius))
        {
            radius = 1.0f;
        }

        // Establish a known-good projection before reading cam.fov back for the
        // framing distance. See ApplyProjection() for why this is necessary.
        ApplyProjection();
        const float fov = cam.fov;

        const float rad  = g_azimuthDeg * XM_PI / 180.0f;
        const float dist = (radius / tanf(fov * 0.5f)) * 1.15f;

        const XMFLOAT3 eye = XMFLOAT3(
            center.x + sinf(rad) * dist,
            center.y,
            center.z + cosf(rad) * dist
        );

        // CameraComponent::At is a normalised FORWARD DIRECTION, not a look-at
        // point, despite the name. CameraComponent::UpdateCamera feeds it to
        // XMMatrixLookToLH(eye, direction, up), and TransformCamera stores
        // normalize(world +Z) into it. Assigning a position here aims the camera
        // at whatever that vector happens to point at - which silently culls the
        // entire scene and yields a black frame with an empty stencil mask.
        XMFLOAT3 dir = XMFLOAT3(center.x - eye.x, center.y - eye.y, center.z - eye.z);
        const float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (len > 1e-6f)
        {
            dir.x /= len;
            dir.y /= len;
            dir.z /= len;
        }

        cam.Eye = eye;
        cam.At  = dir;
        cam.Up  = XMFLOAT3(0.0f, 1.0f, 0.0f);

        // Rebuild View/Projection/VP now that Eye/At/Up are set. CreatePerspective()
        // above ran while Eye/At were still the defaults, so its UpdateCamera() call
        // produced a VP for the wrong pose.
        cam.UpdateCamera();

        wi::backlog::post("VRMPet: camera fov=" + Fmt(cam.fov)
            + " eye=" + Fmt(cam.Eye.x) + "," + Fmt(cam.Eye.y) + "," + Fmt(cam.Eye.z)
            + " dir=" + Fmt(cam.At.x) + "," + Fmt(cam.At.y) + "," + Fmt(cam.At.z)
            + " dist=" + Fmt(dist) + " radius=" + Fmt(radius));

        // Read the projection back so a bad one is visible in the log rather than
        // showing up only as an empty stencil mask.
        wi::backlog::post("VRMPet: proj w=" + Fmt(cam.width) + " h=" + Fmt(cam.height)
            + " near=" + Fmt(cam.zNearP) + " far=" + Fmt(cam.zFarP)
            + " custom=" + std::to_string(cam.IsCustomProjectionEnabled() ? 1 : 0)
            + " ortho=" + std::to_string(cam.IsOrtho() ? 1 : 0)
            + " VP[0]=" + Fmt(cam.VP._11) + "," + Fmt(cam.VP._22) + "," + Fmt(cam.VP._33)
            + " VP[3]=" + Fmt(cam.VP._41) + "," + Fmt(cam.VP._42) + "," + Fmt(cam.VP._43));
        wi::backlog::Flush();
    }

    void HideAllObjects()
    {
        wi::scene::Scene& scene = wi::scene::GetScene();
        for (size_t i = 0; i < scene.objects.GetCount(); ++i)
        {
            scene.objects[i].SetRenderable(false);
        }
        wi::backlog::post("VRMPet: hid " + std::to_string(scene.objects.GetCount())
            + " objects for the background reference");
        wi::backlog::Flush();
    }

    void TagCharacterWithStencil()
    {
        wi::scene::Scene& scene = wi::scene::GetScene();
        for (size_t i = 0; i < scene.objects.GetCount(); ++i)
        {
            scene.objects[i].SetUserStencilRef(kUserStencilRef);
        }
        wi::backlog::post("VRMPet: tagged " + std::to_string(scene.objects.GetCount())
            + " objects with user stencil ref " + std::to_string(kUserStencilRef));
        wi::backlog::Flush();
    }

    void SaveColor()
    {
        const wi::graphics::Texture& tex = path->GetRenderResult3D();
        const std::string file = g_outDir + "/color.png";
        const bool ok = wi::helper::saveTextureToFile(tex, file);
        wi::backlog::post("VRMPet: saved " + file + (ok ? " OK" : " FAILED"));
    }

    void SaveMask(const std::string& fileName,
                  uint8_t stencilRef = wi::enums::STENCILREF_EMPTY,
                  wi::image::STENCILMODE stencilMode = wi::image::STENCILMODE_NOT,
                  wi::image::STENCILREFMODE refMode = wi::image::STENCILREFMODE_ALL)
    {
        wi::graphics::Texture tex = path->CreateScreenshotWithAlphaBackground(stencilRef, stencilMode, refMode);
        const std::string file = g_outDir + "/" + fileName;
        const bool ok = wi::helper::saveTextureToFile(tex, file);
        wi::backlog::post("VRMPet: saved " + file + (ok ? " OK" : " FAILED"));
        wi::backlog::Flush();
    }
};

PetApp app;

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
    // Sampled as the very first thing the process does, before any WickedEngine
    // code runs. wi::scene::GetCamera() returns a function-local static of an
    // inline function, which in this static-library build reads as all zeros -
    // its default member initializers appear never to run. Reading it here shows
    // whether that is a static-initialisation problem or something the engine
    // zeroes during start-up.
    g_bootNear = wi::scene::GetCamera().zNearP;
    g_bootFar  = wi::scene::GetCamera().zFarP;
    g_bootFov  = wi::scene::GetCamera().fov;

    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv != nullptr)
        {
            if (argc > 1) g_outDir      = Narrow(argv[1]);
            if (argc > 2) g_modelPath   = Narrow(argv[2]);
            if (argc > 3) g_azimuthDeg  = float(_wtof(argv[3]));
            if (argc > 4) g_focusMesh   = _wtoi(argv[4]);
            if (argc > 5) g_isolateMesh = _wtoi(argv[5]);
            if (argc > 6) g_msaa        = _wtoi(argv[6]);
            LocalFree(argv);
        }
    }

    static auto WndProc = [](HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) -> LRESULT
    {
        switch (message)
        {
        case WM_SIZE:
        case WM_DPICHANGED:
            if (app.is_window_active)
                app.SetWindow(hWnd);
            break;
        case WM_INPUT:
            wi::input::rawinput::ParseMessage((void*)lParam);
            break;
        case WM_SETFOCUS:
            app.is_window_active = true;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        return 0;
    };

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wcex = {};
    wcex.cbSize        = sizeof(WNDCLASSEX);
    wcex.style         = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc   = WndProc;
    wcex.hInstance     = hInstance;
    wcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszClassName = L"VRMPetWindowClass";
    RegisterClassExW(&wcex);

    RECT rc = { 0, 0, kWindowSize, kWindowSize };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hWnd = CreateWindowW(wcex.lpszClassName, L"VRMPet - capture prototype",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);
    ShowWindow(hWnd, SW_SHOWDEFAULT);

    app.SetWindow(hWnd);

    wi::arguments::Parse(lpCmdLine);

    // A capture tool must keep rendering even when the window is not focused -
    // this process is launched from a background shell and never receives
    // WM_SETFOCUS, and Application::Run() returns early when inactive.
    app.is_window_active = true;

    MSG msg = {};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            app.is_window_active = true;
            app.Run();
        }
    }

    wi::jobsystem::ShutDown();
    return (int)msg.wParam;
}
