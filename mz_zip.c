/* zip.c -- Zip manipulation
   Version 2.5.4, September 30, 2018
   part of the MiniZip project

   Copyright (C) 2010-2018 Nathan Moinvaziri
     https://github.com/nmoinvaz/minizip
   Copyright (C) 2009-2010 Mathias Svensson
     Modifications for Zip64 support
     http://result42.com
   Copyright (C) 2007-2008 Even Rouault
     Modifications of Unzip for Zip64
   Copyright (C) 1998-2010 Gilles Vollant
     http://www.winimage.com/zLibDll/minizip.html

   This program is distributed under the terms of the same license as zlib.
   See the accompanying LICENSE file for the full text of the license.
*/

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>

#include "mz.h"
#include "mz_strm.h"
#ifdef HAVE_AES
#  include "mz_strm_aes.h"
#endif
#ifdef HAVE_BZIP2
#  include "mz_strm_bzip.h"
#endif
#include "mz_strm_crc32.h"
#ifdef HAVE_LZMA
#  include "mz_strm_lzma.h"
#endif
#include "mz_strm_mem.h"
#ifdef HAVE_PKCRYPT
#  include "mz_strm_pkcrypt.h"
#endif
#ifdef HAVE_ZLIB
#  include "mz_strm_zlib.h"
#endif

#include "mz_zip.h"

/***************************************************************************/

#define MZ_ZIP_MAGIC_LOCALHEADER        (0x04034b50)
#define MZ_ZIP_MAGIC_CENTRALHEADER      (0x02014b50)
#define MZ_ZIP_MAGIC_ENDHEADER          (0x06054b50)
#define MZ_ZIP_MAGIC_ENDHEADER64        (0x06064b50)
#define MZ_ZIP_MAGIC_ENDLOCHEADER64     (0x07064b50)
#define MZ_ZIP_MAGIC_DATADESCRIPTOR     (0x08074b50)

#define MZ_ZIP_SIZE_LD_ITEM             (32)
#define MZ_ZIP_SIZE_CD_ITEM             (46)
#define MZ_ZIP_SIZE_CD_LOCATOR64        (20)

#define MZ_ZIP_EXTENSION_ZIP64          (0x0001)
#define MZ_ZIP_EXTENSION_NTFS           (0x000a)
#define MZ_ZIP_EXTENSION_AES            (0x9901)
#define MZ_ZIP_EXTENSION_UNIX1          (0x000d)

/***************************************************************************/

typedef struct mz_zip_s
{
    mz_zip_file file_info;
    mz_zip_file local_file_info;

    void *stream;                   // main stream
    void *cd_stream;                // pointer to the stream with the cd
    void *cd_mem_stream;            // memory stream for central directory
    void *compress_stream;          // compression stream
    void *crc32_stream;             // crc32 stream
    void *crypt_stream;             // encryption stream
    void *file_info_stream;         // memory stream for storing file info
    void *local_file_info_stream;   // memory stream for storing local file info

    int32_t  open_mode;

    uint32_t disk_number_with_cd;   // number of the disk with the central dir
    uint64_t disk_offset_shift;     // correction for zips that have wrong offset start of cd

    uint64_t cd_start_pos;          // pos of the first file in the central dir stream
    uint64_t cd_current_pos;        // pos of the current file in the central dir
    uint64_t cd_offset;             // offset of start of central directory
    uint64_t cd_size;               // size of the central directory

    uint8_t  entry_scanned;         // entry header information read ok
    uint8_t  entry_opened;          // entry is open for read/write
    uint8_t  entry_raw;             // entry opened with raw mode

    int64_t  number_entry;

    uint16_t version_madeby;
    char     *comment;
} mz_zip;

/***************************************************************************/

// Locate the end of central directory of a zip file (at the end, just before the global comment)
static int32_t mz_zip_search_eocd(void *stream, uint64_t *central_pos)
{
    uint8_t buf[1024 + 4];
    int64_t file_size = 0;
    int64_t back_read = 0;
    int64_t max_back = UINT16_MAX; // maximum size of global comment
    int32_t read_size = sizeof(buf);
    int64_t read_pos = 0;
    int32_t i = 0;

    *central_pos = 0;

    if (mz_stream_seek(stream, 0, MZ_SEEK_END) != MZ_OK)
        return MZ_STREAM_ERROR;

    file_size = mz_stream_tell(stream);

    if (max_back > file_size)
        max_back = file_size;

    while (back_read < max_back)
    {
        back_read += (sizeof(buf) - 4);
        if (back_read > max_back)
            back_read = max_back;

        read_pos = file_size - back_read;
        if (read_size > (file_size - read_pos))
            read_size = (uint32_t)(file_size - read_pos);

        if (mz_stream_seek(stream, read_pos, MZ_SEEK_SET) != MZ_OK)
            break;
        if (mz_stream_read(stream, buf, read_size) != read_size)
            break;

        for (i = read_size - 3; (i--) > 0;)
        {
            if (((*(buf + i))     == (MZ_ZIP_MAGIC_ENDHEADER & 0xff)) &&
                ((*(buf + i + 1)) == (MZ_ZIP_MAGIC_ENDHEADER >> 8 & 0xff)) &&
                ((*(buf + i + 2)) == (MZ_ZIP_MAGIC_ENDHEADER >> 16 & 0xff)) &&
                ((*(buf + i + 3)) == (MZ_ZIP_MAGIC_ENDHEADER >> 24 & 0xff)))
            {
                *central_pos = read_pos + i;
                return MZ_OK;
            }
        }

        if (*central_pos != 0)
            break;
    }

    return MZ_EXIST_ERROR;
}

// Locate the end of central directory 64 of a zip file
static int32_t mz_zip_search_zip64_eocd(void *stream, const uint64_t end_central_offset, uint64_t *central_pos)
{
    uint64_t offset = 0;
    uint32_t value32 = 0;
    int32_t err = MZ_OK;


    *central_pos = 0;

    // Zip64 end of central directory locator
    err = mz_stream_seek(stream, end_central_offset - MZ_ZIP_SIZE_CD_LOCATOR64, MZ_SEEK_SET);
    // Read locator signature
    if (err == MZ_OK)
    {
        err = mz_stream_read_uint32(stream, &value32);
        if (value32 != MZ_ZIP_MAGIC_ENDLOCHEADER64)
            err = MZ_FORMAT_ERROR;
    }
    // Number of the disk with the start of the zip64 end of  central directory
    if (err == MZ_OK)
        err = mz_stream_read_uint32(stream, &value32);
    // Relative offset of the zip64 end of central directory record8
    if (err == MZ_OK)
        err = mz_stream_read_uint64(stream, &offset);
    // Total number of disks
    if (err == MZ_OK)
        err = mz_stream_read_uint32(stream, &value32);
    // Goto end of central directory record
    if (err == MZ_OK)
        err = mz_stream_seek(stream, offset, MZ_SEEK_SET);
    // The signature
    if (err == MZ_OK)
    {
        err = mz_stream_read_uint32(stream, &value32);
        if (value32 != MZ_ZIP_MAGIC_ENDHEADER64)
            err = MZ_FORMAT_ERROR;
    }

    if (err == MZ_OK)
        *central_pos = offset;

    return err;
}

