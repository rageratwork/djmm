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
 * dx_draw.c
 *
 *  Created on: Jan 1, 2012
 *      Author: David J. Rager
 */
#define   DIRECTDRAW_VERSION 0x0700

#include <stdio.h>
#include <ddraw.h>
#include "dj_debug.h"

static LPDIRECTDRAW7	lpDDraw				= NULL;

static DDSURFACEDESC2	SurfaceDescriptor;
static LPDIRECTDRAWSURFACE7	lpPrimarySurface	= NULL;
static LPDIRECTDRAWSURFACE7	lpBackSurface		= NULL;
static LPDIRECTDRAWCLIPPER	lpClipper		= NULL;

static HWND g_hwnd = NULL;
static BOOL g_fullScreen = FALSE;

char *DDErrorString(HRESULT hr);
static BOOL DDFailedCheck(HRESULT hr, char *szMessage);

unsigned int dxd_init(HINSTANCE hinstance, HWND hwnd, UINT screen_width, UINT screen_height, UINT bpp, BOOL fullScreen)
{
	unsigned int hr;
	g_hwnd = hwnd;
	g_fullScreen = fullScreen;

	// create object and test for error
	hr = DirectDrawCreateEx(NULL, (void **)&lpDDraw, &IID_IDirectDraw7, NULL);
	if(hr != DD_OK)
	{
		DJ_TRACE("dxd_init: DirectDrawCreateEx failed: %s\n", DDErrorString(hr));
		return 0;
	}

	// set cooperation level to windowed mode normal
	if(g_fullScreen)
	{
		hr = lpDDraw->lpVtbl->SetCooperativeLevel(lpDDraw, hwnd, DDSCL_ALLOWMODEX | DDSCL_FULLSCREEN | DDSCL_EXCLUSIVE | DDSCL_ALLOWREBOOT);
		if (hr != DD_OK)
		{
			DJ_TRACE("dxd_init: SetCooperativeLevel failed: %s\n", DDErrorString(hr));
			return 0;
		}

	//	if (lpDDraw->lpVtbl->SetDisplayMode(lpDDraw, screen_width, screen_height, bpp, 0, 0) != DD_OK)
		hr = lpDDraw->lpVtbl->SetDisplayMode(lpDDraw, 640, 480, 32, 0, 0);
		if (hr != DD_OK)
		{
			DJ_TRACE("dxd_init: SetDisplayMode failed: %s\n", DDErrorString(hr));
			return 0;
		}

		// Create the primary surface
		memset(&SurfaceDescriptor,0,sizeof(SurfaceDescriptor));
		SurfaceDescriptor.dwSize = sizeof(SurfaceDescriptor);
		SurfaceDescriptor.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;

		// we need to let dd know that we want a complex
		// flippable surface structure, set flags for that
		SurfaceDescriptor.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;

		// set the backbuffer count to 1
		SurfaceDescriptor.dwBackBufferCount = 1;

		// create the primary surface
		hr = lpDDraw->lpVtbl->CreateSurface(lpDDraw, &SurfaceDescriptor, &lpPrimarySurface, NULL);
		if (hr != DD_OK)
		{
			DJ_TRACE("dxd_init: CreateSurface (primary) failed: %s\n", DDErrorString(hr));
			return 0;
		}

		memset(&SurfaceDescriptor,0,sizeof(SurfaceDescriptor));
		SurfaceDescriptor.dwSize = sizeof(SurfaceDescriptor);
		SurfaceDescriptor.dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH;
		SurfaceDescriptor.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
		SurfaceDescriptor.dwWidth = screen_width;
		SurfaceDescriptor.dwHeight = screen_height;

		hr = lpDDraw->lpVtbl->CreateSurface(lpDDraw, &SurfaceDescriptor, &lpBackSurface, NULL);
		if (hr != DD_OK)
		{
			DJ_TRACE("dxd_init: CreateSurface (back surface) failed: %s\n", DDErrorString(hr));
			return 0;
		}
	}
	else
	{
		hr = lpDDraw->lpVtbl->SetCooperativeLevel(lpDDraw, hwnd, DDSCL_NORMAL);
		if(hr != DD_OK)
		{
			DJ_TRACE("dxd_init: SetCooperativeLevel windowed: %s\n", DDErrorString(hr));
			return 0;
		}

		memset(&SurfaceDescriptor, 0, sizeof(SurfaceDescriptor));
		// Create the primary surface
		SurfaceDescriptor.dwSize = sizeof(SurfaceDescriptor);
		SurfaceDescriptor.dwFlags = DDSD_CAPS;
		SurfaceDescriptor.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

		hr = lpDDraw->lpVtbl->CreateSurface(lpDDraw, &SurfaceDescriptor, &lpPrimarySurface, NULL);
		if (hr != DD_OK)
		{
			DJ_TRACE("dxd_init: CreateSurface (primary windowed) failed: %s\n", DDErrorString(hr));
			return 0;
		}

		memset(&SurfaceDescriptor, 0, sizeof(SurfaceDescriptor));
		// Create the back buffer
		SurfaceDescriptor.dwSize = sizeof(SurfaceDescriptor);
		SurfaceDescriptor.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_CAPS;
		SurfaceDescriptor.dwWidth = screen_width;
		SurfaceDescriptor.dwHeight = screen_height;
		SurfaceDescriptor.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;

		hr = lpDDraw->lpVtbl->CreateSurface(lpDDraw, &SurfaceDescriptor, &lpBackSurface, NULL);
		if (hr != DD_OK)
		{
			DJ_TRACE("dxd_init: CreateSurface (back surface windowed) failed: %s\n", DDErrorString(hr));
			return 0;
		}

		// Create the clipper using the DirectDraw object
		hr = lpDDraw->lpVtbl->CreateClipper(lpDDraw, 0, &lpClipper, NULL);
		if(hr != DD_OK)
		{
			DJ_TRACE("dxd_init: CreateClipper failed: %s\n", DDErrorString(hr));
			return 0;
		}

		// Assign your window's HWND to the clipper
		hr = lpClipper->lpVtbl->SetHWnd(lpClipper, 0, hwnd);
		if(hr != DD_OK)
		{
			DJ_TRACE("dxd_init: Assign hWnd to clipper failed: %s\n", DDErrorString(hr));
			return 0;
		}

		// Attach the clipper to the primary surface
		hr = lpPrimarySurface->lpVtbl->SetClipper(lpPrimarySurface, lpClipper);
		if(hr != DD_OK)
		{
			DJ_TRACE("dxd_init: SetClipper failed: %s\n", DDErrorString(hr));
			return 0;
		}
	}

	return 1;
}

