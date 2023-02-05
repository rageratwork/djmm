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
 * dx_input.c
 *
 *  Created on: Jan 1, 2012
 *      Author: David J. Rager
 */
#define   DIRECTINPUT_VERSION 0x0800

#include <ddraw.h>
#include <dinput.h>

#include "dj_debug.h"
#include "dx_input.h"

static LPDIRECTINPUT8 lpdi = NULL;
static LPDIRECTINPUTDEVICE8 lpdiKeyboard = NULL;
static LPDIRECTINPUTDEVICE8 lpdiMouse = NULL;
static LPDIRECTINPUTDEVICE8 lpdiJoystick = NULL;

char *DDErrorString(HRESULT hr);

static BOOL CALLBACK enumCallback(const DIDEVICEINSTANCE* instance, VOID* context)
{
    HRESULT hr;

    hr = lpdi->lpVtbl->CreateDevice(lpdi, &instance->guidInstance, &lpdiJoystick, NULL);
	if(hr != DI_OK)
	{
		DJ_TRACE("enumCallback: CreateDevice failed: %s\n", DDErrorString(hr));
		return 0;
	}

    if (FAILED(hr)) {
        return DIENUM_CONTINUE;
    }

    return DIENUM_STOP;
}

unsigned int dxi_init(HINSTANCE hinstance, HWND hwnd)
{
	unsigned int hr;

	hr = DirectInput8Create(hinstance, DIRECTINPUT_VERSION, &IID_IDirectInput8, (void**)&lpdi, NULL);
	if(hr != DI_OK)
	{
		DJ_TRACE("dxi_init: DirectInput8Create failed: %s\n", DDErrorString(hr));
		return 0;
	}

	hr = lpdi->lpVtbl->CreateDevice(lpdi, &GUID_SysKeyboard, &lpdiKeyboard, NULL);
	if(hr != DI_OK)
	{
		DJ_TRACE("dxi_init: CreateDevice (GUID_SysKeyboard) failed: %s\n", DDErrorString(hr));
		return 0;
	}

	hr = lpdiKeyboard->lpVtbl->SetCooperativeLevel(lpdiKeyboard, hwnd, DISCL_FOREGROUND | DISCL_EXCLUSIVE | DISCL_NOWINKEY);
	if(hr != DI_OK)
	{
		DJ_TRACE("dxi_init: SetCooperativeLevel (GUID_SysKeyboard) failed: %s\n", DDErrorString(hr));
		return 0;
	}

	hr = lpdiKeyboard->lpVtbl->SetDataFormat(lpdiKeyboard, &c_dfDIKeyboard);
	if(hr != DI_OK)
	{
		DJ_TRACE("dxi_init: SetDataFormat (GUID_SysKeyboard) failed: %s\n", DDErrorString(hr));
		return 0;
	}

	hr = lpdiKeyboard->lpVtbl->Acquire(lpdiKeyboard);
	if(hr != DI_OK)
	{
		DJ_TRACE("dxi_init: Acquire (GUID_SysKeyboard) failed: %s\n", DDErrorString(hr));
		return 0;
	}

	hr = lpdi->lpVtbl->CreateDevice(lpdi, &GUID_SysMouse, &lpdiMouse, NULL);
	if(hr != DI_OK)
	{
		DJ_TRACE("dxi_init: CreateDevice (GUID_SysMouse) failed: %s\n", DDErrorString(hr));
		return 0;
	}

	hr = lpdiMouse->lpVtbl->SetCooperativeLevel(lpdiMouse, hwnd, DISCL_FOREGROUND | DISCL_EXCLUSIVE);
	if(hr != DI_OK)
	{
		DJ_TRACE("dxi_init: SetCooperativeLevel (GUID_SysMouse) failed: %s\n", DDErrorString(hr));
		return 0;
	}

	hr = lpdiMouse->lpVtbl->SetDataFormat(lpdiMouse, &c_dfDIMouse);
	if(hr != DI_OK)
	{
		DJ_TRACE("dxi_init: SetDataFormat (GUID_SysMouse) failed: %s\n", DDErrorString(hr));
		return 0;
	}

	hr = lpdiMouse->lpVtbl->Acquire(lpdiMouse);
	if(hr != DI_OK)
	{
		DJ_TRACE("dxi_init: Acquire (GUID_SysMouse) failed: %s\n", DDErrorString(hr));
		return 0;
	}


	hr = lpdi->lpVtbl->EnumDevices(lpdi, DI8DEVCLASS_GAMECTRL, enumCallback, NULL, DIEDFL_ATTACHEDONLY);
	if(hr != DI_OK)
	{
		DJ_TRACE("dxi_init: EnumDevices (joystick) failed: %s\n", DDErrorString(hr));
		return 0;
	}


	if(lpdiJoystick != NULL)
	{
		hr = lpdiJoystick->lpVtbl->SetCooperativeLevel(lpdiJoystick, hwnd, DISCL_FOREGROUND | DISCL_EXCLUSIVE);
		if(hr != DI_OK)
		{
			DJ_TRACE("dxi_init: SetCooperativeLevel (joystick) failed: %s\n", DDErrorString(hr));
			return 0;
		}

		hr = lpdiJoystick->lpVtbl->SetDataFormat(lpdiJoystick, &c_dfDIJoystick2);
		if(hr != DI_OK)
		{
			DJ_TRACE("dxi_init: SetDataFormat (joystick) failed: %s\n", DDErrorString(hr));
			return 0;
		}

		hr = lpdiJoystick->lpVtbl->Acquire(lpdiJoystick);
		if(hr != DI_OK)
		{
			DJ_TRACE("dxi_init: Acquire (joystick) failed: %s\n", DDErrorString(hr));
			return 0;
		}
	}

	return 1;
}

