/* EasyTAG - Tag editor for audio files
 * Copyright (C) 2000-2003  Jerome Couderc <easytag@gmail.com>
 * Copyright (C) 2014  David King <amigadave@amigadave.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "config.h"

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gi18n.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include "win32/win32dep.h"
#ifndef G_OS_WIN32
#include <sys/socket.h>
/* Patch OpenBSD from Jim Geovedi. */
#include <netinet/in.h>
#include <arpa/inet.h>
/* End patch */
#include <netdb.h>
#endif /* !G_OS_WIN32 */
#include <errno.h>

#include "gtk2_compat.h"
#include "application_window.h"
#include "cddb_dialog.h"
#include "easytag.h"
#include "enums.h"
#include "et_core.h"
#include "browser.h"
#include "scan_dialog.h"
#include "log.h"
#include "misc.h"
#include "setting.h"
#include "id3_tag.h"
#include "setting.h"
#include "charset.h"

/* TODO: Use G_DEFINE_TYPE_WITH_PRIVATE. */
G_DEFINE_TYPE (EtCDDBDialog, et_cddb_dialog, GTK_TYPE_DIALOG)

#define et_cddb_dialog_get_instance_private(dialog) (dialog->priv)

struct _EtCDDBDialogPrivate
{
    GtkWidget *album_list_view;
    GtkWidget *track_list_view;

    GList *album_list;

    GtkListStore *album_list_model;
    GtkListStore *search_string_model;
    GtkListStore *search_string_in_result_model;
    GtkListStore *track_list_model;

    GtkWidget *search_string_entry;
    GtkWidget *search_string_in_results_entry;

    GtkWidget *apply_button;
    GtkWidget *search_button;
    GtkWidget *stop_search_button;
    GtkWidget *stop_auto_search_button;

    GtkWidget *display_red_lines_toggle;
    GtkWidget *show_categories_toggle;

    GtkWidget *status_bar;
    guint status_bar_context;

    gboolean stop_searching;

    GtkWidget *run_scanner_toggle;
    GtkWidget *use_dlm2_toggle; /* '2' as also used in prefs.c */

    GtkWidget *separator_h;
    GtkWidget *category_toggle[10];
};

/*
 * Structure used for each item of the album list. Aslo attached to each row of
 * the album list
 */
typedef struct
{
    gchar *server_name; /* Remote access: server name. Local access : NULL */
    guint server_port; /* Remote access: server port. Local access: 0 */
    gchar *server_cgi_path; /* Remote access: server CGI path.
                             * Local access: discid file path */

    GdkPixbuf *bitmap; /* Pixmap logo for the server. */

    gchar *artist_album; /* CDDB artist+album (allocated) */
    gchar *category; /* CDDB genre (allocated) */
    gchar *id; /* example : 8d0de30c (allocated) */
    GList *track_list; /* List of CddbTrackAlbum items. */
    gboolean other_version; /* TRUE if this album is another version of the
                             * previous one. */

    /* Filled when loading the track list. */
    gchar *artist; /* (allocated) */
    gchar *album; /* (allocated) */
    gchar *genre; /* (allocated) */
    gchar *year; /* (allocated) */
    guint duration;
} CddbAlbum;


/*
 * Structure used for each item of the track_list of the CddbAlbum structure.
 */
typedef struct
{
    guint track_number;
    gchar *track_name; /* (allocated) */
    guint duration;
    CddbAlbum *cddbalbum; /* Pointer to the parent CddbAlbum structure (to
                           * quickly access album properties). */
} CddbTrackAlbum;


typedef struct
{
    gulong offset;
} CddbTrackFrameOffset;

enum
{
    CDDB_ALBUM_LIST_PIXBUF,
    CDDB_ALBUM_LIST_ALBUM,
    CDDB_ALBUM_LIST_CATEGORY,
    CDDB_ALBUM_LIST_DATA,
    CDDB_ALBUM_LIST_FONT_STYLE,
    CDDB_ALBUM_LIST_FONT_WEIGHT,
    CDDB_ALBUM_LIST_FOREGROUND_COLOR,
    CDDB_ALBUM_LIST_COUNT
};

enum
{
    CDDB_TRACK_LIST_NUMBER,
    CDDB_TRACK_LIST_NAME,
    CDDB_TRACK_LIST_TIME,
    CDDB_TRACK_LIST_DATA,
    CDDB_TRACK_LIST_ETFILE,
    CDDB_TRACK_LIST_COUNT
};

enum
{
    SORT_LIST_NUMBER,
    SORT_LIST_NAME
};


#define CDDB_GENRE_MAX ( sizeof(cddb_genre_vs_id3_genre)/sizeof(cddb_genre_vs_id3_genre[0]) - 1 )
static const gchar *cddb_genre_vs_id3_genre [][2] =
{
    /* Cddb Genre - ID3 Genre */
    {"Blues",       "Blues"},
    {"Classical",   "Classical"},
    {"Country",     "Country"},
    {"Data",        "Other"},
    {"Folk",        "Folk"},
    {"Jazz",        "Jazz"},
    {"NewAge",      "New Age"},
    {"Reggae",      "Reggae"},
    {"Rock",        "Rock"},
    {"Soundtrack",  "Soundtrack"},
    {"Misc",        "Other"}
};


// File for result of the Cddb/Freedb request (on remote access)
static const gchar CDDB_RESULT_FILE[] = "cddb_result_file.tmp";

static const guint BOX_SPACING = 6;


/**************
 * Prototypes *
 **************/
static gboolean Cddb_Free_Track_Album_List (GList *track_list);

static gint Cddb_Read_Line        (FILE **file, gchar **cddb_out);
static gint Cddb_Read_Http_Header (FILE **file, gchar **cddb_out);
static gint Cddb_Read_Cddb_Header (FILE **file, gchar **cddb_out);

static GdkPixbuf *Cddb_Get_Pixbuf_From_Server_Name (const gchar *server_name);

static const gchar *Cddb_Get_Id3_Genre_From_Cddb_Genre (const gchar *cddb_genre);

static gint Cddb_Track_List_Sort_Func (GtkTreeModel *model, GtkTreeIter *a,
                                       GtkTreeIter *b, gpointer data);

static gchar *Cddb_Format_Proxy_Authentification (void);

static gboolean Cddb_Get_Album_Tracks_List_CB (EtCDDBDialog *self, GtkTreeSelection *selection);


/*
 * The window to connect to the cd data base.
 */

static void
on_show_categories_toggle_toggled (EtCDDBDialog *self)
{
    EtCDDBDialogPrivate *priv;
    gsize i;

    priv = et_cddb_dialog_get_instance_private (self);

    /* FIXME: Toggle visibility of the container instead. */
    if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (priv->show_categories_toggle)))
    {
        gtk_widget_show (priv->separator_h);

        for (i = 0; i < 10; i++)
        {
            gtk_widget_show (priv->category_toggle[i]);
        }
    }
    else
    {
        gtk_widget_hide (priv->separator_h);

        for (i = 0; i < 10; i++)
        {
            gtk_widget_hide (priv->category_toggle[i]);
        }
    }

    /* Force the window to be redrawn. */
    gtk_widget_queue_resize (GTK_WIDGET (self));
}

static void
update_apply_button_sensitivity (EtCDDBDialog *self)
{
    EtCDDBDialogPrivate *priv;

    priv = et_cddb_dialog_get_instance_private (self);

    /* If any field is set, enable the apply button. */
    if (priv->apply_button
        && gtk_tree_model_iter_n_children (GTK_TREE_MODEL (priv->track_list_model),
                                           NULL) > 0
        && (g_settings_get_flags (MainSettings, "cddb-set-fields") != 0))
    {
        gtk_widget_set_sensitive (GTK_WIDGET (priv->apply_button), TRUE);
    }
    else
    {
        gtk_widget_set_sensitive (GTK_WIDGET (priv->apply_button), FALSE);
    }
}

static void
update_search_button_sensitivity (EtCDDBDialog *self)
{
    EtCDDBDialogPrivate *priv;

    priv = et_cddb_dialog_get_instance_private (self);

    if (priv->search_button
        && g_utf8_strlen (gtk_entry_get_text (GTK_ENTRY (priv->search_string_entry)), -1) > 0
        && (g_settings_get_flags (MainSettings, "cddb-search-fields") != 0)
        && (g_settings_get_flags (MainSettings, "cddb-search-categories") != 0))
    {
        gtk_widget_set_sensitive (GTK_WIDGET (priv->search_button), TRUE);
    }
    else
    {
        gtk_widget_set_sensitive (GTK_WIDGET (priv->search_button), FALSE);
    }
}

/*
 * Searches the Cddb Album List for specific terms
 * (this is not search the remote CDDB database...)
 */
static void
find_previous_string_in_results (EtCDDBDialog *self)
{
    EtCDDBDialogPrivate *priv;
    gchar *string;
    gchar  buffer[256];
    gchar *pbuffer;
    gchar *text;
    gchar *temp;
    GtkTreeSelection* treeSelection;
    GtkTreeIter iter;
    GtkTreePath *rowpath;
    gboolean itemselected = FALSE;

    priv = et_cddb_dialog_get_instance_private (self);

    string = g_strdup(gtk_entry_get_text(GTK_ENTRY(priv->search_string_in_results_entry)));
    if (!string || strlen(string)==0)
        return;
    temp = g_utf8_strdown(string, -1);
    g_free(string);
    string = temp;

    Add_String_To_Combo_List(priv->search_string_in_result_model, string);

    /* Get the currently selected row into &iter and set itemselected to reflect this */
    treeSelection = gtk_tree_view_get_selection(GTK_TREE_VIEW(priv->album_list_view));
    if (gtk_tree_selection_get_selected(treeSelection, NULL, &iter) == TRUE)
        itemselected = TRUE;

    /* Previous result button */

    /* Search in the album list (from bottom/selected-item to top) */
    if (itemselected == TRUE)
    {
        rowpath = gtk_tree_model_get_path(GTK_TREE_MODEL(priv->album_list_model), &iter);
        gtk_tree_path_prev(rowpath);
    } else
    {
        rowpath = gtk_tree_path_new_from_indices(gtk_tree_model_iter_n_children(GTK_TREE_MODEL(priv->album_list_model), NULL) - 1, -1);
    }

    do
    {
        gboolean found;

        found = gtk_tree_model_get_iter(GTK_TREE_MODEL(priv->album_list_model), &iter, rowpath);
        if (found)
        {
            gtk_tree_model_get(GTK_TREE_MODEL(priv->album_list_model), &iter, CDDB_ALBUM_LIST_ALBUM, &text, -1);
            g_utf8_strncpy(buffer,text,256);
            temp = g_utf8_strdown(buffer, -1);
            pbuffer = temp;

            if (pbuffer && strstr(pbuffer,string) != NULL)
            {
                gtk_tree_selection_select_iter(treeSelection, &iter);
                gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(priv->album_list_view), rowpath, NULL, FALSE, 0, 0);
                gtk_tree_path_free(rowpath);
                g_free(text);
                g_free(temp);
                g_free(string);
                return;
            }
            g_free(temp);
            g_free(text);
        }
    } while(gtk_tree_path_prev(rowpath));
    gtk_tree_path_free(rowpath);
}

static void
find_next_string_in_results (EtCDDBDialog *self)
{
    EtCDDBDialogPrivate *priv;
    gchar *string;
    gchar  buffer[256];
    gchar *pbuffer;
    gchar *text;
    gchar *temp;
    gint   i;
    gint  rowcount;
    GtkTreeSelection* treeSelection;
    GtkTreeIter iter;
    GtkTreePath *rowpath;
    gboolean result;
    gboolean itemselected = FALSE;
    GtkTreeIter itercopy;

    priv = et_cddb_dialog_get_instance_private (self);

    string = g_strdup (gtk_entry_get_text (GTK_ENTRY (priv->search_string_in_results_entry)));
    if (!string || strlen(string)==0)
        return;
    temp = g_utf8_strdown(string, -1);
    g_free(string);
    string = temp;

    Add_String_To_Combo_List(priv->search_string_in_result_model, string);

    /* Get the currently selected row into &iter and set itemselected to reflect this */
    treeSelection = gtk_tree_view_get_selection(GTK_TREE_VIEW(priv->album_list_view));
    if (gtk_tree_selection_get_selected(treeSelection, NULL, &iter) == TRUE)
        itemselected = TRUE;

    rowcount = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(priv->album_list_model), NULL);

    /* Search in the album list (from top to bottom) */
    if (itemselected == TRUE)
    {
        gtk_tree_selection_unselect_iter(treeSelection, &iter);
        result = gtk_tree_model_iter_next(GTK_TREE_MODEL(priv->album_list_model), &iter);
    } else
    {
        result = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(priv->album_list_model), &iter);
    }

    itercopy = iter;

    /* If list entries follow the previously selected item, loop through them looking for a match */
    if(result == TRUE)
    {
        do /* Search following results */
        {
            gtk_tree_model_get(GTK_TREE_MODEL(priv->album_list_model), &iter, CDDB_ALBUM_LIST_ALBUM, &text, -1);
            g_utf8_strncpy(buffer, text, 256);

            temp = g_utf8_strdown(buffer, -1);
            pbuffer = temp;

            if (pbuffer && strstr(pbuffer, string) != NULL)
            {
                gtk_tree_selection_select_iter(treeSelection, &iter);
                rowpath = gtk_tree_model_get_path(GTK_TREE_MODEL(priv->album_list_model), &iter);
                gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(priv->album_list_view), rowpath, NULL, FALSE, 0, 0);
                gtk_tree_path_free(rowpath);
                g_free(text);
                g_free(temp);
                g_free(string);
                return;
            }
            g_free(temp);
            g_free(text);
        } while(gtk_tree_model_iter_next(GTK_TREE_MODEL(priv->album_list_model), &iter));
    }

    for (i = 0; i < rowcount; i++)
    {
        gboolean found;

        if (i == 0)
            found = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(priv->album_list_model), &itercopy);
        else
            found = gtk_tree_model_iter_next(GTK_TREE_MODEL(priv->album_list_model), &itercopy);

        if (found)
        {
            gtk_tree_model_get(GTK_TREE_MODEL(priv->album_list_model), &itercopy, CDDB_ALBUM_LIST_ALBUM, &text, -1);
            g_utf8_strncpy(buffer, text, 256);

            temp = g_utf8_strdown(buffer, -1);
            pbuffer = temp;

            if (pbuffer && strstr(pbuffer,string) != NULL)
            {
                gtk_tree_selection_select_iter(treeSelection, &itercopy);
                rowpath = gtk_tree_model_get_path(GTK_TREE_MODEL(priv->album_list_model), &itercopy);
                gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(priv->album_list_view), rowpath, NULL, FALSE, 0, 0);
                gtk_tree_path_free(rowpath);
                g_free(text);
                g_free(temp);
                g_free(string);
                return;
            }
            g_free(temp);
            g_free(text);
        }
    }
    g_free(string);
}

/*
 * Show collected infos of the album in the status bar
 */
static void
show_album_info (EtCDDBDialog *self, GtkTreeSelection *selection)
{
    EtCDDBDialogPrivate *priv;
    CddbAlbum *cddbalbum = NULL;
    gchar *msg, *duration_str;
    GtkTreeIter row;
    priv = et_cddb_dialog_get_instance_private (self);

    if (gtk_tree_selection_get_selected(selection, NULL, &row))
    {
        gtk_tree_model_get(GTK_TREE_MODEL(priv->album_list_model), &row, CDDB_ALBUM_LIST_DATA, &cddbalbum, -1);
    }
    if (!cddbalbum)
        return;

    duration_str = Convert_Duration((gulong)cddbalbum->duration);
    msg = g_strdup_printf(_("Album: '%s', "
                            "artist: '%s', "
                            "length: '%s', "
                            "year: '%s', "
                            "genre: '%s', "
                            "ID: '%s'"),
                            cddbalbum->album ? cddbalbum->album : "",
                            cddbalbum->artist ? cddbalbum->artist : "",
                            duration_str,
                            cddbalbum->year ? cddbalbum->year : "",
                            cddbalbum->genre ? cddbalbum->genre : "",
                            cddbalbum->id ? cddbalbum->id : "");
    gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar), priv->status_bar_context, msg);
    g_free(msg);
    g_free(duration_str);
}

/*
 * Select the corresponding file into the main file list
 */
static void
Cddb_Track_List_Row_Selected (EtCDDBDialog *self, GtkTreeSelection *selection)
{
    EtCDDBDialogPrivate *priv;
    GList       *selectedRows;
    GList *l;
    GtkTreeIter  currentFile;
    gchar       *text_path;
    ET_File    **etfile;

    priv = et_cddb_dialog_get_instance_private (self);

    // Exit if we don't have to select files in the main list
    if (!g_settings_get_boolean (MainSettings, "cddb-follow-file"))
        return;

    selectedRows = gtk_tree_selection_get_selected_rows(selection, NULL);

    // We might be called with no rows selected
    if (!selectedRows)
    {
        return;
    }

    /* Unselect files in the main list before re-selecting them... */
    et_application_window_browser_unselect_all (ET_APPLICATION_WINDOW (MainWindow));

    for (l = selectedRows; l != NULL; l = g_list_next (l))
    {
        gboolean found;

        found = gtk_tree_model_get_iter (GTK_TREE_MODEL (priv->track_list_model),
                                         &currentFile, (GtkTreePath*)l->data);

        if (found)
        {
            if (g_settings_get_boolean (MainSettings, "cddb-dlm-enabled"))
            {
                gtk_tree_model_get(GTK_TREE_MODEL(priv->track_list_model), &currentFile,
                                   CDDB_TRACK_LIST_NAME, &text_path,
                                   CDDB_TRACK_LIST_ETFILE, &etfile, -1);
                *etfile = et_application_window_browser_select_file_by_dlm (ET_APPLICATION_WINDOW (MainWindow),
                                                                            text_path,
                                                                            TRUE);
            } else
            {
                text_path = gtk_tree_model_get_string_from_iter(GTK_TREE_MODEL(priv->track_list_model), &currentFile);
                et_application_window_browser_select_file_by_iter_string (ET_APPLICATION_WINDOW (MainWindow),
                                                                          text_path,
                                                                          TRUE);
            }
            g_free(text_path);
        }
    }

    g_list_free_full (selectedRows, (GDestroyNotify)gtk_tree_path_free);
}

/*
 * Invert the selection of every row in the track list
 */
static void
Cddb_Track_List_Invert_Selection (EtCDDBDialog *self)
{
    EtCDDBDialogPrivate *priv;
    GtkTreeSelection *selection;
    GtkTreeIter iter;
    gboolean valid;

    priv = et_cddb_dialog_get_instance_private (self);

    selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(priv->track_list_view));

    if (selection)
    {
        /* Must block the select signal to avoid selecting all files (one by one) in the main list */
        g_signal_handlers_block_by_func (selection,
                                         G_CALLBACK (Cddb_Track_List_Row_Selected),
                                         NULL);

        valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(priv->track_list_model), &iter);
        while (valid)
        {
            if (gtk_tree_selection_iter_is_selected(selection, &iter))
            {
                gtk_tree_selection_unselect_iter(selection, &iter);
            } else
            {
                gtk_tree_selection_select_iter(selection, &iter);
            }
            valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(priv->track_list_model), &iter);
        }
        g_signal_handlers_unblock_by_func (selection,
                                           G_CALLBACK (Cddb_Track_List_Row_Selected),
                                           NULL);
        g_signal_emit_by_name(G_OBJECT(gtk_tree_view_get_selection(GTK_TREE_VIEW(priv->track_list_view))), "changed");
    }
}

