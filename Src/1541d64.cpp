/*
 *  1541d64.cpp - 1541 emulation in disk image files (.d64/.x64/zipcode)
 *
 *  Frodo Copyright (C) Christian Bauer
 *  zipcode decoding routines (C) 1993-1997 Marko M�kel�, Paul David Doherty
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 *  Incompatibilities:
 *   - No support for relative files
 *   - Unimplemented commands: P
 *   - Impossible to implement: B-E, M-E
 */

#include <string.h>

#include "sysdeps.h"

#include "1541d64.h"
#include "IEC.h"
#include "Prefs.h"
#include "C64.h"
#include "main.h"

/* Channel modes */
enum
{
   CHMOD_FREE,			// Channel free
   CHMOD_COMMAND,		// Command/error channel
   CHMOD_DIRECTORY,	// Reading directory, using large allocated buffer
   CHMOD_FILE,			// Sequential file open, using buffer in 1541 RAM
   CHMOD_REL,			// Relative file open, using buffer in 1541 RAM
   CHMOD_DIRECT		// Direct buffer access ('#'), using buffer in 1541 RAM
};

/* Directory track */
#define DIR_TRACK 18

/* BAM structure */
enum
{
   BAM_DIR_TRACK  = 0,		// Track...
   BAM_DIR_SECTOR = 1,		// ...and sector of first directory block (unused)
   BAM_FMT_TYPE   = 2,		// Format type
   BAM_BITMAP     = 4,			// Sector allocation map
   BAM_DISK_NAME  = 144,	// Disk name
   BAM_DISK_ID    = 162,		// Disk ID
   BAM_FMT_CHAR   = 165		// Format characters
};

/* Directory structure */
enum
{
   DIR_NEXT_TRACK  = 0,		// Track...
   DIR_NEXT_SECTOR = 1,	   // ... and sector of next directory block
   DIR_ENTRIES     = 2,		// Start of directory entries (8)

   DE_TYPE         = 0,		// File type/flags
   DE_TRACK        = 1,		// Track...
   DE_SECTOR       = 2,		// ...and sector of first data block
   DE_NAME         = 3,		// File name
   DE_SIDE_TRACK   = 19,	// Track...
   DE_SIDE_SECTOR  = 20,	// ...and sector of first side sector
   DE_REC_LEN      = 21,	// Record length
   DE_OVR_TRACK    = 26,	// Track...
   DE_OVR_SECTOR   = 27,	// ...and sector on overwrite (@)
   DE_NUM_BLOCKS_L = 28,	// Number of blocks, LSB
   DE_NUM_BLOCKS_H = 29,	// Number of blocks, MSB

   SIZEOF_DE       = 32		// Size of directory entry
};

// Interleave of directory and data blocks
#define DIR_INTERLEAVE 3
#define DATA_INTERLEAVE 10

// Number of sectors per track, for all tracks
const int num_sectors[41] = 
{
   0,
   21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,
   19,19,19,19,19,19,19,
   18,18,18,18,18,18,
   17,17,17,17,17,
   17,17,17,17,17		// Tracks 36..40
};

// Accumulated number of sectors
const int accum_num_sectors[41] = 
{
   0,
   0,21,42,63,84,105,126,147,168,189,210,231,252,273,294,315,336,
   357,376,395,414,433,452,471,
   490,508,526,544,562,580,
   598,615,632,649,666,
   683,700,717,734,751	// Tracks 36..40
};

// Prototypes
static bool match(const uint8 *p, int p_len, const uint8 *n);
static bool parse_image_file(RFILE *f, image_file_desc &desc);

/* Forward declarations */
extern "C" {
RFILE* rfopen(const char *path, const char *mode);
int64_t rfseek(RFILE* stream, int64_t offset, int origin);
int64_t rftell(RFILE* stream);
int rfclose(RFILE* stream);
int64_t rfread(void* buffer,
   size_t elem_size, size_t elem_count, RFILE* stream);
int64_t rfwrite(void const* buffer,
   size_t elem_size, size_t elem_count, RFILE* stream);
int rfgetc(RFILE* stream);
}

/*
 *  Constructor: Prepare emulation, open image file
 */

ImageDrive::ImageDrive(IEC *iec, const char *filepath) : Drive(iec), the_file(NULL), bam(ram + 0x700), bam_dirty(false)
{
   int i;

	desc.type           = TYPE_D64;
	desc.header_size    = 0;
	desc.num_tracks     = 35;
	desc.id1            = 0;
   desc.id2            = 0;
	desc.has_error_info = false;

	for (i = 0; i < 18; i++)
   {
      ch[i].mode = CHMOD_FREE;
      ch[i].buf  = NULL;
   }
	ch[15].mode = CHMOD_COMMAND;

	Reset();

	// Open image file
	if (change_image(filepath))
		Ready = true;
}

/* Destructor */
ImageDrive::~ImageDrive()
{
	close_image();
}


/*
 *  Close the image file
 */

void ImageDrive::close_image(void)
{
   if (!the_file)
      return;
   close_all_channels();
   if (bam_dirty)
   {
      write_sector(DIR_TRACK, 0, bam);
      bam_dirty = false;
   }
   rfclose(the_file);
   the_file = NULL;
}


/*  Open the image file */
bool ImageDrive::change_image(const char *path)
{
   // Close old image file
   close_image();

   // Open new image file (try write access first, then read-only)
   write_protected    = false;
   the_file           = rfopen(path, "r+b");
   if (!the_file)
   {
      write_protected = true;
      the_file        = rfopen(path, "rb");
   }
   if (the_file)
   {
      // Determine file type and fill in image_file_desc structure
      if (!parse_image_file(the_file, desc))
      {
         rfclose(the_file);
         the_file = NULL;
         return false;
      }

      // Read BAM
      read_sector(DIR_TRACK, 0, bam);
      bam_dirty = false;
      return true;
   }
   return false;
}


/*
 *  Open channel
 */

uint8 ImageDrive::Open(int channel, const uint8 *name, int name_len)
{
   set_error(ERR_OK);

   // Channel 15: execute file name as command
   if (channel == 15)
   {
      execute_cmd(name, name_len);
      return ST_OK;
   }

   if (ch[channel].mode != CHMOD_FREE)
   {
      set_error(ERR_NOCHANNEL);
      return ST_OK;
   }

   if (name[0] == '$')
   {
      if (channel)
         return open_file_ts(channel, DIR_TRACK, 0);
      return open_directory(name + 1, name_len - 1);
   }

   if (name[0] == '#')
      return open_direct(channel, name);
   return open_file(channel, name, name_len);
}


/*
 *  Open file
 */

