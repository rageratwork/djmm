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
 * dx_draw.h
 *
 *  Created on: Jan 1, 2012
 *      Author: David J. Rager
 */

#ifndef DX_DRAW_H_
#define DX_DRAW_H_

#ifdef __cplusplus
extern "C" {
#endif

unsigned int dxd_init(HINSTANCE hinstance, HWND hwnd, UINT screen_width, UINT screen_height, UINT bpp, BOOL fullScreen);
unsigned int dxd_shutdown();

unsigned int dxd_setpalette(unsigned char* bytes);
unsigned int dxd_draw(unsigned char* src, UINT width, UINT height);

#ifdef __cplusplus
}
#endif

#endif /* DX_DRAW_H_ */