static int32_t mz_zip_read_cd(void *handle)
{
    mz_zip *zip = (mz_zip *)handle;
    int64_t number_entry_cd = 0;
    uint64_t number_entry_cd64 = 0;
    uint64_t number_entry = 0;
    uint64_t eocd_pos = 0;
    uint64_t eocd_pos64 = 0;
    uint16_t value16 = 0;
    uint32_t value32 = 0;
    uint64_t value64 = 0;
    uint16_t comment_size = 0;
    int32_t err = MZ_OK;


    if (zip == NULL)
        return MZ_PARAM_ERROR;

    // Read and cache central directory records
    err = mz_zip_search_eocd(zip->stream, &eocd_pos);
    if (err == MZ_OK)
    {
        // Read end of central directory info
        err = mz_stream_seek(zip->stream, eocd_pos, MZ_SEEK_SET);
        // The signature, already checked
        if (err == MZ_OK)
            err = mz_stream_read_uint32(zip->stream, &value32);
        // Number of this disk
        if (err == MZ_OK)
            err = mz_stream_read_uint16(zip->stream, &value16);
        // Number of the disk with the start of the central directory
        if (err == MZ_OK)
            err = mz_stream_read_uint16(zip->stream, &value16);
        zip->disk_number_with_cd = value16;
        // Total number of entries in the central dir on this disk
        if (err == MZ_OK)
            err = mz_stream_read_uint16(zip->stream, &value16);
        zip->number_entry = value16;
        // Total number of entries in the central dir
        if (err == MZ_OK)
            err = mz_stream_read_uint16(zip->stream, &value16);
        number_entry_cd = value16;
        if (number_entry_cd != zip->number_entry)
            err = MZ_FORMAT_ERROR;
        // Size of the central directory
        if (err == MZ_OK)
            err = mz_stream_read_uint32(zip->stream, &value32);
        if (err == MZ_OK)
            zip->cd_size = value32;
        // Offset of start of central directory with respect to the starting disk number
        if (err == MZ_OK)
            err = mz_stream_read_uint32(zip->stream, &value32);
        if (err == MZ_OK)
            zip->cd_offset = value32;
        // Zip file global comment length
        if (err == MZ_OK)
            err = mz_stream_read_uint16(zip->stream, &comment_size);
        if ((err == MZ_OK) && (comment_size > 0))
        {
            zip->comment = (char *)MZ_ALLOC(comment_size + 1);
            if (zip->comment != NULL)
            {
                if (mz_stream_read(zip->stream, zip->comment, comment_size) != comment_size)
                    err = MZ_STREAM_ERROR;
                zip->comment[comment_size] = 0;
            }
        }

        if ((err == MZ_OK) && ((number_entry_cd == UINT16_MAX) || (zip->cd_offset == UINT32_MAX)))
        {
            // Format should be Zip64, as the central directory or file size is too large
            if (mz_zip_search_zip64_eocd(zip->stream, eocd_pos, &eocd_pos64) == MZ_OK)
            {
                eocd_pos = eocd_pos64;

                err = mz_stream_seek(zip->stream, eocd_pos, MZ_SEEK_SET);
                // The signature, already checked
                if (err == MZ_OK)
                    err = mz_stream_read_uint32(zip->stream, &value32);
                // Size of zip64 end of central directory record
                if (err == MZ_OK)
                    err = mz_stream_read_uint64(zip->stream, &value64);
                // Version made by
                if (err == MZ_OK)
                    err = mz_stream_read_uint16(zip->stream, &zip->version_madeby);
                // Version needed to extract
                if (err == MZ_OK)
                    err = mz_stream_read_uint16(zip->stream, &value16);
                // Number of this disk
                if (err == MZ_OK)
                    err = mz_stream_read_uint32(zip->stream, &value32);
                // Number of the disk with the start of the central directory
                if (err == MZ_OK)
                    err = mz_stream_read_uint32(zip->stream, &zip->disk_number_with_cd);
                // Total number of entries in the central directory on this disk
                if (err == MZ_OK)
                    err = mz_stream_read_uint64(zip->stream, &number_entry);
                // Total number of entries in the central directory
                if (err == MZ_OK)
                    err = mz_stream_read_uint64(zip->stream, &number_entry_cd64);
                if (number_entry == UINT32_MAX)
                    zip->number_entry = number_entry_cd64;
                // Size of the central directory
                if (err == MZ_OK)
                    err = mz_stream_read_uint64(zip->stream, &zip->cd_size);
                // Offset of start of central directory with respect to the starting disk number
                if (err == MZ_OK)
                    err = mz_stream_read_uint64(zip->stream, &zip->cd_offset);
            }
            else if ((zip->number_entry == UINT16_MAX) || (number_entry_cd != zip->number_entry) ||
                     (zip->cd_size == UINT16_MAX) || (zip->cd_offset == UINT32_MAX))
            {
                err = MZ_FORMAT_ERROR;
            }
        }
    }

    if (err == MZ_OK)
    {
        if (eocd_pos < zip->cd_offset + zip->cd_size)
            err = MZ_FORMAT_ERROR;
    }

    if (err == MZ_OK)
    {
        // Verify central directory signature exists at offset
        err = mz_stream_seek(zip->stream, zip->cd_offset, MZ_SEEK_SET);
        if (err == MZ_OK)
            err = mz_stream_read_uint32(zip->stream, &value32);
        if (value32 != MZ_ZIP_MAGIC_CENTRALHEADER)
        {
            // If not found attempt to seek backward to find it
            err = mz_stream_seek(zip->stream, eocd_pos - zip->cd_size, MZ_SEEK_SET);
            if (err == MZ_OK)
                err = mz_stream_read_uint32(zip->stream, &value32);
            if (value32 == MZ_ZIP_MAGIC_CENTRALHEADER)
            {
                // If found compensate for incorrect locations
                value64 = zip->cd_offset;
                zip->cd_offset = eocd_pos - zip->cd_size;
                zip->disk_offset_shift = zip->cd_offset - value64;
            }
        }
    }

    return err;
}

static int32_t mz_zip_write_cd(void *handle)
{
    mz_zip *zip = (mz_zip *)handle;
    uint16_t comment_size = 0;
    uint64_t zip64_eocd_pos_inzip = 0;
    int64_t disk_number = 0;
    int64_t disk_size = 0;
    int32_t err = MZ_OK;


    if (zip == NULL)
        return MZ_PARAM_ERROR;

    if (mz_stream_get_prop_int64(zip->stream, MZ_STREAM_PROP_DISK_NUMBER, &disk_number) == MZ_OK)
        zip->disk_number_with_cd = (uint32_t)disk_number;
    if (mz_stream_get_prop_int64(zip->stream, MZ_STREAM_PROP_DISK_SIZE, &disk_size) == MZ_OK && disk_size > 0)
        zip->disk_number_with_cd += 1;
    mz_stream_set_prop_int64(zip->stream, MZ_STREAM_PROP_DISK_NUMBER, -1);

    zip->cd_offset = mz_stream_tell(zip->stream);
    mz_stream_seek(zip->cd_mem_stream, 0, MZ_SEEK_END);
    zip->cd_size = (uint32_t)mz_stream_tell(zip->cd_mem_stream);
    mz_stream_seek(zip->cd_mem_stream, 0, MZ_SEEK_SET);

    err = mz_stream_copy(zip->stream, zip->cd_mem_stream, (int32_t)zip->cd_size);

    // Write the ZIP64 central directory header
    if (zip->cd_offset >= UINT32_MAX || zip->number_entry > UINT16_MAX)
    {
        zip64_eocd_pos_inzip = mz_stream_tell(zip->stream);

        err = mz_stream_write_uint32(zip->stream, MZ_ZIP_MAGIC_ENDHEADER64);

        // Size of this 'zip64 end of central directory'
        if (err == MZ_OK)
            err = mz_stream_write_uint64(zip->stream, (uint64_t)44);
        // Version made by
        if (err == MZ_OK)
            err = mz_stream_write_uint16(zip->stream, zip->version_madeby);
        // Version needed
        if (err == MZ_OK)
            err = mz_stream_write_uint16(zip->stream, (uint16_t)45);
        // Number of this disk
        if (err == MZ_OK)
            err = mz_stream_write_uint32(zip->stream, zip->disk_number_with_cd);
        // Number of the disk with the start of the central directory
        if (err == MZ_OK)
            err = mz_stream_write_uint32(zip->stream, zip->disk_number_with_cd);
        // Total number of entries in the central dir on this disk
        if (err == MZ_OK)
            err = mz_stream_write_uint64(zip->stream, zip->number_entry);
        // Total number of entries in the central dir
        if (err == MZ_OK)
            err = mz_stream_write_uint64(zip->stream, zip->number_entry);
        // Size of the central directory
        if (err == MZ_OK)
            err = mz_stream_write_uint64(zip->stream, (uint64_t)zip->cd_size);
        // Offset of start of central directory with respect to the starting disk number
        if (err == MZ_OK)
            err = mz_stream_write_uint64(zip->stream, zip->cd_offset);
        if (err == MZ_OK)
            err = mz_stream_write_uint32(zip->stream, MZ_ZIP_MAGIC_ENDLOCHEADER64);

        // Number of the disk with the start of the central directory
        if (err == MZ_OK)
            err = mz_stream_write_uint32(zip->stream, zip->disk_number_with_cd);
        // Relative offset to the end of zip64 central directory
        if (err == MZ_OK)
            err = mz_stream_write_uint64(zip->stream, zip64_eocd_pos_inzip);
        // Number of the disk with the start of the central directory
        if (err == MZ_OK)
            err = mz_stream_write_uint32(zip->stream, zip->disk_number_with_cd + 1);
    }

    // Write the central directory header

    // Signature
    if (err == MZ_OK)
        err = mz_stream_write_uint32(zip->stream, MZ_ZIP_MAGIC_ENDHEADER);
    // Number of this disk
    if (err == MZ_OK)
        err = mz_stream_write_uint16(zip->stream, (uint16_t)zip->disk_number_with_cd);
    // Number of the disk with the start of the central directory
    if (err == MZ_OK)
        err = mz_stream_write_uint16(zip->stream, (uint16_t)zip->disk_number_with_cd);
    // Total number of entries in the central dir on this disk
    if (err == MZ_OK)
    {
        if (zip->number_entry >= UINT16_MAX)
            err = mz_stream_write_uint16(zip->stream, UINT16_MAX);
        else
            err = mz_stream_write_uint16(zip->stream, (uint16_t)zip->number_entry);
    }
    // Total number of entries in the central dir
    if (err == MZ_OK)
    {
        if (zip->number_entry >= UINT16_MAX)
            err = mz_stream_write_uint16(zip->stream, UINT16_MAX);
        else
            err = mz_stream_write_uint16(zip->stream, (uint16_t)zip->number_entry);
    }
    // Size of the central directory
    if (err == MZ_OK)
        err = mz_stream_write_uint32(zip->stream, (uint32_t)zip->cd_size);
    // Offset of start of central directory with respect to the starting disk number
    if (err == MZ_OK)
    {
        if (zip->cd_offset >= UINT32_MAX)
            err = mz_stream_write_uint32(zip->stream, UINT32_MAX);
        else
            err = mz_stream_write_uint32(zip->stream, (uint32_t)zip->cd_offset);
    }

    // Write global comment
    if (zip->comment != NULL)
        comment_size = (uint16_t)strlen(zip->comment);
    if (err == MZ_OK)
        err = mz_stream_write_uint16(zip->stream, comment_size);
    if (err == MZ_OK)
    {
        if (mz_stream_write(zip->stream, zip->comment, comment_size) != comment_size)
            err = MZ_STREAM_ERROR;
    }
    return err;
}

void *mz_zip_create(void **handle)
{
    mz_zip *zip = NULL;

    zip = (mz_zip *)MZ_ALLOC(sizeof(mz_zip));
    if (zip != NULL)
    {
        memset(zip, 0, sizeof(mz_zip));
    }
    if (handle != NULL)
        *handle = zip;

    return zip;
}

void mz_zip_delete(void **handle)
{
    mz_zip *zip = NULL;
    if (handle == NULL)
        return;
    zip = (mz_zip *)*handle;
    if (zip != NULL)
    {
        MZ_FREE(zip);
    }
    *handle = NULL;
}