uint8 ImageDrive::open_file(int channel, const uint8 *name, int name_len)
{
	uint8 plain_name[NAMEBUF_LENGTH];
	int plain_name_len;
	int mode    = FMODE_READ;
	int type    = FTYPE_DEL;
	int rec_len = 0;
	parse_file_name(name, name_len, plain_name, plain_name_len, mode, type, rec_len);
	if (plain_name_len > 16)
		plain_name_len = 16;

	// Channel 0 is READ, channel 1 is WRITE
	if (channel == 0 || channel == 1)
   {
      mode = channel ? FMODE_WRITE : FMODE_READ;
      if (type == FTYPE_DEL)
         type = FTYPE_PRG;
   }

	ch[channel].writing = (mode == FMODE_WRITE || mode == FMODE_APPEND);

	// Wildcards are only allowed on reading
	if (ch[channel].writing 
         && (  strchr((const char *)plain_name, '*') 
            || strchr((const char *)plain_name, '?')))
   {
      set_error(ERR_SYNTAX33);
      return ST_OK;
   }

	// Check for write-protection if writing
	if (ch[channel].writing && write_protected)
   {
      set_error(ERR_WRITEPROTECT);
      return ST_OK;
   }

	// Relative files are not supported
	if (type == FTYPE_REL)
   {
      set_error(ERR_UNIMPLEMENTED);
      return ST_OK;
   }

	// Find file in directory
	int dir_track, dir_sector, entry;
	if (find_first_file(plain_name, plain_name_len,
            dir_track, dir_sector, entry))
   {
      uint8 *de;
      // File exists
      ch[channel].dir_track  = dir_track;
      ch[channel].dir_sector = dir_sector;
      ch[channel].entry      = entry;
      de                     = dir + DIR_ENTRIES + entry * SIZEOF_DE;

      // Get file type from existing file if not specified in file name
      if (type == FTYPE_DEL)
         type                = de[DE_TYPE] & 7;

      // File type doesn't match
      if ((de[DE_TYPE] & 7) != type)
         set_error(ERR_FILETYPE);
      else if (mode == FMODE_WRITE)
      {
         // Open old file for overwriting (save-replace)
         if (name[0] == '@')
            return create_file(channel, plain_name, plain_name_len, type, true);
         else
            // File to be written already exists, error
            set_error(ERR_FILEEXISTS);
      }
      else if (mode == FMODE_APPEND)
      {
         int track = 0, sector = 0, num_blocks = 0;
         // Open old file for appending
         open_file_ts(channel, de[DE_TRACK], de[DE_SECTOR]);

         // Seek to end of file
         while (ch[channel].buf[0])
         {
            if (!read_sector(
                     track = ch[channel].buf[0],
                     sector = ch[channel].buf[1],
                     ch[channel].buf))
               return ST_OK;
            num_blocks++;
         }

         // Change channel mode to writing, adjust buffer pointer
         ch[channel].writing    = true;
         ch[channel].buf_len    = ch[channel].buf[1] + 1;
         ch[channel].buf_ptr    = ch[channel].buf + ch[channel].buf_len;
         ch[channel].track      = track;
         ch[channel].sector     = sector;
         ch[channel].num_blocks = num_blocks;

      }
      else if (mode == FMODE_M)
      {
         // Open old file for reading, even if it is not closed
         return open_file_ts(channel, de[DE_TRACK], de[DE_SECTOR]);

      }
      else
      {
         // Open old file for reading, error if file is open
         if (de[DE_TYPE] & 0x80)
            return open_file_ts(channel, de[DE_TRACK], de[DE_SECTOR]);
         set_error(ERR_WRITEFILEOPEN);
      }
   }
   else
   {
      // File doesn't exist
      // Set file type to SEQ if not specified in file name
      if (type == FTYPE_DEL)
         type = FTYPE_SEQ;

      // Create new file for writing
      if (mode == FMODE_WRITE)
         return create_file(channel, plain_name, plain_name_len, type);
      set_error(ERR_FILENOTFOUND);
   }
	return ST_OK;
}


/*
 *  Open channel for reading from file given track/sector of first block
 */

uint8 ImageDrive::open_file_ts(int channel, int track, int sector)
{
   // Allocate buffer and set channel mode
   int buf = alloc_buffer(-1);
   if (buf == -1)
   {
      set_error(ERR_NOCHANNEL);
      return ST_OK;
   }
   ch[channel].buf_num = buf;
   ch[channel].buf     = ram + 0x300 + buf * 0x100;
   ch[channel].mode    = CHMOD_FILE;

   // On the next call to Read, the first block will be read
   ch[channel].buf[0]  = track;
   ch[channel].buf[1]  = sector;
   ch[channel].buf_len = 0;

   return ST_OK;
}


/*
 *  Create file and open channel for writing to file
 */

uint8 ImageDrive::create_file(int channel, const uint8 *name, int name_len, int type, bool overwrite)
{
   uint8 *de;
   // Allocate buffer
   int buf = alloc_buffer(-1);
   if (buf == -1)
   {
      set_error(ERR_NOCHANNEL);
      return ST_OK;
   }
   ch[channel].buf_num = buf;
   ch[channel].buf     = ram + 0x300 + buf * 0x100;

   // Allocate new directory entry if not overwriting
   if (!overwrite)
   {
      if (!alloc_dir_entry(ch[channel].dir_track, ch[channel].dir_sector, ch[channel].entry))
      {
         free_buffer(buf);
         return ST_OK;
      }
   }
   de                 = dir + DIR_ENTRIES + ch[channel].entry * SIZEOF_DE;

   // Allocate first data block
   ch[channel].track  = DIR_TRACK - 1;
   ch[channel].sector = -DATA_INTERLEAVE;
   if (!alloc_next_block(ch[channel].track, ch[channel].sector, DATA_INTERLEAVE))
   {
      free_buffer(buf);
      return ST_OK;
   }
   ch[channel].num_blocks = 1;

   // Write directory entry
   memset(de, 0, SIZEOF_DE);
   de[DE_TYPE] = type;		// bit 7 not set -> open file
   if (overwrite)
   {
      de[DE_OVR_TRACK]  = ch[channel].track;
      de[DE_OVR_SECTOR] = ch[channel].sector;
   }
   else
   {
      de[DE_TRACK]      = ch[channel].track;
      de[DE_SECTOR]     = ch[channel].sector;
   }
   memset(de + DE_NAME, 0xa0, 16);
   memcpy(de + DE_NAME, name, name_len);
   write_sector(ch[channel].dir_track, ch[channel].dir_sector, dir);

   // Set channel descriptor
   ch[channel].mode    = CHMOD_FILE;
   ch[channel].writing = true;
   ch[channel].buf_ptr = ch[channel].buf + 2;
   ch[channel].buf_len = 2;
   return ST_OK;
}


/*
 *  Prepare directory as BASIC program (channel 0)
 */

const char type_char_1[] = "DSPUREER";
const char type_char_2[] = "EERSELQG";
const char type_char_3[] = "LQGRL???";

