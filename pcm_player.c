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

#include "SDL3/sdl.h"
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

#include "SDL3/sdl.h"

unsigned char* load_file(unsigned char* filename, unsigned int* len);

#endif

struct pcm_player {
  HANDLE mutex;

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
  unsigned int sample_count;
  unsigned int channels;
  unsigned int silence;

  unsigned char* ptr;

  unsigned char* raw_bytes;
  unsigned int raw_len;
};

#pragma pack(push,1)
struct dmx_header {
  unsigned short format;
  unsigned short sample_rate;
  unsigned int length; // # of samples + 32 bytes of padding
  unsigned char padding[16];
  unsigned char samples[]; // size is length - 32 bytes
  // ends with another padding[16];
};
#pragma pack(pop)

#define MAX_BUFFER_SIZE	1024 / 2

static void _pcm_audio_callback(void* userdata, Uint8* stream, int len);

static struct pcm_player* _pcm_player_load(pcm_notify_cb callback);
static void _pcm_player_unload(struct pcm_player* p);

static struct pcm_player* _pcm_player_list_add(struct pcm_player* p);
static struct pcm_player* _pcm_player_list_remove(DJ_HANDLE h);

static void _pcm_player_free(struct pcm_player* p);

static struct pcm_sample* _pcm_sample_create(unsigned char* buf, unsigned int len);
static void _pcm_sample_free(struct pcm_sample* s);

static struct pcm_player* _pcm_stream_open(struct pcm_player* p);
static void _pcm_stream_close(struct pcm_player* p);

static void _pcm_player_pool_add(struct pcm_player* p);
static struct pcm_player* _pcm_player_pool_remove();

static boolean _pcm_is_handle_valid(DJ_HANDLE h);

static DJ_RESULT _pcm_lock(DJ_HANDLE h);
static DJ_RESULT _pcm_unlock(DJ_HANDLE h);
static DJ_RESULT _pcm_rewind(DJ_HANDLE h);

static unsigned int _pcm_adjust_volume(unsigned char* out, unsigned int len, struct pcm_player* p);

static DJ_HANDLE players_mutex = NULL;
static struct pcm_player* players = NULL;

static DJ_HANDLE pool_mutex = NULL;
static struct pcm_player* pool = NULL;

DJ_RESULT pcm_init() {
  players = NULL;
  pool = NULL;

  players_mutex = CreateMutex(NULL, FALSE, NULL);
  if (players_mutex == NULL)
    return ERROR;

  pool_mutex = CreateMutex(NULL, FALSE, NULL);
  if (pool_mutex == NULL)
    return ERROR;

  return NOERROR;
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

  // At this point all handles are closed and the global list empty.
  CloseHandle(players_mutex);
  CloseHandle(pool_mutex);
}


