/*
 * DjMM
 * v0.1
 *
 * Copyright (c) 2011, David J. Rager
 * djrager@fourthwoods.com
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * dj_input.c
 *
 *  Created on: Jan 1, 2012
 *      Author: David J. Rager
 */

#include "dj_debug.h"
#include "dj_input.h"

#include "SDL3/sdl.h"

unsigned int dji_init()
{
	SDL_ResetKeyboard();

	return 1;
}

unsigned int dji_poll() {
	SDL_Event event;

	return SDL_PollEvent(&event);
}

unsigned int dji_get_keyboard_state(unsigned char* keystate, unsigned int len)
{
	void SDL_PumpEvents(void);

	int numkeys;
	const Uint8* keys = SDL_GetKeyboardState(&numkeys);

	for (unsigned int i = 0; i < len; i++) {
		keystate[i] = keys[i] ? 0x80 : 0;
	}

	return 1;
}

unsigned int dji_get_mouse_state(struct mouse_state* mouse)
{
	//mouse->x = ms.lX;
	//mouse->y = ms.lY;
	//mouse->b1 = ms.rgbButtons[0];
	//mouse->b2 = ms.rgbButtons[1];
	//mouse->b3 = ms.rgbButtons[2];
	//mouse->b4 = ms.rgbButtons[3];

	return 1;
}

unsigned int dji_get_joystick_state(struct joystick_state* joystick)
{
//	DJ_TRACE("lX %d, lY %d\n", js.lX - 32767, js.lY - 32511);
//	DJ_TRACE("Buttons[0] %02x, Buttons[1] %02x, Buttons[2] %02x, Buttons[3] %02x\n", js.rgbButtons[0], js.rgbButtons[1], js.rgbButtons[2], js.rgbButtons[3]);
//	DJ_TRACE("Buttons[4] %02x, Buttons[5] %02x, Buttons[6] %02x, Buttons[7] %02x\n", js.rgbButtons[4], js.rgbButtons[5], js.rgbButtons[6], js.rgbButtons[7]);
//	DJ_TRACE("Buttons[8] %02x, Buttons[9] %02x, Buttons[10] %02x, Buttons[11] %02x\n", js.rgbButtons[8], js.rgbButtons[9], js.rgbButtons[10], js.rgbButtons[11]);
//	DJ_TRACE("POV[0] %d\n", js.rgdwPOV[0]);

	//joystick->x = js.lX - 32767;
	//joystick->y = js.lY - 32511;
	//joystick->dpad = js.rgdwPOV[0];
	//joystick->b1 = js.rgbButtons[0];
	//joystick->b2 = js.rgbButtons[1];
	//joystick->b3 = js.rgbButtons[2];
	//joystick->b4 = js.rgbButtons[3];

	return 1;
}

unsigned int dji_shutdown()
{
	return 1;
}