uint8 ImageDrive::open_directory(const uint8 *pattern, int pattern_len)
{
   unsigned i;
	int n = 0;
   uint8 *t, *p, *q;
	int num_dir_blocks   = 0;
	// Special treatment for "$0"
	if (pattern[0] == '0' && pattern_len == 1)
   {
      pattern++;
      pattern_len--;
   }

	// Skip everything before the ':' in the pattern
	t = (uint8 *)memchr(pattern, ':', pattern_len);
	if (t)
   {
      t++;
      pattern_len -= t - pattern;
      pattern = t;
   }

	ch[0].mode = CHMOD_DIRECTORY;
	p          = ch[0].buf_ptr = ch[0].buf = new uint8[8192];

	/* Create directory title with disk name, ID and format type */
	*p++ = 0x01;	/* Load address $0401 (from PET days :-) */
	*p++ = 0x04;
	*p++ = 0x01;	/* Dummy line link */
	*p++ = 0x01;
	*p++ = 0;		/* Drive number (0) as line number */
	*p++ = 0;
	*p++ = 0x12;	/* RVS ON */
	*p++ = '\"';

	q = bam + BAM_DISK_NAME;
	for (int i=0; i<23; i++)
   {
      int c;
      if ((c = *q++) == 0xa0)
         *p++ = ' ';		// Replace 0xa0 by space
      else
         *p++ = c;
   }
	*(p-7) = '\"';
	*p++   = 0;

	// Scan all directory blocks
	dir[DIR_NEXT_TRACK]  = DIR_TRACK;
	dir[DIR_NEXT_SECTOR] = 1;

	while (  dir[DIR_NEXT_TRACK] 
         && num_dir_blocks < num_sectors[DIR_TRACK])
   {
      int j;
      if (!read_sector(dir[DIR_NEXT_TRACK], dir[DIR_NEXT_SECTOR], dir))
         return ST_OK;
      num_dir_blocks++;

      // Scan all 8 entries of a block
      uint8 *de = dir + DIR_ENTRIES;
      for (j=0; j<8; j++, de+=SIZEOF_DE)
      {
         if (de[DE_TYPE] 
               && 
               (   pattern_len == 0 
                || match(pattern, pattern_len, de + DE_NAME)))
         {

            // Dummy line link
            *p++ = 0x01;
            *p++ = 0x01;

            // Line number = number of blocks
            *p++ = de[DE_NUM_BLOCKS_L];
            *p++ = de[DE_NUM_BLOCKS_H];

            // Appropriate number of spaces to align file names
            *p++ = ' ';
            int n = (de[DE_NUM_BLOCKS_H] << 8) + de[DE_NUM_BLOCKS_L];
            if (n<10) *p++ = ' ';
            if (n<100) *p++ = ' ';

            // File name enclosed in quotes
            *p++ = '\"';
            q = de + DE_NAME;
            uint8 c;
            bool m = false;
            for (int i=0; i<16; i++) {
               if ((c = *q++) == 0xa0) {
                  if (m)
                     *p++ = ' ';			// Replace all 0xa0 by spaces
                  else
                     m = (*p++ = '\"');	// But the first by a '"'
               } else
                  *p++ = c;
            }
            if (m)
               *p++ = ' ';
            else
               *p++ = '\"';			// No 0xa0, then append a space

            // Open files are marked by '*'
            if (de[DE_TYPE] & 0x80)
               *p++ = ' ';
            else
               *p++ = '*';

            // File type
            *p++ = type_char_1[de[DE_TYPE] & 7];
            *p++ = type_char_2[de[DE_TYPE] & 7];
            *p++ = type_char_3[de[DE_TYPE] & 7];

            // Protected files are marked by '<'
            if (de[DE_TYPE] & 0x40)
               *p++ = '<';
            else
               *p++ = ' ';

            // Appropriate number of spaces at the end
            *p++ = ' ';
            if (n >= 10)
               *p++ = ' ';
            if (n >= 100)
               *p++ = ' ';
            *p++ = 0;
         }
      }
   }

	// Final line, count number of free blocks
	for (i=1; i<=35; i++)
   {
      if (i != DIR_TRACK)	// exclude track 18
         n += num_free_blocks(i);
   }

	*p++ = 0x01;		// Dummy line link
	*p++ = 0x01;
	*p++ = n & 0xff;	// Number of free blocks as line number
	*p++ = (n >> 8) & 0xff;

	*p++ = 'B';
	*p++ = 'L';
	*p++ = 'O';
	*p++ = 'C';
	*p++ = 'K';
	*p++ = 'S';
	*p++ = ' ';
	*p++ = 'F';
	*p++ = 'R';
	*p++ = 'E';
	*p++ = 'E';
	*p++ = '.';

	memset(p, ' ', 13);
	p += 13;

	*p++ = 0;
	*p++ = 0;
	*p++ = 0;

	ch[0].buf_len = p - ch[0].buf;
	return ST_OK;
}


/*
 *  Open channel for direct buffer access
 */

uint8 ImageDrive::open_direct(int channel, const uint8 *name)
{
	int buf = -1;

	if (name[1] == 0)
		buf = alloc_buffer(-1);
	else
		if ((name[1] >= '0') && (name[1] <= '3') && (name[2] == 0))
			buf = alloc_buffer(name[1] - '0');

	if (buf == -1)
   {
      set_error(ERR_NOCHANNEL);
      return ST_OK;
   }

	// The buffers are in the 1541 RAM at $300 and are 256 bytes each
	ch[channel].mode    = CHMOD_DIRECT;
	ch[channel].buf     = ram + 0x300 + buf * 0x100;
	ch[channel].buf_num = buf;

	// Store actual buffer number in buffer
	ch[channel].buf[1]  = buf + '0';
	ch[channel].buf_len = 1;
	ch[channel].buf_ptr = ch[channel].buf + 1;

	return ST_OK;
}


/*
 *  Close channel
 */

uint8 ImageDrive::Close(int channel)
{
	switch (ch[channel].mode)
   {
      case CHMOD_FREE:
         break;

      case CHMOD_COMMAND:
         close_all_channels();
         break;

      case CHMOD_DIRECT:
         free_buffer(ch[channel].buf_num);
         ch[channel].buf = NULL;
         ch[channel].mode = CHMOD_FREE;
         break;

      case CHMOD_FILE:
         if (ch[channel].writing) {

            // Current block empty? Then write CR character
            if (ch[channel].buf_len == 2) {
               ch[channel].buf[2] = 0x0d;
               ch[channel].buf_len++;
            }

            // Write last data block
            ch[channel].buf[0] = 0;
            ch[channel].buf[1] = ch[channel].buf_len - 1;
            if (!write_sector(ch[channel].track, ch[channel].sector, ch[channel].buf))
               goto free;

            // Close write file in directory
            read_sector(ch[channel].dir_track, ch[channel].dir_sector, dir);
            uint8 *de = dir + DIR_ENTRIES + ch[channel].entry * SIZEOF_DE;
            de[DE_TYPE] |= 0x80;
            de[DE_NUM_BLOCKS_L] = ch[channel].num_blocks & 0xff;
            de[DE_NUM_BLOCKS_H] = ch[channel].num_blocks >> 8;
            if (de[DE_OVR_TRACK]) {
               // Overwriting, free old data blocks and set pointer to new ones
               free_block_chain(de[DE_TRACK], de[DE_SECTOR]);
               de[DE_TRACK] = de[DE_OVR_TRACK];
               de[DE_SECTOR] = de[DE_OVR_SECTOR];
               de[DE_OVR_TRACK] = de[DE_OVR_SECTOR] = 0;
            }
            write_sector(ch[channel].dir_track, ch[channel].dir_sector, dir);
         }
free:		free_buffer(ch[channel].buf_num);
         ch[channel].buf = NULL;
         ch[channel].mode = CHMOD_FREE;
         break;

      case CHMOD_DIRECTORY:
         delete[] ch[channel].buf;
         ch[channel].buf = NULL;
         ch[channel].mode = CHMOD_FREE;
         break;
   }

	return ST_OK;
}


/*
 *  Close all channels
 */

void ImageDrive::close_all_channels()
{
   int i;
	for (i=0; i<15; i++)
		Close(i);
	Close(16);
	Close(17);

	cmd_len = 0;
}


/*
 *  Read from channel
 */

