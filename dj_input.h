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
 * dj_input.h
 *
 *  Created on: Jan 1, 2012
 *      Author: David J. Rager
 */

#ifndef DJ_INPUT_H_
#define DJ_INPUT_H_

#include "dj_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KEY_DOWN(key) (((key) & 0x80) ? 1 : 0)
#define KEY_UP(key)   (((key) & 0x80) ? 0 : 1)

#define MOUSE_DOWN(btn) (((btn) & 0x80) ? 1 : 0)
#define MOUSE_UP(btn)   (((btn) & 0x80) ? 0 : 1)

#define JOYBTN_DOWN(key) (((key) & 0x80) ? 1 : 0)
#define JOYBTN_UP(key)   (((key) & 0x80) ? 0 : 1)

struct mouse_state {
	int x;
	int y;
	unsigned int b1;
	unsigned int b2;
	unsigned int b3;
	unsigned int b4;
};

struct joystick_state {
	int x;
	int y;
	int dpad;
	unsigned int b1;
	unsigned int b2;
	unsigned int b3;
	unsigned int b4;
};

unsigned int dji_init();
unsigned int dji_poll();
unsigned int dji_get_keyboard_state(unsigned char* keystate, unsigned int len);

unsigned int dji_get_mouse_state(struct mouse_state* mouse);
unsigned int dji_get_joystick_state(struct joystick_state* joystick);

unsigned int dji_shutdown();

#ifdef __cplusplus
}
#endif

#endif /* DJ_INPUT_H_ */