DJ_HANDLE pcm_sound_open(unsigned char* buf, unsigned int len, pcm_notify_cb callback) {
  struct pcm_player* p = NULL;

  p = _pcm_player_load(callback);
  if (p == NULL) {
    goto error1;
  }

  p->sample = _pcm_sample_create(buf, len);
  if (p->sample == NULL) {
    goto error2;
  }

  if (_pcm_stream_open(p) != NULL) {
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

void pcm_sound_close(DJ_HANDLE h) {
  struct pcm_player* p = _pcm_player_list_remove(h);
  if (p == NULL) {
    return;
  }

  // If it's still playing, stop it.
  if (p->state != STATE_STOPPED) {
    pcm_stop(p);
  }

  _pcm_stream_close(p);
  _pcm_player_unload(p);

  return;
}

DJ_RESULT pcm_play(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;
  unsigned int err = NOERROR;

  if (!_pcm_is_handle_valid(h)) {
    return INVALID_PARAM;
  }

  _pcm_lock(p);
  if ((p->state == STATE_PLAYING) || (p->state == STATE_PAUSED) || (p->state == STATE_STOPPED)) {
    _pcm_rewind(p);
    p->state = STATE_PLAYING;
    if (SDL_PlayAudioDevice(p->device) < 0) {
      printf("pcm_play(): SDL_PlayAudioDevice %s\n", SDL_GetError());
    }
  }
  _pcm_unlock(p);

  return err;
}

DJ_RESULT pcm_pause(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;
  unsigned int err = NOERROR;

  if(!_pcm_is_handle_valid(h)) {
    return INVALID_PARAM;
  }

  _pcm_lock(p);
  if (p->state == STATE_PLAYING) {
    p->state = STATE_PAUSED;
    if (SDL_PauseAudioDevice(p->device) < 0) {
      printf("pcm_pause(): SDL_PauseAudioDevice %s\n", SDL_GetError());
    }
  }
  _pcm_unlock(p);

  return err;
}

DJ_RESULT pcm_resume(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;
  unsigned int err = NOERROR;

  if (!_pcm_is_handle_valid(h)) {
    return INVALID_PARAM;
  }

  _pcm_lock(p);
  if (p->state == STATE_PAUSED) {
    p->state = STATE_PLAYING;
    if (SDL_PlayAudioDevice(p->device) < 0) {
      printf("pcm_resume(): SDL_PlayAudioDevice %s\n", SDL_GetError());
    }
  }
  _pcm_unlock(p);

  return err;
}

DJ_RESULT pcm_stop(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;

  if (!_pcm_is_handle_valid(h)) {
    return INVALID_PARAM;
  }

  _pcm_lock(p);
  p->state = STATE_STOPPED;
  if (SDL_PauseAudioDevice(p->device) < 0) {
    printf("pcm_stop(): SDL_PauseAudioDevice %s\n", SDL_GetError());
  }
  _pcm_rewind(p);
  _pcm_unlock(p);

  return NOERROR;
}

DJ_RESULT pcm_set_looping(DJ_HANDLE h, boolean looping) {
  struct pcm_player* p = (struct pcm_player*)h;

  if (!_pcm_is_handle_valid(h)) {
    return INVALID_PARAM;
  }

  _pcm_lock(p);
  p->looping = looping;
  _pcm_unlock(p);

  return NOERROR;
}

DJ_RESULT pcm_set_volume_left(DJ_HANDLE h, unsigned int level) {
  struct pcm_player* p = (struct pcm_player*)h;

  if (!_pcm_is_handle_valid(h)) {
    return INVALID_PARAM;
  }
  _pcm_lock(p);
  p->lvolume = level;
  _pcm_unlock(p);

  return NOERROR;
}

unsigned int pcm_get_volume_left(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;

  if (!_pcm_is_handle_valid(h)) {
    return 0;
  }

  return p->lvolume;
}

DJ_RESULT pcm_set_volume_right(DJ_HANDLE h, unsigned int level) {
  struct pcm_player* p = (struct pcm_player*)h;

  if (!_pcm_is_handle_valid(h)) {
    return INVALID_PARAM;
  }

  _pcm_lock(p);
  p->rvolume = level;
  _pcm_unlock(p);

  return NOERROR;
}

unsigned int pcm_get_volume_right(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;

  if (!_pcm_is_handle_valid(h)) {
    return 0;
  }

  return p->rvolume;
}

DJ_RESULT pcm_set_volume(DJ_HANDLE h, unsigned int level) {
  unsigned int err = NOERROR;

  err = pcm_set_volume_left(h, level);
  if (err == NOERROR) {
    err = pcm_set_volume_right(h, level);
  }

  return err;
}

boolean pcm_is_looping(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;
  if (!_pcm_is_handle_valid(h)) {
    return false;
  }

  return p->looping;
}

boolean pcm_is_playing(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;
  if (!_pcm_is_handle_valid(h)) {
    return false;
  }

  return (p->state == STATE_PLAYING);
}

boolean pcm_is_paused(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;
  if (!_pcm_is_handle_valid(h)) {
    return false;
  }

  return (p->state == STATE_PAUSED);
}

boolean pcm_is_stopped(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;
  if (!_pcm_is_handle_valid(h)) {
    return false;
  }

  return (p->state == STATE_STOPPED);
}

static void _pcm_audio_callback(void* userdata, Uint8* stream, int len) {
  struct pcm_player* p = (struct pcm_player*)userdata;
  struct pcm_sample* s = p->sample;

  unsigned int bytesread = s->ptr - s->raw_bytes;
  unsigned int bytesleft = s->raw_len - bytesread;

  if (bytesleft == 0) {
    _pcm_rewind(p);

    if(p->looping) {
      bytesread = s->ptr - s->raw_bytes;
      bytesleft = s->raw_len - bytesread;
    } else {
      memset(stream, p->sample->silence, len);
      p->state = STATE_STOPPED;
      SDL_PauseAudioDevice(p->device);
      if (p->cb) {
        p->cb(p);
      }

      return;
    }
  }

  if (bytesleft >= (unsigned int)len) {
    bytesread = len;
  } else {
    bytesread = bytesleft;
    memset(stream + bytesread, p->sample->silence, len - bytesread);
  }

  memcpy(stream, s->ptr, bytesread);
  _pcm_adjust_volume(stream, bytesread, p);

  s->ptr += bytesread;

}

static DJ_RESULT _pcm_lock(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;

  WaitForSingleObject(p->mutex, INFINITE);

  return NOERROR;
}

static DJ_RESULT _pcm_unlock(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;

  if(ReleaseMutex(p->mutex)) {
    return NOERROR;
  }
  return ERROR;
}

static DJ_RESULT _pcm_rewind(DJ_HANDLE h) {
  struct pcm_player* p = (struct pcm_player*)h;

  if (p->sample != NULL)
    p->sample->ptr = p->sample->raw_bytes;

  return NOERROR;
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
      p->state = STATE_STOPPED;
      p->looping = 0;
      p->lvolume = 65536;
      p->rvolume = 65536;
      p->sample = NULL;
      p->next = NULL;
      p->cb = NULL;

      p->mutex = CreateMutex(NULL, FALSE, NULL);
      if (p->mutex == NULL) {
        fprintf(stderr, "CreateMutex failed %lu\n", GetLastError());
        goto error1;
      }
    }
  }
  
  if (p != NULL) {
    p->cb = callback;
  }

  return p;

error1:
  free(p);
  return NULL;
}

