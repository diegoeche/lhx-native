#pragma once
#include <SDL.h>
#include <stdint.h>
#include "controls.h"

typedef void (*LhxPadKey)(unsigned scan, int release);
void lhx_gamepad_configure(const LhxControls *);
void lhx_gamepad_init(LhxPadKey);
void lhx_gamepad_event(const SDL_Event *);
void lhx_gamepad_poll(uint64_t now_us);
void lhx_gamepad_focus(int focused);
void lhx_gamepad_close(void);
void lhx_gamepad_list(void);
