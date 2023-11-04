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
 * djmm_utils.c
 *
 *  Created on: Oct 29, 2011
 *      Author: David J. Rager
 *       Email: djrager@fourthwoods.com
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

void write_var_long(unsigned int t, unsigned char** buf, unsigned int* len)
{
	unsigned int tmp = 0;
	unsigned int i = 1;

	*buf = NULL;
	*len = 0;

	tmp = t & LONG_MASK;
	while((t >>= 7) > 0)
	{
		tmp <<= 8;
		tmp |= LONG_MORE_BIT;
		tmp += (t & LONG_MASK);
		i++;
	}

	*buf = malloc(i);
	if(*buf == NULL)
		return;

	memcpy(*buf, (unsigned char*)&tmp, i);
	*len = i;
}
