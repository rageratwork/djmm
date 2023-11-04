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
 * pcm_player.c
 *
 *  Created on: Dec 6, 2011
 *      Author: David J. Rager
 *       Email: djrager@fourthwoods.com
 */
#include <stdio.h>
#include <conio.h>
#include <windows.h>
#include <mmsystem.h>

#include "pcm_player.h"

#define STATE_ERROR		0
#define STATE_STARTING	1
#define STATE_PLAYING	2
#define STATE_PAUSED	3
#define STATE_STOPPING	4
#define STATE_STOPPED	5
#define STATE_SHUTDOWN	6
#define STATE_INITIALIZING 7

#define VOL_UP 0
#define VOL_DOWN 1

#ifndef PCM_PLAYER_STANDALONE

#include "dj_debug.h"
#include "djmm_utils.h"

#else

unsigned char* load_file(unsigned char* filename, unsigned int* len);

#endif

struct pcm_player {
  HANDLE event;
  HANDLE thread;
  HANDLE ready;
  HANDLE mutex;

  HWAVEOUT stream;
  WAVEHDR header[2]; // double buffer
  unsigned int idx;

  unsigned int device;

  unsigned int state;
  unsigned int looping;
  unsigned int lvolume;
  unsigned int rvolume;

  struct pcm_sample* sample;
  pcm_notify_cb cb;

  struct pcm_player* next;
};

struct pcm_sample {
  unsigned int sample_rate;
  unsigned int sample_size;
  unsigned int channels;

  unsigned char* ptr;

  unsigned char* raw_bytes;
  unsigned int raw_len;
};

struct pcm_player_holder {
  struct pcm_player* player;

  struct pcm_player_holder* next;
  struct pcm_player_holder* prev;
};

struct pcm_queue {
  HANDLE mutex;
  struct pcm_player_holder* front;
  struct pcm_player_holder* back;
};

struct pcm_queue queue;

static void _queue_init() {
  queue.mutex = CreateMutex(NULL, FALSE, NULL);
  if (queue.mutex == NULL) {
    fprintf(stderr, "CreateMutex failed %lu\n", GetLastError());
    return;
  }

  queue.front = NULL;
  queue.back = NULL;
}

// +---------+      +--------+      +--------+      +--------+
// | queue   |      | holder |      | holder |      | holder |
// |---------|      |--------|      |--------|      |--------|
// | front   |----->| next   |----->| next   |----->| next   |-->0
// | back    |  0<--| prev   |<-----| prev   |<-----| prev   |
// +---------+      +________+      +________+      +________+
//    \                                                  ^
//     \------------------------------------------------/
//
static void _queue_add(struct pcm_player* p) {
  WaitForSingleObject(queue.mutex, INFINITE);

  struct pcm_player_holder* holder = malloc(sizeof(struct pcm_player_holder));
  if (holder != NULL) {
    holder->player = p;
    holder->next = NULL;
    holder->prev = NULL;

    holder->next = queue.front;
    queue.front = holder;
    if(holder->next != NULL){
      holder->next->prev = holder;
    }

    if (queue.back == NULL) {
      queue.back = holder;
    }
  }

  ReleaseMutex(queue.mutex);
}

static struct pcm_player* _queue_remove() {
  struct pcm_player* p = NULL;
  WaitForSingleObject(queue.mutex, INFINITE);
  struct pcm_player_holder* holder = queue.back;
  if(holder != NULL) {
    p = holder->player;

    if(queue.back == queue.front) {
      queue.back = NULL;
      queue.front = NULL;
    } else {
      queue.back = holder->prev;
      queue.back->next = NULL;
    }

    free(holder);
  }

  ReleaseMutex(queue.mutex);
  return p;
}

#define MAX_BUFFER_SIZE	1024 * 8

static unsigned int _pcm_get_streambuf(struct pcm_sample* s, unsigned char* out, unsigned int* outlen);
static unsigned int _pcm_adjust_volume(unsigned char* out, unsigned int len, unsigned int sample_size, unsigned int channels, unsigned int lvol, unsigned int rvol);

static struct pcm_player* _pcm_player_load(pcm_notify_cb callback);
static void _pcm_player_unload(struct pcm_player* p);
static struct pcm_player* _pcm_player_list_add(struct pcm_player* p);
static void _pcm_player_free(struct pcm_player* p);

static void _pcm_sample_free(struct pcm_sample* s);

static struct pcm_player* _pcm_stream_open(struct pcm_player* p);
static void _pcm_stream_close(struct pcm_player* p);