/*
 * Set the row apperance depending if we have cached info or not
 * Bold/Red = Info are already loaded, but not displayed
 * Italic/Light Red = Duplicate CDDB entry
 */
static void
Cddb_Album_List_Set_Row_Appearance (EtCDDBDialog *self, GtkTreeIter *row)
{
    EtCDDBDialogPrivate *priv;
    CddbAlbum *cddbalbum = NULL;

    priv = et_cddb_dialog_get_instance_private (self);

    gtk_tree_model_get(GTK_TREE_MODEL(priv->album_list_model), row,
                       CDDB_ALBUM_LIST_DATA, &cddbalbum, -1);

    if (cddbalbum->track_list != NULL)
    {
        if (g_settings_get_boolean (MainSettings, "file-changed-bold"))
        {
            gtk_list_store_set(priv->album_list_model, row,
                               CDDB_ALBUM_LIST_FONT_STYLE,       PANGO_STYLE_NORMAL,
                               CDDB_ALBUM_LIST_FONT_WEIGHT,      PANGO_WEIGHT_BOLD,
                               CDDB_ALBUM_LIST_FOREGROUND_COLOR, NULL,-1);
        } else
        {
            if (cddbalbum->other_version == TRUE)
            {
                const GdkRGBA LIGHT_RED = { 1.0, 0.5, 0.5, 1.0 };
                gtk_list_store_set(priv->album_list_model, row,
                                   CDDB_ALBUM_LIST_FONT_STYLE,       PANGO_STYLE_NORMAL,
                                   CDDB_ALBUM_LIST_FONT_WEIGHT,      PANGO_WEIGHT_NORMAL,
                                   CDDB_ALBUM_LIST_FOREGROUND_COLOR, &LIGHT_RED, -1);
            } else
            {
                gtk_list_store_set(priv->album_list_model, row,
                                   CDDB_ALBUM_LIST_FONT_STYLE,       PANGO_STYLE_NORMAL,
                                   CDDB_ALBUM_LIST_FONT_WEIGHT,      PANGO_WEIGHT_NORMAL,
                                   CDDB_ALBUM_LIST_FOREGROUND_COLOR, &RED, -1);
            }
        }
    }
    else
    {
        if (cddbalbum->other_version == TRUE)
        {
            if (g_settings_get_boolean (MainSettings, "file-changed-bold"))
            {
                gtk_list_store_set(priv->album_list_model, row,
                                   CDDB_ALBUM_LIST_FONT_STYLE,       PANGO_STYLE_ITALIC,
                                   CDDB_ALBUM_LIST_FONT_WEIGHT,      PANGO_WEIGHT_NORMAL,
                                   CDDB_ALBUM_LIST_FOREGROUND_COLOR, NULL,-1);
            } else
            {
                const GdkRGBA GREY = { 0.664, 0.664, 0.664, 1.0 };
                gtk_list_store_set(priv->album_list_model, row,
                                   CDDB_ALBUM_LIST_FONT_STYLE,       PANGO_STYLE_NORMAL,
                                   CDDB_ALBUM_LIST_FONT_WEIGHT,      PANGO_WEIGHT_NORMAL,
                                   CDDB_ALBUM_LIST_FOREGROUND_COLOR, &GREY, -1);
            }
        } else
        {
            gtk_list_store_set(priv->album_list_model, row,
                               CDDB_ALBUM_LIST_FONT_STYLE,       PANGO_STYLE_NORMAL,
                               CDDB_ALBUM_LIST_FONT_WEIGHT,      PANGO_WEIGHT_NORMAL,
                               CDDB_ALBUM_LIST_FOREGROUND_COLOR, NULL, -1);
        }
    }
}

/*
 * Clear the album model, blocking the tree view selection changed handlers
 * during the process, to prevent the handlers being called on removed rows.
 */
static void
cddb_album_model_clear (EtCDDBDialog *self)
{
    EtCDDBDialogPrivate *priv;
    GtkTreeSelection *selection;

    priv = et_cddb_dialog_get_instance_private (self);

    selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (priv->album_list_view));

    g_signal_handlers_block_by_func (selection,
                                     G_CALLBACK (Cddb_Get_Album_Tracks_List_CB),
                                     self);
    g_signal_handlers_block_by_func (selection, G_CALLBACK (show_album_info),
                                     self);

    gtk_list_store_clear (priv->album_list_model);

    g_signal_handlers_unblock_by_func (selection, G_CALLBACK (show_album_info),
                                       self);
    g_signal_handlers_unblock_by_func (selection,
                                       G_CALLBACK (Cddb_Get_Album_Tracks_List_CB),
                                       self);
}

/*
 * Clear the album model, blocking the tree view selection changed handlers
 * during the process, to prevent the handlers being called on removed rows.
 */
static void
cddb_track_model_clear (EtCDDBDialog *self)
{
    EtCDDBDialogPrivate *priv;
    GtkTreeSelection *selection;

    priv = et_cddb_dialog_get_instance_private (self);

    selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (priv->track_list_view));

    g_signal_handlers_block_by_func (selection,
                                     G_CALLBACK (Cddb_Track_List_Row_Selected),
                                     self);

    gtk_list_store_clear (priv->track_list_model);

    g_signal_handlers_unblock_by_func (selection,
                                       G_CALLBACK (Cddb_Track_List_Row_Selected),
                                       self);
}

/*
 * Load the CddbTrackList into the corresponding List
 */
static void
Cddb_Load_Track_Album_List (EtCDDBDialog *self, GList *track_list)
{
    EtCDDBDialogPrivate *priv;

    priv = et_cddb_dialog_get_instance_private (self);

    if (track_list && priv->track_list_view)
    {
        GList *l;

        /* Must block the select signal of the target to avoid looping. */
        cddb_track_model_clear (self);

        for (l = g_list_first (track_list); l != NULL; l = g_list_next (l))
        {
            gchar *row_text[1];
            CddbTrackAlbum *cddbtrackalbum = l->data;
            ET_File **etfile;
            etfile = g_malloc0(sizeof(ET_File *));

            row_text[0] = Convert_Duration((gulong)cddbtrackalbum->duration);

            /* Load the row in the list. */
            gtk_list_store_insert_with_values (priv->track_list_model, NULL,
                                               G_MAXINT,
                                               CDDB_TRACK_LIST_NUMBER,
                                               cddbtrackalbum->track_number,
                                               CDDB_TRACK_LIST_NAME,
                                               cddbtrackalbum->track_name,
                                               CDDB_TRACK_LIST_TIME,
                                               row_text[0],
                                               CDDB_TRACK_LIST_DATA,
                                               cddbtrackalbum,
                                               CDDB_TRACK_LIST_ETFILE, etfile,
                                               -1);

            g_free(row_text[0]);
        }

        update_apply_button_sensitivity (self);
    }
}

/*
 * Cddb_Open_Connection:
 * @host: a hostname
 * @port: a port number
 *
 * Open a connection to @hostname, performing a DNS lookup as necessary.
 *
 * Returns: the socket fd, or 0 upon failure
 */
/* TODO: Propagate the GError to the caller. */
static gint
Cddb_Open_Connection (EtCDDBDialog *self, const gchar *host, gint port)
{
    EtCDDBDialogPrivate *priv;
    GSocketConnectable *address;
    GSocketAddressEnumerator *enumerator;
    GCancellable *cancellable;
    GSocketAddress *sockaddress;
    GError *error = NULL;
    GError *sock_error = NULL;
    gint socket_id = 0;
    gchar *msg;

    g_return_val_if_fail (self != NULL, 0);
    g_return_val_if_fail (host != NULL && port > 0, 0);

    priv = et_cddb_dialog_get_instance_private (self);

    msg = g_strdup_printf(_("Resolving host '%s'…"),host);
    gtk_statusbar_push (GTK_STATUSBAR (priv->status_bar),
                        priv->status_bar_context, msg);
    g_free(msg);

    while (gtk_events_pending ())
    {
        gtk_main_iteration ();
    }

    address = g_network_address_new (host, port);
    enumerator = g_socket_connectable_enumerate (address);
    g_object_unref (address);

    cancellable = g_cancellable_new ();

    while (socket_id == 0
           && (sockaddress = g_socket_address_enumerator_next (enumerator,
                                                               cancellable,
                                                               &error)))
    {
        struct sockaddr sockaddr_in;
        gint optval = 1;

        if (!g_socket_address_to_native (sockaddress, &sockaddr_in,
                                         sizeof (sockaddr_in),
                                         sock_error ? NULL : &sock_error))
        {
            g_object_unref (sockaddress);
            continue;
        }

        g_object_unref (sockaddress);

        while (gtk_events_pending ())
        {
            gtk_main_iteration ();
        }

        /* Create socket. */
        if ((socket_id = socket (AF_INET, SOCK_STREAM, 0)) < 0)
        {
            msg = g_strdup_printf (_("Cannot create a new socket (%s)"),
                                   g_strerror (errno));
            gtk_statusbar_push (GTK_STATUSBAR (priv->status_bar),
                                priv->status_bar_context, msg);
            Log_Print (LOG_ERROR, "%s", msg);
            g_free (msg);
            goto err;
        }

        /* FIXME : must catch SIGPIPE? */
        if (setsockopt (socket_id, SOL_SOCKET, SO_KEEPALIVE, (void *)&optval,
            sizeof (optval)) < 0)
        {
            Log_Print (LOG_WARNING,
                       _("Cannot set options on the newly-created socket"));
        }

        /* Open connection to the server. */
        msg = g_strdup_printf (_("Connecting to host '%s', port '%d'…"), host,
                               port);
        gtk_statusbar_push (GTK_STATUSBAR (priv->status_bar),
                            priv->status_bar_context, msg);
        g_free (msg);

        while (gtk_events_pending ())
        {
            gtk_main_iteration ();
        }

        if (connect (socket_id, &sockaddr_in, sizeof (struct sockaddr)) < 0)
        {
            msg = g_strdup_printf (_("Cannot connect to host '%s' (%s)"), host,
                                   g_strerror (errno));
            gtk_statusbar_push (GTK_STATUSBAR (priv->status_bar),
                                priv->status_bar_context, msg);
            Log_Print (LOG_ERROR, "%s", msg);
            g_free (msg);

            goto err;
        }
    }

    if (socket_id != 0)
    {
        /* First address failed, but a later address succeeded. */
        if (sock_error)
        {
            g_debug ("Failure while looking up address: %s",
                     sock_error->message);
            g_error_free (sock_error);
        }
    }

    if (error)
    {
        msg = g_strdup_printf (_("Cannot resolve host '%s' (%s)"), host,
                               error->message);
        gtk_statusbar_push (GTK_STATUSBAR (priv->status_bar),
                            priv->status_bar_context, msg);
        Log_Print (LOG_ERROR, "%s", msg);
        g_free (msg);
        g_error_free (error);
        goto err;
    }

    g_object_unref (enumerator);
    g_object_unref (cancellable);

    msg = g_strdup_printf (_("Connected to host '%s'"), host);
    gtk_statusbar_push (GTK_STATUSBAR (priv->status_bar), priv->status_bar_context,
                        msg);
    g_free (msg);

    while (gtk_events_pending ())
    {
        gtk_main_iteration ();
    }

    return socket_id;

err:
    g_object_unref (enumerator);
    g_object_unref (cancellable);

    return 0;
}

/*
 * Close the connection correcponding to the socket_id
 */
static void
Cddb_Close_Connection (EtCDDBDialog *self, gint socket_id)
{
    EtCDDBDialogPrivate *priv;

#ifndef G_OS_WIN32
    shutdown(socket_id,SHUT_RDWR);
#endif /* !G_OS_WIN32 */
    close(socket_id);

    g_return_if_fail (ET_CDDB_DIALOG (self));

    priv = et_cddb_dialog_get_instance_private (self);

    priv->stop_searching = FALSE;
}

/*
 * Read the result of the request and write it into a file.
 * And return the number of bytes read.
 *  - bytes_read=0 => no more data.
 *  - bytes_read_total : use to add bytes of severals pages... must be initialized before
 *
 * Server answser is formated like this :
 *
 * HTTP/1.1 200 OK\r\n                              }
 * Server: Apache/1.3.19 (Unix) PHP/4.0.4pl1\r\n    } "Header"
 * Connection: close\r\n                            }
 * \r\n
 * <html>\n                                         }
 * [...]                                            } "Body"
 */
static gint
Cddb_Write_Result_To_File (EtCDDBDialog *self,
                           gint socket_id,
                           gulong *bytes_read_total)
{
    EtCDDBDialogPrivate *priv;
    gchar *file_path = NULL;
    FILE  *file;

    priv = et_cddb_dialog_get_instance_private (self);

    /* Cache directory was already created by Log_Print(). */
    file_path = g_build_filename (g_get_user_cache_dir (), PACKAGE_TARNAME,
                                  CDDB_RESULT_FILE, NULL);

    if ((file = fopen (file_path, "w+")) != NULL)
    {
        gchar cddb_out[MAX_STRING_LEN+1];
        gint  bytes_read = 0;

        while ( self && !priv->stop_searching
        // Read data
        && (bytes_read = recv(socket_id,(void *)&cddb_out,MAX_STRING_LEN,0)) > 0 )
        {
            gchar *size_str;
            gchar *msg;


            // Write to file
            cddb_out[bytes_read] = 0;
            if (fwrite (&cddb_out, bytes_read, 1, file) != 1)
            {
                 Log_Print (LOG_ERROR,
                            _("Error while writing CDDB results to file '%s'"),
                            file_path);
                 break;
            }

            *bytes_read_total += bytes_read;

            //g_print("\nLine : %lu : %s\n",bytes_read,cddb_out);

            // Display message
            size_str =  g_format_size (*bytes_read_total);
            msg = g_strdup_printf(_("Receiving data (%s)…"),size_str);
            gtk_statusbar_push (GTK_STATUSBAR (priv->status_bar),
                                priv->status_bar_context, msg);
            g_free(msg);
            g_free(size_str);
            while (gtk_events_pending())
                gtk_main_iteration();
        }

        fclose(file);

        if (bytes_read < 0)
        {
            Log_Print (LOG_ERROR, _("Error when reading CDDB response (%s)"),
	               g_strerror(errno));
            return -1; // Error!
        }

    } else
    {
        Log_Print (LOG_ERROR, _("Cannot create file '%s' (%s)"), file_path,
	           g_strerror(errno));
    }
    g_free(file_path);

    return 0;
}

/*
 * Look up a specific album in freedb, and save to a CddbAlbum structure
 */
