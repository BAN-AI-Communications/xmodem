/*	
 * Copyright 2001-2019 Georges Menie (www.menie.org)
 * Modified by Thuffir in 2019
 * All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the University of California, Berkeley nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS AND CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* this code needs standard functions memcpy() and memset()
   and input/output functions _inbyte() and _outbyte().

   the prototypes of the input/output functions are:
     int _inbyte(unsigned short timeout); // msec timeout
     void _outbyte(int c);

 */

/* Needed for memcpy() */
#include <string.h>
#include "xmodem.h"

/*** Config Section ***/
/* Define this if you have your own CCITT-CRC-16 implementation */
/* #define HAVE_CRC16 */

/* Define this if you want XMODEM-1K support, it will increase stack usage by 896 bytes */
#define XMODEM_1K

#define DLY_1S 1000
#define MAXRETRANS 25
/*** End Config Section ***/

#define SOH  0x01
#define STX  0x02
#define EOT  0x04
#define ACK  0x06
#define NAK  0x15
#define CAN  0x18
#define CTRLZ 0x1A

#ifdef XMODEM_1K
/* 1024 for XModem 1k + 3 head chars + 2 crc */
#define XBUF_SIZE (1024 + 3 + 2)
#else
/* 128 for XModem + 3 head chars + 2 crc */
#define XBUF_SIZE (128 + 3 + 2)
#endif

#ifndef HAVE_CRC16
/*
 * Calculate the CCITT-CRC-16 value of a given buffer
 */
static unsigned short crc16_ccitt(
	/* Pointer to the byte buffer */
	const unsigned char *buffer,
	/* length of the byte buffer */
	int length)
{
	unsigned short crc16 = 0;
	while(length != 0) {
		crc16  = (unsigned char)(crc16 >> 8) | (crc16 << 8);
		crc16 ^= *buffer;
		crc16 ^= (unsigned char)(crc16 & 0xff) >> 4;
		crc16 ^= (crc16 << 8) << 4;
		crc16 ^= ((crc16 & 0xff) << 4) << 1;
		buffer++;
		length--;
	}

	return crc16;
}
#endif

static int check(int crc, const unsigned char *buf, int sz)
{
	if (crc) {
		unsigned short crc = crc16_ccitt(buf, sz);
		unsigned short tcrc = (buf[sz]<<8)+buf[sz+1];
		if (crc == tcrc)
			return 1;
	}
	else {
		int i;
		unsigned char cks = 0;
		for (i = 0; i < sz; ++i) {
			cks += buf[i];
		}
		if (cks == buf[sz])
		return 1;
	}

	return 0;
}

static void flushinput(void)
{
	while (_inbyte(((DLY_1S)*3)>>1) >= 0)
		;
}

/*
 * XMODEM Receive
 */
int xmodemReceive(
	/* Function pointer for storing the received chunks or NULL*/
	void (*storeChunk)(
		/* Pointer to the function context (can be used for anything) */
		void *funcCtx,
		/* Pointer to the XMODEM receive buffer (store data from here) */
		void *xmodemBuffer,
		/* Number of bytes received in the XMODEM buffer (and to be stored) */
		int xmodemSize),
	/* If storeChunk is NULL, pointer to the buffer to store the received data, else function context pointer to pass to storeChunk() */
	void *ctx,
	/* If nonzero, number of bytes to receive else receive control packet for YMODEM support */
	int destsz,
	/* If nonzero request CRC-16 checksum instead of simple checksum */
	int crc)
{
	unsigned char xbuff[XBUF_SIZE];
	unsigned char *p;
	int bufsz;
	unsigned char trychar = crc ? 'C' : NAK;
	unsigned char packetno = destsz ? 1 : 0;
	int i, c, len = 0;
	int retry, retrans = MAXRETRANS;

	for(;;) {
		for( retry = 0; retry < 16; ++retry) {
			if (trychar) _outbyte(trychar);
			if ((c = _inbyte((DLY_1S)<<1)) >= 0) {
				switch (c) {
				case SOH:
					bufsz = 128;
					goto start_recv;
#ifdef XMODEM_1K
				case STX:
					bufsz = 1024;
					goto start_recv;
#endif
				case EOT:
					_outbyte(ACK);
					return len; /* normal end */
				case CAN:
					if ((c = _inbyte(DLY_1S)) == CAN) {
						flushinput();
						_outbyte(ACK);
						return -1; /* canceled by remote */
					}
					break;
				default:
					break;
				}
			}
		}
		if (trychar == 'C') { trychar = NAK; crc = 0; continue; }
		flushinput();
		_outbyte(CAN);
		_outbyte(CAN);
		_outbyte(CAN);
		return -2; /* sync error */

	start_recv:
		trychar = 0;
		p = xbuff;
		*p++ = c;
		for (i = 0;  i < (bufsz+(crc?1:0)+3); ++i) {
			if ((c = _inbyte(DLY_1S)) < 0) goto reject;
			*p++ = c;
		}

		if (xbuff[1] == (unsigned char)(~xbuff[2]) && 
			(xbuff[1] == packetno || xbuff[1] == (unsigned char)packetno-1) &&
			check(crc, &xbuff[3], bufsz)) {
			if (xbuff[1] == packetno)	{
				register int count = (destsz ? destsz : bufsz) - len;
				if (count > bufsz) count = bufsz;
				if (count > 0) {
					if(storeChunk) {
						storeChunk(ctx, &xbuff[3], count);
					}
					else {
						memcpy (&((unsigned char *)ctx)[len], &xbuff[3], count);
					}
					len += count;
				}
				++packetno;
				retrans = MAXRETRANS+1;
			}
			if (--retrans <= 0) {
				flushinput();
				_outbyte(CAN);
				_outbyte(CAN);
				_outbyte(CAN);
				return -3; /* too many retry error */
			}
			_outbyte(ACK);
			if(destsz) {
				continue;
			}
			else {
				return len;
			}
		}
	reject:
		flushinput();
		_outbyte(NAK);
	}
}