static void _pcm_player_pool_add(struct pcm_player* p);

static DJ_RESULT _pcm_rewind(DJ_HANDLE h);

static void CALLBACK _pcm_callback_proc(HWAVEOUT hmo, UINT wMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
  struct pcm_player* p = (struct pcm_player*)dwInstance;
  if (p == NULL)
    return;

  switch (wMsg) {
  case WOM_DONE:
    _queue_add(p);
    break;
  case WOM_OPEN:
    break;
  case WOM_CLOSE:
    break;
  }

}

static boolean _pcm_player_proc_done = false;
static HANDLE _pcm_player_proc_handle = NULL;

static DWORD WINAPI _pcm_player_proc(LPVOID lparameter) {
  unsigned int err = 0;
  while(!_pcm_player_proc_done) {
    struct pcm_player* p = _queue_remove();
    if(p == NULL) {
      // yield
      Sleep(20);
      continue;
    }

    switch(p->state) {
    case STATE_PLAYING:
      // buffer[idx] finished, buffer[idx + 1] will have started, unless it was empty
      WaitForSingleObject(p->mutex, INFINITE);

      // try to fill buffer[idx] with more data
      _pcm_get_streambuf(p->sample, (unsigned char*)p->header[p->idx].lpData, (unsigned int*)&p->header[p->idx].dwBufferLength);
      _pcm_adjust_volume((unsigned char*)p->header[p->idx].lpData, p->header[p->idx].dwBufferLength, p->sample->sample_size, p->sample->channels, p->lvolume, p->rvolume);
      p->header[p->idx].dwBytesRecorded = p->header[p->idx].dwBufferLength;
      p->header[p->idx].dwFlags &= ~WHDR_DONE; // clear the done flag to reuse the buffer

      if (p->header[p->idx].dwBufferLength > 0) {
        // we were able to read more into the buffer, so queue it
        err = waveOutWrite(p->stream, &p->header[p->idx], sizeof(WAVEHDR));
        p->idx = (p->idx + 1) % 2;
      } else if (p->looping) {
        // we could not read more into the buffer, but we're looping so rewind and start again
        _pcm_rewind(p);
        _pcm_get_streambuf(p->sample, (unsigned char*)p->header[p->idx].lpData, (unsigned int*)&p->header[p->idx].dwBufferLength);
        _pcm_adjust_volume((unsigned char*)p->header[p->idx].lpData, p->header[p->idx].dwBufferLength, p->sample->sample_size, p->sample->channels, p->lvolume, p->rvolume);
        p->header[p->idx].dwBytesRecorded = p->header[p->idx].dwBufferLength;
        p->header[p->idx].dwFlags &= ~WHDR_DONE;

        waveOutWrite(p->stream, &p->header[p->idx], sizeof(WAVEHDR));
        p->idx = (p->idx + 1) % 2;
      } else {
        p->state = STATE_STOPPED;
        p->idx = 0;

        _pcm_rewind(p);

        if (p->cb) {
          p->cb(p->state);
        }
      }

      ReleaseMutex(p->mutex);

      break;
    }
  }

  return err;
}

static DJ_HANDLE players_mutex = NULL;
static struct pcm_player* players = NULL;
static DJ_HANDLE pool_mutex = NULL;
static struct pcm_player* pool = NULL;

DJ_RESULT pcm_init() {
  players = NULL;
  pool = NULL;

  players_mutex = CreateMutex(NULL, FALSE, NULL);
  if (players_mutex == NULL)
    return MMSYSERR_ERROR;

  pool_mutex = CreateMutex(NULL, FALSE, NULL);
  if (pool_mutex == NULL)
    return MMSYSERR_ERROR;

  _pcm_player_proc_done = false;
  _pcm_player_proc_handle = CreateThread(NULL, 0, _pcm_player_proc, NULL, 0, NULL);
  if (_pcm_player_proc_handle == NULL) {
    fprintf(stderr, "CreateThread failed %lu\n", GetLastError());
    return MMSYSERR_ERROR;
  }

  return MMSYSERR_NOERROR;
}

void pcm_shutdown() {
  // repeatedly close the first player in the list until the list is empty.
  struct pcm_player* tmp = players;
  while (tmp != NULL) {
    pcm_sound_close(tmp);

    tmp = players;
  }

  // all the players will have been moved to the pool so loop through and free them
  tmp = pool;
  while (tmp != NULL) {
    pool = pool->next;
    _pcm_player_free(tmp);
    tmp = pool;
  }

  _pcm_player_proc_done = true;
  WaitForSingleObject(_pcm_player_proc_handle, INFINITE);

  // At this point all handles are closed and the global list empty.
  CloseHandle(players_mutex);
  CloseHandle(pool_mutex);
}

