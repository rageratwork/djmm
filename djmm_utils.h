/*
 * DjMM
 * v0.1
 *
 * Copyright (c) 2011, David J. Rager
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
 * djmm_utils.h
 *
 *  Created on: Oct 29, 2011
 *      Author: David J. Rager
 *       Email: djrager@fourthwoods.com
 */

#ifndef DJMM_UTILS_H_
#define DJMM_UTILS_H_

#ifdef  __cplusplus
extern "C" {
#endif

unsigned short swap_bytes_short(unsigned short in);
unsigned long swap_bytes_long(unsigned long in);

unsigned long read_var_long(unsigned char* buf, unsigned int* inc);
void write_var_long(unsigned int t, unsigned char** buf, unsigned int* len);

unsigned char* load_file(const unsigned char* filename, unsigned int* len);
unsigned int save_file(const unsigned char* filename, unsigned char* buf, unsigned int len);

//#define STATE_ERROR		0
//#define STATE_STARTING	1
//#define STATE_PLAYING	2
//#define STATE_PAUSED	3
//#define STATE_STOPPING	4
//#define STATE_STOPPED	5
//#define STATE_SHUTDOWN	6
//
//#define VOL_UP 0
//#define VOL_DOWN 1

#ifdef  __cplusplus
}
#endif

#endif /* DJMM_UTILS_H_ */