int32_t mz_zip_open(void *handle, void *stream, int32_t mode)
{
    mz_zip *zip = (mz_zip *)handle;
    int32_t err = MZ_OK;


    if (zip == NULL)
        return MZ_PARAM_ERROR;

    zip->stream = stream;

    if (mode & MZ_OPEN_MODE_WRITE)
    {
        mz_stream_mem_create(&zip->cd_mem_stream);
        mz_stream_mem_open(zip->cd_mem_stream, NULL, MZ_OPEN_MODE_CREATE);

        zip->cd_stream = zip->cd_mem_stream;
    }
    else
    {
        zip->cd_stream = stream;
    }

    if ((mode & MZ_OPEN_MODE_READ) || (mode & MZ_OPEN_MODE_APPEND))
    {
        if ((mode & MZ_OPEN_MODE_CREATE) == 0)
            err = mz_zip_read_cd(zip);

        if ((err == MZ_OK) && (mode & MZ_OPEN_MODE_APPEND))
        {
            if (zip->cd_size > 0)
            {
                // Store central directory in memory
                err = mz_stream_seek(zip->stream, zip->cd_offset, MZ_SEEK_SET);
                if (err == MZ_OK)
                    err = mz_stream_copy(zip->cd_mem_stream, zip->stream, (uint32_t)zip->cd_size);
                if (err == MZ_OK)
                    err = mz_stream_seek(zip->stream, zip->cd_offset, MZ_SEEK_SET);
            }
            else
            {
                // If no central directory, append new zip to end of file
                err = mz_stream_seek(zip->stream, 0, MZ_SEEK_END);
            }
        }
        else
        {
            zip->cd_start_pos = zip->cd_offset;
        }
    }

    if (err == MZ_OK)
    {
        // Memory streams used to store variable length file info data
        mz_stream_mem_create(&zip->file_info_stream);
        mz_stream_mem_open(zip->file_info_stream, NULL, MZ_OPEN_MODE_CREATE);

        mz_stream_mem_create(&zip->local_file_info_stream);
        mz_stream_mem_open(zip->local_file_info_stream, NULL, MZ_OPEN_MODE_CREATE);
    }

    if (err != MZ_OK)
    {
        mz_zip_close(zip);
        return err;
    }

    zip->open_mode = mode;

    return err;
}

int32_t mz_zip_close(void *handle)
{
    mz_zip *zip = (mz_zip *)handle;
    int32_t err = MZ_OK;

    if (zip == NULL)
        return MZ_PARAM_ERROR;

    if (mz_zip_entry_is_open(handle) == MZ_OK)
    {
        err = mz_zip_entry_close(handle);
        if (err != MZ_OK)
            return err;
    }

    if (zip->open_mode & MZ_OPEN_MODE_WRITE)
        err = mz_zip_write_cd(handle);

    if (zip->cd_mem_stream != NULL)
    {
        mz_stream_close(zip->cd_mem_stream);
        mz_stream_delete(&zip->cd_mem_stream);
    }

    if (zip->file_info_stream != NULL)
    {
        mz_stream_mem_close(zip->file_info_stream);
        mz_stream_mem_delete(&zip->file_info_stream);
    }
    if (zip->local_file_info_stream != NULL)
    {
        mz_stream_mem_close(zip->local_file_info_stream);
        mz_stream_mem_delete(&zip->local_file_info_stream);
    }

    if (zip->comment)
        MZ_FREE(zip->comment);

    zip->stream = NULL;
    zip->cd_stream = NULL;

    return err;
}

int32_t mz_zip_get_comment(void *handle, const char **comment)
{
    mz_zip *zip = (mz_zip *)handle;
    if (zip == NULL || comment == NULL)
        return MZ_PARAM_ERROR;
    if (zip->comment == NULL)
        return MZ_EXIST_ERROR;
    *comment = zip->comment;
    return MZ_OK;
}

int32_t mz_zip_set_comment(void *handle, const char *comment)
{
    mz_zip *zip = (mz_zip *)handle;
    uint16_t comment_size = 0;
    if (zip == NULL || comment == NULL)
        return MZ_PARAM_ERROR;
    if (zip->comment != NULL)
        MZ_FREE(zip->comment);
    comment_size = (uint16_t)(strlen(comment) + 1);
    zip->comment = (char *)MZ_ALLOC(comment_size);
    if (zip->comment == NULL)
        return MZ_MEM_ERROR;
    strncpy(zip->comment, comment, comment_size - 1);
    zip->comment[comment_size - 1] = 0;
    return MZ_OK;
}

int32_t mz_zip_get_version_madeby(void *handle, uint16_t *version_madeby)
{
    mz_zip *zip = (mz_zip *)handle;
    if (zip == NULL || version_madeby == NULL)
        return MZ_PARAM_ERROR;
    *version_madeby = zip->version_madeby;
    return MZ_OK;
}

int32_t mz_zip_set_version_madeby(void *handle, uint16_t version_madeby)
{
    mz_zip *zip = (mz_zip *)handle;
    if (zip == NULL)
        return MZ_PARAM_ERROR;
    zip->version_madeby = version_madeby;
    return MZ_OK;
}

int32_t mz_zip_get_stream(void *handle, void **stream)
{
    mz_zip *zip = (mz_zip *)handle;
    if (zip == NULL || stream == NULL)
        return MZ_PARAM_ERROR;
    *stream = zip->stream;
    if (*stream == NULL)
        return MZ_EXIST_ERROR;
    return MZ_OK;
}

// Get info about the current file in the zip file
static int32_t mz_zip_entry_read_header(void *stream, uint8_t local, mz_zip_file *file_info, void *file_info_stream)
{
    uint64_t ntfs_time = 0;
    uint32_t reserved = 0;
    uint32_t magic = 0;
    uint32_t dos_date = 0;
    uint32_t extra_pos = 0;
    uint32_t extra_data_size_read = 0;
    uint16_t extra_header_id = 0;
    uint16_t extra_data_size = 0;
    uint16_t ntfs_attrib_id = 0;
    uint16_t ntfs_attrib_size = 0;
    uint16_t value16 = 0;
    uint32_t value32 = 0;
    int64_t max_seek = 0;
    int64_t seek = 0;
    int32_t err = MZ_OK;


    memset(file_info, 0, sizeof(mz_zip_file));

    // Check the magic
    err = mz_stream_read_uint32(stream, &magic);
    if (err == MZ_END_OF_STREAM)
        err = MZ_END_OF_LIST;
    else if (magic == MZ_ZIP_MAGIC_ENDHEADER || magic == MZ_ZIP_MAGIC_ENDHEADER64)
        err = MZ_END_OF_LIST;
    else if ((local) && (magic != MZ_ZIP_MAGIC_LOCALHEADER))
        err = MZ_FORMAT_ERROR;
    else if ((!local) && (magic != MZ_ZIP_MAGIC_CENTRALHEADER))
        err = MZ_FORMAT_ERROR;

    // Read header fields
    if (err == MZ_OK)
    {
        if (!local)
            err = mz_stream_read_uint16(stream, &file_info->version_madeby);
        if (err == MZ_OK)
            err = mz_stream_read_uint16(stream, &file_info->version_needed);
        if (err == MZ_OK)
            err = mz_stream_read_uint16(stream, &file_info->flag);
        if (err == MZ_OK)
            err = mz_stream_read_uint16(stream, &file_info->compression_method);
        if (err == MZ_OK)
        {
            err = mz_stream_read_uint32(stream, &dos_date);
            file_info->modified_date = mz_zip_dosdate_to_time_t(dos_date);
        }
        if (err == MZ_OK)
            err = mz_stream_read_uint32(stream, &file_info->crc);
        if (err == MZ_OK)
        {
            err = mz_stream_read_uint32(stream, &value32);
            file_info->compressed_size = value32;
        }
        if (err == MZ_OK)
        {
            err = mz_stream_read_uint32(stream, &value32);
            file_info->uncompressed_size = value32;
        }
        if (err == MZ_OK)
            err = mz_stream_read_uint16(stream, &file_info->filename_size);
        if (err == MZ_OK)
            err = mz_stream_read_uint16(stream, &file_info->extrafield_size);
        if (!local)
        {
            if (err == MZ_OK)
                err = mz_stream_read_uint16(stream, &file_info->comment_size);
            if (err == MZ_OK)
            {
                err = mz_stream_read_uint16(stream, &value16);
                file_info->disk_number = value16;
            }
            if (err == MZ_OK)
                err = mz_stream_read_uint16(stream, &file_info->internal_fa);
            if (err == MZ_OK)
                err = mz_stream_read_uint32(stream, &file_info->external_fa);
            if (err == MZ_OK)
            {
                err = mz_stream_read_uint32(stream, &value32);
                file_info->disk_offset = value32;
            }
        }
    }

    max_seek = (int64_t)file_info->filename_size + file_info->extrafield_size + file_info->comment_size + 3;
    if (err == MZ_OK)
        err = mz_stream_seek(file_info_stream, max_seek, MZ_SEEK_SET);
    if (err == MZ_OK)
        err = mz_stream_seek(file_info_stream, 0, MZ_SEEK_SET);

    if ((err == MZ_OK) && (file_info->filename_size > 0))
    {
        mz_stream_mem_get_buffer(file_info_stream, (const void **)&file_info->filename);

        err = mz_stream_copy(file_info_stream, stream, file_info->filename_size);
        if (err == MZ_OK)
            err = mz_stream_write_uint8(file_info_stream, 0);

        seek += (int64_t)file_info->filename_size + 1;
    }

    if ((err == MZ_OK) && (file_info->extrafield_size > 0))
    {
        mz_stream_mem_get_buffer_at(file_info_stream, seek, (const void **)&file_info->extrafield);

        err = mz_stream_copy(file_info_stream, stream, file_info->extrafield_size);
        if (err == MZ_OK)
            err = mz_stream_write_uint8(file_info_stream, 0);

        // Seek back and parse the extra field
        if (err == MZ_OK)
            err = mz_stream_seek(file_info_stream, seek, MZ_SEEK_SET);

        seek += (int64_t)file_info->extrafield_size + 1;

        while ((err == MZ_OK) && (extra_pos < file_info->extrafield_size))
        {
            err = mz_stream_read_uint16(file_info_stream, &extra_header_id);
            if (err == MZ_OK)
                err = mz_stream_read_uint16(file_info_stream, &extra_data_size);

            // Read ZIP64 extra field
            if (extra_header_id == MZ_ZIP_EXTENSION_ZIP64)
            {
                if ((err == MZ_OK) && (file_info->uncompressed_size == UINT32_MAX))
                    err = mz_stream_read_uint64(file_info_stream, &file_info->uncompressed_size);
                if ((err == MZ_OK) && (file_info->compressed_size == UINT32_MAX))
                    err = mz_stream_read_uint64(file_info_stream, &file_info->compressed_size);
                if ((err == MZ_OK) && (file_info->disk_offset == UINT32_MAX))
                    err = mz_stream_read_uint64(file_info_stream, &file_info->disk_offset);
                if ((err == MZ_OK) && (file_info->disk_number == UINT16_MAX))
                    err = mz_stream_read_uint32(file_info_stream, &file_info->disk_number);
            }
            // Read NTFS extra field
            else if (extra_header_id == MZ_ZIP_EXTENSION_NTFS)
            {
                if (err == MZ_OK)
                    err = mz_stream_read_uint32(file_info_stream, &reserved);
                extra_data_size_read = 4;

                while ((err == MZ_OK) && (extra_data_size_read < extra_data_size))
                {
                    err = mz_stream_read_uint16(file_info_stream, &ntfs_attrib_id);
                    if (err == MZ_OK)
                        err = mz_stream_read_uint16(file_info_stream, &ntfs_attrib_size);

                    if ((err == MZ_OK) && (ntfs_attrib_id == 0x01) && (ntfs_attrib_size == 24))
                    {
                        err = mz_stream_read_uint64(file_info_stream, &ntfs_time);
                        mz_zip_ntfs_to_unix_time(ntfs_time, &file_info->modified_date);

                        if (err == MZ_OK)
                        {
                            err = mz_stream_read_uint64(file_info_stream, &ntfs_time);
                            mz_zip_ntfs_to_unix_time(ntfs_time, &file_info->accessed_date);
                        }
                        if (err == MZ_OK)
                        {
                            err = mz_stream_read_uint64(file_info_stream, &ntfs_time);
                            mz_zip_ntfs_to_unix_time(ntfs_time, &file_info->creation_date);
                        }
                    }
                    else
                    {
                        if (err == MZ_OK)
                            err = mz_stream_seek(file_info_stream, ntfs_attrib_size, MZ_SEEK_CUR);
                    }

                    extra_data_size_read += ntfs_attrib_size + 4;
                }
            }
            // Read UNIX1 extra field
            else if (extra_header_id == MZ_ZIP_EXTENSION_UNIX1)
            {
                if (err == MZ_OK && file_info->accessed_date == 0)
                {
                    err = mz_stream_read_uint32(file_info_stream, &value32);
                    if (err == MZ_OK)
                        file_info->accessed_date = value32;
                }
                if (err == MZ_OK && file_info->modified_date == 0)
                {
                    err = mz_stream_read_uint32(file_info_stream, &value32);
                    if (err == MZ_OK)
                        file_info->modified_date = value32;
                }
                if (err == MZ_OK)
                    err = mz_stream_read_uint16(file_info_stream, &value16); // User id
                if (err == MZ_OK)
                    err = mz_stream_read_uint16(file_info_stream, &value16); // Group id

                // Skip variable data
                mz_stream_seek(file_info_stream, extra_data_size - (4 + 4 + 2 + 2), SEEK_CUR);
            }
#ifdef HAVE_AES
            // Read AES extra field
            else if (extra_header_id == MZ_ZIP_EXTENSION_AES)
            {
                uint8_t value8 = 0;
                // Verify version info
                err = mz_stream_read_uint16(file_info_stream, &value16);
                // Support AE-1 and AE-2
                if (value16 != 1 && value16 != 2)
                    err = MZ_FORMAT_ERROR;
                file_info->aes_version = value16;
                if (err == MZ_OK)
                    err = mz_stream_read_uint8(file_info_stream, &value8);
                if ((char)value8 != 'A')
                    err = MZ_FORMAT_ERROR;
                if (err == MZ_OK)
                    err = mz_stream_read_uint8(file_info_stream, &value8);
                if ((char)value8 != 'E')
                    err = MZ_FORMAT_ERROR;
                // Get AES encryption strength and actual compression method
                if (err == MZ_OK)
                {
                    err = mz_stream_read_uint8(file_info_stream, &value8);
                    file_info->aes_encryption_mode = value8;
                }
                if (err == MZ_OK)
                {
                    err = mz_stream_read_uint16(file_info_stream, &value16);
                    file_info->compression_method = value16;
                }
            }
#endif
            else
            {
                err = mz_stream_seek(file_info_stream, extra_data_size, MZ_SEEK_CUR);
            }

            extra_pos += 4 + extra_data_size;
        }
    }

    if ((err == MZ_OK) && (file_info->comment_size > 0))
    {
        mz_stream_mem_get_buffer_at(file_info_stream, seek, (const void **)&file_info->comment);

        err = mz_stream_copy(file_info_stream, stream, file_info->comment_size);
        if (err == MZ_OK)
            err = mz_stream_write_uint8(file_info_stream, 0);
    }

    return err;
}

