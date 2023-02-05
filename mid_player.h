/** @file mid_player.h
 * @if headers
 *
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
 * mid_player.h
 *
 *  Created on: Dec 3, 2011
 *      Author: David J. Rager
 * @endif
 * @brief This is the public API for playing MIDI data.
 *
 * This is the API for playing MIDI data stored in a memory buffer.
 */
#ifndef MID_PLAYER_H_
#define MID_PLAYER_H_

#include <windows.h>
#include <mmsystem.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief This defines a callback that is called during MIDI playback.
 *
 * This defines a callback function that is to be called during MIDI playback.
 * Currently, if a callback is registered, it is only called when playback
 * stops.
 *
 * @param[in] val	Reserved.
 */
typedef void (*mid_notify_cb)(unsigned int val);

/**
 * @brief Initialize the MIDI subsystem.
 *
 * This function initializes the MIDI subsystem. This method must be called
 * before calling any of the other MIDI functions. When finished, the MIDI
 * subsystem should be terminated by calling mid_shutdown().
 *
 * @return Returns MMSYSERR_NOERROR if successful, MMSYSERR_ERROR on error.
 */
MMRESULT mid_init();

/**
 * @brief Shut down the MIDI subsystem.
 *
 * This function shuts down the MIDI subsystem. Any currently playing MIDI
 * files will be stopped and open handles will be closed automatically. Further
 * calls to any of the MIDI functions will fail.
 */
void mid_shutdown();

/**
 * @brief This function prepares a buffer for playing.
 *
 * This function takes a buffer of MIDI formatted data and returns a HANDLE
 * that is used in subsequent calls to the MIDI functions.
 *
 * @param[in] buf	A pointer to a buffer containing the MIDI data.
 * @param[in] len	The length of the buffer containing the MIDI data.
 * @return		Returns a HANDLE to the open MIDI score if successful, NULL
 *				otherwise. This HANDLE must be closed by calling mid_score_close()
 *				when finished.
 */
HANDLE mid_score_open(unsigned char* buf, unsigned int len);

/**
 * @brief this function closes an open MIDI score.
 *
 * This function takes an open HANDLE to a MIDI score returned by a previous
 * call to mid_score_open(). Further calls to any of the MIDI functions using
 * the HANDLE value will fail.
 *
 * @param[in] h	The HANDLE to the MIDI score.
 */
void mid_score_close(HANDLE h);

/**
 * @brief This function begins playing an open MIDI score.
 *
 * This function causes an open MIDI score to begin playing.
 *
 * @param[in] h	The HANDLE to the MIDI score.
 * @return		This function returns MMSYSERR_NOERROR if successful, an error code
 * 				otherwise.
 */
MMRESULT mid_play(HANDLE h);

/**
 * @brief This function stops a playing MIDI score.
 *
 * This function causes a MIDI score that is currently playing to terminate.
 *
 * @param[in] h	The HANDLE to the MIDI score.
 * @return		This function returns MMSYSERR_NOERROR if successful, an error code
 * 				otherwise.
 */
MMRESULT mid_stop(HANDLE h);

/**
 * @brief This function registers a callback to receive MIDI notifications.
 *
 * This function registers a callback that is called by the MIDI subsystem when
 * certain events happen. Currently the callback is only called when playback
 * of the MIDI score associated with the HANDLE stops.
 *
 * @param[in] h		The HANDLE to the MIDI score.
 * @param[in] cb	The pointer to the callback function to be called.
 * @return			This function returns MMSYSERR_NOERROR if successful, an error code
 * 					otherwise.
 */
MMRESULT mid_register_callback(HANDLE h, mid_notify_cb cb);

/**
 * @brief This function pauses a playing MIDI score.
 *
 * This function pauses a playing MIDI score. A paused score may be resumed by
 * calling mid_resume().
 *
 * @param[in] h	The HANDLE to the MIDI score.
 * @return		This function returns MMSYSERR_NOERROR if successful, an error code
 * 				otherwise.
 */
MMRESULT mid_pause(HANDLE h);

/**
 * @brief This function resumes a paused MIDI score.
 *
 * This function causes an paused MIDI score to resume playing from the point
 * where it left off.
 *
 * @param[in] h	The HANDLE to the MIDI score.
 * @return		This function returns MMSYSERR_NOERROR if successful, an error code
 * 				otherwise.
 */
MMRESULT mid_resume(HANDLE h);

/**
 * @brief This function will cause a MIDI score to loop indefinitely.
 *
 * This function causes a MIDI score to loop indefinitely. When a playing score
 * reaches the end it will automatically begin again from the beginning.
 *
 * @param[in] h			The HANDLE to the MIDI score.
 * @param[in] looping	TRUE to enable looping, FALSE to disable.
 * @return				This function returns MMSYSERR_NOERROR if successful, an
 * 						error code otherwise.
 */
MMRESULT mid_set_looping(HANDLE h, BOOL looping);

/**
 * @brief This function will determine if looping is enable for the score.
 *
 * This function will determine whether or not looping is enabled for the
 * score.
 *
 * @param[in] h	The HANDLE to the MIDI score.
 * @return		TRUE if looping is enabled, FALSE otherwise.
 */
BOOL mid_get_looping(HANDLE h);