static void checkSurfaces()
{
	unsigned int hr;

	// Check the primary surface
	if((lpPrimarySurface) && (lpPrimarySurface->lpVtbl->IsLost(lpPrimarySurface) == DDERR_SURFACELOST))
	{
		hr = lpPrimarySurface->lpVtbl->Restore(lpPrimarySurface);
		if(hr != DD_OK)
		{
			DJ_TRACE("checkSurfaces: Restore (primary) failed: %s\n", DDErrorString(hr));
		}
	}
	// Check the back buffer
	if((lpBackSurface) && (lpBackSurface->lpVtbl->IsLost(lpBackSurface) == DDERR_SURFACELOST))
	{
		hr = lpBackSurface->lpVtbl->Restore(lpBackSurface);
		if(hr != DD_OK)
		{
			DJ_TRACE("checkSurfaces: Restore (back) failed: %s\n", DDErrorString(hr));
		}
	}
}

unsigned int dxd_shutdown()
{
	// first release the secondary surface
	if (lpBackSurface)
		lpBackSurface->lpVtbl->Release(lpBackSurface);

	// now release the primary surface
	if (lpPrimarySurface)
		lpPrimarySurface->lpVtbl->Release(lpPrimarySurface);

	// release the directdraw object
	if (lpDDraw!=NULL)
		lpDDraw->lpVtbl->Release(lpDDraw);

	return 1;
}

static unsigned int colors[256] = {0};
unsigned int dxd_setpalette(unsigned char* bytes)
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
		colors[i] = c;
	}
	return 0;
}