static gboolean
Cddb_Get_Album_Tracks_List (EtCDDBDialog *self, GtkTreeSelection* selection)
{
    EtCDDBDialogPrivate *priv;
    gint       socket_id = 0;
    CddbAlbum *cddbalbum = NULL;
    GList     *TrackOffsetList = NULL;
    gchar     *cddb_in, *cddb_out = NULL;
    gchar     *cddb_end_str, *msg, *copy, *valid;
    gchar     *proxy_auth;
    gchar     *cddb_server_name;
    gint       cddb_server_port;
    gchar     *cddb_server_cgi_path;
    gboolean proxy_enabled;
    gchar *proxy_hostname;
    guint proxy_port;
    gint       bytes_written;
    gulong     bytes_read_total = 0;
    FILE      *file = NULL;
    gboolean   read_track_offset = FALSE;
    GtkTreeIter row;

    priv = et_cddb_dialog_get_instance_private (self);

    cddb_track_model_clear (self);
    update_apply_button_sensitivity (self);

    if (gtk_tree_selection_get_selected(selection, NULL, &row))
    {
        gtk_tree_model_get(GTK_TREE_MODEL(priv->album_list_model), &row, CDDB_ALBUM_LIST_DATA, &cddbalbum, -1);
    }
    if (!cddbalbum)
        return FALSE;

    // We have already the track list
    if (cddbalbum->track_list != NULL)
    {
        Cddb_Load_Track_Album_List (self, cddbalbum->track_list);
        return TRUE;
    }

    // Parameters of the server used
    cddb_server_name     = cddbalbum->server_name;
    cddb_server_port     = cddbalbum->server_port;
    cddb_server_cgi_path = cddbalbum->server_cgi_path;

    if (!cddb_server_name)
    {
        // Local access
        if ( (file=fopen(cddb_server_cgi_path,"r"))==0 )
        {
            Log_Print(LOG_ERROR,_("Can't load file: '%s' (%s)."),cddb_server_cgi_path,g_strerror(errno));
            return FALSE;
        }

    }else
    {
        /* Connection to the server. */
        proxy_enabled = g_settings_get_boolean (MainSettings,
                                                "cddb-proxy-enabled");
        proxy_hostname = g_settings_get_string (MainSettings,
                                                "cddb-proxy-hostname");
        proxy_port = g_settings_get_uint (MainSettings, "cddb-proxy-port");
        if ((socket_id = Cddb_Open_Connection (self,
                                               proxy_enabled
                                               ? proxy_hostname
                                               : cddb_server_name,
                                               proxy_enabled
                                               ? proxy_port
                                               : cddb_server_port)) <= 0)
        {
            g_free (proxy_hostname);
            return FALSE;
        }

		if ( strstr(cddb_server_name,"gnudb") != NULL )
		{
			// For gnudb
			// New version of gnudb doesn't use a cddb request, but a http request
		    cddb_in = g_strdup_printf("GET %s%s/gnudb/"
		                              "%s/%s"
		                              " HTTP/1.1\r\n"
		                              "Host: %s:%d\r\n"
		                              "User-Agent: %s %s\r\n"
		                              "%s"
		                              "Connection: close\r\n"
		                              "\r\n",
		                              proxy_enabled ? "http://" : "",
                                              proxy_enabled ? cddb_server_name : "",
		                              cddbalbum->category,cddbalbum->id,
		                              cddb_server_name,cddb_server_port,
		                              PACKAGE_NAME, PACKAGE_VERSION,
		                              (proxy_auth=Cddb_Format_Proxy_Authentification())
		                              );
		}else
		{
		    // CDDB Request (ex: GET /~cddb/cddb.cgi?cmd=cddb+read+jazz+0200a401&hello=noname+localhost+EasyTAG+0.31&proto=1 HTTP/1.1\r\nHost: freedb.freedb.org:80\r\nConnection: close)
		    // Without proxy : "GET /~cddb/cddb.cgi?…" but doesn't work with a proxy.
		    // With proxy    : "GET http://freedb.freedb.org/~cddb/cddb.cgi?…"
		    cddb_in = g_strdup_printf("GET %s%s%s?cmd=cddb+read+"
		                              "%s+%s"
		                              "&hello=noname+localhost+%s+%s"
		                              "&proto=6 HTTP/1.1\r\n"
		                              "Host: %s:%d\r\n"
		                              "%s"
		                              "Connection: close\r\n\r\n",
		                              proxy_enabled ? "http://" : "",
                                              proxy_enabled ? cddb_server_name : "",
                                              cddb_server_cgi_path,
		                              cddbalbum->category,cddbalbum->id,
		                              PACKAGE_NAME, PACKAGE_VERSION,
		                              cddb_server_name,cddb_server_port,
		                              (proxy_auth=Cddb_Format_Proxy_Authentification())
		                              );
		}

		
		g_free(proxy_auth);
        //g_print("Request Cddb_Get_Album_Tracks_List : '%s'\n", cddb_in);

        // Send the request
        gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,_("Sending request…"));
        while (gtk_events_pending()) gtk_main_iteration();
        if ( (bytes_written=send(socket_id,cddb_in,strlen(cddb_in)+1,0)) < 0)
        {
            Log_Print(LOG_ERROR,_("Cannot send the request (%s)"),g_strerror(errno));
            Cddb_Close_Connection (self, socket_id);
            g_free(cddb_in);
            g_free (proxy_hostname);
            return FALSE;
        }
        g_free(cddb_in);
        g_free (proxy_hostname);


        // Read the answer
        gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,_("Receiving data…"));
        while (gtk_events_pending())
            gtk_main_iteration();

        /* Write result in a file. */
        if (Cddb_Write_Result_To_File (self, socket_id, &bytes_read_total) < 0)
        {
            msg = g_strdup(_("The server returned a bad response"));
            gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,msg);
            Log_Print(LOG_ERROR,"%s",msg);
            g_free(msg);
            gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_search_button),FALSE);
            gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_auto_search_button),FALSE);
            return FALSE;
        }


        // Parse server answer : Check HTTP Header (freedb or gnudb) and CDDB Header (freedb only)
        file = NULL;
		if ( strstr(cddb_server_name,"gnudb") != NULL )
		{
			// For gnudb (don't check CDDB header)
			if ( Cddb_Read_Http_Header(&file,&cddb_out) <= 0 )
		    {
		        gchar *msg = g_strdup_printf(_("The server returned a bad response: %s"),cddb_out);
		        gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,msg);
		        Log_Print(LOG_ERROR,"%s",msg);
		        g_free(msg);
		        g_free(cddb_out);
		        if (file)
		            fclose(file);
		        return FALSE;
		    }
		}else
		{
			// For freedb
			if ( Cddb_Read_Http_Header(&file,&cddb_out) <= 0
		      || Cddb_Read_Cddb_Header(&file,&cddb_out) <= 0 )
		    {
		        gchar *msg = g_strdup_printf(_("The server returned a bad response: %s"),cddb_out);
		        gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,msg);
		        Log_Print(LOG_ERROR,"%s",msg);
		        g_free(msg);
		        g_free(cddb_out);
		        if (file)
		            fclose(file);
		        return FALSE;
		    }
		}
        g_free(cddb_out);

    }
    cddb_end_str = g_strdup(".");

    while ( self && !priv->stop_searching
    && Cddb_Read_Line(&file,&cddb_out) > 0 )
    {
        if (!cddb_out) // Empty line?
            continue;
        //g_print("%s\n",cddb_out);

        // To avoid the cddb lookups to hang (Patch from Paul Giordano)
        /* It appears that on some systems that cddb lookups continue to attempt
         * to get data from the socket even though the other system has completed
         * sending. The fix adds one check to the loops to see if the actual
         * end of data is in the last block read. In this case, the last line
         * will be a single '.'
         */
        if (strlen(cddb_out)<=3 && strstr(cddb_out,cddb_end_str)!=NULL)
            break;

        if ( strstr(cddb_out,"Track frame offsets")!=NULL ) // We read the Track frame offset
        {
            read_track_offset = TRUE; // The next reads are for the tracks offset
            continue;

        }else if (read_track_offset) // We are reading a track offset? (generates TrackOffsetList)
        {
            if ( strtoul(cddb_out+1,NULL,10)>0 )
            {
                CddbTrackFrameOffset *cddbtrackframeoffset = g_malloc0(sizeof(CddbTrackFrameOffset));
                cddbtrackframeoffset->offset = strtoul(cddb_out+1,NULL,10);
                TrackOffsetList = g_list_append(TrackOffsetList,cddbtrackframeoffset);
            }else
            {
                read_track_offset = FALSE; // No more track offset
            }
            continue;

        }else if ( strstr(cddb_out,"Disc length: ")!=NULL ) // Length of album (in second)
        {
            cddbalbum->duration = atoi(strchr(cddb_out,':')+1);
            if (TrackOffsetList) // As it must be the last item, do nothing if no previous data
            {
                CddbTrackFrameOffset *cddbtrackframeoffset = g_malloc0(sizeof(CddbTrackFrameOffset));
                cddbtrackframeoffset->offset = cddbalbum->duration * 75; // It's the last offset
                TrackOffsetList = g_list_append(TrackOffsetList,cddbtrackframeoffset);
            }
            continue;

        }else if ( strncmp(cddb_out,"DTITLE=",7)==0 ) // "Artist / Album" names
        {
            // Note : disc title too long take severals lines. For example :
            // DTITLE=Marilyn Manson / The Nobodies (2005 Against All Gods Mix - Korea Tour L
            // DTITLE=imited Edition)
            if (!cddbalbum->album)
            {
                // It is the first time we find DTITLE...

                gchar *alb_ptr = strstr(cddb_out," / ");
                // Album
                if (alb_ptr && alb_ptr+3)
                {
                    cddbalbum->album = Try_To_Validate_Utf8_String(alb_ptr+3);
                    *alb_ptr = 0;
                }

                // Artist
                cddbalbum->artist = Try_To_Validate_Utf8_String(cddb_out+7); // '7' to skip 'DTITLE='
            }else
            {
                // It is at least the second time we find DTITLE
                // So we suppose that only the album was truncated

                // Album
                valid = Try_To_Validate_Utf8_String(cddb_out+7); // '7' to skip 'DTITLE='
                copy = cddbalbum->album; // To free...
                cddbalbum->album = g_strconcat(cddbalbum->album,valid,NULL);
                g_free(copy);
            }
            continue;

        }else if ( strncmp(cddb_out,"DYEAR=",6)==0 ) // Year
        {
            valid = Try_To_Validate_Utf8_String(cddb_out+6); // '6' to skip 'DYEAR='
            if (g_utf8_strlen(valid, -1))
                cddbalbum->year = valid;
            continue;

        }else if ( strncmp(cddb_out,"DGENRE=",7)==0 ) // Genre
        {
            valid = Try_To_Validate_Utf8_String(cddb_out+7); // '7' to skip 'DGENRE='
            if (g_utf8_strlen(valid, -1))
                cddbalbum->genre = valid;
            continue;

        }else if ( strncmp(cddb_out,"TTITLE",6)==0 ) // Track title (for exemple : TTITLE10=xxxx)
        {
            CddbTrackAlbum *cddbtrackalbum_last = NULL;

            CddbTrackAlbum *cddbtrackalbum = g_malloc0(sizeof(CddbTrackAlbum));
            cddbtrackalbum->cddbalbum = cddbalbum; // To find the CddbAlbum father quickly

            // Here is a fix when TTITLExx doesn't contain an "=", we skip the line
            if ( (copy = g_utf8_strchr(cddb_out,-1,'=')) != NULL )
            {
                cddbtrackalbum->track_name = Try_To_Validate_Utf8_String(copy+1);
            }else
            {
                continue;
            }

            *g_utf8_strchr(cddb_out,-1,'=') = 0;
            cddbtrackalbum->track_number = atoi(cddb_out+6)+1;

            // Note : titles too long take severals lines. For example :
            // TTITLE15=Bob Marley vs. Funkstar De Luxe Remix - Sun Is Shining (Radio De Lu
            // TTITLE15=xe Edit)
            // So to check it, we compare current track number with the previous one...
            if (cddbalbum->track_list)
                cddbtrackalbum_last = g_list_last(cddbalbum->track_list)->data;
            if (cddbtrackalbum_last && cddbtrackalbum_last->track_number == cddbtrackalbum->track_number)
            {
                gchar *track_name = g_strconcat(cddbtrackalbum_last->track_name,cddbtrackalbum->track_name,NULL);
                g_free(cddbtrackalbum_last->track_name);

                cddbtrackalbum_last->track_name = Try_To_Validate_Utf8_String(track_name);

                // Frees useless allocated data previously
                g_free(cddbtrackalbum->track_name);
                g_free(cddbtrackalbum);
            }else
            {
                if (TrackOffsetList && TrackOffsetList->next)
                {
                    cddbtrackalbum->duration = ( ((CddbTrackFrameOffset *)TrackOffsetList->next->data)->offset - ((CddbTrackFrameOffset *)TrackOffsetList->data)->offset ) / 75; // Calculate time in seconds
                    TrackOffsetList = TrackOffsetList->next;
                }
                cddbalbum->track_list = g_list_append(cddbalbum->track_list,cddbtrackalbum);
            }
            continue;

        }else if ( strncmp(cddb_out,"EXTD=",5)==0 ) // Extended album data
        {
            gchar *genre_ptr = strstr(cddb_out,"ID3G:");
            gchar *year_ptr  = strstr(cddb_out,"YEAR:");
            // May contains severals EXTD field it too long
            // EXTD=Techno
            // EXTD= YEAR: 1997 ID3G:  18
            // EXTD= ID3G:  17
            if (year_ptr && cddbalbum->year)
                cddbalbum->year = g_strdup_printf("%d",atoi(year_ptr+5));
            if (genre_ptr && cddbalbum->genre)
                cddbalbum->genre = g_strdup(Id3tag_Genre_To_String(atoi(genre_ptr+5)));
            continue;
        }

        g_free(cddb_out);
    }
    g_free(cddb_end_str);

    // Close file opened for reading lines
    if (file)
    {
        fclose(file);
        file = NULL;
    }

    if (cddb_server_name)
    {
        /* Remote access. */
        /* Close connection */
        Cddb_Close_Connection (self, socket_id);
    }

    /* Set color of the selected row (without reloading the whole list) */
    Cddb_Album_List_Set_Row_Appearance (self, &row);

    /* Load the track list of the album */
    gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,_("Loading album track list…"));
    while (gtk_events_pending()) gtk_main_iteration();
    Cddb_Load_Track_Album_List (self, cddbalbum->track_list);

    show_album_info (self, gtk_tree_view_get_selection (GTK_TREE_VIEW (priv->album_list_view)));

    // Frees 'TrackOffsetList'
    g_list_free_full (TrackOffsetList, (GDestroyNotify)g_free);
    TrackOffsetList = NULL;
    return TRUE;
}

/*
 * Callback when selecting a row in the Album List.
 * We get the list of tracks of the selected album
 */
static gboolean
Cddb_Get_Album_Tracks_List_CB (EtCDDBDialog *self, GtkTreeSelection *selection)
{
    gint i;
    gint i_max = 5;

    /* As may be not opened the first time (The server returned a wrong answer!)
     * me try to reconnect severals times */
    for (i = 1; i <= i_max; i++)
    {
        if (Cddb_Get_Album_Tracks_List (self, selection) == TRUE)
        {
            break;
        }
    }
    if (i <= i_max)
    {
        return TRUE;
    } else
    {
        return FALSE;
    }
}

/*
 * Load the priv->album_list into the corresponding List
 */
static void
Cddb_Load_Album_List (EtCDDBDialog *self, gboolean only_red_lines)
{
    EtCDDBDialogPrivate *priv;
    GtkTreeIter iter;
    GList *l;

    GtkTreeSelection *selection;
    GList            *selectedRows = NULL;
    GtkTreeIter       currentIter;
    CddbAlbum        *cddbalbumSelected = NULL;

    priv = et_cddb_dialog_get_instance_private (self);

    // Memorize the current selected item
    selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(priv->album_list_view));
    selectedRows = gtk_tree_selection_get_selected_rows(selection, NULL);
    if (selectedRows)
    {
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(priv->album_list_model), &currentIter, (GtkTreePath*)selectedRows->data))
            gtk_tree_model_get(GTK_TREE_MODEL(priv->album_list_model), &currentIter,
                               CDDB_ALBUM_LIST_DATA, &cddbalbumSelected, -1);
    }

    /* Remove lines. */
    cddb_album_model_clear (self);

    // Reload list following parameter 'only_red_lines'
    for (l = g_list_first (priv->album_list); l != NULL; l = g_list_next (l))
    {
        CddbAlbum *cddbalbum = l->data;

        if ( (only_red_lines && cddbalbum->track_list) || !only_red_lines)
        {
            /* Load the row in the list. */
            gtk_list_store_insert_with_values (priv->album_list_model, &iter,
                                               G_MAXINT,
                                               CDDB_ALBUM_LIST_PIXBUF,
                                               cddbalbum->bitmap,
                                               CDDB_ALBUM_LIST_ALBUM,
                                               cddbalbum->artist_album,
                                               CDDB_ALBUM_LIST_CATEGORY,
                                               cddbalbum->category,
                                               CDDB_ALBUM_LIST_DATA,
                                               cddbalbum, -1);

            Cddb_Album_List_Set_Row_Appearance (self, &iter);

            // Select this item if it is the saved one...
            if (cddbalbum == cddbalbumSelected)
                gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(priv->album_list_view)), &iter);
        }
    }
}

static void
Cddb_Display_Red_Lines_In_Result (EtCDDBDialog *self)
{
    EtCDDBDialogPrivate *priv;

    priv = et_cddb_dialog_get_instance_private (self);

    if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (priv->display_red_lines_toggle)))
    {
        /* Show only red lines. */
        Cddb_Load_Album_List (self, TRUE);
    }
    else
    {
        /* Show all lines. */
        Cddb_Load_Album_List (self, FALSE);
    }
}

/*
 * Free priv->album_list
 */
static gboolean
Cddb_Free_Album_List (EtCDDBDialog *self)
{
    EtCDDBDialogPrivate *priv;
    GList *l;

    priv = et_cddb_dialog_get_instance_private (self);

    g_return_val_if_fail (priv->album_list != NULL, FALSE);

    priv->album_list = g_list_first (priv->album_list);

    for (l = priv->album_list; l != NULL; l = g_list_next (l))
    {
        CddbAlbum *cddbalbum = l->data;

        if (cddbalbum)
        {
            g_free(cddbalbum->server_name);
            g_free (cddbalbum->server_cgi_path);
            g_object_unref(cddbalbum->bitmap);

            g_free(cddbalbum->artist_album);
            g_free(cddbalbum->category);
            g_free(cddbalbum->id);
            if (cddbalbum->track_list)
            {
                Cddb_Free_Track_Album_List(cddbalbum->track_list);
                cddbalbum->track_list = NULL;
            }
            g_free(cddbalbum->artist);
            g_free(cddbalbum->album);
            g_free(cddbalbum->genre);
            g_free(cddbalbum->year);

            g_free(cddbalbum);
            cddbalbum = NULL;
        }
    }

    g_list_free (priv->album_list);
    priv->album_list = NULL;

    return TRUE;
}

/*
 * Fields          : artist, title, track, rest
 * CDDB Categories : blues, classical, country, data, folk, jazz, misc, newage, reggae, rock, soundtrack
 */
static gchar *
Cddb_Generate_Request_String_With_Fields_And_Categories_Options (EtCDDBDialog *self)
{
    GString *string;
    guint search_fields;
    guint search_categories;

    /* Init. */
    string = g_string_sized_new (256);

    /* Fields. */
    /* FIXME: Fetch cddb-search-fields "all-set" mask. */
#if 0
    if (search_all_fields)
    {
        g_string_append (string, "&allfields=YES");
    }
    else
    {
        g_string_append (string, "&allfields=NO");
    }
#endif

    search_fields = g_settings_get_flags (MainSettings, "cddb-search-fields");

    if (search_fields & ET_CDDB_SEARCH_FIELD_ARTIST)
    {
        g_string_append (string, "&fields=artist");
    }
    if (search_fields & ET_CDDB_SEARCH_FIELD_TITLE)
    {
        g_string_append (string, "&fields=title");
    }
    if (search_fields & ET_CDDB_SEARCH_FIELD_TRACK)
    {
        g_string_append (string, "&fields=track");
    }
    if (search_fields & ET_CDDB_SEARCH_FIELD_OTHER)
    {
        g_string_append (string, "&fields=rest");
    }

    /* Categories (warning: there is one other CDDB category that is not used
     * here ("data")) */
    search_categories = g_settings_get_flags (MainSettings,
                                              "cddb-search-categories");
    g_string_append (string, "&allcats=NO");

    if (search_categories & ET_CDDB_SEARCH_CATEGORY_BLUES)
    {
        g_string_append (string, "&cats=blues");
    }
    if (search_categories & ET_CDDB_SEARCH_CATEGORY_CLASSICAL)
    {
        g_string_append (string, "&cats=classical");
    }
    if (search_categories & ET_CDDB_SEARCH_CATEGORY_COUNTRY)
    {
        g_string_append (string, "&cats=country");
    }
    if (search_categories & ET_CDDB_SEARCH_CATEGORY_FOLK)
    {
        g_string_append (string, "&cats=folk");
    }
    if (search_categories & ET_CDDB_SEARCH_CATEGORY_JAZZ)
    {
        g_string_append (string, "&cats=jazz");
    }
    if (search_categories & ET_CDDB_SEARCH_CATEGORY_MISC)
    {
        g_string_append (string, "&cats=misc");
    }
    if (search_categories & ET_CDDB_SEARCH_CATEGORY_NEWAGE)
    {
        g_string_append (string, "&cats=newage");
    }
    if (search_categories & ET_CDDB_SEARCH_CATEGORY_REGGAE)
    {
        g_string_append (string, "&cats=reggae");
    }
    if (search_categories & ET_CDDB_SEARCH_CATEGORY_ROCK)
    {
        g_string_append (string, "&cats=rock");
    }
    if (search_categories & ET_CDDB_SEARCH_CATEGORY_SOUNDTRACK)
    {
        g_string_append (string, "&cats=soundtrack");
    }

    return g_string_free (string, FALSE);
}


/*
 * Site FREEDB.ORG - Manual Search
 * Send request (using the HTML search page in freedb.org site) to the CD database
 * to get the list of albums matching to a string.
 */