uint8 ImageDrive::Read(int channel, uint8 &byte)
{
	switch (ch[channel].mode)
   {
      case CHMOD_FREE:
         if (current_error == ERR_OK)
            set_error(ERR_FILENOTOPEN);
         break;

      case CHMOD_COMMAND:
         // Read error channel
         byte = *error_ptr++;
         if (--error_len)
            return ST_OK;
         else {
            set_error(ERR_OK);
            return ST_EOF;
         }
         break;

      case CHMOD_FILE:
         if (ch[channel].writing)
            return ST_READ_TIMEOUT;
         if (current_error != ERR_OK)
            return ST_READ_TIMEOUT;

         // Read next block if necessary
         if (ch[channel].buf_len == 0 && ch[channel].buf[0]) {
            if (!read_sector(ch[channel].buf[0], ch[channel].buf[1], ch[channel].buf))
               return ST_READ_TIMEOUT;
            ch[channel].buf_ptr = ch[channel].buf + 2;

            // Determine block length
            ch[channel].buf_len = ch[channel].buf[0] ? 254 : ch[channel].buf[1] - 1;
         }

         if (ch[channel].buf_len > 0) {
            byte = *(ch[channel].buf_ptr)++;
            if (--(ch[channel].buf_len) == 0 && ch[channel].buf[0] == 0)
               return ST_EOF;
            else
               return ST_OK;
         } else
            return ST_READ_TIMEOUT;
         break;

      case CHMOD_DIRECTORY:
      case CHMOD_DIRECT:
         if (ch[channel].buf_len > 0) {
            byte = *(ch[channel].buf_ptr)++;
            if (--(ch[channel].buf_len))
               return ST_OK;
            else
               return ST_EOF;
         } else
            return ST_READ_TIMEOUT;
         break;
   }
	return ST_READ_TIMEOUT;
}


/*
 *  Write byte to channel
 */

uint8 ImageDrive::Write(int channel, uint8 byte, bool eoi)
{
	switch (ch[channel].mode)
   {
      case CHMOD_FREE:
         if (current_error == ERR_OK)
            set_error(ERR_FILENOTOPEN);
         break;

      case CHMOD_COMMAND:
         // Collect characters and execute command on EOI
         if (cmd_len > 58) {
            set_error(ERR_SYNTAX32);
            return ST_TIMEOUT;
         }

         cmd_buf[cmd_len++] = byte;

         if (eoi) {
            execute_cmd(cmd_buf, cmd_len);
            cmd_len = 0;
         }
         return ST_OK;

      case CHMOD_DIRECTORY:
         set_error(ERR_WRITEFILEOPEN);
         break;

      case CHMOD_FILE:
         if (!ch[channel].writing)
            return ST_TIMEOUT;
         if (current_error != ERR_OK)
            return ST_TIMEOUT;

         // Buffer full?
         if (ch[channel].buf_len >= 256) {

            // Yes, allocate new block
            int track = ch[channel].track, sector = ch[channel].sector;
            if (!alloc_next_block(track, sector, DATA_INTERLEAVE))
               return ST_TIMEOUT;
            ch[channel].num_blocks++;

            // Write buffer with link to new block
            ch[channel].buf[0] = track;
            ch[channel].buf[1] = sector;
            write_sector(ch[channel].track, ch[channel].sector, ch[channel].buf);

            // Reset buffer
            ch[channel].buf_ptr = ch[channel].buf + 2;
            ch[channel].buf_len = 2;
            ch[channel].track = track;
            ch[channel].sector = sector;
         }
         *(ch[channel].buf_ptr)++ = byte;
         ch[channel].buf_len++;
         return ST_OK;

      case CHMOD_DIRECT:
         if (ch[channel].buf_len < 256) {
            *(ch[channel].buf_ptr)++ = byte;
            ch[channel].buf_len++;
            return ST_OK;
         } else
            return ST_TIMEOUT;
         break;
   }
	return ST_TIMEOUT;
}


/*
 *  Reset drive
 */

void ImageDrive::Reset(void)
{
   int i;
	close_all_channels();

	cmd_len = 0;
	for (i=0; i<4; i++)
		buf_free[i] = true;

	if (bam_dirty)
   {
      write_sector(DIR_TRACK, 0, bam);
      bam_dirty = false;
   }

	memset(ram, 0, sizeof(ram));

	read_sector(DIR_TRACK, 0, bam);

	set_error(ERR_STARTUP);
}


/*
 *  Allocate floppy buffer
 *   -> Desired buffer number or -1
 *   <- Allocated buffer number or -1
 */

int ImageDrive::alloc_buffer(int want)
{
   if (want == -1)
   {
      for (want=3; want>=0; want--)
      {
         if (buf_free[want])
         {
            buf_free[want] = false;
            return want;
         }
      }
   }
   else if (want < 4)
   {
      if (buf_free[want])
      {
         buf_free[want] = false;
         return want;
      }
   }

   return -1;
}


/*
 *  Free floppy buffer
 */

void ImageDrive::free_buffer(int buf)
{
	buf_free[buf] = true;
}


/*
 *  Search file in directory, return 
 *  directory track/sector and entry number
 *  false: not found, true: found
 */

// Return true if name 'n' matches pattern 'p'
static bool match(const uint8 *p, int p_len, const uint8 *n)
{
	int c = 0;
	if (p_len > 16)
		p_len = 16;

	while (p_len-- > 0)
   {
      if (*p == '*')	// Wildcard '*' matches all following characters
         return true;
      // Wildcard '?' matches single character
      if ((*p != *n) && (*p != '?')) 
         return false;
      p++; n++; c++;
   }

	return *n == 0xa0 || c == 16;
}

bool ImageDrive::find_file(const uint8 *pattern,
      int pattern_len, int &dir_track,
      int &dir_sector, int &entry, bool cont)
{
	/* Counter to prevent cyclic directories from 
      resulting in an infinite loop */
	int num_dir_blocks      = 0;
	/* Pointer to current directory entry */
	uint8 *de               = NULL;
	if (cont)
		de                   = dir + DIR_ENTRIES + entry * SIZEOF_DE;
	else
   {
      dir[DIR_NEXT_TRACK]  = DIR_TRACK;
      dir[DIR_NEXT_SECTOR] = 1;
      entry                = 8;
   }

	while (num_dir_blocks < num_sectors[DIR_TRACK])
   {
      /* Goto next entry */
      entry++;
      de += SIZEOF_DE;
      if (entry >= 8)
      {
         /* Read next directory block */
         if (dir[DIR_NEXT_TRACK] == 0)
            return false;
         if (!read_sector(
                  dir_track = dir[DIR_NEXT_TRACK],
                  dir_sector = dir[DIR_NEXT_SECTOR], dir))
            return false;
         num_dir_blocks++;
         entry = 0;
         de = dir + DIR_ENTRIES;
      }

      // Does entry match pattern?
      if (de[DE_TYPE] && match(pattern, pattern_len, de + DE_NAME))
         return true;
   }
	return false;
}

bool ImageDrive::find_first_file(const uint8 *pattern,
      int pattern_len, int &dir_track, int &dir_sector, int &entry)
{
	return find_file(pattern, pattern_len, dir_track, dir_sector, entry, false);
}

bool ImageDrive::find_next_file(const uint8 *pattern,
      int pattern_len, int &dir_track, int &dir_sector, int &entry)
{
	return find_file(pattern, pattern_len, dir_track, dir_sector, entry, true);
}


/*
 *  Allocate new entry in directory, returns false on error (usually when
 *  all sectors of track 18 are allocated)
 *  The track/sector and entry numbers are returned
 */