static int32_t mz_zip_entry_write_header(void *stream, uint8_t local, mz_zip_file *file_info)
{
    uint64_t ntfs_time = 0;
    uint32_t reserved = 0;
    uint32_t dos_date = 0;
    uint16_t extrafield_size = 0;
    uint16_t extrafield_zip64_size = 0;
    uint16_t extrafield_ntfs_size = 0;
    uint16_t filename_size = 0;
    uint16_t filename_length = 0;
    uint16_t comment_size = 0;
    uint16_t version_needed = 0;
    uint16_t field_type = 0;
    uint16_t field_length = 0;
    int32_t err = MZ_OK;
    int32_t err_mem = MZ_OK;
    uint8_t zip64 = 0;
    uint8_t skip_aes = 0;
    void *extrafield_ms = NULL;

    if (file_info == NULL)
        return MZ_PARAM_ERROR;

    // Calculate extra field sizes
    if (file_info->uncompressed_size >= UINT32_MAX)
        extrafield_zip64_size += 8;
    if (file_info->compressed_size >= UINT32_MAX)
        extrafield_zip64_size += 8;
    if (file_info->disk_offset >= UINT32_MAX)
        extrafield_zip64_size += 8;

    if (file_info->zip64 == MZ_ZIP64_AUTO)
    {
        // If uncompressed size is unknown, assume zip64 for 64-bit data descriptors
        zip64 = (local && file_info->uncompressed_size == 0) || (extrafield_zip64_size > 0);
    }
    else if (file_info->zip64 == MZ_ZIP64_FORCE)
    {
        zip64 = 1;
    }
    else if (file_info->zip64 == MZ_ZIP64_DISABLE)
    {
        // Zip64 extension is required to zip file
        if (extrafield_zip64_size > 0)
            return MZ_PARAM_ERROR;
    }

    if (zip64)
    {
        extrafield_size += 4;
        extrafield_size += extrafield_zip64_size;
    }

    // Calculate extra field size and check for duplicates
    if (file_info->extrafield_size > 0)
    {
        mz_stream_mem_create(&extrafield_ms);
        mz_stream_mem_set_buffer(extrafield_ms, (void *)file_info->extrafield, file_info->extrafield_size);

        do
        {
            err_mem = mz_stream_read_uint16(extrafield_ms, &field_type);
            if (err_mem == MZ_OK)
                err_mem = mz_stream_read_uint16(extrafield_ms, &field_length);
            if (err_mem != MZ_OK)
                break;

            // Prefer incoming aes extensions over ours
            if (field_type == MZ_ZIP_EXTENSION_AES)
                skip_aes = 1;

            // Prefer our zip64, ntfs extension over incoming
            if (field_type != MZ_ZIP_EXTENSION_ZIP64 && field_type != MZ_ZIP_EXTENSION_NTFS)
                extrafield_size += 4 + field_length;

            if (err_mem == MZ_OK)
                err_mem = mz_stream_seek(extrafield_ms, field_length, SEEK_CUR);
        }
        while (err_mem == MZ_OK);
    }

#ifdef HAVE_AES
    if (!skip_aes)
    {
        if ((file_info->flag & MZ_ZIP_FLAG_ENCRYPTED) && (file_info->aes_version))
            extrafield_size += 4 + 7;
    }
#endif
    // NTFS timestamps
    if ((file_info->modified_date != 0) &&
        (file_info->accessed_date != 0) &&
        (file_info->creation_date != 0))
    {
        extrafield_ntfs_size += 8 + 8 + 8 + 4 + 2 + 2;
        extrafield_size += 4;
        extrafield_size += extrafield_ntfs_size;
    }

    if (local)
        err = mz_stream_write_uint32(stream, MZ_ZIP_MAGIC_LOCALHEADER);
    else
    {
        err = mz_stream_write_uint32(stream, MZ_ZIP_MAGIC_CENTRALHEADER);
        if (err == MZ_OK)
            err = mz_stream_write_uint16(stream, file_info->version_madeby);
    }

    // Calculate version needed to extract
    if (err == MZ_OK)
    {
        version_needed = file_info->version_needed;
        if (version_needed == 0)
        {
            version_needed = 20;
            if (zip64)
                version_needed = 45;
#ifdef HAVE_AES
            if ((file_info->flag & MZ_ZIP_FLAG_ENCRYPTED) && (file_info->aes_version))
                version_needed = 51;
#endif
#ifdef HAVE_LZMA
            if (file_info->compression_method == MZ_COMPRESS_METHOD_LZMA)
                version_needed = 63;
#endif
        }
        err = mz_stream_write_uint16(stream, version_needed);
    }
    if (err == MZ_OK)
        err = mz_stream_write_uint16(stream, file_info->flag);
    if (err == MZ_OK)
    {
#ifdef HAVE_AES
        if (file_info->aes_version)
            err = mz_stream_write_uint16(stream, MZ_COMPRESS_METHOD_AES);
        else
#endif
            err = mz_stream_write_uint16(stream, file_info->compression_method);
    }
    if (err == MZ_OK)
    {
        if (file_info->modified_date != 0)
            dos_date = mz_zip_time_t_to_dos_date(file_info->modified_date);
        err = mz_stream_write_uint32(stream, dos_date);
    }

    if (err == MZ_OK)
        err = mz_stream_write_uint32(stream, file_info->crc); // crc
    if (err == MZ_OK)
    {
        if (file_info->compressed_size >= UINT32_MAX) // compr size
            err = mz_stream_write_uint32(stream, UINT32_MAX);
        else
            err = mz_stream_write_uint32(stream, (uint32_t)file_info->compressed_size);
    }
    if (err == MZ_OK)
    {
        if (file_info->uncompressed_size >= UINT32_MAX) // uncompr size
            err = mz_stream_write_uint32(stream, UINT32_MAX);
        else
            err = mz_stream_write_uint32(stream, (uint32_t)file_info->uncompressed_size);
    }

    filename_length = (uint16_t)strlen(file_info->filename);
    if (err == MZ_OK)
    {
        filename_size = filename_length;
        if (mz_zip_attrib_is_dir(file_info->external_fa, file_info->version_madeby) == MZ_OK)
        {
            if ((file_info->filename[filename_length - 1] == '/') ||
                (file_info->filename[filename_length - 1] == '\\'))
                filename_length -= 1;
            else
                filename_size += 1;
        }
        err = mz_stream_write_uint16(stream, filename_size);
    }
    if (err == MZ_OK)
        err = mz_stream_write_uint16(stream, extrafield_size);

    if (!local)
    {
        if (file_info->comment != NULL)
            comment_size = (uint16_t)strlen(file_info->comment);
        if (err == MZ_OK)
            err = mz_stream_write_uint16(stream, comment_size);
        if (err == MZ_OK)
            err = mz_stream_write_uint16(stream, (uint16_t)file_info->disk_number);
        if (err == MZ_OK)
            err = mz_stream_write_uint16(stream, file_info->internal_fa);
        if (err == MZ_OK)
            err = mz_stream_write_uint32(stream, file_info->external_fa);
        if (err == MZ_OK)
        {
            if (file_info->disk_offset >= UINT32_MAX)
                err = mz_stream_write_uint32(stream, UINT32_MAX);
            else
                err = mz_stream_write_uint32(stream, (uint32_t)file_info->disk_offset);
        }
    }

    if (err == MZ_OK)
    {
        if (mz_stream_write(stream, file_info->filename, filename_length) != filename_length)
            err = MZ_STREAM_ERROR;
        if (err == MZ_OK)
        {
            // Ensure that directories have a slash appended to them for compatibility
            if (mz_zip_attrib_is_dir(file_info->external_fa, file_info->version_madeby) == MZ_OK)
                err = mz_stream_write_uint8(stream, '/');
        }
    }

    if (file_info->extrafield_size > 0)
    {
        err_mem = mz_stream_mem_seek(extrafield_ms, 0, SEEK_SET);
        while (err == MZ_OK && err_mem == MZ_OK)
        {
            err_mem = mz_stream_read_uint16(extrafield_ms, &field_type);
            if (err_mem == MZ_OK)
                err_mem = mz_stream_read_uint16(extrafield_ms, &field_length);
            if (err_mem != MZ_OK)
                break;

            // Prefer our zip 64, ntfs extensions over incoming
            if (field_type == MZ_ZIP_EXTENSION_ZIP64 || field_type == MZ_ZIP_EXTENSION_NTFS)
            {
                err_mem = mz_stream_seek(extrafield_ms, field_length, SEEK_CUR);
                continue;
            }

            err = mz_stream_write_uint16(stream, field_type);
            if (err == MZ_OK)
                err = mz_stream_write_uint16(stream, field_length);
            if (err == MZ_OK)
                err = mz_stream_copy(stream, extrafield_ms, field_length);
        }

        mz_stream_mem_delete(&extrafield_ms);
    }

    // Write ZIP64 extra field
    if ((err == MZ_OK) && (zip64))
    {
        err = mz_stream_write_uint16(stream, MZ_ZIP_EXTENSION_ZIP64);
        if (err == MZ_OK)
            err = mz_stream_write_uint16(stream, extrafield_zip64_size);
        if ((err == MZ_OK) && (file_info->uncompressed_size >= UINT32_MAX))
            err = mz_stream_write_uint64(stream, file_info->uncompressed_size);
        if ((err == MZ_OK) && (file_info->compressed_size >= UINT32_MAX))
            err = mz_stream_write_uint64(stream, file_info->compressed_size);
        if ((err == MZ_OK) && (file_info->disk_offset >= UINT32_MAX))
            err = mz_stream_write_uint64(stream, file_info->disk_offset);
    }
    // Write NTFS extra field
    if ((err == MZ_OK) && (extrafield_ntfs_size > 0))
    {
        err = mz_stream_write_uint16(stream, MZ_ZIP_EXTENSION_NTFS);
        if (err == MZ_OK)
            err = mz_stream_write_uint16(stream, extrafield_ntfs_size);
        if (err == MZ_OK)
            err = mz_stream_write_uint32(stream, reserved);
        if (err == MZ_OK)
            err = mz_stream_write_uint16(stream, 0x01);
        if (err == MZ_OK)
            err = mz_stream_write_uint16(stream, extrafield_ntfs_size - 8);
        if (err == MZ_OK)
        {
            mz_zip_unix_to_ntfs_time(file_info->modified_date, &ntfs_time);
            err = mz_stream_write_uint64(stream, ntfs_time);
        }
        if (err == MZ_OK)
        {
            mz_zip_unix_to_ntfs_time(file_info->accessed_date, &ntfs_time);
            err = mz_stream_write_uint64(stream, ntfs_time);
        }
        if (err == MZ_OK)
        {
            mz_zip_unix_to_ntfs_time(file_info->creation_date, &ntfs_time);
            err = mz_stream_write_uint64(stream, ntfs_time);
        }
    }
#ifdef HAVE_AES
    // Write AES extra field
    if ((err == MZ_OK) && (!skip_aes) && (file_info->flag & MZ_ZIP_FLAG_ENCRYPTED) && (file_info->aes_version))
    {
        err = mz_stream_write_uint16(stream, MZ_ZIP_EXTENSION_AES);
        if (err == MZ_OK)
            err = mz_stream_write_uint16(stream, 7);
        if (err == MZ_OK)
            err = mz_stream_write_uint16(stream, file_info->aes_version);
        if (err == MZ_OK)
            err = mz_stream_write_uint8(stream, 'A');
        if (err == MZ_OK)
            err = mz_stream_write_uint8(stream, 'E');
        if (err == MZ_OK)
            err = mz_stream_write_uint8(stream, file_info->aes_encryption_mode);
        if (err == MZ_OK)
            err = mz_stream_write_uint16(stream, file_info->compression_method);
    }
#endif
    if ((err == MZ_OK) && (!local) && (file_info->comment != NULL))
    {
        if (mz_stream_write(stream, file_info->comment, file_info->comment_size) != file_info->comment_size)
            err = MZ_STREAM_ERROR;
    }

    return err;
}