static gboolean
Cddb_Search_Album_List_From_String_Freedb (EtCDDBDialog *self)
{
    EtCDDBDialogPrivate *priv;
    gint   socket_id;
    gchar *string = NULL;
    gchar *tmp, *tmp1;
    gchar *cddb_in;         // For the request to send
    gchar *cddb_out = NULL; // Answer received
    gchar *cddb_out_tmp;
    gchar *msg;
    gchar *proxy_auth = NULL;
    gchar *cddb_server_name;
    gint   cddb_server_port;
    gchar *cddb_server_cgi_path;
    gboolean proxy_enabled;
    gchar *proxy_hostname;
    guint proxy_port;

    gchar *ptr_cat, *cat_str, *id_str, *art_alb_str;
    gchar *art_alb_tmp = NULL;
    gboolean use_art_alb = FALSE;
    gchar *end_str;
    gchar *html_end_str;
    gchar  buffer[MAX_STRING_LEN+1];
    gint   bytes_written;
    gulong bytes_read_total = 0;
    FILE  *file = NULL;
    gboolean web_search_disabled = FALSE;

    priv = et_cddb_dialog_get_instance_private (self);

    gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,"");

    /* Get words to search... */
    string = g_strdup (gtk_entry_get_text (GTK_ENTRY (priv->search_string_entry)));
    if (!string || g_utf8_strlen(string, -1) <= 0)
        return FALSE;

    /* Format the string of words */
    g_strstrip (string);
    /* Remove the duplicated spaces */
    while ((tmp=strstr(string,"  "))!=NULL) // Search 2 spaces
    {
        tmp1 = tmp + 1;
        while (*tmp1)
            *(tmp++) = *(tmp1++);
        *tmp = '\0';
    }

    Add_String_To_Combo_List(priv->search_string_model, string);

    /* Convert spaces to '+' */
    while ( (tmp=strchr(string,' '))!=NULL )
        *tmp = '+';

    cddb_server_name = g_settings_get_string (MainSettings,
                                              "cddb-manual-search-hostname");
    cddb_server_port = g_settings_get_uint (MainSettings,
                                            "cddb-manual-search-port");
    cddb_server_cgi_path = g_settings_get_string (MainSettings,
                                                  "cddb-manual-search-path");

    /* Connection to the server */
    proxy_enabled = g_settings_get_boolean (MainSettings,
                                            "cddb-proxy-enabled");
    proxy_hostname = g_settings_get_string (MainSettings,
                                            "cddb-proxy-hostname");
    proxy_port = g_settings_get_uint (MainSettings, "cddb-proxy-port");
    if ((socket_id = Cddb_Open_Connection (self,
                                           proxy_enabled
                                           ? proxy_hostname
                                           : cddb_server_name,
                                           proxy_enabled
                                           ? proxy_port
                                           : cddb_server_port)) <= 0)
    {
        g_free (string);
        g_free (cddb_server_name);
        g_free (cddb_server_cgi_path);
        g_free (proxy_hostname);
        return FALSE;
    }

    /* Build request */
    //cddb_in = g_strdup_printf("GET http://www.freedb.org/freedb_search.php?" // In this case, problem with squid cache...
    cddb_in = g_strdup_printf("GET %s%s/freedb_search.php?"
                              "words=%s"
                              "%s"
                              "&grouping=none"
                              " HTTP/1.1\r\n"
                              "Host: %s:%d\r\n"
                              "User-Agent: %s %s\r\n"
                              "%s"
                              "Connection: close\r\n"
                              "\r\n",
                              proxy_enabled ? "http://" : "",
                              proxy_enabled ? cddb_server_name : "",
                              string,
                              (tmp = Cddb_Generate_Request_String_With_Fields_And_Categories_Options (self)),
                              cddb_server_name,cddb_server_port,
                              PACKAGE_NAME, PACKAGE_VERSION,
                              (proxy_auth=Cddb_Format_Proxy_Authentification())
                              );

    g_free(string);
    g_free(tmp);
    g_free(proxy_auth);
    //g_print("Request Cddb_Search_Album_List_From_String_Freedb : '%s'\n", cddb_in);

    // Send the request
    gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,_("Sending request…"));
    while (gtk_events_pending()) gtk_main_iteration();
    if ( (bytes_written=send(socket_id,cddb_in,strlen(cddb_in)+1,0)) < 0)
    {
        Log_Print(LOG_ERROR,_("Cannot send the request (%s)"),g_strerror(errno));
        Cddb_Close_Connection (self, socket_id);
        g_free(cddb_in);
        g_free(string);
        g_free(cddb_server_name);
        g_free(cddb_server_cgi_path);
        g_free (proxy_hostname);
        return FALSE;
    }
    g_free(cddb_in);


    /* Delete previous album list. */
    cddb_album_model_clear (self);
    cddb_track_model_clear (self);

    if (priv->album_list)
    {
        Cddb_Free_Album_List (self);
    }
    gtk_widget_set_sensitive (GTK_WIDGET (priv->stop_search_button), TRUE);
    gtk_widget_set_sensitive (GTK_WIDGET (priv->stop_auto_search_button),
                              TRUE);


    /*
     * Read the answer
     */
    gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,_("Receiving data…"));
    while (gtk_events_pending())
        gtk_main_iteration();

    /* Write result in a file. */
    if (Cddb_Write_Result_To_File (self, socket_id, &bytes_read_total) < 0)
    {
        msg = g_strdup(_("The server returned a bad response"));
        gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,msg);
        Log_Print(LOG_ERROR,"%s",msg);
        g_free(msg);
        g_free(cddb_server_name);
        g_free(cddb_server_cgi_path);
        gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_search_button),FALSE);
        gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_auto_search_button),FALSE);
        return FALSE;
    }

    // Parse server answer : Check returned code in the first line
    if (Cddb_Read_Http_Header(&file,&cddb_out) <= 0 || !cddb_out) // Order is important!
    {
        msg = g_strdup_printf(_("The server returned a bad response: %s"),cddb_out);
        gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,msg);
        Log_Print(LOG_ERROR,"%s",msg);
        g_free(msg);
        g_free(cddb_out);
        g_free(cddb_server_name);
        g_free(cddb_server_cgi_path);
        g_free (proxy_hostname);
        gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_search_button),FALSE);
        gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_auto_search_button),FALSE);
        if (file)
            fclose(file);
        return FALSE;
    }
    g_free(cddb_out);

    // Read other lines, and get list of matching albums
    // Composition of a line :
    //  - freedb.org
    // <a href="http://www.freedb.org/freedb_search_fmt.php?cat=rock&id=8c0f0a0b">Bob Dylan / MTV Unplugged</a><br>
    cat_str      = g_strdup("http://www.freedb.org/freedb_search_fmt.php?cat=");
    id_str       = g_strdup("&id=");
    art_alb_str  = g_strdup("\">");
    end_str      = g_strdup("</a>"); //"</a><br>");
    html_end_str = g_strdup("</body>"); // To avoid the cddb lookups to hang
    while ( self && !priv->stop_searching
    && Cddb_Read_Line(&file,&cddb_out) > 0 )
    {
        cddb_out_tmp = cddb_out;
        //g_print("%s\n",cddb_out); // To print received data

        // If the web search is disabled! (ex : http://www.freedb.org/modules.php?name=News&file=article&sid=246)
        // The following string is displayed in the search page
        if (cddb_out != NULL && strstr(cddb_out_tmp,"Sorry, The web-based search is currently down.") != NULL)
        {
            web_search_disabled = TRUE;
            break;
        }

        // We may have severals album in the same line (other version of the same album?)
        // Note : we test that the 'end' delimiter exists to avoid crashes
        while ( cddb_out != NULL && (ptr_cat=strstr(cddb_out_tmp,cat_str)) != NULL && strstr(cddb_out_tmp,end_str) != NULL )
        {
            gchar *ptr_font, *ptr_font1;
            gchar *ptr_id, *ptr_art_alb, *ptr_end;
            gchar *copy;
            CddbAlbum *cddbalbum;

            cddbalbum = g_malloc0(sizeof(CddbAlbum));


            // Parameters of the server used
            cddbalbum->server_name     = g_strdup(cddb_server_name);
            cddbalbum->server_port     = cddb_server_port;
            cddbalbum->server_cgi_path = g_strdup(cddb_server_cgi_path);
            cddbalbum->bitmap          = Cddb_Get_Pixbuf_From_Server_Name(cddbalbum->server_name);

            // Get album category
            cddb_out_tmp = ptr_cat + strlen(cat_str);
            strncpy(buffer,cddb_out_tmp,MAX_STRING_LEN);
            if ( (ptr_id=strstr(buffer,id_str)) != NULL )
                *ptr_id = 0;
            cddbalbum->category = Try_To_Validate_Utf8_String(buffer);


            // Get album ID
            //cddb_out_tmp = strstr(cddb_out_tmp,id_str) + strlen(id_str);
            cddb_out_tmp = ptr_cat + strlen(cat_str) + 2;
            strncpy(buffer,cddb_out_tmp,MAX_STRING_LEN);
            if ( (ptr_art_alb=strstr(buffer,art_alb_str)) != NULL )
                *ptr_art_alb = 0;
            cddbalbum->id = Try_To_Validate_Utf8_String(buffer);


            // Get album and artist names.
            // Note : some names can be like this "<font size=-1>2</font>" (for other version of the same album)
            cddb_out_tmp = strstr(cddb_out_tmp,art_alb_str) + strlen(art_alb_str);
            strncpy(buffer,cddb_out_tmp,MAX_STRING_LEN);
            if ( (ptr_end=strstr(buffer,end_str)) != NULL )
                *ptr_end = 0;
            if ( (ptr_font=strstr(buffer,"</font>")) != NULL )
            {
                copy = NULL;
                *ptr_font = 0;
                if ( (ptr_font1=strstr(buffer,">")) != NULL )
                {
                    copy = g_strdup_printf("%s -> %s",ptr_font1+1,art_alb_tmp);
                    cddbalbum->other_version = TRUE;
                }else
                {
                    copy = g_strdup(buffer);
                }

            }else
            {
                copy = g_strdup(buffer);
                art_alb_tmp = cddbalbum->artist_album;
                use_art_alb = TRUE;
            }

            cddbalbum->artist_album = Try_To_Validate_Utf8_String(copy);
            g_free(copy);

            if (use_art_alb)
            {
                art_alb_tmp = cddbalbum->artist_album;
                use_art_alb = FALSE;
            }


            // New position the search the next string
            cddb_out_tmp = strstr(cddb_out_tmp,end_str) + strlen(end_str);

            priv->album_list = g_list_append(priv->album_list,cddbalbum);
        }

        // To avoid the cddb lookups to hang (Patch from Paul Giordano)
        /* It appears that on some systems that cddb lookups continue to attempt
         * to get data from the socket even though the other system has completed
         * sending. Here we see if the actual end of data is in the last block read.
         * In the case of the html scan, the </body> tag is used because there's
         * no crlf followint the </html> tag.
         */
        if (strstr(cddb_out_tmp,html_end_str)!=NULL)
        {
            g_free(cddb_out);
            break;
        }
        g_free(cddb_out);
    }
    g_free(cat_str); g_free(id_str); g_free(art_alb_str); g_free(end_str); g_free(html_end_str);
    g_free(cddb_server_name);
    g_free(cddb_server_cgi_path);
    g_free (proxy_hostname);

    // Close file opened for reading lines
    if (file)
    {
        fclose(file);
        file = NULL;
    }

    gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_search_button),FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_auto_search_button),FALSE);

    /* Close connection. */
    Cddb_Close_Connection (self, socket_id);

    if (web_search_disabled)
        msg = g_strdup_printf(_("Sorry, the web-based search is currently not available"));
    else
        msg = g_strdup_printf(ngettext("Found one matching album","Found %d matching albums",g_list_length(priv->album_list)),g_list_length(priv->album_list));
    gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,msg);
    g_free(msg);

    // Initialize the button
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->display_red_lines_toggle),FALSE);

    /* Load the albums found in the list. */
    Cddb_Load_Album_List (self, FALSE);

    return TRUE;
}

/*
 * Site GNUDB.ORG - Manual Search
 * Send request (using the HTML search page in freedb.org site) to the CD database
 * to get the list of albums matching to a string.
 */
static gboolean
Cddb_Search_Album_List_From_String_Gnudb (EtCDDBDialog *self)
{
    EtCDDBDialogPrivate *priv;
    gint   socket_id;
    gchar *string = NULL;
    gchar *tmp, *tmp1;
    gchar *cddb_in;         // For the request to send
    gchar *cddb_out = NULL; // Answer received
    gchar *cddb_out_tmp;
    gchar *msg;
    gchar *proxy_auth = NULL;
    gchar *cddb_server_name;
    gint   cddb_server_port;
    gchar *cddb_server_cgi_path;
    gboolean proxy_enabled;
    gchar *proxy_hostname;
    guint proxy_port;

    gchar *ptr_cat, *cat_str, *art_alb_str;
    gchar *end_str;
    gchar *ptr_sraf, *sraf_str, *sraf_end_str;
    gchar *html_end_str;
    gchar  buffer[MAX_STRING_LEN+1];
    gint   bytes_written;
    gulong bytes_read_total = 0;
    FILE  *file;
    gint   num_albums = 0;
    gint   total_num_albums = 0;

    gchar *next_page = NULL;
    gint   next_page_cpt = 0;
    gboolean next_page_found;

    priv = et_cddb_dialog_get_instance_private (self);

    gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,"");

    /* Get words to search... */
    string = g_strdup (gtk_entry_get_text (GTK_ENTRY (priv->search_string_entry)));
    if (!string || g_utf8_strlen(string, -1) <= 0)
        return FALSE;

    /* Format the string of words */
    g_strstrip (string);
    /* Remove the duplicated spaces */
    while ((tmp=strstr(string,"  "))!=NULL) // Search 2 spaces
    {
        tmp1 = tmp + 1;
        while (*tmp1)
            *(tmp++) = *(tmp1++);
        *tmp = '\0';
    }

    Add_String_To_Combo_List(priv->search_string_model, string);

    /* Convert spaces to '+' */
    while ( (tmp=strchr(string,' '))!=NULL )
        *tmp = '+';

    /* Delete previous album list. */
    cddb_album_model_clear (self);
    cddb_track_model_clear (self);

    if (priv->album_list)
    {
        Cddb_Free_Album_List (self);
    }
    gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_search_button),TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_auto_search_button),TRUE);


    // Do a loop to load all the pages of results
    do
    {
        cddb_server_name = g_settings_get_string (MainSettings,
                                                  "cddb-manual-search-hostname");
        cddb_server_port = g_settings_get_uint (MainSettings,
                                                "cddb-manual-search-port");
        cddb_server_cgi_path = g_settings_get_string (MainSettings,
                                                      "cddb-manual-search-path");

        /* Connection to the server */
        proxy_enabled = g_settings_get_boolean (MainSettings,
                                                "cddb-proxy-enabled");
        proxy_hostname = g_settings_get_string (MainSettings,
                                                "cddb-proxy-hostname");
        proxy_port = g_settings_get_uint (MainSettings, "cddb-proxy-port");
        if ((socket_id = Cddb_Open_Connection (self,
                                               proxy_enabled ? proxy_hostname
                                                             : cddb_server_name,
                                               proxy_enabled ? proxy_port
                                                             : cddb_server_port)) <= 0)
        {
            g_free(string);
            g_free(cddb_server_name);
            g_free(cddb_server_cgi_path);
            g_free (proxy_hostname);
            gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_search_button),FALSE);
            gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_auto_search_button),FALSE);
            return FALSE;
        }


        /* Build request */
        cddb_in = g_strdup_printf("GET %s%s/search/"
                                  "%s"
                                  "?page=%d"
                                  " HTTP/1.1\r\n"
                                  "Host: %s:%d\r\n"
                                  "User-Agent: %s %s\r\n"
                                  "%s"
                                  "Connection: close\r\n"
                                  "\r\n",
                                  proxy_enabled ? "http://" : "",
                                  proxy_enabled ? cddb_server_name : "",
                                  string,
                                  next_page_cpt,
                                  cddb_server_name,cddb_server_port,
                                  PACKAGE_NAME, PACKAGE_VERSION,
                                  (proxy_auth=Cddb_Format_Proxy_Authentification())
                                  );
        next_page_found = FALSE;
        g_free(proxy_auth);
        //g_print("Request Cddb_Search_Album_List_From_String_Gnudb : '%s'\n", cddb_in);

        // Send the request
        gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,_("Sending request…"));
        while (gtk_events_pending()) gtk_main_iteration();
        if ( (bytes_written=send(socket_id,cddb_in,strlen(cddb_in)+1,0)) < 0)
        {
            Log_Print(LOG_ERROR,_("Cannot send the request (%s)"),g_strerror(errno));
            Cddb_Close_Connection (self, socket_id);
            g_free (cddb_in);
            g_free (string);
            g_free (cddb_server_name);
            g_free (cddb_server_cgi_path);
            g_free (proxy_hostname);
            gtk_widget_set_sensitive (GTK_WIDGET (priv->stop_search_button),
                                      FALSE);
            gtk_widget_set_sensitive (GTK_WIDGET (priv->stop_auto_search_button),
                                      FALSE);
            return FALSE;
        }
        g_free(cddb_in);


        /*
         * Read the answer
         */
        if (total_num_albums != 0)
            msg = g_strdup_printf(_("Receiving data of page %d (album %d/%d)…"),next_page_cpt,num_albums,total_num_albums);
        else
            msg = g_strdup_printf(_("Receiving data of page %d…"),next_page_cpt);

        gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,msg);
        g_free(msg);
        while (gtk_events_pending())
            gtk_main_iteration();

        /* Write result in a file. */
        if (Cddb_Write_Result_To_File (self, socket_id, &bytes_read_total) < 0)
        {
            msg = g_strdup(_("The server returned a bad response"));
            gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,msg);
            Log_Print(LOG_ERROR,"%s",msg);
            g_free(msg);
            g_free(string);
            g_free(cddb_server_name);
            g_free(cddb_server_cgi_path);
            g_free (proxy_hostname);
            gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_search_button),FALSE);
            gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_auto_search_button),FALSE);
            return FALSE;
        }

        // Parse server answer : Check returned code in the first line
        file = NULL;
        if (Cddb_Read_Http_Header(&file,&cddb_out) <= 0 || !cddb_out) // Order is important!
        {
            msg = g_strdup_printf(_("The server returned a bad response: %s"),cddb_out);
            gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,msg);
            Log_Print(LOG_ERROR,"%s",msg);
            g_free(msg);
            g_free(cddb_out);
            g_free(string);
            g_free(cddb_server_name);
            g_free(cddb_server_cgi_path);
            g_free (proxy_hostname);
            gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_search_button),FALSE);
            gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_auto_search_button),FALSE);
            if (file)
                fclose(file);
            return FALSE;
        }
        g_free(cddb_out);

        // The next page if exists will contains this url :
        g_free(next_page);
        next_page = g_strdup_printf("?page=%d",++next_page_cpt);

        // Read other lines, and get list of matching albums
        // Composition of a line :
        //  - gnudb.org
        // <a href="http://www.gnudb.org/cd/ro21123813"><b>Indochine / Le Birthday Album</b></a><br>
        cat_str      = g_strdup("http://www.gnudb.org/cd/");
        art_alb_str  = g_strdup("\"><b>");
        end_str      = g_strdup("</b></a>"); //"</a><br>");
        html_end_str = g_strdup("</body>"); // To avoid the cddb lookups to hang
        // Composition of a line displaying the number of albums
        // <h2>Search Results, 3486 albums found:</h2>
        sraf_str     = g_strdup("<h2>Search Results, ");
        sraf_end_str = g_strdup(" albums found:</h2>");

        while ( self && !priv->stop_searching
        && Cddb_Read_Line(&file,&cddb_out) > 0 )
        {
            cddb_out_tmp = cddb_out;
            //g_print("%s\n",cddb_out); // To print received data

            // Line that displays the number of total albums return by the search
            if ( cddb_out != NULL
            && total_num_albums == 0 // Do it only the first time
            && (ptr_sraf=strstr(cddb_out_tmp,sraf_end_str)) != NULL
            && strstr(cddb_out_tmp,sraf_str) != NULL )
            {
                // Get total number of albums
                ptr_sraf = 0;
                total_num_albums = atoi(cddb_out_tmp + strlen(sraf_str));
            }

            // For GNUDB.ORG : one album per line
            if ( cddb_out != NULL
            && (ptr_cat=strstr(cddb_out_tmp,cat_str)) != NULL
            && strstr(cddb_out_tmp,end_str) != NULL )
            {
                gchar *ptr_art_alb, *ptr_end;
                gchar *valid;
                CddbAlbum *cddbalbum;

                cddbalbum = g_malloc0(sizeof(CddbAlbum));

                // Parameters of the server used
                cddbalbum->server_name     = g_strdup(cddb_server_name);
                cddbalbum->server_port     = cddb_server_port;
                cddbalbum->server_cgi_path = g_strdup(cddb_server_cgi_path);
                cddbalbum->bitmap          = Cddb_Get_Pixbuf_From_Server_Name(cddbalbum->server_name);

                num_albums++;

                // Get album category
                cddb_out_tmp = ptr_cat + strlen(cat_str);
                strncpy(buffer,cddb_out_tmp,MAX_STRING_LEN);
                *(buffer+2) = 0;

                // Check only the 2 first characters to set the right category
                if ( strncmp(buffer,"blues",2)==0 )
                    valid = g_strdup("blues");
                else if ( strncmp(buffer,"classical",2)==0 )
                    valid = g_strdup("classical");
                else if ( strncmp(buffer,"country",2)==0 )
                    valid = g_strdup("country");
                else if ( strncmp(buffer,"data",2)==0 )
                    valid = g_strdup("data");
                else if ( strncmp(buffer,"folk",2)==0 )
                    valid = g_strdup("folk");
                else if ( strncmp(buffer,"jazz",2)==0 )
                    valid = g_strdup("jazz");
                else if ( strncmp(buffer,"misc",2)==0 )
                    valid = g_strdup("misc");
                else if ( strncmp(buffer,"newage",2)==0 )
                    valid = g_strdup("newage");
                else if ( strncmp(buffer,"reggae",2)==0 )
                    valid = g_strdup("reggae");
                else if ( strncmp(buffer,"rock",2)==0 )
                    valid = g_strdup("rock");
                else //if ( strncmp(buffer,"soundtrack",2)==0 )
                    valid = g_strdup("soundtrack");

                cddbalbum->category = valid; //Not useful -> Try_To_Validate_Utf8_String(valid);


                // Get album ID
                cddb_out_tmp = ptr_cat + strlen(cat_str) + 2;
                strncpy(buffer,cddb_out_tmp,MAX_STRING_LEN);
                if ( (ptr_art_alb=strstr(buffer,art_alb_str)) != NULL )
                    *ptr_art_alb = 0;
                cddbalbum->id = Try_To_Validate_Utf8_String(buffer);


                // Get album and artist names.
                cddb_out_tmp = strstr(cddb_out_tmp,art_alb_str) + strlen(art_alb_str);
                strncpy(buffer,cddb_out_tmp,MAX_STRING_LEN);
                if ( (ptr_end=strstr(buffer,end_str)) != NULL )
                    *ptr_end = 0;
                cddbalbum->artist_album = Try_To_Validate_Utf8_String(buffer);

                priv->album_list = g_list_append(priv->album_list,cddbalbum);
            }

            // To avoid the cddb lookups to hang (Patch from Paul Giordano)
            /* It appears that on some systems that cddb lookups continue to attempt
             * to get data from the socket even though the other system has completed
             * sending. Here we see if the actual end of data is in the last block read.
             * In the case of the html scan, the </body> tag is used because there's
             * no crlf followint the </html> tag.
             */
            /***if (strstr(cddb_out_tmp,html_end_str)!=NULL)
                break;***/


            // Check if the link to the next results exists to loop again with the next link
            if (cddb_out != NULL && next_page != NULL
            && (strstr(cddb_out_tmp,next_page) != NULL || next_page_cpt < 2) ) // BUG : "next_page_cpt < 2" to fix a bug in gnudb : the page 0 doesn't contain link to the page=1, so we force it...
            {
                next_page_found = TRUE;

                if ( !(next_page_cpt < 2) ) // Don't display message in this case as it will be displayed each line of page 0 and 1
                {
                    msg = g_strdup_printf(_("More results to load…"));
                    gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,msg);
                    g_free(msg);

                    while (gtk_events_pending())
                        gtk_main_iteration();
                }
            }

            g_free(cddb_out);
        }
        g_free(cat_str); g_free(art_alb_str); g_free(end_str); g_free(html_end_str);
        g_free(sraf_str);g_free(sraf_end_str);
        g_free(cddb_server_name);
        g_free(cddb_server_cgi_path);
        g_free (proxy_hostname);

        // Close file opened for reading lines
        if (file)
        {
            fclose(file);
            file = NULL;
        }

        /* Close connection. */
        Cddb_Close_Connection (self, socket_id);

    } while (next_page_found);
    g_free(string);


    gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_search_button),FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_auto_search_button),FALSE);

    msg = g_strdup_printf(ngettext("Found one matching album","Found %d matching albums",num_albums),num_albums);
    gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,msg);
    g_free(msg);

    // Initialize the button
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->display_red_lines_toggle),FALSE);

    /* Load the albums found in the list. */
    Cddb_Load_Album_List (self, FALSE);

    return TRUE;
}

