#include "bubble.h"
#include "config.h"
#include "util.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using namespace D2D1;

namespace vrmpet
{
    namespace
    {
        // #RRGGBB or #RRGGBBAA, which is how config.txt carries a colour. A
        // value that is not one of those falls back to the built-in default and
        // says so, rather than silently painting a bubble black.
        D2D1_COLOR_F FromHex(const std::string& hex)
        {
            std::string s = hex;
            if (!s.empty() && s[0] == '#')
                s.erase(0, 1);
            if (s.size() != 6 && s.size() != 8)
                return ColorF(1.0f, 1.0f, 1.0f, 1.0f);
            for (char c : s)
            {
                if (!std::isxdigit((unsigned char)c))
                    return ColorF(1.0f, 1.0f, 1.0f, 1.0f);
            }
            auto byte = [&](size_t index)
            {
                return float(std::strtol(s.substr(index, 2).c_str(), nullptr, 16)) / 255.0f;
            };
            // Positions, not ordinals: #RRGGBBAA is bytes at 0, 2, 4, 6. Stepping
            // by one overlaps them ("FCFCFFF2" would read FC, CF, FC, CF) and
            // stepping to 3 for the alpha reads the middle of the string - both
            // of which are what the first version of this did, and both of which
            // render a plausible-looking but wrong-coloured bubble.
            return ColorF(byte(0), byte(2), byte(4), s.size() == 8 ? byte(6) : 1.0f);
        }

        D2D1_COLOR_F FillFor(const Bubble::Style& style, const std::string& role)
        {
            if (role == "user")
                return style.userColor;
            if (role == "tool")
                return style.toolColor;
            return style.agentColor;   // agent, and anything a future role adds
        }

        // The colour, back as the file wrote it - so a log line can show what
        // was actually parsed, which is the step a wrong colour hides behind.
        std::string HexOf(D2D1_COLOR_F c)
        {
            char buf[16];
            auto byte = [](float v) { return int(v * 255.0f + 0.5f); };
            snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", byte(c.r), byte(c.g), byte(c.b), byte(c.a));
            return buf;
        }

        D2D1_COLOR_F ColorFromConfig(const std::string& key)
        {
            const std::string raw = Settings().Text(key);
            const D2D1_COLOR_F value = FromHex(raw);
            if (raw == Settings().Default(key))
                return value;   // the built-in default, parsed once
            if (value.r == 1.0f && value.g == 1.0f && value.b == 1.0f && value.a == 1.0f &&
                raw != "#FFFFFFFF")
            {
                Log("VRMPet: config - " + key + " = '" + raw + "' is not #RRGGBB or #RRGGBBAA; using "
                    + Settings().Default(key));
                return FromHex(Settings().Default(key));
            }
            return value;
        }

        template <typename T>
        void SafeRelease(T*& p)
        {
            if (p != nullptr)
            {
                p->Release();
                p = nullptr;
            }
        }

