/* EasyTAG - tag editor for audio files
 * Copyright (C) 2014  David King <amigadave@amigadave.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "file_tag.h"

#include "et_core.h"

static void
ET_Initialize_File_Tag_Item (File_Tag *FileTag)
{
    if (FileTag)
    {
        FileTag->key         = ET_Undo_Key_New();
        FileTag->saved       = FALSE;
        FileTag->title       = NULL;
        FileTag->artist      = NULL;
        FileTag->album_artist= NULL;
        FileTag->album       = NULL;
        FileTag->disc_number = NULL;
        FileTag->disc_total = NULL;
        FileTag->track       = NULL;
        FileTag->track_total = NULL;
        FileTag->year        = NULL;
        FileTag->genre       = NULL;
        FileTag->comment     = NULL;
        FileTag->composer    = NULL;
        FileTag->orig_artist = NULL;
        FileTag->copyright   = NULL;
        FileTag->url         = NULL;
        FileTag->encoded_by  = NULL;
        FileTag->picture     = NULL;
        FileTag->other       = NULL;
    }
}

/*
 * Create a new File_Tag structure
 */
File_Tag *ET_File_Tag_Item_New (void)
{
    File_Tag *FileTag;

    FileTag = g_slice_new (File_Tag);
    ET_Initialize_File_Tag_Item (FileTag);

    return FileTag;
}

/*
 * Frees the list of 'other' field in a File_Tag item (contains attached gchar data).
 */
static gboolean
ET_Free_File_Tag_Item_Other_Field (File_Tag *FileTag)
{
    g_list_free_full (FileTag->other, g_free);

    return TRUE;
}


/*
 * Frees a File_Tag item.
 */
gboolean ET_Free_File_Tag_Item (File_Tag *FileTag)
{
    g_return_val_if_fail (FileTag != NULL, FALSE);

    g_free(FileTag->title);
    g_free(FileTag->artist);
    g_free(FileTag->album_artist);
    g_free(FileTag->album);
    g_free(FileTag->disc_number);
    g_free (FileTag->disc_total);
    g_free(FileTag->year);
    g_free(FileTag->track);
    g_free(FileTag->track_total);
    g_free(FileTag->genre);
    g_free(FileTag->comment);
    g_free(FileTag->composer);
    g_free(FileTag->orig_artist);
    g_free(FileTag->copyright);
    g_free(FileTag->url);
    g_free(FileTag->encoded_by);
    et_file_tag_set_picture (FileTag, NULL);
    // Free list of other fields
    ET_Free_File_Tag_Item_Other_Field(FileTag);

    g_slice_free (File_Tag, FileTag);
    return TRUE;
}

/*
 * Duplicate the 'other' list
 */
void
ET_Copy_File_Tag_Item_Other_Field (const ET_File *ETFile,
                                   File_Tag *FileTag)
{
    const File_Tag *FileTagCur;
    GList *l;

    FileTagCur = (File_Tag *)(ETFile->FileTag)->data;

    for (l = FileTagCur->other; l != NULL; l = g_list_next (l))
    {
        FileTag->other = g_list_prepend (FileTag->other,
                                         g_strdup ((gchar *)l->data));
    }

    FileTag->other = g_list_reverse (FileTag->other);
}


/*
 * Copy data of the File_Tag structure (of ETFile) to the FileTag item.
 * Reallocate data if not null.
 */