/*
 * Select the function to use according the server adress for the manual search
 *      - freedb.freedb.org
 *      - gnudb.gnudb.org
 */
static gboolean
Cddb_Search_Album_List_From_String (EtCDDBDialog *self)
{
    gchar *hostname = g_settings_get_string (MainSettings,
                                             "cddb-manual-search-hostname");

    if (strstr (hostname, "gnudb") != NULL)
    {
        /* Use gnudb. */
        g_free (hostname);
        return Cddb_Search_Album_List_From_String_Gnudb (self);
    }
    else
    {
        /* Use freedb. */
        g_free (hostname);
        return Cddb_Search_Album_List_From_String_Freedb (self);
    }
}

/*
 * Set CDDB data (from tracks list) into tags of the main file list
 */
static gboolean
Cddb_Set_Track_Infos_To_File_List (EtCDDBDialog *self)
{
    EtCDDBDialogPrivate *priv;
    guint row;
    guint list_length;
    guint rows_to_loop = 0;
    guint selectedcount;
    guint file_selectedcount;
    guint counter = 0;
    GList *file_iterlist = NULL;
    GList *file_selectedrows;
    GList *selectedrows = NULL;
    gchar buffer[256];
    gboolean CddbTrackList_Line_Selected;
    CddbTrackAlbum *cddbtrackalbum = NULL;
    GtkTreeSelection *selection = NULL;
    GtkTreeSelection *file_selection = NULL;
    GtkListStore *fileListModel;
    GtkTreePath *currentPath = NULL;
    GtkTreeIter  currentIter;
    GtkTreeIter *fileIter;
    gpointer iterptr;

    g_return_val_if_fail (ETCore->ETFileDisplayedList != NULL, FALSE);

    priv = et_cddb_dialog_get_instance_private (self);

    // Save the current displayed data
    ET_Save_File_Data_From_UI(ETCore->ETFileDisplayed);

    /* FIXME: Hack! */
    file_selection = et_application_window_browser_get_selection (ET_APPLICATION_WINDOW (MainWindow));
    fileListModel = GTK_LIST_STORE (gtk_tree_view_get_model (gtk_tree_selection_get_tree_view (file_selection)));
    list_length = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(priv->track_list_model), NULL);

    // Take the selected files in the cddb track list, else the full list
    // Note : Just used to calculate "cddb_track_list_length" because
    // "GPOINTER_TO_INT(cddb_track_list->data)" doesn't return the number of the
    // line when "cddb_track_list = g_list_first(GTK_CLIST(CddbTrackCList)->row_list)"
    selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(priv->track_list_view));
    selectedcount = gtk_tree_selection_count_selected_rows(selection);

    /* Check if at least one line was selected. No line selected is equal to all lines selected. */
    CddbTrackList_Line_Selected = FALSE;

    if (selectedcount > 0)
    {
        /* Loop through selected rows only */
        CddbTrackList_Line_Selected = TRUE;
        rows_to_loop = selectedcount;
        selectedrows = gtk_tree_selection_get_selected_rows(selection, NULL);
    } else
    {
        /* Loop through all rows */
        CddbTrackList_Line_Selected = FALSE;
        rows_to_loop = list_length;
    }

    file_selectedcount = gtk_tree_selection_count_selected_rows (file_selection);

    if (file_selectedcount > 0)
    {
        GList *l;

        /* Rows are selected in the file list, apply tags to them only */
        file_selectedrows = gtk_tree_selection_get_selected_rows(file_selection, NULL);

        for (l = file_selectedrows; l != NULL; l = g_list_next (l))
        {
            counter++;
            iterptr = g_malloc0(sizeof(GtkTreeIter));
            if (gtk_tree_model_get_iter (GTK_TREE_MODEL (fileListModel),
                                         (GtkTreeIter *)iterptr,
                                         (GtkTreePath *)l->data))
            {
                file_iterlist = g_list_prepend (file_iterlist, iterptr);
            }

            if (counter == rows_to_loop) break;
        }

        /* Free the useless bit */
        g_list_free_full (file_selectedrows,
                          (GDestroyNotify)gtk_tree_path_free);

    } else /* No rows selected, use the first x items in the list */
    {
        gtk_tree_model_get_iter_first(GTK_TREE_MODEL(fileListModel), &currentIter);

        do
        {
            counter++;
            iterptr = g_memdup(&currentIter, sizeof(GtkTreeIter));
            file_iterlist = g_list_prepend (file_iterlist, iterptr);
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(fileListModel), &currentIter));

        file_selectedcount = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(fileListModel), NULL);
    }

    if (file_selectedcount != rows_to_loop)
    {
        GtkWidget *msgdialog;
        gint response;

        msgdialog = gtk_message_dialog_new(GTK_WINDOW(self),
                                           GTK_DIALOG_MODAL  | GTK_DIALOG_DESTROY_WITH_PARENT,
                                           GTK_MESSAGE_QUESTION,
                                           GTK_BUTTONS_NONE,
                                           "%s",
                                           _("The number of CDDB results does not match the number of selected files"));
        gtk_dialog_add_buttons(GTK_DIALOG(msgdialog),GTK_STOCK_CANCEL,GTK_RESPONSE_CANCEL,GTK_STOCK_APPLY,GTK_RESPONSE_APPLY, NULL);
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(msgdialog),"%s","Do you want to continue?");
        gtk_window_set_title (GTK_WINDOW (msgdialog),
                              _("Write Tag from CDDB"));
        response = gtk_dialog_run(GTK_DIALOG(msgdialog));
        gtk_widget_destroy(msgdialog);

        if (response != GTK_RESPONSE_APPLY)
        {
            g_list_free_full (file_iterlist, (GDestroyNotify)g_free);
            //gdk_window_raise(CddbWindow->window);
            return FALSE;
        }
    }

    file_iterlist = g_list_reverse (file_iterlist);
    //ET_Debug_Print_File_List (NULL, __FILE__, __LINE__, __FUNCTION__);

    for (row=0; row < rows_to_loop; row++)
    {
        if (CddbTrackList_Line_Selected == FALSE)
        {
            if(row == 0)
                currentPath = gtk_tree_path_new_first();
            else
                gtk_tree_path_next(currentPath);
        } else /* (e.g.: if CddbTrackList_Line_Selected == TRUE) */
        {
            if(row == 0)
            {
                selectedrows = g_list_first(selectedrows);
                currentPath = (GtkTreePath *)selectedrows->data;
            } else
            {
                selectedrows = g_list_next(selectedrows);
                currentPath = (GtkTreePath *)selectedrows->data;
            }
        }

        if (gtk_tree_model_get_iter (GTK_TREE_MODEL (priv->track_list_model),
                                     &currentIter, currentPath))
        {
            gtk_tree_model_get (GTK_TREE_MODEL (priv->track_list_model),
                                &currentIter, CDDB_TRACK_LIST_DATA,
                                &cddbtrackalbum, -1);
        }
        else
        {
            g_warning ("Iter not found matching path in CDDB track list model");
        }

        /* Set values in the ETFile. */
        if (g_settings_get_boolean (MainSettings, "cddb-dlm-enabled"))
        {
            // RQ : this part is ~ equal to code for '!CDDB_USE_DLM', but uses '*etfile' instead of 'etfile'
            ET_File **etfile = NULL;
            File_Name *FileName = NULL;
            File_Tag *FileTag = NULL;
            guint set_fields;

            gtk_tree_model_get(GTK_TREE_MODEL(priv->track_list_model), &currentIter,
                               CDDB_TRACK_LIST_ETFILE, &etfile, -1);

            /* Tag fields. */
            set_fields = g_settings_get_flags (MainSettings, "cddb-set-fields");

            if (set_fields != 0)
            {
                // Allocation of a new FileTag
                FileTag = ET_File_Tag_Item_New();
                ET_Copy_File_Tag_Item(*etfile,FileTag);

                if (set_fields & ET_CDDB_SET_FIELD_TITLE)
                {
                    ET_Set_Field_File_Tag_Item (&FileTag->title,
                                                cddbtrackalbum->track_name);
                }

                if ((set_fields & ET_CDDB_SET_FIELD_ARTIST)
                    && cddbtrackalbum->cddbalbum->artist)
                {
                    ET_Set_Field_File_Tag_Item (&FileTag->artist,
                                                cddbtrackalbum->cddbalbum->artist);
                }

                if ((set_fields & ET_CDDB_SET_FIELD_ALBUM)
                    && cddbtrackalbum->cddbalbum->album)
                {
                    ET_Set_Field_File_Tag_Item (&FileTag->album,
                                                cddbtrackalbum->cddbalbum->album);
                }

                if ((set_fields & ET_CDDB_SET_FIELD_YEAR)
                    && cddbtrackalbum->cddbalbum->year)
                {
                    ET_Set_Field_File_Tag_Item (&FileTag->year,
                                                cddbtrackalbum->cddbalbum->year);
                }

                if (set_fields & ET_CDDB_SET_FIELD_TRACK)
                {
                    snprintf (buffer, sizeof (buffer), "%s",
                              et_track_number_to_string (cddbtrackalbum->track_number));

                    ET_Set_Field_File_Tag_Item (&FileTag->track, buffer);
                }

                if (set_fields & ET_CDDB_SET_FIELD_TRACK_TOTAL)
                {
                    snprintf (buffer, sizeof (buffer), "%s",
                              et_track_number_to_string (list_length));

                    ET_Set_Field_File_Tag_Item (&FileTag->track_total, buffer);
                }

                if ((set_fields & ET_CDDB_SET_FIELD_GENRE)
                    && (cddbtrackalbum->cddbalbum->genre
                        || cddbtrackalbum->cddbalbum->category))
                {
                    if (cddbtrackalbum->cddbalbum->genre
                        && g_utf8_strlen (cddbtrackalbum->cddbalbum->genre, -1) > 0)
                    {
                        ET_Set_Field_File_Tag_Item (&FileTag->genre,
                                                    Cddb_Get_Id3_Genre_From_Cddb_Genre (cddbtrackalbum->cddbalbum->genre));
                    }
                    else
                    {
                        ET_Set_Field_File_Tag_Item (&FileTag->genre,
                                                    Cddb_Get_Id3_Genre_From_Cddb_Genre (cddbtrackalbum->cddbalbum->category));
                    }
                }
            }

            /* Filename field. */
            if (set_fields & ET_CDDB_SET_FIELD_FILENAME)
            {
                gchar *filename_generated_utf8;
                gchar *filename_new_utf8;

                // Allocation of a new FileName
                FileName = ET_File_Name_Item_New();

                // Build the filename with the path
                snprintf (buffer, sizeof (buffer), "%s",
                          et_track_number_to_string (cddbtrackalbum->track_number));

                filename_generated_utf8 = g_strconcat(buffer," - ",cddbtrackalbum->track_name,NULL);
                ET_File_Name_Convert_Character(filename_generated_utf8); // Replace invalid characters
                filename_new_utf8 = ET_File_Name_Generate(*etfile,filename_generated_utf8);

                ET_Set_Filename_File_Name_Item(FileName,filename_new_utf8,NULL);

                g_free(filename_generated_utf8);
                g_free(filename_new_utf8);
            }

            ET_Manage_Changes_Of_File_Data(*etfile,FileName,FileTag);

            /* Then run current scanner if requested. */
            if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (priv->run_scanner_toggle)))
            {
                EtScanDialog *dialog;

                dialog = ET_SCAN_DIALOG (et_application_window_get_scan_dialog (ET_APPLICATION_WINDOW (MainWindow)));

                if (dialog)
                {
                    Scan_Select_Mode_And_Run_Scanner (dialog, *etfile);
                }
            }
        }
        else if (cddbtrackalbum && file_iterlist && file_iterlist->data)
        {
            ET_File   *etfile;
            File_Name *FileName = NULL;
            File_Tag  *FileTag  = NULL;
            guint set_fields;

            fileIter = (GtkTreeIter*) file_iterlist->data;
            etfile = et_application_window_browser_get_et_file_from_iter (ET_APPLICATION_WINDOW (MainWindow),
                                                                          fileIter);

            /* Tag fields. */
            set_fields = g_settings_get_flags (MainSettings, "cddb-set-fields");

            if (set_fields != 0)
            {
                /* Allocation of a new FileTag. */
                FileTag = ET_File_Tag_Item_New ();
                ET_Copy_File_Tag_Item (etfile, FileTag);

                if (set_fields & ET_CDDB_SET_FIELD_TITLE)
                {
                    ET_Set_Field_File_Tag_Item (&FileTag->title,
                                                cddbtrackalbum->track_name);
                }

                if ((set_fields & ET_CDDB_SET_FIELD_ARTIST)
                    && cddbtrackalbum->cddbalbum->artist)
                {
                    ET_Set_Field_File_Tag_Item (&FileTag->artist,
                                                cddbtrackalbum->cddbalbum->artist);
                }

                if ((set_fields & ET_CDDB_SET_FIELD_ALBUM)
                    && cddbtrackalbum->cddbalbum->album)
                {
                    ET_Set_Field_File_Tag_Item (&FileTag->album,
                                                cddbtrackalbum->cddbalbum->album);
                }

                if ((set_fields & ET_CDDB_SET_FIELD_YEAR)
                    && cddbtrackalbum->cddbalbum->year)
                {
                    ET_Set_Field_File_Tag_Item (&FileTag->year,
                                                cddbtrackalbum->cddbalbum->year);
                }

                if (set_fields & ET_CDDB_SET_FIELD_TRACK)
                {
                    snprintf (buffer, sizeof (buffer), "%s",
                              et_track_number_to_string (cddbtrackalbum->track_number));

                    ET_Set_Field_File_Tag_Item (&FileTag->track, buffer);
                }

                if (set_fields & ET_CDDB_SET_FIELD_TRACK_TOTAL)
                {
                    snprintf (buffer, sizeof (buffer), "%s",
                              et_track_number_to_string (list_length));

                    ET_Set_Field_File_Tag_Item (&FileTag->track_total, buffer);
                }

                if ((set_fields & ET_CDDB_SET_FIELD_GENRE)
                    && (cddbtrackalbum->cddbalbum->genre
                        || cddbtrackalbum->cddbalbum->category) )
                {
                    if (cddbtrackalbum->cddbalbum->genre
                        && g_utf8_strlen (cddbtrackalbum->cddbalbum->genre, -1) > 0)
                    {
                        ET_Set_Field_File_Tag_Item (&FileTag->genre,
                                                    Cddb_Get_Id3_Genre_From_Cddb_Genre (cddbtrackalbum->cddbalbum->genre));
                    }
                    else
                    {
                        ET_Set_Field_File_Tag_Item (&FileTag->genre,
                                                    Cddb_Get_Id3_Genre_From_Cddb_Genre (cddbtrackalbum->cddbalbum->category));
                    }
                }
            }

            /*
             * Filename field
             */
            if (set_fields & ET_CDDB_SET_FIELD_FILENAME)
            {
                gchar *filename_generated_utf8;
                gchar *filename_new_utf8;

                // Allocation of a new FileName
                FileName = ET_File_Name_Item_New();

                // Build the filename with the path
                snprintf (buffer, sizeof (buffer), "%s",
                          et_track_number_to_string (cddbtrackalbum->track_number));

                filename_generated_utf8 = g_strconcat(buffer," - ",cddbtrackalbum->track_name,NULL);
                ET_File_Name_Convert_Character(filename_generated_utf8); // Replace invalid characters
                filename_new_utf8 = ET_File_Name_Generate(etfile,filename_generated_utf8);

                ET_Set_Filename_File_Name_Item(FileName,filename_new_utf8,NULL);

                g_free(filename_generated_utf8);
                g_free(filename_new_utf8);
            }

            ET_Manage_Changes_Of_File_Data(etfile,FileName,FileTag);

            /* Then run current scanner if requested. */
            if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (priv->run_scanner_toggle)))
            {
                EtScanDialog *dialog;

                dialog = ET_SCAN_DIALOG (et_application_window_get_scan_dialog (ET_APPLICATION_WINDOW (MainWindow)));

                if (dialog)
                {
                    Scan_Select_Mode_And_Run_Scanner (dialog, etfile);
                }
            }
        }

        if(!file_iterlist->next) break;
        file_iterlist = file_iterlist->next;
    }

    g_list_free_full (file_iterlist, (GDestroyNotify)g_free);

    et_application_window_browser_refresh_list (ET_APPLICATION_WINDOW (MainWindow));
    ET_Display_File_Data_To_UI(ETCore->ETFileDisplayed);

    return TRUE;
}

