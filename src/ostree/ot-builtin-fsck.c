/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree.h"
#include "ostree-cmdprivate.h"
#include "otutil.h"

#include <libsoup/soup.h>

static gboolean opt_quiet;
static gboolean opt_delete;
static gboolean opt_add_tombstones;
static gchar** opt_repair_remotes;

static GOptionEntry options[] = {
  { "add-tombstones", 0, 0, G_OPTION_ARG_NONE, &opt_add_tombstones, "Add tombstones for missing commits", NULL },
  { "quiet", 'q', 0, G_OPTION_ARG_NONE, &opt_quiet, "Only print error messages", NULL },
  { "delete", 0, 0, G_OPTION_ARG_NONE, &opt_delete, "Remove corrupted objects", NULL },
  { "repair-from-remote", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_repair_remotes, "Try do download corrupted files from the remote", NULL },
  { NULL }
};

static gboolean
repair_object (OstreeRepo *repo,
               gchar **repair_remotes,
               const gchar *checksum,
               OstreeObjectType objtype,
               GCancellable *cancellable)
{
  gchar **iter;
  g_autofree gchar *relative_path = NULL;
  glnx_unref_object SoupSession *session = NULL;
  const gchar *type = ostree_object_type_to_string (objtype);

  if (objtype != OSTREE_OBJECT_TYPE_FILE)
    {
      g_printerr ("repair of %s %s failed, not implenented",
                  type, checksum);
      return FALSE;
    }

  session = soup_session_new ();
  relative_path = ostree_get_relative_object_path (checksum, objtype, TRUE);
  for (iter = repair_remotes; *iter; ++iter)
    {
      const gchar *remote = *iter;
      g_autofree gchar *server_url = NULL;
      g_autofree gchar *url = NULL;
      glnx_unref_object SoupRequest *request = NULL;
      g_autoptr(GInputStream) stream = NULL;
      g_autoptr(GInputStream) file_stream = NULL;
      g_autoptr(GFileInfo) file_info = NULL;
      g_autoptr(GVariant) xattrs = NULL;
      g_autoptr(GInputStream) content_stream = NULL;
      guint64 content_len;
      g_autofree guchar *binary_checksum = NULL;
      g_autoptr(GError) error = NULL;

      if (!ostree_repo_remote_get_url (repo, remote, &server_url, &error))
        {
          g_printerr ("repair of %s %s from %s failed, failed to get a URL for remote: %s",
                      type, checksum, remote, error->message);
          continue;
        }

      url = g_strconcat (server_url, "/", relative_path, NULL);
      request = soup_session_request (session, url, &error);
      if (!request)
        {
          g_printerr ("repair of %s %s from %s failed, failed to get a request to URL %s: %s",
                      type, checksum, remote, url, error->message);
          continue;
        }

      stream = soup_request_send (request,
                                  cancellable,
                                  &error);
      if (!stream)
        {
          if (g_cancellable_is_cancelled (cancellable))
            return FALSE;
          g_printerr ("repair of %s %s from %s failed, failed to download the object from URL %s: %s",
                      type, checksum, remote, url, error->message);
          continue;
        }

      if (!ostree_content_stream_parse (TRUE,
                                        stream,
                                        soup_request_get_content_length (request),
                                        FALSE,
                                        &file_stream,
                                        &file_info,
                                        &xattrs,
                                        cancellable,
                                        &error))
        {
          g_printerr ("repair of %s %s from %s failed, failed to parse the content stream: %s",
                      type, checksum, remote, error->message);
          continue;
        }

      if (!ostree_raw_file_to_content_stream (file_stream,
                                              file_info,
                                              xattrs,
                                              &content_stream,
                                              &content_len,
                                              cancellable,
                                              &error))
        {
          g_printerr ("repair of %s %s from %s failed, failed to create a content stream: %s",
                      type, checksum, remote, error->message);
          continue;
        }

      if (!ostree_repo_write_content (repo,
                                      checksum,
                                      content_stream,
                                      content_len,
                                      &binary_checksum,
                                      cancellable,
                                      &error))
        {
          if (opt_delete)
            (void) ostree_repo_delete_object (repo, objtype, checksum, cancellable, NULL);
          g_printerr ("repair of %s %s from %s failed, failed to write object to the repository: %s",
                      type, checksum, remote, error->message);
          continue;
        }

      return TRUE;
    }

  return FALSE;
}