/*
 * XMODEM Transmit
 */
int xmodemTransmit(
	/* Function pointer for fetching the data chunks to be sent or NULL*/
	void (*fetchChunk)(
		/* Pointer to the function context (can be used for anything) */
		void *funcCtx,
		/* Pointer to the XMODEM send buffer (fetch data into here) */
		void *xmodemBuffer,
		/* Number of bytes that should be fetched (and stored into the XMODEM send buffer) */
		int xmodemSize),
	/* If fetchChunk is NULL, pointer to the buffer to be sent, else function context pointer to pass to fetchChunk() */
	void *ctx,
	/* If nonzero, number of bytes to send else send control packet for YMODEM support */
	int srcsz,
	/* If nonzero 1024 byte blocks are used (XMODEM-1K) */
	int onek,
	/* If nonzero binary mode is active (do not append CTRLZ to the end of data) */
	int binary)
{
	unsigned char xbuff[XBUF_SIZE];
	int bufsz, crc = -1;
	unsigned char packetno = srcsz ? 1 : 0;
	int i, c, len = 0;
	int retry;

	for(;;) {
		for( retry = 0; retry < 16; ++retry) {
			if ((c = _inbyte((DLY_1S)<<1)) >= 0) {
				switch (c) {
				case 'C':
					crc = 1;
					goto start_trans;
				case NAK:
					crc = 0;
					goto start_trans;
				case CAN:
					if ((c = _inbyte(DLY_1S)) == CAN) {
						_outbyte(ACK);
						flushinput();
						return -1; /* canceled by remote */
					}
					break;
				default:
					break;
				}
			}
		}
		_outbyte(CAN);
		_outbyte(CAN);
		_outbyte(CAN);
		flushinput();
		return -2; /* no sync */

		for(;;) {
		start_trans:
#ifdef XMODEM_1K
			if(onek && ((srcsz - len) > 128)) {
				xbuff[0] = STX; bufsz = 1024;
			}
			else
#endif
			{
				xbuff[0] = SOH; bufsz = 128;
			}
			xbuff[1] = packetno;
			xbuff[2] = ~packetno;
			c = (srcsz ? srcsz : bufsz) - len;
			if (c > bufsz) c = bufsz;
			if ((c > 0) || (!binary && (c == 0))) {
				memset (&xbuff[3], 0, bufsz);
				if (!binary && (c == 0)) {
					xbuff[3] = CTRLZ;
				}
				else {
					if(fetchChunk) {
						fetchChunk(ctx, &xbuff[3], c);
					}
					else {
						memcpy (&xbuff[3], &((unsigned char *)ctx)[len], c);
					}
					if (!binary && (c < bufsz)) xbuff[3+c] = CTRLZ;
				}
				if (crc) {
					unsigned short ccrc = crc16_ccitt(&xbuff[3], bufsz);
					xbuff[bufsz+3] = (ccrc>>8) & 0xFF;
					xbuff[bufsz+4] = ccrc & 0xFF;
				}
				else {
					unsigned char ccks = 0;
					for (i = 3; i < bufsz+3; ++i) {
						ccks += xbuff[i];
					}
					xbuff[bufsz+3] = ccks;
				}
				for (retry = 0; retry < MAXRETRANS; ++retry) {
					for (i = 0; i < bufsz+4+(crc?1:0); ++i) {
						_outbyte(xbuff[i]);
					}
					if ((c = _inbyte(DLY_1S)) >= 0 ) {
						switch (c) {
						case ACK:
							++packetno;
							len += bufsz;
							goto start_trans;
						case CAN:
							if ((c = _inbyte(DLY_1S)) == CAN) {
								_outbyte(ACK);
								flushinput();
								return -1; /* canceled by remote */
							}
							break;
						case NAK:
						default:
							break;
						}
					}
				}
				_outbyte(CAN);
				_outbyte(CAN);
				_outbyte(CAN);
				flushinput();
				return -4; /* xmit error */
			}
			else if(srcsz) {
				for (retry = 0; retry < 10; ++retry) {
					_outbyte(EOT);
					if ((c = _inbyte((DLY_1S)<<1)) == ACK) break;
				}
				if(c == ACK) {
					return len; /* Normal exit */
				}
				else {
					flushinput();
					return -5; /* No ACK after EOT */
				}
			}
			else {
				return len; /* YMODEM control block sent */
			}
		}
	}
}

#ifdef TEST_XMODEM_RECEIVE
int main(void)
{
	int st;

	printf ("Send data using the xmodem protocol from your terminal emulator now...\n");
	/* the following should be changed for your environment:
	   0x30000 is the download address,
	   65536 is the maximum size to be written at this address
	 */
	st = xmodemReceive(NULL, (char *)0x30000, 65536, 1);
	if (st < 0) {
		printf ("Xmodem receive error: status: %d\n", st);
	}
	else  {
		printf ("Xmodem successfully received %d bytes\n", st);
	}

	return 0;
}
#endif
#ifdef TEST_XMODEM_SEND
int main(void)
{
	int st;

	printf ("Prepare your terminal emulator to receive data now...\n");
	/* the following should be changed for your environment:
	   0x30000 is the download address,
	   12000 is the maximum size to be send from this address
	 */
	st = xmodemTransmit(NULL, (char *)0x30000, 12000, 0, 0);
	if (st < 0) {
		printf ("Xmodem transmit error: status: %d\n", st);
	}
	else  {
		printf ("Xmodem successfully transmitted %d bytes\n", st);
	}

	return 0;
}
#endif