static void
stop_search (EtCDDBDialog *self)
{
    EtCDDBDialogPrivate *priv;

    priv = et_cddb_dialog_get_instance_private (self);

    priv->stop_searching = TRUE;
}

/*
 * Unselect all rows in the track list
 */
static void
track_list_unselect_all (EtCDDBDialog *self)
{
    EtCDDBDialogPrivate *priv;
    GtkTreeSelection *selection;

    priv = et_cddb_dialog_get_instance_private (self);

    g_return_if_fail (priv->track_list_view != NULL);

    selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (priv->track_list_view));
    if (selection)
    {
        gtk_tree_selection_unselect_all (selection);
    }
}

/*
 * Select all rows in the track list
 */
static void
track_list_select_all (EtCDDBDialog *self)
{
    EtCDDBDialogPrivate *priv;
    GtkTreeSelection *selection;

    priv = et_cddb_dialog_get_instance_private (self);

    selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (priv->track_list_view));

    if (selection)
    {
        gtk_tree_selection_select_all (selection);
    }
}

static gboolean
on_track_list_button_press_event (EtCDDBDialog *self, GdkEventButton *event)
{
    if (event->type == GDK_2BUTTON_PRESS
        && event->button == GDK_BUTTON_PRIMARY)
    {
        /* Double left mouse click */
        track_list_select_all (self);
    }

    return FALSE;
}

static void
et_cddb_dialog_on_response (EtCDDBDialog *self,
                            gint response_id,
                            gpointer user_data)
{
    EtCDDBDialogPrivate *priv;

    priv = et_cddb_dialog_get_instance_private (self);

    switch (response_id)
    {
        case GTK_RESPONSE_CLOSE:
            priv->stop_searching = TRUE;
            et_cddb_dialog_apply_changes (self);
            gtk_widget_hide (GTK_WIDGET (self));
            break;
        case GTK_RESPONSE_DELETE_EVENT:
            break;
        default:
            g_assert_not_reached ();
            break;
    }
}

static void
Cddb_Destroy_Window (EtCDDBDialog *self)
{
    et_cddb_dialog_on_response (self, GTK_RESPONSE_CLOSE, NULL);
}

static void
create_cddb_dialog (EtCDDBDialog *self)
{
    EtCDDBDialogPrivate *priv;
    GtkWidget *VBox, *vbox, *hbox, *notebookvbox;
    GtkWidget *Frame;
    GtkWidget *Table;
    GtkWidget *Label;
    GtkWidget *Button;
    GtkWidget *Separator;
    GtkWidget *ScrollWindow;
    GtkWidget *Icon;
    GtkWidget *combo;
    GtkWidget *paned;
    GtkWidget *notebook;
    const gchar *CddbAlbumList_Titles[] = { NULL, N_("Artist / Album"), N_("Category")}; // Note: don't set "" instead of NULL else this will cause problem with translation language
    const gchar *CddbTrackList_Titles[] = { "#", N_("Track Name"), N_("Duration")};
    GtkCellRenderer* renderer;
    GtkTreeViewColumn* column;
    GtkTreePath *path;

    priv = et_cddb_dialog_get_instance_private (self);

    gtk_window_set_title (GTK_WINDOW (self), _("CDDB Search"));

    g_signal_connect (self, "response",
                      G_CALLBACK (et_cddb_dialog_on_response), NULL);
    g_signal_connect (self, "delete-event",
                      G_CALLBACK (gtk_widget_hide_on_delete), NULL);

    VBox = gtk_dialog_get_content_area (GTK_DIALOG (self));
    gtk_container_set_border_width (GTK_CONTAINER (self), BOX_SPACING);

     /*
      * Cddb NoteBook
      */
    notebook = gtk_notebook_new ();
    gtk_notebook_popup_enable (GTK_NOTEBOOK (notebook));
    gtk_box_pack_start (GTK_BOX (VBox), notebook, FALSE, FALSE, 0);

    /*
     * 1 - Page for automatic search (generate the CDDBId from files)
     */
    Label = gtk_label_new(_("Automatic Search"));

    notebookvbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, BOX_SPACING);
    gtk_container_set_border_width (GTK_CONTAINER (notebookvbox), BOX_SPACING);
    gtk_notebook_append_page (GTK_NOTEBOOK (notebook), notebookvbox, Label);

    hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, BOX_SPACING);
    gtk_box_pack_start(GTK_BOX(notebookvbox),hbox,FALSE,FALSE,0);

    Label = gtk_label_new(_("Request CDDB"));
    gtk_widget_set_halign (Label, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(hbox),Label,FALSE,FALSE,0);

    // Button to generate CddbId and request string from the selected files
    Button = gtk_button_new_from_stock (GTK_STOCK_FIND);
    gtk_box_pack_start (GTK_BOX (hbox), Button, FALSE, FALSE, 0);
    gtk_widget_set_can_default (Button, TRUE);
    gtk_widget_grab_default (Button);
    g_signal_connect_swapped (Button, "clicked",
                              G_CALLBACK (et_cddb_dialog_search_from_selection),
                              self);
    gtk_widget_set_tooltip_text(Button,
                                _("Request automatically the CDDB using the selected files (the order is important) to generate the CddbID"));

    // Button to stop the search
    priv->stop_auto_search_button = gtk_button_new ();
    gtk_container_add (GTK_CONTAINER (priv->stop_auto_search_button),
                       gtk_image_new_from_icon_name ("process-stop",
                                                     GTK_ICON_SIZE_BUTTON));
    gtk_box_pack_start(GTK_BOX(hbox),priv->stop_auto_search_button,FALSE,FALSE,0);
    gtk_button_set_relief(GTK_BUTTON(priv->stop_auto_search_button),GTK_RELIEF_NONE);
    gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_auto_search_button),FALSE);
    g_signal_connect_swapped (priv->stop_auto_search_button, "clicked",
                              G_CALLBACK (stop_search), self);
    gtk_widget_set_tooltip_text (priv->stop_auto_search_button,
                                 _("Stop the search"));

    // Separator line
    Separator = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(hbox),Separator,FALSE,FALSE,0);

    /* Button to quit. */
    Button = gtk_button_new_from_stock(GTK_STOCK_CLOSE);
    gtk_box_pack_end(GTK_BOX(hbox),Button,FALSE,FALSE,0);
    gtk_widget_set_can_default(Button,TRUE);
    g_signal_connect_swapped (Button, "clicked",
                              G_CALLBACK (Cddb_Destroy_Window), self);

    /*
     * 2 - Page for manual search
     */
    Label = gtk_label_new(_("Manual Search"));
    notebookvbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, BOX_SPACING);
    gtk_notebook_append_page (GTK_NOTEBOOK (notebook), notebookvbox, Label);
    gtk_container_set_border_width (GTK_CONTAINER (notebookvbox), BOX_SPACING);

    /*
     * Words to search
     */
    hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, BOX_SPACING);
    gtk_box_pack_start(GTK_BOX(notebookvbox),hbox,FALSE,FALSE,0);

    Label = gtk_label_new(_("Words:"));
    gtk_widget_set_halign (Label, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(hbox),Label,FALSE,FALSE,0);

    g_assert (priv->search_string_model == NULL);
    priv->search_string_model = gtk_list_store_new (MISC_COMBO_COUNT,
                                                G_TYPE_STRING);

    combo = gtk_combo_box_new_with_model_and_entry (GTK_TREE_MODEL (priv->search_string_model));
    g_object_unref (priv->search_string_model);
    gtk_combo_box_set_entry_text_column (GTK_COMBO_BOX (combo),
                                         MISC_COMBO_TEXT);
    gtk_widget_set_size_request (combo, 220, -1);
    gtk_box_pack_start (GTK_BOX (hbox), combo, FALSE, TRUE, 0);
    gtk_widget_set_tooltip_text(combo,
                                _("Enter the words to search (separated by a space or '+')"));
    /* History List. */
    Load_Cddb_Search_String_List (priv->search_string_model, MISC_COMBO_TEXT);

    priv->search_string_entry = gtk_bin_get_child (GTK_BIN (combo));
    g_signal_connect_swapped (priv->search_string_entry, "activate",
                              G_CALLBACK (Cddb_Search_Album_List_From_String),
                              self);
    gtk_entry_set_text (GTK_ENTRY (priv->search_string_entry),"");

    /* Set content of the clipboard if available. */
    gtk_editable_paste_clipboard (GTK_EDITABLE (priv->search_string_entry));

    // Button to run the search
    priv->search_button = gtk_button_new_from_stock(GTK_STOCK_FIND);
    gtk_box_pack_start(GTK_BOX(hbox),priv->search_button,FALSE,FALSE,0);
    gtk_widget_set_can_default(priv->search_button,TRUE);
    gtk_widget_grab_default(priv->search_button);
    g_signal_connect_swapped (priv->search_button, "clicked",
                              G_CALLBACK (Cddb_Search_Album_List_From_String),
                              self);
    g_signal_connect_swapped (GTK_ENTRY (priv->search_string_entry), "changed",
                              G_CALLBACK (update_search_button_sensitivity),
                              self);

    /* Button to stop the search. */
    priv->stop_search_button = gtk_button_new ();
    gtk_container_add (GTK_CONTAINER (priv->stop_search_button),
                       gtk_image_new_from_icon_name ("process-stop",
                                                     GTK_ICON_SIZE_BUTTON));
    gtk_box_pack_start(GTK_BOX(hbox),priv->stop_search_button,FALSE,FALSE,0);
    gtk_button_set_relief(GTK_BUTTON(priv->stop_search_button),GTK_RELIEF_NONE);
    gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_search_button),FALSE);
    g_signal_connect (priv->stop_search_button, "clicked",
                      G_CALLBACK (stop_search), self);
    gtk_widget_set_tooltip_text (priv->stop_search_button, _("Stop the search"));

    /* Button to quit. */
    Button = gtk_button_new_from_stock(GTK_STOCK_CLOSE);
    gtk_box_pack_end(GTK_BOX(hbox),Button,FALSE,FALSE,0);
    gtk_widget_set_can_default(Button,TRUE);
    g_signal_connect_swapped (Button, "clicked",
                              G_CALLBACK (Cddb_Destroy_Window), self);

    /*
     * Search options
     */
    Frame = gtk_frame_new(_("Search In:"));
    gtk_box_pack_start(GTK_BOX(notebookvbox),Frame,FALSE,TRUE,0);

    Table = et_grid_new (7,4);
    gtk_container_add(GTK_CONTAINER(Frame),Table);
    gtk_grid_set_row_spacing (GTK_GRID (Table), 1);
    gtk_grid_set_column_spacing (GTK_GRID (Table), 1);

    {
        gsize i;
        GFlagsClass *flags_class;
        static const struct
        {
            const gchar *label;
            /* const gchar *tooltip; */
        } mapping[] =
        {
            /* Translators: This option is for the previous 'search in' option.
             * For instance, translate this as "Search in:" "Artist". */
            { N_("Artist") },
            /* Translators: This option is for the previous 'search in' option.
             * For instance, translate this as "Search in:" "Album". */
            { N_("Album") },
            /* Translators: This option is for the previous 'search in' option.
             * For instance, translate this as "Search in:" "Track Name". */
            { N_("Track Name") },
            /* Translators: This option is for the previous 'search in' option.
             * For instance, translate this as "Search in:" "Other". */
            { N_("Other") }
        };

        flags_class = g_type_class_ref (ET_TYPE_CDDB_SEARCH_FIELD);

        for (i = 0; i < G_N_ELEMENTS (mapping); i++)
        {
            GFlagsValue *flags_value;
            GtkWidget *widget;

            flags_value = g_flags_get_first_value (flags_class, 1 << i);
            widget = gtk_check_button_new_with_label (gettext (mapping[i].label));
            gtk_widget_set_name (widget, flags_value->value_nick);
            g_object_set_data (G_OBJECT (widget), "flags-type",
                               GSIZE_TO_POINTER (ET_TYPE_CDDB_SEARCH_FIELD));
            g_settings_bind_with_mapping (MainSettings, "cddb-search-fields",
                                          widget, "active",
                                          G_SETTINGS_BIND_DEFAULT,
                                          et_settings_flags_toggle_get,
                                          et_settings_flags_toggle_set,
                                          widget, NULL);
            gtk_grid_attach (GTK_GRID (Table), widget, i, 0, 1, 1);
            g_signal_connect_swapped (widget, "toggled",
                                      G_CALLBACK (update_search_button_sensitivity),
                                      self);
        }
    }

    priv->separator_h = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_grid_attach (GTK_GRID (Table), priv->separator_h, 0, 1, 6, 1);

    {
        gsize i;
        GFlagsClass *flags_class;
        static const struct
        {
            const gchar *label;
            const gchar *tooltip;
        } mapping[] =
        {
            /* Translators: This option is for the previous 'search in' option.
             * For instance, translate this as "Search in:" "Blues". */
            { N_("Blues"), NULL },
            /* Translators: This option is for the previous 'search in' option.
             * For instance, translate this as "Search in:" "Classical". */
            { N_("Classical"), NULL },
            /* Translators: This option is for the previous 'search in' option.
             * For instance, translate this as "Search in:" "Country". */
            { N_("Country"), NULL },
            /* Translators: This option is for the previous 'search in' option.
             * For instance, translate this as "Search in:" "Folk". */
            { N_("Folk"), NULL },
            /* Translators: This option is for the previous 'search in' option.
             * For instance, translate this as "Search in:" "Jazz". */
            { N_("Jazz"), NULL },
            /* Translators: This option is for the previous 'search in' option.
             * For instance, translate this as "Search in:" "Misc". */
            { N_("Misc."),
              N_("others that do not fit in the above categories") },
            /* Translators: This option is for the previous 'search in' option.
             * For instance, translate this as "Search in:" "New age". */
            { N_("New Age"), NULL },
            /* Translators: This option is for the previous 'search in' option.
             * For instance, translate this as "Search in:" "Reggae". */
            { N_("Reggae"), NULL },
            /* Translators: This option is for the previous 'search in' option.
             * For instance, translate this as "Search in:" "Rock". */
            { N_("Rock"),
              N_("included: funk, soul, rap, pop, industrial, metal, etc.") },
            /* Translators: This option is for the previous 'search in' option.
             * For instance, translate this as "Search in:" "Soundtrack". */
            { N_("Soundtrack"), N_("movies, shows") }
    
        };

        flags_class = g_type_class_ref (ET_TYPE_CDDB_SEARCH_CATEGORY);

        for (i = 0; i < G_N_ELEMENTS (mapping); i++)
        {
            GFlagsValue *flags_value;
            GtkWidget *widget;

            flags_value = g_flags_get_first_value (flags_class, 1 << i);
            widget = gtk_check_button_new_with_label (gettext (mapping[i].label));
            priv->category_toggle[i] = widget;
            gtk_widget_set_tooltip_text (widget, gettext (mapping[i].tooltip));
            gtk_widget_set_name (widget, flags_value->value_nick);
            g_object_set_data (G_OBJECT (widget), "flags-type",
                               GSIZE_TO_POINTER (ET_TYPE_CDDB_SEARCH_CATEGORY));
            g_settings_bind_with_mapping (MainSettings,
                                          "cddb-search-categories", widget,
                                          "active", G_SETTINGS_BIND_DEFAULT,
                                          et_settings_flags_toggle_get,
                                          et_settings_flags_toggle_set,
                                          widget, NULL);
            /* 2 rows of 5 columns each. */
            gtk_grid_attach (GTK_GRID (Table), widget, (i % 5), (i / 5) + 2, 1,
                             1);
            g_signal_connect_swapped (G_OBJECT (widget), "toggled",
                                      G_CALLBACK (update_search_button_sensitivity),
                                      self);
        }

        g_type_class_unref (flags_class);
    }

    /* Button to display/hide the categories. */
    priv->show_categories_toggle = gtk_toggle_button_new_with_label (_("Categories"));
    gtk_grid_attach (GTK_GRID (Table), priv->show_categories_toggle, 6, 0, 1,
                     1);
    g_settings_bind (MainSettings, "cddb-search-show-categories",
                     priv->show_categories_toggle, "active",
                     G_SETTINGS_BIND_DEFAULT);
    g_signal_connect_swapped (priv->show_categories_toggle, "toggled",
                              G_CALLBACK (on_show_categories_toggle_toggled),
                              self);

    /*
     * Results command
     */
    Frame = gtk_frame_new(_("Results:"));
    gtk_box_pack_start(GTK_BOX(VBox),Frame,FALSE,TRUE,0);

    hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, BOX_SPACING);
    gtk_container_add(GTK_CONTAINER(Frame),hbox);

    Label = gtk_label_new(_("Search:"));
    gtk_widget_set_halign (Label, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(hbox),Label,FALSE,FALSE,0);

    g_assert (priv->search_string_in_result_model == NULL);
    priv->search_string_in_result_model = gtk_list_store_new (MISC_COMBO_COUNT,
                                                        G_TYPE_STRING);

    combo = gtk_combo_box_new_with_model_and_entry (GTK_TREE_MODEL (priv->search_string_in_result_model));
    g_object_unref (priv->search_string_in_result_model);
    gtk_combo_box_set_entry_text_column (GTK_COMBO_BOX (combo),
                                         MISC_COMBO_TEXT);
    gtk_box_pack_start (GTK_BOX (hbox), combo, FALSE, FALSE, 0);
    priv->search_string_in_results_entry = gtk_bin_get_child (GTK_BIN (combo));
    g_signal_connect_swapped (priv->search_string_in_results_entry,
                              "activate",
                              G_CALLBACK (find_next_string_in_results),
                              self);
    gtk_widget_set_tooltip_text (priv->search_string_in_results_entry,
                                 _("Enter the words to search in the list below"));

    /* History List. */
    Load_Cddb_Search_String_In_Result_List(priv->search_string_in_result_model, MISC_COMBO_TEXT);

    gtk_entry_set_text (GTK_ENTRY (priv->search_string_in_results_entry), "");

    Button = gtk_button_new ();
    gtk_container_add (GTK_CONTAINER (Button),
                       gtk_image_new_from_icon_name ("go-down",
                                                     GTK_ICON_SIZE_BUTTON));
    gtk_box_pack_start(GTK_BOX(hbox),Button,FALSE,FALSE,0);
    gtk_button_set_relief(GTK_BUTTON(Button),GTK_RELIEF_NONE);
    g_signal_connect_swapped (Button, "clicked",
                              G_CALLBACK (find_next_string_in_results), self);
    gtk_widget_set_tooltip_text(Button,_("Search Next"));

    Button = gtk_button_new ();
    gtk_container_add (GTK_CONTAINER (Button),
                       gtk_image_new_from_icon_name ("go-up",
                                                     GTK_ICON_SIZE_BUTTON));
    gtk_box_pack_start (GTK_BOX (hbox), Button, FALSE, FALSE, 0);
    gtk_button_set_relief (GTK_BUTTON (Button), GTK_RELIEF_NONE);
    g_signal_connect_swapped (Button, "clicked",
                              G_CALLBACK (find_previous_string_in_results),
                              self);
    gtk_widget_set_tooltip_text (Button, _("Search Previous"));

    // Separator line
    Separator = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(hbox),Separator,FALSE,FALSE,0);

    priv->display_red_lines_toggle = gtk_toggle_button_new();
    Icon = gtk_image_new_from_resource ("/org/gnome/EasyTAG/images/red-lines.png");
    gtk_container_add(GTK_CONTAINER(priv->display_red_lines_toggle),Icon);
    gtk_box_pack_start(GTK_BOX(hbox),priv->display_red_lines_toggle,FALSE,FALSE,0);
    gtk_button_set_relief(GTK_BUTTON(priv->display_red_lines_toggle),GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(priv->display_red_lines_toggle,_("Show only red lines (or show all lines) in the 'Artist / Album' list"));
    g_signal_connect_swapped (priv->display_red_lines_toggle, "toggled",
                              G_CALLBACK (Cddb_Display_Red_Lines_In_Result),
                              self);

    Icon = gtk_image_new_from_resource ("/org/gnome/EasyTAG/images/unselect-all.png");
    Button = gtk_button_new ();
    gtk_container_add (GTK_CONTAINER (Button), Icon);
    gtk_box_pack_end (GTK_BOX (hbox), Button, FALSE, FALSE, 0);
    gtk_button_set_relief (GTK_BUTTON (Button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text (Button, _("Unselect all lines"));
    g_signal_connect_swapped (Button, "clicked",
                              G_CALLBACK (track_list_unselect_all), self);

    Icon = gtk_image_new_from_resource ("/org/gnome/EasyTAG/images/invert-selection.png");
    Button = gtk_button_new ();
    gtk_container_add (GTK_CONTAINER (Button), Icon);
    gtk_box_pack_end (GTK_BOX (hbox), Button, FALSE, FALSE, 0);
    gtk_button_set_relief (GTK_BUTTON (Button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text (Button, _("Invert lines selection"));
    g_signal_connect_swapped (Button, "clicked",
                              G_CALLBACK (Cddb_Track_List_Invert_Selection),
                              self);

    Button = gtk_button_new ();
    gtk_container_add (GTK_CONTAINER (Button),
                       gtk_image_new_from_icon_name ("edit-select-all",
                                                     GTK_ICON_SIZE_BUTTON));
    gtk_box_pack_end (GTK_BOX (hbox), Button, FALSE, FALSE, 0);
    gtk_button_set_relief (GTK_BUTTON (Button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text (Button, _("Select all lines"));
    g_signal_connect_swapped (Button, "clicked",
                              G_CALLBACK (track_list_select_all), self);

    /*
     * Result of search
     */
    paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start (GTK_BOX (VBox), paned, TRUE, TRUE, 0);

    /* List of albums. */
    ScrollWindow = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ScrollWindow),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(GTK_WIDGET(ScrollWindow),-1,100);
    gtk_paned_pack1 (GTK_PANED (paned), ScrollWindow, TRUE, FALSE);

    priv->album_list_model = gtk_list_store_new(CDDB_ALBUM_LIST_COUNT,
                                            GDK_TYPE_PIXBUF,
                                            G_TYPE_STRING,
                                            G_TYPE_STRING,
                                            G_TYPE_POINTER,
                                            PANGO_TYPE_STYLE,
                                            G_TYPE_INT,
                                            GDK_TYPE_RGBA);
    priv->album_list_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(priv->album_list_model));
    g_object_unref (priv->album_list_model);

    renderer = gtk_cell_renderer_pixbuf_new();
    column = gtk_tree_view_column_new_with_attributes(_(CddbAlbumList_Titles[0]), renderer,
                                                      "pixbuf",         CDDB_ALBUM_LIST_PIXBUF,
                                                      NULL);
    gtk_tree_view_column_set_resizable(column, FALSE);
    gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_AUTOSIZE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(priv->album_list_view), column);

    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes(_(CddbAlbumList_Titles[1]), renderer,
                                                      "text",           CDDB_ALBUM_LIST_ALBUM,
                                                      "weight",         CDDB_ALBUM_LIST_FONT_WEIGHT,
                                                      "style",          CDDB_ALBUM_LIST_FONT_STYLE,
                                                      "foreground-rgba", CDDB_ALBUM_LIST_FOREGROUND_COLOR,
                                                      NULL);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_AUTOSIZE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(priv->album_list_view), column);

    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes(_(CddbAlbumList_Titles[2]), renderer,
                                                      "text",           CDDB_ALBUM_LIST_CATEGORY,
                                                      "weight",         CDDB_ALBUM_LIST_FONT_WEIGHT,
                                                      "style",          CDDB_ALBUM_LIST_FONT_STYLE,
                                                      "foreground-rgba", CDDB_ALBUM_LIST_FOREGROUND_COLOR,
                                                      NULL);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_AUTOSIZE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(priv->album_list_view), column);
    //gtk_tree_view_columns_autosize(GTK_TREE_VIEW(priv->album_list_view));

    gtk_container_add(GTK_CONTAINER(ScrollWindow), priv->album_list_view);

    path = gtk_tree_path_new_first ();
    gtk_tree_view_set_cursor (GTK_TREE_VIEW (priv->album_list_view), path, NULL,
                              FALSE);
    gtk_tree_path_free (path);
    g_signal_connect_swapped (gtk_tree_view_get_selection (GTK_TREE_VIEW (priv->album_list_view)),
                              "changed", G_CALLBACK (show_album_info), self);
    g_signal_connect_swapped (gtk_tree_view_get_selection (GTK_TREE_VIEW (priv->album_list_view)),
                              "changed",
                              G_CALLBACK (Cddb_Get_Album_Tracks_List_CB),
                              self);

    // List of tracks
    ScrollWindow = gtk_scrolled_window_new(NULL,NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ScrollWindow),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);


    gtk_widget_set_size_request(GTK_WIDGET(ScrollWindow), -1, 100);
    gtk_paned_pack2 (GTK_PANED (paned), ScrollWindow, TRUE, FALSE);

    priv->track_list_model = gtk_list_store_new(CDDB_TRACK_LIST_COUNT,
                                            G_TYPE_UINT,
                                            G_TYPE_STRING,
                                            G_TYPE_STRING,
                                            G_TYPE_POINTER,
                                            G_TYPE_POINTER);
    priv->track_list_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(priv->track_list_model));
    g_object_unref (priv->track_list_model);
    renderer = gtk_cell_renderer_text_new();
    g_object_set(G_OBJECT(renderer), "xalign", 1.0, NULL); // Align to the right
    column = gtk_tree_view_column_new_with_attributes(_(CddbTrackList_Titles[0]), renderer,
                                                      "text", CDDB_TRACK_LIST_NUMBER, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(priv->track_list_view), column);
    gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(priv->track_list_model), SORT_LIST_NUMBER,
                                    Cddb_Track_List_Sort_Func, GINT_TO_POINTER(SORT_LIST_NUMBER), NULL);
    gtk_tree_view_column_set_sort_column_id(column, SORT_LIST_NUMBER);
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes(_(CddbTrackList_Titles[1]), renderer,
                                                      "text", CDDB_TRACK_LIST_NAME, NULL);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_AUTOSIZE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(priv->track_list_view), column);
    gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(priv->track_list_model), SORT_LIST_NAME,
                                    Cddb_Track_List_Sort_Func, GINT_TO_POINTER(SORT_LIST_NAME), NULL);
    gtk_tree_view_column_set_sort_column_id(column, SORT_LIST_NAME);

    renderer = gtk_cell_renderer_text_new();
    g_object_set(G_OBJECT(renderer), "xalign", 1.0, NULL); // Align to the right
    column = gtk_tree_view_column_new_with_attributes(_(CddbTrackList_Titles[2]), renderer,
                                                      "text", CDDB_TRACK_LIST_TIME, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(priv->track_list_view), column);

    //gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(priv->track_list_model), SORT_LIST_NUMBER, GTK_SORT_ASCENDING);
    gtk_tree_view_set_reorderable(GTK_TREE_VIEW(priv->track_list_view), TRUE);

    gtk_container_add(GTK_CONTAINER(ScrollWindow),priv->track_list_view);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(priv->track_list_view)),
                                GTK_SELECTION_MULTIPLE);
    g_signal_connect_swapped (gtk_tree_view_get_selection (GTK_TREE_VIEW (priv->track_list_view)),
                              "changed",
                              G_CALLBACK (Cddb_Track_List_Row_Selected), self);
    g_signal_connect_swapped (priv->track_list_view, "button-press-event",
                              G_CALLBACK (on_track_list_button_press_event),
                              self);
    gtk_widget_set_tooltip_text(priv->track_list_view, _("Select lines to 'apply' to "
        "your files list. All lines will be processed if no line is selected.\n"
        "You can also reorder lines in this list before using 'apply' button."));

    /*
     * Apply results to fields...
     */
    Frame = gtk_frame_new(_("Set Into:"));
    gtk_box_pack_start(GTK_BOX(VBox),Frame,FALSE,TRUE,0);

    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, BOX_SPACING);
    gtk_container_add(GTK_CONTAINER(Frame),vbox);

    hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, BOX_SPACING);
    gtk_box_pack_start(GTK_BOX(vbox),hbox,FALSE,FALSE,0);

    {
        gsize i;
        GFlagsClass *flags_class;
        static const struct
        {
            const gchar *label;
            /* const gchar *tooltip; */
        } mapping[] =
        {
            { N_("Filename") },
            { N_("Title") },
            { N_("Artist") },
            { N_("Album") },
            { N_("Year") },
            { N_("Track #") },
            { N_("# Tracks") },
            { N_("Genre") }
        };

        flags_class = g_type_class_ref (ET_TYPE_CDDB_SET_FIELD);

        for (i = 0; i < G_N_ELEMENTS (mapping); i++)
        {
            GFlagsValue *flags_value;
            GtkWidget *widget;

            flags_value = g_flags_get_first_value (flags_class, 1 << i);
            widget = gtk_check_button_new_with_label (gettext (mapping[i].label));
            gtk_widget_set_name (widget, flags_value->value_nick);
            g_object_set_data (G_OBJECT (widget), "flags-type",
                               GSIZE_TO_POINTER (ET_TYPE_CDDB_SET_FIELD));
            g_settings_bind_with_mapping (MainSettings, "cddb-set-fields",
                                          widget, "active",
                                          G_SETTINGS_BIND_DEFAULT,
                                          et_settings_flags_toggle_get,
                                          et_settings_flags_toggle_set,
                                          widget, NULL);
            gtk_box_pack_start (GTK_BOX (hbox), widget, FALSE, FALSE, 2);
            g_signal_connect_swapped (G_OBJECT (widget), "toggled",
                                      G_CALLBACK (update_apply_button_sensitivity),
                                      self);
        }

        g_type_class_unref (flags_class);
    }

    hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, BOX_SPACING);
    gtk_box_pack_start(GTK_BOX(vbox),hbox,FALSE,FALSE,0);

    // Check box to run the scanner
    priv->run_scanner_toggle = gtk_check_button_new_with_label(_("Run the current scanner for each file"));
    gtk_box_pack_start(GTK_BOX(hbox),priv->run_scanner_toggle,FALSE,TRUE,0);
    g_settings_bind (MainSettings, "cddb-run-scanner",
                     priv->run_scanner_toggle, "active",
                     G_SETTINGS_BIND_DEFAULT);
    gtk_widget_set_tooltip_text(priv->run_scanner_toggle,_("When activating this option, after loading the "
        "fields, the current selected scanner will be ran (the scanner window must be opened)."));

    // Check box to use DLM (also used in the preferences window)
    priv->use_dlm2_toggle = gtk_check_button_new_with_label(_("Match lines with the Levenshtein algorithm"));
    gtk_box_pack_start(GTK_BOX(hbox),priv->use_dlm2_toggle,FALSE,FALSE,0);
    g_settings_bind (MainSettings, "cddb-dlm-enabled", priv->use_dlm2_toggle,
                     "active", G_SETTINGS_BIND_DEFAULT);
    gtk_widget_set_tooltip_text(priv->use_dlm2_toggle,_("When activating this option, the "
        "Levenshtein algorithm (DLM: Damerau-Levenshtein Metric) will be used "
        "to match the CDDB title against every filename in the current folder, "
        "and to select the best match. This will be used when selecting the "
        "corresponding audio file, or applying CDDB results, instead of using "
        "directly the position order."));

    /* Button to apply. */
    priv->apply_button = gtk_button_new_from_stock(GTK_STOCK_APPLY);
    gtk_box_pack_end(GTK_BOX(hbox),priv->apply_button,FALSE,FALSE,0);
    g_signal_connect_swapped (priv->apply_button, "clicked",
                              G_CALLBACK (Cddb_Set_Track_Infos_To_File_List),
                              self);
    gtk_widget_set_tooltip_text(priv->apply_button,_("Load the selected lines or all lines (if no line selected)."));

    /*
     * Status bar
     */
    priv->status_bar = gtk_statusbar_new();
    gtk_box_pack_start(GTK_BOX(VBox),priv->status_bar,FALSE,TRUE,0);
    gtk_widget_set_size_request(priv->status_bar, 300, -1);
    priv->status_bar_context = gtk_statusbar_get_context_id(GTK_STATUSBAR(priv->status_bar),"Messages");
    gtk_statusbar_push (GTK_STATUSBAR (priv->status_bar), priv->status_bar_context,
                        _("Ready to search"));

    g_signal_emit_by_name (priv->search_string_entry, "changed");
    priv->stop_searching = FALSE;

    /* TODO: Force resize window? */
    g_signal_emit_by_name (priv->show_categories_toggle, "toggled");
}

