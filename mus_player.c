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
 * mus_player.c
 *
 *  Created on: Dec 3, 2011
 *      Author: David J. Rager
 */
#include <stdio.h>
#include <conio.h>

#include "mus_player.h"

#define STATE_ERROR		0
#define STATE_STARTING	1
#define STATE_PLAYING	2
#define STATE_PAUSED	3
#define STATE_STOPPING	4
#define STATE_STOPPED	5
#define STATE_SHUTDOWN	6

#define MID_EVENT_RELEASE			0 // Release note
#define MID_EVENT_PLAY				1 // Play note
#define MID_EVENT_CONTROLLER_CHANGE	3 // Control Change
#define MID_EVENT_PATCH_CHANGE		4 // Program (patch) change
#define MID_EVENT_PITCH_CHANGE		6 // Pitch wheel change

#define VOL_UP 0
#define VOL_DOWN 1

struct _mus_header {
	unsigned int	id;				// identifier "MUS" 0x1A
	unsigned short	score_len;
	unsigned short	score_start;	// file position
	unsigned short	channels;		// count of primary channels
	unsigned short	sec_channels;	// count of secondary channels
	unsigned short	instr_cnt;
	unsigned short	dummy;

	unsigned short instruments[1];
};

static unsigned int is_mus_header(unsigned char* buf, unsigned int len);
static const unsigned int MUS_ID = '\x1ASUM';

#ifndef MUS_PLAYER_STANDALONE

//#include "dj_debug.h"
#include "djmm_utils.h"

#else

static unsigned long read_var_long(unsigned char* buf, unsigned int* inc);

#endif

struct mus_player {
	HANDLE event;
	HANDLE thread;
	HANDLE ready;
	HANDLE mutex;

	HMIDISTRM stream;
	MIDIHDR header[2]; // double buffer

	unsigned int device;

	unsigned int state;
	unsigned int looping;

	unsigned int timebase;

	struct mus_score* score;
	mus_notify_cb cb;

	struct mus_player* next;
};

struct mus_score {
	unsigned char volume;
	unsigned int ticks;

	unsigned char* ptr;

	unsigned char* raw_bytes;
	unsigned int raw_len;
};

#define MAX_BUFFER_SIZE (1024 * 12)