static int32_t mz_zip_entry_open_int(void *handle, uint8_t raw, int16_t compress_level, const char *password)
{
    mz_zip *zip = (mz_zip *)handle;
    int64_t max_total_in = 0;
    int64_t header_size = 0;
    int64_t footer_size = 0;
    int32_t err = MZ_OK;
    uint8_t use_crypt = 0;

    if (zip == NULL)
        return MZ_PARAM_ERROR;

    switch (zip->file_info.compression_method)
    {
    case MZ_COMPRESS_METHOD_STORE:
    case MZ_COMPRESS_METHOD_DEFLATE:
#ifdef HAVE_BZIP2
    case MZ_COMPRESS_METHOD_BZIP2:
#endif
#if HAVE_LZMA
    case MZ_COMPRESS_METHOD_LZMA:
#endif
        err = MZ_OK;
        break;
    default:
        return MZ_SUPPORT_ERROR;
    }

    zip->entry_raw = raw;

    if ((zip->file_info.flag & MZ_ZIP_FLAG_ENCRYPTED) && (password != NULL))
    {
        if (zip->open_mode & MZ_OPEN_MODE_WRITE)
        {
            // Encrypt only when we are not trying to write raw and password is supplied.
            if (!zip->entry_raw)
                use_crypt = 1;
        }
        else if (zip->open_mode & MZ_OPEN_MODE_READ)
        {
            // Decrypt only when password is supplied. Don't error when password
            // is not supplied as we may want to read the raw encrypted data.
            use_crypt = 1;
        }
    }

    if ((err == MZ_OK) && (use_crypt))
    {
#ifdef HAVE_AES
        if (zip->file_info.aes_version)
        {
            mz_stream_aes_create(&zip->crypt_stream);
            mz_stream_aes_set_password(zip->crypt_stream, password);
            mz_stream_aes_set_encryption_mode(zip->crypt_stream, zip->file_info.aes_encryption_mode);
        }
        else
#endif
        {
#ifdef HAVE_PKCRYPT
            uint8_t verify1 = 0;
            uint8_t verify2 = 0;

            // Info-ZIP modification to ZipCrypto format:
            // If bit 3 of the general purpose bit flag is set, it uses high byte of 16-bit File Time.

            if (zip->file_info.flag & MZ_ZIP_FLAG_DATA_DESCRIPTOR)
            {
                uint32_t dos_date = 0;

                dos_date = mz_zip_time_t_to_dos_date(zip->file_info.modified_date);

                verify1 = (uint8_t)((dos_date >> 16) & 0xff);
                verify2 = (uint8_t)((dos_date >> 8) & 0xff);
            }
            else
            {
                verify1 = (uint8_t)((zip->file_info.crc >> 16) & 0xff);
                verify2 = (uint8_t)((zip->file_info.crc >> 24) & 0xff);
            }

            mz_stream_pkcrypt_create(&zip->crypt_stream);
            mz_stream_pkcrypt_set_password(zip->crypt_stream, password);
            mz_stream_pkcrypt_set_verify(zip->crypt_stream, verify1, verify2);
#endif
        }
    }

    if (err == MZ_OK)
    {
        if (zip->crypt_stream == NULL)
            mz_stream_raw_create(&zip->crypt_stream);

        mz_stream_set_base(zip->crypt_stream, zip->stream);

        err = mz_stream_open(zip->crypt_stream, NULL, zip->open_mode);
    }

    if (err == MZ_OK)
    {
        if (zip->entry_raw || zip->file_info.compression_method == MZ_COMPRESS_METHOD_STORE)
            mz_stream_raw_create(&zip->compress_stream);
#ifdef HAVE_ZLIB
        else if (zip->file_info.compression_method == MZ_COMPRESS_METHOD_DEFLATE)
            mz_stream_zlib_create(&zip->compress_stream);
#endif
#ifdef HAVE_BZIP2
        else if (zip->file_info.compression_method == MZ_COMPRESS_METHOD_BZIP2)
            mz_stream_bzip_create(&zip->compress_stream);
#endif
#ifdef HAVE_LZMA
        else if (zip->file_info.compression_method == MZ_COMPRESS_METHOD_LZMA)
            mz_stream_lzma_create(&zip->compress_stream);
#endif
        else
            err = MZ_PARAM_ERROR;
    }

    if (err == MZ_OK)
    {
        if (zip->open_mode & MZ_OPEN_MODE_WRITE)
        {
            mz_stream_set_prop_int64(zip->compress_stream, MZ_STREAM_PROP_COMPRESS_LEVEL, compress_level);
        }
        else
        {
            if (zip->entry_raw || zip->file_info.compression_method == MZ_COMPRESS_METHOD_STORE || zip->file_info.flag & MZ_ZIP_FLAG_ENCRYPTED)
            {
                max_total_in = zip->file_info.compressed_size;
                mz_stream_set_prop_int64(zip->crypt_stream, MZ_STREAM_PROP_TOTAL_IN_MAX, max_total_in);

                if (mz_stream_get_prop_int64(zip->crypt_stream, MZ_STREAM_PROP_HEADER_SIZE, &header_size) == MZ_OK)
                    max_total_in -= header_size;
                if (mz_stream_get_prop_int64(zip->crypt_stream, MZ_STREAM_PROP_FOOTER_SIZE, &footer_size) == MZ_OK)
                    max_total_in -= footer_size;

                mz_stream_set_prop_int64(zip->compress_stream, MZ_STREAM_PROP_TOTAL_IN_MAX, max_total_in);
            }
            if ((zip->file_info.compression_method == MZ_COMPRESS_METHOD_LZMA) && (zip->file_info.flag & MZ_ZIP_FLAG_LZMA_EOS_MARKER) == 0)
            {
                mz_stream_set_prop_int64(zip->compress_stream, MZ_STREAM_PROP_TOTAL_IN_MAX, zip->file_info.compressed_size);
                mz_stream_set_prop_int64(zip->compress_stream, MZ_STREAM_PROP_TOTAL_OUT_MAX, zip->file_info.uncompressed_size);
            }
        }

        mz_stream_set_base(zip->compress_stream, zip->crypt_stream);

        err = mz_stream_open(zip->compress_stream, NULL, zip->open_mode);
    }

    if (err == MZ_OK)
    {
        mz_stream_crc32_create(&zip->crc32_stream);
        mz_stream_set_base(zip->crc32_stream, zip->compress_stream);

        err = mz_stream_open(zip->crc32_stream, NULL, zip->open_mode);
    }

    if (err == MZ_OK)
    {
        zip->entry_opened = 1;
    }

    return err;
}