        std::wstring ToWide(const std::string& utf8)
        {
            if (utf8.empty())
                return {};
            const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), nullptr, 0);
            if (len <= 0)
                return {};
            std::wstring out(size_t(len), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), out.data(), len);
            return out;
        }
    }

    Bubble::~Bubble()
    {
        Shutdown();
    }

    // The band's style, from config.txt. Called before the bitmap exists so a
    // metric that is nonsense in the file is reported once, at startup, next to
    // the line that says which file was read.
    void Bubble::LoadStyle()
    {
        const Config& config = Settings();

        style_.padX            = config.Float("band.pad_x");
        style_.padY            = config.Float("band.pad_y");
        style_.radius          = config.Float("band.radius");
        style_.gapBetween      = config.Float("band.line_gap");
        style_.tailRoom        = config.Float("band.tail_room");
        style_.tailRows        = config.Float("band.tail_rows");
        style_.tailBase        = config.Float("band.tail_base");
        style_.fontSize        = config.Float("band.font_size");
        style_.statusFontSize  = config.Float("band.status_font_size");
        style_.statusHeight    = config.Float("band.status_height");
        style_.lineHeight      = config.Float("band.line_height");
        style_.turnsShown      = (std::max)(1, config.Int("band.turns_shown"));
        style_.maxSegments     = (std::max)(style_.turnsShown, config.Int("band.max_segments"));
        style_.historyLines    = (std::min)(style_.turnsShown,
                                            (std::max)(1, config.Int("band.history_lines")));
        style_.fontFamily      = ToWide(config.Text("band.font_family"));
        if (style_.fontFamily.empty())
            style_.fontFamily = L"Segoe UI";

        style_.agentColor  = ColorFromConfig("band.agent_color");
        style_.userColor   = ColorFromConfig("band.user_color");
        style_.toolColor   = ColorFromConfig("band.tool_color");
        style_.textColor   = ColorFromConfig("band.text_color");
        style_.borderColor = ColorFromConfig("band.border_color");
        style_.statusColor = ColorFromConfig("band.status_color");
        style_.meterTrack  = ColorFromConfig("band.mic_track_color");
        style_.meterLive   = ColorFromConfig("band.mic_live_color");
        style_.meterMuted  = ColorFromConfig("band.mic_muted_color");

        // The height is the caller's, but the bubbles inside it are not: a file
        // that asks for more lines than the band can hold would push the status
        // line off the bottom.
        const int capacity = int((float(height_) - style_.statusHeight) / (style_.lineHeight + style_.gapBetween));
        if (capacity > 0 && style_.historyLines > capacity)
        {
            Log("VRMPet: config - band.history_lines " + std::to_string(style_.historyLines)
                + " does not fit a " + std::to_string(height_) + " px band; using "
                + std::to_string(capacity));
            style_.historyLines = capacity;
        }
        historyLines_ = style_.historyLines;

        Log("VRMPet: band style - font '" + Narrow(style_.fontFamily) + "' "
            + Fmt(style_.fontSize, 1) + "/" + Fmt(style_.statusFontSize, 1)
            + ", pad " + Fmt(style_.padX, 0) + "x" + Fmt(style_.padY, 0)
            + ", radius " + Fmt(style_.radius, 0)
            + "; colors agent " + HexOf(style_.agentColor) + " user " + HexOf(style_.userColor)
            + " text " + HexOf(style_.textColor) + " meter " + HexOf(style_.meterLive));
    }

    bool Bubble::Create(int width, int height)
    {
        Shutdown();

        width_ = width;
        height_ = height;
        LoadStyle();

        // A DC to allocate against. The window is never shown and never used for
        // anything but this; CreateCompatibleDC against the screen is enough and
        // avoids needing a real window at all.
        HDC screen = GetDC(nullptr);
        dc_ = CreateCompatibleDC(screen);

        BITMAPINFO info = {};
        info.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth       = width;
        info.bmiHeader.biHeight      = -height;   // top-down, matching the overlay DIB
        info.bmiHeader.biPlanes      = 1;
        info.bmiHeader.biBitCount    = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap_ = CreateDIBSection(screen, &info, DIB_RGB_COLORS, (void**)&bits_, nullptr, 0);
        ReleaseDC(nullptr, screen);

        if (dc_ == nullptr || bitmap_ == nullptr || bits_ == nullptr)
        {
            Log("VRMPet: bubble - could not create the band bitmap");
            Shutdown();
            return false;
        }
        SelectObject(dc_, bitmap_);
        stride_ = width * 4;
        bottomUp_ = false;

        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_);
        if (FAILED(hr) || d2d_ == nullptr)
        {
            Log("VRMPet: bubble - D2D1CreateFactory failed (0x" + std::to_string((unsigned)hr) + ")");
            Shutdown();
            return false;
        }

        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(&dwrite_));
        if (FAILED(hr) || dwrite_ == nullptr)
        {
            Log("VRMPet: bubble - DWriteCreateFactory failed (0x" + std::to_string((unsigned)hr) + ")");
            Shutdown();
            return false;
        }

        // PREMULTIPLIED is not optional: Blit() already premultiplies the avatar
        // for AC_SRC_ALPHA, and D2D has to match it or the two antialiased edges
        // disagree in opposite directions and the bubble looks burnt.
        const D2D1_RENDER_TARGET_PROPERTIES props = RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);

        hr = d2d_->CreateDCRenderTarget(&props, &target_);
        if (FAILED(hr) || target_ == nullptr)
        {
            Log("VRMPet: bubble - CreateDCRenderTarget failed (0x" + std::to_string((unsigned)hr) + ")");
            Shutdown();
            return false;
        }
        target_->SetDpi(96.0f, 96.0f);   // 1 DIP == 1 device pixel, so layout is in pixels

        auto makeFormat = [&](float size, IDWriteTextFormat** out) -> bool
        {
            HRESULT r = dwrite_->CreateTextFormat(style_.fontFamily.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                  size, L"", out);
            if (FAILED(r) || *out == nullptr)
                return false;
            (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            return true;
        };

        if (!makeFormat(style_.fontSize, &format_) || !makeFormat(style_.statusFontSize, &formatSmall_))
        {
            Log("VRMPet: bubble - could not create the text formats");
            Shutdown();
            return false;
        }

        ready_ = true;
        dirty_ = true;
        Log("VRMPet: bubble band " + std::to_string(width_) + "x" + std::to_string(height_)
            + " (Direct2D DC render target, premultiplied)");
        return true;
    }

    void Bubble::Shutdown()
    {
        SafeRelease(target_);
        SafeRelease(format_);
        SafeRelease(formatSmall_);
        SafeRelease(dwrite_);
        SafeRelease(d2d_);
        if (bitmap_ != nullptr)
            DeleteObject(bitmap_);
        if (dc_ != nullptr)
            DeleteDC(dc_);
        bitmap_ = nullptr;
        dc_ = nullptr;
        bits_ = nullptr;
        ready_ = false;
    }

    void Bubble::OnTranscript(uint32_t segmentId, const std::string& role,
                              const std::string& text, bool final)
    {
        for (BubbleTurn& turn : turns_)
        {
            if (turn.segmentId == segmentId)
            {
                turn.role = role;
                turn.text = text;
                turn.final = final;
                dirty_ = true;
                return;
            }
        }

        BubbleTurn turn;
        turn.segmentId = segmentId;
        turn.role = role;
        turn.text = text;
        turn.final = final;
        turns_.insert(turns_.begin(), turn);      // newest on top
        if (int(turns_.size()) > style_.maxSegments)
            turns_.resize(size_t(style_.maxSegments));
        dirty_ = true;
    }

    void Bubble::SetStatus(const std::string& status)
    {
        if (status == status_)
            return;
        status_ = status;
        dirty_ = true;
    }

    void Bubble::SetLevel(float level, bool micOpen)
    {
        // 1/60th of the width per frame is imperceptible; a redraw per frame
        // would cost a full re-layout of every line for a 4-pixel bar.
        const float clamped = std::clamp(level, 0.0f, 1.0f);
        if (std::abs(clamped - level_) < 0.02f && micOpen == micOpen_)
            return;
        level_ = clamped;
        micOpen_ = micOpen;
        dirty_ = true;
    }

    void Bubble::SetHistoryLines(int lines)
    {
        const int clamped = std::clamp(lines, 1, style_.turnsShown);
        if (clamped == historyLines_)
            return;
        historyLines_ = clamped;
        dirty_ = true;
    }

    void Bubble::Clear()
    {
        turns_.clear();
        dirty_ = true;
    }

    const uint8_t* Bubble::Row(int y) const
    {
        if (bits_ == nullptr || y < 0 || y >= height_)
            return nullptr;
        if (bottomUp_)
            return bits_ + size_t(height_ - 1 - y) * size_t(stride_);
        return bits_ + size_t(y) * size_t(stride_);
    }

    bool Bubble::Render()
    {
        if (!ready_)
            return false;
        if (!dirty_)
            return false;

        RECT band = { 0, 0, width_, height_ };
        target_->BindDC(dc_, &band);

        target_->BeginDraw();
        target_->Clear(ColorF(0.0f, 0.0f));   // fully transparent: the desktop shows through

        ID2D1SolidColorBrush* brush = nullptr;
        target_->CreateSolidColorBrush(ColorF(1, 1, 1, 1), &brush);
        if (brush == nullptr)
        {
            target_->EndDraw();
            return false;
        }

        const float maxTextWidth = float(width_) - 2.0f * style_.padX - style_.tailRoom;
        float y = 6.0f;
        const int visible = std::min<int>(historyLines_, int(turns_.size()));
        float lastBubbleLeft = 0.0f, lastBubbleBottom = 0.0f, lastBubbleRight = 0.0f;
        D2D1_COLOR_F lastFill = style_.agentColor;

        for (int i = 0; i < visible; ++i)
        {
            const BubbleTurn& turn = turns_[size_t(i)];
            const std::wstring wide = ToWide(turn.text);
            if (wide.empty())
                continue;

            IDWriteTextLayout* layout = nullptr;
            if (FAILED(dwrite_->CreateTextLayout(wide.c_str(), UINT32(wide.size()), format_,
                                                 maxTextWidth, 400.0f, &layout)) || layout == nullptr)
                continue;

            DWRITE_TEXT_METRICS metrics = {};
            layout->GetMetrics(&metrics);

            const float bubbleW = (std::min)(maxTextWidth, metrics.width) + 2.0f * style_.padX;
            const float bubbleH = metrics.height + 2.0f * style_.padY;
            const float left = float(width_) - style_.tailRoom - bubbleW;
            const D2D1_RECT_F rect = RectF(left, y, left + bubbleW, y + bubbleH);

            const D2D1_COLOR_F fill = FillFor(style_, turn.role);
            brush->SetColor(fill);
            target_->FillRoundedRectangle(RoundedRect(rect, style_.radius, style_.radius), brush);

            // Straight alpha: D2D converts to the target's premultiplied alpha
            // itself. Multiplying by alpha here as well - which is what the
            // overlay's own BGRA write has to do - would darken every edge twice.
            brush->SetColor(style_.borderColor);
            target_->DrawRoundedRectangle(RoundedRect(rect, style_.radius, style_.radius), brush, 1.0f);

            brush->SetColor(style_.textColor);
            target_->DrawTextLayout(Point2F(left + style_.padX, y + style_.padY), layout, brush);

            layout->Release();

            lastBubbleLeft = left;
            lastBubbleRight = left + bubbleW;
            lastBubbleBottom = y + bubbleH;
            lastFill = fill;
            y += bubbleH + style_.gapBetween;
        }

        // The tail, on the bottom-most bubble, pointing down-right at the head.
        //
        // It is rasterised row by row out of FillRectangle rather than drawn as
        // path geometry. A filled ID2D1PathGeometry on this DC render target
        // draws nothing at all - verified with the bubble probe, which counts the
        // opaque pixels of every row band in the written BMP, not by eye - while
        // the primitive fills in the same transform are all present. Fourteen
        // one-pixel-tall rectangles once per turn is a cheaper answer than
        // reverse-engineering Direct2D's DC target, and it is exact.
        if (visible > 0)
        {
            const int   tailRows = int(style_.tailRows);
            const float tailBase = style_.tailBase;
            // Under the bottom-right of the last bubble, because that is the
            // direction the avatar's head is in.
            const float baseX = (std::max)(lastBubbleLeft + 30.0f, lastBubbleRight - 62.0f);
            const float baseY = lastBubbleBottom - 1.0f;
            brush->SetColor(lastFill);
            for (int i = 0; i < tailRows; ++i)
            {
                const float t = float(i) / float(tailRows);
                target_->FillRectangle(RectF(baseX + t * 8.0f, baseY + float(i),
                                             baseX + t * 8.0f + tailBase * (1.0f - t) + 4.0f,
                                             baseY + float(i) + 1.0f), brush);
            }
        }

        // Status line and the mic meter, pinned to the bottom of the band.
        {
            const float statusY = float(height_) - style_.statusHeight;
            const std::wstring status = ToWide(status_);
            IDWriteTextLayout* layout = nullptr;
            if (!status.empty() &&
                SUCCEEDED(dwrite_->CreateTextLayout(status.c_str(), UINT32(status.size()),
                                                    formatSmall_, float(width_) - 2.0f * style_.padX, 20.0f,
                                                    &layout)) && layout != nullptr)
            {
                brush->SetColor(style_.statusColor);
                target_->DrawTextLayout(Point2F(style_.padX, statusY), layout, brush);
                layout->Release();
            }

            // A 60 px meter: the leading edge is the level, so "is it hearing
            // me" is answerable without a second window or a log.
            const float meterY = float(height_) - 8.0f;
            const float meterW = 60.0f;
            brush->SetColor(style_.meterTrack);
            target_->FillRectangle(RectF(style_.padX, meterY, style_.padX + meterW, meterY + 4.0f), brush);
            brush->SetColor(micOpen_ ? style_.meterLive : style_.meterMuted);
            target_->FillRectangle(RectF(style_.padX, meterY, style_.padX + meterW * level_, meterY + 4.0f), brush);
        }

        const HRESULT hr = target_->EndDraw();
        brush->Release();

        if (FAILED(hr))
        {
            Log("VRMPet: bubble - EndDraw failed (0x" + std::to_string((unsigned)hr)
                + "); the band will be rebuilt on the next change");
            return false;
        }

        dirty_ = false;
        lastDrawnStatus_ = status_;
        lastDrawnTurns_ = turns_.size();
        return true;
    }

    bool Bubble::SaveBitmap(const std::string& path)
    {
        if (bits_ == nullptr || width_ <= 0 || height_ <= 0)
            return false;

        const uint32_t rowBytes = uint32_t(width_) * 4;
        const uint32_t imageBytes = rowBytes * uint32_t(height_);

        BITMAPFILEHEADER file = {};
        file.bfType = 0x4D42;   // "BM"
        file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        file.bfSize = file.bfOffBits + imageBytes;

        BITMAPINFOHEADER info = {};
        info.biSize = sizeof(BITMAPINFOHEADER);
        info.biWidth = width_;
        info.biHeight = height_;    // positive: BMP rows are stored bottom-up
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biCompression = BI_RGB;
        info.biSizeImage = imageBytes;

        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (f == nullptr)
            return false;

        std::fwrite(&file, sizeof(file), 1, f);
        std::fwrite(&info, sizeof(info), 1, f);
        for (int y = height_ - 1; y >= 0; --y)
            std::fwrite(Row(y), rowBytes, 1, f);

        std::fclose(f);
        return true;
    }
}