static gboolean
load_and_fsck_one_object (OstreeRepo            *repo,
                          const char            *checksum,
                          OstreeObjectType       objtype,
                          gchar                **repair_remotes,
                          gboolean              *out_found_corruption,
                          GCancellable          *cancellable,
                          GError               **error)
{
  gboolean ret = FALSE;
  gboolean missing = FALSE;
  g_autoptr(GVariant) metadata = NULL;
  g_autoptr(GInputStream) input = NULL;
  g_autoptr(GFileInfo) file_info = NULL;
  g_autoptr(GVariant) xattrs = NULL;
  GError *temp_error = NULL;

  if (OSTREE_OBJECT_TYPE_IS_META (objtype))
    {
      if (!ostree_repo_load_variant (repo, objtype,
                                     checksum, &metadata, &temp_error))
        {
          if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_clear_error (&temp_error);
              g_printerr ("Object missing: %s.%s\n", checksum,
                          ostree_object_type_to_string (objtype));
              missing = TRUE;
            }
          else
            {
              g_prefix_error (error, "Loading metadata object %s: ", checksum);
              goto out;
            }
        }
      else
        {
          if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
            {
              if (!ostree_validate_structureof_commit (metadata, error))
                {
                  g_prefix_error (error, "While validating commit metadata '%s': ", checksum);
                  goto out;
                }
            }
          else if (objtype == OSTREE_OBJECT_TYPE_DIR_TREE)
            {
              if (!ostree_validate_structureof_dirtree (metadata, error))
                {
                  g_prefix_error (error, "While validating directory tree '%s': ", checksum);
                  goto out;
                }
            }
          else if (objtype == OSTREE_OBJECT_TYPE_DIR_META)
            {
              if (!ostree_validate_structureof_dirmeta (metadata, error))
                {
                  g_prefix_error (error, "While validating directory metadata '%s': ", checksum);
                  goto out;
                }
            }
      
          input = g_memory_input_stream_new_from_data (g_variant_get_data (metadata),
                                                       g_variant_get_size (metadata),
                                                       NULL);

        }
    }
  else
    {
      guint32 mode;
      g_assert (objtype == OSTREE_OBJECT_TYPE_FILE);
      if (!ostree_repo_load_file (repo, checksum, &input, &file_info,
                                  &xattrs, cancellable, &temp_error))
        {
          if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_clear_error (&temp_error);
              g_printerr ("Object missing: %s.%s\n", checksum,
                          ostree_object_type_to_string (objtype));
              missing = TRUE;
            }
          else
            {
              *error = temp_error;
              g_prefix_error (error, "Loading file object %s: ", checksum);
              goto out;
            }
        }
      else
        {
          mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
          if (!ostree_validate_structureof_file_mode (mode, error))
            {
              g_prefix_error (error, "While validating file '%s': ", checksum);
              goto out;
            }
        }
    }

  if (missing)
    {
      if (repair_remotes == NULL ||
          !repair_object (repo, repair_remotes, checksum, objtype, cancellable))
        *out_found_corruption = TRUE;
    }
  else
    {
      g_autofree guchar *computed_csum = NULL;
      g_autofree char *tmp_checksum = NULL;

      if (!ostree_checksum_file_from_input (file_info, xattrs, input,
                                            objtype, &computed_csum,
                                            cancellable, error))
        goto out;
      
      tmp_checksum = ostree_checksum_from_bytes (computed_csum);
      if (strcmp (checksum, tmp_checksum) != 0)
        {
          g_autofree char *msg = g_strdup_printf ("corrupted object %s.%s; actual checksum: %s",
                                               checksum, ostree_object_type_to_string (objtype),
                                               tmp_checksum);
          if (opt_delete || repair_remotes != NULL)
            {
              g_printerr ("%s\n", msg);
              (void) ostree_repo_delete_object (repo, objtype, checksum, cancellable, NULL);
              if (repair_remotes == NULL ||
                  !repair_object (repo, repair_remotes, checksum, objtype, cancellable))
                *out_found_corruption = TRUE;
            }
          else
            {
              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, msg);
              goto out;
            }
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
fsck_reachable_objects_from_commits (OstreeRepo            *repo,
                                     GHashTable            *commits,
                                     gchar                **repair_remotes,
                                     gboolean              *out_found_corruption,
                                     GCancellable          *cancellable,
                                     GError               **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  g_autoptr(GHashTable) reachable_objects = NULL;
  guint i;
  guint mod;
  guint count;

  reachable_objects = ostree_repo_traverse_new_reachable ();

  g_hash_table_iter_init (&hash_iter, commits);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      g_assert (objtype == OSTREE_OBJECT_TYPE_COMMIT);

      if (!ostree_repo_traverse_commit_union (repo, checksum, 0, reachable_objects,
                                              cancellable, error))
        goto out;
    }

  count = g_hash_table_size (reachable_objects);
  mod = count / 10;
  i = 0;
  g_hash_table_iter_init (&hash_iter, reachable_objects);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      if (!load_and_fsck_one_object (repo, checksum, objtype, repair_remotes, out_found_corruption,
                                     cancellable, error))
        goto out;

      if (mod == 0 || (i % mod == 0))
        g_print ("%u/%u objects\n", i + 1, count);
      i++;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
strv_contains (gchar **haystack,
               const gchar *needle)
{
  gchar **iter;

  for (iter = haystack; *iter; ++iter)
    if (g_strcmp0 (*iter, needle) == 0)
      return TRUE;

  return FALSE;
}

static gboolean
prepare_repair_remotes (OstreeRepo *repo,
                        gchar ***out_repair_remotes,
                        GError **error)
{
  gchar **iter;

  if (opt_repair_remotes == NULL || *opt_repair_remotes == NULL)
    {
      *out_repair_remotes = NULL;
      return TRUE;
    }

  if (strv_contains (opt_repair_remotes, "-"))
    {
      if (g_strv_length (opt_repair_remotes) > 1)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Either list repair remotes explicitly or use - (dash) to use all available remotes");
          return FALSE;
        }
      *out_repair_remotes = ostree_repo_remote_list (repo, NULL);
      return TRUE;
    }

  for (iter = opt_repair_remotes; *iter; ++iter)
    {
      if (!ostree_repo_remote_get_url (repo, *iter, NULL, error))
        return FALSE;
    }
  *out_repair_remotes = g_strdupv (opt_repair_remotes);
  return TRUE;
}

