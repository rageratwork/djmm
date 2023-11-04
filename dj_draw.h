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
 * dj_draw.h
 *
 *  Created on: Jan 1, 2012
 *      Author: David J. Rager
 */

#ifndef DJ_DRAW_H_
#define DJ_DRAW_H_

#include "dj_types.h"

#ifdef __cplusplus
extern "C" {
#endif

unsigned int djd_init(const char* title, unsigned int screen_width, unsigned int screen_height, unsigned int bpp, boolean fullScreen);
unsigned int djd_shutdown();

unsigned int djd_setpalette(unsigned char* bytes);
unsigned int djd_draw(unsigned char* src, unsigned int width, unsigned int height);

#ifdef __cplusplus
}
#endif

#endif /* DJ_DRAW_H_ */