bool ImageDrive::alloc_dir_entry(int &track, int &sector, int &entry)
{
   int last_track, last_sector;
	// First look for free entry in existing directory blocks
	dir[DIR_NEXT_TRACK]  = DIR_TRACK;
	dir[DIR_NEXT_SECTOR] = 1;
	while (dir[DIR_NEXT_TRACK])
   {
      uint8 *de;
      if (!read_sector(
               track = dir[DIR_NEXT_TRACK],
               sector = dir[DIR_NEXT_SECTOR],
               dir))
         return false;

      de = dir + DIR_ENTRIES;
      for (entry=0; entry<8; entry++, de+=SIZEOF_DE)
      {
         if (de[DE_TYPE] == 0)
            return true;
      }
   }

	/* No free entry found, allocate new directory block */
	last_track  = track;
   last_sector = sector;
	if (!alloc_next_block(track, sector, DIR_INTERLEAVE))
		return false;

	/* Write link to new block to last block */
	dir[DIR_NEXT_TRACK]  = track;
	dir[DIR_NEXT_SECTOR] = sector;
	write_sector(last_track, last_sector, dir);

	// Write new empty directory block and return first entry
	memset(dir, 0, 256);
	dir[DIR_NEXT_SECTOR] = 0xff;
	write_sector(track, sector, dir);
	entry                = 0;
	return true;
}

/*
 *  Test if block is free in BAM 
 *  (track/sector are not checked for validity)
 */

bool ImageDrive::is_block_free(int track, int sector)
{
	uint8 *p = bam + BAM_BITMAP + (track - 1) * 4;
	int byte = sector / 8 + 1;
	int bit  = sector & 7;
	return p[byte] & (1 << bit);
}

/*
 *  Get number of free blocks on a track
 */
int ImageDrive::num_free_blocks(int track)
{
	return bam[BAM_BITMAP + (track - 1) * 4];
}


/*
 *  Clear BAM, mark all blocks as free
 */

static void clear_bam(uint8 *bam)
{
   int track;
	for (track=1; track<=35; track++)
   {
      static const uint8 num2bits[8]        = {0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f, 0xff};
      (bam + BAM_BITMAP)[(track-1) * 4 + 0] = num_sectors[track];
      (bam + BAM_BITMAP)[(track-1) * 4 + 1] = 0xff;
      (bam + BAM_BITMAP)[(track-1) * 4 + 2] = 0xff;
      (bam + BAM_BITMAP)[(track-1) * 4 + 3] = num2bits[num_sectors[track] - 16];
   }
}

/*
 *  Allocate block in BAM, returns error code
 */

int ImageDrive::alloc_block(int track, int sector)
{
   uint8 *p;
   int byte, bit;
	if (
            track < 1 
         || track > 35 
         || sector < 0 
         || sector >= num_sectors[track])
		return ERR_ILLEGALTS;

	p    = bam + BAM_BITMAP + (track - 1) * 4;
	byte = sector / 8 + 1;
	bit  = sector & 7;

	// Block free?
	if (p[byte] & (1 << bit))
   {
      // Yes, allocate and decrement free block count
      p[byte] &= ~(1 << bit);
      p[0]--;
      bam_dirty = true;
      return ERR_OK;
   }
   return ERR_NOBLOCK;
}


/*
 *  Free block in BAM, returns error code
 */

int ImageDrive::free_block(int track, int sector)
{
   uint8 *p;
   int byte, bit;
	if (
            track < 1 
         || track > 35 
         || sector < 0 
         || sector >= num_sectors[track])
		return ERR_ILLEGALTS;

	p    = bam + BAM_BITMAP + (track - 1) * 4;
	byte = sector / 8 + 1;
	bit  = sector & 7;

	// Block allocated?
	if (!(p[byte] & (1 << bit)))
   {
      // Yes, free and increment free block count
      p[byte] |= (1 << bit);
      p[0]++;
      bam_dirty = true;
   }
	return ERR_OK;
}


/*
 *  Allocate chain of data blocks in BAM
 */

bool ImageDrive::alloc_block_chain(int track, int sector)
{
	uint8 buf[256];
	while (alloc_block(track, sector) == ERR_OK)
   {
      if (!read_sector(track, sector, buf))
         return false;
      track = buf[0];
      sector = buf[1];
   }
	return true;
}


/*
 *  Free chain of data blocks in BAM
 */

bool ImageDrive::free_block_chain(int track, int sector)
{
	uint8 buf[256];
	while (free_block(track, sector) == ERR_OK)
   {
      if (!read_sector(track, sector, buf))
         return false;
      track  = buf[0];
      sector = buf[1];
   }
	return true;
}


/*
 *  Search and allocate next free block, returns false if no more blocks
 *  are free (ERR_DISKFULL is also set in this case)
 *  "track" and "sector" must be set to the block where the search should
 *  begin
 */

bool ImageDrive::alloc_next_block(int &track, int &sector, int interleave)
{
	// Find track with free blocks
	bool side_changed = false;
	while (num_free_blocks(track) == 0)
   {
      if (track == DIR_TRACK)
      {
         // Directory doesn't grow to other tracks
full:		track = sector = 0;
         set_error(ERR_DISKFULL);
         return false;
      }
      else if (track > DIR_TRACK)
      {
         track++;
         if (track > 35)
         {
            if (side_changed)
               goto full;
            side_changed = true;
            track        = DIR_TRACK - 1;
            sector       = 0;
         }
      }
      else
      {
         track--;
         if (track < 1)
         {
            if (side_changed)
               goto full;
            side_changed = true;
            track        = DIR_TRACK + 1;
            sector       = 0;
         }
      }
   }

	// Find next free block on track
	int num = num_sectors[track];
	sector  = sector + interleave;
	if (sector >= num)
   {
      sector -= num;
      if (sector)
         sector--;
   }
	while (!is_block_free(track, sector))
   {
      sector++;
      if (sector >= num_sectors[track])
      {
         sector = 0;
         while (!is_block_free(track, sector))
         {
            sector++;
            if (sector >= num_sectors[track])
            {
               // Something is wrong: the BAM free block count for this
               // track was >0, but we found no free blocks
               track = sector = 0;
               set_error(ERR_DIRERROR);
               return false;
            }
         }
      }
   }

	alloc_block(track, sector);
	return true;
}


/*
 *  Sector reading/writing routines
 */

static long offset_from_ts(const image_file_desc &desc, int track, int sector)
{
	if (
            (track < 1) 
         || (track > desc.num_tracks)
         || (sector < 0) 
         || (sector >= num_sectors[track]))
		return -1;

	return ((accum_num_sectors[track] + sector) << 8) + desc.header_size;
}

// Get number of sectors per given track
int sectors_per_track(const image_file_desc &desc, int track)
{
	return num_sectors[track];
}

// Get reference to error info byte of given track/sector
uint8 &error_info_for_sector(image_file_desc &desc, int track, int sector)
{
	return desc.error_info[accum_num_sectors[track] + sector];
}

static inline const uint8 &error_info_for_sector(const image_file_desc &desc, int track, int sector)
{
	return desc.error_info[accum_num_sectors[track] + sector];
}

const int conv_job_error[16] = {
	ERR_OK,				// 0 -> 00 OK
	ERR_OK,				// 1 -> 00 OK
	ERR_READ20,			// 2 -> 20 READ ERROR
	ERR_READ21,			// 3 -> 21 READ ERROR
	ERR_READ22,			// 4 -> 22 READ ERROR
	ERR_READ23,			// 5 -> 23 READ ERROR
	ERR_READ24,			// 6 -> 24 READ ERROR (undetected by 1541)
	ERR_WRITE25,		// 7 -> 25 WRITE ERROR
	ERR_WRITEPROTECT,	// 8 -> 26 WRITE PROTECT ON
	ERR_READ27,			// 9 -> 27 READ ERROR
	ERR_WRITE28,		// 10 -> 28 WRITE ERROR
	ERR_DISKID,			// 11 -> 29 DISK ID MISMATCH
	ERR_OK,				// 12 -> 00 OK
	ERR_OK,				// 13 -> 00 OK
	ERR_OK,				// 14 -> 00 OK
	ERR_NOTREADY		// 15 -> 74 DRIVE NOT READY
};