gboolean
ET_Copy_File_Tag_Item (const ET_File *ETFile, File_Tag *FileTag)
{
    const File_Tag *FileTagCur;

    g_return_val_if_fail (ETFile != NULL && ETFile->FileTag != NULL &&
                          (File_Tag *)(ETFile->FileTag)->data != NULL, FALSE);
    g_return_val_if_fail (FileTag != NULL, FALSE);

    /* The data to duplicate to FileTag */
    FileTagCur = (File_Tag *)(ETFile->FileTag)->data;
    // Key for the item, may be overwritten
    FileTag->key = ET_Undo_Key_New();

    if (FileTagCur->title)
    {
        FileTag->title = g_strdup(FileTagCur->title);
    }else
    {
        g_free(FileTag->title);
        FileTag->title = NULL;
    }

    if (FileTagCur->artist)
    {
        FileTag->artist = g_strdup(FileTagCur->artist);
    }else
    {
        g_free(FileTag->artist);
        FileTag->artist = NULL;
    }

    if (FileTagCur->album_artist)
    {
        FileTag->album_artist = g_strdup(FileTagCur->album_artist);
    }else
    {
        g_free(FileTag->album_artist);
        FileTag->album_artist = NULL;
    }

    if (FileTagCur->album)
    {
        FileTag->album = g_strdup(FileTagCur->album);
    }else
    {
        g_free(FileTag->album);
        FileTag->album = NULL;
    }

    if (FileTagCur->disc_number)
    {
        FileTag->disc_number = g_strdup(FileTagCur->disc_number);
    }else
    {
        g_free(FileTag->disc_number);
        FileTag->disc_number = NULL;
    }

    if (FileTagCur->disc_total)
    {
        FileTag->disc_total = g_strdup (FileTagCur->disc_total);
    }
    else
    {
        g_free (FileTag->disc_total);
        FileTag->disc_total = NULL;
    }

    if (FileTagCur->year)
    {
        FileTag->year = g_strdup(FileTagCur->year);
    }else
    {
        g_free(FileTag->year);
        FileTag->year = NULL;
    }

    if (FileTagCur->track)
    {
        FileTag->track = g_strdup(FileTagCur->track);
    }else
    {
        g_free(FileTag->track);
        FileTag->track = NULL;
    }

    if (FileTagCur->track_total)
    {
        FileTag->track_total = g_strdup(FileTagCur->track_total);
    }else
    {
        g_free(FileTag->track_total);
        FileTag->track_total = NULL;
    }

    if (FileTagCur->genre)
    {
        FileTag->genre = g_strdup(FileTagCur->genre);
    }else
    {
        g_free(FileTag->genre);
        FileTag->genre = NULL;
    }

    if (FileTagCur->comment)
    {
        FileTag->comment = g_strdup(FileTagCur->comment);
    }else
    {
        g_free(FileTag->comment);
        FileTag->comment = NULL;
    }

    if (FileTagCur->composer)
    {
        FileTag->composer = g_strdup(FileTagCur->composer);
    }else
    {
        g_free(FileTag->composer);
        FileTag->composer = NULL;
    }

    if (FileTagCur->orig_artist)
    {
        FileTag->orig_artist = g_strdup(FileTagCur->orig_artist);
    }else
    {
        g_free(FileTag->orig_artist);
        FileTag->orig_artist = NULL;
    }

    if (FileTagCur->copyright)
    {
        FileTag->copyright = g_strdup(FileTagCur->copyright);
    }else
    {
        g_free(FileTag->copyright);
        FileTag->copyright = NULL;
    }

    if (FileTagCur->url)
    {
        FileTag->url = g_strdup(FileTagCur->url);
    }else
    {
        g_free(FileTag->url);
        FileTag->url = NULL;
    }

    if (FileTagCur->encoded_by)
    {
        FileTag->encoded_by = g_strdup(FileTagCur->encoded_by);
    }else
    {
        g_free(FileTag->encoded_by);
        FileTag->encoded_by = NULL;
    }

    et_file_tag_set_picture (FileTag, FileTagCur->picture);

    if (FileTagCur->other)
    {
        ET_Copy_File_Tag_Item_Other_Field(ETFile,FileTag);
    }else
    {
        ET_Free_File_Tag_Item_Other_Field (FileTag);
    }

    return TRUE;
}

/*
 * Set the value of a field of a FileTag item (for ex, value of FileTag->title)
 * Must be used only for the 'gchar *' components
 */
static void
ET_Set_Field_File_Tag_Item (gchar **FileTagField,
                            const gchar *value)
{
    g_return_if_fail (FileTagField != NULL);

    if (*FileTagField != NULL)
    {
        g_free (*FileTagField);
        *FileTagField = NULL;
    }

    if (value != NULL)
    {
        if (*value != '\0')
        {
            *FileTagField = g_strdup (value);
        }
    }
}

void
et_file_tag_set_title (File_Tag *file_tag,
                       const gchar *title)
{
    g_return_if_fail (file_tag != NULL);

    ET_Set_Field_File_Tag_Item (&file_tag->title, title);
}

void
et_file_tag_set_artist (File_Tag *file_tag,
                        const gchar *artist)
{
    g_return_if_fail (file_tag != NULL);

    ET_Set_Field_File_Tag_Item (&file_tag->artist, artist);
}

void
et_file_tag_set_album_artist (File_Tag *file_tag,
                              const gchar *album_artist)
{
    g_return_if_fail (file_tag != NULL);

    ET_Set_Field_File_Tag_Item (&file_tag->album_artist, album_artist);
}