int32_t mz_zip_entry_is_open(void *handle)
{
    mz_zip *zip = (mz_zip *)handle;
    if (zip == NULL)
        return MZ_PARAM_ERROR;
    if (zip->entry_opened == 0)
        return MZ_EXIST_ERROR;
    return MZ_OK;
}

int32_t mz_zip_entry_is_dir(void *handle)
{
    mz_zip *zip = (mz_zip *)handle;
    int32_t filename_length = 0;

    if (zip == NULL)
        return MZ_PARAM_ERROR;
    if (zip->entry_scanned == 0)
        return MZ_PARAM_ERROR;
    if (mz_zip_attrib_is_dir(zip->file_info.external_fa, zip->file_info.version_madeby) == MZ_OK)
        return MZ_OK;

    filename_length = (int32_t )strlen(zip->file_info.filename);
    if (filename_length > 0)
    {
        if ((zip->file_info.filename[filename_length - 1] == '/') ||
            (zip->file_info.filename[filename_length - 1] == '\\'))
            return MZ_OK;
    }
    return MZ_EXIST_ERROR;
}

int32_t mz_zip_entry_read_open(void *handle, uint8_t raw, const char *password)
{
    mz_zip *zip = (mz_zip *)handle;
    int32_t err = MZ_OK;

#if defined(MZ_ZIP_NO_ENCRYPTION)
    if (password != NULL)
        return MZ_PARAM_ERROR;
#endif
    if (zip == NULL)
        return MZ_PARAM_ERROR;
    if ((zip->open_mode & MZ_OPEN_MODE_READ) == 0)
        return MZ_PARAM_ERROR;
    if (zip->entry_scanned == 0)
        return MZ_PARAM_ERROR;
    if ((zip->file_info.flag & MZ_ZIP_FLAG_ENCRYPTED) && (password == NULL) && (!raw))
        return MZ_PARAM_ERROR;

    if (zip->file_info.disk_number == zip->disk_number_with_cd)
        mz_stream_set_prop_int64(zip->stream, MZ_STREAM_PROP_DISK_NUMBER, -1);
    else
        mz_stream_set_prop_int64(zip->stream, MZ_STREAM_PROP_DISK_NUMBER, zip->file_info.disk_number);

    err = mz_stream_seek(zip->stream, zip->file_info.disk_offset + zip->disk_offset_shift, MZ_SEEK_SET);
    if (err == MZ_OK)
        err = mz_zip_entry_read_header(zip->stream, 1, &zip->local_file_info, zip->local_file_info_stream);

#ifdef MZ_ZIP_NO_DECOMPRESSION
    if (zip->file_info.compression_method != MZ_COMPRESS_METHOD_STORE)
        err = MZ_SUPPORT_ERROR;
#endif
    if (err == MZ_OK)
        err = mz_zip_entry_open_int(handle, raw, 0, password);

    return err;
}

int32_t mz_zip_entry_write_open(void *handle, const mz_zip_file *file_info, int16_t compress_level, uint8_t raw, const char *password)
{
    mz_zip *zip = (mz_zip *)handle;
    int64_t disk_number = 0;
    int32_t err = MZ_OK;

#if defined(MZ_ZIP_NO_ENCRYPTION)
    if (password != NULL)
        return MZ_PARAM_ERROR;
#endif
    if (zip == NULL || file_info == NULL || file_info->filename == NULL)
        return MZ_PARAM_ERROR;

    if (mz_zip_entry_is_open(handle) == MZ_OK)
    {
        err = mz_zip_entry_close(handle);
        if (err != MZ_OK)
            return err;
    }

    memcpy(&zip->file_info, file_info, sizeof(mz_zip_file));

    mz_stream_seek(zip->file_info_stream, 0, SEEK_SET);
    mz_stream_write(zip->file_info_stream, file_info, sizeof(mz_zip_file));

    // Copy filename, extrafield, and comment internally
    if (file_info->filename != NULL)
    {
        mz_stream_mem_get_buffer_at_current(zip->file_info_stream, (const void **)&zip->file_info.filename);
        mz_stream_write_chars(zip->file_info_stream, file_info->filename, 1);
    }
    if (file_info->extrafield != NULL)
    {
        mz_stream_mem_get_buffer_at_current(zip->file_info_stream, (const void **)&zip->file_info.extrafield);
        mz_stream_write(zip->file_info_stream, file_info->extrafield, file_info->extrafield_size);
    }
    if (file_info->comment != NULL)
    {
        mz_stream_mem_get_buffer_at_current(zip->file_info_stream, (const void **)&zip->file_info.comment);
        mz_stream_write_chars(zip->file_info_stream, file_info->comment, 1);
    }

    if (zip->file_info.compression_method == MZ_COMPRESS_METHOD_DEFLATE)
    {
        if ((compress_level == 8) || (compress_level == 9))
            zip->file_info.flag |= MZ_ZIP_FLAG_DEFLATE_MAX;
        if (compress_level == 2)
            zip->file_info.flag |= MZ_ZIP_FLAG_DEFLATE_FAST;
        if (compress_level == 1)
            zip->file_info.flag |= MZ_ZIP_FLAG_DEFLATE_SUPER_FAST;
    }
#ifdef HAVE_LZMA
    else if (zip->file_info.compression_method == MZ_COMPRESS_METHOD_LZMA)
        zip->file_info.flag |= MZ_ZIP_FLAG_LZMA_EOS_MARKER;
#endif

    zip->file_info.flag |= MZ_ZIP_FLAG_DATA_DESCRIPTOR;

    if (password != NULL)
        zip->file_info.flag |= MZ_ZIP_FLAG_ENCRYPTED;

    mz_stream_get_prop_int64(zip->stream, MZ_STREAM_PROP_DISK_NUMBER, &disk_number);
    zip->file_info.disk_number = (uint32_t)disk_number;

    zip->file_info.disk_offset = mz_stream_tell(zip->stream);
    zip->file_info.crc = 0;
    zip->file_info.compressed_size = 0;

#ifdef HAVE_AES
    if (zip->file_info.aes_version && zip->file_info.aes_encryption_mode == 0)
        zip->file_info.aes_encryption_mode = MZ_AES_ENCRYPTION_MODE_256;
#endif

    if ((compress_level == 0) || (mz_zip_attrib_is_dir(zip->file_info.external_fa, zip->file_info.version_madeby) == MZ_OK))
        zip->file_info.compression_method = MZ_COMPRESS_METHOD_STORE;

#ifdef MZ_ZIP_NO_COMPRESSION
    if (zip->file_info.compression_method != MZ_COMPRESS_METHOD_STORE)
        err = MZ_SUPPORT_ERROR;
#endif
    if (err == MZ_OK)
        err = mz_zip_entry_write_header(zip->stream, 1, &zip->file_info);
    if (err == MZ_OK)
        err = mz_zip_entry_open_int(handle, raw, compress_level, password);

    return err;
}

