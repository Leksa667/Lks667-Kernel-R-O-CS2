#pragma once
#include "ui_theme.hpp"
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <windows.h>

namespace dvm::ui {

enum Font {
  FontDisplay,
  FontTitle,
  FontSubtitle,
  FontBody,
  FontBodyStrong,
  FontCaption,
  FontMicro,
  FontIcon,
  FontIconLarge,
  FontCount
};

enum Align { AlignLeft, AlignCenter, AlignRight };

class Renderer {
public:
  bool create_device_independent();
  bool create_target(HWND window, unsigned dpi);
  void discard_target();
  void destroy();
  void resize(unsigned width_px, unsigned height_px);
  void set_dpi(unsigned dpi);

  ID2D1HwndRenderTarget *target() const { return target_; }
  D2D1_SIZE_F size() const;
  bool ready() const { return target_ != nullptr; }

  void begin();
  bool end();

  void fill(const D2D1_RECT_F &r, const D2D1_COLOR_F &c);
  void fill_round(const D2D1_RECT_F &r, float radius, const D2D1_COLOR_F &c);
  void stroke_round(const D2D1_RECT_F &r, float radius, const D2D1_COLOR_F &c,
                    float width = 1.0f);
  void fill_circle(float cx, float cy, float radius, const D2D1_COLOR_F &c);
  void fill_polygon(const std::vector<D2D1_POINT_2F> &points,
                    const D2D1_COLOR_F &c);
  void shadow(const D2D1_RECT_F &r, float radius, float spread, float strength);
  void gradient_round(const D2D1_RECT_F &r, float radius,
                      const D2D1_COLOR_F &from, const D2D1_COLOR_F &to);

  void text(const std::wstring &s, const D2D1_RECT_F &r, Font font,
            const D2D1_COLOR_F &c, Align align = AlignLeft);
  float text_width(const std::wstring &s, Font font) const;
  float text_height(const std::wstring &s, Font font, float width) const;
  void paragraph(const std::wstring &s, const D2D1_RECT_F &r, Font font,
                 const D2D1_COLOR_F &c, Align align = AlignLeft);

  void push_clip(const D2D1_RECT_F &r);
  void pop_clip();

private:
  IDWriteTextFormat *format(Font font, Align align, bool wrap) const;
  IDWriteTextFormat *build(const wchar_t *family, float size,
                           DWRITE_FONT_WEIGHT weight, Align align, bool wrap);
  ID2D1Factory *factory_ = nullptr;
  IDWriteFactory *dwrite_ = nullptr;
  ID2D1HwndRenderTarget *target_ = nullptr;
  ID2D1SolidColorBrush *brush_ = nullptr;
  ID2D1LinearGradientBrush *gradient_ = nullptr;
  IDWriteTextFormat *formats_[FontCount * 3 * 2] = {};
};

namespace icon {
inline constexpr wchar_t machines = 0xE977;
inline constexpr wchar_t session = 0xE945;
inline constexpr wchar_t diagnostics = 0xE9D9;
inline constexpr wchar_t play = 0xE768;
inline constexpr wchar_t stop = 0xE71A;
inline constexpr wchar_t trash = 0xE74D;
inline constexpr wchar_t folder = 0xE8B7;
inline constexpr wchar_t add = 0xE710;
inline constexpr wchar_t unlink = 0xE738;
inline constexpr wchar_t refresh = 0xE72C;
inline constexpr wchar_t check = 0xE73E;
inline constexpr wchar_t warning = 0xE7BA;
inline constexpr wchar_t info = 0xE946;
inline constexpr wchar_t drop = 0xE896;
inline constexpr wchar_t document = 0xE8A5;
inline constexpr wchar_t minimize = 0xE921;
inline constexpr wchar_t maximize = 0xE922;
inline constexpr wchar_t restore = 0xE923;
inline constexpr wchar_t close = 0xE8BB;
} // namespace icon

inline std::wstring glyph(wchar_t code) { return std::wstring(1, code); }

} // namespace dvm::ui