/*
 * For the configuration file...
 */
void
et_cddb_dialog_apply_changes (EtCDDBDialog *self)
{
    EtCDDBDialogPrivate *priv;

    g_return_if_fail (ET_CDDB_DIALOG (self));

    priv = et_cddb_dialog_get_instance_private (self);

    /* Save combobox history lists before exit. */
    Save_Cddb_Search_String_List(priv->search_string_model, MISC_COMBO_TEXT);
    Save_Cddb_Search_String_In_Result_List(priv->search_string_in_result_model, MISC_COMBO_TEXT);
}

/*
 * Sort the track list
 */
static gint
Cddb_Track_List_Sort_Func (GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
                           gpointer data)
{
    gint sortcol = GPOINTER_TO_INT(data);
    gchar *text1, *text1cp;
    gchar *text2, *text2cp;
    gint num1;
    gint num2;
    gint ret = 0;

    switch (sortcol)
    {
        case SORT_LIST_NUMBER:
            gtk_tree_model_get(model, a, CDDB_TRACK_LIST_NUMBER, &num1, -1);
            gtk_tree_model_get(model, b, CDDB_TRACK_LIST_NUMBER, &num2, -1);
            if (num1 < num2)
                return -1;
            else if(num1 > num2)
                return 1;
            else
                return 0;
            break;

        case SORT_LIST_NAME:
            gtk_tree_model_get(model, a, CDDB_TRACK_LIST_NAME, &text1, -1);
            gtk_tree_model_get(model, b, CDDB_TRACK_LIST_NAME, &text2, -1);
            text1cp = g_utf8_collate_key_for_filename(text1, -1);
            text2cp = g_utf8_collate_key_for_filename(text2, -1);
            // Must be the same rules as "ET_Comp_Func_Sort_File_By_Ascending_Filename" to be
            // able to sort in the same order files in cddb and in the file list.
            ret = g_settings_get_boolean (MainSettings,
                                          "sort-case-sensitive") ? strcmp (text1cp, text2cp)
                                                                 : strcasecmp (text1cp, text2cp);

            g_free(text1);
            g_free(text2);
            g_free(text1cp);
            g_free(text2cp);
            break;
    }

    return ret;
}

/*
 * Read one line (of the connection) into cddb_out.
 * return  : -1 on error
 *            0 if no more line to read (EOF)
 *            1 if more lines to read
 *
 * Server answser is formated like this :
 *
 * HTTP/1.1 200 OK\r\n                              }
 * Server: Apache/1.3.19 (Unix) PHP/4.0.4pl1\r\n    } "Header"
 * Connection: close\r\n                            }
 * \r\n
 * <html>\n                                         }
 * [...]                                            } "Body"
 */
static gint
Cddb_Read_Line (FILE **file, gchar **cddb_out)
{
    gchar  buffer[MAX_STRING_LEN];
    gchar *result;
    size_t l;

    if (*file == NULL)
    {
        // Open the file for reading the first time
        gchar *file_path;

        file_path = g_build_filename (g_get_user_cache_dir (), PACKAGE_TARNAME,
                                      CDDB_RESULT_FILE, NULL);

        if ((*file = fopen (file_path, "r")) == 0)
        {
            Log_Print (LOG_ERROR, _("Cannot open file '%s' (%s)"), file_path,
                       g_strerror(errno));
            g_free (file_path);
            return -1; // Error!
        }
        g_free (file_path);
    }

    result = fgets(buffer,sizeof(buffer),*file);
    if (result != NULL)
    {
	l = strlen(buffer);
        if (l > 0 && buffer[l-1] == '\n')
            buffer[l-1] = '\0';

	// Many '\r' chars may be present
        while ((l = strlen(buffer)) > 0 && buffer[l-1] == '\r')
            buffer[l-1] = '\0';

        *cddb_out = g_strdup(buffer);
    }else
    {
        // On error, or EOF
        fclose(*file);
        *file = NULL;

        //*cddb_out = NULL;
        *cddb_out = g_strdup(""); // To avoid a crash

        return 0;
    }

    //g_print("Line read: %s\n",*cddb_out);
    return 1;
}


/*
 * Read HTTP header data : from "HTTP/1.1 200 OK" to the blank line
 */
static gint
Cddb_Read_Http_Header (FILE **file, gchar **cddb_out)
{

    // The 'file' is opened (if no error) in this function
    if ( Cddb_Read_Line(file,cddb_out) < 0 )
        return -1; // Error!

    // First line must be : "HTTP/1.1 200 OK"
    if ( !*cddb_out || strncmp("HTTP",*cddb_out,4)!=0 || strstr(*cddb_out,"200 OK")==NULL )
        return -1;

    /* Read until end of the HTTP header up to the next blank line. */
    do
    {
        g_free (*cddb_out);
    }
    while (Cddb_Read_Line (file, cddb_out) > 0
           && *cddb_out && strlen (*cddb_out) > 0);

    //g_print("Http Header : %s\n",*cddb_out);
    return 1;
}

/*
 * Read CDDB header data when requesting a file (cmd=cddb+read+<album genre>+<discid>)
 * Must be read after the HTTP header :
 *
 *      HTTP/1.1 200 OK
 *      Date: Sun, 26 Nov 2006 22:37:13 GMT
 *      Server: Apache/2.0.54 (Debian GNU/Linux) mod_python/3.1.3 Python/2.3.5 PHP/4.3.10-16 proxy_html/2.4 mod_perl/1.999.21 Perl/v5.8.4
 *      Expires: Sun Nov 26 23:37:14 2006
 *      Content-Length: 1013
 *      Connection: close
 *      Content-Type: text/plain; charset=UTF-8
 *
 *      210 newage 710ed208 CD database entry follows (until terminating `.')
 *
 * Cddb Header is the line like this :
 *      210 newage 710ed208 CD database entry follows (until terminating `.')
 */