static void _pcm_player_free(struct pcm_player* p) {
  if (p == NULL) {
    return;
  }

  CloseHandle(p->mutex);
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

static struct pcm_sample* _pcm_sample_create(unsigned char* buf, unsigned int len) {
  struct pcm_sample* s = (struct pcm_sample*)malloc(sizeof(struct pcm_sample));
  if (s == NULL)
    goto error1;

  struct dmx_header* dmx = (struct dmx_header*)buf;

  unsigned short formatNumber = dmx->format; // always 3

  s->sample_rate = dmx->sample_rate;
  s->sample_count = dmx->length - 32; // length includes 16 bytes buffer on each end of samples
  s->sample_size = 8;
  s->channels = 1;
  s->raw_bytes = s->ptr = (unsigned char*)malloc(s->sample_count);
  if (s->raw_bytes == NULL)
    goto error2;

  memcpy(s->raw_bytes, &dmx->samples, s->sample_count);
  s->raw_len = s->sample_count;

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
  SDL_AudioSpec audioSpec, have;
  SDL_AudioDeviceID id;

  SDL_memset(&audioSpec, 0, sizeof(audioSpec)); /* or SDL_zero(want) */

  audioSpec.format = AUDIO_U8;
  audioSpec.channels = p->sample->channels;
  audioSpec.freq = p->sample->sample_rate;
  audioSpec.samples = MAX_BUFFER_SIZE;
  audioSpec.userdata = p;
  audioSpec.callback = _pcm_audio_callback;

  id = SDL_OpenAudioDevice(NULL, 0, &audioSpec, &have, 0);

  p->sample->silence = have.silence;

  p->device = id;

  return p;
}

static void _pcm_stream_close(struct pcm_player* p) {
  SDL_CloseAudioDevice(p->device);
}




static unsigned int _pcm_adjust_volume(unsigned char* out, unsigned int len, struct pcm_player* p) {
  unsigned int i, length;

  unsigned int lvol = p->lvolume;
  unsigned int rvol = p->rvolume;

  if (p->sample->channels == 1)
    rvol = lvol;

  switch (p->sample->sample_size) {
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

DJ_RESULT pcm_volume_left(DJ_HANDLE h, unsigned int dir) {
  unsigned int vol = 0;
  const unsigned int val = 3277;
  struct pcm_player* p = (struct pcm_player*)h;

  if(!_pcm_is_handle_valid(h)) {
    return INVALID_PARAM;
  }

  vol = pcm_get_volume_left(h);

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

  pcm_set_volume_left(h, vol);
  return NOERROR;
}

DJ_RESULT pcm_volume_right(DJ_HANDLE h, unsigned int dir) {
  unsigned int vol = 0;
  const unsigned int val = 3277;
  struct pcm_player* p = (struct pcm_player*)h;

  if (!_pcm_is_handle_valid(h)) {
    return INVALID_PARAM;
  }

  vol = pcm_get_volume_right(h);

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

  pcm_set_volume_right(h, vol);
  return NOERROR;
}

DJ_RESULT pcm_volume(DJ_HANDLE h, unsigned int dir) {
  unsigned int err;

  err = pcm_volume_left(h, dir);
  if (err == NOERROR)
    err = pcm_volume_right(h, dir);

  return err;
}

void standalone_callback(void* data) {
  printf("\r       \rStopped");
}

int main(int argc, char* argv[]) {
  unsigned char* filename;
  unsigned char* wavbuf = NULL;
  unsigned int wavbuflen = 0;
  unsigned char c;

  DJ_HANDLE s;

  unsigned long n, i;
  unsigned int err;

  if (argc <= 1) {
    printf("Usage: %s <filename>\n", argv[0]);
    return 0;
  }

  SDL_Init(SDL_INIT_AUDIO);

  n = SDL_GetNumAudioDevices(0);
  if (n == 0) {
    fprintf(stderr, "No WAVE devices found!\n");
    return 0;
  }

  for (i = 0; i < n; i++) {
      printf("Device %lu: %s\r\n", i, SDL_GetAudioDeviceName(i, 0));
  }

  pcm_init();

  printf("loading\n");

  filename = argv[1];

  wavbuf = load_file(filename, &wavbuflen);
  if (wavbuf == NULL) {
    fprintf(stderr, "Failed to load file %s\n", filename);
    return 0;
  }

  s = pcm_sound_open(wavbuf, wavbuflen, standalone_callback);
  if (s == NULL) {
    printf("Failed to open sample.\n");
    return 0;
  }


  printf("Loaded %s\n", filename);
  free(wavbuf);

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
        if (err != NOERROR) {
          //fprintf(stderr, "Error playing file %s, error %d\n", filename, err);
          goto error;
        }
        printf("\r       \rPlaying");
      } else if (pcm_is_playing(s)) {
        err = pcm_pause(s);
        if (err != NOERROR) {
          //fprintf(stderr, "Error pausing file %s, error %d\n", filename, err);
          goto error;
        }
        printf("\r       \rPaused");
      } else if (pcm_is_paused(s)) {
        err = pcm_resume(s);
        if (err != NOERROR) {
          //fprintf(stderr, "Error pausing file %s, error %d\n", filename, err);
          goto error;
        }
        printf("\r       \rPlaying");
      }
      break;
    case 's':
    case 'S':
        err = pcm_stop(s);
        if (err != NOERROR) {
          //fprintf(stderr, "Error stopping file %s, error %d\n", filename, err);
          goto error;
        }
        printf("\r       \rStopped");

      break;
    case '-':
    case '_':
      err = pcm_volume(s, VOL_DOWN);
      break;
    case '=':
    case '+':
      err = pcm_volume(s, VOL_UP);
      break;
    case 'q':
    default:
      break;
    }
  }

error:

  pcm_sound_close(s);

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