unsigned int dxi_get_keyboard_state(unsigned char* keystate, unsigned int len)
{
	unsigned int hr;
	if(lpdiKeyboard == NULL)
	{
		//DJ_TRACE("dxi_get_keyboard_state: Keyboard not initialized.\n");
		return 0;
	}

	hr = lpdiKeyboard->lpVtbl->GetDeviceState(lpdiKeyboard, 256, keystate);
	if(hr != DI_OK)
	{
		DJ_TRACE("dxi_get_keyboard_state: GetDeviceState failed: %s\n", DDErrorString(hr));
		return 0;
	}

	return 1;
}

unsigned int dxi_get_mouse_state(struct mouse_state* mouse)
{
	DIMOUSESTATE ms;
	HRESULT hr;
	if(lpdiMouse == NULL)
	{
		//DJ_TRACE("dxi_get_mouse_state: Mouse not initialized.\n");
		return 0;
	}

	hr = lpdiMouse->lpVtbl->Poll(lpdiMouse);
	if (hr != DI_OK)
	{
	    hr = lpdiMouse->lpVtbl->Acquire(lpdiMouse);
	    while(hr == DIERR_INPUTLOST)
	        hr = lpdiMouse->lpVtbl->Acquire(lpdiMouse);

	    hr = lpdiMouse->lpVtbl->Poll(lpdiMouse);
	    if(hr != DI_OK && hr != DI_NOEFFECT)
		{
			DJ_TRACE("dxi_get_mouse_state: Poll failed: %s\n", DDErrorString(hr));
			return 0;
		}
	}
	hr = lpdiMouse->lpVtbl->GetDeviceState(lpdiMouse, sizeof(ms), &ms);
	if(hr != DI_OK)
	{
		DJ_TRACE("dxi_get_mouse_state: GetDeviceState failed: %s\n", DDErrorString(hr));
		return 0;
	}

	mouse->x = ms.lX;
	mouse->y = ms.lY;
	mouse->b1 = ms.rgbButtons[0];
	mouse->b2 = ms.rgbButtons[1];
	mouse->b3 = ms.rgbButtons[2];
	mouse->b4 = ms.rgbButtons[3];

	return 1;
}

unsigned int dxi_get_joystick_state(struct joystick_state* joystick)
{
	DIJOYSTATE2 js;
	HRESULT hr;

	if(lpdiJoystick == NULL)
	{
		//DJ_TRACE("dxi_get_joystick_state: Joystick not initialized.\n");
		return 0;
	}

	joystick->dpad = -1;
	hr = lpdiJoystick->lpVtbl->Poll(lpdiJoystick);
	if (FAILED(hr))
	{
	    hr = lpdiJoystick->lpVtbl->Acquire(lpdiJoystick);
	    while(hr == DIERR_INPUTLOST)
	        hr = lpdiJoystick->lpVtbl->Acquire(lpdiJoystick);

	    hr = lpdiJoystick->lpVtbl->Poll(lpdiJoystick);
	    if(hr != DI_OK && hr != DI_NOEFFECT)
		{
			DJ_TRACE("dxi_get_joystick_state: Poll failed: %s\n", DDErrorString(hr));
			return 0;
		}
	}
	hr = lpdiJoystick->lpVtbl->GetDeviceState(lpdiJoystick, sizeof(js), &js);
	if(hr != DI_OK)
	{
		DJ_TRACE("dxi_get_joystick_state: GetDeviceState failed: %s\n", DDErrorString(hr));
		return 0;
	}

//	DJ_TRACE("lX %d, lY %d\n", js.lX - 32767, js.lY - 32511);
//	DJ_TRACE("Buttons[0] %02x, Buttons[1] %02x, Buttons[2] %02x, Buttons[3] %02x\n", js.rgbButtons[0], js.rgbButtons[1], js.rgbButtons[2], js.rgbButtons[3]);
//	DJ_TRACE("Buttons[4] %02x, Buttons[5] %02x, Buttons[6] %02x, Buttons[7] %02x\n", js.rgbButtons[4], js.rgbButtons[5], js.rgbButtons[6], js.rgbButtons[7]);
//	DJ_TRACE("Buttons[8] %02x, Buttons[9] %02x, Buttons[10] %02x, Buttons[11] %02x\n", js.rgbButtons[8], js.rgbButtons[9], js.rgbButtons[10], js.rgbButtons[11]);
//	DJ_TRACE("POV[0] %d\n", js.rgdwPOV[0]);

	joystick->x = js.lX - 32767;
	joystick->y = js.lY - 32511;
	joystick->dpad = js.rgdwPOV[0];
	joystick->b1 = js.rgbButtons[0];
	joystick->b2 = js.rgbButtons[1];
	joystick->b3 = js.rgbButtons[2];
	joystick->b4 = js.rgbButtons[3];

	return 1;
}

unsigned int dxi_shutdown()
{
	if(lpdiKeyboard)
		lpdiKeyboard->lpVtbl->Unacquire(lpdiKeyboard);

	if(lpdi)
		lpdi->lpVtbl->Release(lpdi);

	return 1;
}