gboolean
ostree_builtin_fsck (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  glnx_unref_object OstreeRepo *repo = NULL;
  GHashTableIter hash_iter;
  gpointer key, value;
  gboolean found_corruption = FALSE;
  guint n_partial = 0;
  g_autoptr(GHashTable) objects = NULL;
  g_autoptr(GHashTable) commits = NULL;
  g_autoptr(GPtrArray) tombstones = NULL;
  g_auto(GStrv) repair_remotes = NULL;

  context = g_option_context_new ("- Check the repository for consistency");
  if (!ostree_option_context_parse (context, options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (!prepare_repair_remotes (repo, &repair_remotes, error))
    return FALSE;

  if (!opt_quiet)
    g_print ("Enumerating objects...\n");

  if (!ostree_repo_list_objects (repo, OSTREE_REPO_LIST_OBJECTS_ALL,
                                 &objects, cancellable, error))
    goto out;

  commits = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                   (GDestroyNotify)g_variant_unref, NULL);
  
  g_hash_table_iter_init (&hash_iter, objects);

  if (opt_add_tombstones)
    tombstones = g_ptr_array_new_with_free_func (g_free);

  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;
      OstreeRepoCommitState commitstate = 0;
      g_autoptr(GVariant) commit = NULL;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
        {
          if (!ostree_repo_load_commit (repo, checksum, &commit, &commitstate, error))
            goto out;

          if (opt_add_tombstones)
            {
              GError *local_error = NULL;
              const char *parent = ostree_commit_get_parent (commit);
              if (parent)
                {
                  g_autoptr(GVariant) parent_commit = NULL;
                  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, parent,
                                                 &parent_commit, &local_error))
                    {
                      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
                        {
                          g_ptr_array_add (tombstones, g_strdup (checksum));
                          g_clear_error (&local_error);
                        }
                      else
                        {
                          g_propagate_error (error, local_error);
                          goto out;
                        }
                    }
                }
            }

          if (commitstate & OSTREE_REPO_COMMIT_STATE_PARTIAL)
            n_partial++;
          else
            g_hash_table_insert (commits, g_variant_ref (serialized_key), serialized_key);
        }
    }

  g_clear_pointer (&objects, (GDestroyNotify) g_hash_table_unref);

  if (!opt_quiet)
    g_print ("Verifying content integrity of %u commit objects...\n",
             (guint)g_hash_table_size (commits));

  if (!fsck_reachable_objects_from_commits (repo, commits, repair_remotes, &found_corruption,
                                            cancellable, error))
    goto out;

  if (opt_add_tombstones)
    {
      guint i;
      if (tombstones->len)
        {
          if (!ot_enable_tombstone_commits (repo, error))
            goto out;
        }
      for (i = 0; i < tombstones->len; i++)
        {
          const char *checksum = tombstones->pdata[i];
          g_print ("Adding tombstone for commit %s\n", checksum);
          if (!ostree_repo_delete_object (repo, OSTREE_OBJECT_TYPE_COMMIT, checksum, cancellable, error))
            goto out;
        }
    }
  else if (n_partial > 0)
    {
      g_print ("%u partial commits not verified\n", n_partial);
    }

  if (found_corruption)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Repository corruption encountered");
      goto out;
    }

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  g_strfreev (opt_repair_remotes);
  opt_repair_remotes = NULL;
  return ret;
}