unsigned int dxd_draw(unsigned char* src, UINT width, UINT height)
{
	register int x, y;

	unsigned char *source_ptr;   // working pointers
	unsigned int *dest_ptr;

	HRESULT hr;

	checkSurfaces();

	source_ptr = src;

	// set size of the structure
	SurfaceDescriptor.dwSize = sizeof(SurfaceDescriptor);

	// lock the display surface
	hr = lpBackSurface->lpVtbl->Lock(lpBackSurface, NULL, &SurfaceDescriptor, DDLOCK_WAIT | DDLOCK_SURFACEMEMORYPTR, NULL);
	while(hr == DDERR_SURFACELOST)
	{
		DJ_TRACE("dxd_draw: Back surface lost\n");
		hr = lpBackSurface->lpVtbl->Restore(lpBackSurface);
		if(hr != DD_OK)
		{
			DJ_TRACE("dxd_draw: Failed to restore back buffer: %s\n", DDErrorString(hr));
		}

		hr = lpBackSurface->lpVtbl->Lock(lpBackSurface, NULL, &SurfaceDescriptor, DDLOCK_WAIT | DDLOCK_SURFACEMEMORYPTR, NULL);
		if(hr != DD_OK)
		{
			DJ_TRACE("dxd_draw: Failed to lock back buffer: %s\n", DDErrorString(hr));
		}
	}

	if(hr != DD_OK)
	{
		DJ_TRACE("dxd_draw: Failed to lock back buffer: %s\n", DDErrorString(hr));
		return 0;
	}

	// assign a pointer to the memory surface for manipulation
	dest_ptr = (unsigned int *)SurfaceDescriptor.lpSurface;

	// iterate thru each scanline and copy bitmap
	for (y = 0; y < height; y++)
	{
		for(x = 0; x < width; x++)
			*dest_ptr++ = colors[*source_ptr++];
	}

	// unlock the surface
	hr = lpBackSurface->lpVtbl->Unlock(lpBackSurface, NULL);
	if(hr != DD_OK)
	{
		DJ_TRACE("dxd_draw: Failed to unlock back buffer: %s\n", DDErrorString(hr));
		return 0;
	}

//	if(g_fullScreen)
//	{
//		lpPrimarySurface->lpVtbl->Flip(lpPrimarySurface, NULL, DDFLIP_WAIT);
//	}
//	else
	{
		POINT p;
		RECT rsrc,rdest;
		p.x = p.y = 0;
		ClientToScreen(g_hwnd, &p);
		GetClientRect(g_hwnd, &rdest);
		OffsetRect(&rdest, p.x, p.y);
		SetRect(&rsrc, 0, 0, width, height);

		hr = lpPrimarySurface->lpVtbl->Blt(lpPrimarySurface, &rdest, lpBackSurface, &rsrc, DDBLT_WAIT, NULL);
		while(hr == DDERR_SURFACELOST)
		{
			DJ_TRACE("Primary surface lost\n");
			hr = lpPrimarySurface->lpVtbl->Restore(lpBackSurface);
			if(hr != DD_OK)
			{
				DJ_TRACE("dxd_draw: Failed to restore primary surface: %s\n", DDErrorString(hr));
			}

			hr = lpPrimarySurface->lpVtbl->Blt(lpPrimarySurface, &rdest, lpBackSurface, &rsrc, DDBLT_WAIT, NULL);
			if(hr != DD_OK)
			{
				DJ_TRACE("dxd_draw: Failed to blt image: %s\n", DDErrorString(hr));
			}
		}

		if(hr != DD_OK)
		{
			DJ_TRACE("dxd_draw: Failed to blt image: %s\n", DDErrorString(hr));
			return 0;
		}
	}

	return 1;
}

