/*
 * DjMM
 * v0.2
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
 * mus_player.h
 *
 *  Created on: Dec 3, 2011
 *      Author: David J. Rager
 */
#ifndef MUS_PLAYER_H_
#define MUS_PLAYER_H_

#include "dj_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mus_notify_cb)(unsigned int val);

DJ_RESULT mus_init();
void mus_shutdown();

DJ_HANDLE mus_score_open(unsigned char* buf, unsigned int len, mus_notify_cb callback);
void mus_score_close(DJ_HANDLE h);

DJ_RESULT mus_play(DJ_HANDLE h);
DJ_RESULT mus_stop(DJ_HANDLE h);

DJ_RESULT mus_pause(DJ_HANDLE h);
DJ_RESULT mus_resume(DJ_HANDLE h);

DJ_RESULT mus_set_looping(DJ_HANDLE h, boolean looping);

DJ_RESULT mus_set_volume_left(DJ_HANDLE h, unsigned int level);
DJ_RESULT mus_set_volume_right(DJ_HANDLE h, unsigned int level);
DJ_RESULT mus_set_volume(DJ_HANDLE h, unsigned int level);
DJ_RESULT mus_volume_left(DJ_HANDLE h, unsigned int dir);
DJ_RESULT mus_volume_right(DJ_HANDLE h, unsigned int dir);
DJ_RESULT mus_volume(DJ_HANDLE h, unsigned int dir);

boolean mus_is_playing(DJ_HANDLE h);
boolean mus_is_paused(DJ_HANDLE h);
boolean mus_is_stopped(DJ_HANDLE h);
boolean mus_is_looping(DJ_HANDLE h);

#ifdef __cplusplus
}
#endif

#endif /* MUS_PLAYER_H_ */
