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
//  No arguments is the pet: `VRMPet.exe` alone is overlay mode with the idle
//  loop and the agent, which is what this program is for. Everything below is
//  that same run with something changed.
//
//  Overlay mode - the avatar standing on the desktop, no window, no background:
//    VRMPet.exe [overlay] [model.vrm] [azimuth-deg] [msaa] [seconds] [anim.vrma]
//      azimuth 180 faces the camera. seconds 0 = run until Ctrl+Shift+Q.
//
//  Capture mode - writes PNGs for analysis. It has to name itself, because the
//  bare command line now belongs to the overlay:
//    VRMPet.exe capture [out-dir] [model.vrm] [azimuth-deg] [focus-mesh] [isolate-mesh] [msaa] [anim.vrma]
//  Any first argument that is not a mode word is still read as out-dir, so the
//  old capture command lines keep working.
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
//
//  Talking, in overlay mode (see docs/DESIGN-voice-and-chat.md, option A):
//
//    mic  -> POST /v1/asr   -> the agent's own LLM (no extra LLM)
//         -> POST /v1/tts   -> wi::audio -> the mouth
//
//  Messages appear in a speech bubble above the avatar's head, composited into
//  the same layered window, so there is no second window and no separate log.
//  Ctrl+Shift+V pastes the clipboard as a chat message, Ctrl+Shift+H widens the
//  transcript band, Ctrl+Shift+M mutes the microphone, Ctrl+Shift+Q still quits.
//
//  Everything tunable - the band's size and colours, the avatar's headroom, the
//  mouth gain, the API key, the agent, the voice model and the greeting - lives
//  in config.txt next to VRMPet.exe. See src/config.h; the file is written with
//  every key and its comment on a run where none is found.
//
//  Each leg also has a command-line probe that runs before the engine starts, so
//  a failure has one possible cause:
//
//    VRMPet.exe agent "hello"      one turn through the agent, text only
//    VRMPet.exe asr out/turn.wav   a wav through speech-to-text
//    VRMPet.exe tts "hello"        one line of speech as wav + validation
//    VRMPet.exe mic 10             the microphone and the speech gate, no network
//    VRMPet.exe bubble "hello"     the bubble band alone, written to a BMP
//    VRMPet.exe config             the complete config.txt, to stdout
//    VRMPet.exe ask "hello"        overlay + one scripted turn, then quit
// ---------------------------------------------------------------------------

#include "WickedEngine.h"

#include <shellapi.h>
#include <windowsx.h>   // GET_X_LPARAM, for the hit test

#include "bubble.h"
#include "config.h"
#include "fish.h"
#include "petcall.h"
#include "util.h"
#include "voice.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <functional>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Declared in WickedEngine/Editor/ModelImporter.h. The engine library exports no
// model importer - wi::scene::LoadModel() only understands the .wiscene Archive
// format - so the editor's glTF/VRM importer is compiled into this target
// instead (see CMakeLists.txt).
void ImportModel_GLTF(const std::string& fileName, wi::scene::Scene& scene);

namespace
{
    // -----------------------------------------------------------------------
    // config.txt
    //
    // Beside VRMPet.exe first, then the working directory, then upwards - the
    // same folder-first rule ResolveAssetPath() uses for the avatar, so the tool
    // behaves the same from build/run, from the project root and from a shortcut
    // with an unrelated working directory. The file is the interface: every
    // number that used to be a constant here is read from it, and a key the file
    // omits keeps the default in config.cpp.
    // -----------------------------------------------------------------------
    std::vector<std::string> ConfigSearchPath()
    {
        // The shipped config.txt is staged beside the executable by the build
        // (see CMakeLists.txt), and the working directory is checked second so
        // the tool also works when it is launched with a file manager's default
        // directory. The project root deliberately is NOT searched: the file in
        // the repository is the template the build copies from, and a second
        // copy that silently disagreed with the live one would be worse than no
        // fallback at all.
        return { vrmpet::ExeDirectory(), vrmpet::WorkingDirectory() };
    }

    int   WindowSize()  { return vrmpet::Settings().Int("overlay.window_size"); }
    int   CropPad()     { return vrmpet::Settings().Int("overlay.crop_pad"); }
    int   CheckFrames() { return vrmpet::Settings().Int("overlay.check_frames"); }
    int   SettleFrames(){ return vrmpet::Settings().Int("overlay.settle_frames"); }
    int   BandGap()     { return vrmpet::Settings().Int("band.gap"); }
    int   BandWidth()   { return vrmpet::Settings().Int("band.width"); }
    int   BandHeight()  { return vrmpet::Settings().Int("band.height"); }

    // avatar.zoom, clamped, in one place. RenderSize() grows the render buffer by
    // it. FrameCamera deliberately does NOT move the camera by it: pulling the
    // camera closer is a true zoom-in and crops the pet.
    float Zoom() { return (std::max)(0.25f, (std::min)(4.0f, vrmpet::Settings().Float("avatar.zoom"))); }

    // The engine's render target, in px. avatar.zoom multiplies it: the camera keeps
    // framing the whole avatar (FrameCamera), so the same framing is rasterised at
    // zoom times the pixels, and the crop box - measured in those pixels - comes out
    // zoom times larger on screen. Growing the buffer is what makes the pet bigger
    // without cropping it, which is exactly what a fixed overlay.window_size failed
    // to do past about 1.2 (the "zoom 2 cuts the pet" bug).
    int RenderSize() { return int(std::lround(double(WindowSize()) * double(Zoom()))); }

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

    // String helpers and the logger live in vrmpet:: (src/util.{h,cpp}) because
    // the voice modules need exactly the same ones. Narrow() takes a wchar_t*
    // here and a std::wstring there; both bind to an LPWSTR argument.
    using vrmpet::Fmt;
    using vrmpet::Log;
    using vrmpet::Settings;
    std::string Narrow(const wchar_t* wide) { return vrmpet::Narrow(wide == nullptr ? std::wstring() : std::wstring(wide)); }

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
    bool g_overlaySecsFromCli = false;
    int  g_overlayMargin = 0;   // gap between the character and the work-area edge

