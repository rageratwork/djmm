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

#include "dj_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NOERROR 0
#define ERROR -1
#define INVALID_PARAM -1

typedef void (*pcm_notify_cb)(void* data);

DJ_RESULT pcm_init();
void pcm_shutdown();

DJ_HANDLE pcm_sound_open(unsigned char* buf, unsigned int len, pcm_notify_cb callback);
void pcm_sound_close(DJ_HANDLE h);

DJ_RESULT pcm_play(DJ_HANDLE h);
DJ_RESULT pcm_stop(DJ_HANDLE h);

DJ_RESULT pcm_pause(DJ_HANDLE h);
DJ_RESULT pcm_resume(DJ_HANDLE h);

DJ_RESULT pcm_set_volume_left(DJ_HANDLE h, unsigned int level);
DJ_RESULT pcm_set_volume_right(DJ_HANDLE h, unsigned int level);
DJ_RESULT pcm_set_volume(DJ_HANDLE h, unsigned int level);

boolean pcm_is_playing(DJ_HANDLE h);
boolean pcm_is_paused(DJ_HANDLE h);
boolean pcm_is_stopped(DJ_HANDLE h);
boolean pcm_is_looping(DJ_HANDLE h);

#ifdef __cplusplus
}
#endif

#endif /* PCM_PLAYER_H_ */