// Read sector, return error code
static int read_sector(RFILE *f,
      const image_file_desc &desc, int track, int sector, uint8 *buffer)
{
   unsigned int error;
   // Convert track/sector to byte offset in file
   long offset = offset_from_ts(desc, track, sector);
   if (offset < 0)
      return ERR_ILLEGALTS;

   if (f == NULL)
      return ERR_NOTREADY;

   rfseek(f, offset, SEEK_SET);
   if (rfread(buffer, 1, 256, f) != 256)
      return ERR_READ22;
   error = error_info_for_sector(desc, track, sector);
   return conv_job_error[error & 0x0f];
}

// Write sector, return error code
static int write_sector(RFILE *f, const image_file_desc &desc,
      int track, int sector, uint8 *buffer)
{
	// Convert track/sector to byte offset in file
	long offset = offset_from_ts(desc, track, sector);
	if (offset < 0)
		return ERR_ILLEGALTS;

	if (!f)
		return ERR_NOTREADY;

	rfseek(f, offset, SEEK_SET);
	if (rfwrite(buffer, 1, 256, f) != 256)
		return ERR_WRITE25;
   return ERR_OK;
}

// Read sector and set error message, returns false on error
bool ImageDrive::read_sector(int track, int sector, uint8 *buffer)
{
	int error = ::read_sector(the_file, desc, track, sector, buffer);
	if (error)
		set_error(error, track, sector);
	return error == ERR_OK;
}

// Write sector and set error message, returns false on error
bool ImageDrive::write_sector(int track, int sector, uint8 *buffer)
{
	int error = ::write_sector(the_file, desc, track, sector, buffer);
	if (error)
		set_error(error, track, sector);
	return error == ERR_OK;
}

// Write error info back to image file
static void write_back_error_info(RFILE *f, const image_file_desc &desc)
{
	if (desc.type == TYPE_D64 && desc.has_error_info)
   {
      int num_sectors = desc.num_tracks == 40 
         ? NUM_SECTORS_40 
         : NUM_SECTORS_35;
      rfseek(f, num_sectors * 256, SEEK_SET);
      rfwrite(desc.error_info, num_sectors, 1, f);
   }
}

// Format disk image
static bool format_image(RFILE *f, image_file_desc &desc, bool lowlevel, uint8 id1, uint8 id2, const uint8 *disk_name, int disk_name_len)
{
	uint8 p[256];

	if (lowlevel)
   {
      int track;
      // Fill buffer with 1541 empty sector pattern (4b 01 01 ...,
      // except on track 1 where it's 01 01 01 ...)
      memset(p, 1, 256);

      // Overwrite all blocks
      for (track=1; track<=35; track++)
      {
         int sector;
         if (track == 2)
            p[0] = 0x4b;
         for (sector=0; sector<num_sectors[track]; sector++)
         {
            if (write_sector(f, desc, track, sector, p) != ERR_OK)
               return false;
         }
      }

      // Clear and write error info
      memset(desc.error_info, 1, sizeof(desc.error_info));
      write_back_error_info(f, desc);

      // Clear BAM
      memset(p, 0, 256);

   }
   else
   {
      // Read BAM
      if (read_sector(f, desc, DIR_TRACK, 0, p) != ERR_OK)
         return false;
   }

	// Create and write empty BAM
	p[BAM_DIR_TRACK]  = DIR_TRACK;
	p[BAM_DIR_SECTOR] = 1;
	p[BAM_FMT_TYPE]   = 'A';
	clear_bam(p);
	p[BAM_BITMAP + (DIR_TRACK - 1) * 4 + 0] -= 2;	// Allocate BAM and first directory block
	p[BAM_BITMAP + (DIR_TRACK - 1) * 4 + 1] &= 0xfc;
	memset(p + BAM_DISK_NAME, 0xa0, 27);
	if (disk_name_len > 16)
		disk_name_len = 16;
	memcpy(p + BAM_DISK_NAME, disk_name, disk_name_len);
	p[BAM_DISK_ID] = id1;
	p[BAM_DISK_ID + 1] = id2;
	p[BAM_FMT_CHAR] = '2';
	p[BAM_FMT_CHAR + 1] = 'A';
	if (write_sector(f, desc, DIR_TRACK, 0, p) != ERR_OK)
		return false;

	// Create and write empty directory
	memset(p, 0, 256);
	p[1] = 255;
	return write_sector(f, desc, DIR_TRACK, 1, p) == ERR_OK;
}

/*
 *  Execute drive commands
 */

// BLOCK-READ:channel,0,track,sector
void ImageDrive::block_read_cmd(int channel, int track, int sector, bool user_cmd)
{
	if (channel >= 16 || ch[channel].mode != CHMOD_DIRECT)
   {
      set_error(ERR_NOCHANNEL);
      return;
   }
	if (!read_sector(track, sector, ch[channel].buf))
		return;
	if (user_cmd)
   {
      ch[channel].buf_len = 256;
      ch[channel].buf_ptr = ch[channel].buf;
   }
   else
   {
      ch[channel].buf_len = ch[channel].buf[0];
      ch[channel].buf_ptr = ch[channel].buf + 1;
   }
}

// BLOCK-WRITE:channel,0,track,sector
void ImageDrive::block_write_cmd(int channel, int track, int sector, bool user_cmd)
{
   if (write_protected)
   {
      set_error(ERR_WRITEPROTECT);
      return;
   }
   if (channel >= 16 || ch[channel].mode != CHMOD_DIRECT)
   {
      set_error(ERR_NOCHANNEL);
      return;
   }
   if (!user_cmd)
      ch[channel].buf[0] = ch[channel].buf_len ? ch[channel].buf_len - 1 : 1;
   if (!write_sector(track, sector, ch[channel].buf))
      return;
   if (!user_cmd)
   {
      ch[channel].buf_len = 1;
      ch[channel].buf_ptr = ch[channel].buf + 1;
   }
}

// BLOCK-ALLOCATE:0,track,sector
void ImageDrive::block_allocate_cmd(int track, int sector)
{
	int err = alloc_block(track, sector);
	if (err)
   {
      if (err == ERR_NOBLOCK)
      {
         // Find next free block and return its track/sector address in the
         // error message (only look on higher tracks)
         for (;;)
         {
            sector++;
            if (sector >= num_sectors[track])
            {
               track++;
               sector = 0;
               if (track > 35)
               {
                  set_error(ERR_NOBLOCK, 0, 0);
                  return;
               }
            }
            if (is_block_free(track, sector))
            {
               set_error(ERR_NOBLOCK, track, sector);
               return;
            }
         }
      }
      else
         set_error(err, track, sector);
   }
}

// BLOCK-FREE:0,track,sector
void ImageDrive::block_free_cmd(int track, int sector)
{
	int err = free_block(track, sector);
	if (err)
		set_error(err, track, sector);
}

// BUFFER-POINTER:channel,pos
void ImageDrive::buffer_pointer_cmd(int channel, int pos)
{
	if (channel >= 16 || ch[channel].mode != CHMOD_DIRECT) {
		set_error(ERR_NOCHANNEL);
		return;
	}
	ch[channel].buf_ptr = ch[channel].buf + pos;
	ch[channel].buf_len = 256 - pos;
}

