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
 * mid_player.c
 *
 *  Created on: Dec 3, 2011
 *      Author: David J. Rager
 */
#include <stdio.h>
#include <conio.h>
#include <limits.h>
#include <windows.h>
#include <mmsystem.h>

#include "mid_player.h"

#ifndef MID_PLAYER_STANDALONE

#include "dj_debug.h"
#include "djmm_utils.h"

#else

static unsigned long read_var_long(unsigned char* buf, unsigned int* inc);
static unsigned short swap_bytes_short(unsigned short in);
static unsigned long swap_bytes_long(unsigned long in);

#define DJ_TRACE printf
#define ERROR_BUFFER_SIZE 1024
static unsigned char* DJ_FORMAT_MESSAGE(unsigned int error)
{
	static unsigned char buffer[ERROR_BUFFER_SIZE];

	FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, 0, error, 0, (char*)buffer, ERROR_BUFFER_SIZE, 0);

	return buffer;
}

#endif

#pragma pack(push, 1)

/**
 * @brief This is the header structure at the beginning of a MIDI file on
 * disk.
 *
 * This struct represents the header structure at the beginning of a MIDI file
 * on disk.
 * @code
 * struct _mid_header {
 * 	unsigned int	id;
 * 	unsigned int	size;
 * 	unsigned short	format;
 * 	unsigned short  tracks;
 * 	unsigned short	ticks;
 * };
 * @endcode
 */
struct _mid_header {
	/**
	 * @brief This identifies the MIDI header and is always the four byte value
	 * "MThd".
	 *
	 * This field identifies the MIDI header and is always the four byte value
	 * "MThd".
	 */
	unsigned int	id;
	/**
	 * @brief This specifies the size of the MIDI header in bytes excluding the
	 * @ref id and @ref size fields.
	 *
	 * This field specifies the size of the MIDI header in bytes excluding the
	 * @ref id and @ref size fields. This value is in big-endian format and is
	 * always 6.
	 */
	unsigned int	size;
	/**
	 * @brief This specifies the format of the MIDI file.
	 *
	 * This field specifies the format of the MIDI file. This value is in
	 * big-endian format and takes one of the following values:
	 * @li	0	The MIDI file contains a single track.
	 * @li	1	The MIDI file contains multiple synchronous tracks.
	 * @li	2	The MIDI file contains multiple asynchronous tracks.
	 */
	unsigned short	format;
	/**
	 * @brief This specifies the number of tracks in the MIDI file.
	 *
	 * This field specifies the number of tracks in the MIDI file. If the \ref
	 * format field is 0, this value is always 1. This value is in big-endian
	 * format.
	 */
	unsigned short  tracks;
	/**
	 * @brief This specifies the number of ticks per quarter note.
	 *
	 * This field specifies the number of pulses (ticks) per quarter note
	 * (PPQN). This value is used together with the tempo event to determine
	 * the speed of playback. This value is in big-endian format
	 */
	unsigned short	ticks;
};

struct _mid_track {
	unsigned int	id;		// identifier "MTrk"
	unsigned int	length;	// track length, big-endian
};

struct _mid_event {
	unsigned char channel:4;
	unsigned char command:3;
	unsigned char one:1;		// always 1
};

#pragma pack(pop)

#define STATE_ERROR		0
#define STATE_STARTING	1
#define STATE_PLAYING	2
#define STATE_PAUSED	3
#define STATE_STOPPING	4
#define STATE_STOPPED	5
#define STATE_SHUTDOWN	6

static unsigned int is_mid_header(unsigned char* buf, unsigned int len);
static const unsigned int MID_ID = 'dhTM';

struct mid_player
{
	HANDLE event;
	HANDLE thread;
	HANDLE ready;
	HANDLE mutex;

	HMIDISTRM stream;
	MIDIHDR header[2]; // double buffer

	unsigned int device;

	unsigned int state;
	unsigned int looping;

	struct mid_score* score;
	mid_notify_cb cb;
	
	struct mid_player* next;
};

struct trk {
	struct _mid_track* track;
	unsigned char* buf;
	unsigned char last_event;
	unsigned int absolute_time;
};

struct evt {
	unsigned int absolute_time;
	union {
		unsigned char event;
		struct _mid_event e;
	};
	unsigned char* data;
};

struct mid_score
{
	unsigned int timebase;

	unsigned char* raw_bytes;
	unsigned int raw_len;

	unsigned int curr_time;

	unsigned int num_tracks;
	struct trk* tracks;
};

#define MAX_BUFFER_SIZE (4096 * 12)

struct mid_player_event
{
	union {
		unsigned char byte;
		struct {
			unsigned char channel:4;
			unsigned char command:3;
			unsigned char last:1;
		};
	};
	unsigned char* ptr;
};

static unsigned char* mid_format_error(unsigned int err);
static unsigned int mid_get_streambuf(struct mid_score* m, unsigned int* out, unsigned int* outlen);
static void mid_player_shutdown(struct mid_player* p);
static void mid_close_stream(struct mid_player* p);
static void mid_rewind(struct mid_score* m);

static DJ_RESULT mid_lock_score(DJ_HANDLE h);
static DJ_RESULT mid_unlock_score(DJ_HANDLE h);