static struct pcm_player* _pcm_player_pool_remove() {
  struct pcm_player* p = NULL;
  WaitForSingleObject(pool_mutex, INFINITE);
  if (pool != NULL) { // if a player is available in the pool, grab that
    p = pool;
    pool = pool->next;
    ReleaseMutex(pool_mutex);

    p->state = STATE_STOPPED;
    p->looping = 0;
    p->lvolume = 65536;
    p->rvolume = 65536;
    p->stream = 0;
    p->thread = 0;
    p->sample = NULL;
    p->next = NULL;
    p->cb = NULL;
  }

  ReleaseMutex(pool_mutex);
  return p;
}

static struct pcm_player* _pcm_player_load(pcm_notify_cb callback) {
  struct pcm_player* p = NULL;
  
  p = _pcm_player_pool_remove();
  if(p == NULL) { // otherwise, create a new one

    p = (struct pcm_player*)malloc(sizeof(struct pcm_player));
    if (p != NULL) {
      // Initialize the double buffers.
      ZeroMemory(&p->header[0], sizeof(WAVEHDR));
      p->header[0].lpData = (char*)malloc(MAX_BUFFER_SIZE);
      if (p->header[0].lpData == NULL)
        goto error1;
      p->header[0].dwBufferLength = p->header[0].dwBytesRecorded = MAX_BUFFER_SIZE;

      ZeroMemory(&p->header[1], sizeof(WAVEHDR));
      p->header[1].lpData = (char*)malloc(MAX_BUFFER_SIZE);
      if (p->header[1].lpData == NULL)
        goto error2;
      p->header[1].dwBufferLength = p->header[1].dwBytesRecorded = MAX_BUFFER_SIZE;

      p->state = STATE_STOPPED;
      p->looping = 0;
      p->lvolume = 65536;
      p->rvolume = 65536;
      p->stream = 0;
      p->thread = 0;
      p->sample = NULL;
      p->next = NULL;
      p->cb = NULL;

      p->mutex = CreateMutex(NULL, FALSE, NULL);
      if (p->mutex == NULL) {
        fprintf(stderr, "CreateMutex failed %lu\n", GetLastError());
        goto error3;
      }
    }
  }
  
  if (p != NULL) {
    p->cb = callback;
  }

  return p;

error3:
  free(p->header[1].lpData);

error2:
  free(p->header[0].lpData);

error1:
  free(p);
  return NULL;
}

static void _pcm_player_free(struct pcm_player* p) {
  if (p == NULL) {
    return;
  }

  CloseHandle(p->mutex);
  free(p->header[0].lpData);
  free(p->header[1].lpData);
  free(p);
}

static void _pcm_player_unload(struct pcm_player* p) {
  if (p->sample) {
    _pcm_sample_free(p->sample);
    p->sample = NULL;
  }

  _pcm_player_pool_add(p);
}

static void _pcm_player_pool_add(struct pcm_player* p) {
  WaitForSingleObject(pool_mutex, INFINITE);
  p->next = pool;
  pool = p;
  ReleaseMutex(pool_mutex);
}

static struct pcm_sample* _pcm_sample_create(unsigned int sample_rate, unsigned int sample_size, unsigned int channels, unsigned char* buf, unsigned int len) {
  struct pcm_sample* s = (struct pcm_sample*)malloc(sizeof(struct pcm_sample));
  if (s == NULL)
    goto error1;

  s->sample_rate = sample_rate;
  s->sample_size = sample_size;
  s->channels = channels;
  s->raw_bytes = s->ptr = (unsigned char*)malloc(len);
  if (s->raw_bytes == NULL)
    goto error2;

  memcpy(s->raw_bytes, buf, len);
  s->raw_len = len;

  return s;

error2:
  free(s);

error1:
  return NULL;
}

static void _pcm_sample_free(struct pcm_sample* s) {
  if (s != NULL) {
    free(s->raw_bytes);
    free(s);
  }
}