int32_t mz_zip_entry_read(void *handle, void *buf, int32_t len)
{
    mz_zip *zip = (mz_zip *)handle;
    int32_t read = 0;

    if (zip == NULL || mz_zip_entry_is_open(handle) != MZ_OK)
        return MZ_PARAM_ERROR;
    if (UINT_MAX == UINT16_MAX && len > UINT16_MAX) // zlib limitation
        return MZ_PARAM_ERROR;
    if (len == 0)
        return MZ_PARAM_ERROR;

    if (zip->file_info.compressed_size == 0)
        return 0;

    // Read entire entry even if uncompressed_size = 0, otherwise
    // aes encryption validation will fail if compressed_size > 0
    read = mz_stream_read(zip->crc32_stream, buf, len);
    return read;
}

int32_t mz_zip_entry_write(void *handle, const void *buf, int32_t len)
{
    mz_zip *zip = (mz_zip *)handle;
    int32_t written = 0;

    if (zip == NULL || mz_zip_entry_is_open(handle) != MZ_OK)
        return MZ_PARAM_ERROR;
    written = mz_stream_write(zip->crc32_stream, buf, len);
    return written;
}

int32_t mz_zip_entry_get_info(void *handle, mz_zip_file **file_info)
{
    mz_zip *zip = (mz_zip *)handle;
    if (zip == NULL || !zip->entry_scanned)
        return MZ_PARAM_ERROR;
    *file_info = &zip->file_info;
    return MZ_OK;
}

int32_t mz_zip_entry_get_local_info(void *handle, mz_zip_file **local_file_info)
{
    mz_zip *zip = (mz_zip *)handle;
    if (zip == NULL || mz_zip_entry_is_open(handle) != MZ_OK)
        return MZ_PARAM_ERROR;
    *local_file_info = &zip->local_file_info;
    return MZ_OK;
}

int32_t mz_zip_entry_close(void *handle)
{
    return mz_zip_entry_close_raw(handle, 0, 0);
}

int32_t mz_zip_entry_close_raw(void *handle, uint64_t uncompressed_size, uint32_t crc32)
{
    mz_zip *zip = (mz_zip *)handle;
    uint64_t compressed_size = 0;
    int64_t total_in = 0;
    int32_t err = MZ_OK;

    if (zip == NULL || mz_zip_entry_is_open(handle) != MZ_OK)
        return MZ_PARAM_ERROR;

    mz_stream_close(zip->compress_stream);

    if (!zip->entry_raw)
        crc32 = mz_stream_crc32_get_value(zip->crc32_stream);

    if ((zip->open_mode & MZ_OPEN_MODE_WRITE) == 0)
    {
#ifdef HAVE_AES
        // AES zip version AE-1 will expect a valid crc as well
        if (zip->file_info.aes_version <= 0x0001)
#endif
        {
            mz_stream_get_prop_int64(zip->crc32_stream, MZ_STREAM_PROP_TOTAL_IN, &total_in);
            // If entire entry was not read this will fail
            if ((total_in > 0) && (!zip->entry_raw))
            {
                if (crc32 != zip->file_info.crc)
                    err = MZ_CRC_ERROR;
            }
        }
    }

    mz_stream_get_prop_int64(zip->compress_stream, MZ_STREAM_PROP_TOTAL_OUT, (int64_t *)&compressed_size);
    if (!zip->entry_raw)
        mz_stream_get_prop_int64(zip->crc32_stream, MZ_STREAM_PROP_TOTAL_OUT, (int64_t *)&uncompressed_size);

    if (zip->file_info.flag & MZ_ZIP_FLAG_ENCRYPTED)
    {
        mz_stream_set_base(zip->crypt_stream, zip->stream);
        err = mz_stream_close(zip->crypt_stream);

        mz_stream_get_prop_int64(zip->crypt_stream, MZ_STREAM_PROP_TOTAL_OUT, (int64_t *)&compressed_size);
    }

    mz_stream_delete(&zip->crypt_stream);

    mz_stream_delete(&zip->compress_stream);
    mz_stream_crc32_delete(&zip->crc32_stream);

    if (zip->open_mode & MZ_OPEN_MODE_WRITE)
    {
        if (err == MZ_OK)
        {
            err = mz_stream_write_uint32(zip->stream, MZ_ZIP_MAGIC_DATADESCRIPTOR);
            if (err == MZ_OK)
                err = mz_stream_write_uint32(zip->stream, crc32);
            // Store data descriptor as 8 bytes if zip 64 extension enabled
            if (err == MZ_OK)
            {
                // Zip 64 extension is enabled when uncompressed size is > UINT32_mAX
                if (zip->file_info.uncompressed_size <= UINT32_MAX)
                    err = mz_stream_write_uint32(zip->stream, (uint32_t)compressed_size);
                else
                    err = mz_stream_write_uint64(zip->stream, compressed_size);
            }
            if (err == MZ_OK)
            {
                if (zip->file_info.uncompressed_size <= UINT32_MAX)
                    err = mz_stream_write_uint32(zip->stream, (uint32_t)uncompressed_size);
                else
                    err = mz_stream_write_uint64(zip->stream, uncompressed_size);
            }
        }

        zip->file_info.crc = crc32;
        zip->file_info.compressed_size = compressed_size;
        zip->file_info.uncompressed_size = uncompressed_size;

        if (err == MZ_OK)
            err = mz_zip_entry_write_header(zip->cd_mem_stream, 0, &zip->file_info);

        zip->number_entry += 1;
    }

    zip->entry_opened = 0;

    return err;
}

static int32_t mz_zip_goto_next_entry_int(void *handle)
{
    mz_zip *zip = (mz_zip *)handle;
    int32_t err = MZ_OK;

    if (zip == NULL)
        return MZ_PARAM_ERROR;

    zip->entry_scanned = 0;

    mz_stream_set_prop_int64(zip->cd_stream, MZ_STREAM_PROP_DISK_NUMBER, -1);

    err = mz_stream_seek(zip->cd_stream, zip->cd_current_pos, MZ_SEEK_SET);
    if (err == MZ_OK)
        err = mz_zip_entry_read_header(zip->cd_stream, 0, &zip->file_info, zip->file_info_stream);
    if (err == MZ_OK)
        zip->entry_scanned = 1;
    return err;
}

int32_t mz_zip_get_number_entry(void *handle, int64_t *number_entry)
{
    mz_zip *zip = (mz_zip *)handle;
    if (zip == NULL || number_entry == NULL)
        return MZ_PARAM_ERROR;
    *number_entry = zip->number_entry;
    return MZ_OK;
}

int32_t mz_zip_get_disk_number_with_cd(void *handle, uint32_t *disk_number_with_cd)
{
    mz_zip *zip = (mz_zip *)handle;
    if (zip == NULL || disk_number_with_cd == NULL)
        return MZ_PARAM_ERROR;
    *disk_number_with_cd = zip->disk_number_with_cd;
    return MZ_OK;
}

uint64_t mz_zip_get_entry(void *handle)
{
    mz_zip *zip = (mz_zip *)handle;

    if (zip == NULL)
        return MZ_PARAM_ERROR;

    return zip->cd_current_pos;
}

int32_t mz_zip_goto_entry(void *handle, uint64_t cd_pos)
{
    mz_zip *zip = (mz_zip *)handle;

    if (zip == NULL)
        return MZ_PARAM_ERROR;

    if (cd_pos < zip->cd_start_pos || cd_pos > zip->cd_start_pos + zip->cd_size)
        return MZ_PARAM_ERROR;

    zip->cd_current_pos = cd_pos;

    return mz_zip_goto_next_entry_int(handle);
}

int32_t mz_zip_goto_first_entry(void *handle)
{
    mz_zip *zip = (mz_zip *)handle;

    if (zip == NULL)
        return MZ_PARAM_ERROR;

    zip->cd_current_pos = zip->cd_start_pos;

    return mz_zip_goto_next_entry_int(handle);
}

int32_t mz_zip_goto_next_entry(void *handle)
{
    mz_zip *zip = (mz_zip *)handle;

    if (zip == NULL)
        return MZ_PARAM_ERROR;

    zip->cd_current_pos += (uint64_t)MZ_ZIP_SIZE_CD_ITEM + zip->file_info.filename_size +
        zip->file_info.extrafield_size + zip->file_info.comment_size;

    return mz_zip_goto_next_entry_int(handle);
}

int32_t mz_zip_locate_entry(void *handle, const char *filename, uint8_t ignore_case)
{
    mz_zip *zip = (mz_zip *)handle;
    int32_t err = MZ_OK;
    int32_t result = 0;

    if (zip == NULL || filename == NULL)
        return MZ_PARAM_ERROR;

    // If we are already on the current entry, no need to search
    if ((zip->entry_scanned) && (zip->file_info.filename != NULL))
    {
        result = mz_zip_path_compare(zip->file_info.filename, filename, ignore_case);
        if (result == 0)
            return MZ_OK;
    }

    // Search all entries starting at the first
    err = mz_zip_goto_first_entry(handle);
    while (err == MZ_OK)
    {
        result = mz_zip_path_compare(zip->file_info.filename, filename, ignore_case);
        if (result == 0)
            return MZ_OK;

        err = mz_zip_goto_next_entry(handle);
    }

    return err;
}

int32_t mz_zip_locate_first_entry(void *handle, void *userdata, mz_zip_locate_entry_cb cb)
{
    mz_zip *zip = (mz_zip *)handle;
    int32_t err = MZ_OK;
    int32_t result = 0;

    // Search first entry looking for match
    err = mz_zip_goto_first_entry(handle);
    if (err != MZ_OK)
        return err;

    result = cb(handle, userdata, &zip->file_info);
    if (result == 0)
        return MZ_OK;

    return mz_zip_locate_next_entry(handle, userdata, cb);
}

