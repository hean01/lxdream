/*
 * This file has been modified for the cdrkit suite.
 *
 * The behaviour and appearence of the program code below can differ to a major
 * extent from the version distributed by the original author(s).
 *
 * For details, see Changelog file distributed with the cdrkit package. If you
 * received this file from another source then ask the distributing person for
 * a log of modifications.
 *
 */

/* @(#)ecc.h	1.4 02/10/19 Copyright 1998-2002 Heiko Eissfeldt, Joerg Schilling */

/*
 * compact disc reed-solomon routines
 *
 * (c) 1998-2002 by Heiko Eissfeldt, heiko@colossus.escape.de
 * (c) 2002 by Joerg Schilling
 */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; see the file COPYING.  If not, write to the Free Software
 * Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#define RS_L12_BITS 8

/* audio sector definitions for CIRC */
#define FRAMES_PER_SECTOR 98
/* user data bytes per frame */
#define L1_RAW 24
/* parity bytes with 8 bit */
#define L1_Q   4
#define L1_P   4

int cd_build_address(unsigned char inout[], int sectortype, unsigned address);

/* audio sector Cross Interleaved Reed-Solomon Code (CIRC) encoder (layer 1) */
/* adds P- and Q- parity information to audio (f2) frames. Also
   optionally handles the various delays and permutations. The output with all
   stages enabled can be fed into the Eight-Fourteen-Modulator.
   On input: 2352 bytes of audio data is given.
   On output: 3136 bytes of CIRC enriched audio data are returned.
 */
int do_encode_L1(unsigned char in[L1_RAW*FRAMES_PER_SECTOR],
					  unsigned char out[(L1_RAW+L1_Q+L1_P)*FRAMES_PER_SECTOR],
					  int delay1, int delay2, int delay3, int scramble);

/* data sector definitions for RSPC */
/* user data bytes per frame */
#define L2_RAW (1024*2)
/* parity bytes for 16 bit units */
#define L2_Q   (26*2*2)
#define L2_P   (43*2*2)

/* known sector types */
#define MODE_0	GDROM_MODE0
#define MODE_1	GDROM_MODE1
#define MODE_2	GDROM_MODE2_FORMLESS
#define MODE_2_FORM_1	GDROM_MODE2_FORM1
#define MODE_2_FORM_2	GDROM_MODE2_FORM2

/* set one of the MODE_* constants for subsequent data sector formatting */
int set_sector_type(int st);
/* get the current sector type setting for data sector formatting */
int get_sector_type(void);

/* data sector layer 2 Reed-Solomon Product Code encoder */
/* encode the given data portion depending on sector type (see
   get/set_sector_type() functions). Use the given address for the header.
   The returned data is __unscrambled__ and not in F2-frame format (for that
   see function scramble_L2()).
   Supported sector types:
     MODE_0: a 12-byte sync field, a header and 2336 zeros are returned.
     MODE_1: the user data portion (2048 bytes) has to be given
             at offset 16 in the inout array.
             Sync-, header-, edc-, spare-, p- and q- fields will be added.
     MODE_2: the user data portion (2336 bytes) has to be given
             at offset 16 in the inout array.
             Sync- and header- fields will be added.
     MODE_2_FORM_1: the user data portion (8 bytes subheader followed
                    by 2048 bytes data) has to be given at offset 16
                    in the inout array.
                    Sync-, header-, edc-, p- and q- fields will be added.
     MODE_2_FORM_2: the user data portion (8 bytes subheader followed
                    by 2324 bytes data) has to be given at offset 16
                    in the inout array.
                    Sync-, header- and edc- fields will be added.
*/
int do_encode_L2(unsigned char *inout, int sectortype, unsigned address);
int decode_L2_Q(unsigned char inout[4 + L2_RAW + 12 + L2_Q]);
int decode_L2_P(unsigned char inout[4 + L2_RAW + 12 + L2_Q + L2_P]);
unsigned int build_edc(unsigned char inout[], int from, int upto);

/* generates f2 frames from otherwise fully formatted sectors (generated by
   do_encode_L2()). */
#define	EDC_SCRAMBLE_NOSWAP	1	/* Do not swap bytes while scrambling */
int scramble_L2(unsigned char *inout);

/* r-w sub channel definitions */
#define RS_SUB_RW_BITS 6

#define PACKETS_PER_SUBCHANNELFRAME 4
#define LSUB_RAW 18
#define LSUB_QRAW 2
/* 6 bit */
#define LSUB_Q 2
#define LSUB_P 4

/* R-W subchannel encoder */
/* On input: 72 bytes packed user data, four frames with each 18 bytes.
   On output: per frame: 2 bytes user data, 2 bytes Q parity, 
                         16 bytes user data, 4 bytes P parity.
   Options:
     delay1: use low level delay line
     scramble: perform low level permutations
 */
int do_encode_sub(unsigned char in[LSUB_RAW*PACKETS_PER_SUBCHANNELFRAME],
		unsigned char out[(LSUB_RAW+LSUB_Q+LSUB_P)*PACKETS_PER_SUBCHANNELFRAME],
		int delay1, int scramble);
int do_decode_sub(unsigned char in[(LSUB_RAW+LSUB_Q+LSUB_P)*PACKETS_PER_SUBCHANNELFRAME],
						unsigned char out[LSUB_RAW*PACKETS_PER_SUBCHANNELFRAME],
						int delay1, int scramble);

int decode_LSUB_Q(unsigned char inout[LSUB_QRAW + LSUB_Q]);
int decode_LSUB_P(unsigned char inout[LSUB_RAW + LSUB_Q + LSUB_P]);