DJ_HANDLE pcm_sound_open(unsigned int sample_rate, unsigned int sample_size, unsigned int channels, unsigned char* buf, unsigned int len, pcm_notify_cb callback) {

  unsigned int err = MMSYSERR_NOERROR;
  struct pcm_player* p = NULL;

  p = _pcm_player_load(callback);
  if (p == NULL) {
    goto error1;
  }

  p->sample = _pcm_sample_create(sample_rate, sample_size, channels, buf, len);
  if (p->sample == NULL) {
    goto error2;
  }

  if(_pcm_stream_open(p) != NULL) {
    // Score is loaded and ready. Add the player to our global list
    // and return the "HANDLE" to the user.
    return _pcm_player_list_add(p);
  }

  _pcm_sample_free(p->sample);
  p->sample = NULL;

error2:
  _pcm_player_unload(p);
  p = NULL;

error1:
  return NULL;
}

static boolean _pcm_is_handle_valid(DJ_HANDLE h) {
  boolean res = false;
  if (h != NULL) {
    WaitForSingleObject(players_mutex, INFINITE);
    struct pcm_player* p = players;

    while (p != NULL) {
      if (p == h) {
        res = true;
        break;
      } else {
        p = p->next;
      }
    }
    ReleaseMutex(players_mutex);
  }

  return res;
}

static struct pcm_player* _pcm_player_list_add(struct pcm_player* p) {
  WaitForSingleObject(players_mutex, INFINITE);
  p->next = players;
  players = p;
  ReleaseMutex(players_mutex);

  return players;
}

static struct pcm_player* _pcm_player_list_remove(DJ_HANDLE h) {
  struct pcm_player* p = NULL;
  if (h != NULL) {
    WaitForSingleObject(players_mutex, INFINITE);
    p = players;

    if (p == h) {
      players = players->next;
      p->next = NULL;
    } else {
      struct pcm_player* tmp = p;
      while (p != h && p != NULL) {
        tmp = p;
        p = p->next;
      }
      if (p != NULL) {
        tmp->next = p->next;
        p->next = NULL;
      }
    }
    ReleaseMutex(players_mutex);
  }

  return p;
}

static struct pcm_player* _pcm_stream_open(struct pcm_player* p) {
  unsigned int err;
  WAVEFORMATEX wfx;

  wfx.nSamplesPerSec = p->sample->sample_rate;
  wfx.wBitsPerSample = p->sample->sample_size;
  wfx.nChannels = p->sample->channels;
  wfx.cbSize = 0;
  wfx.wFormatTag = WAVE_FORMAT_PCM;
  wfx.nBlockAlign = (wfx.wBitsPerSample >> 3) * wfx.nChannels;
  wfx.nAvgBytesPerSec = wfx.nBlockAlign * wfx.nSamplesPerSec;

  if (p->stream == NULL) {
    err = waveOutOpen(&p->stream, WAVE_MAPPER, &wfx, (DWORD_PTR)_pcm_callback_proc, (DWORD_PTR)p, CALLBACK_FUNCTION);
    if (err != MMSYSERR_NOERROR) {
      fprintf(stderr, "_pcm_stream_open: waveOutOpen %d\n", err);
      return NULL;
    }
  }

  err = waveOutPrepareHeader(p->stream, &p->header[0], sizeof(WAVEHDR));
  if (err != MMSYSERR_NOERROR) {
    fprintf(stderr, "_pcm_stream_open: waveOutPrepareHeader %d\n", err);
    goto error;
  }

  err = waveOutPrepareHeader(p->stream, &p->header[1], sizeof(WAVEHDR));
  if (err != MMSYSERR_NOERROR) {
    fprintf(stderr, "_pcm_stream_open: waveOutPrepareHeader %d\n", err);
    goto error;
  }

  return p;

error:
  err = waveOutClose(p->stream);
  if (err != MMSYSERR_NOERROR) {
    fprintf(stderr, "_pcm_stream_open: waveOutClose %d\n", err);
    goto error;
  }

  return NULL;
}

static void _pcm_stream_close(struct pcm_player* p) {
  unsigned int err = waveOutReset(p->stream);
  if (err != MMSYSERR_NOERROR) {
    fprintf(stderr, "_pcm_stream_close: waveOutReset failed %d\n", err);
  }
  err = waveOutUnprepareHeader(p->stream, &p->header[0], sizeof(WAVEHDR));
  if (err != MMSYSERR_NOERROR) {
    fprintf(stderr, "_pcm_stream_close: midiOutUnprepareHeader %d\n", err);
  }
  err = waveOutUnprepareHeader(p->stream, &p->header[1], sizeof(WAVEHDR));
  if (err != MMSYSERR_NOERROR) {
    fprintf(stderr, "_pcm_stream_close: midiOutUnprepareHeader %d\n", err);
  }
  err = waveOutClose(p->stream);
  if (err != MMSYSERR_NOERROR) {
    fprintf(stderr, "_pcm_stream_close: waveOutClose %d\n", err);
  }

  p->stream = 0;
}