void
et_file_tag_set_album (File_Tag *file_tag,
                       const gchar *album)
{
    g_return_if_fail (file_tag != NULL);

    ET_Set_Field_File_Tag_Item (&file_tag->album, album);
}

void
et_file_tag_set_disc_number (File_Tag *file_tag,
                             const gchar *disc_number)
{
    g_return_if_fail (file_tag != NULL);

    ET_Set_Field_File_Tag_Item (&file_tag->disc_number, disc_number);
}

void
et_file_tag_set_disc_total (File_Tag *file_tag,
                            const gchar *disc_total)
{
    g_return_if_fail (file_tag != NULL);

    ET_Set_Field_File_Tag_Item (&file_tag->disc_total, disc_total);
}

void
et_file_tag_set_year (File_Tag *file_tag,
                      const gchar *year)
{
    g_return_if_fail (file_tag != NULL);

    ET_Set_Field_File_Tag_Item (&file_tag->year, year);
}

void
et_file_tag_set_track_number (File_Tag *file_tag,
                              const gchar *track_number)
{
    g_return_if_fail (file_tag != NULL);

    ET_Set_Field_File_Tag_Item (&file_tag->track, track_number);
}

void
et_file_tag_set_track_total (File_Tag *file_tag,
                             const gchar *track_total)
{
    g_return_if_fail (file_tag != NULL);

    ET_Set_Field_File_Tag_Item (&file_tag->track_total, track_total);
}

void
et_file_tag_set_genre (File_Tag *file_tag,
                       const gchar *genre)
{
    g_return_if_fail (file_tag != NULL);

    ET_Set_Field_File_Tag_Item (&file_tag->genre, genre);
}

void
et_file_tag_set_comment (File_Tag *file_tag,
                         const gchar *comment)
{
    g_return_if_fail (file_tag != NULL);

    ET_Set_Field_File_Tag_Item (&file_tag->comment, comment);
}

void
et_file_tag_set_composer (File_Tag *file_tag,
                          const gchar *composer)
{
    g_return_if_fail (file_tag != NULL);

    ET_Set_Field_File_Tag_Item (&file_tag->composer, composer);
}

void
et_file_tag_set_orig_artist (File_Tag *file_tag,
                             const gchar *orig_artist)
{
    g_return_if_fail (file_tag != NULL);

    ET_Set_Field_File_Tag_Item (&file_tag->orig_artist, orig_artist);
}

void
et_file_tag_set_copyright (File_Tag *file_tag,
                           const gchar *copyright)
{
    g_return_if_fail (file_tag != NULL);

    ET_Set_Field_File_Tag_Item (&file_tag->copyright, copyright);
}

void
et_file_tag_set_url (File_Tag *file_tag,
                     const gchar *url)
{
    g_return_if_fail (file_tag != NULL);

    ET_Set_Field_File_Tag_Item (&file_tag->url, url);
}

void
et_file_tag_set_encoded_by (File_Tag *file_tag,
                            const gchar *encoded_by)
{
    g_return_if_fail (file_tag != NULL);

    ET_Set_Field_File_Tag_Item (&file_tag->encoded_by, encoded_by);
}

/*
 * et_file_tag_set_picture:
 * @file_tag: the #File_Tag on which to set the image
 * @pic: the image to set
 *
 * Set the images inside @file_tag to be @pic, freeing existing images as
 * necessary. Copies @pic with et_picture_copy_all().
 */
void
et_file_tag_set_picture (File_Tag *file_tag,
                         const EtPicture *pic)
{
    g_return_if_fail (file_tag != NULL);

    if (file_tag->picture != NULL)
    {
        et_picture_free (file_tag->picture);
        file_tag->picture = NULL;
    }

    if (pic)
    {
        file_tag->picture = et_picture_copy_all (pic);
    }
}

/*
 * Compares two File_Tag items and returns TRUE if there aren't the same.
 * Notes:
 *  - if field is '' or NULL => will be removed
 */
