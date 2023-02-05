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
 * pcm_player.h
 *
 *  Created on: Dec 8, 2011
 *      Author: David J. Rager
 *       Email: djrager@fourthwoods.com
 */

#ifndef PCM_PLAYER_H_
#define PCM_PLAYER_H_

#include <windows.h>
#include <mmsystem.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*pcm_notify_cb)(unsigned int val);

unsigned int pcm_init();
void pcm_shutdown();

HANDLE pcm_sample_open(unsigned int sample_rate, unsigned int sample_size, unsigned int channels, unsigned char* buf, unsigned int len);
void pcm_sample_close(HANDLE h);

MMRESULT pcm_play(HANDLE h);
MMRESULT pcm_stop(HANDLE h);

MMRESULT pcm_pause(HANDLE h);
MMRESULT pcm_resume(HANDLE h);

MMRESULT pcm_set_volume_left(HANDLE h, unsigned int level);
MMRESULT pcm_set_volume_right(HANDLE h, unsigned int level);
MMRESULT pcm_set_volume(HANDLE h, unsigned int level);

MMRESULT pcm_volume_left(HANDLE h, unsigned int dir);
MMRESULT pcm_volume_right(HANDLE h, unsigned int dir);
MMRESULT pcm_volume(HANDLE h, unsigned int dir);

BOOL pcm_is_playing(HANDLE h);
BOOL pcm_is_paused(HANDLE h);
BOOL pcm_is_stopped(HANDLE h);

#ifdef __cplusplus
}
#endif

#endif /* PCM_PLAYER_H_ */