void pcm_sound_close(DJ_HANDLE h) {
  struct pcm_player* p = _pcm_player_list_remove(h);
  if (p == NULL) {
    return;
  }

  // If it's still playing, stop it.
  WaitForSingleObject(p->mutex, INFINITE);
  if (p->state != STATE_STOPPED) {

    p->state = STATE_STOPPED;
    p->idx = 0;

    _pcm_rewind(p);
  }

  ReleaseMutex(p->mutex);

  // Player should be in the STATE_STOPPED state. No existing
  // handles can restart it. Let's close things out.
  _pcm_stream_close(p);
  _pcm_player_unload(p);

  return;
}

DJ_RESULT pcm_play(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;
  unsigned int err = MMSYSERR_NOERROR;

  if (_pcm_is_handle_valid(h) == false) {
    printf("pcm_play(): invalid handle %p\n", h);
    return MMSYSERR_INVALPARAM;
  }

  WaitForSingleObject(p->mutex, INFINITE);
  if (p->state == STATE_STOPPED) {
    p->idx = 0;
    _pcm_get_streambuf(p->sample, (unsigned char*)p->header[0].lpData, (unsigned int*)&p->header[0].dwBufferLength);
    _pcm_adjust_volume((unsigned char*)p->header[0].lpData, p->header[0].dwBufferLength, p->sample->sample_size, p->sample->channels, p->lvolume, p->rvolume);
    p->header[0].dwBytesRecorded = p->header[0].dwBufferLength;
    p->header[0].dwFlags &= ~WHDR_DONE; // clear the done flag to reuse the buffer

    _pcm_get_streambuf(p->sample, (unsigned char*)p->header[1].lpData, (unsigned int*)&p->header[1].dwBufferLength);
    _pcm_adjust_volume((unsigned char*)p->header[1].lpData, p->header[1].dwBufferLength, p->sample->sample_size, p->sample->channels, p->lvolume, p->rvolume);
    p->header[1].dwBytesRecorded = p->header[1].dwBufferLength;
    p->header[1].dwFlags &= ~WHDR_DONE; // clear the done flag to reuse the buffer

    if (p->header[0].dwBufferLength > 0) {
      p->state = STATE_PLAYING;
      err = waveOutWrite(p->stream, &p->header[0], sizeof(WAVEHDR));
    }

    if (p->header[1].dwBufferLength > 0) {
      err = waveOutWrite(p->stream, &p->header[1], sizeof(WAVEHDR));
    }
  }

  ReleaseMutex(p->mutex);

  return err;
}

DJ_RESULT pcm_stop(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;
  unsigned int err = MMSYSERR_NOERROR;

  if (_pcm_is_handle_valid(h) == false) {
    return MMSYSERR_INVALPARAM;
  }

  WaitForSingleObject(p->mutex, INFINITE);
  if (p->state != STATE_STOPPED) {
    p->state = STATE_STOPPED;
    p->idx = 0;

    waveOutReset(p->stream);
    _pcm_rewind(p);

    if (p->cb) {
      p->cb(p->state);
    }
  }
  ReleaseMutex(p->mutex);

  return MMSYSERR_NOERROR;
}

DJ_RESULT pcm_pause(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;
  unsigned int err = MMSYSERR_NOERROR;

  if (_pcm_is_handle_valid(h) == false) {
    return MMSYSERR_INVALPARAM;
  }

  WaitForSingleObject(p->mutex, INFINITE);
  if (p->state == STATE_PLAYING) {
    err = waveOutPause(p->stream);
    if (err == MMSYSERR_NOERROR) {
      p->state = STATE_PAUSED;
    } else {
      printf("err pausing: %d\n", err);
      p->state = STATE_ERROR;
    }
  }
  ReleaseMutex(p->mutex);

  return err;
}

DJ_RESULT pcm_resume(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;
  unsigned int err = MMSYSERR_NOERROR;

  if (_pcm_is_handle_valid(h) == false) {
    return MMSYSERR_INVALPARAM;
  }

  WaitForSingleObject(p->mutex, INFINITE);
  if (p->state == STATE_PAUSED) {
    err = waveOutRestart(p->stream);
    if (err == MMSYSERR_NOERROR) {
      p->state = STATE_PLAYING;
    } else {
      printf("err restart: %d\n", err);
      p->state = STATE_ERROR;
    }
  }
  ReleaseMutex(p->mutex);

  return err;
}

