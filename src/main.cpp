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
//  Capture mode - writes PNGs for analysis:
//    VRMPet.exe [out-dir] [model.vrm] [azimuth-deg] [focus-mesh] [isolate-mesh] [msaa] [anim.vrma]
//
//  Overlay mode - the avatar standing on the desktop, no window, no background:
//    VRMPet.exe overlay [model.vrm] [azimuth-deg] [msaa] [seconds] [anim.vrma]
//      azimuth 180 faces the camera. seconds 0 = run until Ctrl+Shift+Q.
//
//  Both modes default to the assets/ copy of the avatar, so the tool runs with
//  no arguments at all. Overlay mode also defaults to the assets/ idle loop,
//  retargeted onto the avatar at load time; capture mode leaves the animation
//  off unless a path is given, because its mask captures are differenced across
//  frames and a moving character would make them disagree. A trailing "-" or
//  "none" disables the animation in either mode.
//
//  A bare "lookat" or "nolookat" token anywhere on the command line forces the
//  humanoid look-at solver on or off. It defaults to on in overlay mode - the
//  camera is the viewer's eye there, so the pet watches whoever is looking at
//  the screen - and off in capture mode, where the pose should be exactly the
//  one the animation produces.
// ---------------------------------------------------------------------------

#include "WickedEngine.h"