gboolean
ET_Detect_Changes_Of_File_Tag (const File_Tag *FileTag1,
                               const File_Tag *FileTag2)
{
    const EtPicture *pic1;
    const EtPicture *pic2;

    g_return_val_if_fail (FileTag1 != NULL && FileTag2 != NULL, FALSE);

    if ( ( FileTag1 && !FileTag2)
      || (!FileTag1 &&  FileTag2) )
        return TRUE;

    /* Title */
    if ( FileTag1->title && !FileTag2->title && g_utf8_strlen(FileTag1->title, -1)>0 ) return TRUE;
    if (!FileTag1->title &&  FileTag2->title && g_utf8_strlen(FileTag2->title, -1)>0 ) return TRUE;
    if ( FileTag1->title &&  FileTag2->title && g_utf8_collate(FileTag1->title,FileTag2->title)!=0 ) return TRUE;

    /* Artist */
    if ( FileTag1->artist && !FileTag2->artist && g_utf8_strlen(FileTag1->artist, -1)>0 ) return TRUE;
    if (!FileTag1->artist &&  FileTag2->artist && g_utf8_strlen(FileTag2->artist, -1)>0 ) return TRUE;
    if ( FileTag1->artist &&  FileTag2->artist && g_utf8_collate(FileTag1->artist,FileTag2->artist)!=0 ) return TRUE;

	/* Album Artist */
    if ( FileTag1->album_artist && !FileTag2->album_artist && g_utf8_strlen(FileTag1->album_artist, -1)>0 ) return TRUE;
    if (!FileTag1->album_artist &&  FileTag2->album_artist && g_utf8_strlen(FileTag2->album_artist, -1)>0 ) return TRUE;
    if ( FileTag1->album_artist &&  FileTag2->album_artist && g_utf8_collate(FileTag1->album_artist,FileTag2->album_artist)!=0 ) return TRUE;

    /* Album */
    if ( FileTag1->album && !FileTag2->album && g_utf8_strlen(FileTag1->album, -1)>0 ) return TRUE;
    if (!FileTag1->album &&  FileTag2->album && g_utf8_strlen(FileTag2->album, -1)>0 ) return TRUE;
    if ( FileTag1->album &&  FileTag2->album && g_utf8_collate(FileTag1->album,FileTag2->album)!=0 ) return TRUE;

    /* Disc Number */
    if ( FileTag1->disc_number && !FileTag2->disc_number && g_utf8_strlen(FileTag1->disc_number, -1)>0 ) return TRUE;
    if (!FileTag1->disc_number &&  FileTag2->disc_number && g_utf8_strlen(FileTag2->disc_number, -1)>0 ) return TRUE;
    if ( FileTag1->disc_number &&  FileTag2->disc_number && g_utf8_collate(FileTag1->disc_number,FileTag2->disc_number)!=0 ) return TRUE;

    /* Discs Total */
    if (FileTag1->disc_total && !FileTag2->disc_total
        && g_utf8_strlen (FileTag1->disc_total, -1) > 0)
    {
        return TRUE;
    }

    if (!FileTag1->disc_total &&  FileTag2->disc_total
        && g_utf8_strlen (FileTag2->disc_total, -1) > 0)
    {
        return TRUE;
    }

    if (FileTag1->disc_total &&  FileTag2->disc_total
        && g_utf8_collate (FileTag1->disc_total, FileTag2->disc_total) != 0)
    {
        return TRUE;
    }

    /* Year */
    if ( FileTag1->year && !FileTag2->year && g_utf8_strlen(FileTag1->year, -1)>0 ) return TRUE;
    if (!FileTag1->year &&  FileTag2->year && g_utf8_strlen(FileTag2->year, -1)>0 ) return TRUE;
    if ( FileTag1->year &&  FileTag2->year && g_utf8_collate(FileTag1->year,FileTag2->year)!=0 ) return TRUE;

    /* Track */
    if ( FileTag1->track && !FileTag2->track && g_utf8_strlen(FileTag1->track, -1)>0 ) return TRUE;
    if (!FileTag1->track &&  FileTag2->track && g_utf8_strlen(FileTag2->track, -1)>0 ) return TRUE;
    if ( FileTag1->track &&  FileTag2->track && g_utf8_collate(FileTag1->track,FileTag2->track)!=0 ) return TRUE;

    /* Track Total */
    if ( FileTag1->track_total && !FileTag2->track_total && g_utf8_strlen(FileTag1->track_total, -1)>0 ) return TRUE;
    if (!FileTag1->track_total &&  FileTag2->track_total && g_utf8_strlen(FileTag2->track_total, -1)>0 ) return TRUE;
    if ( FileTag1->track_total &&  FileTag2->track_total && g_utf8_collate(FileTag1->track_total,FileTag2->track_total)!=0 ) return TRUE;

    /* Genre */
    if ( FileTag1->genre && !FileTag2->genre && g_utf8_strlen(FileTag1->genre, -1)>0 ) return TRUE;
    if (!FileTag1->genre &&  FileTag2->genre && g_utf8_strlen(FileTag2->genre, -1)>0 ) return TRUE;
    if ( FileTag1->genre &&  FileTag2->genre && g_utf8_collate(FileTag1->genre,FileTag2->genre)!=0 ) return TRUE;

    /* Comment */
    if ( FileTag1->comment && !FileTag2->comment && g_utf8_strlen(FileTag1->comment, -1)>0 ) return TRUE;
    if (!FileTag1->comment &&  FileTag2->comment && g_utf8_strlen(FileTag2->comment, -1)>0 ) return TRUE;
    if ( FileTag1->comment &&  FileTag2->comment && g_utf8_collate(FileTag1->comment,FileTag2->comment)!=0 ) return TRUE;

    /* Composer */
    if ( FileTag1->composer && !FileTag2->composer && g_utf8_strlen(FileTag1->composer, -1)>0 ) return TRUE;
    if (!FileTag1->composer &&  FileTag2->composer && g_utf8_strlen(FileTag2->composer, -1)>0 ) return TRUE;
    if ( FileTag1->composer &&  FileTag2->composer && g_utf8_collate(FileTag1->composer,FileTag2->composer)!=0 ) return TRUE;

    /* Original artist */
    if ( FileTag1->orig_artist && !FileTag2->orig_artist && g_utf8_strlen(FileTag1->orig_artist, -1)>0 ) return TRUE;
    if (!FileTag1->orig_artist &&  FileTag2->orig_artist && g_utf8_strlen(FileTag2->orig_artist, -1)>0 ) return TRUE;
    if ( FileTag1->orig_artist &&  FileTag2->orig_artist && g_utf8_collate(FileTag1->orig_artist,FileTag2->orig_artist)!=0 ) return TRUE;

    /* Copyright */
    if ( FileTag1->copyright && !FileTag2->copyright && g_utf8_strlen(FileTag1->copyright, -1)>0 ) return TRUE;
    if (!FileTag1->copyright &&  FileTag2->copyright && g_utf8_strlen(FileTag2->copyright, -1)>0 ) return TRUE;
    if ( FileTag1->copyright &&  FileTag2->copyright && g_utf8_collate(FileTag1->copyright,FileTag2->copyright)!=0 ) return TRUE;

    /* URL */
    if ( FileTag1->url && !FileTag2->url && g_utf8_strlen(FileTag1->url, -1)>0 ) return TRUE;
    if (!FileTag1->url &&  FileTag2->url && g_utf8_strlen(FileTag2->url, -1)>0 ) return TRUE;
    if ( FileTag1->url &&  FileTag2->url && g_utf8_collate(FileTag1->url,FileTag2->url)!=0 ) return TRUE;

    /* Encoded by */
    if ( FileTag1->encoded_by && !FileTag2->encoded_by && g_utf8_strlen(FileTag1->encoded_by, -1)>0 ) return TRUE;
    if (!FileTag1->encoded_by &&  FileTag2->encoded_by && g_utf8_strlen(FileTag2->encoded_by, -1)>0 ) return TRUE;
    if ( FileTag1->encoded_by &&  FileTag2->encoded_by && g_utf8_collate(FileTag1->encoded_by,FileTag2->encoded_by)!=0 ) return TRUE;

    /* Picture */
    for (pic1 = FileTag1->picture, pic2 = FileTag2->picture; ;
         pic1 = pic1->next, pic2 = pic2->next)
    {
        if( (pic1 && !pic2) || (!pic1 && pic2) )
            return TRUE;
        if (!pic1 || !pic2)
            break; // => no changes
        //if (!pic1->data || !pic2->data)
        //    break; // => no changes

        if (!g_bytes_equal (pic1->bytes, pic2->bytes))
        {
            return TRUE;
        }
        if (pic1->type != pic2->type)
            return TRUE;
        if (pic1->description && !pic2->description
        &&  g_utf8_strlen(pic1->description, -1)>0 )
            return TRUE;
        if (!pic1->description && pic2->description
        &&  g_utf8_strlen(pic2->description, -1)>0 )
            return TRUE;
        if (pic1->description && pic2->description
        &&  g_utf8_collate(pic1->description, pic2->description)!=0 )
            return TRUE;
    }

    return FALSE; /* No changes */
}