int32_t mz_zip_locate_next_entry(void *handle, void *userdata, mz_zip_locate_entry_cb cb)
{
    mz_zip *zip = (mz_zip *)handle;
    int32_t err = MZ_OK;
    int32_t result = 0;

    // Search next entries looking for match
    err = mz_zip_goto_next_entry(handle);
    while (err == MZ_OK)
    {
        result = cb(handle, userdata, &zip->file_info);
        if (result == 0)
            return MZ_OK;

        err = mz_zip_goto_next_entry(handle);
    }

    return err;
}

/***************************************************************************/

int32_t mz_zip_attrib_is_dir(uint32_t attrib, uint16_t version_madeby)
{
    int32_t system = MZ_HOST_SYSTEM(version_madeby);
    int32_t posix_attrib = 0;
    int32_t err = MZ_OK;

    err = mz_zip_attrib_convert(system, attrib, MZ_HOST_SYSTEM_UNIX, &posix_attrib);
    if (err == MZ_OK)
    {
        if ((posix_attrib & 00170000) == 0040000) // S_ISDIR
            return MZ_OK;
    }

    return MZ_EXIST_ERROR;
}

int32_t mz_zip_attrib_convert(uint8_t src_sys, int32_t src_attrib, uint8_t target_sys, int32_t *target_attrib)
{
    if (target_attrib == NULL)
        return MZ_PARAM_ERROR;

    *target_attrib = 0;

    if (src_sys == MZ_HOST_SYSTEM_MSDOS || src_sys == MZ_HOST_SYSTEM_WINDOWS_NTFS)
    {
        if (target_sys == MZ_HOST_SYSTEM_MSDOS || target_sys == MZ_HOST_SYSTEM_WINDOWS_NTFS)
        {
            *target_attrib = src_attrib;
            return MZ_OK;
        }
        if (target_sys == MZ_HOST_SYSTEM_UNIX || target_sys == MZ_HOST_SYSTEM_OSX_DARWIN)
            return mz_zip_attrib_win32_to_posix(src_attrib, target_attrib);
    }
    else if (src_sys == MZ_HOST_SYSTEM_UNIX || src_sys == MZ_HOST_SYSTEM_OSX_DARWIN)
    {
        if (target_sys == MZ_HOST_SYSTEM_UNIX || target_sys == MZ_HOST_SYSTEM_OSX_DARWIN)
        {
            *target_attrib = src_attrib;
            return MZ_OK;
        }
        if (target_sys == MZ_HOST_SYSTEM_MSDOS || target_sys == MZ_HOST_SYSTEM_WINDOWS_NTFS)
            return mz_zip_attrib_posix_to_win32(src_attrib, target_attrib);
    }

    return MZ_SUPPORT_ERROR;
}

int32_t mz_zip_attrib_posix_to_win32(int32_t posix_attrib, int32_t *win32_attrib)
{
    if (win32_attrib == NULL)
        return MZ_PARAM_ERROR;

    // S_IWUSR | S_IWGRP | S_IWOTH | S_IXUSR | S_IXGRP | S_IXOTH
    if ((posix_attrib & 0000333) == 0 && (posix_attrib & 0000444) != 0)
        *win32_attrib = 0x01;       // FILE_ATTRIBUTE_READONLY
    // S_IFDIR
    if ((posix_attrib & 0040000) == 0040000)
        *win32_attrib |= 0x10;      // FILE_ATTRIBUTE_DIRECTORY
    // S_ISLNK
    else if ((posix_attrib & 0120000) == 0120000)
        *win32_attrib |= 0x400;     // FILE_ATTRIBUTE_REPARSE_POINT
    // S_IFREG
    else
        *win32_attrib |= 0x80;      // FILE_ATTRIBUTE_NORMAL

    return MZ_OK;
}

int32_t mz_zip_attrib_win32_to_posix(int32_t win32_attrib, int32_t *posix_attrib)
{
    if (posix_attrib == NULL)
        return MZ_PARAM_ERROR;

    *posix_attrib = 0000444;        // S_IRUSR | S_IRGRP | S_IROTH
    // FILE_ATTRIBUTE_READONLY
    if ((win32_attrib & 0x01) == 0)
        *posix_attrib |= 0000222;   // S_IWUSR | S_IWGRP | S_IWOTH
    // FILE_ATTRIBUTE_DIRECTORY
    if ((win32_attrib & 0x10) == 0x10)
        *posix_attrib |= 0040111;   // S_IFDIR | S_IXUSR | S_IXGRP | S_IXOTH
    // FILE_ATTRIBUTE_REPARSE_POINT
    else if ((win32_attrib & 0x400) == 0x400)
        *posix_attrib |= 0120000;   // S_ISLNK
    else
        *posix_attrib |= 0100000;   // S_IFREG

    return MZ_OK;
}

/***************************************************************************/

static int32_t mz_zip_invalid_date(const struct tm *ptm)
{
#define datevalue_in_range(min, max, value) ((min) <= (value) && (value) <= (max))
    return (!datevalue_in_range(0, 127 + 80, ptm->tm_year) ||  // 1980-based year, allow 80 extra
            !datevalue_in_range(0, 11, ptm->tm_mon) ||
            !datevalue_in_range(1, 31, ptm->tm_mday) ||
            !datevalue_in_range(0, 23, ptm->tm_hour) ||
            !datevalue_in_range(0, 59, ptm->tm_min) ||
            !datevalue_in_range(0, 59, ptm->tm_sec));
#undef datevalue_in_range
}

static void mz_zip_dosdate_to_raw_tm(uint64_t dos_date, struct tm *ptm)
{
    uint64_t date = (uint64_t)(dos_date >> 16);

    ptm->tm_mday = (uint16_t)(date & 0x1f);
    ptm->tm_mon = (uint16_t)(((date & 0x1E0) / 0x20) - 1);
    ptm->tm_year = (uint16_t)(((date & 0x0FE00) / 0x0200) + 80);
    ptm->tm_hour = (uint16_t)((dos_date & 0xF800) / 0x800);
    ptm->tm_min = (uint16_t)((dos_date & 0x7E0) / 0x20);
    ptm->tm_sec = (uint16_t)(2 * (dos_date & 0x1f));
    ptm->tm_isdst = -1;
}

int32_t mz_zip_dosdate_to_tm(uint64_t dos_date, struct tm *ptm)
{
    if (ptm == NULL)
        return MZ_PARAM_ERROR;

    mz_zip_dosdate_to_raw_tm(dos_date, ptm);

    if (mz_zip_invalid_date(ptm))
    {
        // Invalid date stored, so don't return it
        memset(ptm, 0, sizeof(struct tm));
        return MZ_FORMAT_ERROR;
    }
    return MZ_OK;
}

time_t mz_zip_dosdate_to_time_t(uint64_t dos_date)
{
    struct tm ptm;
    mz_zip_dosdate_to_raw_tm(dos_date, &ptm);
    return mktime(&ptm);
}

int32_t mz_zip_time_t_to_tm(time_t unix_time, struct tm *ptm)
{
    struct tm *ltm = NULL;
    if (ptm == NULL)
        return MZ_PARAM_ERROR;
    ltm = localtime(&unix_time);  // Returns a 1900-based year
    if (ltm == NULL)
    {
        // Invalid date stored, so don't return it
        memset(ptm, 0, sizeof(struct tm));
        return MZ_INTERNAL_ERROR;
    }
    memcpy(ptm, ltm, sizeof(struct tm));
    return MZ_OK;
}

uint32_t mz_zip_time_t_to_dos_date(time_t unix_time)
{
    struct tm ptm;
    mz_zip_time_t_to_tm(unix_time, &ptm);
    return mz_zip_tm_to_dosdate((const struct tm *)&ptm);
}

uint32_t mz_zip_tm_to_dosdate(const struct tm *ptm)
{
    struct tm fixed_tm;

    // Years supported:

    // [00, 79]      (assumed to be between 2000 and 2079)
    // [80, 207]     (assumed to be between 1980 and 2107, typical output of old
    //                software that does 'year-1900' to get a double digit year)
    // [1980, 2107]  (due to format limitations, only years 1980-2107 can be stored.)

    memcpy(&fixed_tm, ptm, sizeof(struct tm));
    if (fixed_tm.tm_year >= 1980) // range [1980, 2107]
        fixed_tm.tm_year -= 1980;
    else if (fixed_tm.tm_year >= 80) // range [80, 207]
        fixed_tm.tm_year -= 80;
    else // range [00, 79]
        fixed_tm.tm_year += 20;

    if (mz_zip_invalid_date(&fixed_tm))
        return 0;

    return (uint32_t)(((fixed_tm.tm_mday) + (32 * (fixed_tm.tm_mon + 1)) + (512 * fixed_tm.tm_year)) << 16) |
        ((fixed_tm.tm_sec / 2) + (32 * fixed_tm.tm_min) + (2048 * (uint32_t)fixed_tm.tm_hour));
}

int32_t mz_zip_ntfs_to_unix_time(uint64_t ntfs_time, time_t *unix_time)
{
    *unix_time = (time_t)((ntfs_time - 116444736000000000LL) / 10000000);
    return MZ_OK;
}

int32_t mz_zip_unix_to_ntfs_time(time_t unix_time, uint64_t *ntfs_time)
{
    *ntfs_time = ((uint64_t)unix_time * 10000000) + 116444736000000000LL;
    return MZ_OK;
}

/***************************************************************************/

int32_t mz_zip_path_compare(const char *path1, const char *path2, uint8_t ignore_case)
{
    do
    {
        if ((*path1 == '\\' && *path2 == '/') ||
            (*path2 == '\\' && *path1 == '/'))
        {
            // Ignore comparison of path slashes
        }
        else if (ignore_case)
        {
            if (tolower(*path1) != tolower(*path2))
                break;
        }
        else if (*path1 != *path2)
        {
            break;
        }

        path1 += 1;
        path2 += 1;
    }
    while (*path1 != 0 && *path2 != 0);

    if (ignore_case)
        return (int32_t)(tolower(*path1) - tolower(*path2));

    return (int32_t)(*path1 - *path2);
}

/***************************************************************************/