#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <climits>
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
    std::string g_modelPath  = "assets/AvatarSample_B.vrm";
    // A .vrma is not an animation *for* the avatar - it is a humanoid animation
    // on a bare 33-node skeleton of its own. It has to be retargeted onto the
    // avatar's skeleton at load time; see PetApp::ApplyAnimation(). Assigned
    // per-mode in wWinMain: the overlay defaults to the idle loop, capture mode
    // defaults to none. "-" or "none" disables it in either mode.
    std::string g_animPath;
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

    // Opt-in: re-enable the engine's humanoid look-at solver and aim it at the
    // camera every frame, so the pet watches the viewer instead of holding
    // whatever the animation does with the head. Defaults on in overlay mode and
    // off in capture mode - see the note in PetApp::LoadModel() for what the
    // solver does when nobody aims it. Overridable with a bare "lookat" or
    // "nolookat" token on the command line.
    bool g_lookAtCamera = false;

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

    std::string JoinPath(std::string dir, const std::string& rel)
    {
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
            dir += '/';
        return dir + rel;
    }

    // The default asset paths are relative - "assets/AvatarSample_B.vrm" - so the
    // project folder stays the single source of truth rather than an absolute
    // path to one machine's Documents directory. CMake stages assets/ next to the
    // executable the same way it stages dxcompiler.dll, so running from build/run
    // works; resolving against the executable as well means the tool also works
    // from the project root or from a shortcut, whatever the working directory
    // happens to be. An absolute path on the command line is always honoured
    // as-is, because FileExists() succeeds on it immediately.
    std::string ResolveAssetPath(const std::string& path)
    {
        if (path.empty() || path == "-" || path == "none")
            return path;

        if (wi::helper::FileExists(path))
            return path;

        const std::string exeDir =
            wi::helper::GetDirectoryFromPath(wi::helper::GetExecutablePath());

        const std::string candidates[] = {
            JoinPath(exeDir, path),              // build/run/assets/...
            JoinPath(exeDir, "../" + path),      // build/assets/...
            JoinPath(exeDir, "../../" + path),   // <project>/assets/...
        };
        for (const std::string& candidate : candidates)
        {
            if (wi::helper::FileExists(candidate))
            {
                wi::backlog::post("VRMPet: resolved '" + path + "' -> " + candidate);
                return candidate;
            }
        }

        wi::backlog::post("VRMPet: WARNING - '" + path + "' not found relative to '"
            + exeDir + "' or the working directory");
        return path;
    }

    // The capture tool writes its PNGs and its log into the output directory, and
    // wi::backlog::SetLogFile() does not create it. Without this a run against a
    // directory that does not exist yet does all the work and then writes nothing,
    // silently - the worst possible failure mode for a diagnostic tool.
    bool EnsureDirectory(const std::string& dir)
    {
        if (dir.empty() || dir == "." || wi::helper::DirectoryExists(dir))
            return true;

        const int len = MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0);
        if (len <= 0)
            return false;
        std::wstring wide(size_t(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, wide.data(), len);

        // Create every missing component, so "out/body" works as well as "out".
        for (size_t i = 1; i < wide.size(); ++i)
        {
            if (wide[i] == L'/' || wide[i] == L'\\')
            {
                const std::wstring partial = wide.substr(0, i);
                if (!CreateDirectoryW(partial.c_str(), nullptr) &&
                    GetLastError() != ERROR_ALREADY_EXISTS)
                {
                    return false;
                }
            }
        }
        return CreateDirectoryW(wide.c_str(), nullptr) != FALSE
            || GetLastError() == ERROR_ALREADY_EXISTS;
    }

    std::string Fmt(float v)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.4f", v);
        return buf;
    }

    // -----------------------------------------------------------------------
    // Overlay mode
    //
    // The end goal: the avatar standing on the desktop, no background, no
    // window, fully transparent. WickedEngine's own swap chain cannot carry
    // alpha - GraphicsDevice_DX12::CreateSwapChain hardcodes
    // DXGI_ALPHA_MODE_IGNORE - so the engine renders into a hidden window and
    // the masked RGBA is pushed to a separate layered window instead.
    //
    // WS_POPUP          no caption, no border, no chrome at all
    // WS_EX_LAYERED     required for UpdateLayeredWindow (per-pixel alpha)
    // WS_EX_TRANSPARENT mouse input falls through to whatever is underneath
    // WS_EX_TOOLWINDOW  keeps it out of the taskbar and Alt+Tab
    // WS_EX_NOACTIVATE  never steals focus from the app you are working in
    // WS_EX_TOPMOST     floats above other windows
    // -----------------------------------------------------------------------
    bool g_overlayMode   = false;
    int  g_overlaySecs   = 0;   // 0 = run until Ctrl+Shift+Q
    int  g_overlayMargin = 0;   // gap between the character and the work-area edge

    // Frames to render before the first blit, so spring bones and temporal
    // effects have settled and the character is not captured mid-twitch.
    constexpr int kOverlaySettleFrames = 30;

    // An idle loop sways the hips and swings the arms, so the tight alpha box
    // measured over the first few settled frames is not necessarily the box for
    // the whole loop. Pad it generously, and re-measure once a second so a
    // too-tight crop reports itself as a number in the log rather than as a
    // hand quietly losing its fingers at the window edge.
    constexpr int kOverlayPad         = 40;
    constexpr int kOverlayCheckFrames = 60;

    struct LayeredOverlay
    {
        HWND     hwnd = nullptr;
        HDC      dc   = nullptr;
        HBITMAP  bmp  = nullptr;
        uint8_t* bits = nullptr;   // BGRA, premultiplied, top-down
        int      w = 0;
        int      h = 0;
        int      x = 0;
        int      y = 0;

        bool Create(int width, int height, int posX, int posY)
        {
            w = width;
            h = height;
            x = posX;
            y = posY;

            HINSTANCE inst = GetModuleHandleW(nullptr);

            WNDCLASSEXW wc = {};
            wc.cbSize        = sizeof(wc);
            wc.lpfnWndProc   = OverlayProc;
            wc.hInstance     = inst;
            wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = L"VRMPetOverlay";
            RegisterClassExW(&wc);

            hwnd = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                wc.lpszClassName, L"VRMPet",
                WS_POPUP,
                x, y, w, h,
                nullptr, nullptr, inst, nullptr);
            if (hwnd == nullptr)
                return false;

            HDC screen = GetDC(nullptr);
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth       = w;
            bmi.bmiHeader.biHeight      = -h;   // negative = top-down, matching the readback
            bmi.bmiHeader.biPlanes      = 1;
            bmi.bmiHeader.biBitCount    = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            bmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
            if (bmp == nullptr)
            {
                ReleaseDC(nullptr, screen);
                return false;
            }
            dc = CreateCompatibleDC(screen);
            ReleaseDC(nullptr, screen);
            if (dc == nullptr)
                return false;
            SelectObject(dc, bmp);

            return true;
        }

        void Show()
        {
            if (hwnd == nullptr)
                return;
            SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }

        // Crop the avatar's box out of the engine's RGBA readback, premultiply it,
        // and write it as BGRA. UpdateLayeredWindow with AC_SRC_ALPHA requires the
        // colour channels to already be multiplied by alpha.
        void Blit(const uint8_t* rgba, int srcW, int cropX, int cropY)
        {
            if (bits == nullptr)
                return;
            for (int row = 0; row < h; ++row)
            {
                const uint8_t* src = rgba + (size_t(cropY + row) * size_t(srcW) + size_t(cropX)) * 4;
                uint8_t* dst = bits + size_t(row) * size_t(w) * 4;
                for (int col = 0; col < w; ++col)
                {
                    const uint8_t r = src[0];
                    const uint8_t g = src[1];
                    const uint8_t b = src[2];
                    const uint8_t a = src[3];
                    dst[0] = uint8_t(b * a / 255);
                    dst[1] = uint8_t(g * a / 255);
                    dst[2] = uint8_t(r * a / 255);
                    dst[3] = a;
                    src += 4;
                    dst += 4;
                }
            }
        }

        void Push()
        {
            if (hwnd == nullptr || dc == nullptr)
                return;
            POINT dstPt = { x, y };
            POINT srcPt = { 0, 0 };
            SIZE  size  = { w, h };
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            UpdateLayeredWindow(hwnd, nullptr, &dstPt, &size, dc, &srcPt, 0, &bf, ULW_ALPHA);
        }

        static LRESULT CALLBACK OverlayProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            switch (msg)
            {
            case WM_NCHITTEST:
                return HTTRANSPARENT;   // belt and braces alongside WS_EX_TRANSPARENT
            case WM_HOTKEY:
                if (wParam == 1)
                {
                    wi::backlog::post("VRMPet: overlay hotkey - quitting");
                    wi::backlog::Flush();
                    PostQuitMessage(0);
                }
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(hWnd, msg, wParam, lParam);
            }
        }
    };

    LayeredOverlay g_overlay;
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

    // Overlay mode state
    bool overlayReady   = false;
    int  overlaySettle  = 0;
    bool overlayCreated = false;
    int  cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    long long overlayFrames = 0;
    std::chrono::steady_clock::time_point overlayClock;
    int  overlayCheck = 0;      // frames since the last crop-box re-measure
    // Worst px by which the character has left the crop box. Negative means
    // margin. Starts at INT_MIN so the very first measurement always reports,
    // whether it is a warning or the reassuring "still contains it" line.
    int  overlayOverflow = INT_MIN;

    void Initialize() override
    {
        wi::Application::Initialize();

        // Route the engine's own log to disk. Without this a standalone GUI app
        // has no visible output at all and failures are silent.
        EnsureDirectory(g_outDir);
        wi::backlog::SetLogFile(g_outDir + "/engine.log");
        wi::backlog::SetLogLevel(wi::backlog::LogLevel::Default);
        wi::backlog::post("VRMPet: === initialize ===");
        wi::backlog::post("VRMPet: shader binary path = " + wi::renderer::GetShaderPath());
        wi::backlog::post("VRMPet: shader source path = " + wi::renderer::GetShaderSourcePath());
        wi::backlog::Flush();

        // Relative asset paths resolve against the working directory first, then
        // against the executable. Done here rather than in wWinMain because the
        // log file only exists from this point on, and a path that failed to
        // resolve is exactly the thing that needs to be in the log.
        g_modelPath = ResolveAssetPath(g_modelPath);
        g_animPath  = ResolveAssetPath(g_animPath);
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

        // In overlay mode every frame ends with a readback and a blit to the
        // layered window. Run() invokes this right after Present, with the GPU
        // idle and no render pass open - the only safe point to touch the
        // textures we capture from.
        onFrameComplete = [this]() { OnFrameComplete(); };

        wi::backlog::post("VRMPet: probe post-path path cam near=" + Fmt(path->camera->zNearP)
            + " far=" + Fmt(path->camera->zFarP)
            + " fov=" + Fmt(path->camera->fov)
            + " same_object=" + std::to_string(path->camera == &wi::scene::GetCamera() ? 1 : 0)
            + " msaa=" + std::to_string(path->getMSAASampleCount()));
        wi::backlog::Flush();
    }

    void Update(float dt) override
    {
        // Must run before the base Update() - that is what steps the scene, and
        // the humanoid solver reads humanoid.lookAt while it does.
        AimLookAt();

        wi::Application::Update(dt);

        if (g_overlayMode)
        {
            UpdateOverlay();
            return;
        }

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
    // -----------------------------------------------------------------------
    // Look-at
    //
    // HumanoidComponent::lookAt is a world-space point, and the solver turns the
    // head and both eyes toward it, clamped by head_rotation_max (60 deg yaw,
    // 30 deg pitch) and eye_rotation_max, and eased in by head_rotation_speed so
    // it never snaps. Pointing it at the camera is what makes the pet watch
    // whoever is looking at the screen.
    // -----------------------------------------------------------------------
    void AimLookAt()
    {
        if (!g_lookAtCamera)
            return;

        wi::scene::Scene& scene = wi::scene::GetScene();
        if (scene.humanoids.GetCount() == 0 || path == nullptr || path->camera == nullptr)
            return;

        scene.humanoids[0].lookAt = path->camera->Eye;
    }

    // -----------------------------------------------------------------------
    // Overlay mode
    // -----------------------------------------------------------------------
    void UpdateOverlay()
    {
        if (!overlayReady)
        {
            if (!wi::initializer::IsInitializeFinished())
                return;
            LoadModel();
            FrameCamera();
            overlayReady = true;
            return;
        }

        // Nothing to do here - the capture happens in OnFrameComplete(), after
        // the frame has been rendered and presented. This only counts frames.
        ++overlaySettle;
    }

    void OnFrameComplete()
    {
        if (!g_overlayMode || !overlayReady || overlaySettle < kOverlaySettleFrames)
            return;
        CaptureAndBlit();
    }

    // Tight box around everything with non-zero alpha, in render-target pixels.
    // Returns false when nothing was rasterised (a fully transparent frame).
    static bool TightBox(const uint8_t* rgba, int w, int h,
                         int& outX, int& outY, int& outW, int& outH)
    {
        int minX = w, minY = h, maxX = -1, maxY = -1;
        for (int y = 0; y < h; ++y)
        {
            const uint8_t* row = rgba + size_t(y) * size_t(w) * 4;
            for (int x = 0; x < w; ++x)
            {
                if (row[size_t(x) * 4 + 3] != 0)
                {
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                }
            }
        }
        if (maxX < minX || maxY < minY)
            return false;

        outX = minX;
        outY = minY;
        outW = maxX - minX + 1;
        outH = maxY - minY + 1;
        return true;
    }

    // The box the overlay window covers. Used once, to size the window to the
    // character rather than to the whole 1024x1024 frame, then padded so the
    // animation has room to move inside it.
    bool ComputeCropBox(const uint8_t* rgba, int w, int h)
    {
        int x, y, bw, bh;
        if (!TightBox(rgba, w, h, x, y, bw, bh))
            return false;

        const int left   = std::max(0, x - kOverlayPad);
        const int top    = std::max(0, y - kOverlayPad);
        const int right  = std::min(w, x + bw + kOverlayPad);
        const int bottom = std::min(h, y + bh + kOverlayPad);

        cropX = left;
        cropY = top;
        cropW = right - left;
        cropH = bottom - top;
        return true;
    }

    // Re-measure the character's footprint and compare it with the window we
    // already sized. An idle loop should stay well inside the padded box; if it
    // ever does not, that is a real clipping bug and it needs to be visible.
    void CheckCropBox(const uint8_t* rgba, int w, int h)
    {
        int x, y, bw, bh;
        if (!TightBox(rgba, w, h, x, y, bw, bh))
            return;

        const int overL = cropX - x;
        const int overT = cropY - y;
        const int overR = (x + bw) - (cropX + cropW);
        const int overB = (y + bh) - (cropY + cropH);
        const int worst = std::max(std::max(overL, overT), std::max(overR, overB));

        if (worst > overlayOverflow)
        {
            overlayOverflow = worst;
            char buf[200];
            if (worst > 0)
            {
                snprintf(buf, sizeof(buf),
                    "VRMPet: WARNING - character leaves the crop box by %d px "
                    "(box %dx%d at %d,%d, tight %dx%d at %d,%d) - raise kOverlayPad",
                    worst, cropW, cropH, cropX, cropY, bw, bh, x, y);
            }
            else
            {
                snprintf(buf, sizeof(buf),
                    "VRMPet: crop box %dx%d at %d,%d still contains the character "
                    "(%d px of margin)",
                    cropW, cropH, cropX, cropY, -worst);
            }
            wi::backlog::post(buf);
            wi::backlog::Flush();
        }
    }

    void CaptureAndBlit()
    {
        // Same call the analysis tool uses: STENCILMODE_NOT against stencil 0
        // keeps every pixel the scene rasterised and drops the cleared sky.
        wi::graphics::Texture tex = path->CreateScreenshotWithAlphaBackground(
            wi::enums::STENCILREF_EMPTY, wi::image::STENCILMODE_NOT, wi::image::STENCILREFMODE_ALL);

        wi::vector<uint8_t> rgba;
        if (!wi::helper::saveTextureToMemory(tex, rgba))
        {
            wi::backlog::post("VRMPet: overlay readback FAILED");
            wi::backlog::Flush();
            return;
        }

        const int w = (int)tex.desc.width;
        const int h = (int)tex.desc.height;
        if (w <= 0 || h <= 0 || rgba.size() < size_t(w) * size_t(h) * 4)
            return;

        if (!overlayCreated)
        {
            if (!ComputeCropBox(rgba.data(), w, h))
                return;   // nothing rendered yet; try again next frame

            RECT wa = {};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            const int px = wa.right - cropW - g_overlayMargin;
            const int py = wa.bottom - cropH - g_overlayMargin;

            if (!g_overlay.Create(cropW, cropH, px, py))
            {
                wi::backlog::post("VRMPet: overlay window creation FAILED");
                wi::backlog::Flush();
                return;
            }
            g_overlay.Show();

            wi::backlog::post("VRMPet: overlay window " + std::to_string(cropW) + "x"
                + std::to_string(cropH) + " at " + std::to_string(px) + ","
                + std::to_string(py) + "  (work area "
                + std::to_string(wa.right - wa.left) + "x" + std::to_string(wa.bottom - wa.top)
                + ", crop from " + std::to_string(cropX) + "," + std::to_string(cropY)
                + ", " + std::to_string(kOverlayPad) + " px pad for the animation)");
            wi::backlog::Flush();

            overlayCreated = true;
            overlayClock = std::chrono::steady_clock::now();
        }

        g_overlay.Blit(rgba.data(), w, cropX, cropY);
        g_overlay.Push();

        // The window is sized once and never resized, so re-measure occasionally
        // and complain if the animation has walked the character out of it.
        if (++overlayCheck >= kOverlayCheckFrames)
        {
            overlayCheck = 0;
            CheckCropBox(rgba.data(), w, h);
        }

        ++overlayFrames;
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - overlayClock).count();
        if (elapsed >= 5.0)
        {
            char buf[160];
            snprintf(buf, sizeof(buf), "VRMPet: overlay %.1f fps over %.0fs (%lld frames)",
                     double(overlayFrames) / elapsed, elapsed, overlayFrames);
            wi::backlog::post(buf);
            wi::backlog::Flush();
            overlayFrames = 0;
            overlayClock = std::chrono::steady_clock::now();
        }
    }

    // -----------------------------------------------------------------------
    // Animation
    //
    // idle_loop.vrma is a VRM Animation: 33 nodes, no meshes, 28 channels over
    // 22 humanoid bones. It is not an animation *for* this avatar - it carries a
    // skeleton of its own, in its own units, with its own rest pose. Playing it
    // directly would animate nodes nobody can see.
    //
    // Scene::RetargetAnimation() is the engine's own answer to that, and it is
    // the same call the Editor makes. For every source channel whose target is a
    // humanoid bone it matches the bone by HumanoidComponent::bones[] index -
    // the VRM spec's own bone identity, so nothing is matched by name - and
    // records the rest-pose offset between the two skeletons. At runtime each
    // keyframe is pushed through that offset. That is what lets a 90 cm-tall,
    // 0.01-scaled centimetre skeleton drive a metre-scale avatar without the
    // avatar turning into a giant or collapsing to the origin.
    //
    // The VRMA goes into a throwaway Scene, for three reasons: its own
    // HumanoidComponent would otherwise be a second humanoid in the main scene,
    // indistinguishable from the avatar's; its 33 nodes and 0.01 root scale
    // would pollute the scene the stencil mask is computed from; and
    // bake_data=true copies the retargeted keyframes into the main scene, so the
    // scratch scene can be destroyed the moment this returns.
    // -----------------------------------------------------------------------
    void ApplyAnimation()
    {
        wi::scene::Scene& scene = wi::scene::GetScene();

        if (g_animPath.empty() || g_animPath == "-" || g_animPath == "none")
        {
            wi::backlog::post("VRMPet: animation disabled");
            return;
        }

        if (scene.humanoids.GetCount() == 0)
        {
            wi::backlog::post("VRMPet: " + g_modelPath
                + " imported no humanoid - cannot retarget " + g_animPath);
            return;
        }
        const wi::ecs::Entity avatarRoot = scene.humanoids.GetEntity(0);

        wi::scene::Scene animScene;
        wi::backlog::post("VRMPet: loading animation " + g_animPath);
        ImportModel_GLTF(g_animPath, animScene);
        wi::backlog::post("VRMPet: animation import returned  nodes="
            + std::to_string(animScene.transforms.GetCount())
            + " humanoids=" + std::to_string(animScene.humanoids.GetCount())
            + " animations=" + std::to_string(animScene.animations.GetCount()));

        if (animScene.humanoids.GetCount() == 0 || animScene.animations.GetCount() == 0)
        {
            wi::backlog::post("VRMPet: " + g_animPath
                + " has no humanoid or no animation - nothing to retarget");
            return;
        }

        constexpr size_t kBoneCount = size_t(wi::scene::HumanoidComponent::HumanoidBone::Count);
        const wi::scene::HumanoidComponent& srcHumanoid = animScene.humanoids[0];
        const wi::scene::HumanoidComponent& dstHumanoid = scene.humanoids[0];

        size_t commonBones = 0;
        for (size_t i = 0; i < kBoneCount; ++i)
        {
            if (srcHumanoid.bones[i] != wi::ecs::INVALID_ENTITY &&
                dstHumanoid.bones[i] != wi::ecs::INVALID_ENTITY)
            {
                ++commonBones;
            }
        }

        const wi::ecs::Entity animSource = animScene.animations.GetEntity(0);
        const wi::scene::AnimationComponent& srcAnim = animScene.animations[0];

        std::string animName;
        if (const wi::scene::NameComponent* nc = animScene.names.GetComponent(animSource))
            animName = nc->name;

        // A channel whose target is not one of the VRMA's 22 humanoid bones has
        // no bone identity to match on and cannot be retargeted. This VRMA is
        // authored on a 33-node skeleton, so the extra spine/neck segments and
        // the head node itself fall out here - three-vrm's VRMAnimationLoader
        // drops exactly the same channels. Listing them makes the consequence
        // explicit: the head does not move, because the file maps `head` to a
        // leaf end-site rather than to the animated head node.
        size_t dropped = 0;
        std::string droppedNames;
        for (const wi::scene::AnimationComponent::AnimationChannel& ch : srcAnim.channels)
        {
            bool isHumanoidBone = false;
            for (size_t i = 0; i < kBoneCount && !isHumanoidBone; ++i)
                isHumanoidBone = (srcHumanoid.bones[i] == ch.target);

            if (!isHumanoidBone)
            {
                ++dropped;
                if (const wi::scene::NameComponent* nc = animScene.names.GetComponent(ch.target))
                {
                    if (!droppedNames.empty())
                        droppedNames += ", ";
                    droppedNames += nc->name;
                }
            }
        }

        wi::backlog::post("VRMPet: retargeting '" + animName + "'  bones_in_common="
            + std::to_string(commonBones) + "/" + std::to_string(kBoneCount)
            + "  channels=" + std::to_string(srcAnim.channels.size())
            + "  dropped=" + std::to_string(dropped)
            + (droppedNames.empty() ? "" : " (" + droppedNames + ")")
            + "  length=" + Fmt(srcAnim.end - srcAnim.start) + "s");

        const wi::ecs::Entity retargeted =
            scene.RetargetAnimation(avatarRoot, animSource, true, &animScene);
        if (retargeted == wi::ecs::INVALID_ENTITY)
        {
            wi::backlog::post("VRMPet: RetargetAnimation() found no usable channel");
            wi::backlog::Flush();
            return;
        }

        wi::scene::AnimationComponent* anim = scene.animations.GetComponent(retargeted);
        if (anim == nullptr)
        {
            wi::backlog::post("VRMPet: retargeted animation has no component");
            wi::backlog::Flush();
            return;
        }

        // The import leaves the flags at their default LOOPED with PLAYING clear,
        // and the retarget copies the source's flags, so this has to be explicit.
        // An idle loop is only useful if it loops.
        anim->Play();
        anim->SetLooped(true);
        anim->timer = 0.0f;
        anim->last_update_time = 0.0f;

        wi::backlog::post("VRMPet: animation playing - "
            + std::to_string(anim->channels.size()) + " channels retargeted onto "
            + std::to_string(anim->samplers.size()) + " samplers, looped over "
            + Fmt(anim->end - anim->start) + "s");
        wi::backlog::Flush();
    }

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

        // Retarget the idle loop onto the skeleton that was just imported. Must
        // happen before the first rendered frame, so the character never appears
        // in the bind pose.
        ApplyAnimation();

        // WickedEngine enables the humanoid look-at solver by default
        // (HumanoidComponent::_flags is initialised to LOOKAT) and its target,
        // HumanoidComponent::lookAt, defaults to the world origin. The solver
        // runs whenever that flag is set - lookAtEntity only gates whether the
        // target gets refreshed from an entity - so out of the box the head and
        // both eyes are rotated every frame to stare at (0, 0, 0), which is down
        // at the character's feet. Nothing errors, nothing is logged, the pose
        // just comes out wrong: the head ends up clamped against
        // head_rotation_max (60 deg yaw, 30 deg pitch) at an angle nothing in the
        // model or the animation asks for.
        //
        // Measured with the diagnostic in DumpSceneState(): with the solver on
        // and left aimed at the origin, the head's lateral axis sits 55 deg off
        // the neck's in the bind pose and 128 deg off once the animation is
        // playing - it is the difference between the two screenshots in
        // REPORT.md section 5.
        //
        // So it is always aimed deliberately. Off (capture mode): the head
        // follows the neck, which is what the retargeted animation expects - the
        // .vrma's own head channel is dropped during retargeting anyway (see
        // ApplyAnimation), so there is nothing for the solver to blend against.
        // On (overlay mode): AimLookAt() points it at the camera every frame, so
        // the pet watches whoever is looking at the screen.
        if (wi::scene::HumanoidComponent* hum = scene.humanoids.GetComponent(scene.humanoids.GetEntity(0)))
        {
            hum->SetLookAtEnabled(g_lookAtCamera);
            wi::backlog::post(std::string("VRMPet: look-at solver ")
                + (g_lookAtCamera ? "ON - aimed at the camera" : "OFF - head follows the neck"));
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
        // to the imported skinned meshes. Skipped in overlay mode, where a stray
        // sphere floating next to the character would be a bug, not a diagnostic.
        if (!g_overlayMode)
        {
            const wi::ecs::Entity probe = scene.Entity_CreateSphere("stencil_probe", 0.12f);
            if (wi::scene::TransformComponent* tr = scene.transforms.GetComponent(probe))
            {
                tr->translation_local = XMFLOAT3(0.75f, 1.25f, 0.25f);
                tr->SetDirty();
            }
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

        // Which way is the face actually pointing? Reading it off a screenshot is
        // guesswork; reading it off the bones is not. The eye bones give the
        // head's own left-right axis, and the head bone's world basis gives its
        // forward. At azimuth 180 the camera looks down +Z from -Z, so a head
        // facing the camera has forward ~= (0, 0, -1) and right ~= (-1, 0, 0).
        // Logged in both the bind pose and the animated pose, the difference is
        // the head's actual yaw.
        if (scene.humanoids.GetCount() > 0)
        {
            using wi::scene::HumanoidComponent;
            const HumanoidComponent& hum = scene.humanoids[0];

            auto pos = [&](HumanoidComponent::HumanoidBone b, bool& valid) -> XMFLOAT3
            {
                const wi::ecs::Entity e = hum.bones[size_t(b)];
                if (const wi::scene::TransformComponent* tr = scene.transforms.GetComponent(e))
                {
                    valid = true;
                    return tr->GetPosition();
                }
                valid = false;
                return XMFLOAT3(0, 0, 0);
            };

            bool okL = false, okR = false, okH = false;
            const XMFLOAT3 L = pos(HumanoidComponent::HumanoidBone::LeftEye, okL);
            const XMFLOAT3 R = pos(HumanoidComponent::HumanoidBone::RightEye, okR);
            const XMFLOAT3 H = pos(HumanoidComponent::HumanoidBone::Head, okH);

            wi::backlog::post("VRMPet: headbone pos=" + Fmt(H.x) + "," + Fmt(H.y) + "," + Fmt(H.z)
                + "  lefteye_valid=" + std::to_string(okL)
                + " righteye_valid=" + std::to_string(okR)
                + " head_valid=" + std::to_string(okH));

            // The look-at solver builds its target rotation in the head bone's
            // own space, measuring against FORWARD = (0, 0, default_facing). That
            // only works if the head bone's local +Z really is the direction the
            // face points; when it is not, the solver drives the head the wrong
            // way. default_facing is a heuristic the engine fills in from the
            // shoulder rest positions, so it is worth seeing next to the head's
            // actual axes.
            wi::backlog::post("VRMPet: humanoid default_facing=" + Fmt(hum.default_facing)
                + " knee_bending=" + Fmt(hum.knee_bending)
                + " lookat_enabled=" + std::to_string(hum.IsLookAtEnabled() ? 1 : 0)
                + " lookAt_target=" + Fmt(hum.lookAt.x) + "," + Fmt(hum.lookAt.y) + "," + Fmt(hum.lookAt.z));

            if (okL && okR)
            {
                // Right-eye -> left-eye is the head's lateral axis. A head facing
                // the camera has it along -X (the avatar faces -Z, so its left is
                // -X in world space once the importer has turned it around).
                const XMFLOAT3 axis = XMFLOAT3(L.x - R.x, L.y - R.y, L.z - R.z);
                const float yaw = atan2f(axis.z, -axis.x) * 180.0f / XM_PI;
                wi::backlog::post("VRMPet: eye axis=" + Fmt(axis.x) + "," + Fmt(axis.y) + "," + Fmt(axis.z)
                    + "  |axis|=" + Fmt(sqrtf(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z))
                    + "  head_yaw=" + Fmt(yaw) + " deg");
            }

            if (const wi::scene::TransformComponent* htr =
                    scene.transforms.GetComponent(hum.bones[size_t(HumanoidComponent::HumanoidBone::Head)]))
            {
                const XMFLOAT3 fwd = htr->GetForward();
                const XMFLOAT3 rgt = htr->GetRight();
                const XMFLOAT3 up  = htr->GetUp();
                wi::backlog::post("VRMPet: head world fwd=" + Fmt(fwd.x) + "," + Fmt(fwd.y) + "," + Fmt(fwd.z)
                    + " right=" + Fmt(rgt.x) + "," + Fmt(rgt.y) + "," + Fmt(rgt.z)
                    + " up=" + Fmt(up.x) + "," + Fmt(up.y) + "," + Fmt(up.z));
            }

            if (const wi::scene::TransformComponent* ntr =
                    scene.transforms.GetComponent(hum.bones[size_t(HumanoidComponent::HumanoidBone::Neck)]))
            {
                const XMFLOAT3 fwd = ntr->GetForward();
                const XMFLOAT3 rgt = ntr->GetRight();
                wi::backlog::post("VRMPet: neck world fwd=" + Fmt(fwd.x) + "," + Fmt(fwd.y) + "," + Fmt(fwd.z)
                    + " right=" + Fmt(rgt.x) + "," + Fmt(rgt.y) + "," + Fmt(rgt.z));
            }
        }

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
            int base = 1;
            // Overlay mode has its own argument layout, since it has no output
            // directory and no focus/isolate options.
            //   VRMPet.exe overlay [model.vrm] [azimuth] [msaa] [seconds] [anim.vrma]
            if (argc > 1 && wcscmp(argv[1], L"overlay") == 0)
            {
                g_overlayMode = true;
                base = 2;
                // The overlay exists to put a living character on the desktop, so
                // it defaults to the idle loop. Capture mode defaults to no
                // animation: its masks are differenced across frames - mask_user
                // against mask_nostencil, mask_nostencil against background - and
                // a character that moves between them would make them disagree.
                // That is exactly the comparison REPORT.md sections 3 and 4 rest
                // on, so the earlier numbers stay reproducible.
                g_animPath = "assets/idle_loop.vrma";
                // In overlay mode the camera IS the viewer's eye, so aiming the
                // look-at solver at it is exactly "look at whoever is watching".
                // Capture mode leaves it off: there the camera is a measuring
                // instrument, and the pose should be the one the animation
                // actually produces.
                g_lookAtCamera = true;
                // Azimuth 180 puts the camera on -Z, which is the side the
                // importer turned the avatar to face. Azimuth 0 is the capture
                // default and looks at the avatar's back - correct for reading a
                // stencil mask, useless for a pet that is supposed to be looking
                // at you.
                g_azimuthDeg = 180.0f;
            }

            if (base == 2)
            {
                if (argc > base + 0) g_modelPath   = Narrow(argv[base + 0]);
                if (argc > base + 1) g_azimuthDeg  = float(_wtof(argv[base + 1]));
                if (argc > base + 2) g_msaa        = _wtoi(argv[base + 2]);
                if (argc > base + 3) g_overlaySecs = _wtoi(argv[base + 3]);
                if (argc > base + 4) g_animPath    = Narrow(argv[base + 4]);
            }
            else
            {
                //   VRMPet.exe <out-dir> [model.vrm] [azimuth] [focus] [isolate] [msaa] [anim.vrma]
                if (argc > 1) g_outDir      = Narrow(argv[1]);
                if (argc > 2) g_modelPath   = Narrow(argv[2]);
                if (argc > 3) g_azimuthDeg  = float(_wtof(argv[3]));
                if (argc > 4) g_focusMesh   = _wtoi(argv[4]);
                if (argc > 5) g_isolateMesh = _wtoi(argv[5]);
                if (argc > 6) g_msaa        = _wtoi(argv[6]);
                if (argc > 7) g_animPath    = Narrow(argv[7]);
            }

            // Bare tokens anywhere on the command line, so they do not disturb the
            // positional layout. Nothing can collide with them: every positional
            // argument is a directory, a file path, or a number.
            for (int i = 1; i < argc; ++i)
            {
                if (_wcsicmp(argv[i], L"lookat") == 0)
                    g_lookAtCamera = true;
                else if (_wcsicmp(argv[i], L"nolookat") == 0)
                    g_lookAtCamera = false;
            }

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
        case WM_HOTKEY:
            if (wParam == 1)
                PostQuitMessage(0);
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

    HWND hWnd = nullptr;
    if (g_overlayMode)
    {
        // The engine's window is only a render target here. It is never shown -
        // everything the user sees goes through the layered overlay window, which
        // g_overlay creates once the character's bounding box is known. The
        // engine's own swap chain is irrelevant: RenderPath3D draws into its own
        // textures, so the render is correct even though nothing is ever
        // presented to a visible window.
        hWnd = CreateWindowW(wcex.lpszClassName, L"VRMPet (engine, hidden)",
            WS_POPUP, 0, 0, kWindowSize, kWindowSize,
            nullptr, nullptr, hInstance, nullptr);
    }
    else
    {
        RECT rc = { 0, 0, kWindowSize, kWindowSize };
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        hWnd = CreateWindowW(wcex.lpszClassName, L"VRMPet - capture prototype",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            rc.right - rc.left, rc.bottom - rc.top,
            nullptr, nullptr, hInstance, nullptr);
        ShowWindow(hWnd, SW_SHOWDEFAULT);
    }

    if (hWnd == nullptr)
    {
        MessageBoxW(nullptr, L"Could not create the window.", L"VRMPet", MB_ICONERROR);
        return 1;
    }

    app.SetWindow(hWnd);

    wi::arguments::Parse(lpCmdLine);

    // A capture tool must keep rendering even when the window is not focused -
    // this process is launched from a background shell and never receives
    // WM_SETFOCUS, and Application::Run() returns early when inactive. In overlay
    // mode the engine window is never shown at all, so this is the only thing
    // keeping the render loop alive.
    app.is_window_active = true;

    if (g_overlayMode)
    {
        // The overlay is click-through and has no taskbar button, so without a
        // global hotkey there would be no way to close it.
        if (!RegisterHotKey(hWnd, 1, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'Q'))
        {
            wi::backlog::post("VRMPet: WARNING - could not register Ctrl+Shift+Q");
            wi::backlog::Flush();
        }
    }

    MSG msg = {};
    const auto startTime = std::chrono::steady_clock::now();
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            if (g_overlayMode && g_overlaySecs > 0)
            {
                const double elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - startTime).count();
                if (elapsed >= double(g_overlaySecs))
                {
                    wi::backlog::post("VRMPet: overlay reached its "
                        + std::to_string(g_overlaySecs) + "s limit - quitting");
                    wi::backlog::Flush();
                    break;
                }
            }
            app.is_window_active = true;
            app.Run();
        }
    }

    if (g_overlayMode)
        UnregisterHotKey(hWnd, 1);

    wi::jobsystem::ShutDown();
    return (int)msg.wParam;
}
