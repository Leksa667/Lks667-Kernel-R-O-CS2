#include "ui_render.hpp"
#include <algorithm>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace dvm::ui {
namespace {

template <class T> void release(T *&p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

struct FontSpec {
  int family;
  float size;
  DWRITE_FONT_WEIGHT weight;
};

constexpr FontSpec kFonts[FontCount] = {
    {0, 27.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD},
    {0, 19.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD},
    {1, 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD},
    {1, 14.0f, DWRITE_FONT_WEIGHT_NORMAL},
    {1, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD},
    {1, 12.5f, DWRITE_FONT_WEIGHT_NORMAL},
    {1, 11.0f, DWRITE_FONT_WEIGHT_BOLD},
    {2, 16.0f, DWRITE_FONT_WEIGHT_NORMAL},
    {2, 22.0f, DWRITE_FONT_WEIGHT_NORMAL},
};

const wchar_t *available(IDWriteFactory *dwrite,
                        std::initializer_list<const wchar_t *> names) {
  IDWriteFontCollection *collection = nullptr;
  if (FAILED(dwrite->GetSystemFontCollection(&collection)) || !collection)
    return *(names.end() - 1);
  const wchar_t *found = *(names.end() - 1);
  for (const wchar_t *name : names) {
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (SUCCEEDED(collection->FindFamilyName(name, &index, &exists)) && exists) {
      found = name;
      break;
    }
  }
  collection->Release();
  return found;
}

} // namespace

bool Renderer::create_device_independent() {
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory_)))
    return false;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                 __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown **>(&dwrite_))))
    return false;
  const wchar_t *families[3] = {
      available(dwrite_, {L"Segoe UI Variable Display", L"Segoe UI"}),
      available(dwrite_, {L"Segoe UI Variable Text", L"Segoe UI"}),
      available(dwrite_, {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"})};
  for (int f = 0; f < FontCount; ++f)
    for (int a = 0; a < 3; ++a)
      for (int w = 0; w < 2; ++w)
        formats_[(f * 3 + a) * 2 + w] =
            build(families[kFonts[f].family], kFonts[f].size, kFonts[f].weight,
                  static_cast<Align>(a), w != 0);
  return true;
}

IDWriteTextFormat *Renderer::build(const wchar_t *family, float size,
                                   DWRITE_FONT_WEIGHT weight, Align align,
                                   bool wrap) {
  IDWriteTextFormat *format = nullptr;
  if (FAILED(dwrite_->CreateTextFormat(family, nullptr, weight,
                                       DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, size, L"",
                                       &format)))
    return nullptr;
  format->SetTextAlignment(align == AlignLeft     ? DWRITE_TEXT_ALIGNMENT_LEADING
                           : align == AlignCenter ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                  : DWRITE_TEXT_ALIGNMENT_TRAILING);
  if (wrap) {
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  } else {
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    IDWriteInlineObject *sign = nullptr;
    if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(format, &sign))) {
      DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
      format->SetTrimming(&trimming, sign);
      sign->Release();
    }
  }
  return format;
}

IDWriteTextFormat *Renderer::format(Font font, Align align, bool wrap) const {
  return formats_[(static_cast<int>(font) * 3 + static_cast<int>(align)) * 2 +
                  (wrap ? 1 : 0)];
}

bool Renderer::create_target(HWND window, unsigned dpi) {
  if (target_)
    return true;
  RECT rc{};
  GetClientRect(window, &rc);
  const D2D1_SIZE_U px = D2D1::SizeU(
      static_cast<UINT32>(std::max<LONG>(rc.right - rc.left, 1)),
      static_cast<UINT32>(std::max<LONG>(rc.bottom - rc.top, 1)));
  auto properties = D2D1::RenderTargetProperties();
  properties.pixelFormat =
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
  auto hwnd_properties = D2D1::HwndRenderTargetProperties(window, px);
  hwnd_properties.presentOptions = D2D1_PRESENT_OPTIONS_NONE;
  if (FAILED(factory_->CreateHwndRenderTarget(properties, hwnd_properties,
                                              &target_)))
    return false;
  target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  set_dpi(dpi);
  if (FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black),
                                            &brush_))) {
    discard_target();
    return false;
  }
  return true;
}

void Renderer::discard_target() {
  release(brush_);
  release(gradient_);
  release(target_);
}

void Renderer::destroy() {
  discard_target();
  for (auto &format : formats_)
    release(format);
  release(dwrite_);
  release(factory_);
}

void Renderer::resize(unsigned width_px, unsigned height_px) {
  if (target_)
    target_->Resize(D2D1::SizeU(std::max(width_px, 1u), std::max(height_px, 1u)));
}

void Renderer::set_dpi(unsigned dpi) {
  if (target_) {
    const float value = static_cast<float>(dpi);
    target_->SetDpi(value, value);
  }
}

D2D1_SIZE_F Renderer::size() const {
  return target_ ? target_->GetSize() : D2D1::SizeF(0, 0);
}

void Renderer::begin() {
  target_->BeginDraw();
  target_->Clear(theme::app);
}