DJ_RESULT pcm_set_volume_left(DJ_HANDLE h, unsigned int level) {
  unsigned int old = 0, vol = 0;
  struct pcm_player* p = (struct pcm_player*)h;
  unsigned int err = MMSYSERR_NOERROR;

  if (_pcm_is_handle_valid(h)) {
    p->lvolume = level;
    return err;
  }

  //err = waveOutGetVolume((HWAVEOUT)WAVE_MAPPER, (LPDWORD)&old);
  //if (err == MMSYSERR_NOERROR) {
  //  vol = MAKELONG(LOWORD(level), HIWORD(old));
  //  err = waveOutSetVolume((HWAVEOUT)WAVE_MAPPER, vol);
  //}

  return err;
}

DJ_RESULT pcm_set_volume_right(DJ_HANDLE h, unsigned int level) {
  unsigned int old = 0, vol = 0;
  struct pcm_player* p = (struct pcm_player*)h;
  unsigned int err = MMSYSERR_NOERROR;

  if (_pcm_is_handle_valid(h)) {
    p->rvolume = level;
    return err;
  }

  //err = waveOutGetVolume((HWAVEOUT)WAVE_MAPPER, (LPDWORD)&old);
  //if (err == MMSYSERR_NOERROR) {
  //  vol = MAKELONG(LOWORD(old), LOWORD(level));
  //  err = waveOutSetVolume((HWAVEOUT)WAVE_MAPPER, vol);
  //}

  return err;
}

DJ_RESULT pcm_set_volume(DJ_HANDLE h, unsigned int level) {
  unsigned int err = MMSYSERR_NOERROR;

  err = pcm_set_volume_left(h, level);
  if (err == MMSYSERR_NOERROR)
    err = pcm_set_volume_right(h, level);

  return err;
}

DJ_RESULT pcm_volume_left(DJ_HANDLE h, unsigned int dir) {
  unsigned int old = 0, vol = 0;
  const unsigned int val = 3277;
  struct pcm_player* p = (struct pcm_player*)h;
  unsigned int err = MMSYSERR_NOERROR;
  HWAVEOUT stream = (HWAVEOUT)WAVE_MAPPER;
  boolean valid = false;

  valid = _pcm_is_handle_valid(h);
  if (valid == true) {
//    WaitForSingleObject(p->mutex, INFINITE);
    stream = p->stream;
  }

  err = waveOutGetVolume(stream, (LPDWORD)&old);
  if (err == MMSYSERR_NOERROR) {
    vol = LOWORD(old);

    if (dir == VOL_UP) {
      if (0xffff - vol <= val)
        vol = 0xffff;
      else
        vol += val;
    } else {
      if (vol <= val)
        vol = 0;
      else
        vol -= val;
    }

    vol = MAKELONG(vol, HIWORD(old));
    err = waveOutSetVolume(stream, vol);
  }

  if (valid == true) {
//    ReleaseMutex(p->mutex);
  }

  return err;
}

DJ_RESULT pcm_volume_right(DJ_HANDLE h, unsigned int dir) {
  unsigned int old = 0, vol = 0;
  const unsigned int val = 3277;
  struct pcm_player* p = (struct pcm_player*)h;
  unsigned int err = MMSYSERR_NOERROR;
  HWAVEOUT stream = (HWAVEOUT)WAVE_MAPPER;
  boolean valid = false;

  valid = _pcm_is_handle_valid(h);
  if (valid == true) {
//    WaitForSingleObject(p->mutex, INFINITE);
    stream = p->stream;
  }

  err = waveOutGetVolume(stream, (LPDWORD)&old);
  if (err == MMSYSERR_NOERROR) {
    vol = HIWORD(old);

    if (dir == VOL_UP) {
      if (0xffff - vol <= val)
        vol = 0xffff;
      else
        vol += val;
    } else {
      if (vol <= val)
        vol = 0;
      else
        vol -= val;
    }

    vol = MAKELONG(LOWORD(old), vol);
    err = waveOutSetVolume(stream, vol);
  }

  if (valid == true) {
//    ReleaseMutex(p->mutex);
  }

  return err;
}

DJ_RESULT pcm_volume(DJ_HANDLE h, unsigned int dir) {
  unsigned int err;

  err = pcm_volume_left(h, dir);
  if (err == MMSYSERR_NOERROR)
    err = pcm_volume_right(h, dir);

  return err;
}

