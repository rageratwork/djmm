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

struct pcm_player
{
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

static unsigned int pcm_get_streambuf(struct pcm_sample* s, unsigned char* out, unsigned int* outlen);
static unsigned int pcm_adjust_volume(unsigned char* out, unsigned int len, unsigned int sample_size, unsigned int channels, unsigned int lvol, unsigned int rvol);

static void pcm_player_shutdown(struct pcm_player* p);
static void pcm_player_free(struct pcm_player* p);
static void pcm_close_stream(struct pcm_player* p);
static MMRESULT pcm_rewind(HANDLE h);

static void CALLBACK pcm_callback_proc(HWAVEOUT hmo, UINT wMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
	struct pcm_player* p = (struct pcm_player*)dwInstance;
	if(p == NULL)
		return;

    switch (wMsg)
    {
        case WOM_DONE:
            SetEvent(p->event);
            break;
        case WOM_OPEN:
        	break;
        case WOM_CLOSE:
            break;
    }

}

static DWORD WINAPI pcm_player_proc(LPVOID lpParameter)
{
	struct pcm_player* p = (struct pcm_player*)lpParameter;
	unsigned int err = MMSYSERR_NOERROR;

	unsigned int idx = 0;

	WaitForSingleObject(p->mutex, INFINITE);
	while((p->state != STATE_SHUTDOWN) && (p->state != STATE_ERROR))
	{
		switch(p->state)
		{
		case STATE_PLAYING:
			ReleaseMutex(p->mutex);
			WaitForSingleObject(p->event, INFINITE);

			WaitForSingleObject(p->mutex, INFINITE);

			if(p->state == STATE_SHUTDOWN)
				break;

			if((p->state == STATE_PLAYING) || (p->state == STATE_PAUSED))
			{
				pcm_get_streambuf(p->sample, (unsigned char*)p->header[idx].lpData, (unsigned int*)&p->header[idx].dwBufferLength);
				pcm_adjust_volume((unsigned char*)p->header[idx].lpData, p->header[idx].dwBufferLength, p->sample->sample_size, p->sample->channels, p->lvolume, p->rvolume);
				p->header[idx].dwBytesRecorded = p->header[idx].dwBufferLength;
				if(p->header[idx].dwBufferLength > 0)
				{
					p->header[idx].dwFlags &= ~WHDR_DONE;
					err = waveOutWrite(p->stream, &p->header[idx], sizeof(WAVEHDR));
					idx = (idx + 1) % 2;
				}
				else
				{
					if(p->looping)
					{
						pcm_rewind(p);
						pcm_get_streambuf(p->sample, (unsigned char*)p->header[idx].lpData, (unsigned int*)&p->header[idx].dwBufferLength);
						pcm_adjust_volume((unsigned char*)p->header[idx].lpData, p->header[idx].dwBufferLength, p->sample->sample_size, p->sample->channels, p->lvolume, p->rvolume);
						p->header[idx].dwBytesRecorded = p->header[idx].dwBufferLength;
						p->header[idx].dwFlags &= ~WHDR_DONE;
						waveOutWrite(p->stream, &p->header[idx], sizeof(WAVEHDR));
						idx = (idx + 1) % 2;
					}
					else
					{
						// one more buffer left playing, wait for it to finish
						ReleaseMutex(p->mutex);
						WaitForSingleObject(p->event, INFINITE);

						WaitForSingleObject(p->mutex, INFINITE);

						if(p->state == STATE_SHUTDOWN)
							break;

						if(p->state != STATE_STOPPING)
							p->state = STATE_STOPPED;
						waveOutReset(p->stream);

						// don't bother releasing the mutex and looping again,
						// just enter the stopped state.
						goto stopped;
					}
				}
			}

			ReleaseMutex(p->mutex);
			break;
		case STATE_STOPPING:
		case STATE_STOPPED:
stopped:
			idx = 0;
			if(p->cb)
				p->cb(p->state);
			pcm_rewind(p);

			if(p->state == STATE_STOPPING)
			{
				p->state = STATE_STOPPED;
				SetEvent(p->ready);
			}

			ReleaseMutex(p->mutex);
			WaitForSingleObject(p->event, INFINITE);
			break;
		}

		WaitForSingleObject(p->mutex, INFINITE);
	}
	ReleaseMutex(p->mutex);

    return 0;
}

static HANDLE players_mutex = NULL;
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
MMRESULT pcm_init()
{
	players = NULL;
	pool = NULL;

	players_mutex = CreateMutex(NULL, FALSE, NULL);
	if(players_mutex == NULL)
		return MMSYSERR_ERROR;

	return MMSYSERR_NOERROR;
}

/*!
 * Shut down the MUS subsystem.
 *
 * This function loops through the open player handles closing each in turn.
 *
 * This function acquires the global player mutex to read the first player from
 * the list. The mutex is released once a valid handle is found.
 * mus_score_close() is called on the handle which will remove it form the
 * global list.
 */
void pcm_shutdown()
{
	struct pcm_player* tmp = players;
	WaitForSingleObject(players_mutex, INFINITE);
	tmp = players;
	while(tmp != NULL)
	{
		ReleaseMutex(players_mutex);

		// This function acquires players_mutex and tmp->mutex so make sure
		// they are not held here.
		pcm_sample_close(tmp);

		WaitForSingleObject(players_mutex, INFINITE);
		tmp = players;
	}

	tmp = pool;
	while(tmp != NULL)
	{
		pool = pool->next;
		pcm_player_free(tmp);
		tmp = pool;
	}
	ReleaseMutex(players_mutex);

	// At this point all handles are closed and the global list empty.
	CloseHandle(players_mutex);
}

/*!
 * Internal function to initialize a player.
 *
 * This function initializes a player structure and spawns a thread to process
 * buffered MIDIEVENT structures.
 *
 * This function does not acquire any locks. The function does wait on an event
 * signaled from the thread when it is started and waiting in the STATE_STOPPED
 * state.
 *
 * This function is called by mus_score_open() without holding any locks.
 *
 * @return The handle to the new player or NULL if creation failed.
 */
static struct pcm_player* pcm_player_init()
{
	struct pcm_player* p = NULL;
	WaitForSingleObject(players_mutex, INFINITE);
	if(pool != NULL)
	{
		p = pool;
		pool = pool->next;
		ReleaseMutex(players_mutex);

		p->next = NULL;
	}
	else
	{
		ReleaseMutex(players_mutex);

		p = (struct pcm_player*)malloc(sizeof(struct pcm_player));
		if(p != NULL)
		{
			// Initialize the double buffers.
			ZeroMemory(&p->header[0], sizeof(WAVEHDR));
			p->header[0].lpData = (char*)malloc(MAX_BUFFER_SIZE);
			if(p->header[0].lpData == NULL)
				goto error1;
			p->header[0].dwBufferLength = p->header[0].dwBytesRecorded = MAX_BUFFER_SIZE;

			ZeroMemory(&p->header[1], sizeof(WAVEHDR));
			p->header[1].lpData = (char*)malloc(MAX_BUFFER_SIZE);
			if(p->header[1].lpData == NULL)
				goto error2;

			p->header[1].dwBufferLength = p->header[1].dwBytesRecorded = MAX_BUFFER_SIZE;
			p->state = STATE_STOPPING;
			p->looping = 0;
			p->lvolume = 65536;
			p->rvolume = 65536;
			p->stream = 0;
			p->thread = 0;
			p->sample = NULL;
			p->cb = NULL;

			p->mutex = CreateMutex(NULL, FALSE, NULL);
			if(p->mutex == NULL)
			{
				fprintf(stderr, "CreateMutex failed %lu\n", GetLastError());
				goto error3;
			}

			p->event = CreateEvent(0, FALSE, FALSE, 0);
			if(p->event == NULL)
			{
				fprintf(stderr, "CreateEvent failed %lu\n", GetLastError());
				goto error3;
			}

			// Create the event that signals when the player is stopped and ready.
			// This event is used in this function when the thread is first
			// initialized. It is also used in mus_stop() to signal when the thread
			// has settled into its STATE_STOPPED state.
			p->ready = CreateEvent(0, FALSE, FALSE, 0);
			if(p->ready == NULL)
			{
				fprintf(stderr, "CreateEvent failed %lu\n", GetLastError());
				goto error3;
			}

			// Finally, spawn the worker thread.
			p->thread = CreateThread(NULL, 0, pcm_player_proc, p, 0, NULL);
			if(p->thread == NULL)
			{
				fprintf(stderr, "CreateThread failed %lu\n", GetLastError());
				goto error3;
			}

			// Make sure the thread is spun up and ready.
			WaitForSingleObject(p->ready, INFINITE);
		}
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

/*!
 * This function shuts down a player.
 *
 * This function shuts down a player. It acquires the p->mutex to wait for
 * exclusive access to the player. Once it has exclusive access, it sets the
 * state to STATE_SHUTDOWN and signals p->event to cause the main thread loop
 * to finish. Once the thread is signaled, it waits on the thread handle until
 * the thread exits. Finally, it cleans up its resources.
 *
 * This function is called by mus_score_open() with no locks held. It is called
 * in the event of an error, in which case the player has not yet been inserted
 * into the global list.
 *
 * This function is called by mus_score_close() with no locks held.
 *
 * @param p
 */
static void pcm_player_free(struct pcm_player* p)
{
	if(p == NULL)
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

static void pcm_player_shutdown(struct pcm_player* p)
{
	WaitForSingleObject(players_mutex, INFINITE);
	p->next = pool;
	pool = p;
	ReleaseMutex(players_mutex);
}

HANDLE pcm_sample_open(unsigned int sample_rate, unsigned int sample_size, unsigned int channels, unsigned char* buf, unsigned int len)
{
	WAVEFORMATEX wfx;
	unsigned int err = MMSYSERR_NOERROR;
	struct pcm_player* p = NULL;
	struct pcm_sample* s = NULL;

	p = pcm_player_init();
	if(p == NULL)
		goto error1;

	s = (struct pcm_sample*)malloc(sizeof(struct pcm_sample));
	if(s == NULL)
		goto error2;

	s->sample_rate = sample_rate;
	s->sample_size = sample_size;
	s->channels = channels;
	s->raw_bytes = s->ptr = (unsigned char*)malloc(len);
	if(s->raw_bytes == NULL)
		goto error3;

	memcpy(s->raw_bytes, buf, len);
	s->raw_len = len;

	p->sample = s;

	wfx.nSamplesPerSec = p->sample->sample_rate; /* sample rate */
	wfx.wBitsPerSample = p->sample->sample_size; /* sample size */
	wfx.nChannels = p->sample->channels;
	wfx.cbSize = 0; /* size of _extra_ info */
	wfx.wFormatTag = WAVE_FORMAT_PCM;
	wfx.nBlockAlign = (wfx.wBitsPerSample >> 3) * wfx.nChannels;
	wfx.nAvgBytesPerSec = wfx.nBlockAlign * wfx.nSamplesPerSec;

	err = waveOutOpen(&p->stream, WAVE_MAPPER, &wfx, (DWORD_PTR)pcm_callback_proc, (DWORD_PTR)p, CALLBACK_FUNCTION);
	if(err != MMSYSERR_NOERROR)
		goto error4;

	err = waveOutPrepareHeader(p->stream, &p->header[0], sizeof(WAVEHDR));
	if(err != MMSYSERR_NOERROR)
		goto error4;

	err = waveOutPrepareHeader(p->stream, &p->header[1], sizeof(WAVEHDR));
	if(err != MMSYSERR_NOERROR)
		goto error4;

	// Score is loaded and ready. Add the player to our global list
	// and return the "HANDLE" to the user.

	WaitForSingleObject(players_mutex, INFINITE);
	p->next = players;
	players = p;
	ReleaseMutex(players_mutex);

	return p;

error4:
	pcm_sample_close(s);

error3:
	free(s);

error2:
	pcm_player_shutdown(p);
	p = NULL;

error1:
	return NULL;
}

BOOL pcm_is_handle_valid(HANDLE h)
{
	struct pcm_player* p = players;
	if(h == NULL)
		return FALSE;

	while(p != NULL)
	{
		if(p == h)
			return TRUE;
		else
			p = p->next;
	}

	return FALSE;
}

/*!
 * This function stops any buffers that are currently playing and closes the
 * stream. This function should be called while holding p->mutex.
 *
 * This function is called by mus_player_proc with p->mutex held.
 * This function is called by mus_score_close with p->mutex held.
 * This function is called by mus_stop with p->mutex held.
 *
 * @param p
 */
static void pcm_close_stream(struct pcm_player* p)
{
	unsigned int err;
	waveOutReset(p->stream);
	err = waveOutUnprepareHeader(p->stream, &p->header[0], sizeof(WAVEHDR));
	if(err != MMSYSERR_NOERROR)
		printf("midiOutUnprepareHeader %d\n", err);
	err = waveOutUnprepareHeader(p->stream, &p->header[1], sizeof(WAVEHDR));
	if(err != MMSYSERR_NOERROR)
		printf("midiOutUnprepareHeader %d\n", err);
	waveOutClose(p->stream);
	p->stream = 0;
}

void pcm_sample_close(HANDLE h)
{
	struct pcm_player* p = players;

	WaitForSingleObject(players_mutex, INFINITE);
	if(pcm_is_handle_valid(h) == FALSE)
	{
		ReleaseMutex(players_mutex);
		DJ_TRACE("pcm_sample_close(): invalid handle %p\n", h);
		return;
	}

	p = players;
	if(players == h)
	{
		players = p->next;
		p->next = NULL;
	}
	else
	{
		while(p != NULL)
		{
			if(p->next == h)
			{
				p->next = p->next->next;
				p = h;
				p->next = NULL;
				break;
			}
			else
				p = p->next;
		}
	}

	ReleaseMutex(players_mutex);

	// The player has been removed from the global list. Any further calls
	// using this handle will return MMSYSERR_INVALPARAM.

	// Start shutting down the thread. If it's still playing, stop it.
	WaitForSingleObject(p->mutex, INFINITE);
	if(p->state != STATE_STOPPED)
	{
		p->state = STATE_STOPPING;
		ResetEvent(p->ready);
		pcm_close_stream(p);
		SetEvent(p->event);
		ReleaseMutex(p->mutex);
		WaitForSingleObject(p->ready, INFINITE);
	}
	else
		ReleaseMutex(p->mutex);

	// Player thread should be in the STATE_STOPPED state. No existing
	// handles can restart it. Let's close things out.

	if(p->sample)
		free(p->sample->raw_bytes);

	free(p->sample);
	p->sample = NULL;

	pcm_player_shutdown(p);

	return;
}

MMRESULT pcm_play(HANDLE h)
{
	struct pcm_player* p = (struct pcm_player*)h;
	unsigned int err = MMSYSERR_NOERROR;

	WaitForSingleObject(players_mutex, INFINITE);
	if(pcm_is_handle_valid(h) == FALSE)
	{
		DJ_TRACE("pcm_play(): invalid handle %p\n", h);
		ReleaseMutex(players_mutex);
		return MMSYSERR_INVALPARAM;
	}

	WaitForSingleObject(p->mutex, INFINITE);
	ReleaseMutex(players_mutex);
	if(p->state == STATE_STOPPED)
	{
		pcm_get_streambuf(p->sample, (unsigned char*)p->header[0].lpData, (unsigned int*)&p->header[0].dwBufferLength);
		pcm_adjust_volume((unsigned char*)p->header[0].lpData, p->header[0].dwBufferLength, p->sample->sample_size, p->sample->channels, p->lvolume, p->rvolume);
		p->header[0].dwBytesRecorded = p->header[0].dwBufferLength;

		err = waveOutWrite(p->stream, &p->header[0], sizeof(WAVEHDR));
		if(err == MMSYSERR_NOERROR)
		{
			pcm_get_streambuf(p->sample, (unsigned char*)p->header[1].lpData, (unsigned int*)&p->header[1].dwBufferLength);
			pcm_adjust_volume((unsigned char*)p->header[1].lpData, p->header[1].dwBufferLength, p->sample->sample_size, p->sample->channels, p->lvolume, p->rvolume);
			p->header[1].dwBytesRecorded = p->header[1].dwBufferLength;

			err = waveOutWrite(p->stream, &p->header[1], sizeof(WAVEHDR));
			if(err == MMSYSERR_NOERROR)
			{
				p->state = STATE_PLAYING;
				SetEvent(p->event);
			}
		}
	}
	ReleaseMutex(p->mutex);

	return err;
}

MMRESULT pcm_stop(HANDLE h)
{
	struct pcm_player* p = (struct pcm_player*)h;
	unsigned int err = MMSYSERR_NOERROR;

	WaitForSingleObject(players_mutex, INFINITE);
	if(pcm_is_handle_valid(h) == FALSE)
	{
		ReleaseMutex(players_mutex);
		return MMSYSERR_INVALPARAM;
	}

	WaitForSingleObject(p->mutex, INFINITE);
	ReleaseMutex(players_mutex);
	if(p->state != STATE_STOPPED)
	{
		ResetEvent(p->ready);
		p->state = STATE_STOPPING;
		waveOutReset(p->stream);
		SetEvent(p->event);
		ReleaseMutex(p->mutex);
		WaitForSingleObject(p->ready, INFINITE);
	}
	else
	{
		ReleaseMutex(p->mutex);
	}

	return MMSYSERR_NOERROR;
}

MMRESULT pcm_pause(HANDLE h)
{
	struct pcm_player* p = (struct pcm_player*)h;
	unsigned int err = MMSYSERR_NOERROR;

	WaitForSingleObject(players_mutex, INFINITE);
	if(pcm_is_handle_valid(h) == FALSE)
	{
		ReleaseMutex(players_mutex);
		return MMSYSERR_INVALPARAM;
	}

	WaitForSingleObject(p->mutex, INFINITE);
	ReleaseMutex(players_mutex);
	if(p->state == STATE_PLAYING)
	{
		err = waveOutPause(p->stream);
		if(err == MMSYSERR_NOERROR)
			p->state = STATE_PAUSED;
		else
		{
			printf("err pausing: %d\n", err);
			p->state = STATE_ERROR;
		}
	}
	ReleaseMutex(p->mutex);

	return err;
}

MMRESULT pcm_resume(HANDLE h)
{
	struct pcm_player* p = (struct pcm_player*)h;
	unsigned int err = MMSYSERR_NOERROR;

	WaitForSingleObject(players_mutex, INFINITE);
	if(pcm_is_handle_valid(h) == FALSE)
	{
		ReleaseMutex(players_mutex);
		return MMSYSERR_INVALPARAM;
	}

	WaitForSingleObject(p->mutex, INFINITE);
	ReleaseMutex(players_mutex);
	if(p->state == STATE_PAUSED)
	{
		err = waveOutRestart(p->stream);
		if(err == MMSYSERR_NOERROR)
			p->state = STATE_PLAYING;
		else
		{
			printf("err restart: %d\n", err);
			p->state = STATE_ERROR;
		}
	}
	ReleaseMutex(p->mutex);

	return err;
}

MMRESULT pcm_set_volume_left(HANDLE h, unsigned int level)
{
	unsigned int old = 0, vol = 0;
	struct pcm_player* p = (struct pcm_player*)h;
	unsigned int err = MMSYSERR_NOERROR;

	WaitForSingleObject(players_mutex, INFINITE);
	if(pcm_is_handle_valid(h))
	{
		WaitForSingleObject(p->mutex, INFINITE);
		ReleaseMutex(players_mutex);

		p->lvolume = level;
		ReleaseMutex(p->mutex);
		return err;
	}
	ReleaseMutex(players_mutex);

	err = waveOutGetVolume((HWAVEOUT)WAVE_MAPPER, (LPDWORD)&old);
	if(err == MMSYSERR_NOERROR)
	{
		vol = MAKELONG(LOWORD(level), HIWORD(old));
		err = waveOutSetVolume((HWAVEOUT)WAVE_MAPPER, vol);
	}

	return err;
}

MMRESULT pcm_set_volume_right(HANDLE h, unsigned int level)
{
	unsigned int old = 0, vol = 0;
	struct pcm_player* p = (struct pcm_player*)h;
	unsigned int err = MMSYSERR_NOERROR;

	WaitForSingleObject(players_mutex, INFINITE);
	if(pcm_is_handle_valid(h))
	{
		WaitForSingleObject(p->mutex, INFINITE);
		ReleaseMutex(players_mutex);

		p->rvolume = level;
		ReleaseMutex(p->mutex);
		return err;
	}
	ReleaseMutex(players_mutex);

	err = waveOutGetVolume((HWAVEOUT)WAVE_MAPPER, (LPDWORD)&old);
	if(err == MMSYSERR_NOERROR)
	{
		vol = MAKELONG(LOWORD(old), LOWORD(level));
		err = waveOutSetVolume((HWAVEOUT)WAVE_MAPPER, vol);
	}


	return err;
}

MMRESULT pcm_set_volume(HANDLE h, unsigned int level)
{
	unsigned int err = MMSYSERR_NOERROR;

	err = pcm_set_volume_left(h, level);
	if(err == MMSYSERR_NOERROR)
		err = pcm_set_volume_right(h, level);

	return err;
}

MMRESULT pcm_volume_left(HANDLE h, unsigned int dir)
{
	unsigned int old = 0, vol = 0;
	const unsigned int val = 3277;
	struct pcm_player* p = (struct pcm_player*)h;
	unsigned int err = MMSYSERR_NOERROR;
	HWAVEOUT stream = (HWAVEOUT)WAVE_MAPPER;
	BOOL valid = FALSE;

	WaitForSingleObject(players_mutex, INFINITE);
	valid = pcm_is_handle_valid(h);
	if(valid == TRUE)
	{
		stream = p->stream;
		WaitForSingleObject(p->mutex, INFINITE);
	}
	ReleaseMutex(players_mutex);

	err = waveOutGetVolume(stream, (LPDWORD)&old);
	if(err == MMSYSERR_NOERROR)
	{
		vol = LOWORD(old);

		if(dir == VOL_UP)
		{
			if(0xffff - vol <= val)
				vol = 0xffff;
			else
				vol += val;
		}
		else
		{
			if(vol <= val)
				vol = 0;
			else
				vol -= val;
		}

		vol = MAKELONG(vol, HIWORD(old));
		err = waveOutSetVolume(stream, vol);
	}

	if(valid == TRUE)
		ReleaseMutex(p->mutex);

	return err;
}

MMRESULT pcm_volume_right(HANDLE h, unsigned int dir)
{
	unsigned int old = 0, vol = 0;
	const unsigned int val = 3277;
	struct pcm_player* p = (struct pcm_player*)h;
	unsigned int err = MMSYSERR_NOERROR;
	HWAVEOUT stream = (HWAVEOUT)WAVE_MAPPER;
	BOOL valid = FALSE;

	WaitForSingleObject(players_mutex, INFINITE);
	valid = pcm_is_handle_valid(h);
	if(valid == TRUE)
	{
		stream = p->stream;
		WaitForSingleObject(p->mutex, INFINITE);
	}
	ReleaseMutex(players_mutex);

	err = waveOutGetVolume(stream, (LPDWORD)&old);
	if(err == MMSYSERR_NOERROR)
	{
		vol = HIWORD(old);

		if(dir == VOL_UP)
		{
			if(0xffff - vol <= val)
				vol = 0xffff;
			else
				vol += val;
		}
		else
		{
			if(vol <= val)
				vol = 0;
			else
				vol -= val;
		}

		vol = MAKELONG(LOWORD(old), vol);
		err = waveOutSetVolume(stream, vol);
	}

	if(valid == TRUE)
		ReleaseMutex(p->mutex);

	return err;
}

MMRESULT pcm_volume(HANDLE h, unsigned int dir)
{
	unsigned int err;

	err = pcm_volume_left(h, dir);
	if(err == MMSYSERR_NOERROR)
		err = pcm_volume_right(h, dir);

	return err;
}

MMRESULT pcm_set_looping(HANDLE h, BOOL looping)
{
	struct pcm_player* p = (struct pcm_player*)h;
	unsigned int err = MMSYSERR_NOERROR;

	WaitForSingleObject(players_mutex, INFINITE);
	if(pcm_is_handle_valid(h) == FALSE)
	{
		ReleaseMutex(players_mutex);
		return MMSYSERR_INVALPARAM;
	}

	WaitForSingleObject(p->mutex, INFINITE);
	ReleaseMutex(players_mutex);

	p->looping = looping;

	ReleaseMutex(p->mutex);

	return MMSYSERR_NOERROR;
}

BOOL pcm_get_looping(HANDLE h)
{
	struct pcm_player* p = (struct pcm_player*)h;
	unsigned int err = MMSYSERR_NOERROR;
	BOOL looping;

	WaitForSingleObject(players_mutex, INFINITE);
	if(pcm_is_handle_valid(h) == FALSE)
	{
		ReleaseMutex(players_mutex);
		return FALSE;
	}

	WaitForSingleObject(p->mutex, INFINITE);
	ReleaseMutex(players_mutex);

	looping = p->looping;

	ReleaseMutex(p->mutex);

	return looping;
}

BOOL pcm_is_playing(HANDLE h)
{
	struct pcm_player* p = (struct pcm_player*)h;
	BOOL ret = FALSE;

	WaitForSingleObject(players_mutex, INFINITE);
	if(pcm_is_handle_valid(h) == FALSE)
		ret = FALSE;
	else
		ret = (p->state == STATE_PLAYING);
	ReleaseMutex(players_mutex);

	return ret;
}

BOOL pcm_is_paused(HANDLE h)
{
	struct pcm_player* p = (struct pcm_player*)h;
	BOOL ret = FALSE;

	WaitForSingleObject(players_mutex, INFINITE);
	if(pcm_is_handle_valid(h) == FALSE)
		ret = FALSE;
	else
		ret = (p->state == STATE_PAUSED);
	ReleaseMutex(players_mutex);

	return ret;
}

BOOL pcm_is_stopped(HANDLE h)
{
	struct pcm_player* p = (struct pcm_player*)h;
	BOOL ret = FALSE;

	WaitForSingleObject(players_mutex, INFINITE);
	if(pcm_is_handle_valid(h) == FALSE)
		ret = FALSE;
	else
		ret = (p->state == STATE_STOPPED);
	ReleaseMutex(players_mutex);

	return ret;
}

static MMRESULT pcm_rewind(HANDLE h)
{
	struct pcm_player* p = (struct pcm_player*)h;

	if(pcm_is_handle_valid(h) == FALSE)
		return MMSYSERR_INVALPARAM;

	if(p->sample != NULL)
		p->sample->ptr = p->sample->raw_bytes;

	return MMSYSERR_NOERROR;
}

static unsigned int pcm_get_streambuf(struct pcm_sample* s, unsigned char* out, unsigned int* outlen)
{
	unsigned int blocklen = (s->sample_size / 8) * s->channels;
	unsigned int streambufsize = MAX_BUFFER_SIZE - (MAX_BUFFER_SIZE % blocklen);

	unsigned int bytesread = s->ptr - s->raw_bytes;
	unsigned int bytesleft = s->raw_len - bytesread;

	*outlen = 0;
	if(bytesleft == 0)
		return 0;

	if(bytesleft >= streambufsize)
		bytesread = streambufsize;
	else
		bytesread = bytesleft;

	memcpy(out, s->ptr, bytesread);

	s->sample_size;
	s->ptr += bytesread;
	*outlen = bytesread;
	return 0;
}

static unsigned int pcm_adjust_volume(unsigned char* out, unsigned int len, unsigned int sample_size, unsigned int channels, unsigned int lvol, unsigned int rvol)
{
	unsigned int i, length;
	if(channels == 1)
		rvol = lvol;

	switch(sample_size)
	{
	case 8:
		length = len;
		lvol >>= 8;
		rvol >>=8;
		for(i = 0; i < length; i++)
		{
			out[i] = (((int)out[i] - 128) * lvol / 256) + 128;
			i++;
			if(i < length)
				out[i] = (((int)out[i] - 128) * rvol / 256) + 128;
		}
		break;
	case 16:
		{
			short* s = (short*)out;
			length = len / 2;
			for(i = 0; i < length; i++)
			{
				s[i] = s[i] * lvol / 65536;
				i++;
				if(i < length)
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

void pcm_callback(unsigned int val)
{
	printf("\r       \rStopped");
}

int main(int argc, char* argv[])
{
	unsigned char* filename;
	unsigned char* wavbuf = NULL;
	unsigned int wavbuflen = 0;
	unsigned char c;

	WAVEOUTCAPS caps;

	unsigned long n, i;
	unsigned int err;

	HANDLE s;

	if(argc > 1)
		filename = (unsigned char*)argv[1];
	else
	{
		printf("Usage: %s <filename>\n", argv[0]);
		return 0;
	}

	n = waveOutGetNumDevs();
	if(n == 0)
	{
		fprintf(stderr, "No WAVE devices found!\n");
		return 0;
	}

	for (i = 0; i < n; i++)
	{
	    if (!waveOutGetDevCaps(i, &caps, sizeof(WAVEOUTCAPS)))
	    {
	        printf("Device %lu: %s\r\n", i, caps.szPname);
	        if(caps.dwSupport & WAVECAPS_PITCH)
	        {
	        	printf(" - supports pitch control.\n");
	        }
	        if(caps.dwSupport & WAVECAPS_VOLUME)
	        {
	        	printf(" - supports volume control.\n");
	        }
	        if(caps.dwSupport & WAVECAPS_LRVOLUME)
	        {
	        	printf(" - supports separate left and right volume control.\n");
	        }
	        if(caps.dwSupport & WAVECAPS_PLAYBACKRATE)
	        {
	        	printf(" - supports playback rate control.\n");
	        }
	        if(caps.dwSupport & WAVECAPS_SYNC)
	        {
	        	printf(" - the driver is synchronous and will block while playing a buffer.\n");
	        }
	        if(caps.dwSupport & WAVECAPS_SAMPLEACCURATE)
	        {
	        	printf(" - returns sample-accurate position information.\n");
	        }
	        printf("\n");
	    }
	}

	wavbuf = load_file(filename, &wavbuflen);
	if(wavbuf == NULL)
	{
		fprintf(stderr, "Failed to load file %s\n", filename);
		return 0;
	}

	pcm_init();

	s = pcm_sample_open(11025, 8, 1, wavbuf, wavbuflen);

	printf("Loaded %s\n", filename);

	printf("\n(p) play/pause, (s) stop, (l) loop on/off, (q) quit, (+/-) volume up/down\n");
	printf("\r       \rStopped");
	err = 0;
	while((c = getch()) != 'q')
	{
		switch(c)
		{
		case 'l':
		case 'L':
//			m->looping = !m->looping;
			break;
		case 'p':
		case 'P':
			if(pcm_is_stopped(s))
			{
				err = pcm_play(s);
				if(err != MMSYSERR_NOERROR)
				{
					fprintf(stderr, "Error playing file %s, error %d\n", filename, err);
					goto error;
				}
				printf("\r       \rPlaying");
			}
			else if(pcm_is_playing(s))
			{
				err = pcm_pause(s);
				if(err != MMSYSERR_NOERROR)
				{
					fprintf(stderr, "Error pausing file %s, error %d\n", filename, err);
					goto error;
				}
				printf("\r       \rPaused");
			}
			else if(pcm_is_paused(s))
			{
				err = pcm_resume(s);
				if(err != MMSYSERR_NOERROR)
				{
					fprintf(stderr, "Error pausing file %s, error %d\n", filename, err);
					goto error;
				}
				printf("\r       \rPlaying");
			}
			break;
		case 's':
		case 'S':
			err = pcm_stop(s);
			if(err != MMSYSERR_NOERROR)
			{
				fprintf(stderr, "Error stopping file %s, error %d\n", filename, err);
				goto error;
			}
			printf("\r       \rStopped");
			break;
		case '-':
		case '_':
			err = pcm_volume(s, VOL_DOWN);
			if(err != MMSYSERR_NOERROR)
			{
				fprintf(stderr, "Error adjusting volume, error %d\n", err);
				goto error;
			}
			break;
		case '=':
		case '+':
			err = pcm_volume(s, VOL_UP);
			if(err != MMSYSERR_NOERROR)
			{
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

unsigned char* load_file(unsigned char* filename, unsigned int* len)
{
	unsigned char* buf;
	unsigned int ret;
	FILE* f = fopen((char*)filename, "rb");
	if(f == NULL)
		return 0;

	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);

	buf = (unsigned char*)malloc(*len);

	if(buf == 0)
	{
		fclose(f);
		return 0;
	}

	ret = fread(buf, 1, *len, f);
	fclose(f);

	if(ret != *len)
	{
		free(buf);
		return 0;
	}

	return buf;
}

#endif