struct mus_player_event {
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

static unsigned int mus_get_streambuf(struct mus_score* m, unsigned int* out, unsigned int* outlen);
static void mus_player_shutdown(struct mus_player* p);
static void mus_close_stream(struct mus_player* p);
static void mus_rewind(struct mus_score* m);

static void CALLBACK mus_callback_proc(HMIDIOUT hmo, UINT wMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
	struct mus_player* p = (struct mus_player*)dwInstance;
	if (p == NULL)
		return;

	switch (wMsg) {
	case MOM_POSITIONCB:
		break;
	case MOM_DONE:
		// The last event in the queued buffer has played. Signal the
		// player thread to continue.
		SetEvent(p->event);
		break;
	case MOM_OPEN:
		break;
	case MOM_CLOSE:
		break;
	}

}

static DWORD WINAPI mus_player_proc(LPVOID lpParameter) {
	struct mus_player* p = (struct mus_player*)lpParameter;
	unsigned int err = MMSYSERR_NOERROR;

	unsigned int idx = 0;

	while ((p->state != STATE_SHUTDOWN) && (p->state != STATE_ERROR)) {
		WaitForSingleObject(p->event, INFINITE);

		switch (p->state) {
		case STATE_STARTING:
			WaitForSingleObject(p->mutex, INFINITE);
			p->state = STATE_PLAYING;
			ReleaseMutex(p->mutex);

			break;
		case STATE_PLAYING:
			WaitForSingleObject(p->mutex, INFINITE);

			mus_get_streambuf(p->score, (unsigned int*)p->header[idx].lpData, (unsigned int*)&p->header[idx].dwBufferLength);
			p->header[idx].dwBytesRecorded = p->header[idx].dwBufferLength;

			if (p->header[idx].dwBufferLength > 0) {
				p->header[idx].dwFlags &= ~WHDR_DONE; // clear the done flag to reuse the buffer
				midiStreamOut(p->stream, &p->header[idx], sizeof(MIDIHDR));
				idx = (idx + 1) % 2;
			} else {
				if (p->looping) {
					mus_rewind(p->score);
					mus_get_streambuf(p->score, (unsigned int*)p->header[idx].lpData, (unsigned int*)&p->header[idx].dwBufferLength);
					p->header[idx].dwBytesRecorded = p->header[idx].dwBufferLength;
					p->header[idx].dwFlags &= ~MHDR_DONE;
					midiStreamOut(p->stream, &p->header[idx], sizeof(MIDIHDR));
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

			mus_close_stream(p);
			mus_rewind(p->score);

			SetEvent(p->event);
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

static HANDLE players_mutex = NULL;
static struct mus_player* players = NULL; // player handle list.

/*!
 * Initialize the MUS subsystem.
 *
 * This function initializes the global player handle list and associated
 * mutex.
 *
 * This function does not acquire a lock.
 *
 * @return Returns MMSYSERR_NOERROR if successful, MMSYSERR_ERROR if the mutex
 *  could not be initialized.
 */
MMRESULT mus_init() {
	players = NULL;

	players_mutex = CreateMutex(NULL, FALSE, NULL);
	if (players_mutex == NULL)
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
void mus_shutdown() {
	struct mus_player* tmp = NULL;
	WaitForSingleObject(players_mutex, INFINITE);
	tmp = players;
	while (tmp != NULL) {
		ReleaseMutex(players_mutex);

		// This function acquires players_mutex and tmp->mutex so make sure
		// they are not held here.
		mus_score_close(tmp);

		WaitForSingleObject(players_mutex, INFINITE);
		tmp = players;
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
static struct mus_player* mus_player_init(mus_notify_cb callback) {
	struct mus_player* p = (struct mus_player*)malloc(sizeof(struct mus_player));
	if (p == NULL)
		return NULL;

	// Initialize the double buffers.
	ZeroMemory(&p->header[0], sizeof(MIDIHDR));
	p->header[0].lpData = (char*)malloc(MAX_BUFFER_SIZE);
	if (p->header[0].lpData == NULL)
		goto error1;

	p->header[0].dwBufferLength = p->header[0].dwBytesRecorded = MAX_BUFFER_SIZE;

	ZeroMemory(&p->header[1], sizeof(MIDIHDR));
	p->header[1].lpData = (char*)malloc(MAX_BUFFER_SIZE);
	if (p->header[1].lpData == NULL)
		goto error2;

	p->header[1].dwBufferLength = p->header[1].dwBytesRecorded = MAX_BUFFER_SIZE;

	p->device = 0;
	p->score = NULL;
	p->state = STATE_STOPPED;
	p->timebase = 70;
	p->looping = 0;
	p->stream = 0;
	p->cb = callback;

	// Create the internal mutex.
	p->mutex = CreateMutex(NULL, FALSE, NULL);
	if (p->mutex == NULL) {
		fprintf(stderr, "CreateMutex failed %lu\n", GetLastError());
		goto error3;
	}

	// Create the event object that is signaled by the MIDI api and state
	// changes.
	p->event = CreateEvent(0, FALSE, TRUE, 0);
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
	p->thread = CreateThread(NULL, 0, mus_player_proc, p, 0, NULL);
	if (p->thread == NULL) {
		fprintf(stderr, "CreateThread failed %lu\n", GetLastError());
		goto error6;
	}

	// Make sure the thread is spun up and ready.
	WaitForSingleObject(p->ready, INFINITE);
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
static void mus_player_shutdown(struct mus_player* p) {
	WaitForSingleObject(p->mutex, INFINITE);
	p->state = STATE_SHUTDOWN;
	SetEvent(p->event);
	ReleaseMutex(p->mutex);
	WaitForSingleObject(p->thread, INFINITE);

	CloseHandle(p->event);
	CloseHandle(p->ready);
	CloseHandle(p->mutex);
	CloseHandle(p->thread);
	free(p->header[0].lpData);
	free(p->header[1].lpData);
	free(p);
}

static void mus_add_player(struct mus_player* p) {
	WaitForSingleObject(players_mutex, INFINITE);

	if (players == NULL) {
		players = p;
		p->next = NULL;
	} else {
		struct mus_player* tmp = players;
		while (tmp != p && tmp->next != NULL) {
			tmp = tmp->next;
		}

		if (tmp == p)
			return;

		tmp->next = p;
		p->next = NULL;
	}

	ReleaseMutex(players_mutex);
}

static struct mus_player* mus_remove_player(struct mus_player* p) {
	struct mus_player* tmp = players;

	if (players == p) { // is p the first in the list?
		players = p->next;
		return p;
	}

	while (tmp != NULL && tmp->next != p) {
		tmp = tmp->next;
	}

	if (tmp == NULL) // tmp can only be NULL if we've never added p.
		return NULL;

	tmp->next = p->next;
	p->next = NULL;

	return p;
}
/*!
 * This function prepares a buffer for playing.
 *
 * This function takes a buffer of MUS formatted data and returns a HANDLE that
 * is used in subsequent calls to the mus_* functions.
 *
 * @param buf
 * @param len
 * @return Returns a HANDLE value if successful, NULL otherwise. This HANDLE
 * value must be closed by calling mus_score_close() when finished.
 */
HANDLE mus_score_open(unsigned char* buf, unsigned int len, mus_notify_cb callback) {
	unsigned int err = MMSYSERR_NOERROR;
	struct mus_player* p = NULL;
	struct mus_score* s = NULL;

	if (!is_mus_header(buf, len))
		goto error1;

	p = mus_player_init(callback);
	if (p == NULL)
		goto error1;

	// We have a player, buffers initialized and thread spun up and
	// ready. Prepare the score data for processing.

	s = (struct mus_score*)malloc(sizeof(struct mus_score));
	if (s == NULL)
		goto error2;

	s->raw_bytes = (unsigned char*)malloc(len);
	if (s->raw_bytes == NULL)
		goto error3;
	memcpy(s->raw_bytes, buf, len);
	s->raw_len = len;
	s->volume = 0;
	s->ticks = 0;

	mus_rewind(s);

	p->score = s;

	// Score is loaded and ready. Add the player to our global list
	// and return the "HANDLE" to the user.
	mus_add_player(p);

	return p;

	error3:
	free(s);

	error2:
	mus_player_shutdown(p);
	p = NULL;

	error1:
	return NULL;
}

/*!
 * This function determines if a HANDLE is was opened with mus_score_open().
 *
 * This function determines if a HANDLE is was opened with mus_score_open() and
 * is in the global list. This helper function should be called while holding
 * the players_mutex.
 *
 * This function is called by mus_score_close with players_mutex held.
 * This function is called by mus_play mus_stop with players_mutex held.
 * This function is called by mus_pause with players_mutex held.
 * This function is called by mus_resume with players_mutex held.
 * This function is called by mus_set_volume_left with players_mutex held.
 * This function is called by mus_set_volume_right with players_mutex held.
 * This function is called by mus_volume_left with players_mutex held.
 * This function is called by mus_volume_right with players_mutex held.
 * This function is called by mus_is_playing with players_mutex held.
 * This function is called by mus_is_paused with players_mutex held.
 * This function is called by mus_is_stopped with players_mutex held.
 *
 * @param h
 * @return
 */
static BOOL mus_is_handle_valid(HANDLE h) {
	struct mus_player* p = players;
	if (h == NULL)
		return FALSE;

	while (p != NULL) {
		if (p == h)
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
static void mus_close_stream(struct mus_player* p) {
	unsigned int err;
	p->state = STATE_STOPPED;
	midiOutReset((HMIDIOUT)p->stream);
	err = midiOutUnprepareHeader((HMIDIOUT)p->stream, &p->header[0], sizeof(MIDIHDR));
	if (err != MMSYSERR_NOERROR)
		printf("midiOutUnprepareHeader %d\n", err);
	err = midiOutUnprepareHeader((HMIDIOUT)p->stream, &p->header[1], sizeof(MIDIHDR));
	if (err != MMSYSERR_NOERROR)
		printf("midiOutUnprepareHeader %d\n", err);
	midiStreamClose(p->stream);
	p->stream = 0;
}

void mus_score_close(HANDLE h) {
	struct mus_player* p = NULL;

	WaitForSingleObject(players_mutex, INFINITE);
	if (mus_is_handle_valid(h) == FALSE) {
		ReleaseMutex(players_mutex);
		return;
	}

	p = players;
	if (p == h) // if h is the first in the list
	{
		players = p->next;
		p->next = NULL;
	} else {
		while (p != NULL) {
			if (p->next == h) {
				p->next = p->next->next;
				p = h;
				p->next = NULL;
				break;
			} else
				p = p->next;
		}
	}

	ReleaseMutex(players_mutex);

	// The player has been removed from the global list. Any further calls
	// using this handle will return MMSYSERR_INVALPARAM.

	// Start shutting down the thread. If it's still playing, stop it.
	WaitForSingleObject(p->mutex, INFINITE);
	if (p->state != STATE_STOPPED) {
		ResetEvent(p->ready);
		mus_close_stream(p);
		SetEvent(p->event);
		ReleaseMutex(p->mutex);
		WaitForSingleObject(p->ready, INFINITE);
	} else
		ReleaseMutex(p->mutex);

	// Player thread should be in the STATE_STOPPED state. No existing
	// handles can restart it. Let's close things out.

	if (p->score)
		free(p->score->raw_bytes);

	free(p->score);
	p->score = NULL;

	mus_player_shutdown(p);

	return;
}

MMRESULT mus_play(HANDLE h) {
	struct mus_player* p = (struct mus_player*)h;
	unsigned int err = MMSYSERR_NOERROR;
	MIDIPROPTIMEDIV prop;

	WaitForSingleObject(players_mutex, INFINITE);
	if (mus_is_handle_valid(h) == FALSE) {
		ReleaseMutex(players_mutex);
		return MMSYSERR_INVALPARAM;
	}

	WaitForSingleObject(p->mutex, INFINITE);
	ReleaseMutex(players_mutex);

	if (p->state == STATE_STOPPED) {
		err = midiStreamOpen(&p->stream, &p->device, 1, (DWORD_PTR)mus_callback_proc, (DWORD_PTR)p, CALLBACK_FUNCTION);
		if (err != MMSYSERR_NOERROR) {
			printf("Error opening stream %d\n", err);
			goto error;
		}

		prop.cbStruct = sizeof(MIDIPROPTIMEDIV);
		prop.dwTimeDiv = p->timebase;
		err = midiStreamProperty(p->stream, (LPBYTE)&prop, MIDIPROP_SET | MIDIPROP_TIMEDIV);
		if (err != MMSYSERR_NOERROR) {
			printf("midiStreamProperty %d\n", err);
			goto error;
		}

		mus_get_streambuf(p->score, (unsigned int*)p->header[0].lpData, (unsigned int*)&p->header[0].dwBufferLength);
		p->header[0].dwBytesRecorded = p->header[0].dwBufferLength;
		if(p->header[0].dwBytesRecorded <= 0) {
			printf("MUS buffer is empty\n");
			goto error;
		}

		p->header[0].dwFlags = 0;
		err = midiOutPrepareHeader((HMIDIOUT)p->stream, &p->header[0], sizeof(MIDIHDR));
		if (err != MMSYSERR_NOERROR) {
			printf("midiOutPrepareHeader %d\n", err);
			goto error;
		}

		err = midiStreamOut(p->stream, &p->header[0], sizeof(MIDIHDR)); // Queue the first buffer of midi events
		if (err == MMSYSERR_NOERROR) {
			mus_get_streambuf(p->score, (unsigned int*)p->header[1].lpData, (unsigned int*)&p->header[1].dwBufferLength);
				p->header[1].dwBytesRecorded = p->header[1].dwBufferLength;

			if(p->header[1].dwBytesRecorded > 0) {
				p->header[1].dwFlags = 0;
				err = midiOutPrepareHeader((HMIDIOUT)p->stream, &p->header[1], sizeof(MIDIHDR));
				if (err != MMSYSERR_NOERROR) {
					printf("midiOutPrepareHeader %d\n", err);
					goto error;
				}

				err = midiStreamOut(p->stream, &p->header[1], sizeof(MIDIHDR)); // Queue the second buffer of midi events
				if (err != MMSYSERR_NOERROR) {
					printf("midiStreamOut %d\n", err);
					goto error;
				}
			}

			// midiStreamOpen opens the stream in paused mode so we call restart to begin playing.
			err = midiStreamRestart(p->stream);
			if (err == MMSYSERR_NOERROR) {
				p->state = STATE_STARTING;
				SetEvent(p->event);
			}
		}
	}

error:
	ReleaseMutex(p->mutex);

	return err;
}

MMRESULT mus_stop(HANDLE h) {
	struct mus_player* p = (struct mus_player*)h;
	unsigned int err = MMSYSERR_NOERROR;

	WaitForSingleObject(players_mutex, INFINITE);
	if (mus_is_handle_valid(h) == FALSE) {
		ReleaseMutex(players_mutex);
		return MMSYSERR_INVALPARAM;
	}

	WaitForSingleObject(p->mutex, INFINITE);
	ReleaseMutex(players_mutex);
	if (p->state != STATE_STOPPED) {
		ResetEvent(p->ready);

		mus_close_stream(p);
		mus_rewind(p->score);

		SetEvent(p->event);
		ReleaseMutex(p->mutex);
		WaitForSingleObject(p->ready, INFINITE);
	} else
		ReleaseMutex(p->mutex);

	return MMSYSERR_NOERROR;
}

MMRESULT mus_pause(HANDLE h) {
	struct mus_player* p = (struct mus_player*)h;
	unsigned int err = MMSYSERR_NOERROR;

	WaitForSingleObject(players_mutex, INFINITE);
	if (mus_is_handle_valid(h) == FALSE) {
		ReleaseMutex(players_mutex);
		return MMSYSERR_INVALPARAM;
	}

	WaitForSingleObject(p->mutex, INFINITE);
	ReleaseMutex(players_mutex);
	if (p->state == STATE_PLAYING) {
		err = midiStreamPause(p->stream);
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

MMRESULT mus_resume(HANDLE h) {
	struct mus_player* p = (struct mus_player*)h;
	unsigned int err = MMSYSERR_NOERROR;

	WaitForSingleObject(players_mutex, INFINITE);
	if (mus_is_handle_valid(h) == FALSE) {
		ReleaseMutex(players_mutex);
		return MMSYSERR_INVALPARAM;
	}

	WaitForSingleObject(p->mutex, INFINITE);
	ReleaseMutex(players_mutex);
	if (p->state == STATE_PAUSED) {
		err = midiStreamRestart(p->stream);
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

MMRESULT mus_set_volume_left(HANDLE h, unsigned int level) {
	unsigned int old = 0, vol = 0;
	struct mus_player* p = (struct mus_player*)h;
	unsigned int err = MMSYSERR_NOERROR;
	HMIDISTRM stream = (HMIDISTRM)0;
	BOOL valid = FALSE;

	WaitForSingleObject(players_mutex, INFINITE);
	valid = mus_is_handle_valid(h);
	if (valid == TRUE) {
		stream = p->stream;
		WaitForSingleObject(p->mutex, INFINITE);
	}
	ReleaseMutex(players_mutex);

	err = midiOutGetVolume((HMIDIOUT)stream, (LPDWORD)&old);
	if (err == MMSYSERR_NOERROR) {
		vol = MAKELONG(LOWORD(level), HIWORD(old));
		err = midiOutSetVolume((HMIDIOUT)stream, vol);
	}

	if (valid == TRUE)
		ReleaseMutex(p->mutex);

	return err;
}

MMRESULT mus_set_volume_right(HANDLE h, unsigned int level) {
	unsigned int old = 0, vol = 0;
	struct mus_player* p = (struct mus_player*)h;
	unsigned int err = MMSYSERR_NOERROR;
	HMIDISTRM stream = (HMIDISTRM)0;
	BOOL valid = FALSE;

	WaitForSingleObject(players_mutex, INFINITE);
	valid = mus_is_handle_valid(h);
	if (valid == TRUE) {
		stream = p->stream;
		WaitForSingleObject(p->mutex, INFINITE);
	}
	ReleaseMutex(players_mutex);

	err = midiOutGetVolume((HMIDIOUT)stream, (LPDWORD)&old);
	if (err == MMSYSERR_NOERROR) {
		vol = MAKELONG(LOWORD(old), LOWORD(level));
		err = midiOutSetVolume((HMIDIOUT)stream, vol);
	}

	if (valid == TRUE)
		ReleaseMutex(p->mutex);

	return err;
}

MMRESULT mus_set_volume(HANDLE h, unsigned int level) {
	unsigned int err = MMSYSERR_NOERROR;

	err = mus_set_volume_left(h, level);
	if (err == MMSYSERR_NOERROR)
		err = mus_set_volume_right(h, level);

	return err;
}

MMRESULT mus_volume_left(HANDLE h, unsigned int dir) {
	unsigned int old = 0, vol = 0;
	const unsigned int val = 3277;
	struct mus_player* p = (struct mus_player*)h;
	unsigned int err = MMSYSERR_NOERROR;
	HMIDISTRM stream = (HMIDISTRM)MIDI_MAPPER;
	BOOL valid = FALSE;

	WaitForSingleObject(players_mutex, INFINITE);
	valid = mus_is_handle_valid(h);
	if (valid == TRUE) {
		stream = p->stream;
		WaitForSingleObject(p->mutex, INFINITE);
	}
	ReleaseMutex(players_mutex);

	err = midiOutGetVolume((HMIDIOUT)stream, (LPDWORD)&old);
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
		err = midiOutSetVolume((HMIDIOUT)stream, vol);
	}

	if (valid == TRUE)
		ReleaseMutex(p->mutex);

	return err;
}

MMRESULT mus_volume_right(HANDLE h, unsigned int dir) {
	unsigned int old = 0, vol = 0;
	const unsigned int val = 3277;
	struct mus_player* p = (struct mus_player*)h;
	unsigned int err = MMSYSERR_NOERROR;
	HMIDISTRM stream = (HMIDISTRM)MIDI_MAPPER;
	BOOL valid = FALSE;

	WaitForSingleObject(players_mutex, INFINITE);
	valid = mus_is_handle_valid(h);
	if (valid == TRUE) {
		stream = p->stream;
		WaitForSingleObject(p->mutex, INFINITE);
	}
	ReleaseMutex(players_mutex);

	err = midiOutGetVolume((HMIDIOUT)stream, (LPDWORD)&old);
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
		err = midiOutSetVolume((HMIDIOUT)stream, vol);
	}

	if (valid == TRUE)
		ReleaseMutex(p->mutex);

	return err;
}

MMRESULT mus_volume(HANDLE h, unsigned int dir) {
	unsigned int err;

	err = mus_volume_left(h, dir);
	if (err == MMSYSERR_NOERROR)
		err = mus_volume_right(h, dir);

	return err;
}

MMRESULT mus_set_looping(HANDLE h, BOOL looping) {
	struct mus_player* p = (struct mus_player*)h;
	unsigned int err = MMSYSERR_NOERROR;

	WaitForSingleObject(players_mutex, INFINITE);
	if (mus_is_handle_valid(h) == FALSE) {
		ReleaseMutex(players_mutex);
		return MMSYSERR_INVALPARAM;
	}

	WaitForSingleObject(p->mutex, INFINITE);
	ReleaseMutex(players_mutex);

	p->looping = looping;

	ReleaseMutex(p->mutex);

	return MMSYSERR_NOERROR;
}

BOOL mus_get_looping(HANDLE h) {
	struct mus_player* p = (struct mus_player*)h;
	unsigned int err = MMSYSERR_NOERROR;
	BOOL looping;

	WaitForSingleObject(players_mutex, INFINITE);
	if (mus_is_handle_valid(h) == FALSE) {
		ReleaseMutex(players_mutex);
		return FALSE;
	}

	WaitForSingleObject(p->mutex, INFINITE);
	ReleaseMutex(players_mutex);

	looping = p->looping;

	ReleaseMutex(p->mutex);

	return looping;
}

BOOL mus_is_playing(HANDLE h) {
	struct mus_player* p = (struct mus_player*)h;
	BOOL ret = FALSE;

	WaitForSingleObject(players_mutex, INFINITE);
	if (mus_is_handle_valid(h) == FALSE)
		ret = FALSE;
	else
		ret = (p->state == STATE_PLAYING);
	ReleaseMutex(players_mutex);

	return ret;
}

BOOL mus_is_paused(HANDLE h) {
	struct mus_player* p = (struct mus_player*)h;
	BOOL ret = FALSE;

	WaitForSingleObject(players_mutex, INFINITE);
	if (mus_is_handle_valid(h) == FALSE)
		ret = FALSE;
	else
		ret = (p->state == STATE_PAUSED);
	ReleaseMutex(players_mutex);

	return ret;
}

BOOL mus_is_stopped(HANDLE h) {
	struct mus_player* p = (struct mus_player*)h;
	BOOL ret = FALSE;

	WaitForSingleObject(players_mutex, INFINITE);
	if (mus_is_handle_valid(h) == FALSE)
		ret = FALSE;
	else
		ret = (p->state == STATE_STOPPED);
	ReleaseMutex(players_mutex);

	return ret;
}

static void mus_rewind(struct mus_score* m) {
	struct _mus_header* hdr = NULL;
	if (m != NULL) {
		m->volume = 0;
		m->ticks = 0;

		hdr = (struct _mus_header*)m->raw_bytes;
		m->ptr = m->raw_bytes + hdr->score_start;
	}
}

static unsigned int mus2mid_controller_map[128] = {0, 0, 1, 7, 10, 11, 91, 93, 64, 67, 120, 123, 126, 127, 121};
static unsigned int mus2mid_channel_map[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 9};
static unsigned int mus_velocity_map[16] = {64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64};

static struct mus_player_event mus_get_next_event(const struct mus_score* m) {
	struct mus_player_event e;

	e.ptr = m->ptr;
	e.byte = *e.ptr++;

	return e;
}

static unsigned int mus_pack_event(unsigned char command, unsigned char channel, unsigned char a, unsigned char b) {
	unsigned int event;

	union {
		unsigned char byte;
		struct {
			unsigned char channel:4;
			unsigned char command:3;
			unsigned char one:1;		// always 1
		};
	} mevt;

	mevt.channel = channel;
	mevt.command = command;
	mevt.one = 1;

	event = ((unsigned long)MEVT_SHORTMSG << 24) |
		((unsigned long)mevt.byte << 0) |
		((unsigned long)a << 8) |
		((unsigned long)b << 16);

	return event;
}

static unsigned int mus_get_channel(unsigned int channel) {
	static unsigned int c = 0;
	unsigned int ret = mus2mid_channel_map[channel];

	if (ret == -1) {
		ret = c;
		c++;
		if (c == 9) // mus 15 always maps to midi 9
			c++;

		mus2mid_channel_map[channel] = ret;
	}

	return ret;
}

static unsigned int mus_get_streambuf(struct mus_score* m, unsigned int* out, unsigned int* outlen) {
	MIDIEVENT e, * p;

	unsigned int streamlen = 0;

	*outlen = 0;

	while (m->ptr < m->raw_bytes + m->raw_len) {
		struct mus_player_event evt;
		unsigned int bytesread = 0;
		unsigned char a, b;
		unsigned int pitch;

		// break out if this buffer is full
		if (((streamlen + 3) * sizeof(unsigned int)) >= MAX_BUFFER_SIZE)
			break;

		// get the next event
		evt = mus_get_next_event(m);

		e.dwStreamID = 0; // always 0

		switch (evt.command) {
		case 0: // note off
			a = *evt.ptr++; // note
			b = 0; // velocity

			e.dwEvent = mus_pack_event(MID_EVENT_RELEASE, mus_get_channel(evt.channel), a, b);
			break;
		case 1: // note on
			a = *evt.ptr++; // note
			if (a & 0x80) {
				a &= 0x7f; // clear the volume flag
				mus_velocity_map[evt.channel] = *evt.ptr++; // get the new volume
			}
			b = mus_velocity_map[evt.channel]; // velocity

			e.dwEvent = mus_pack_event(MID_EVENT_PLAY, mus_get_channel(evt.channel), a, b);
			break;
		case 2: // pitch wheel
			a = *evt.ptr++; // value
			pitch = (unsigned int)a;
			pitch *= 64;
			a = (unsigned char)pitch & 0x7f;
			pitch >>= 7;
			b = (unsigned char)pitch & 0x7f;

			e.dwEvent = mus_pack_event(MID_EVENT_PITCH_CHANGE, mus_get_channel(evt.channel), a, b);
			break;
		case 3:
			a = *evt.ptr++; // controller
			a = mus2mid_controller_map[a]; // convert it to midi
			b = 0; // value

			e.dwEvent = mus_pack_event(MID_EVENT_CONTROLLER_CHANGE, mus_get_channel(evt.channel), a, b);
			break;
		case 4:
			a = *evt.ptr++; // controller
			b = *evt.ptr++; // value

			if (a == 0) // patch change
			{
				e.dwEvent = mus_pack_event(MID_EVENT_PATCH_CHANGE, mus_get_channel(evt.channel), b, 0);
			} else {
				a = mus2mid_controller_map[a]; // convert it to midi

				e.dwEvent = mus_pack_event(MID_EVENT_CONTROLLER_CHANGE, mus_get_channel(evt.channel), a, b);
			}
			break;
		case 6:
			a = 0x2f;
			b = 0;

			e.dwEvent = mus_pack_event(7, 15, a, b);
			break;
		case 5:
		case 7:
		default:
			fprintf(stderr, "Unknown event! %d\n", evt.byte & 0x7f);
			exit(1);
			break;
		}

		e.dwDeltaTime = m->ticks;

		p = (MIDIEVENT*)&out[streamlen];
		*p = e;

		streamlen += 3;

		if (evt.last) {
			m->ticks = read_var_long(evt.ptr, &bytesread);
			evt.ptr += bytesread;
		} else
			m->ticks = 0;

		m->ptr = evt.ptr;
	}

	*outlen = streamlen * sizeof(unsigned int);

	return 0;
}

unsigned int is_mus_header(unsigned char* buf, unsigned int len) {
	struct _mus_header* hdr;

	if (buf == NULL)
		return 0;

	if (len < sizeof(struct _mus_header))
		return 0;

	hdr = (struct _mus_header*)buf;
	if (hdr->id != MUS_ID)
		return 0;

	len -= hdr->score_start;
	len -= hdr->score_len;
	if (len != 0)
		return 0;

	return 1;
}

#ifdef MUS_PLAYER_STANDALONE

unsigned char* load_file(unsigned char* filename, unsigned int* len);

void mus_callback(unsigned int val) {
	printf("\r       \rStopped");
}

int main(int argc, char* argv[]) {
	unsigned char* filename;
	unsigned char* musbuf = NULL;
	unsigned int musbuflen = 0;
	unsigned char c;
	MIDIOUTCAPS     caps;

	unsigned long n, i;
	unsigned int err;

	HANDLE score;

	setvbuf(stdout, NULL, _IONBF, 0);

	if (argc > 1)
		filename = (unsigned char*)argv[1];
	else {
		printf("Usage: %s <filename>\n", argv[0]);
		return 0;
	}

	n = midiOutGetNumDevs();

	for (i = 0; i < n; i++) {
		if (!midiOutGetDevCaps(i, &caps, sizeof(MIDIOUTCAPS))) {
			printf("Device %lu: %s\r\n", i, caps.szPname);
			if (caps.dwSupport & MIDICAPS_CACHE) {
				printf(" - supports patch caching.\n");
			}
			if (caps.dwSupport & MIDICAPS_VOLUME) {
				printf(" - supports volume control.\n");
			}
			if (caps.dwSupport & MIDICAPS_LRVOLUME) {
				printf(" - supports separate left and right volume control.\n");
			}
			if (caps.dwSupport & MIDICAPS_STREAM) {
				printf(" - provides direct support for the midiStreamOut function.\n");
			}
			printf("\n");
		}
	}

	musbuf = load_file(filename, &musbuflen);
	if (musbuf == NULL) {
		fprintf(stderr, "Failed to load file %s\n", filename);
		return 0;
	}

	mus_init();

	printf("Loaded %s\n", filename);

	printf("\n(p) play/pause, (s) stop, (l) loop on/off, (q) quit, (+/-) volume up/down\n");

	score = mus_score_open(musbuf, musbuflen, mus_callback);
	if (score == NULL) {
		fprintf(stderr, "Failed to open file %s\n", filename);
		goto error;
	}

	err = 0;
	//	while((c = getc(stdin)) != 'q') // getch doesn't work well in eclipse debugger
	while ((c = _getch()) != 'q') {
		switch (c) {
		case 'l':
		case 'L':
			//			m->looping = !m->looping;
			break;
		case 'p':
		case 'P':
			if (mus_is_stopped(score)) {
				err = mus_play(score);
				if (err != MMSYSERR_NOERROR) {
					fprintf(stderr, "Error playing file %s, error %d\n", filename, err);
					goto error;
				}
				printf("\r       \rPlaying");
			} else if (mus_is_playing(score)) {
				err = mus_pause(score);
				if (err != MMSYSERR_NOERROR) {
					fprintf(stderr, "Error pausing file %s, error %d\n", filename, err);
					goto error;
				}
				printf("\r       \rPaused");
			} else if (mus_is_paused(score)) {
				err = mus_resume(score);
				if (err != MMSYSERR_NOERROR) {
					fprintf(stderr, "Error pausing file %s, error %d\n", filename, err);
					goto error;
				}
				printf("\r       \rPlaying");
			}
			break;
		case 's':
		case 'S':
			err = mus_stop(score);
			if (err != MMSYSERR_NOERROR) {
				fprintf(stderr, "Error stopping file %s, error %d\n", filename, err);
				goto error;
			}
			printf("\r       \rStopped");
			break;
		case '-':
		case '_':
			err = mus_volume(score, VOL_DOWN);
			if (err != MMSYSERR_NOERROR) {
				fprintf(stderr, "Error adjusting volume, error %d\n", err);
				goto error;
			}
			break;
		case '=':
		case '+':
			err = mus_volume(score, VOL_UP);
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

	mus_score_close(score);
	mus_shutdown();
	free(musbuf);

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

#ifndef LONG_MASK
#define LONG_MASK 0x7f
#endif

#ifndef LONG_MORE_BIT
#define LONG_MORE_BIT 0x80
#endif

static unsigned long read_var_long(unsigned char* buf, unsigned int* inc) {
	unsigned long time = 0;
	unsigned char c;

	*inc = 0;


	do {
		c = buf[(*inc)++];
		time = (time * 128) + (c & LONG_MASK);
	} while (c & LONG_MORE_BIT);

	return time;
}

#endif
