/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
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
 */

#include "config.h"

#include <libsoup/soup.h>

#include "ot-builtins.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ot-main.h"
#include "ostree.h"
#include "ostree-repo-file.h"

#include <glib/gi18n.h>

static char *opt_port_file = NULL;
static gboolean opt_daemonize;
static gboolean opt_autoexit;

typedef struct {
  GFile *root;
  gboolean running;
} OtTrivialHttpd;

static GOptionEntry options[] = {
  { "daemonize", 'd', 0, G_OPTION_ARG_NONE, &opt_daemonize, "Fork into background when ready", NULL },
  { "autoexit", 0, 0, G_OPTION_ARG_NONE, &opt_autoexit, "Automatically exit when directory is deleted", NULL },
  { "port-file", 'p', 0, G_OPTION_ARG_FILENAME, &opt_port_file, "Write port number to PATH", "PATH" },
  { NULL }
};

static int
compare_strings (gconstpointer a, gconstpointer b)
{
  const char **sa = (const char **)a;
  const char **sb = (const char **)b;

  return strcmp (*sa, *sb);
}

static GString *
get_directory_listing (const char *path)
{
  GPtrArray *entries;
  GString *listing;
  char *escaped;
  DIR *dir;
  struct dirent *dent;
  int i;

  entries = g_ptr_array_new ();
  dir = opendir (path);
  if (dir)
    {
      while ((dent = readdir (dir)))
        {
          if (!strcmp (dent->d_name, ".") ||
              (!strcmp (dent->d_name, "..") &&
               !strcmp (path, "./")))
            continue;
          escaped = g_markup_escape_text (dent->d_name, -1);
          g_ptr_array_add (entries, escaped);
        }
      closedir (dir);
    }

  g_ptr_array_sort (entries, (GCompareFunc)compare_strings);

  listing = g_string_new ("<html>\r\n");
  escaped = g_markup_escape_text (strchr (path, '/'), -1);
  g_string_append_printf (listing, "<head><title>Index of %s</title></head>\r\n", escaped);
  g_string_append_printf (listing, "<body><h1>Index of %s</h1>\r\n<p>\r\n", escaped);
  g_free (escaped);
  for (i = 0; i < entries->len; i++)
    {
      g_string_append_printf (listing, "<a href=\"%s\">%s</a><br>\r\n",
                              (char *)entries->pdata[i], 
                              (char *)entries->pdata[i]);
      g_free (entries->pdata[i]);
    }
  g_string_append (listing, "</body>\r\n</html>\r\n");

  g_ptr_array_free (entries, TRUE);
  return listing;
}

/* Only allow reading files that have o+r, and for directories, o+x.
 * This makes this server relatively safe to use on multiuser
 * machines.
 */
static gboolean
is_safe_to_access (struct stat *stbuf)
{
  /* Only regular files or directores */
  if (!(S_ISREG (stbuf->st_mode) || S_ISDIR (stbuf->st_mode)))
    return FALSE;
  /* Must be o+r */
  if (!(stbuf->st_mode & S_IROTH))
    return FALSE;
  /* For directories, must be o+x */
  if (S_ISDIR (stbuf->st_mode) && !(stbuf->st_mode & S_IXOTH))
    return FALSE;
  return TRUE;
}