    // -----------------------------------------------------------------------
    // The bubble band, and why the canvas is bigger than the avatar
    //
    // The window's bottom-right corner is pinned to the work area and the canvas
    // grows *up* to hold the band. The avatar therefore lands on exactly the
    // pixel it used to: px + avatarX == wa.right - cropW - g_overlayMargin.
    // The band is measured once, before the window exists, and never resized -
    // the same rule overlay.crop_pad follows for the avatar itself.
    //
    // The band and the avatar are both glued to the canvas's right edge, so the
    // transcript hangs over the avatar's head. Left-aligning the band instead
    // would park it in the empty desktop beside the character, which is where it
    // was and where it looked wrong: the pet is in the corner of the screen, so
    // "above the pet" and "the right edge of the screen" are the same place.
    // -----------------------------------------------------------------------
    // BandGap(), BandWidth() and BandHeight() above are the live values; these
    // are only what the comments refer to.

    // Voice and chat. The transport is built by PetApp::StartVoice(), which is
    // the only place that knows a network exists.
    std::string g_askText;                  // "ask" mode: one scripted turn, then quit
    bool        g_askMode = false;          // ...and only that one turn: no greeting
    std::string g_probeMode;                // agent | asr | tts | mic | bubble | config
    std::string g_probeArg;
    int         g_probeSeconds = 6;
    float       g_mouthGain = 1.0f;
    bool        g_mouthGainFromCli = false;
    bool        g_micEnabled = true;

    // Whether the transcript band is drawn. Set from band.visible at startup and
    // flipped by the right-click menu from then on; the window keeps its full
    // size either way, so the band is blanked rather than the layout moving.
    bool        g_bandVisible = true;

    // Frames to render before the first blit, so spring bones and temporal
    // effects have settled and the character is not captured mid-twitch.


    // An idle loop sways the hips and swings the arms, so the tight alpha box
    // measured over the first few settled frames is not necessarily the box for
    // the whole loop. Pad it generously, and re-measure once a second so a
    // too-tight crop reports itself as a number in the log rather than as a
    // hand quietly losing its fingers at the window edge.


    // Set by PetApp once the overlay window exists. The hotkeys are registered
    // on that window, so its window proc is where they arrive - and the proc is
    // a static member of the struct below, which cannot see the instance that is
    // declared after it.
    std::function<void(int)> g_overlayHotkey;

    // A pointer to the overlay, declared before the overlay itself: an object
    // cannot be declared before its own type, but a pointer to it can, and the
    // inline window proc below needs to name it. Set at the top of Create(),
    // before the window exists - messages can arrive during creation.
    struct LayeredOverlay;
    LayeredOverlay* g_overlayPtr = nullptr;

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
            g_overlayPtr = this;
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

