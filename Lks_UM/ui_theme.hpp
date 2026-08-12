#pragma once
#include <cmath>
#include <d2d1.h>

namespace dvm::theme {

constexpr D2D1_COLOR_F hex(unsigned v, float a = 1.0f) {
  return {static_cast<float>((v >> 16) & 0xFF) / 255.0f,
          static_cast<float>((v >> 8) & 0xFF) / 255.0f,
          static_cast<float>(v & 0xFF) / 255.0f, a};
}

constexpr D2D1_COLOR_F alpha(D2D1_COLOR_F c, float a) {
  return {c.r, c.g, c.b, a};
}

constexpr D2D1_COLOR_F mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
          a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

inline constexpr auto app = hex(0x0A0C11);
inline constexpr auto rail = hex(0x0E1219);
inline constexpr auto canvas = hex(0x10141B);
inline constexpr auto surface = hex(0x161C25);
inline constexpr auto surface_hover = hex(0x1D2531);
inline constexpr auto surface_active = hex(0x232D3C);
inline constexpr auto elevated = hex(0x28323F);
inline constexpr auto stroke = hex(0x222B37);
inline constexpr auto stroke_strong = hex(0x33404F);

inline constexpr auto text_hi = hex(0xF3F6FB);
inline constexpr auto text = hex(0xBFC8D6);
inline constexpr auto text_dim = hex(0x7A8698);
inline constexpr auto text_faint = hex(0x56606F);

inline constexpr auto accent = hex(0x1ED760);
inline constexpr auto accent_hi = hex(0x4DEA88);
inline constexpr auto accent_ink = hex(0x04220E);
inline constexpr auto danger = hex(0xED4245);
inline constexpr auto danger_hi = hex(0xF25E61);
inline constexpr auto warn = hex(0xF0A32E);
inline constexpr auto info = hex(0x5865F2);

inline constexpr auto vmware = hex(0xF0A32E);
inline constexpr auto vbox = hex(0x2F86EB);
inline constexpr auto ldplayer = hex(0xA855F7);

inline constexpr float r_sm = 6.0f;
inline constexpr float r_md = 10.0f;
inline constexpr float r_lg = 16.0f;
inline constexpr float r_xl = 22.0f;

inline constexpr float rail_w = 236.0f;
inline constexpr float caption_h = 40.0f;
inline constexpr float footer_h = 46.0f;
inline constexpr float pad = 28.0f;
inline constexpr float card_h = 86.0f;
inline constexpr float gap = 12.0f;

inline float ease_out(float t) { return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); }

struct Anim {
  float value = 0.0f;
  float target = 0.0f;
};

inline bool advance(Anim &a, float dt, float speed = 16.0f) {
  const float delta = a.target - a.value;
  if (std::fabs(delta) < 0.0015f) {
    if (a.value != a.target) {
      a.value = a.target;
      return true;
    }
    return false;
  }
  a.value += delta * (1.0f - std::exp(-speed * dt));
  return true;
}

} // namespace dvm::theme