static void
do_get (OtTrivialHttpd  *self,
        SoupServer      *server,
        SoupMessage     *msg,
        const char      *path)
{
  char *slash;
  int ret;
  struct stat stbuf;
  gs_free char *safepath = NULL;

  if (strstr (path, "../") != NULL)
    {
      soup_message_set_status (msg, SOUP_STATUS_FORBIDDEN);
      goto out;
    }

  if (path[0] == '/')
    path++;

  safepath = g_build_filename (gs_file_get_path_cached (self->root), path, NULL);

  do
    ret = stat (safepath, &stbuf);
  while (ret == -1 && errno == EINTR);
  if (ret == -1)
    {
      if (errno == EPERM)
        soup_message_set_status (msg, SOUP_STATUS_FORBIDDEN);
      else if (errno == ENOENT)
        soup_message_set_status (msg, SOUP_STATUS_NOT_FOUND);
      else
        soup_message_set_status (msg, SOUP_STATUS_INTERNAL_SERVER_ERROR);
      goto out;
    }

  if (!is_safe_to_access (&stbuf))
    {
      soup_message_set_status (msg, SOUP_STATUS_FORBIDDEN);
      goto out;
    }

  if (S_ISDIR (stbuf.st_mode))
    {
      slash = strrchr (safepath, '/');
      if (!slash || slash[1])
        {
          gs_free char *redir_uri = NULL;

          redir_uri = g_strdup_printf ("%s/", soup_message_get_uri (msg)->path);
          soup_message_set_redirect (msg, SOUP_STATUS_MOVED_PERMANENTLY,
                                     redir_uri);
        }
      else
        {
          gs_free char *index_realpath = g_strconcat (safepath, "/index.html", NULL);
          if (stat (index_realpath, &stbuf) != -1)
            {
              gs_free char *index_path = g_strconcat (path, "/index.html", NULL);
              do_get (self, server, msg, index_path);
            }
          else
            {
              GString *listing = get_directory_listing (safepath);
              soup_message_set_response (msg, "text/html",
                                         SOUP_MEMORY_TAKE,
                                         listing->str, listing->len);
              g_string_free (listing, FALSE);
            }
        }
    }
  else 
    {
      if (!S_ISREG (stbuf.st_mode))
        {
          soup_message_set_status (msg, SOUP_STATUS_FORBIDDEN);
          goto out;
        }
      
      if (msg->method == SOUP_METHOD_GET)
        {
          GMappedFile *mapping;
          SoupBuffer *buffer;

          mapping = g_mapped_file_new (safepath, FALSE, NULL);
          if (!mapping)
            {
              soup_message_set_status (msg, SOUP_STATUS_INTERNAL_SERVER_ERROR);
              goto out;
            }

          buffer = soup_buffer_new_with_owner (g_mapped_file_get_contents (mapping),
                                               g_mapped_file_get_length (mapping),
                                               mapping, (GDestroyNotify)g_mapped_file_unref);
          soup_message_body_append_buffer (msg->response_body, buffer);
          soup_buffer_free (buffer);
        }
      else /* msg->method == SOUP_METHOD_HEAD */
        {
          gs_free char *length = NULL;

          /* We could just use the same code for both GET and
           * HEAD (soup-message-server-io.c will fix things up).
           * But we'll optimize and avoid the extra I/O.
           */
          length = g_strdup_printf ("%lu", (gulong)stbuf.st_size);
          soup_message_headers_append (msg->response_headers,
                                       "Content-Length", length);
        }
      soup_message_set_status (msg, SOUP_STATUS_OK);
    }
 out:
  return;
}

static void
httpd_callback (SoupServer *server, SoupMessage *msg,
                const char *path, GHashTable *query,
                SoupClientContext *context, gpointer data)
{
  OtTrivialHttpd *self = data;
  SoupMessageHeadersIter iter;

  soup_message_headers_iter_init (&iter, msg->request_headers);

  if (msg->method == SOUP_METHOD_GET || msg->method == SOUP_METHOD_HEAD)
    do_get (self, server, msg, path);
  else
    soup_message_set_status (msg, SOUP_STATUS_NOT_IMPLEMENTED);
}

static void
on_dir_changed (GFileMonitor  *mon,
		GFile *file,
		GFile *other,
		GFileMonitorEvent  event,
		gpointer user_data)
{
  OtTrivialHttpd *self = user_data;

  if (event == G_FILE_MONITOR_EVENT_DELETED)
    {
      self->running = FALSE;
      g_main_context_wakeup (NULL);
    }
}

gboolean
ostree_builtin_trivial_httpd (int argc, char **argv, GFile *repo_path, GError **error)
{
  gboolean ret = FALSE;
  GCancellable *cancellable = NULL;
  GOptionContext *context;
  const char *dirpath;
  OtTrivialHttpd appstruct;
  OtTrivialHttpd *app = &appstruct;
  gs_unref_object GFile *dir = NULL;
  gs_unref_object SoupServer *server = NULL;
  gs_unref_object GFileMonitor *dirmon = NULL;

  memset (&appstruct, 0, sizeof (appstruct));

  context = g_option_context_new ("[DIR] - Simple webserver");

  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc > 1)
    dirpath = argv[1];
  else
    dirpath = ".";

  app->root = g_file_new_for_path (dirpath);

  server = soup_server_new (SOUP_SERVER_PORT, 0,
                            SOUP_SERVER_SERVER_HEADER, "ostree-httpd ",
                            NULL);
  soup_server_add_handler (server, NULL, httpd_callback, app, NULL);
  if (opt_port_file)
    {
      gs_free char *portstr = g_strdup_printf ("%u\n", soup_server_get_port (server));
      if (!g_file_set_contents (opt_port_file, portstr, strlen (portstr), error))
        goto out;
    }
  soup_server_run_async (server);

  if (opt_daemonize)
    {
      pid_t pid = fork();
      if (pid == -1)
        {
          int errsv = errno;
          g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                               g_strerror (errsv));
          goto out;
        }
      else if (pid > 0)
        {
          /* Parent */
          _exit (0);
        }
      /* Child, continue */
    }

  app->running = TRUE;
  if (opt_autoexit)
    {
      dirmon = g_file_monitor_directory (app->root, 0, cancellable, error);
      if (!dirmon)
        goto out;
      g_signal_connect (dirmon, "changed", G_CALLBACK (on_dir_changed), app);
    }

  while (app->running)
    g_main_context_iteration (NULL, TRUE);
 
  ret = TRUE;
 out:
  g_clear_object (&app->root);
  if (context)
    g_option_context_free (context);
  return ret;
}