/**
 * @brief This function sets the volume for the left channel.
 *
 * This function sets the volume for the left channel. If \e h is NULL, the new
 * level affects the entire system. If \e h is a valid HANDLE the new level
 * affects only the score represented by the HANDLE.
 *
 * @param[in] h		The HANDLE to the MIDI score, or NULL to adjust the volume for
 * 					the entire system.
 * @param[in] level	The new volume level. This level must be between 0 (off) and
 * 					65535 (full volume).
 * @return			This function returns MMSYSERR_NOERROR if successful, an error
 * 					code otherwise.
 */
MMRESULT mid_set_volume_left(HANDLE h, unsigned int level);

/**
 * @brief This function sets the volume for the right channel.
 *
 * This function sets the volume for the right channel. If \e h is NULL, the new
 * level affects the entire system. If \e h is a valid HANDLE the new level
 * affects only the score represented by the HANDLE.
 *
 * @param[in] h		The HANDLE to the MIDI score, or NULL to adjust the volume for
 * 					the entire system.
 * @param[in] level	The new volume level. This level must be between 0 (off) and
 * 					65535 (full volume).
 * @return			This function returns MMSYSERR_NOERROR if successful, an error
 * 					code otherwise.
 */
MMRESULT mid_set_volume_right(HANDLE h, unsigned int level);

/**
 * @brief This function sets the volume for all channels.
 *
 * This function sets the volume for all channels. If \e h is NULL, the new level
 * affects the entire system. If \e h is a valid HANDLE the new level affects only
 * the score represented by the HANDLE.
 *
 * The volume for each channel will be set to the same value with this
 * function. To control channel volumes separately, use one of the other volume
 * control functions.
 *
 * @param[in] h		The HANDLE to the MIDI score, or NULL to adjust the volume for
 * 					the entire system.
 * @param[in] level	The new volume level. This level must be between 0 (off) and
 * 					65535 (full volume).
 * @return			This function returns MMSYSERR_NOERROR if successful, an error
 * 					code otherwise.
 */
MMRESULT mid_set_volume(HANDLE h, unsigned int level);

/**
 * @brief This function retrieves the volume for the left channel.
 *
 * This function retrieves the volume for the left channel. If \e h is NULL, the
 * level is the system volume. If \e h is a valid HANDLE the level is that of only
 * the score represented by the HANDLE.
 *
 * @param[in] h			The HANDLE to the MIDI score, or NULL to retrieve the system
 * 						volume.
 * @param[out] level	The a pointer to a variable to store the level. The level
 * 						returned will be between 0 (off) and 65535 (full volume).
 * @return				This function returns MMSYSERR_NOERROR if successful, an error
 * 						code otherwise.
 */
MMRESULT mid_get_volume_left(HANDLE h, unsigned int* level);

/**
 * @brief This function retrieves the volume for the right channel.
 *
 * This function retrieves the volume for the right channel. If \e h is NULL, the
 * level is the system volume. If \e h is a valid HANDLE the level is that of only
 * the score represented by the HANDLE.
 *
 * @param[in] h			The HANDLE to the MIDI score, or NULL to retrieve the system
 * 						volume.
 * @param[out] level	The a pointer to a variable to store the level. The level
 * 						returned will be between 0 (off) and 65535 (full volume).
 * @return				This function returns MMSYSERR_NOERROR if successful, an error
 * 						code otherwise.
 */
MMRESULT mid_get_volume_right(HANDLE h, unsigned int* level);

/**
 * @brief This function retrieves the current volume.
 *
 * This function retrieves the current playback volume. If \e h is NULL, the level
 * is the system volume. If \e h is a valid HANDLE the level is that of only the
 * score represented by the HANDLE.
 *
 * On single channel systems this is the current volume. On multi-channel
 * systems this the left channel is used as the current volume. To query
 * channel volumes separately, use one of the other volume control functions.
 *
 * @param[in] h			The HANDLE to the MIDI score, or NULL to retrieve the system
 * 						volume.
 * @param[out] level	The a pointer to a variable to store the level. The level
 * 						returned will be between 0 (off) and 65535 (full volume).
 * @return				This function returns MMSYSERR_NOERROR if successful, an error
 * 						code otherwise.
 */
MMRESULT mid_get_volume(HANDLE h, unsigned int* level);

/**
 * @brief This function will determine if the score is currently playing.
 *
 * This function will determine whether or not the score is currently playing.
 *
 * @param[in] h	The HANDLE to the MIDI score.
 * @return		TRUE if the score is playing, FALSE otherwise.
 */
BOOL mid_is_playing(HANDLE h);

/**
 * @brief This function will determine if the score is currently paused.
 *
 * This function will determine whether or not the score is currently paused.
 *
 * @param[in] h	The HANDLE to the MIDI score.
 * @return		TRUE if the score is paused, FALSE otherwise.
 */
BOOL mid_is_paused(HANDLE h);

/**
 * @brief This function will determine if the score is currently stopped.
 *
 * This function will determine whether or not the score is currently stopped.
 *
 * @param[in] h	The HANDLE to the MIDI score.
 * @return		TRUE if the score is stopped, FALSE otherwise.
 */
BOOL mid_is_stopped(HANDLE h);

#ifdef __cplusplus
}
#endif

#endif /* MID_PLAYER_H_ */
