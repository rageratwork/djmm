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
 * dj_draw.c
 *
 *  Created on: Jan 1, 2012
 *      Author: David J. Rager
 */

#include <stdio.h>

#include "dj_types.h"
#include "SDL3/sdl.h"


static SDL_Window* window = NULL;
static SDL_Renderer* renderer = NULL;
static SDL_Texture* texture = NULL;

SDL_Surface* surface = NULL;

unsigned int djd_init(const char* title, unsigned int screen_width, unsigned int screen_height, unsigned int bpp, boolean fullscreen)
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
		printf("Failed to init SDL. SDL_Error: %s\n", SDL_GetError());
		return 0;
	}

	Uint32 flags = 0;
	if (fullscreen) {
		flags |= SDL_WINDOW_FULLSCREEN;
	}

	window = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, screen_width, screen_height, SDL_WINDOW_RESIZABLE);
	if (window == NULL) {
		printf("Failed to create window. SDL_Error: %s\n", SDL_GetError());
		SDL_Quit();
		return 0;
	}

	for(int i = 0; i < SDL_GetNumRenderDrivers(); i++) {
		printf("%s\n", SDL_GetRenderDriver(i));
	}

	renderer = SDL_CreateRenderer(window, "direct3d12", 0);
	if(renderer == NULL) {
		SDL_DestroyWindow(window);
		SDL_Quit();
		window = NULL;
		printf("Could not get renderer. SDL_Error: %s\n", SDL_GetError());
		return 0;
	}

	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
	texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, 320, 200);

	if (texture == NULL) {
		printf("Could not create texture. SDL_Error: %s\n", SDL_GetError());
		exit(0);
	}

	surface = SDL_GetWindowSurface(window);
	if(surface == NULL) {
		SDL_DestroyWindow(window);
		SDL_Quit();
		window = NULL;
		printf("Could not get window surface. SDL_Error: %s\n", SDL_GetError());
		return 0;
	}

	return 1;
}

unsigned int djd_shutdown()
{
	SDL_DestroyWindow(window);
	SDL_Quit();
	window = NULL;
	surface = NULL;

	return 1;
}

static unsigned int colors[256] = {0};
unsigned int djd_setpalette(unsigned char* bytes)
{
	register int i, c;
//	for(i = 0; i < 256; i++)
//	{
//		palette[i].peBlue = *bytes++;
//		palette[i].peGreen = *bytes++;
//		palette[i].peRed = *bytes++;
//	}
//
//	lpDDraw->CreatePalette(DDPCAPS_8BIT, palette, &lpddpal, NULL);
//	lpPrimarySurface->SetPalette(lpddpal);
	for(i = 0; i < 256; i++)
	{
		c = *bytes++;
		c = (c << 8) + *(bytes++);
		c = (c << 8) + *(bytes++);
		c = (c << 8) + 0xff;
		colors[i] = c;
	}
	return 0;
}

unsigned int djd_draw(unsigned char* src, unsigned int width, unsigned int height)
{
	register unsigned int x, y;

	unsigned char* source_ptr;   // working pointers
	unsigned int* dest_ptr;

	void* pixels;
	int pitch;

	if((window == NULL) || (surface == NULL)) {
		return 0;
	}
	//SDL_SetRenderTarget(renderer, texture);

	SDL_LockTexture(texture, NULL, &pixels, &pitch);
	source_ptr = src;
	dest_ptr = (unsigned int*)pixels;

	// iterate thru each scanline and copy bitmap
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++)
			*dest_ptr++ = colors[*source_ptr++];
	}

	//SDL_ConvertPixels(surface->w, surface->h,
	//									surface->format->format,
	//									surface->pixels, surface->pitch,
	//									SDL_PIXELFORMAT_RGBA8888,
	//									pixels, pitch);

	SDL_UnlockTexture(texture);
	//SDL_SetRenderTarget(renderer, NULL);

	SDL_RenderCopy(renderer, texture, NULL, NULL);

	SDL_RenderPresent(renderer);

	return 1;
}