static gint
Cddb_Read_Cddb_Header (FILE **file, gchar **cddb_out)
{
    if ( Cddb_Read_Line(file,cddb_out) < 0 )
        return -1; // Error!

    // Some requests receive some strange data (arbitrary : less than 10 chars.)
    // at the beginning (2 or 3 characters)... So we read one line more...
    if ( !*cddb_out || strlen(*cddb_out) < 10 )
        if ( Cddb_Read_Line(file,cddb_out) < 0 )
            return -1; // Error!

    //g_print("Cddb Header : %s\n",*cddb_out);

    // Read the line
    // 200 - exact match
    // 210 - multiple exact matches
    // 211 - inexact match
    if ( *cddb_out == NULL
    || (strncmp(*cddb_out,"200",3)!=0
    &&  strncmp(*cddb_out,"210",3)!=0
    &&  strncmp(*cddb_out,"211",3)!=0) )
        return -1;

    return 1;
}



static gboolean
Cddb_Free_Track_Album_List (GList *track_list)
{
    GList *l;

    g_return_val_if_fail (track_list != NULL, FALSE);

    track_list = g_list_first (track_list);

    for (l = track_list; l != NULL; l = g_list_next (l))
    {
        CddbTrackAlbum *cddbtrackalbum = l->data;
        if (cddbtrackalbum)
        {
            g_free(cddbtrackalbum->track_name);
            g_free(cddbtrackalbum);
            cddbtrackalbum = NULL;
        }
    }

    g_list_free (track_list);

    return TRUE;
}

/*
 * Send cddb query using the CddbId generated from the selected files to get the
 * list of albums matching with this cddbid.
 */
gboolean
et_cddb_dialog_search_from_selection (EtCDDBDialog *self)
{
    EtCDDBDialogPrivate *priv;
    gint   socket_id;
    gint   bytes_written;
    gulong bytes_read_total = 0;
    FILE  *file = NULL;

    gchar *cddb_in = NULL; /* For the request to send. */
    gchar *cddb_out = NULL;       /* Answer received */
    gchar *cddb_out_tmp;
    gchar *msg;
    gchar *proxy_auth;
    gchar *cddb_server_name;
    gint   cddb_server_port;
    gchar *cddb_server_cgi_path;
    gboolean proxy_enabled;
    gchar *proxy_hostname;
    guint proxy_port;
    gint   server_try = 0;
    gchar *tmp;
    gchar *query_string;
    gchar *cddb_discid;
    gchar *cddb_end_str;

    guint total_frames = 150;   /* First offset is (almost) always 150 */
    guint disc_length  = 2;     /* and 2s elapsed before first track */

    GtkTreeSelection *file_selection = NULL;
    guint file_selectedcount = 0;
    GtkTreeIter  currentIter;
    guint total_id;
    guint num_tracks;

    gpointer iterptr;

    GtkListStore *fileListModel;
    GtkTreeIter *fileIter;
    GList *file_iterlist = NULL;
    GList *l;

    priv = et_cddb_dialog_get_instance_private (self);

    /* Number of selected files. */
    /* FIXME: Hack! */
    file_selection = et_application_window_browser_get_selection (ET_APPLICATION_WINDOW (MainWindow));
    fileListModel = GTK_LIST_STORE (gtk_tree_view_get_model (gtk_tree_selection_get_tree_view (file_selection)));
    file_selectedcount = gtk_tree_selection_count_selected_rows(file_selection);

    // Create the list 'file_iterlist' of selected files (no selected files => all files selected)
    if (file_selectedcount > 0)
    {
        GList* file_selectedrows = gtk_tree_selection_get_selected_rows(file_selection, NULL);

        for (l = file_selectedrows; l != NULL; l = g_list_next (l))
        {
            iterptr = g_malloc0(sizeof(GtkTreeIter));
            if (gtk_tree_model_get_iter(GTK_TREE_MODEL(fileListModel),
                                        (GtkTreeIter*) iterptr,
                                        (GtkTreePath*) l->data))
            {
                file_iterlist = g_list_prepend (file_iterlist, iterptr);
            }
        }
        g_list_free_full (file_selectedrows,
                          (GDestroyNotify)gtk_tree_path_free);

    } else /* No rows selected, use the whole list */
    {
        gtk_tree_model_get_iter_first(GTK_TREE_MODEL(fileListModel), &currentIter);

        do
        {
            iterptr = g_memdup(&currentIter, sizeof(GtkTreeIter));
            file_iterlist = g_list_prepend (file_iterlist, iterptr);
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(fileListModel), &currentIter));

        file_selectedcount = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(fileListModel), NULL);
    }

    if (file_selectedcount == 0)
    {
        msg = g_strdup_printf(_("No file selected"));
        gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,msg);
        g_free(msg);
        return TRUE;
    }else if (file_selectedcount > 99)
    {
        // The CD redbook standard defines the maximum number of tracks as 99, any
        // queries with more than 99 tracks will never return a result.
        msg = g_strdup_printf(_("More than 99 files selected. Cannot send request"));
        gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,msg);
        g_free(msg);
        return FALSE;
    }else
    {
        msg = g_strdup_printf(ngettext("One file selected","%d files selected",file_selectedcount),file_selectedcount);
        gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,msg);
        g_free(msg);
    }

    // Generate query string and compute discid from the list 'file_iterlist'
    total_id = 0;
    num_tracks = file_selectedcount;
    query_string = g_strdup("");

    for (l = g_list_reverse (file_iterlist); l != NULL; l = g_list_next (l))
    {
        ET_File *etfile;
        gulong secs = 0;

        fileIter = (GtkTreeIter *)l->data;
        etfile = et_application_window_browser_get_et_file_from_iter (ET_APPLICATION_WINDOW (MainWindow),
                                                                      fileIter);

        tmp = query_string;
        if (strlen(query_string)>0)
            query_string = g_strdup_printf("%s+%d", query_string, total_frames);
        else
            query_string = g_strdup_printf("%d", total_frames);
        g_free(tmp);

        secs = etfile->ETFileInfo->duration;
        total_frames += secs * 75;
        disc_length  += secs;
        while (secs > 0)
        {
            total_id = total_id + (secs % 10);
            secs = secs / 10;
        }
    }

    g_list_free_full (file_iterlist, (GDestroyNotify)g_free);

    // Compute CddbId
    cddb_discid = g_strdup_printf("%08x",(guint)(((total_id % 0xFF) << 24) |
                                         (disc_length << 8) | num_tracks));


    /* Delete previous album list. */
    cddb_album_model_clear (self);
    cddb_track_model_clear (self);

    if (priv->album_list)
    {
        Cddb_Free_Album_List (self);
    }
    gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_search_button),TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_auto_search_button),TRUE);


    {
        /*
         * Remote cddb acces
         *
         * Request the two servers
         *   - 1) www.freedb.org
         *   - 2) MusicBrainz Gateway : freedb.musicbrainz.org (in Easytag < 2.1.1, it was: www.mb.inhouse.co.uk)
         */
        while (server_try < 2)
        {
            server_try++;
            if (server_try == 1)
            {
                /* 1st try. */
                cddb_server_name = g_settings_get_string (MainSettings,
                                                          "cddb-automatic-search-hostname");
                cddb_server_port = g_settings_get_uint (MainSettings,
                                                        "cddb-automatic-search-port");
                cddb_server_cgi_path = g_settings_get_string (MainSettings,
                                                              "cddb-automatic-search-path");
 
            }
            else
            {
                /* 2nd try. */
                cddb_server_name = g_settings_get_string (MainSettings,
                                                          "cddb-automatic-search-hostname2");
                cddb_server_port = g_settings_get_uint (MainSettings,
                                                        "cddb-automatic-search-port");
                cddb_server_cgi_path = g_settings_get_string (MainSettings,
                                                              "cddb-automatic-search-path2");
            }

            // Check values
            if (!cddb_server_name || strcmp(cddb_server_name,"")==0)
                continue;

            /* Connection to the server. */
            proxy_enabled = g_settings_get_boolean (MainSettings,
                                                    "cddb-proxy-enabled");
            proxy_hostname = g_settings_get_string (MainSettings,
                                                   "cddb-proxy-hostname");
            proxy_port = g_settings_get_uint (MainSettings, "cddb-proxy-port");

            if ((socket_id = Cddb_Open_Connection (self,
                                                   proxy_enabled
                                                   ? proxy_hostname
                                                   : cddb_server_name,
                                                   proxy_enabled
                                                   ? proxy_port
                                                   : cddb_server_port)) <= 0)
            {
                g_free(cddb_in);
                g_free(cddb_server_name);
                g_free(cddb_server_cgi_path);
                g_free (proxy_hostname);
                return FALSE;
            }

            // CDDB Request (ex: GET /~cddb/cddb.cgi?cmd=cddb+query+0800ac01+1++150+172&hello=noname+localhost+EasyTAG+0.31&proto=1 HTTP/1.1\r\nHost: freedb.freedb.org:80\r\nConnection: close)
            // Without proxy : "GET /~cddb/cddb.cgi?…" but doesn't work with a proxy.
            // With proxy    : "GET http://freedb.freedb.org/~cddb/cddb.cgi?…"
            // proto=1 => ISO-8859-1 - proto=6 => UTF-8
            cddb_in = g_strdup_printf("GET %s%s%s?cmd=cddb+query+"
                                      "%s+"
                                      "%d+%s+"
                                      "%d"
                                      "&hello=noname+localhost+%s+%s"
                                      "&proto=6 HTTP/1.1\r\n"
                                      "Host: %s:%d\r\n"
                                      "%s"
                                      "Connection: close\r\n\r\n",
                                      proxy_enabled ? "http://" : "",
                                      proxy_enabled ? cddb_server_name : "",
                                      cddb_server_cgi_path,
                                      cddb_discid,
                                      num_tracks, query_string,
                                      disc_length,
                                      PACKAGE_NAME, PACKAGE_VERSION,
                                      cddb_server_name,cddb_server_port,
                                      (proxy_auth=Cddb_Format_Proxy_Authentification())
                                      );
            g_free(proxy_auth);
            //g_print("Request Cddb_Search_Album_From_Selected_Files : '%s'\n", cddb_in);

            msg = g_strdup_printf(_("Sending request (CddbId: %s, #tracks: %d, Disc length: %d)…"),
                                cddb_discid,num_tracks,disc_length);
            gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,msg);
            g_free(msg);

            while (gtk_events_pending())
                gtk_main_iteration();

            if ( (bytes_written=send(socket_id,cddb_in,strlen(cddb_in)+1,0)) < 0)
            {
                Log_Print(LOG_ERROR,_("Cannot send the request (%s)"),g_strerror(errno));
                Cddb_Close_Connection (self, socket_id);
                g_free (cddb_in);
                g_free (cddb_server_name);
                g_free (cddb_server_cgi_path);
                g_free (proxy_hostname);
                return FALSE;
            }
            g_free(cddb_in);
            cddb_in = NULL;


            /*
             * Read the answer
             */
            gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,_("Receiving data…"));
            while (gtk_events_pending())
                gtk_main_iteration();

            /* Write result in a file. */
            if (Cddb_Write_Result_To_File (self, socket_id, &bytes_read_total) < 0)
            {
                msg = g_strdup(_("The server returned a bad response"));
                gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,msg);
                Log_Print(LOG_ERROR,"%s",msg);
                g_free(msg);
                g_free(cddb_server_name);
                g_free(cddb_server_cgi_path);
                g_free (proxy_hostname);
                gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_search_button),FALSE);
                gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_auto_search_button),FALSE);
                return FALSE;
            }

            // Parse server answer : Check returned code in the first line
            file = NULL;
            if (Cddb_Read_Http_Header(&file,&cddb_out) <= 0 || !cddb_out) // Order is important!
            {
                msg = g_strdup_printf(_("The server returned a bad response: %s"),cddb_out);
                gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,msg);
                Log_Print(LOG_ERROR,"%s",msg);
                g_free(msg);
                g_free(cddb_out);
                g_free(cddb_server_name);
                g_free(cddb_server_cgi_path);
                g_free (proxy_hostname);
                gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_search_button),FALSE);
                gtk_widget_set_sensitive(GTK_WIDGET(priv->stop_auto_search_button),FALSE);
                if (file)
                    fclose(file);
                return FALSE;
            }
            g_free(cddb_out);

            cddb_end_str = g_strdup(".");

            /*
             * Format :
             * For Freedb, Gnudb, the lines to read are like :
             *      211 Found inexact matches, list follows (until terminating `.')
             *      rock 8f0dc00b Archive / Noise
             *      rock 7b0dd80b Archive / Noise
             *      .
             * For MusicBrainz Cddb Gateway (see http://wiki.musicbrainz.org/CddbGateway), the lines to read are like :
             *      200 jazz 7e0a100a Pink Floyd / Dark Side of the Moon
             */
            while ( self && !priv->stop_searching
            && Cddb_Read_Line(&file,&cddb_out) > 0 )
            {
                cddb_out_tmp = cddb_out;
                //g_print("%s\n",cddb_out);

                // To avoid the cddb lookups to hang (Patch from Paul Giordano)
                /* It appears that on some systems that cddb lookups continue to attempt
                 * to get data from the socket even though the other system has completed
                 * sending. The fix adds one check to the loops to see if the actual
                 * end of data is in the last block read. In this case, the last line
                 * will be a single '.'
                 */
                if ( cddb_out_tmp && strlen(cddb_out_tmp)<=3 && strstr(cddb_out_tmp,cddb_end_str)!=NULL )
                {
                    g_free (cddb_out);
                    break;
                }

                // Compatibility for the MusicBrainz CddbGateway
                if ( cddb_out_tmp && strlen(cddb_out_tmp)>3
                &&  (strncmp(cddb_out_tmp,"200",3)==0
                ||   strncmp(cddb_out_tmp,"210",3)==0
                ||   strncmp(cddb_out_tmp,"211",3)==0) )
                    cddb_out_tmp = cddb_out_tmp + 4;

                // Reading of lines with albums (skiping return code lines :
                // "211 Found inexact matches, list follows (until terminating `.')" )
                if (cddb_out != NULL && strstr(cddb_out_tmp,"/") != NULL)
                {
                    gchar* ptr;
                    CddbAlbum *cddbalbum;

                    cddbalbum = g_malloc0(sizeof(CddbAlbum));

                    // Parameters of the server used
                    cddbalbum->server_name     = g_strdup(cddb_server_name);
                    cddbalbum->server_port     = cddb_server_port;
                    cddbalbum->server_cgi_path = g_strdup(cddb_server_cgi_path);
                    cddbalbum->bitmap          = Cddb_Get_Pixbuf_From_Server_Name(cddbalbum->server_name);

                    // Get album category
                    if ( (ptr = strstr(cddb_out_tmp, " ")) != NULL )
                    {
                        *ptr = 0;
                        cddbalbum->category = Try_To_Validate_Utf8_String(cddb_out_tmp);
                        *ptr = ' ';
                        cddb_out_tmp = ptr + 1;
                    }

                    // Get album ID
                    if ( (ptr = strstr(cddb_out_tmp, " ")) != NULL )
                    {
                        *ptr = 0;
                        cddbalbum->id = Try_To_Validate_Utf8_String(cddb_out_tmp);
                        *ptr = ' ';
                        cddb_out_tmp = ptr + 1;
                    }

                    // Get album and artist names.
                    cddbalbum->artist_album = Try_To_Validate_Utf8_String(cddb_out_tmp);

                    priv->album_list = g_list_append(priv->album_list,cddbalbum);
                }

                g_free(cddb_out);
            }
            g_free(cddb_end_str);
            g_free(cddb_server_name);
            g_free(cddb_server_cgi_path);
            g_free (proxy_hostname);

            /* Close file opened for reading lines. */
            if (file)
            {
                fclose(file);
                file = NULL;
            }

            /* Close connection. */
            Cddb_Close_Connection (self, socket_id);
        }

    }

    msg = g_strdup_printf(ngettext("DiscID '%s' gave one matching album","DiscID '%s' gave %d matching albums",g_list_length(priv->album_list)),cddb_discid,g_list_length(priv->album_list));
    gtk_statusbar_push(GTK_STATUSBAR(priv->status_bar),priv->status_bar_context,msg);
    g_free(msg);

    g_free(cddb_discid);
    g_free(query_string);

    gtk_widget_set_sensitive (GTK_WIDGET (priv->stop_search_button), FALSE);
    gtk_widget_set_sensitive (GTK_WIDGET (priv->stop_auto_search_button),
	                          FALSE);

    /* Initialize the button. */
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (priv->display_red_lines_toggle),
                                  FALSE);

    /* Load the albums found in the list. */
    Cddb_Load_Album_List (self, FALSE);

    return TRUE;
}

/*
 * Returns the corresponding ID3 genre (the name, not the value)
 */
static const gchar *
Cddb_Get_Id3_Genre_From_Cddb_Genre (const gchar *cddb_genre)
{
    guint i;

    g_return_val_if_fail (cddb_genre != NULL, "");

    for (i=0; i<=CDDB_GENRE_MAX; i++)
        if (strcasecmp(cddb_genre,cddb_genre_vs_id3_genre[i][0])==0)
            return cddb_genre_vs_id3_genre[i][1];
    return cddb_genre;
}

/*
 * Returns the pixmap to display following the server name
 */
static GdkPixbuf *
Cddb_Get_Pixbuf_From_Server_Name (const gchar *server_name)
{
    g_return_val_if_fail (server_name != NULL, NULL);

    if (strstr (server_name, "freedb.org"))
        return gdk_pixbuf_new_from_resource ("/org/gnome/EasyTAG/images/freedb.png",
                                             NULL);
    else if (strstr(server_name,"gnudb.org"))
        return gdk_pixbuf_new_from_resource ("/org/gnome/EasyTAG/images/gnudb.png",
                                             NULL);
    else if (strstr(server_name,"musicbrainz.org"))
        return gdk_pixbuf_new_from_resource ("/org/gnome/EasyTAG/images/musicbrainz.png",
                                             NULL);
    else
        return NULL;
}


static gchar *
Cddb_Format_Proxy_Authentification (void)
{
    gchar *username;
    gchar *password;
    gchar *ret;

    username = g_settings_get_string (MainSettings, "cddb-proxy-username");
    password = g_settings_get_string (MainSettings, "cddb-proxy-password");

    if (g_settings_get_boolean (MainSettings, "cddb-proxy-enabled")
        && username && *username )
    {
        const gchar *tempstr;
        gchar *str_encoded;

        tempstr = g_strconcat (username, ":", password, NULL);
        str_encoded = g_base64_encode((const guchar *)tempstr, strlen(tempstr));

        ret = g_strdup_printf("Proxy-authorization: Basic %s\r\n", str_encoded);
        g_free (str_encoded);
    }
    else
    {
        ret = g_strdup ("");
    }

    g_free (username);
    g_free (password);

    return ret;
}

static void
et_cddb_dialog_finalize (GObject *object)
{
    EtCDDBDialog *self;
    EtCDDBDialogPrivate *priv;

    self = ET_CDDB_DIALOG (object);
    priv = et_cddb_dialog_get_instance_private (self);

    if (priv->album_list)
    {
        Cddb_Free_Album_List (self);
    }

    G_OBJECT_CLASS (et_cddb_dialog_parent_class)->finalize (object);
}

static void
et_cddb_dialog_init (EtCDDBDialog *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, ET_TYPE_CDDB_DIALOG,
                                              EtCDDBDialogPrivate);

    self->priv->album_list = NULL;
    self->priv->stop_searching = FALSE;

    create_cddb_dialog (self);
}

static void
et_cddb_dialog_class_init (EtCDDBDialogClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = et_cddb_dialog_finalize;

    g_type_class_add_private (klass, sizeof (EtCDDBDialogPrivate));
}

/*
 * et_cddb_dialog_new:
 *
 * Create a new EtCDDBDialog instance.
 *
 * Returns: a new #EtCDDBDialog
 */
EtCDDBDialog *
et_cddb_dialog_new (void)
{
    return g_object_new (ET_TYPE_CDDB_DIALOG, NULL);
}