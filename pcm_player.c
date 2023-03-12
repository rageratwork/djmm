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

#define MAX_BUFFER_SIZE	1024

static unsigned int _pcm_get_streambuf(struct pcm_sample* s, unsigned char* out, unsigned int* outlen);
static unsigned int _pcm_adjust_volume(unsigned char* out, unsigned int len, unsigned int sample_size, unsigned int channels, unsigned int lvol, unsigned int rvol);

static struct pcm_player* _pcm_player_add(struct pcm_player* p);
static void _pcm_player_shutdown(struct pcm_player* p);
static void _pcm_player_free(struct pcm_player* p);

static void _pcm_sample_free(struct pcm_sample* s);

static void _pcm_close_stream(struct pcm_player* p);
static DJ_RESULT pcm_rewind(DJ_HANDLE h);

static void CALLBACK pcm_callback_proc(HWAVEOUT hmo, UINT wMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
  struct pcm_player* p = (struct pcm_player*)dwInstance;
  if (p == NULL)
    return;

  switch (wMsg) {
  case WOM_DONE:
    SetEvent(p->event);
    break;
  case WOM_OPEN:
    break;
  case WOM_CLOSE:
    break;
  }

}

static DWORD WINAPI _pcm_player_proc(LPVOID lpParameter) {
  struct pcm_player* p = (struct pcm_player*)lpParameter;
  unsigned int err = MMSYSERR_NOERROR;

  unsigned int idx = 0;

  while ((p->state != STATE_SHUTDOWN) && (p->state != STATE_ERROR)) {
    WaitForSingleObject(p->event, INFINITE);

    switch (p->state) {
    case STATE_STARTING:
      WaitForSingleObject(p->mutex, INFINITE);
      _pcm_get_streambuf(p->sample, (unsigned char*)p->header[0].lpData, (unsigned int*)&p->header[0].dwBufferLength);
      _pcm_adjust_volume((unsigned char*)p->header[0].lpData, p->header[0].dwBufferLength, p->sample->sample_size, p->sample->channels, p->lvolume, p->rvolume);
      p->header[0].dwBytesRecorded = p->header[0].dwBufferLength;
      p->header[0].dwFlags &= ~WHDR_DONE; // clear the done flag to reuse the buffer

      _pcm_get_streambuf(p->sample, (unsigned char*)p->header[1].lpData, (unsigned int*)&p->header[1].dwBufferLength);
      _pcm_adjust_volume((unsigned char*)p->header[1].lpData, p->header[1].dwBufferLength, p->sample->sample_size, p->sample->channels, p->lvolume, p->rvolume);
      p->header[1].dwBytesRecorded = p->header[1].dwBufferLength;
      p->header[1].dwFlags &= ~WHDR_DONE; // clear the done flag to reuse the buffer

      if (p->header[0].dwBufferLength > 0) {
        err = waveOutWrite(p->stream, &p->header[0], sizeof(WAVEHDR));
      }

      if (p->header[1].dwBufferLength > 0) {
        err = waveOutWrite(p->stream, &p->header[1], sizeof(WAVEHDR));
      }

      p->state = STATE_PLAYING;
      ReleaseMutex(p->mutex);

      break;
    case STATE_PLAYING:
      WaitForSingleObject(p->mutex, INFINITE);

      _pcm_get_streambuf(p->sample, (unsigned char*)p->header[idx].lpData, (unsigned int*)&p->header[idx].dwBufferLength);
      _pcm_adjust_volume((unsigned char*)p->header[idx].lpData, p->header[idx].dwBufferLength, p->sample->sample_size, p->sample->channels, p->lvolume, p->rvolume);
      p->header[idx].dwBytesRecorded = p->header[idx].dwBufferLength;
      if (p->header[idx].dwBufferLength > 0) {
        p->header[idx].dwFlags &= ~WHDR_DONE; // clear the done flag to reuse the buffer
        err = waveOutWrite(p->stream, &p->header[idx], sizeof(WAVEHDR));
        idx = (idx + 1) % 2;
      } else {
        if (p->looping) {
          pcm_rewind(p);
          _pcm_get_streambuf(p->sample, (unsigned char*)p->header[idx].lpData, (unsigned int*)&p->header[idx].dwBufferLength);
          _pcm_adjust_volume((unsigned char*)p->header[idx].lpData, p->header[idx].dwBufferLength, p->sample->sample_size, p->sample->channels, p->lvolume, p->rvolume);
          p->header[idx].dwBytesRecorded = p->header[idx].dwBufferLength;
          p->header[idx].dwFlags &= ~WHDR_DONE;
          waveOutWrite(p->stream, &p->header[idx], sizeof(WAVEHDR));
          idx = (idx + 1) % 2;
        } else {
          // one more buffer left playing, wait for it to finish
          p->state = STATE_STOPPING;
        }
      }

      ReleaseMutex(p->mutex);
      break;
    case STATE_STOPPING:
      WaitForSingleObject(p->mutex, INFINITE);

      pcm_rewind(p);

      SetEvent(p->event);

      p->state = STATE_STOPPED;
      ReleaseMutex(p->mutex);

      break;
    case STATE_STOPPED:
      idx = 0;
      WaitForSingleObject(p->mutex, INFINITE);

      if (p->cb)
        p->cb(p->state);

      SetEvent(p->ready);
      ReleaseMutex(p->mutex);

      break;
    }
  }

  return 0;
}