static void CALLBACK mid_callback_proc(HMIDIOUT hmo, UINT wMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
	unsigned int err;

	struct mid_player* p = (struct mid_player*)dwInstance;
	if(p == NULL)
		return;

	switch (wMsg)
	{
	case MOM_POSITIONCB:
		break;
	case MOM_DONE:
		// The last event in the queued buffer has played. Signal the
		// player thread to continue.
		err = SetEvent(p->event);
		if(err == 0)
		{
			DJ_TRACE("mid_callback_proc(): SetEvent failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		}

		break;
	case MOM_OPEN:
		break;
	case MOM_CLOSE:
		break;
	}

}

static DWORD WINAPI mid_player_proc(LPVOID lpParameter)
{
	struct mid_player* p = (struct mid_player*)lpParameter;
	unsigned int err = MMSYSERR_NOERROR;

	unsigned int idx = 0;

	err = WaitForSingleObject(p->mutex, INFINITE);
	if(err == WAIT_FAILED)
	{
		DJ_TRACE("mid_player_proc(): WaitForSingleObject failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		return MMSYSERR_ERROR;
	}

	while((p->state != STATE_SHUTDOWN) && (p->state != STATE_ERROR))
	{
		switch(p->state)
		{
		case STATE_PLAYING:
			err = ReleaseMutex(p->mutex);
			if(err == 0)
			{
				DJ_TRACE("mid_player_proc(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
				return MMSYSERR_ERROR;
			}

			err = WaitForSingleObject(p->event, INFINITE);
			if(err == WAIT_FAILED)
			{
				DJ_TRACE("mid_player_proc(): WaitForSingleObject failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
				return MMSYSERR_ERROR;
			}

			err = WaitForSingleObject(p->mutex, INFINITE);
			if(err == WAIT_FAILED)
			{
				DJ_TRACE("mid_player_proc(): WaitForSingleObject failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
				return MMSYSERR_ERROR;
			}

			if(p->state == STATE_SHUTDOWN)
				break;

			if((p->state == STATE_PLAYING) || (p->state == STATE_PAUSED))
			{
				err = mid_get_streambuf(p->score, (unsigned int*)p->header[idx].lpData, (unsigned int*)&p->header[idx].dwBufferLength);
				if(err == 0)
				{
					DJ_TRACE("mid_player_proc(): mid_get_streambuf failed\n");
					err = ReleaseMutex(p->mutex);
					if(err == 0)
						DJ_TRACE("mid_player_proc(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
					return MMSYSERR_ERROR;
				}

				p->header[idx].dwBytesRecorded = p->header[idx].dwBufferLength;
				if(p->header[idx].dwBufferLength > 0)
				{
					p->header[idx].dwFlags &= ~WHDR_DONE;
					err = midiStreamOut(p->stream, &p->header[idx], sizeof(MIDIHDR));

					// we should never get MIDIERR_STILLPLAYING but it shouldn't hurt anything if we do
					if(err != MMSYSERR_NOERROR && err != MIDIERR_STILLPLAYING)
					{
						DJ_TRACE("mid_player_proc(): midiStreamOut failed: %d, %s\n", err, mid_format_error(err));
						err = ReleaseMutex(p->mutex);
						if(err == 0)
							DJ_TRACE("mid_player_proc(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
						return err;
					}

					idx = (idx + 1) % 2;
				}
				else
				{
					if(p->looping)
					{
						mid_rewind(p->score);
						err = mid_get_streambuf(p->score, (unsigned int*)p->header[idx].lpData, (unsigned int*)&p->header[idx].dwBufferLength);
						if(err == 0)
						{
							DJ_TRACE("mid_player_proc(): mid_get_streambuf failed\n");
							err = ReleaseMutex(p->mutex);
							if(err == 0)
								DJ_TRACE("mid_player_proc(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
							return MMSYSERR_ERROR;
						}

						p->header[idx].dwBytesRecorded = p->header[idx].dwBufferLength;
						p->header[idx].dwFlags &= ~MHDR_DONE;
						err = midiStreamOut(p->stream, &p->header[idx], sizeof(MIDIHDR));
						if(err != MMSYSERR_NOERROR && err != MIDIERR_STILLPLAYING)
						{
							DJ_TRACE("mid_player_proc(): midiStreamOut failed: %d, %s\n", err, mid_format_error(err));
							err = ReleaseMutex(p->mutex);
							if(err == 0)
								DJ_TRACE("mid_player_proc(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
							return err;
						}

						idx = (idx + 1) % 2;
					}
					else
					{
						// one more buffer left playing, wait for it to finish
						err = ReleaseMutex(p->mutex);
						if(err == 0)
						{
							DJ_TRACE("mid_player_proc(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
							return MMSYSERR_ERROR;
						}

						err = WaitForSingleObject(p->event, INFINITE);
						if(err == WAIT_FAILED)
						{
							DJ_TRACE("mid_player_proc(): WaitForSingleObject failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
							return MMSYSERR_ERROR;
						}

						err = WaitForSingleObject(p->mutex, INFINITE);
						if(err == WAIT_FAILED)
						{
							DJ_TRACE("mid_player_proc(): WaitForSingleObject failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
							return MMSYSERR_ERROR;
						}

						if(p->state == STATE_SHUTDOWN)
							break;

						mid_close_stream(p);

						// don't bother releasing the mutex and looping again,
						// just enter the stopped state.
						goto stopped;
					}
				}
			}

			err = ReleaseMutex(p->mutex);
			if(err == 0)
			{
				DJ_TRACE("mid_player_proc(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
				return MMSYSERR_ERROR;
			}

			break;
		case STATE_STOPPED:
stopped:
			idx = 0;
			if(p->cb)
				p->cb(p->state);
			mid_rewind(p->score);

			err = SetEvent(p->ready);
			if(err == 0)
			{
				DJ_TRACE("mid_player_proc(): SetEvent failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
				err = ReleaseMutex(p->mutex);
				if(err == 0)
				{
					DJ_TRACE("mid_player_proc(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
				}
				return MMSYSERR_ERROR;
			}

			err = ReleaseMutex(p->mutex);
			if(err == 0)
			{
				DJ_TRACE("mid_player_proc(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
				return MMSYSERR_ERROR;
			}

			err = WaitForSingleObject(p->event, INFINITE);
			if(err == WAIT_FAILED)
			{
				DJ_TRACE("mid_player_proc(): WaitForSingleObject failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
				return MMSYSERR_ERROR;
			}

			break;
		}

		err = WaitForSingleObject(p->mutex, INFINITE);
		if(err == WAIT_FAILED)
		{
			DJ_TRACE("mid_player_proc(): WaitForSingleObject failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
			return MMSYSERR_ERROR;
		}
	}
	err = ReleaseMutex(p->mutex);
	if(err == 0)
	{
		DJ_TRACE("mid_player_proc(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		return MMSYSERR_ERROR;
	}

    return 0;
}

static DJ_HANDLE players_mutex = NULL;
static struct mid_player* players = NULL; // player handle list.

DJ_RESULT mid_init()
{
	players = NULL;

	players_mutex = CreateMutex(NULL, FALSE, NULL);
	if(players_mutex == NULL)
	{
		DJ_TRACE("mid_init(): CreateMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		return MMSYSERR_ERROR;
	}

	return MMSYSERR_NOERROR;
}

void mid_shutdown()
{
	unsigned int err = MMSYSERR_NOERROR;

	struct mid_player* tmp = NULL;
	err = WaitForSingleObject(players_mutex, INFINITE);
	if(err == WAIT_FAILED)
	{
		DJ_TRACE("mid_shutdown(): WaitForSingleObject failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
	}

	tmp = players;
	while(tmp != NULL)
	{
		err = ReleaseMutex(players_mutex);
		if(err == 0)
		{
			DJ_TRACE("mid_shutdown(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		}

		// This function acquires players_mutex and tmp->mutex so make sure
		// they are not held here.
		mid_score_close(tmp);

		err = WaitForSingleObject(players_mutex, INFINITE);
		if(err == WAIT_FAILED)
		{
			DJ_TRACE("mid_shutdown(): WaitForSingleObject failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		}

		tmp = players;
	}
	err = ReleaseMutex(players_mutex);
	if(err == 0)
	{
		DJ_TRACE("mid_shutdown(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
	}

	// At this point all handles are closed and the global list empty.
	err = CloseHandle(players_mutex);
	if(err == 0)
	{
		DJ_TRACE("mid_shutdown(): CloseHandle failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
	}
}

/**
 * @brief Internal function to initialize a player.
 *
 * This function initializes a player structure and spawns a thread to process
 * buffered MIDIEVENT structures.
 *
 * This function does not acquire any locks. The function does wait on an event
 * signaled from the thread when it is started and waiting in the STATE_STOPPED
 * state.
 *
 * This function is called by mid_score_open() without holding any locks.
 *
 * @return The handle to the new player or NULL if creation failed.
 */
static struct mid_player* mid_player_init()
{
	unsigned int err;

	struct mid_player* p = (struct mid_player*)malloc(sizeof(struct mid_player));
	if(p == NULL)
	{
		DJ_TRACE("mid_player_init(): malloc failed\n");
		return NULL;
	}

	// Initialize the double buffers.
	ZeroMemory(&p->header[0], sizeof(MIDIHDR));
	p->header[0].lpData = (char*)malloc(MAX_BUFFER_SIZE);
	if(p->header[0].lpData == NULL)
	{
		DJ_TRACE("mid_player_init(): malloc failed\n");
		goto error1;
	}

	p->header[0].dwBufferLength = p->header[0].dwBytesRecorded = MAX_BUFFER_SIZE;

	ZeroMemory(&p->header[1], sizeof(MIDIHDR));
	p->header[1].lpData = (char*)malloc(MAX_BUFFER_SIZE);
	if(p->header[1].lpData == NULL)
	{
		DJ_TRACE("mid_player_init(): malloc failed\n");
		goto error2;
	}

	p->header[1].dwBufferLength = p->header[1].dwBytesRecorded = MAX_BUFFER_SIZE;

	p->device = 0;
	p->score = NULL;
	p->state = STATE_STOPPED;
	p->looping = 0;
	p->stream = 0;
	p->cb = NULL;

	// Create the internal mutex.
	p->mutex = CreateMutex(NULL, FALSE, NULL);
	if(p->mutex == NULL)
	{
		DJ_TRACE("mid_player_init(): CreateMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		goto error3;
	}

	// Create the event object that is signaled by the MIDI api and state
	// changes.
	p->event = CreateEvent(0, FALSE, FALSE, 0);
	if(p->event == NULL)
	{
		DJ_TRACE("mid_player_init(): CreateEvent failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		goto error4;
	}

	// Create the event that signals when the player is stopped and ready.
	// This event is used in this function when the thread is first
	// initialized. It is also used in mid_stop() to signal when the thread
	// has settled into its STATE_STOPPED state.
	p->ready = CreateEvent(0, FALSE, FALSE, 0);
	if(p->ready == NULL)
	{
		DJ_TRACE("mid_player_init(): CreateEvent failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		goto error5;
	}

	// Finally, spawn the worker thread.
	p->thread = CreateThread(NULL, 0, mid_player_proc, p, 0, NULL);
	if(p->thread == NULL)
	{
		DJ_TRACE("mid_player_init(): CreateThread failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		goto error6;
	}

	// Make sure the thread is spun up and ready.
	err = WaitForSingleObject(p->ready, INFINITE);
	if(err == WAIT_FAILED)
	{
		DJ_TRACE("mid_player_init(): WaitForSingleObject failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		goto error7;
	}

	return p;

error7:
	p->state = STATE_SHUTDOWN;
	err = SetEvent(p->event);
	if(err == 0)
	{
		DJ_TRACE("mid_player_init(): SetEvent failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		goto error7;
	}

	err = WaitForSingleObject(p->thread, INFINITE);
	if(err == WAIT_FAILED)
	{
		DJ_TRACE("mid_player_init(): WaitForSingleObject failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		goto error7;
	}

	err = CloseHandle(p->thread);
	if(err == 0)
	{
		DJ_TRACE("mid_player_init(): CloseHandle failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		goto error7;
	}

error6:
	err = CloseHandle(p->ready);
	if(err == 0)
	{
		DJ_TRACE("mid_player_init(): CloseHandle failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
	}

error5:
	err = CloseHandle(p->event);
	if(err == 0)
	{
		DJ_TRACE("mid_player_init(): CloseHandle failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
	}

error4:
	err = CloseHandle(p->mutex);
	if(err == 0)
	{
		DJ_TRACE("mid_player_init(): CloseHandle failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
	}

error3:
	free(p->header[1].lpData);

error2:
	free(p->header[0].lpData);

error1:
	free(p);
	return NULL;
}

/**
 * @brief This function shuts down a player.
 *
 * This function shuts down a player. It acquires the p->mutex to wait for
 * exclusive access to the player. Once it has exclusive access, it sets the
 * state to STATE_SHUTDOWN and signals p->event to cause the main thread loop
 * to finish. Once the thread is signaled, it waits on the thread handle until
 * the thread exits. Finally, it cleans up its resources.
 *
 * This function is called by mid_score_open() with no locks held. It is called
 * in the event of an error, in which case the player has not yet been inserted
 * into the global list.
 *
 * This function is called by mid_score_close() with no locks held.
 *
 * @param p	The pointer to the mid_player struct to shut down.
 */
static void mid_player_shutdown(struct mid_player* p)
{
	unsigned int err;
	err = WaitForSingleObject(p->mutex, INFINITE);
	if(err == WAIT_FAILED)
	{
		DJ_TRACE("mid_player_shutdown(): WaitForSingleObject failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
	}

	p->state = STATE_SHUTDOWN;

	err = SetEvent(p->event);
	if(err == 0)
	{
		DJ_TRACE("mid_player_shutdown(): SetEvent failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
	}

	err = ReleaseMutex(p->mutex);
	if(err == 0)
	{
		DJ_TRACE("mid_player_shutdown(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
	}

	err = WaitForSingleObject(p->thread, INFINITE);
	if(err == WAIT_FAILED)
	{
		DJ_TRACE("mid_player_shutdown(): WaitForSingleObject failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
	}

	err = CloseHandle(p->event);
	if(err == 0)
	{
		DJ_TRACE("mid_player_shutdown(): CloseHandle failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
	}

	err = CloseHandle(p->ready);
	if(err == 0)
	{
		DJ_TRACE("mid_player_shutdown(): CloseHandle failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
	}

	err = CloseHandle(p->mutex);
	if(err == 0)
	{
		DJ_TRACE("mid_player_shutdown(): CloseHandle failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
	}

	err = CloseHandle(p->thread);
	if(err == 0)
	{
		DJ_TRACE("mid_player_shutdown(): CloseHandle failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
	}

	free(p->header[0].lpData);
	free(p->header[1].lpData);
	free(p);
}

DJ_HANDLE mid_score_open(unsigned char* buf, unsigned int len)
{
	unsigned int err = MMSYSERR_NOERROR;
	struct mid_player* p = NULL;
	struct mid_score* s = NULL;
	struct _mid_header* hdr = NULL;

	if(!is_mid_header(buf, len))
	{
		DJ_TRACE("mid_score_open(): not a valid MIDI header\n");
		goto error1;
	}

	p = mid_player_init();
	if(p == NULL)
	{
		DJ_TRACE("mid_score_open(): could not initialize player\n");
		goto error1;
	}

	// We have a player, buffers initialized and thread spun up and
	// ready. Prepare the score data for processing.

	s = (struct mid_score*)malloc(sizeof(struct mid_score));
	if(s == NULL)
	{
		DJ_TRACE("mid_score_open(): malloc failed\n");
		goto error2;
	}

	s->raw_bytes = (unsigned char*)malloc(len);
	if(s->raw_bytes == NULL)
	{
		DJ_TRACE("mid_score_open(): malloc failed\n");
		goto error3;
	}

	memcpy(s->raw_bytes, buf, len);
	s->raw_len = len;

	hdr = (struct _mid_header*)s->raw_bytes;
	s->num_tracks = swap_bytes_short(hdr->tracks);

	s->tracks = (struct trk*)malloc(s->num_tracks * sizeof(struct trk));
	if(s->tracks == NULL)
	{
		DJ_TRACE("mid_score_open(): malloc failed\n");
		goto error4;
	}

	s->timebase = swap_bytes_short(hdr->ticks);
	mid_rewind(s);

	p->score = s;

	// Score is loaded and ready. Add the player to our global list
	// and return the "HANDLE" to the user.

	err = WaitForSingleObject(players_mutex, INFINITE);
	if(err == WAIT_FAILED)
	{
		DJ_TRACE("mid_score_open(): WaitForSingleObject failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		goto error5;
	}

	p->next = players;
	players = p;
	err = ReleaseMutex(players_mutex);
	if(err == 0)
	{
		DJ_TRACE("mid_score_open(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		goto error5;
	}

	return p;

error5:
	free(s->tracks);

error4:
	free(s->raw_bytes);

error3:
	free(s);

error2:
	mid_player_shutdown(p);
	p = NULL;

error1:
	return NULL;
}

/**
 * @brief This function determines if a HANDLE is was opened with mid_score_open().
 *
 * This function determines if a HANDLE is was opened with mid_score_open() and
 * is in the global list. This helper function should be called while holding
 * the players_mutex.
 *
 * @param h	The handle to test
 * @return	TRUE if the handle is valid, FALSE otherwise.
 */
static boolean mid_is_handle_valid(DJ_HANDLE h)
{
	struct mid_player* p = players;
	if(h == NULL)
		return false;

	while(p != NULL)
	{
		if(p == h)
			return true;
		else
			p = p->next;
	}

	return false;
}

/**
 * @brief This function stops any buffers that are currently playing and closes the
 * stream.
 *
 * This function stops any buffers that are currently playing and closes the
 * stream. This function should be called while holding p->mutex.
 *
 * @param p	The pointer to a mid_player structure to close.
 */
static void mid_close_stream(struct mid_player* p)
{
	unsigned int err;
	p->state = STATE_STOPPED;
	err = midiOutReset((HMIDIOUT)p->stream);
	if(err != MMSYSERR_NOERROR)
	{
		DJ_TRACE("mid_close_stream(): midiOutReset failed: %d, %s\n", err, mid_format_error(err));
	}

	err = midiOutUnprepareHeader((HMIDIOUT)p->stream, &p->header[0], sizeof(MIDIHDR));
	if(err != MMSYSERR_NOERROR)
	{
		DJ_TRACE("mid_close_stream(): midiOutUnprepareHeader failed: %d, %s\n", err, mid_format_error(err));
	}

	err = midiOutUnprepareHeader((HMIDIOUT)p->stream, &p->header[1], sizeof(MIDIHDR));
	if(err != MMSYSERR_NOERROR)
	{
		DJ_TRACE("mid_close_stream(): midiOutUnprepareHeader failed: %d, %s\n", err, mid_format_error(err));
	}

	midiStreamClose(p->stream);
	if(err != MMSYSERR_NOERROR)
	{
		DJ_TRACE("mid_close_stream(): midiStreamClose failed: %d, %s\n", err, mid_format_error(err));
	}

	p->stream = 0;
}

void mid_score_close(DJ_HANDLE h)
{
	unsigned int err;
	struct mid_player* p = NULL;

	err = WaitForSingleObject(players_mutex, INFINITE);
	if(err == WAIT_FAILED)
	{
		DJ_TRACE("mid_score_close(): WaitForSingleObject failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
	}

	if(mid_is_handle_valid(h) == false)
	{
		err = ReleaseMutex(players_mutex);
		if(err == 0)
		{
			DJ_TRACE("mid_score_close(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		}
	}

	p = players;
	if(p == h) // if h is the first in the list
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

	err = ReleaseMutex(players_mutex);
	if(err == 0)
	{
		DJ_TRACE("mid_score_close(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
	}

	// The player has been removed from the global list. Any further calls
	// using this handle will return MMSYSERR_INVALPARAM.

	// Start shutting down the thread. If it's still playing, stop it.
	err = WaitForSingleObject(p->mutex, INFINITE);
	if(err == WAIT_FAILED)
	{
		DJ_TRACE("mid_score_close(): WaitForSingleObject failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
	}

	if(p->state != STATE_STOPPED)
	{
		err = ResetEvent(p->ready);
		if(err == 0)
		{
			DJ_TRACE("mid_score_close(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		}

		mid_close_stream(p);
		err = SetEvent(p->event);
		if(err == 0)
		{
			DJ_TRACE("mid_score_close(): SetEvent failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		}

		err = ReleaseMutex(p->mutex);
		if(err == 0)
		{
			DJ_TRACE("mid_score_close(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		}

		err = WaitForSingleObject(p->ready, INFINITE);
		if(err == WAIT_FAILED)
		{
			DJ_TRACE("mid_score_close(): WaitForSingleObject failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		}
	}
	else
	{
		err = ReleaseMutex(p->mutex);
		if(err == 0)
		{
			DJ_TRACE("mid_score_close(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		}
	}

	// Player thread should be in the STATE_STOPPED state. No existing
	// handles can restart it. Let's close things out.

	if(p->score)
		free(p->score->raw_bytes);

	free(p->score);
	p->score = NULL;

	mid_player_shutdown(p);

	return;
}

DJ_RESULT mid_register_callback(DJ_HANDLE h, mid_notify_cb cb)
{
	struct mid_player* p = (struct mid_player*)h;
	unsigned int err = MMSYSERR_NOERROR;

	err = mid_lock_score(h);
	if(err != MMSYSERR_NOERROR)
	{
		DJ_TRACE("mid_pause(): mid_lock_score failed: %d, %s\n", err, mid_format_error(err));
		return err;
	}

	p->cb = cb;

	err = mid_unlock_score(h);
	if(err != MMSYSERR_NOERROR)
	{
		DJ_TRACE("mid_play(): mid_unlock_score failed: %d, %s\n", err, mid_format_error(err));
		return err;
	}

	return MMSYSERR_NOERROR;
}

DJ_RESULT mid_play(DJ_HANDLE h)
{
	struct mid_player* p = (struct mid_player*)h;
	unsigned int err = MMSYSERR_NOERROR;
	MIDIPROPTIMEDIV prop;

	err = mid_lock_score(h);
	if(err != MMSYSERR_NOERROR)
	{
		DJ_TRACE("mid_play(): mid_lock_score failed: %d, %s\n", err, mid_format_error(err));
		return err;
	}

	if(p->state == STATE_STOPPED)
	{
		err = midiStreamOpen(&p->stream, &p->device, 1, (DWORD_PTR)mid_callback_proc, (DWORD_PTR)p, CALLBACK_FUNCTION);
		if(err != MMSYSERR_NOERROR)
		{
			DJ_TRACE("mid_play(): midiStreamOpen failed: %d, %s\n", err, mid_format_error(err));
			goto error;
		}

		prop.cbStruct = sizeof(MIDIPROPTIMEDIV);
		prop.dwTimeDiv = p->score->timebase;
		err = midiStreamProperty(p->stream, (LPBYTE)&prop, MIDIPROP_SET|MIDIPROP_TIMEDIV);
		if(err != MMSYSERR_NOERROR)
		{
			DJ_TRACE("mid_play(): midiStreamProperty failed: %d, %s\n", err, mid_format_error(err));
			goto error;
		}

		err = mid_get_streambuf(p->score, (unsigned int*)p->header[0].lpData, (unsigned int*)&p->header[0].dwBufferLength);
		if(err == 0)
		{
			DJ_TRACE("mid_play(): mid_get_streambuf failed\n");
			goto error;
		}

		p->header[0].dwBytesRecorded = p->header[0].dwBufferLength;
		p->header[0].dwFlags = 0;
		err = midiOutPrepareHeader((HMIDIOUT)p->stream, &p->header[0], sizeof(MIDIHDR));
		if(err != MMSYSERR_NOERROR)
		{
			DJ_TRACE("mid_play(): midiOutPrepareHeader failed: %d, %s\n", err, mid_format_error(err));
			goto error;
		}

		err = midiStreamOut(p->stream, &p->header[0], sizeof(MIDIHDR));
		if(err == MMSYSERR_NOERROR)
		{
			err = mid_get_streambuf(p->score, (unsigned int*)p->header[1].lpData, (unsigned int*)&p->header[1].dwBufferLength);
			if(err == 0)
			{
				DJ_TRACE("mid_play(): mid_get_streambuf failed\n");
				goto error;
			}

			p->header[1].dwBytesRecorded = p->header[1].dwBufferLength;
			p->header[1].dwFlags = 0;
			err = midiOutPrepareHeader((HMIDIOUT)p->stream, &p->header[1], sizeof(MIDIHDR));
			if(err != MMSYSERR_NOERROR)
			{
				DJ_TRACE("mid_play(): midiOutPrepareHeader failed: %d, %s\n", err, mid_format_error(err));
				goto error;
			}

			err = midiStreamOut(p->stream, &p->header[1], sizeof(MIDIHDR));
			if(err == MMSYSERR_NOERROR)
			{
				err = midiStreamRestart(p->stream);
				if(err == MMSYSERR_NOERROR)
				{
					p->state = STATE_PLAYING;
					SetEvent(p->event);
				}
				else
				{
					DJ_TRACE("mid_play(): midiStreamRestart failed: %d, %s\n", err, mid_format_error(err));
					goto error;
				}
			}
			else
			{
				DJ_TRACE("mid_play(): midiStreamOut failed: %d, %s\n", err, mid_format_error(err));
				goto error;
			}
		}
		else
		{
			DJ_TRACE("mid_play(): midiStreamOut failed: %d, %s\n", err, mid_format_error(err));
			goto error;
		}
	}

error:
	err = mid_unlock_score(h);
	if(err != MMSYSERR_NOERROR)
	{
		DJ_TRACE("mid_play(): mid_unlock_score failed: %d, %s\n", err, mid_format_error(err));
		return err;
	}

	return MMSYSERR_NOERROR;
}

DJ_RESULT mid_stop(DJ_HANDLE h)
{
	struct mid_player* p = (struct mid_player*)h;
	unsigned int err = MMSYSERR_NOERROR;

	err = mid_lock_score(h);
	if(err != MMSYSERR_NOERROR)
	{
		DJ_TRACE("mid_stop(): mid_lock_score failed: %lu, %s\n", err, mid_format_error(err));
		return err;
	}

	if(p->state != STATE_STOPPED)
	{
		err = ResetEvent(p->ready);
		if(err == 0)
		{
			DJ_TRACE("mid_stop(): ResetEvent failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
			err = ReleaseMutex(p->mutex);
			if(err == 0)
			{
				DJ_TRACE("mid_stop(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
			}

			return MMSYSERR_ERROR;
		}

		mid_close_stream(p);

		err = SetEvent(p->event);
		if(err == 0)
		{
			DJ_TRACE("mid_stop(): SetEvent failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
			err = mid_unlock_score(h);
			if(err != MMSYSERR_NOERROR)
			{
				DJ_TRACE("mid_stop(): mid_unlock_score failed: %lu, %s\n", err, mid_format_error(err));
				return err;
			}

			return MMSYSERR_ERROR;
		}

		err = mid_unlock_score(h);
		if(err != MMSYSERR_NOERROR)
		{
			DJ_TRACE("mid_stop(): mid_unlock_score failed: %lu, %s\n", err, mid_format_error(err));
			return err;
		}

		err = WaitForSingleObject(p->ready, INFINITE);
		if(err == WAIT_FAILED)
		{
			DJ_TRACE("mid_stop(): WaitForSingleObject failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
			return MMSYSERR_ERROR;
		}
	}
	else
	{
		err = mid_unlock_score(h);
		if(err != MMSYSERR_NOERROR)
		{
			DJ_TRACE("mid_stop(): mid_unlock_score failed: %lu, %s\n", err, mid_format_error(err));
			return err;
		}
	}

	return MMSYSERR_NOERROR;
}

DJ_RESULT mid_pause(DJ_HANDLE h)
{
	struct mid_player* p = (struct mid_player*)h;
	unsigned int err = MMSYSERR_NOERROR;

	err = mid_lock_score(h);
	if(err != MMSYSERR_NOERROR)
	{
		DJ_TRACE("mid_pause(): mid_lock_score failed: %d, %s\n", err, mid_format_error(err));
		return err;
	}

	if(p->state == STATE_PLAYING)
	{
		err = midiStreamPause(p->stream);
		if(err != MMSYSERR_NOERROR)
		{
			DJ_TRACE("mid_pause(): err pausing: %d, %s\n", err, mid_format_error(err));
			p->state = STATE_ERROR;
			err = ReleaseMutex(p->mutex);
			if(err == 0)
			{
				DJ_TRACE("mid_pause(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
			}

			return MMSYSERR_ERROR;
		}
		else
			p->state = STATE_PAUSED;
	}

	err = mid_unlock_score(h);
	if(err != MMSYSERR_NOERROR)
	{
		DJ_TRACE("mid_pause(): mid_unlock_score failed: %d, %s\n", err, mid_format_error(err));
		return err;
	}

	return MMSYSERR_NOERROR;
}

DJ_RESULT mid_resume(DJ_HANDLE h)
{
	struct mid_player* p = (struct mid_player*)h;
	unsigned int err = MMSYSERR_NOERROR;

	err = mid_lock_score(h);
	if(err != MMSYSERR_NOERROR)
	{
		DJ_TRACE("mid_resume(): mid_lock_score failed: %d, %s\n", err, mid_format_error(err));
		return err;
	}

	if(p->state == STATE_PAUSED)
	{
		err = midiStreamRestart(p->stream);
		if(err != MMSYSERR_NOERROR)
		{
			DJ_TRACE("mid_resume(): err restart: %d, %s\n", err, mid_format_error(err));
			p->state = STATE_ERROR;
			err = ReleaseMutex(p->mutex);
			if(err == 0)
			{
				DJ_TRACE("mid_resume(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
			}

			return MMSYSERR_ERROR;
		}
		else
			p->state = STATE_PLAYING;
	}

	err = mid_unlock_score(h);
	if(err != MMSYSERR_NOERROR)
	{
		DJ_TRACE("mid_resume(): mid_unlock_score failed: %d, %s\n", err, mid_format_error(err));
		return err;
	}

	return MMSYSERR_NOERROR;
}

DJ_RESULT mid_set_volume_left(DJ_HANDLE h, unsigned int level)
{
	unsigned int old = 0, vol = 0;
	struct mid_player* p = (struct mid_player*)h;
	unsigned int err = MMSYSERR_NOERROR;
	HMIDISTRM stream = (HMIDISTRM)MIDI_MAPPER;

	err = mid_lock_score(h);
	if(err == MMSYSERR_NOERROR)
	{
		stream = p->stream;
	}
	else if(err != MMSYSERR_INVALPARAM)
	{
		DJ_TRACE("mid_set_volume_left(): mid_lock_score failed: %d, %s\n", err, mid_format_error(err));
		return err;
	}

	err = midiOutGetVolume((HMIDIOUT)stream, (LPDWORD)&old);
	if(err == MMSYSERR_NOERROR)
	{
		vol = MAKELONG(LOWORD(level), HIWORD(old));
		err = midiOutSetVolume((HMIDIOUT)stream, vol);
	}
	else
	{
		DJ_TRACE("mid_set_volume_left(): err setting left volume: %d, %s\n", err, mid_format_error(err));
	}

	err = mid_unlock_score(h);
	if(err == MMSYSERR_INVALPARAM)
	{
		DJ_TRACE("mid_set_volume_left(): mid_unlock_score failed: %d, %s\n", err, mid_format_error(err));
		return MMSYSERR_NOERROR;
	}

	return err;
}

DJ_RESULT mid_set_volume_right(DJ_HANDLE h, unsigned int level)
{
	unsigned int old = 0, vol = 0;
	struct mid_player* p = (struct mid_player*)h;
	unsigned int err = MMSYSERR_NOERROR;
	HMIDISTRM stream = (HMIDISTRM)MIDI_MAPPER;

	err = mid_lock_score(h);
	if(err == MMSYSERR_NOERROR)
	{
		stream = p->stream;
	}
	else if(err != MMSYSERR_INVALPARAM)
	{
		DJ_TRACE("mid_set_volume_right(): mid_lock_score failed: %d, %s\n", err, mid_format_error(err));
		return err;
	}

	err = midiOutGetVolume((HMIDIOUT)stream, (LPDWORD)&old);
	if(err == MMSYSERR_NOERROR)
	{
		vol = MAKELONG(LOWORD(old), LOWORD(level));
		err = midiOutSetVolume((HMIDIOUT)stream, vol);
	}
	else
	{
		DJ_TRACE("mid_set_volume_right(): err setting right volume: %d, %s\n", err, mid_format_error(err));
	}

	err = mid_unlock_score(h);
	if(err == MMSYSERR_INVALPARAM)
	{
		DJ_TRACE("mid_set_volume_right(): mid_unlock_score failed: %d, %s\n", err, mid_format_error(err));
		return MMSYSERR_NOERROR;
	}

	return err;
}

DJ_RESULT mid_set_volume(DJ_HANDLE h, unsigned int level)
{
	unsigned int err = MMSYSERR_NOERROR;

	err = mid_set_volume_left(h, level);
	if(err == MMSYSERR_NOERROR)
		err = mid_set_volume_right(h, level);

	return err;
}

DJ_RESULT mid_get_volume_left(DJ_HANDLE h, unsigned int* level)
{
	struct mid_player* p = (struct mid_player*)h;
	unsigned int err = MMSYSERR_NOERROR;
	HMIDISTRM stream = (HMIDISTRM)MIDI_MAPPER;

	err = mid_lock_score(h);
	if(err == MMSYSERR_NOERROR)
	{
		stream = p->stream;
	}
	else if(err != MMSYSERR_INVALPARAM)
	{
		DJ_TRACE("mid_get_volume_left(): mid_lock_score failed: %d, %s\n", err, mid_format_error(err));
		return err;
	}

	err = midiOutGetVolume((HMIDIOUT)stream, (LPDWORD)level);
	if(err == MMSYSERR_NOERROR)
	{
		*level = LOWORD(*level);
	}
	else
	{
		DJ_TRACE("mid_get_volume_left(): err getting right volume: %d, %s\n", err, mid_format_error(err));
	}

	err = mid_unlock_score(h);
	if(err == MMSYSERR_INVALPARAM)
	{
		DJ_TRACE("mid_get_volume_left(): mid_unlock_score failed: %d, %s\n", err, mid_format_error(err));
		return MMSYSERR_NOERROR;
	}

	return err;
}

DJ_RESULT mid_get_volume_right(DJ_HANDLE h, unsigned int* level)
{
	struct mid_player* p = (struct mid_player*)h;
	unsigned int err = MMSYSERR_NOERROR;
	HMIDISTRM stream = (HMIDISTRM)MIDI_MAPPER;

	err = mid_lock_score(h);
	if(err == MMSYSERR_NOERROR)
	{
		stream = p->stream;
	}
	else if(err != MMSYSERR_INVALPARAM)
	{
		DJ_TRACE("mid_get_volume_right(): mid_lock_score failed: %d, %s\n", err, mid_format_error(err));
		return err;
	}

	err = midiOutGetVolume((HMIDIOUT)stream, (LPDWORD)level);
	if(err == MMSYSERR_NOERROR)
	{
		*level = HIWORD(*level);
	}
	else
	{
		DJ_TRACE("mid_get_volume_right(): err getting right volume: %d, %s\n", err, mid_format_error(err));
	}

	err = mid_unlock_score(h);
	if(err == MMSYSERR_INVALPARAM)
	{
		DJ_TRACE("mid_get_volume_right(): mid_unlock_score failed: %d, %s\n", err, mid_format_error(err));
		return MMSYSERR_NOERROR;
	}

	return err;
}

DJ_RESULT mid_get_volume(DJ_HANDLE h, unsigned int* level)
{
	return mid_get_volume_left(h, level);
}

DJ_RESULT mid_set_looping(DJ_HANDLE h, boolean looping)
{
	struct mid_player* p = (struct mid_player*)h;
	unsigned int err = MMSYSERR_NOERROR;

	err = mid_lock_score(h);
	if(err != MMSYSERR_NOERROR)
	{
		DJ_TRACE("mid_set_looping(): mid_lock_score failed: %d, %s\n", err, mid_format_error(err));
		return err;
	}

	p->looping = looping;

	err = mid_unlock_score(h);
	if(err != MMSYSERR_NOERROR)
	{
		DJ_TRACE("mid_set_looping(): mid_unlock_score failed: %d, %s\n", err, mid_format_error(err));
		return err;
	}

	return MMSYSERR_NOERROR;
}

boolean mid_is_looping(DJ_HANDLE h)
{
	struct mid_player* p = (struct mid_player*)h;
	unsigned int err = MMSYSERR_NOERROR;
	boolean looping;

	err = mid_lock_score(h);
	if(err != MMSYSERR_NOERROR)
	{
		DJ_TRACE("mid_get_looping(): mid_lock_score failed: %d, %s\n", err, mid_format_error(err));
		return err;
	}

	looping = p->looping;

	err = mid_unlock_score(h);
	if(err != MMSYSERR_NOERROR)
	{
		DJ_TRACE("mid_get_looping(): mid_unlock_score failed: %d, %s\n", err, mid_format_error(err));
		return err;
	}

	return looping;
}

boolean mid_is_playing(DJ_HANDLE h)
{
	struct mid_player* p = (struct mid_player*)h;
	unsigned int err = MMSYSERR_NOERROR;
	boolean ret = false;

	err = mid_lock_score(h);
	if(err != MMSYSERR_NOERROR)
		return false;

	ret = (p->state == STATE_PLAYING);

	mid_unlock_score(h);

	return ret;
}

boolean mid_is_paused(DJ_HANDLE h)
{
	struct mid_player* p = (struct mid_player*)h;
	unsigned int err = MMSYSERR_NOERROR;
	boolean ret = false;

	err = mid_lock_score(h);
	if(err != MMSYSERR_NOERROR)
		return false;

	ret = (p->state == STATE_PAUSED);

	mid_unlock_score(h);
	
	return ret;
}

boolean mid_is_stopped(DJ_HANDLE h)
{
	struct mid_player* p = (struct mid_player*)h;
	unsigned int err = MMSYSERR_NOERROR;
	boolean ret = false;

	err = mid_lock_score(h);
	if(err != MMSYSERR_NOERROR)
		return false;

	ret = (p->state == STATE_STOPPED);

	mid_unlock_score(h);

	return ret;
}

static DJ_RESULT mid_lock_score(DJ_HANDLE h)
{
	struct mid_player* p = (struct mid_player*)h;
	unsigned int err = MMSYSERR_NOERROR;

	err = WaitForSingleObject(players_mutex, INFINITE);
	if(err == WAIT_FAILED)
	{
		DJ_TRACE("mid_lock_score(): WaitForSingleObject failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		return MMSYSERR_ERROR;
	}

	if(mid_is_handle_valid(h) == false)
	{
		err = ReleaseMutex(players_mutex);
		if(err == 0)
		{
			DJ_TRACE("mid_lock_score(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		}

		return MMSYSERR_INVALPARAM;
	}

	err = WaitForSingleObject(p->mutex, INFINITE);
	if(err == WAIT_FAILED)
	{
		DJ_TRACE("mid_lock_score(): WaitForSingleObject failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		err = ReleaseMutex(players_mutex);
		if(err == 0)
		{
			DJ_TRACE("mid_lock_score(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		}

		return MMSYSERR_ERROR;
	}

	err = ReleaseMutex(players_mutex);
	if(err == 0)
	{
		DJ_TRACE("mid_lock_score(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		err = ReleaseMutex(p->mutex);
		if(err == 0)
		{
			DJ_TRACE("mid_lock_score(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		}

		return MMSYSERR_ERROR;
	}

	return MMSYSERR_NOERROR;
}

static DJ_RESULT mid_unlock_score(DJ_HANDLE h)
{
	struct mid_player* p = (struct mid_player*)h;
	unsigned int err = MMSYSERR_NOERROR;

	err = WaitForSingleObject(players_mutex, INFINITE);
	if(err == WAIT_FAILED)
	{
		DJ_TRACE("mid_unlock_score(): WaitForSingleObject failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		return MMSYSERR_ERROR;
	}

	if(mid_is_handle_valid(h) == false)
	{
		err = ReleaseMutex(players_mutex);
		if(err == 0)
		{
			DJ_TRACE("mid_unlock_score(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		}

		return MMSYSERR_INVALPARAM;
	}

	err = ReleaseMutex(players_mutex);
	if(err == 0)
	{
		DJ_TRACE("mid_unlock_score(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		return MMSYSERR_ERROR;
	}

	err = ReleaseMutex(p->mutex);
	if(err == 0)
	{
		DJ_TRACE("mid_unlock_score(): ReleaseMutex failed: %lu, %s\n", GetLastError(), DJ_FORMAT_MESSAGE(GetLastError()));
		return MMSYSERR_ERROR;
	}

	return MMSYSERR_NOERROR;
}

static void mid_rewind(struct mid_score* s)
{
	unsigned char* tmp = NULL;
	unsigned int i;

	if(s != NULL)
	{
		s->curr_time = 0;

		tmp = s->raw_bytes + sizeof(struct _mid_header);

		for(i = 0; i < s->num_tracks; i++)
		{
			s->tracks[i].track = (struct _mid_track*)tmp;
			s->tracks[i].buf = tmp + sizeof(struct _mid_track);
			s->tracks[i].absolute_time = 0;
			s->tracks[i].last_event = 0;

			tmp += sizeof(struct _mid_track) + swap_bytes_long(s->tracks[i].track->length);
		}

	}
}

static struct evt mid_get_next_event(const struct trk* track)
{
	unsigned char* buf;
	struct evt e;
	unsigned int bytesread;
	unsigned int time;

	buf = track->buf;

	time = read_var_long(buf, &bytesread);
	buf += bytesread;

	e.absolute_time = track->absolute_time + time;
	e.data = buf;
	e.event = *e.data;

	return e;
}

/**
 * @brief This function determines if the provided @ref evt parameter is the
 * end-of-track event.
 *
 * This function determines if the provided @ref evt parameter is the end-of-
 * track event.
 *
 * @param e		A pointer to the @ref evt to test.
 * @return		Returns non-zero if the @ref evt is the end-of-track event,
 * 				zero otherwise.
 */
static unsigned int mid_is_track_end(const struct evt* e)
{
	if(e->event == 0xff) // meta-event?
		if(*(e->data + 1) == 0x2f) // track end?
			return 1;

	return 0;
}

/**
 * @brief This function buffers the next chunk of MIDI messages from the score.
 *
 * This function processes the next chunk of MIDI messages and puts them into
 * the supplied buffer. The buffer will be a valid buffer for use with a
 * <a href="http://msdn.microsoft.com/en-us/library/windows/desktop/dd798449%28v=vs.85%29.aspx">MIDIHDR</a>
 * structure.
 *
 * @param s			A pointer to the @ref mid_score currently playing.
 * @param out		A pointer to a buffer to receive the buffered messages.
 * @param outlen	A pointer to a buffer to receive the length of the buffered
 * 					messages.
 *
 * @return Returns non-zero on success, zero on failure
 */
static unsigned int mid_get_streambuf(struct mid_score* s, unsigned int* out, unsigned int* outlen)
{
	MIDIEVENT e, *p;
	unsigned int streamlen = 0;
	unsigned int i;

	if(s == NULL || out == NULL || outlen == NULL)
		return 0;

	*outlen = 0;

	while(true)
	{
		unsigned int time = UINT_MAX;
		unsigned int idx = -1;
		struct evt evt;
		unsigned char c;

		if(((streamlen + 3) * sizeof(unsigned int)) >= MAX_BUFFER_SIZE)
			break;

		// get the next event
		for(i = 0; i < s->num_tracks; i++)
		{
			evt = mid_get_next_event(&s->tracks[i]);
			if(!(mid_is_track_end(&evt)) && (evt.absolute_time < time))
			{
				time = evt.absolute_time;
				idx = i;
			}
		}

		// if idx == -1 then all the tracks have been read up to the end of track mark
		if(idx == -1)
			break; // we're done

		e.dwStreamID = 0; // always 0

		evt = mid_get_next_event(&s->tracks[idx]);

		s->tracks[idx].absolute_time = evt.absolute_time;
		e.dwDeltaTime = s->tracks[idx].absolute_time - s->curr_time;
		s->curr_time = s->tracks[idx].absolute_time;

		if(!(evt.event & 0x80)) // running mode
		{
			unsigned char last = s->tracks[idx].last_event;
			c = *evt.data++; // get the first data byte
			e.dwEvent = ((unsigned long)MEVT_SHORTMSG << 24) |
						((unsigned long)last) |
						((unsigned long)c << 8);
			if(!((last & 0xc0) == 0xc0 || (last & 0xd0) == 0xd0))
			{
				c = *evt.data++; // get the second data byte
				e.dwEvent |= ((unsigned long)c << 16);
			}

			p = (MIDIEVENT*)&out[streamlen];
			*p = e;

			streamlen += 3;

			s->tracks[idx].buf = evt.data;
		}
		else if(evt.event == 0xff) // meta-event
		{
			evt.data++; // skip the event byte
			unsigned char meta = *evt.data++; // read the meta-event byte
			unsigned int len;

			switch(meta)
			{
			case 0x51: // only care about tempo events
				{
					unsigned char a, b, c;
					len = *evt.data++; // get the length byte, should be 3
					a = *evt.data++;
					b = *evt.data++;
					c = *evt.data++;

					e.dwEvent = ((unsigned long)MEVT_TEMPO << 24) |
								((unsigned long)a << 16) |
								((unsigned long)b << 8) |
								((unsigned long)c << 0);

					p = (MIDIEVENT*)&out[streamlen];
					*p = e;

					streamlen += 3;
				}
				break;
			default: // skip all other meta events
				len = *evt.data++; // get the length byte
				evt.data += len;
				break;
			}

			s->tracks[idx].buf = evt.data;
		}
		else if((evt.event & 0xf0) != 0xf0) // normal command
		{
			s->tracks[idx].last_event = evt.event;
			evt.data++; // skip the event byte
			c = *evt.data++; // get the first data byte
			e.dwEvent = ((unsigned long)MEVT_SHORTMSG << 24) |
						((unsigned long)evt.event << 0) |
						((unsigned long)c << 8);
			if(!((evt.event & 0xc0) == 0xc0 || (evt.event & 0xd0) == 0xd0))
			{
				c = *evt.data++; // get the second data byte
				e.dwEvent |= ((unsigned long)c << 16);
			}

			p = (MIDIEVENT*)&out[streamlen];
			*p = e;

			streamlen += 3;

			s->tracks[idx].buf = evt.data;
		}

	}

	*outlen = streamlen * sizeof(unsigned int);

	return 1;
}

static unsigned char* mid_format_error(unsigned int err)
{
	switch(err)
	{
	case MMSYSERR_NOERROR: return (unsigned char*)"MMSYSERR_NOERROR";
	case MMSYSERR_ERROR: return (unsigned char*)"MMSYSERR_ERROR";
	case MMSYSERR_BADDEVICEID: return (unsigned char*)"MMSYSERR_BADDEVICEID";
	case MMSYSERR_NOTENABLED: return (unsigned char*)"MMSYSERR_NOTENABLED";
	case MMSYSERR_ALLOCATED: return (unsigned char*)"MMSYSERR_ALLOCATED";
	case MMSYSERR_INVALHANDLE: return (unsigned char*)"MMSYSERR_INVALHANDLE";
	case MMSYSERR_NODRIVER: return (unsigned char*)"MMSYSERR_NODRIVER";
	case MMSYSERR_NOMEM: return (unsigned char*)"MMSYSERR_NOMEM";
	case MMSYSERR_NOTSUPPORTED: return (unsigned char*)"MMSYSERR_NOTSUPPORTED";
	case MMSYSERR_BADERRNUM: return (unsigned char*)"MMSYSERR_BADERRNUM";
	case MMSYSERR_INVALFLAG: return (unsigned char*)"MMSYSERR_INVALFLAG";
	case MMSYSERR_INVALPARAM: return (unsigned char*)"MMSYSERR_INVALPARAM";
	case MMSYSERR_HANDLEBUSY: return (unsigned char*)"MMSYSERR_HANDLEBUSY";
	case MMSYSERR_INVALIDALIAS: return (unsigned char*)"MMSYSERR_INVALIDALIAS";
	case MMSYSERR_BADDB: return (unsigned char*)"MMSYSERR_BADDB";
	case MMSYSERR_KEYNOTFOUND: return (unsigned char*)"MMSYSERR_KEYNOTFOUND";
	case MMSYSERR_READERROR: return (unsigned char*)"MMSYSERR_READERROR";
	case MMSYSERR_WRITEERROR: return (unsigned char*)"MMSYSERR_WRITEERROR";
	case MMSYSERR_DELETEERROR: return (unsigned char*)"MMSYSERR_DELETEERROR";
	case MMSYSERR_VALNOTFOUND: return (unsigned char*)"MMSYSERR_VALNOTFOUND";
	case MMSYSERR_NODRIVERCB: return (unsigned char*)"MMSYSERR_NODRIVERCB";
	case MIDIERR_UNPREPARED: return (unsigned char*)"MMSYSERR_NODRIVERCB";
	case MIDIERR_STILLPLAYING: return (unsigned char*)"MMSYSERR_NODRIVERCB";
	case MIDIERR_NOMAP: return (unsigned char*)"MMSYSERR_NODRIVERCB";
	case MIDIERR_NOTREADY: return (unsigned char*)"MMSYSERR_NODRIVERCB";
	case MIDIERR_NODEVICE: return (unsigned char*)"MMSYSERR_NODRIVERCB";
	case MIDIERR_INVALIDSETUP: return (unsigned char*)"MMSYSERR_NODRIVERCB";
	case MIDIERR_BADOPENMODE: return (unsigned char*)"MMSYSERR_NODRIVERCB";
	case MIDIERR_DONT_CONTINUE: return (unsigned char*)"MMSYSERR_NODRIVERCB";
	default: return (unsigned char*)"Unknown MIDI error";
	}
}

#define MID_HDR_SIZE 6

/**
 * @brief This function verifies the MIDI header at the beginning of a data
 * buffer.
 *
 * This function takes a data buffer and a length and verifies that it begins
 * with a valid MIDI header.
 *
 * @param buf	A pointer to the buffer containing the MIDI data.
 * @param len	The length of the buffer.
 * @return	Returns non-zero if the buffer begins with a valid MIDI header,
 * 			zero otherwise.
 */
static unsigned int is_mid_header(unsigned char* buf, unsigned int len)
{
	struct _mid_header* hdr;

	if(len < sizeof(struct _mid_header))
		return 0;

	hdr = (struct _mid_header*)buf;
	if(hdr->id != MID_ID)
		return 0;

	if(swap_bytes_long(hdr->size) != MID_HDR_SIZE)
		return 0;

	return 1;
}

#ifdef MID_PLAYER_STANDALONE

unsigned char* load_file(unsigned char* filename, unsigned int* len);

void mid_callback(unsigned int val)
{
	printf("\r       \rStopped");
}

const unsigned int vol_tick = 3277;

int main(int argc, char* argv[])
{
	unsigned char* filename;
	unsigned char* midbuf = NULL;
	unsigned int midbuflen = 0;
	unsigned char c;
	MIDIOUTCAPS     caps;

	unsigned long n, i;
	unsigned int err;

	DJ_HANDLE score;

	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stdin, NULL, _IONBF, 0);

	if(argc > 1)
		filename = (unsigned char*)argv[1];
	else
	{
		printf("Usage: %s <filename>\n", argv[0]);
		return 0;
	}

	n = midiOutGetNumDevs();

	for (i = 0; i < n; i++)
	{
	    if (!midiOutGetDevCaps(i, &caps, sizeof(MIDIOUTCAPS)))
	    {
	        printf("Device %lu: %s\r\n", i, caps.szPname);
	        if(caps.dwSupport & MIDICAPS_CACHE)
	        {
	        	printf(" - supports patch caching.\n");
	        }
	        if(caps.dwSupport & MIDICAPS_VOLUME)
	        {
	        	printf(" - supports volume control.\n");
	        }
	        if(caps.dwSupport & MIDICAPS_LRVOLUME)
	        {
	        	printf(" - supports separate left and right volume control.\n");
	        }
	        if(caps.dwSupport & MIDICAPS_STREAM)
	        {
	        	printf(" - provides direct support for the midiStreamOut function.\n");
	        }
	        printf("\n");
	    }
	}

	midbuf = load_file(filename, &midbuflen);
	if(midbuf == NULL)
	{
		fprintf(stderr, "Failed to load file %s\n", filename);
		return 0;
	}

	mid_init();

	score = mid_score_open(midbuf, midbuflen);
	if(score == NULL)
	{
		fprintf(stderr, "Failed to open file %s\n", filename);
		goto error;
	}

	printf("Loaded %s\n", filename);

	printf("\n(p) play/pause, (s) stop, (l) loop on/off, (q) quit, (+/-) volume up/down\n");
	printf("\r       \rStopped");
	err = 0;
//	while((c = getc(stdin)) != 'q') // getch doesn't work well in eclipse debugger
	while((c = getch()) != 'q')
	{
		unsigned int vol;

		switch(c)
		{
		case 'l':
		case 'L':
			mid_set_looping(score, !mid_get_looping(score));
			break;
		case 'p':
		case 'P':
			if(mid_is_stopped(score))
			{
				err = mid_play(score);
				if(err != MMSYSERR_NOERROR)
				{
					fprintf(stderr, "Error playing file %s, error %d\n", filename, err);
					goto error;
				}
				printf("\r       \rPlaying");
			}
			else if(mid_is_playing(score))
			{
				err = mid_pause(score);
				if(err != MMSYSERR_NOERROR)
				{
					fprintf(stderr, "Error pausing file %s, error %d\n", filename, err);
					goto error;
				}
				printf("\r       \rPaused");
			}
			else if(mid_is_paused(score))
			{
				err = mid_resume(score);
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
			err = mid_stop(score);
			if(err != MMSYSERR_NOERROR)
			{
				fprintf(stderr, "Error stopping file %s, error %d\n", filename, err);
				goto error;
			}
			printf("\r       \rStopped");
			break;
		case '-':
		case '_':
			err = mid_get_volume(score, &vol);
			if(err != MMSYSERR_NOERROR)
			{
				fprintf(stderr, "Error adjusting volume, error %d\n", err);
				goto error;
			}
			if(vol <= vol_tick)
				vol = 0;
			else
				vol -= vol_tick;
			err = mid_set_volume(score, vol);
			if(err != MMSYSERR_NOERROR)
			{
				fprintf(stderr, "Error adjusting volume, error %d\n", err);
				goto error;
			}
			break;
		case '=':
		case '+':
			err = mid_get_volume(score, &vol);
			if(err != MMSYSERR_NOERROR)
			{
				fprintf(stderr, "Error adjusting volume, error %d\n", err);
				goto error;
			}
			if(0xffff - vol <= vol_tick)
				vol = 0xffff;
			else
				vol += vol_tick;
			err = mid_set_volume(score, vol);
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

	mid_score_close(score);
	mid_shutdown();
	free(midbuf);

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

#ifndef LONG_MASK
#define LONG_MASK 0x7f
#endif

#ifndef LONG_MORE_BIT
#define LONG_MORE_BIT 0x80
#endif

unsigned long read_var_long(unsigned char* buf, unsigned int* inc)
{
	unsigned long time = 0;
	unsigned char c;

	*inc = 0;


	do
	{
		c = buf[(*inc)++];
		time = (time * 128) + (c & LONG_MASK);
	}
	while(c & LONG_MORE_BIT);

	return time;
}

unsigned short swap_bytes_short(unsigned short in)
{
	return ((in << 8) | (in >> 8));
}

unsigned long swap_bytes_long(unsigned long in)
{
	unsigned short *p;
	p = (unsigned short*)&in;

	return (  (((unsigned long)swap_bytes_short(p[0])) << 16) |
				(unsigned long)swap_bytes_short(p[1]));
}

#endif
