#pragma once
#include <windows.h>

namespace lks::modern_ui {
bool initialize(HWND window);
void shutdown();
void resize(unsigned width, unsigned height);
void paint();
void mouse_move(int x, int y);
void mouse_leave();
void mouse_down(int x, int y);
void mouse_up(int x, int y);
void wheel(short delta);
bool hit_caption_button(int x, int y);
}