static DJ_HANDLE players_mutex = NULL;
static struct pcm_player* players = NULL;
static struct pcm_player* pool = NULL;

/*!
 * Initialize the PCM subsystem.
 *
 * This function initializes the global player handle list and associated
 * mutex.
 *
 * This function does not acquire a lock.
 *
 * @return Returns MMSYSERR_NOERROR if successful, MMSYSERR_ERROR if the mutex
 *  could not be initialized.
 */
DJ_RESULT pcm_init() {
  players = NULL;
  pool = NULL;

  players_mutex = CreateMutex(NULL, FALSE, NULL);
  if (players_mutex == NULL)
    return MMSYSERR_ERROR;

  return MMSYSERR_NOERROR;
}

void pcm_shutdown() {
  // repeatedly close the first player in the list until the list is empty.
  struct pcm_player* tmp = players;
  while (tmp != NULL) {
    pcm_sample_close(tmp);

    tmp = players;
  }

  // all the players will have been moved to the pool so loop through and free them
  tmp = pool;
  while (tmp != NULL) {
    pool = pool->next;
    _pcm_player_free(tmp);
    tmp = pool;
  }

  // At this point all handles are closed and the global list empty.
  CloseHandle(players_mutex);
}

static struct pcm_player* _pcm_player_init(pcm_notify_cb callback) {
  struct pcm_player* p = NULL;
  WaitForSingleObject(players_mutex, INFINITE);
  if (pool != NULL) { // if a player is available in the pool, grab that
    p = pool;
    pool = pool->next;

    p->state = STATE_STOPPED;
    p->looping = 0;
    p->lvolume = 65536;
    p->rvolume = 65536;
    p->stream = 0;
    p->thread = 0;
    p->sample = NULL;
    p->cb = callback;
    p->next = NULL;

    ReleaseMutex(players_mutex);
  } else { // otherwise, create a new one
    ReleaseMutex(players_mutex);

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
      p->cb = callback;
      p->next = NULL;

      p->mutex = CreateMutex(NULL, FALSE, NULL);
      if (p->mutex == NULL) {
        fprintf(stderr, "CreateMutex failed %lu\n", GetLastError());
        goto error3;
      }

      p->event = CreateEvent(0, FALSE, TRUE, 0); // initially set to signalled so we get the ready event below
      if (p->event == NULL) {
        fprintf(stderr, "CreateEvent failed %lu\n", GetLastError());
        goto error4;
      }

      // Create the event that signals when the player is stopped and ready.
      // This event is used in this function when the thread is first
      // initialized. It is also used in mus_stop() to signal when the thread
      // has settled into its STATE_STOPPED state.
      p->ready = CreateEvent(0, FALSE, FALSE, 0);
      if (p->ready == NULL) {
        fprintf(stderr, "CreateEvent failed %lu\n", GetLastError());
        goto error5;
      }

      // Finally, spawn the worker thread.
      p->thread = CreateThread(NULL, 0, _pcm_player_proc, p, 0, NULL);
      if (p->thread == NULL) {
        fprintf(stderr, "CreateThread failed %lu\n", GetLastError());
        goto error6;
      }

      // Make sure the thread is spun up and ready.
      WaitForSingleObject(p->ready, INFINITE);
    }
  }

  return p;

error6:
  CloseHandle(p->ready);

error5:
  CloseHandle(p->event);

error4:
  CloseHandle(p->mutex);

error3:
  free(p->header[1].lpData);

error2:
  free(p->header[0].lpData);

error1:
  free(p);
  return NULL;
}

static void _pcm_player_free(struct pcm_player* p) {
  if (p == NULL)
    return;

  p->state = STATE_SHUTDOWN;
  SetEvent(p->event);
  WaitForSingleObject(p->thread, INFINITE);

  CloseHandle(p->event);
  CloseHandle(p->ready);
  CloseHandle(p->mutex);
  CloseHandle(p->thread);
  free(p->header[0].lpData);
  free(p->header[1].lpData);
  free(p);
}