// M-R<adr low><adr high>[<number>]
void ImageDrive::mem_read_cmd(uint16 adr, uint8 len)
{
	error_len = len;
	if (adr >= 0x300 && adr < 0x1000) {
		// Read from RAM
		error_ptr = (char *)ram + (adr & 0x7ff);
	} else if (adr >= 0xc000) {
		// Read from ROM
		error_ptr = (char *)(TheC64->ROM1541) + (adr - 0xc000);
	} else {
		unsupp_cmd();
		memset(error_buf, 0, len);
		error_ptr = error_buf;
	}
}

// M-W<adr low><adr high><number><data...>
void ImageDrive::mem_write_cmd(uint16 adr, uint8 len, uint8 *p)
{
	while (len)
   {
      if (adr >= 0x300 && adr < 0x1000)
      {
         // Write to RAM
         ram[adr & 0x7ff] = *p;
      }
      else if (adr < 0xc000)
      {
         unsupp_cmd();
         return;
      }
      len--; adr++; p++;
   }
}

//   COPY:new=file1,file2,...
//        ^   ^
// new_file   old_files
void ImageDrive::copy_cmd(const uint8 *new_file, int new_file_len, const uint8 *old_files, int old_files_len)
{
   bool first = true;
   // Check if destination file is already present
   int dir_track, dir_sector, entry;
   if (find_first_file(new_file, new_file_len, dir_track, dir_sector, entry)) {
      set_error(ERR_FILEEXISTS);
      return;
   }

   // Loop for all source files
   while (old_files_len > 0)
   {
      uint8 byte, st;
      uint8 *comma = (uint8 *)memchr(old_files, ',', old_files_len);
      int name_len = comma ? comma - old_files : old_files_len;

      // Check if source file is present
      if (!find_first_file(old_files, name_len,
               dir_track, dir_sector, entry))
      {
         set_error(ERR_FILENOTFOUND);
         Close(17);
         return;
      }
      uint8 *de  = dir + DIR_ENTRIES + entry * SIZEOF_DE;
      uint8 type = de[DE_TYPE] & 7, track = de[DE_TRACK],
      sector = de[DE_SECTOR];

      // If this is the first source file, open internal write channel for destination file
      if (first)
      {
         create_file(17, new_file, new_file_len, type, false);
         if (ch[17].mode == CHMOD_FREE)
            return;
         first = false;
      }

      // Open internal read channel for source file
      open_file_ts(16, track, sector);
      if (ch[16].mode == CHMOD_FREE)
      {
         Close(17);
         return;
      }

      // Copy file
      do {
         st = Read(16, byte);
         Write(17, byte, false);
      } while (st == ST_OK);
      Close(16);
      if (st != ST_EOF)
      {
         Close(17);
         return;
      }

      if (comma)
      {
         old_files_len -= name_len + 1;
         old_files      = comma + 1;
      }
      else
         old_files_len = 0;
   }
   Close(17);
}

// RENAME:new=old
//        ^   ^
// new_file   old_file
void ImageDrive::rename_cmd(const uint8 *new_file, int new_file_len, const uint8 *old_file, int old_file_len)
{
   // Check if destination file is already present
   int dir_track, dir_sector, entry;
   if (find_first_file(new_file, new_file_len, dir_track, dir_sector, entry)) {
      set_error(ERR_FILEEXISTS);
      return;
   }

   // Check if source file is present
   if (!find_first_file(old_file, old_file_len,
            dir_track, dir_sector, entry))
   {
      set_error(ERR_FILENOTFOUND);
      return;
   }

   // Check for write-protection
   if (write_protected)
   {
      set_error(ERR_WRITEPROTECT);
      return;
   }

   // Rename file in directory entry
   uint8 *p = dir + DIR_ENTRIES + entry * SIZEOF_DE;
   memset(p + DE_NAME, 0xa0, 16);
   memcpy(p + DE_NAME, new_file, new_file_len);
   write_sector(dir_track, dir_sector, dir);
}

// SCRATCH:file1,file2,...
//         ^
//         files
void ImageDrive::scratch_cmd(const uint8 *files, int files_len)
{
	int num_files = 0;
	// Check for write-protection
	if (write_protected)
   {
      set_error(ERR_WRITEPROTECT);
      return;
   }

	// Loop for all files
	while (files_len > 0)
   {
      int dir_track, dir_sector, entry;
      uint8 *comma = (uint8 *)memchr(files, ',', files_len);
      int name_len = comma ? comma - files : files_len;

      if (find_first_file(files, name_len, dir_track, dir_sector, entry))
      {
         do
         {
            uint8 *de = dir + DIR_ENTRIES + entry * SIZEOF_DE;

            // File protected? Then skip
            if (de[DE_TYPE] & 0x40)
               continue;

            // Free allocated data blocks and side sectors
            free_block_chain(de[DE_TRACK], de[DE_SECTOR]);
            free_block_chain(de[DE_SIDE_TRACK], de[DE_SIDE_SECTOR]);

            // Clear file type
            de[DE_TYPE] = 0;

            // Write directory block back
            write_sector(dir_track, dir_sector, dir);
            num_files++;
         }while (find_next_file(files, name_len, dir_track, dir_sector, entry));
      }

      if (comma)
      {
         files_len -= name_len + 1;
         files      = comma + 1;
      } else
         files_len  = 0;
   }

	// Report number of files scratched
	set_error(ERR_SCRATCHED, num_files);
}

// INITIALIZE
void ImageDrive::initialize_cmd(void)
{
   // Close all channels and re-read BAM
   close_all_channels();
   if (bam_dirty)
   {
      write_sector(DIR_TRACK, 0, bam);
      bam_dirty = false;
   }
   read_sector(DIR_TRACK, 0, bam);
}

// NEW:name,id
//     ^   ^
//  name   comma (or NULL)
void ImageDrive::new_cmd(const uint8 *name,
      int name_len, const uint8 *comma)
{
	// Check for write-protection
	if (write_protected)
   {
      set_error(ERR_WRITEPROTECT);
      return;
   }

	// Remember current ID
	uint8 id1 = bam[BAM_DISK_ID], id2 = bam[BAM_DISK_ID + 1];

	// Formatting with ID?
	if (comma)
   {
      close_all_channels();

      // Clear BAM buffer
      memset(bam, 0, 256);

      // Get ID from command
      if (comma[1])
      {
         id1 = comma[1];
         id2 = comma[2] ? comma[2] : ' ';
      }
      else
         id1 = id2 = ' ';
   }

	// Format disk image
	format_image(the_file, desc, comma, id1, id2, name, name_len);

	// Re-read BAM
	read_sector(DIR_TRACK, 0, bam);
	bam_dirty = false;
}

// VALIDATE
void ImageDrive::validate_cmd(void)
{
	// Backup of old BAM in case something goes amiss
	uint8 old_bam[256];
	memcpy(old_bam, bam, 256);

	// Clear BAM
	clear_bam(bam);
	bam_dirty = true;

	// Allocate BAM and directory
	if (!alloc_block_chain(DIR_TRACK, 0)) {
		memcpy(bam, old_bam, 256);
		return;
	}

	// Allocate all file data and side sector blocks
	int dir_track, dir_sector, entry;
	if (find_first_file((uint8 *)"*", 1, dir_track, dir_sector, entry))
   {
      do
      {
         uint8 *de = dir + DIR_ENTRIES + entry * SIZEOF_DE;

         if (de[DE_TYPE] & 0x80)
         {
            // Closed file, allocate all file data and side sector blocks
            if (!alloc_block_chain(de[DE_TRACK], de[DE_SECTOR]) || !alloc_block_chain(de[DE_SIDE_TRACK], de[DE_SIDE_SECTOR]))
            {
               memcpy(bam, old_bam, 256);
               return;
            }
         }
         else
         {
            // Open file, delete it
            de[DE_TYPE] = 0;
            write_sector(dir_track, dir_sector, dir);
         }
      } while (find_next_file((uint8 *)"*", 1, dir_track, dir_sector, entry));
   }
}


