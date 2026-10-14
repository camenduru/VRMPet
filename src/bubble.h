// ---------------------------------------------------------------------------
//  vrmpet::bubble - the transcript, drawn where the avatar is
//
//  The text goes into the overlay's bitmap, not into the engine's frame, and
//  that is a design constraint rather than a preference: ComputeCropBox() and
//  CheckCropBox() measure the *engine's* alpha channel to find the character, so
//  a bubble drawn into that frame would be counted as the character, the crop
//  box would balloon to the whole window, and the margin report that warns about
//  clipping would become meaningless. Keeping the bubble in the DIB keeps the
//  two in separate address spaces.
//
//  Plain GDI text is the other trap: DrawText/ExtTextOut onto a 32-bpp DIB do
//  not write meaningful alpha, and UpdateLayeredWindow requires premultiplied
//  BGRA. Direct2D with a premultiplied DC render target blends in premultiplied
//  space and matches what Blit() already produces for the avatar.
//
//  The band is a fixed, reserved rectangle - the window does not resize when a
//  reply arrives, exactly as kOverlayPad does not resize it when the idle loop
//  sways. The text grows inside it.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vrmpet
{
    struct BubbleTurn
    {
        uint32_t    segmentId = 0;
        std::string role;      // "user" | "agent" | "tool"
        std::string text;
        bool        final = false;
    };

    class Bubble
    {
    public:
        ~Bubble();

        // Creates the band bitmap and the Direct2D/DirectWrite stack. Needs COM
        // on the calling thread (apartment threaded is fine).
        bool Create(int width, int height);
        void Shutdown();
        bool Ready() const { return ready_; }

        // Newest turn on top, keyed by segment: the same id replaces the text in
        // the bubble it already has, a new id adds one. Appending instead is
        // what makes a streaming reply look like it is stuttering.
        void OnTranscript(uint32_t segmentId, const std::string& role,
                          const std::string& text, bool final);

        void SetStatus(const std::string& status);
        void SetLevel(float level, bool micOpen);
        void SetHistoryLines(int lines);          // 1..kTurnsShown
        int  HistoryLines() const { return historyLines_; }
        void Clear();

        int  Width() const { return width_; }
        int  Height() const { return height_; }

        // Row 0 is the top of the band, whichever way the bitmap is stored.
        // Premultiplied BGRA.
        const uint8_t* Row(int y) const;

        bool Render();                            // redraws only when dirty
        bool SaveBitmap(const std::string& path); // a real BMP, for eyeballing

        static constexpr int kTurnsShown = 4;
        static constexpr int kMaxSegments = 12;

    private:
        ID2D1Factory*          d2d_ = nullptr;
        IDWriteFactory*        dwrite_ = nullptr;
        ID2D1DCRenderTarget*   target_ = nullptr;
        IDWriteTextFormat*     format_ = nullptr;
        IDWriteTextFormat*     formatSmall_ = nullptr;

        HWND      hwnd_ = nullptr;      // never shown; only for a DC to allocate against
        HDC       dc_ = nullptr;
        HBITMAP   bitmap_ = nullptr;
        uint8_t*  bits_ = nullptr;
        int       stride_ = 0;
        bool      bottomUp_ = false;

        std::vector<BubbleTurn> turns_;   // newest first
        std::string status_ = "starting up";
        float       level_ = 0.0f;
        bool        micOpen_ = false;
        int         historyLines_ = 3;

        bool dirty_ = true;
        bool ready_ = false;
        std::string lastDrawnStatus_;
        size_t      lastDrawnTurns_ = size_t(-1);

        int width_ = 0;
        int height_ = 0;
    };
}