static void _pcm_player_shutdown(struct pcm_player* p) {
  WaitForSingleObject(players_mutex, INFINITE);
  if (p->sample) {
    _pcm_sample_free(p->sample);
    p->sample = NULL;
  }

  p->next = pool;
  pool = p;
  ReleaseMutex(players_mutex);
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

DJ_HANDLE pcm_sample_open(unsigned int sample_rate, unsigned int sample_size, unsigned int channels, unsigned char* buf, unsigned int len, pcm_notify_cb callback) {
  WAVEFORMATEX wfx;
  unsigned int err = MMSYSERR_NOERROR;
  struct pcm_player* p = NULL;

  p = _pcm_player_init(callback);
  if (p == NULL)
    goto error1;

  p->sample = _pcm_sample_create(sample_rate, sample_size, channels, buf, len);
  if (p->sample == NULL) {
    goto error2;
  }

  wfx.nSamplesPerSec = p->sample->sample_rate;
  wfx.wBitsPerSample = p->sample->sample_size;
  wfx.nChannels = p->sample->channels;
  wfx.cbSize = 0;
  wfx.wFormatTag = WAVE_FORMAT_PCM;
  wfx.nBlockAlign = (wfx.wBitsPerSample >> 3) * wfx.nChannels;
  wfx.nAvgBytesPerSec = wfx.nBlockAlign * wfx.nSamplesPerSec;

  err = waveOutOpen(&p->stream, WAVE_MAPPER, &wfx, (DWORD_PTR)pcm_callback_proc, (DWORD_PTR)p, CALLBACK_FUNCTION);
  if (err != MMSYSERR_NOERROR)
    goto error2;

  err = waveOutPrepareHeader(p->stream, &p->header[0], sizeof(WAVEHDR));
  if (err != MMSYSERR_NOERROR)
    goto error3;

  err = waveOutPrepareHeader(p->stream, &p->header[1], sizeof(WAVEHDR));
  if (err != MMSYSERR_NOERROR)
    goto error3;

  // Score is loaded and ready. Add the player to our global list
  // and return the "HANDLE" to the user.
  return _pcm_player_add(p);

error3:
  waveOutClose(p->stream);

error2:
  _pcm_player_shutdown(p);
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

static struct pcm_player* _pcm_player_add(struct pcm_player* p) {
  WaitForSingleObject(players_mutex, INFINITE);
  p->next = players;
  players = p;
  ReleaseMutex(players_mutex);
  return p;
}

static struct pcm_player* _pcm_player_remove(DJ_HANDLE h) {
  struct pcm_player* p = NULL;
  if (h != NULL) {
    WaitForSingleObject(players_mutex, INFINITE);
    p = players;

    if (h == players) {
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

static void _pcm_close_stream(struct pcm_player* p) {
  unsigned int err;
  waveOutReset(p->stream);
  err = waveOutUnprepareHeader(p->stream, &p->header[0], sizeof(WAVEHDR));
  if (err != MMSYSERR_NOERROR)
    printf("midiOutUnprepareHeader %d\n", err);
  err = waveOutUnprepareHeader(p->stream, &p->header[1], sizeof(WAVEHDR));
  if (err != MMSYSERR_NOERROR)
    printf("midiOutUnprepareHeader %d\n", err);
  waveOutClose(p->stream);
  p->stream = 0;
}

void pcm_sample_close(DJ_HANDLE h) {
  struct pcm_player* p = _pcm_player_remove(h);
  if (p == NULL) {
    return;
  }

  // The player has been removed from the global list. Any further calls
  // using this handle will return MMSYSERR_INVALPARAM.

  // Start shutting down the thread. If it's still playing, stop it.
  WaitForSingleObject(p->mutex, INFINITE);
  if (p->state != STATE_STOPPED) {
    p->state = STATE_STOPPING;
    ResetEvent(p->ready);

    SetEvent(p->event);
    ReleaseMutex(p->mutex);
    WaitForSingleObject(p->ready, INFINITE);
  } else
    ReleaseMutex(p->mutex);

  // Player thread should be in the STATE_STOPPED state. No existing
  // handles can restart it. Let's close things out.
  _pcm_close_stream(p);
  _pcm_player_shutdown(p);

  // all the resources for the sample should be released and the
  // player is in the pool which will allow us to not need to
  // wait for another thread to spin up the next time we need
  // a player.

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
    p->state = STATE_STARTING;
    SetEvent(p->event);
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
    ResetEvent(p->ready);
    p->state = STATE_STOPPING;

    SetEvent(p->event);
    ReleaseMutex(p->mutex);
    WaitForSingleObject(p->ready, INFINITE);
  } else {
    ReleaseMutex(p->mutex);
  }

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
    if (err == MMSYSERR_NOERROR)
      p->state = STATE_PAUSED;
    else {
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
    if (err == MMSYSERR_NOERROR)
      p->state = STATE_PLAYING;
    else {
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
    WaitForSingleObject(p->mutex, INFINITE);

    p->lvolume = level;
    ReleaseMutex(p->mutex);
    return err;
  }

  err = waveOutGetVolume((HWAVEOUT)WAVE_MAPPER, (LPDWORD)&old);
  if (err == MMSYSERR_NOERROR) {
    vol = MAKELONG(LOWORD(level), HIWORD(old));
    err = waveOutSetVolume((HWAVEOUT)WAVE_MAPPER, vol);
  }

  return err;
}

DJ_RESULT pcm_set_volume_right(DJ_HANDLE h, unsigned int level) {
  unsigned int old = 0, vol = 0;
  struct pcm_player* p = (struct pcm_player*)h;
  unsigned int err = MMSYSERR_NOERROR;

  if (_pcm_is_handle_valid(h)) {
    WaitForSingleObject(p->mutex, INFINITE);

    p->rvolume = level;
    ReleaseMutex(p->mutex);
    return err;
  }

  err = waveOutGetVolume((HWAVEOUT)WAVE_MAPPER, (LPDWORD)&old);
  if (err == MMSYSERR_NOERROR) {
    vol = MAKELONG(LOWORD(old), LOWORD(level));
    err = waveOutSetVolume((HWAVEOUT)WAVE_MAPPER, vol);
  }


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
    stream = p->stream;
    WaitForSingleObject(p->mutex, INFINITE);
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

  if (valid == true)
    ReleaseMutex(p->mutex);

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
    stream = p->stream;
    WaitForSingleObject(p->mutex, INFINITE);
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

  if (valid == true)
    ReleaseMutex(p->mutex);

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

static DJ_RESULT pcm_rewind(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;

  if (_pcm_is_handle_valid(h) == false)
    return MMSYSERR_INVALPARAM;

//  waveOutReset(p->stream);

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

void pcm_callback(unsigned int val) {
  printf("\r       \rStopped");
}

int main(int argc, char* argv[]) {
  unsigned char* filename;
  unsigned char* wavbuf = NULL;
  unsigned int wavbuflen = 0;
  unsigned char c;

  WAVEOUTCAPS caps;

  unsigned long n, i;
  unsigned int err;

  DJ_HANDLE s = NULL;

  if (argc > 1)
    filename = (unsigned char*)argv[1];
  else {
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
      printf("Device %lu: %s\r\n", i, caps.szPname);
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

  wavbuf = load_file(filename, &wavbuflen);
  if (wavbuf == NULL) {
    fprintf(stderr, "Failed to load file %s\n", filename);
    return 0;
  }

  pcm_init();

  s = pcm_sample_open(11025, 8, 1, wavbuf, wavbuflen, pcm_callback);
  if (s == NULL) {
    printf("Failed to open sample.\n");
    return 0;
  }

  printf("Loaded %s\n", filename);

  printf("\n(p) play/pause, (s) stop, (l) loop on/off, (q) quit, (+/-) volume up/down\n");
  printf("\r       \rStopped");
  err = 0;
  while ((c = _getch()) != 'q') {
    switch (c) {
    case 'l':
    case 'L':
      pcm_set_looping(s, !pcm_is_looping(s));
      break;
    case 'p':
    case 'P':
      if (pcm_is_stopped(s)) {
        err = pcm_play(s);
        if (err != MMSYSERR_NOERROR) {
          fprintf(stderr, "Error playing file %s, error %d\n", filename, err);
          goto error;
        }
        printf("\r       \rPlaying");
      } else if (pcm_is_playing(s)) {
        err = pcm_pause(s);
        if (err != MMSYSERR_NOERROR) {
          fprintf(stderr, "Error pausing file %s, error %d\n", filename, err);
          goto error;
        }
        printf("\r       \rPaused");
      } else if (pcm_is_paused(s)) {
        err = pcm_resume(s);
        if (err != MMSYSERR_NOERROR) {
          fprintf(stderr, "Error pausing file %s, error %d\n", filename, err);
          goto error;
        }
        printf("\r       \rPlaying");
      }
      break;
    case 's':
    case 'S':
      err = pcm_stop(s);
      if (err != MMSYSERR_NOERROR) {
        fprintf(stderr, "Error stopping file %s, error %d\n", filename, err);
        goto error;
      }
      printf("\r       \rStopped");
      break;
    case '-':
    case '_':
      err = pcm_volume(s, VOL_DOWN);
      if (err != MMSYSERR_NOERROR) {
        fprintf(stderr, "Error adjusting volume, error %d\n", err);
        goto error;
      }
      break;
    case '=':
    case '+':
      err = pcm_volume(s, VOL_UP);
      if (err != MMSYSERR_NOERROR) {
        fprintf(stderr, "Error adjusting volume, error %d\n", err);
        goto error;
      }
      break;
    case 'q':
    default:
      break;
    }
  }

error:
  pcm_sample_close(s);
  pcm_shutdown();

  free(wavbuf);

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