DJ_RESULT pcm_set_looping(DJ_HANDLE h, boolean looping) {
  struct pcm_player* p = (struct pcm_player*)h;
  unsigned int err = MMSYSERR_NOERROR;

  if (_pcm_is_handle_valid(h) == false) {
    return MMSYSERR_INVALPARAM;
  }

  WaitForSingleObject(p->mutex, INFINITE);

  p->looping = looping;

  ReleaseMutex(p->mutex);

  return MMSYSERR_NOERROR;
}

boolean pcm_is_looping(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;
  unsigned int err = MMSYSERR_NOERROR;
  boolean looping;

  if (_pcm_is_handle_valid(h) == false) {
    return false;
  }

  WaitForSingleObject(p->mutex, INFINITE);

  looping = p->looping;

  ReleaseMutex(p->mutex);

  return looping;
}

boolean pcm_is_playing(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;
  boolean ret = false;

  if (_pcm_is_handle_valid(h) == false)
    ret = false;
  else
    ret = (p->state == STATE_PLAYING);

  return ret;
}

boolean pcm_is_paused(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;
  boolean ret = false;

  if (_pcm_is_handle_valid(h) == false)
    ret = false;
  else
    ret = (p->state == STATE_PAUSED);

  return ret;
}

boolean pcm_is_stopped(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;
  boolean ret = false;

  if (_pcm_is_handle_valid(h) == false)
    ret = false;
  else
    ret = (p->state == STATE_STOPPED);

  return ret;
}

static DJ_RESULT _pcm_rewind(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;

  if (_pcm_is_handle_valid(h) == false)
    return MMSYSERR_INVALPARAM;

  if (p->sample != NULL)
    p->sample->ptr = p->sample->raw_bytes;

  return MMSYSERR_NOERROR;
}

static unsigned int _pcm_get_streambuf(struct pcm_sample* s, unsigned char* out, unsigned int* outlen) {
  unsigned int blocklen = (s->sample_size / 8) * s->channels;
  unsigned int streambufsize = MAX_BUFFER_SIZE - (MAX_BUFFER_SIZE % blocklen);

  unsigned int bytesread = s->ptr - s->raw_bytes;
  unsigned int bytesleft = s->raw_len - bytesread;

  *outlen = 0;
  if (bytesleft == 0)
    return 0;

  if (bytesleft >= streambufsize)
    bytesread = streambufsize;
  else
    bytesread = bytesleft;

  memcpy(out, s->ptr, bytesread);

  s->sample_size;
  s->ptr += bytesread;
  *outlen = bytesread;
  return 0;
}

static unsigned int _pcm_adjust_volume(unsigned char* out, unsigned int len, unsigned int sample_size, unsigned int channels, unsigned int lvol, unsigned int rvol) {
  unsigned int i, length;
  if (channels == 1)
    rvol = lvol;

  switch (sample_size) {
  case 8:
    length = len;
    lvol >>= 8;
    rvol >>= 8;
    for (i = 0; i < length; i++) {
      out[i] = (((int)out[i] - 128) * lvol / 256) + 128;
      i++;
      if (i < length)
        out[i] = (((int)out[i] - 128) * rvol / 256) + 128;
    }
    break;
  case 16:
  {
    short* s = (short*)out;
    length = len / 2;
    for (i = 0; i < length; i++) {
      s[i] = s[i] * lvol / 65536;
      i++;
      if (i < length)
        s[i] = s[i] * rvol / 65536;
    }
  }
  break;
  default:
    break;
  }

  return 0;
}

#ifdef PCM_PLAYER_STANDALONE

void standalone_callback(unsigned int val) {
  printf("\r       \rStopped");
}

static DJ_HANDLE* handles;
static unsigned int numhandles;