            // WS_EX_TRANSPARENT is deliberately NOT set. It would make the whole
            // window click-through, which also means the pet could never be
            // grabbed or right-clicked. The click-through is per pixel instead,
            // in WM_NCHITTEST below: the avatar's own pixels belong to the pet,
            // everything else on the canvas still belongs to the desktop behind
            // it. overlay.grabbable = 0 restores the old always-click-through
            // window for anyone who wants the pet to be pure decoration.
            hwnd = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
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
        // dstX/dstY place the avatar inside the composited canvas rather than at
        // (0, 0). Only the avatar's rows and columns are touched, which is what
        // leaves the bubble band above it exactly as the bubble drew it - so the
        // bubble costs one composite per turn, not one per frame.
        void Blit(const uint8_t* rgba, int srcW, int cropX, int cropY,
                  int cropW, int cropH, int dstX, int dstY)
        {
            if (bits == nullptr)
                return;
            for (int row = 0; row < cropH; ++row)
            {
                const uint8_t* src = rgba + (size_t(cropY + row) * size_t(srcW) + size_t(cropX)) * 4;
                uint8_t* dst = bits + (size_t(dstY + row) * size_t(w) + size_t(dstX)) * 4;
                for (int col = 0; col < cropW; ++col)
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

        // Is this canvas pixel solid enough to be the pet? The composited
        // bitmap is the only thing that knows: its alpha is the avatar's
        // silhouette blended with whatever the band drew, so a test on it is a
        // test on exactly what the user can see. (This is the same channel
        // ComputeCropBox() measures, read for a different reason.)
        bool OpaqueAt(int clientX, int clientY) const
        {
            if (bits == nullptr || clientX < 0 || clientY < 0 || clientX >= w || clientY >= h)
                return false;
            const int threshold = (std::max)(0, (std::min)(255, Settings().Int("overlay.grab_alpha")));
            return bits[(size_t(clientY) * size_t(w) + size_t(clientX)) * 4 + 3] >= uint8_t(threshold);
        }

        // The right-click menu: the only way to close the pet with a mouse. The
        // overlay has no taskbar button, no title bar and never takes focus, so
        // without this Ctrl+Shift+Q would be the only way out - which is fine
        // for its author and useless for everyone else.
        static void ShowMenu(HWND hWnd)
        {
            HMENU menu = CreatePopupMenu();
            if (menu == nullptr)
                return;

            AppendMenuW(menu, MF_STRING, 1, L"Close VRMPet");
            AppendMenuW(menu, MF_STRING, 2, L"Mute / unmute the microphone");
            AppendMenuW(menu, MF_STRING, 3, L"Send the clipboard as a message");
            AppendMenuW(menu, MF_STRING | (g_bandVisible ? MF_CHECKED : MF_UNCHECKED),
                        7, L"Show transcript bubbles");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 6, L"Open config.txt");
            // Which copy, spelled out: there is a shipped template in the source
            // tree and a live one beside the exe, and editing the wrong one is
            // the one mistake this menu exists to prevent.
            const std::string live = Settings().Path().empty()
                ? JoinPath(vrmpet::ExeDirectory(), "config.txt") : Settings().Path();
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, vrmpet::Widen(live).c_str());
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 4, L"Return to the corner");
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 5, L"Drag the pet to move it");

            POINT pt = {};
            GetCursorPos(&pt);
            // The window never activates, so it has to be pushed to the
            // foreground for the menu to receive the click that dismisses it -
            // and the WM_NULL afterwards is the documented way to close that
            // door again rather than leave the window stuck in the foreground.
            SetForegroundWindow(hWnd);
            const int id = int(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                                pt.x, pt.y, hWnd, nullptr));
            PostMessageW(hWnd, WM_NULL, 0, 0);
            DestroyMenu(menu);

            switch (id)
            {
            case 1:
                wi::backlog::post("VRMPet: closed from the menu");
                wi::backlog::Flush();
                PostQuitMessage(0);
                break;
            case 2:
                if (g_overlayHotkey) g_overlayHotkey(4);   // the mic hotkey's id
                break;
            case 3:
                if (g_overlayHotkey) g_overlayHotkey(2);   // the clipboard hotkey's id
                break;
            case 4:
                MoveHome(hWnd);
                break;
            case 6:
                OpenConfig(hWnd);
                break;
            case 7:
                if (g_overlayHotkey) g_overlayHotkey(7);   // the bubbles' toggle
                break;
            default:
                break;
            }
        }

        // Back to the work area's corner, which is where Create() put it. The
        // window's own rect is all that has to move: the band and the avatar are
        // two halves of one bitmap, so they cannot come apart.
        static void OpenConfig(HWND hWnd)
        {
            std::string path = Settings().Path();
            if (path.empty())
            {
                path = JoinPath(vrmpet::ExeDirectory(), "config.txt");
                // No file anywhere yet: write one so "open" opens a real file
                // rather than the shell's "where is it" dialog.
                Settings().Load(ConfigSearchPath());
                path = Settings().Path().empty() ? path : Settings().Path();
            }

            const HINSTANCE result = ShellExecuteW(hWnd, L"open",
                                                   vrmpet::Widen(path).c_str(),
                                                   nullptr, nullptr, SW_SHOWNORMAL);
            if (INT_PTR(result) <= 32)
            {
                wi::backlog::post("VRMPet: could not open " + path + " (ShellExecute returned "
                                  + std::to_string((long long)result) + ") - open it by hand");
                wi::backlog::Flush();
            }
            else
            {
                wi::backlog::post("VRMPet: opened " + path);
                wi::backlog::Flush();
            }
        }

        static void MoveHome(HWND hWnd)
        {
            if (g_overlayPtr == nullptr)
                return;
            LayeredOverlay& self = *g_overlayPtr;
            RECT wa = {};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            const int homeX = wa.right - self.w - Settings().Int("overlay.margin");
            const int homeY = wa.bottom - self.h - Settings().Int("overlay.margin");
            self.x = homeX;
            self.y = homeY;
            SetWindowPos(hWnd, HWND_TOPMOST, homeX, homeY, self.w, self.h,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            wi::backlog::post("VRMPet: overlay moved back to " + std::to_string(homeX) + ","
                              + std::to_string(homeY));
            wi::backlog::Flush();
        }

        static LRESULT CALLBACK OverlayProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            switch (msg)
            {
            case WM_NCHITTEST:
            {
                // Per-pixel click-through, and the drag: returning HTCAPTION
                // hands the move to Windows' own modal drag loop, which is the
                // only way to move a layered window without a frame of lag
                // behind the cursor.
                if (!Settings().Flag("overlay.grabbable"))
                    return HTTRANSPARENT;
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hWnd, &pt);
                return g_overlayPtr != nullptr && g_overlayPtr->OpaqueAt(pt.x, pt.y)
                    ? HTCAPTION : HTTRANSPARENT;
            }
            case WM_NCLBUTTONDBLCLK:
                return 0;   // a double-click on the pet must not maximise it
            case WM_MOVE:
            {
                // The window was dragged. Push() passes a destination point to
                // UpdateLayeredWindow every frame, so a stale x/y would snap the
                // pet back to where it started on the next composite.
                POINT origin = { 0, 0 };
                ClientToScreen(hWnd, &origin);
                if (g_overlayPtr != nullptr)
                {
                    g_overlayPtr->x = origin.x;
                    g_overlayPtr->y = origin.y;
                }
                return 0;
            }
            case WM_RBUTTONUP:
            case WM_NCRBUTTONUP:
                ShowMenu(hWnd);
                return 0;
            case WM_HOTKEY:
                if (wParam == 1)
                {
                    wi::backlog::post("VRMPet: overlay hotkey - quitting");
                    wi::backlog::Flush();
                    PostQuitMessage(0);
                }
                else if (g_overlayHotkey)
                {
                    g_overlayHotkey(int(wParam));
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

    // -----------------------------------------------------------------------
    // Small Win32 helpers the voice feature needs
    // -----------------------------------------------------------------------

    // Ctrl+Shift+V is the typed-input path. The overlay is click-through and
    // never takes focus, so there is nowhere to type into - composing in any
    // editor and pasting is both the honest answer to "chat" and the one that
    // needs no focusable surface.
    std::string ReadClipboardText()
    {
        std::string text;
        if (!OpenClipboard(nullptr))
            return text;
        if (HANDLE handle = GetClipboardData(CF_UNICODETEXT))
        {
            if (const wchar_t* raw = (const wchar_t*)GlobalLock(handle))
            {
                text = vrmpet::Narrow(std::wstring(raw));
                GlobalUnlock(handle);
            }
        }
        CloseClipboard();
        return text;
    }

    // config.txt lookup is ConfigSearchPath() above; this name is gone with .env.

    bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out)
    {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f == nullptr)
            return false;
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size <= 0)
        {
            std::fclose(f);
            return false;
        }
        out.resize(size_t(size));
        const size_t read = std::fread(out.data(), 1, out.size(), f);
        std::fclose(f);
        out.resize(read);
        return read == size_t(size);
    }

    // -----------------------------------------------------------------------
    // Probes
    //
    // Each one runs before the engine starts - no window, no GPU, no shader
    // compilation - and exercises exactly one leg of the pipeline, so a failure
    // has one possible cause. This is the staged plan from the design document
    // turned into commands, and it is also how the parts were verified here:
    // the microphone could be checked without the network, the network without
    // the microphone, and the bubble without either.
    //
    // Returns a process exit code: 0 pass, 1 fail, 2 unusable.
    // -----------------------------------------------------------------------
    int RunProbe(const std::string& outDir)
    {
        using namespace vrmpet;

        EnsureDirectory(outDir);
        wi::backlog::SetLogFile(outDir + "/probe.log");
        Log("VRMPet: probe '" + g_probeMode + "' -> " + outDir);

        // --- the microphone, and nothing else -------------------------------
        if (g_probeMode == "mic")
        {
            int utterances = 0;
            double speechSeconds = 0.0;
            float loudest = 0.0f;
            float lastUtteranceRms = 0.0f;

            MicCapture mic;
            mic.Start(
                [&](Utterance utterance)
                {
                    ++utterances;
                    speechSeconds += utterance.seconds;
                    lastUtteranceRms = utterance.rms;
                },
                nullptr);

            const double end = NowSeconds() + double(g_probeSeconds);
            double nextLevel = NowSeconds() + 0.5;
            while (NowSeconds() < end)
            {
                const float level = mic.Level();
                if (level > loudest)
                    loudest = level;
                if (NowSeconds() >= nextLevel)
                {
                    nextLevel += 0.5;
                    Log("VRMPet:   level " + Fmt(level, 4));
                }
                Sleep(20);
            }
            mic.Stop();

            const MicStatus status = mic.Status();
            Log("VRMPet: probe mic - endpoints " + std::to_string(status.devices)
                + ", opened " + (status.opened ? "yes" : "NO")
                + ", device '" + status.deviceName + "'");
            if (status.opened)
            {
                Log("VRMPet: probe mic - source " + std::to_string(status.sourceRate)
                    + " Hz -> 16000 Hz" + (status.resampled ? " (converted in-process)" : ""));
            }
            else if (!status.error.empty())
            {
                Log("VRMPet: probe mic - " + status.error);
            }
            Log("VRMPet: probe mic - loudest frame " + Fmt(loudest, 4)
                + ", utterances " + std::to_string(utterances)
                + ", speech " + Fmt(float(speechSeconds), 2) + "s"
                + (utterances > 0 ? ", last rms " + Fmt(lastUtteranceRms, 4) : ""));

            if (!status.opened)
                return 1;
            if (loudest <= 0.00001f)
            {
                Log("VRMPet: probe mic - the device delivered digital silence. On Windows 11 "
                    "that is usually Settings > Privacy > Microphone rather than the A.");
                return 1;
            }
            if (g_probeSeconds >= 3 && utterances == 0)
            {
                Log("VRMPet: probe mic - the device is live but nothing crossed the speech "
                    "gate; say something, or lower VRMPet's start threshold.");
                return 1;
            }
            Log("VRMPet: probe mic OK");
            return 0;
        }

        // --- the bubble band, and nothing else ------------------------------
        if (g_probeMode == "bubble")
        {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

            Bubble bubble;
            if (!bubble.Create(BandWidth(), BandHeight()))
            {
                Log("VRMPet: probe bubble FAILED - the band was not created");
                return 1;
            }

            bubble.OnTranscript(1, "user",
                g_probeArg.empty() ? "Hello, who are you?" : g_probeArg, true);
            bubble.OnTranscript(2, "agent",
                "Please, as if you didn't already know. I'm Draconia Malfoy, and I do not "
                "repeat myself for people who cannot be bothered to listen.", true);
            bubble.SetStatus("speaking");
            bubble.SetLevel(0.6f, true);
            bubble.Render();

            const std::string path = outDir + "/bubble.bmp";
            if (!bubble.SaveBitmap(path))
            {
                Log("VRMPet: probe bubble FAILED - could not write " + path);
                return 1;
            }

            // A numeric orientation check, because "the text is upside down" is
            // not something a log line can otherwise report: the bubbles sit in
            // the upper part of the band and the status line at the bottom, so
            // the top half must hold most of the opaque pixels.
            size_t top = 0, bottom = 0;
            for (int y = 0; y < bubble.Height(); ++y)
            {
                const uint8_t* row = bubble.Row(y);
                for (int x = 0; x < bubble.Width(); ++x)
                {
                    if (row[size_t(x) * 4 + 3] > 8)
                        (y < bubble.Height() / 2 ? top : bottom)++;
                }
            }
            Log("VRMPet: probe bubble - wrote " + path + " ("
                + std::to_string(bubble.Width()) + "x" + std::to_string(bubble.Height()) + ")");
            Log("VRMPet: probe bubble - opaque pixels: top half " + std::to_string(top)
                + ", bottom half " + std::to_string(bottom));
            return (top > bottom) ? 0 : 1;
        }

        // The complete file, to stdout, so it can be redirected into a fresh
        // install (or the project's shipped copy) rather than documented. It
        // comes from the same table the reader falls back to, so what is printed
        // is exactly what an absent key would take.
        if (g_probeMode == "config")
        {
            const std::string text = vrmpet::Settings().Render();
            std::fwrite(text.data(), 1, text.size(), stdout);
            std::fflush(stdout);
            Log("VRMPet: probe config - " + std::to_string(text.size()) + " bytes to stdout ("
                + std::to_string(vrmpet::Settings().Missing().size()) + " keys not set in the live file)");
            return 0;
        }

        // --- the three network legs -----------------------------------------
        FishClient fish;
        if (!fish.Configure(ConfigSearchPath()))
        {
            Log("VRMPet: probe " + g_probeMode + " FAILED - " + fish.Error());
            return 2;
        }
        if (!fish.ResolveAgent() || !fish.FetchAgentConfig())
        {
            Log("VRMPet: probe " + g_probeMode + " FAILED - " + fish.Error());
            return 2;
        }

        if (g_probeMode == "agent")
        {
            std::vector<TurnMessage> conversation;
            conversation.push_back({ "user", g_probeArg.empty() ? "Hello, who are you?" : g_probeArg });
            std::string reply;
            FishResult result;
            if (!fish.AgentTurn(conversation, reply, result))
            {
                Log("VRMPet: probe agent FAILED - " + result.error);
                return 1;
            }
            Log("VRMPet: probe agent - reply in " + FmtMs(result.ms) + ": " + reply);
            Log("VRMPet: probe agent OK (the scratch test is deleted; the run stays on "
                "the account's run list)");
            return 0;
        }

        if (g_probeMode == "tts")
        {
            std::vector<int16_t> samples;
            FishResult result;
            if (!fish.Tts(g_probeArg.empty() ? "Oh please, Potter. Try to keep up." : g_probeArg,
                          samples, result))
            {
                Log("VRMPet: probe tts FAILED - " + result.error);
                return 1;
            }

            const std::vector<uint8_t> wav = WrapPcm16AsWav(samples, 16000);
            const size_t expected = 44 + samples.size() * 2;
            const std::string path = outDir + "/tts.wav";
            std::FILE* f = std::fopen(path.c_str(), "wb");
            if (f != nullptr)
            {
                std::fwrite(wav.data(), 1, wav.size(), f);
                std::fclose(f);
            }

            Log("VRMPet: probe tts - " + std::to_string(samples.size()) + " samples = "
                + Fmt(float(samples.size()) / 16000.0f, 2) + "s at 16 kHz mono, in "
                + FmtMs(result.ms));
            Log("VRMPet: probe tts - wrapped " + std::to_string(wav.size()) + " bytes (expected "
                + std::to_string(expected) + ") -> " + path);

            std::string why;
            const bool good = SafeToPlay(wav.data(), wav.size(), why);
            Log("VRMPet:   validator accepts our own wav: " + std::string(good ? "yes" : "NO")
                + (why.empty() ? "" : " (" + why + ")"));

            // The negative test the design document asks for at stage 0: the
            // placeholder data size a streaming endpoint writes must be rejected
            // before it reaches wi::audio, where it would be a 4 GB resize.
            std::vector<uint8_t> corrupted = wav;
            const uint32_t placeholder = 0xFFFFFC00u;
            memcpy(&corrupted[40], &placeholder, 4);
            std::string whyCorrupt;
            const bool rejected = !SafeToPlay(corrupted.data(), corrupted.size(), whyCorrupt);
            Log("VRMPet:   validator rejects the streaming placeholder size: "
                + std::string(rejected ? "yes" : "NO") + " (" + whyCorrupt + ")");

            if (!good || !rejected || wav.size() != expected)
            {
                Log("VRMPet: probe tts FAILED");
                return 1;
            }
            Log("VRMPet: probe tts OK");
            return 0;
        }

        if (g_probeMode == "asr")
        {
            std::vector<uint8_t> bytes;
            if (g_probeArg.empty() || !ReadFileBytes(g_probeArg, bytes))
            {
                Log("VRMPet: probe asr FAILED - could not read '" + g_probeArg + "'");
                return 2;
            }

            std::string why;
            if (!SafeToPlay(bytes.data(), bytes.size(), why))
            {
                Log("VRMPet: probe asr FAILED - " + why);
                return 2;
            }

            // Walk our own header rather than trust anything in it twice.
            size_t dataPos = 0, dataSize = 0;
            uint32_t rate = 0;
            size_t pos = 12;
            while (pos + 8 <= bytes.size())
            {
                char id[4];
                memcpy(id, bytes.data() + pos, 4);
                uint32_t size = 0;
                memcpy(&size, bytes.data() + pos + 4, 4);
                if (memcmp(id, "fmt ", 4) == 0)
                    memcpy(&rate, bytes.data() + pos + 8 + 4, 4);
                if (memcmp(id, "data", 4) == 0)
                {
                    dataPos = pos + 8;
                    dataSize = size;
                    break;
                }
                pos += 8 + size + (size & 1u);
            }

            std::vector<int16_t> samples(dataSize / 2);
            memcpy(samples.data(), bytes.data() + dataPos, dataSize);

            std::string text;
            FishResult result;
            if (!fish.Asr(samples, int(rate == 0 ? 16000 : rate), text, result))
            {
                Log("VRMPet: probe asr FAILED - " + result.error);
                return 1;
            }
            Log("VRMPet: probe asr - " + Fmt(float(samples.size()) / float(rate), 2)
                + "s at " + std::to_string(rate) + " Hz -> \"" + text + "\" in "
                + FmtMs(result.ms));
            Log("VRMPet: probe asr OK");
            return 0;
        }

        Log("VRMPet: probe '" + g_probeMode + "' is not a probe");
        return 2;
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

    // Overlay mode state
    bool overlayReady   = false;
    int  overlaySettle  = 0;
    bool overlayCreated = false;
    bool greeted       = false;
    int  cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    long long overlayFrames = 0;
    std::chrono::steady_clock::time_point overlayClock;
    int  overlayCheck = 0;      // frames since the last crop-box re-measure
    // Worst px by which the character has left the crop box. Negative means
    // margin. Starts at INT_MIN so the very first measurement always reports,
    // whether it is a warning or the reassuring "still contains it" line.
    int  overlayOverflow = INT_MIN;

    // Voice and chat. The bubble band is composited into the overlay's own
    // bitmap, so there is no second window and no separate log.
    vrmpet::Bubble         bubble;
    vrmpet::Speaker        speaker;
    vrmpet::CallController call;
    bool   voiceStarted = false;
    int    bandW = 0, bandH = 0, bandX = 0, avatarX = 0, avatarY = 0;
    bool   askSent = false;
    bool   bandNotYetComposited = true;
    double askStartAt = -1.0;
    double askDoneAt = -1.0;

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
            UpdateOverlay(dt);
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
    void UpdateOverlay(float dt)
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

        if (!voiceStarted)
        {
            StartVoice();
            voiceStarted = true;
        }

        // The mouth is driven here, on the main thread, because every wi::audio
        // call is made here. An idle speaker still gets an update: that is what
        // closes the mouth. The capture itself happens in OnFrameComplete().
        const float mouth = speaker.Update(dt, g_mouthGain);
        vrmpet::ApplyMouth(mouth);

        // Drain the worker's events into the bubble, then publish the state the
        // band shows. Both are Direct2D on this thread's DC, so both are here.
        call.Tick();
        bubble.SetLevel(call.Level(), call.MicOpen());
        bubble.SetStatus(call.StatusText());

        if (!g_askText.empty() && !askSent)
        {
            Log("VRMPet: scripted turn - \"" + g_askText + "\"");
            call.SendText(g_askText);
            askSent = true;
            askStartAt = vrmpet::NowSeconds();
        }

        // "ask" mode exists so a turn can be verified end to end without a
        // microphone and without watching the screen: it quits once the reply
        // has been spoken, so the run has a defined end and a measurable length.
        if (askSent && g_overlaySecs <= 0)
        {
            const double now = vrmpet::NowSeconds();
            const bool quiet = !call.Busy() && !speaker.IsPlaying() && speaker.Pending() == 0;
            if (quiet && speaker.SpokenLines() > 0)
            {
                if (askDoneAt < 0.0)
                {
                    askDoneAt = now;
                    Log("VRMPet: scripted turn spoken - quitting in 2s");
                }
                else if (now - askDoneAt > 2.0)
                {
                    wi::backlog::Flush();
                    PostQuitMessage(0);
                }
            }
            else if (askStartAt > 0.0 && now - askStartAt > 120.0)
            {
                Log("VRMPet: WARNING - the scripted turn never produced speech; quitting");
                wi::backlog::Flush();
                PostQuitMessage(0);
            }
        }

        ++overlaySettle;
    }

    // -----------------------------------------------------------------------
    // Voice wiring
    //
    // Everything the transport needs is built here and nowhere else, and the
    // two directions are kept apart on purpose: the worker thread produces audio
    // and events, this thread (through Speaker and Bubble) consumes them.
    // -----------------------------------------------------------------------
    void StartVoice()
    {
        // config.txt is looked for beside the executable first, then in the
        // working directory, then upwards - the same folder-first rule the
        // assets use, so it works from build/run, from the project root and from
        // a shortcut with any working directory at all.
        const std::vector<std::string> configDirs = ConfigSearchPath();

        call.Configure(configDirs);

        // The samples cross the thread boundary once per spoken sentence and are
        // owned by the main thread from that point on.
        call.SetAudioSink([this](std::vector<int16_t> samples)
        {
            speaker.Enqueue(std::move(samples), std::string());
        });

        vrmpet::AgentEvents events;
        events.onTranscript = [this](uint32_t segmentId, const std::string& role,
                                     const std::string& text, bool final)
        {
            bubble.OnTranscript(segmentId, role, text, final);
        };
        events.onMode = [](const std::string& mode) { Log("VRMPet: mode -> " + mode); };
        events.onError = [](const std::string& message) { Log("VRMPet: turn failed - " + message); };
        call.Attach(events);

        // Section 10.1 of the report, done before any mouth code runs: a model
        // without an `aa` preset has no mouth at all, and the engine's own
        // talking path would index presets[] out of bounds.
        vrmpet::DumpExpressionPresets();
        std::string why;
        if (!vrmpet::MouthAvailable(why))
            Log("VRMPet: no mouth - " + why);

        if (!g_micEnabled)
            Log("VRMPet: microphone muted at startup (-mute)");

        call.Start();
        Log("VRMPet: voice ready - Ctrl+Shift+V pastes the clipboard as a message, "
            "Ctrl+Shift+H widens the transcript, Ctrl+Shift+M mutes the mic");
    }

    // Ctrl+Shift+V / H / M. The overlay is click-through and has no taskbar
    // button, so hotkeys are the only input surface it has - which is also why
    // the clipped text comes from the clipboard rather than from a text box.
    void OnOverlayHotkey(int id)
    {
        switch (id)
        {
        case 2:
        {
            const std::string text = ReadClipboardText();
            if (text.empty())
            {
                Log("VRMPet: Ctrl+Shift+V - the clipboard holds no text");
                break;
            }
            bubble.SetStatus("sending the clipboard...");
            call.SendText(text);
            break;
        }
        case 3:
        {
            // One line and the configured default: the band clamps the default
            // to what actually fits, so the pair is never the same number twice.
            const int wide = bubble.GetStyle().historyLines;
            if (wide <= 1)
            {
                Log("VRMPet: band.history_lines is 1 - nothing to toggle; raise it in config.txt");
                break;
            }
            const int next = bubble.HistoryLines() == 1 ? wide : 1;
            bubble.SetHistoryLines(next);
            Log("VRMPet: transcript band -> " + std::to_string(next) + " lines");
            break;
        }
        case 4:
            call.SetMicEnabled(!call.MicEnabled());
            break;
        case 7:
            // Blank or restore the band. bandNotYetComposited makes the next
            // frame act on the change instead of waiting for the bubble to
            // become dirty, which for "hide" would never happen.
            g_bandVisible = !g_bandVisible;
            bandNotYetComposited = true;
            Log("VRMPet: transcript band " + std::string(g_bandVisible ? "shown" : "hidden"));
            break;
        default:
            break;
        }
    }

    void OnFrameComplete()
    {
        if (!g_overlayMode || !overlayReady || overlaySettle < SettleFrames())
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

        const int pad    = CropPad();
        const int left   = std::max(0, x - pad);
        const int top    = std::max(0, y - pad);
        const int right  = std::min(w, x + bw + pad);
        const int bottom = std::min(h, y + bh + pad);

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
                    "(box %dx%d at %d,%d, tight %dx%d at %d,%d) - raise overlay.crop_pad",
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

            // The bubble band is reserved here, once, before the window exists:
            // the text grows inside it and the window never resizes.
            bandW = BandWidth();
            bandH = BandHeight();
            // Both pinned to the right edge; the canvas is only as wide as the
            // wider of the two, so the window no longer reserves a 420 px
            // column that nothing is drawn in.
            const int canvasW = (std::max)(bandW, cropW);
            const int canvasH = bandH + BandGap() + cropH;
            bandX   = canvasW - bandW;
            avatarX = canvasW - cropW;
            avatarY = bandH + BandGap();

            RECT wa = {};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            const int px = wa.right - canvasW - g_overlayMargin;
            const int py = wa.bottom - canvasH - g_overlayMargin;

            if (!bubble.Create(bandW, bandH))
            {
                Log("VRMPet: bubble band creation FAILED - the avatar still runs, without text");
            }

            if (!g_overlay.Create(canvasW, canvasH, px, py))
            {
                Log("VRMPet: overlay window creation FAILED");
                return;
            }
            g_overlay.Show();

            g_overlayHotkey = [this](int id) { OnOverlayHotkey(id); };
            if (!RegisterHotKey(g_overlay.hwnd, 2, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'V'))
                Log("VRMPet: WARNING - could not register Ctrl+Shift+V (another app owns it)");
            if (!RegisterHotKey(g_overlay.hwnd, 3, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'H'))
                Log("VRMPet: WARNING - could not register Ctrl+Shift+H (another app owns it)");
            if (!RegisterHotKey(g_overlay.hwnd, 4, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'M'))
                Log("VRMPet: WARNING - could not register Ctrl+Shift+M (another app owns it)");

            Log("VRMPet: overlay canvas " + std::to_string(canvasW) + "x" + std::to_string(canvasH)
                + " at " + std::to_string(px) + "," + std::to_string(py)
                + "  avatar " + std::to_string(cropW) + "x" + std::to_string(cropH)
                + " at " + std::to_string(px + avatarX) + "," + std::to_string(py + avatarY)
                + "  band " + std::to_string(bandW) + "x" + std::to_string(bandH)
                + " at " + std::to_string(px + bandX) + "," + std::to_string(py)
                + "  (work area " + std::to_string(wa.right - wa.left) + "x"
                + std::to_string(wa.bottom - wa.top) + ", crop from "
                + std::to_string(cropX) + "," + std::to_string(cropY) + ", "
                + std::to_string(CropPad()) + " px pad for the animation)");
            Log("VRMPet: avatar screen position is unchanged: "
                + std::to_string(px + avatarX) + "," + std::to_string(py + avatarY));
            wi::backlog::Flush();

            overlayCreated = true;
            overlayClock = std::chrono::steady_clock::now();
        }

        // The pet says hello, once, as soon as it is actually standing there.
        // Fired here rather than at startup for two reasons: the band exists by
        // now, so the greeting lands in the bubble instead of being dropped, and
        // the avatar is on screen, so the speech has something to come out of.
        if (!greeted)
        {
            greeted = true;
            if (g_askMode)
                Log("VRMPet: not greeting - 'ask' mode runs exactly one scripted turn");
            else
                call.Greet();
        }

        // Render the band, and copy it into the canvas only when it changed. The
        // avatar blit below never touches these rows, which is what keeps the
        // bubble at one composite per turn instead of one per frame.
        if (g_bandVisible)
        {
            if (bubble.Render() || bandNotYetComposited)
            {
                bandNotYetComposited = false;
                for (int row = 0; row < bandH; ++row)
                {
                    const uint8_t* src = bubble.Row(row);
                    if (src == nullptr)
                        break;
                    memcpy(g_overlay.bits + size_t(row) * size_t(g_overlay.w) * 4 + size_t(bandX) * 4,
                           src, size_t(bandW) * 4);
                }
            }
        }
        else if (bandNotYetComposited)
        {
            // Hidden, and this is the frame the toggle arrived on: wipe the band
            // rows to fully transparent once. The avatar blit never writes these
            // rows, so they stay blank until the band is shown again - which
            // reuses the same one-shot flag. bubble.Render() is skipped while
            // hidden, so a repaint that arrives in the meantime is not wasted;
            // showing again repaints the latest text in one frame.
            bandNotYetComposited = false;
            for (int row = 0; row < bandH; ++row)
                memset(g_overlay.bits + size_t(row) * size_t(g_overlay.w) * 4 + size_t(bandX) * 4,
                       0, size_t(bandW) * 4);
        }

        g_overlay.Blit(rgba.data(), w, cropX, cropY, cropW, cropH, avatarX, avatarY);
        g_overlay.Push();

        // The window is sized once and never resized, so re-measure occasionally
        // and complain if the animation has walked the character out of it.
        if (++overlayCheck >= CheckFrames())
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
        if (!(w > 0.0f) || !std::isfinite(w)) w = float(RenderSize());
        if (!(h > 0.0f) || !std::isfinite(h)) h = float(RenderSize());

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

        // The framing distance is fixed by the character's own size, so the whole
        // avatar is always inside the frustum with 1.15 of headroom. avatar.zoom
        // does NOT move the camera: pulling it closer is a true zoom-in, and the
        // head and feet leave the frame - which is the "zoom 2 cuts the pet"
        // bug. Instead the zoom multiplies the render target (RenderSize()), so
        // the same framing is rasterised at more pixels; the avatar is measured
        // as a tight box in those pixels, so it simply comes out larger on screen
        // with nothing cropped.
        const float dist = (radius / tanf(fov * 0.5f)) * 1.15f;

        if (std::fabs(Zoom() - 1.0f) > 0.001f)
        {
            wi::backlog::post("VRMPet: avatar.zoom " + Fmt(Zoom(), 2) + " - render target "
                              + std::to_string(RenderSize()) + " px (base "
                              + std::to_string(WindowSize()) + ", framing distance "
                              + Fmt(dist, 2) + " unchanged)");
        }

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
            bool skipPositional = false;

            // A bare `VRMPet.exe` is the pet, not the measuring instrument. The
            // overlay is what this program is for, so it is the default: no
            // arguments means the avatar goes on the desktop with the idle loop,
            // the look-at solver, the agent behind Ctrl+Shift+V, no window of
            // its own and nothing in the taskbar. Capture mode - the PNGs
            // REPORT.md's measurements come from - still has its whole command
            // line, but it now has to name itself, either with the `capture`
            // keyword or by leading with an out-dir that is not a mode word.
            if (argc <= 1)
            {
                g_overlayMode  = true;
                g_animPath     = "assets/idle_loop.vrma";
                g_lookAtCamera = true;
                g_azimuthDeg   = 180.0f;
                skipPositional = true;   // no arguments left to read
            }
            // Overlay mode has its own argument layout, since it has no output
            // directory and no focus/isolate options.
            //   VRMPet.exe overlay [model.vrm] [azimuth] [msaa] [seconds] [anim.vrma]
            else if (wcscmp(argv[1], L"overlay") == 0)
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
            else if (argc > 1 && wcscmp(argv[1], L"ask") == 0)
            {
                // The overlay, plus one scripted turn with a defined end. This is
                // how a whole turn is verified without a microphone and without
                // watching the screen.
                g_overlayMode  = true;
                g_animPath     = "assets/idle_loop.vrma";
                g_lookAtCamera = true;
                g_azimuthDeg   = 180.0f;
                g_askText      = (argc > 2) ? Narrow(argv[2]) : "";
                g_askMode      = true;
                skipPositional = true;
            }
            else if (wcscmp(argv[1], L"agent") == 0 ||
                     wcscmp(argv[1], L"tts")   == 0 ||
                     wcscmp(argv[1], L"asr")   == 0 ||
                     wcscmp(argv[1], L"bubble") == 0 ||
                     wcscmp(argv[1], L"config") == 0 ||
                     wcscmp(argv[1], L"mic")   == 0)
            {
                g_probeMode = Narrow(argv[1]);
                if (g_probeMode == "mic")
                    g_probeSeconds = (argc > 2) ? _wtoi(argv[2]) : 6;
                else if (argc > 2)
                    g_probeArg = Narrow(argv[2]);
                skipPositional = true;
            }
            else if (wcscmp(argv[1], L"capture") == 0)
            {
                // Spelling the mode out: the same layout as below, one argument
                // to the right, so out-dir is argv[2].
                base = 2;
            }

            if (skipPositional)
            {
                // This mode has its own layout and no positional arguments to read.
            }
            else if (g_overlayMode)
            {
                if (argc > base + 0) g_modelPath   = Narrow(argv[base + 0]);
                if (argc > base + 1) g_azimuthDeg  = float(_wtof(argv[base + 1]));
                if (argc > base + 2) g_msaa        = _wtoi(argv[base + 2]);
                if (argc > base + 3)
                {
                    g_overlaySecs = _wtoi(argv[base + 3]);
                    g_overlaySecsFromCli = true;
                }
                if (argc > base + 4) g_animPath    = Narrow(argv[base + 4]);
            }
            else
            {
                //   VRMPet.exe [capture] <out-dir> [model.vrm] [azimuth] [focus] [isolate] [msaa] [anim.vrma]
                if (argc > base + 0) g_outDir      = Narrow(argv[base + 0]);
                if (argc > base + 1) g_modelPath   = Narrow(argv[base + 1]);
                if (argc > base + 2) g_azimuthDeg  = float(_wtof(argv[base + 2]));
                if (argc > base + 3) g_focusMesh   = _wtoi(argv[base + 3]);
                if (argc > base + 4) g_isolateMesh = _wtoi(argv[base + 4]);
                if (argc > base + 5) g_msaa        = _wtoi(argv[base + 5]);
                if (argc > base + 6) g_animPath    = Narrow(argv[base + 6]);
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
                else if (_wcsicmp(argv[i], L"mute") == 0)
                    g_micEnabled = false;
                else if (_wcsnicmp(argv[i], L"gain=", 5) == 0)
                {
                    g_mouthGain = float(_wtof(argv[i] + 5));
                    g_mouthGainFromCli = true;   // an explicit gain= beats config.txt
                }
            }

            LocalFree(argv);
        }
    }

    // config.txt is read before anything uses it: the probes below draw the band
    // at the configured size, and the window is created at the configured size.
    // Every value the command line did not set comes from the file from here on,
    // and Settings().Load() is a no-op if something already loaded it.
    vrmpet::Settings().Load(ConfigSearchPath());
    if (!g_mouthGainFromCli)
        g_mouthGain = vrmpet::Settings().Float("mouth.gain");
    if (!g_overlaySecsFromCli)
        g_overlaySecs = vrmpet::Settings().Int("overlay.seconds");
    g_overlayMargin = vrmpet::Settings().Int("overlay.margin");
    g_bandVisible = vrmpet::Settings().Flag("band.visible");

    // Every probe runs here: before a window, before WickedEngine, before any
    // shader is compiled. Nothing is presented and nothing is rendered, so the
    // only thing that can have gone wrong is the leg under test.
    if (!g_probeMode.empty())
    {
        const int code = RunProbe("out");
        wi::backlog::Flush();
        return code;
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
            WS_POPUP, 0, 0, RenderSize(), RenderSize(),
            nullptr, nullptr, hInstance, nullptr);
    }
    else
    {
        RECT rc = { 0, 0, RenderSize(), RenderSize() };
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
    {
        UnregisterHotKey(hWnd, 1);
        if (g_overlay.hwnd != nullptr)
        {
            UnregisterHotKey(g_overlay.hwnd, 2);
            UnregisterHotKey(g_overlay.hwnd, 3);
            UnregisterHotKey(g_overlay.hwnd, 4);
        }
        g_overlayHotkey = nullptr;
    }

    wi::jobsystem::ShutDown();
    return (int)msg.wParam;
}