char *DDErrorString(HRESULT hr)
{
	static char unk[64];

	switch (hr)
	{
	case DDERR_ALREADYINITIALIZED:           return "DDERR_ALREADYINITIALIZED";
	case DDERR_CANNOTATTACHSURFACE:          return "DDERR_CANNOTATTACHSURFACE";
	case DDERR_CANNOTDETACHSURFACE:          return "DDERR_CANNOTDETACHSURFACE";
	case DDERR_CURRENTLYNOTAVAIL:            return "DDERR_CURRENTLYNOTAVAIL";
	case DDERR_EXCEPTION:                    return "DDERR_EXCEPTION";
	case DDERR_GENERIC:                      return "DDERR_GENERIC";
	case DDERR_HEIGHTALIGN:                  return "DDERR_HEIGHTALIGN";
	case DDERR_INCOMPATIBLEPRIMARY:          return "DDERR_INCOMPATIBLEPRIMARY";
	case DDERR_INVALIDCAPS:                  return "DDERR_INVALIDCAPS";
	case DDERR_INVALIDCLIPLIST:              return "DDERR_INVALIDCLIPLIST";
	case DDERR_INVALIDMODE:                  return "DDERR_INVALIDMODE";
	case DDERR_INVALIDOBJECT:                return "DDERR_INVALIDOBJECT";
	case DDERR_INVALIDPARAMS:                return "DDERR_INVALIDPARAMS";
	case DDERR_INVALIDPIXELFORMAT:           return "DDERR_INVALIDPIXELFORMAT";
	case DDERR_INVALIDRECT:                  return "DDERR_INVALIDRECT";
	case DDERR_LOCKEDSURFACES:               return "DDERR_LOCKEDSURFACES";
	case DDERR_NO3D:                         return "DDERR_NO3D";
	case DDERR_NOALPHAHW:                    return "DDERR_NOALPHAHW";
	case DDERR_NOCLIPLIST:                   return "DDERR_NOCLIPLIST";
	case DDERR_NOCOLORCONVHW:                return "DDERR_NOCOLORCONVHW";
	case DDERR_NOCOOPERATIVELEVELSET:        return "DDERR_NOCOOPERATIVELEVELSET";
	case DDERR_NOCOLORKEY:                   return "DDERR_NOCOLORKEY";
	case DDERR_NOCOLORKEYHW:                 return "DDERR_NOCOLORKEYHW";
	case DDERR_NODIRECTDRAWSUPPORT:          return "DDERR_NODIRECTDRAWSUPPORT";
	case DDERR_NOEXCLUSIVEMODE:              return "DDERR_NOEXCLUSIVEMODE";
	case DDERR_NOFLIPHW:                     return "DDERR_NOFLIPHW";
	case DDERR_NOGDI:                        return "DDERR_NOGDI";
	case DDERR_NOMIRRORHW:                   return "DDERR_NOMIRRORHW";
	case DDERR_NOTFOUND:                     return "DDERR_NOTFOUND";
	case DDERR_NOOVERLAYHW:                  return "DDERR_NOOVERLAYHW";
	case DDERR_NORASTEROPHW:                 return "DDERR_NORASTEROPHW";
	case DDERR_NOROTATIONHW:                 return "DDERR_NOROTATIONHW";
	case DDERR_NOSTRETCHHW:                  return "DDERR_NOSTRETCHHW";
	case DDERR_NOT4BITCOLOR:                 return "DDERR_NOT4BITCOLOR";
	case DDERR_NOT4BITCOLORINDEX:            return "DDERR_NOT4BITCOLORINDEX";
	case DDERR_NOT8BITCOLOR:                 return "DDERR_NOT8BITCOLOR";
	case DDERR_NOTEXTUREHW:                  return "DDERR_NOTEXTUREHW";
	case DDERR_NOVSYNCHW:                    return "DDERR_NOVSYNCHW";
	case DDERR_NOZBUFFERHW:                  return "DDERR_NOZBUFFERHW";
	case DDERR_NOZOVERLAYHW:                 return "DDERR_NOZOVERLAYHW";
	case DDERR_OUTOFCAPS:                    return "DDERR_OUTOFCAPS";
	case DDERR_OUTOFMEMORY:                  return "DDERR_OUTOFMEMORY";
	case DDERR_OUTOFVIDEOMEMORY:             return "DDERR_OUTOFVIDEOMEMORY";
	case DDERR_OVERLAYCANTCLIP:              return "DDERR_OVERLAYCANTCLIP";
	case DDERR_OVERLAYCOLORKEYONLYONEACTIVE: return "DDERR_OVERLAYCOLORKEYONLYONEACTIVE";
	case DDERR_PALETTEBUSY:                  return "DDERR_PALETTEBUSY";
	case DDERR_COLORKEYNOTSET:               return "DDERR_COLORKEYNOTSET";
	case DDERR_SURFACEALREADYATTACHED:       return "DDERR_SURFACEALREADYATTACHED";
	case DDERR_SURFACEALREADYDEPENDENT:      return "DDERR_SURFACEALREADYDEPENDENT";
	case DDERR_SURFACEBUSY:                  return "DDERR_SURFACEBUSY";
	case DDERR_CANTLOCKSURFACE:              return "DDERR_CANTLOCKSURFACE";
	case DDERR_SURFACEISOBSCURED:            return "DDERR_SURFACEISOBSCURED";
	case DDERR_SURFACELOST:                  return "DDERR_SURFACELOST";
	case DDERR_SURFACENOTATTACHED:           return "DDERR_SURFACENOTATTACHED";
	case DDERR_TOOBIGHEIGHT:                 return "DDERR_TOOBIGHEIGHT";
	case DDERR_TOOBIGSIZE:                   return "DDERR_TOOBIGSIZE";
	case DDERR_TOOBIGWIDTH:                  return "DDERR_TOOBIGWIDTH";
	case DDERR_UNSUPPORTED:                  return "DDERR_UNSUPPORTED";
	case DDERR_UNSUPPORTEDFORMAT:            return "DDERR_UNSUPPORTEDFORMAT";
	case DDERR_UNSUPPORTEDMASK:              return "DDERR_UNSUPPORTEDMASK";
	case DDERR_VERTICALBLANKINPROGRESS:      return "DDERR_VERTICALBLANKINPROGRESS";
	case DDERR_WASSTILLDRAWING:              return "DDERR_WASSTILLDRAWING";
	case DDERR_XALIGN:                       return "DDERR_XALIGN";
	case DDERR_INVALIDDIRECTDRAWGUID:        return "DDERR_INVALIDDIRECTDRAWGUID";
	case DDERR_DIRECTDRAWALREADYCREATED:     return "DDERR_DIRECTDRAWALREADYCREATED";
	case DDERR_NODIRECTDRAWHW:               return "DDERR_NODIRECTDRAWHW";
	case DDERR_PRIMARYSURFACEALREADYEXISTS:  return "DDERR_PRIMARYSURFACEALREADYEXISTS";
	case DDERR_NOEMULATION:                  return "DDERR_NOEMULATION";
	case DDERR_REGIONTOOSMALL:               return "DDERR_REGIONTOOSMALL";
	case DDERR_CLIPPERISUSINGHWND:           return "DDERR_CLIPPERISUSINGHWND";
	case DDERR_NOCLIPPERATTACHED:            return "DDERR_NOCLIPPERATTACHED";
	case DDERR_NOHWND:                       return "DDERR_NOHWND";
	case DDERR_HWNDSUBCLASSED:               return "DDERR_HWNDSUBCLASSED";
	case DDERR_HWNDALREADYSET:               return "DDERR_HWNDALREADYSET";
	case DDERR_NOPALETTEATTACHED:            return "DDERR_NOPALETTEATTACHED";
	case DDERR_NOPALETTEHW:                  return "DDERR_NOPALETTEHW";
	case DDERR_BLTFASTCANTCLIP:              return "DDERR_BLTFASTCANTCLIP";
	case DDERR_NOBLTHW:                      return "DDERR_NOBLTHW";
	case DDERR_NODDROPSHW:                   return "DDERR_NODDROPSHW";
	case DDERR_OVERLAYNOTVISIBLE:            return "DDERR_OVERLAYNOTVISIBLE";
	case DDERR_NOOVERLAYDEST:                return "DDERR_NOOVERLAYDEST";
	case DDERR_INVALIDPOSITION:              return "DDERR_INVALIDPOSITION";
	case DDERR_NOTAOVERLAYSURFACE:           return "DDERR_NOTAOVERLAYSURFACE";
	case DDERR_EXCLUSIVEMODEALREADYSET:      return "DDERR_EXCLUSIVEMODEALREADYSET";
	case DDERR_NOTFLIPPABLE:                 return "DDERR_NOTFLIPPABLE";
	case DDERR_CANTDUPLICATE:                return "DDERR_CANTDUPLICATE";
	case DDERR_NOTLOCKED:                    return "DDERR_NOTLOCKED";
	case DDERR_CANTCREATEDC:                 return "DDERR_CANTCREATEDC";
	case DDERR_NODC:                         return "DDERR_NODC";
	case DDERR_WRONGMODE:                    return "DDERR_WRONGMODE";
	case DDERR_IMPLICITLYCREATED:            return "DDERR_IMPLICITLYCREATED";
	case DDERR_NOTPALETTIZED:                return "DDERR_NOTPALETTIZED";
	case DDERR_UNSUPPORTEDMODE:              return "DDERR_UNSUPPORTEDMODE";
	case DDERR_NOMIPMAPHW:                   return "DDERR_NOMIPMAPHW";
	case DDERR_INVALIDSURFACETYPE:           return "DDERR_INVALIDSURFACETYPE";
	case DDERR_DCALREADYCREATED:             return "DDERR_DCALREADYCREATED";
	case DDERR_CANTPAGELOCK:                 return "DDERR_CANTPAGELOCK";
	case DDERR_CANTPAGEUNLOCK:               return "DDERR_CANTPAGEUNLOCK";
	case DDERR_NOTPAGELOCKED:                return "DDERR_NOTPAGELOCKED";
	case DDERR_NOTINITIALIZED:               return "DDERR_NOTINITIALIZED";
	}
	snprintf(unk, sizeof(unk), "Unknown error: %d", hr);
	return unk;
}

static BOOL DDFailedCheck(HRESULT hr, char *szMessage)
{
	if (FAILED(hr))
	{
		DJ_TRACE("%s (%s)\n", szMessage, DDErrorString(hr) );
		return TRUE;
	}
	return FALSE;
}