int main(int argc, char* argv[]) {
  unsigned char* filename;
  unsigned char* wavbuf = NULL;
  unsigned int wavbuflen = 0;
  unsigned char c;

  WAVEOUTCAPS caps;

  unsigned long n, i;
  unsigned int err;

  if (argc > 1) {
    numhandles = argc - 1;
    handles = malloc(numhandles * sizeof(DJ_HANDLE));
  } else {
    printf("Usage: %s <filename>\n", argv[0]);
    return 0;
  }

  n = waveOutGetNumDevs();
  if (n == 0) {
    fprintf(stderr, "No WAVE devices found!\n");
    return 0;
  }

  for (i = 0; i < n; i++) {
    if (!waveOutGetDevCaps(i, &caps, sizeof(WAVEOUTCAPS))) {
      printf("Device %lu: %ws\r\n", i, caps.szPname);
      if (caps.dwSupport & WAVECAPS_PITCH) {
        printf(" - supports pitch control.\n");
      }
      if (caps.dwSupport & WAVECAPS_VOLUME) {
        printf(" - supports volume control.\n");
      }
      if (caps.dwSupport & WAVECAPS_LRVOLUME) {
        printf(" - supports separate left and right volume control.\n");
      }
      if (caps.dwSupport & WAVECAPS_PLAYBACKRATE) {
        printf(" - supports playback rate control.\n");
      }
      if (caps.dwSupport & WAVECAPS_SYNC) {
        printf(" - the driver is synchronous and will block while playing a buffer.\n");
      }
      if (caps.dwSupport & WAVECAPS_SAMPLEACCURATE) {
        printf(" - returns sample-accurate position information.\n");
      }
      printf("\n");
    }
  }

  pcm_init();

  printf("loading\n");
  for (int i = 0; i < numhandles; i++) {
    unsigned char* filename = argv[i + 1];

    wavbuf = load_file(filename, &wavbuflen);
    if (wavbuf == NULL) {
      fprintf(stderr, "Failed to load file %s\n", filename);
      return 0;
    }

    DJ_HANDLE s = pcm_sound_open(11025, 8, 1, wavbuf, wavbuflen, standalone_callback);
    if (s == NULL) {
      printf("Failed to open sample.\n");
      return 0;
    }
    handles[i] = s;

    printf("Loaded %s\n", filename);
    free(wavbuf);
  }

  printf("\n(p) play/pause, (s) stop, (l) loop on/off, (q) quit, (+/-) volume up/down\n");
  printf("\r       \rStopped");
  err = 0;
  while ((c = _getch()) != 'q') {
    switch (c) {
    case 'l':
    case 'L':
      for (int i = 0; i < numhandles; i++) {
        pcm_set_looping(handles[i], !pcm_is_looping(handles[i]));
      }
      break;
    case 'p':
    case 'P':
      for (int i = 0; i < numhandles; i++) {
        if (pcm_is_stopped(handles[i])) {
          err = pcm_play(handles[i]);
          if (err != MMSYSERR_NOERROR) {
            //fprintf(stderr, "Error playing file %s, error %d\n", filename, err);
            goto error;
          }
          printf("\r       \rPlaying");
        } else if (pcm_is_playing(handles[i])) {
          err = pcm_pause(handles[i]);
          if (err != MMSYSERR_NOERROR) {
            //fprintf(stderr, "Error pausing file %s, error %d\n", filename, err);
            goto error;
          }
          printf("\r       \rPaused");
        } else if (pcm_is_paused(handles[i])) {
          err = pcm_resume(handles[i]);
          if (err != MMSYSERR_NOERROR) {
            //fprintf(stderr, "Error pausing file %s, error %d\n", filename, err);
            goto error;
          }
          printf("\r       \rPlaying");
        }
      }
      break;
    case 's':
    case 'S':
      for (int i = 0; i < numhandles; i++) {
        err = pcm_stop(handles[i]);
        if (err != MMSYSERR_NOERROR) {
          //fprintf(stderr, "Error stopping file %s, error %d\n", filename, err);
          goto error;
        }
        printf("\r       \rStopped");
      }
      break;
    case '-':
    case '_':
      for (int i = 0; i < numhandles; i++) {
        err = pcm_volume(handles[i], VOL_DOWN);
        if (err != MMSYSERR_NOERROR) {
          fprintf(stderr, "Error adjusting volume, error %d\n", err);
          goto error;
        }
      }
      break;
    case '=':
    case '+':
      for (int i = 0; i < numhandles; i++) {
        err = pcm_volume(handles[i], VOL_UP);
        if (err != MMSYSERR_NOERROR) {
          fprintf(stderr, "Error adjusting volume, error %d\n", err);
          goto error;
        }
      }
      break;
    case 'q':
    default:
      break;
    }
  }

error:
  for (int i = 0; i < numhandles; i++) {
    pcm_sound_close(handles[i]);
  }
  pcm_shutdown();

  return EXIT_SUCCESS;
}

unsigned char* load_file(unsigned char* filename, unsigned int* len) {
  unsigned char* buf;
  unsigned int ret;
  FILE* f = fopen((char*)filename, "rb");
  if (f == NULL)
    return 0;

  fseek(f, 0, SEEK_END);
  *len = ftell(f);
  fseek(f, 0, SEEK_SET);

  buf = (unsigned char*)malloc(*len);

  if (buf == 0) {
    fclose(f);
    return 0;
  }

  ret = fread(buf, 1, *len, f);
  fclose(f);

  if (ret != *len) {
    free(buf);
    return 0;
  }

  return buf;
}

#endif