bool Renderer::end() {
  const HRESULT hr = target_->EndDraw();
  if (hr == D2DERR_RECREATE_TARGET) {
    discard_target();
    return false;
  }
  return true;
}

void Renderer::fill(const D2D1_RECT_F &r, const D2D1_COLOR_F &c) {
  brush_->SetColor(c);
  target_->FillRectangle(r, brush_);
}

void Renderer::fill_round(const D2D1_RECT_F &r, float radius,
                          const D2D1_COLOR_F &c) {
  brush_->SetColor(c);
  target_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_);
}

void Renderer::stroke_round(const D2D1_RECT_F &r, float radius,
                            const D2D1_COLOR_F &c, float width) {
  brush_->SetColor(c);
  const float inset = width * 0.5f;
  const D2D1_RECT_F inner{r.left + inset, r.top + inset, r.right - inset,
                          r.bottom - inset};
  target_->DrawRoundedRectangle(D2D1::RoundedRect(inner, radius, radius),
                                brush_, width);
}

void Renderer::fill_circle(float cx, float cy, float radius,
                           const D2D1_COLOR_F &c) {
  brush_->SetColor(c);
  target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius),
                       brush_);
}

void Renderer::fill_polygon(const std::vector<D2D1_POINT_2F> &points,
                            const D2D1_COLOR_F &c) {
  if (!target_ || !factory_ || points.size() < 3)
    return;
  ID2D1PathGeometry *geometry = nullptr;
  ID2D1GeometrySink *sink = nullptr;
  if (FAILED(factory_->CreatePathGeometry(&geometry)) ||
      FAILED(geometry->Open(&sink))) {
    if (geometry) geometry->Release();
    return;
  }
  sink->BeginFigure(points.front(), D2D1_FIGURE_BEGIN_FILLED);
  sink->AddLines(points.data() + 1, static_cast<UINT32>(points.size() - 1));
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);
  sink->Close();
  sink->Release();
  brush_->SetColor(c);
  target_->FillGeometry(geometry, brush_);
  geometry->Release();
}

void Renderer::shadow(const D2D1_RECT_F &r, float radius, float spread,
                      float strength) {
  const int layers = 7;
  for (int i = layers; i >= 1; --i) {
    const float grow = spread * static_cast<float>(i) / layers;
    const D2D1_RECT_F band{r.left - grow, r.top - grow * 0.35f, r.right + grow,
                           r.bottom + grow * 0.9f};
    fill_round(band, radius + grow, theme::alpha(theme::hex(0x000000),
                                                 strength / layers));
  }
}

void Renderer::gradient_round(const D2D1_RECT_F &r, float radius,
                              const D2D1_COLOR_F &from, const D2D1_COLOR_F &to) {
  D2D1_GRADIENT_STOP stops[2] = {{0.0f, from}, {1.0f, to}};
  ID2D1GradientStopCollection *collection = nullptr;
  if (FAILED(target_->CreateGradientStopCollection(stops, 2, &collection))) {
    fill_round(r, radius, from);
    return;
  }
  ID2D1LinearGradientBrush *brush = nullptr;
  const auto properties = D2D1::LinearGradientBrushProperties(
      D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.bottom));
  if (SUCCEEDED(target_->CreateLinearGradientBrush(properties, collection,
                                                   &brush))) {
    target_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush);
    brush->Release();
  }
  collection->Release();
}

void Renderer::text(const std::wstring &s, const D2D1_RECT_F &r, Font font,
                    const D2D1_COLOR_F &c, Align align) {
  IDWriteTextFormat *f = format(font, align, false);
  if (!f || s.empty())
    return;
  brush_->SetColor(c);
  target_->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), f, r, brush_,
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Renderer::paragraph(const std::wstring &s, const D2D1_RECT_F &r, Font font,
                         const D2D1_COLOR_F &c, Align align) {
  IDWriteTextFormat *f = format(font, align, true);
  if (!f || s.empty())
    return;
  brush_->SetColor(c);
  target_->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), f, r, brush_,
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Renderer::text_width(const std::wstring &s, Font font) const {
  IDWriteTextFormat *f = format(font, AlignLeft, true);
  if (!f || s.empty())
    return 0.0f;
  IDWriteTextLayout *layout = nullptr;
  if (FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()),
                                       f, 4096.0f, 128.0f, &layout)))
    return 0.0f;
  DWRITE_TEXT_METRICS metrics{};
  layout->GetMetrics(&metrics);
  layout->Release();
  return metrics.widthIncludingTrailingWhitespace;
}

float Renderer::text_height(const std::wstring &s, Font font,
                            float width) const {
  IDWriteTextFormat *f = format(font, AlignLeft, true);
  if (!f || s.empty())
    return 0.0f;
  IDWriteTextLayout *layout = nullptr;
  if (FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()),
                                       f, std::max(width, 1.0f), 4096.0f,
                                       &layout)))
    return 0.0f;
  DWRITE_TEXT_METRICS metrics{};
  layout->GetMetrics(&metrics);
  layout->Release();
  return metrics.height;
}

void Renderer::push_clip(const D2D1_RECT_F &r) {
  target_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
}

void Renderer::pop_clip() { target_->PopAxisAlignedClip(); }

} // namespace dvm::ui