/*
 *  Check whether file with given header (64 bytes) and size looks like one
 *  of the file types supported by this module
 */

static bool is_d64_file(const uint8 *header, int64_t size)
{
	return size == NUM_SECTORS_35 * 256 || size == NUM_SECTORS_35 * 257
	    || size == NUM_SECTORS_40 * 256 || size == NUM_SECTORS_40 * 257;
}

static bool is_ed64_file(const uint8 *header, long size)
{
	// 35-track d64 file with header ID at the end (only used internally for
	// converted zipcode files)
	return size == NUM_SECTORS_35 * 256 + 2;
}

static bool is_x64_file(const uint8 *header)
{
	return memcmp(header, "C\x15\x41\x64\x01\x02", 6) == 0;
}

bool IsImageFile(const char *path, const uint8 *header, int64_t size)
{
	return is_d64_file(header, size) || is_x64_file(header);
}

/*
 *  Parse image file and fill in image_file_desc structure
 */

static bool parse_d64_file(RFILE *f, image_file_desc &desc, bool has_header_id)
{
   int64_t size;
	// .d64 files have no header
	desc.type        = has_header_id ? TYPE_ED64 : TYPE_D64;
	desc.header_size = 0;

	// Determine number of tracks
	rfseek(f, 0, SEEK_END);
	size = rftell(f);
	if (size == NUM_SECTORS_40 * 256 || size == NUM_SECTORS_40 * 257)
		desc.num_tracks = 40;
	else
		desc.num_tracks = 35;

	if (has_header_id)
   {
      // Read header ID from image file (last 2 bytes)
      rfseek(f, -2, SEEK_END);
      desc.id1 = rfgetc(f);
      desc.id2 = rfgetc(f);
   }
   else
   {
      // Read header ID from BAM (use error_info as buffer)
      rfseek(f, accum_num_sectors[18] * 256, SEEK_SET);
      rfread(desc.error_info, 1, 256, f);
      desc.id1 = desc.error_info[BAM_DISK_ID];
      desc.id2 = desc.error_info[BAM_DISK_ID + 1];
   }

	// Read error info
	memset(desc.error_info, 1, sizeof(desc.error_info));
	if (size == NUM_SECTORS_35 * 257)
   {
      rfseek(f, NUM_SECTORS_35 * 256, SEEK_SET);
      rfread(desc.error_info, NUM_SECTORS_35, 1, f);
      desc.has_error_info = true;
   }
   else if (size == NUM_SECTORS_40 * 257)
   {
      rfseek(f, NUM_SECTORS_40 * 256, SEEK_SET);
      rfread(desc.error_info, NUM_SECTORS_40, 1, f);
      desc.has_error_info = true;
   }
   else
		desc.has_error_info = false;

	return true;
}

static bool parse_x64_file(RFILE *f, image_file_desc &desc)
{
	desc.type        = TYPE_X64;
	desc.header_size = 64;

	// Read number of tracks
	rfseek(f, 7, SEEK_SET);
	desc.num_tracks = rfgetc(f);
	if (desc.num_tracks < 35 || desc.num_tracks > 40)
		return false;

	// Read header ID from BAM (use error_info as buffer)
	rfseek(f, desc.header_size + accum_num_sectors[18] * 256, SEEK_SET);
	rfread(desc.error_info, 1, 256, f);
	desc.id1 = desc.error_info[BAM_DISK_ID];
	desc.id2 = desc.error_info[BAM_DISK_ID + 1];

	// .x64 files have no error info
	memset(desc.error_info, 1, sizeof(desc.error_info));
	desc.has_error_info = false;
	return true;
}

static bool parse_image_file(RFILE *f, image_file_desc &desc)
{
   int64_t size;
	// Read header
	uint8 header[64];
	rfread(header, 1, sizeof(header), f);

	// Determine file size
	rfseek(f, 0, SEEK_END);
	size = rftell(f);

	// Determine file type and fill in image_file_desc structure
	if (is_x64_file(header))
		return parse_x64_file(f, desc);
	if (is_d64_file(header, size))
		return parse_d64_file(f, desc, false);
	if (is_ed64_file(header, size))
		return parse_d64_file(f, desc, true);
   return false;
}


/*
 *  Read directory of disk image file into (empty) 
 *  c64_dir_entry vector, return false on error 
 */
bool ReadImageDirectory(const char *path, std::vector<c64_dir_entry> &vec)
{
   bool result  = false;
   RFILE     *f = rfopen(path, "rb");
   if (f)
   {
      uint8 dir[256];
      int num_dir_blocks = 0;
      // Determine file type and fill in image_file_desc structure
      image_file_desc desc;
      if (!parse_image_file(f, desc))
         goto done;

      // Scan all directory blocks
      dir[DIR_NEXT_TRACK] = DIR_TRACK;
      dir[DIR_NEXT_SECTOR] = 1;

      while (dir[DIR_NEXT_TRACK] && num_dir_blocks < num_sectors[DIR_TRACK]) {
         if (read_sector(f, desc, dir[DIR_NEXT_TRACK], dir[DIR_NEXT_SECTOR], dir) != ERR_OK)
            break;
         num_dir_blocks++;

         // Scan all 8 entries of a block
         uint8 *de = dir + DIR_ENTRIES;
         for (int j=0; j<8; j++, de+=SIZEOF_DE) {

            // Skip empty entries
            if (de[DE_TYPE] == 0)
               continue;

            // Convert file name (strip everything after and including the first trailing space)
            uint8 name_buf[17];
            memcpy(name_buf, de + DE_NAME, 16);
            name_buf[16] = 0;
            uint8 *p = (uint8 *)memchr(name_buf, 0xa0, 16);
            if (p)
               *p = 0;

            // Convert file type
            int type = de[DE_TYPE] & 7;
            if (type > 4)
               type = FTYPE_UNKNOWN;

            // Read start address
            uint8 sa_lo = 0, sa_hi = 0;
            uint8 buf[256];
            if (read_sector(f, desc, de[DE_TRACK], de[DE_SECTOR], buf) == ERR_OK) {
               sa_lo = buf[2];
               sa_hi = buf[3];
            }

            // Add entry
            vec.push_back(c64_dir_entry(name_buf, type, !(de[DE_TYPE] & 0x80), de[DE_TYPE] & 0x40, ((de[DE_NUM_BLOCKS_H] << 8) + de[DE_NUM_BLOCKS_L]) * 254, 0, sa_lo, sa_hi));
         }
      }

      result = true;
done:
      rfclose(f);
   }
   return result;
}


/*
 *  Create new blank disk image file, returns false on error
 */

bool CreateImageFile(const char *path)
{
	image_file_desc desc;
	// Open file for writing
	RFILE *f = rfopen(path, "wb");
	if (!f)
		return false;

	// Create descriptor
	desc.type           = TYPE_D64;
	desc.header_size    = 0;
	desc.num_tracks     = 35;
	desc.id1            = 'F';
	desc.id1            = 'R';
	memset(desc.error_info, 1, sizeof(desc.error_info));
	desc.has_error_info = false;

	// Format image file
	if (!format_image(f, desc, true, 'F', 'R', (uint8 *)"D64 FILE", 8))
   {
      rfclose(f);
      remove(path);
      return false;
   }

	/* Close file */
	rfclose(f);
	return true;
}
