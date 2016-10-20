/* ide-flatpak-clone-widget.c
 *
 * Copyright (C) 2016 Endless
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <glib/gi18n.h>
#include <ide.h>

#include "egg-animation.h"

#include "ide-macros.h"
#include "ide-flatpak-clone-widget.h"

#define ANIMATION_DURATION_MSEC 250

struct _IdeFlatpakCloneWidget
{
  GtkBin                parent_instance;

  gchar                *child_name;

  GtkEntry             *clone_uri_entry;
  GtkLabel             *clone_error_label;
  GtkProgressBar       *clone_progress;
  GtkSpinner           *clone_spinner;

  guint                 is_ready : 1;
  gchar		       *manifest;
};

typedef struct
{
  IdeVcsUri *uri;
  GFile     *location;
  GFile     *project_file;
} CloneRequest;

enum {
  PROP_0,
  PROP_IS_READY,
  PROP_MANIFEST,
  LAST_PROP
};

G_DEFINE_TYPE (IdeFlatpakCloneWidget, ide_flatpak_clone_widget, GTK_TYPE_BIN)

static void
clone_request_free (gpointer data)
{
  CloneRequest *req = data;

  if (req != NULL)
    {
      g_clear_pointer (&req->uri, ide_vcs_uri_unref);
      g_clear_object (&req->location);
      g_clear_object (&req->project_file);
      g_slice_free (CloneRequest, req);
    }
}

static CloneRequest *
clone_request_new (IdeVcsUri *uri,
                   GFile     *location)
{
  CloneRequest *req;

  g_assert (uri);
  g_assert (location);

  req = g_slice_new0 (CloneRequest);
  req->uri = ide_vcs_uri_ref (uri);
  req->location = g_object_ref (location);
  req->project_file = NULL;

  return req;
}

static void
ide_flatpak_clone_widget_uri_changed (IdeFlatpakCloneWidget *self,
                                  GtkEntry              *entry)
{
  g_autoptr(IdeVcsUri) uri = NULL;
  const gchar *text;
  gboolean is_ready = FALSE;

  g_assert (IDE_IS_FLATPAK_CLONE_WIDGET (self));
  g_assert (GTK_IS_ENTRY (entry));

  text = gtk_entry_get_text (entry);
  uri = ide_vcs_uri_new (text);

  if (uri != NULL)
    {
      const gchar *path;
      gchar *name = NULL;

      g_object_set (self->clone_uri_entry,
                    "secondary-icon-tooltip-text", "",
                    "secondary-icon-name", NULL,
                    NULL);

      path = ide_vcs_uri_get_path (uri);

      if (path != NULL)
        {
          name = g_path_get_basename (path);

          if (g_str_has_suffix (name, ".git"))
            *(strrchr (name, '.')) = '\0';

          if (!g_str_equal (name, "/"))
            {
              g_free (self->child_name);
              self->child_name = g_steal_pointer (&name);
            }

          g_free (name);
        }

      is_ready = TRUE;
    }
  else
    {
      g_object_set (self->clone_uri_entry,
                    "secondary-icon-name", "dialog-warning-symbolic",
                    "secondary-icon-tooltip-text", _("A valid Git URL is required"),
                    NULL);
    }

  if (is_ready != self->is_ready)
    {
      self->is_ready = is_ready;
      g_object_notify (G_OBJECT (self), "is-ready");
    }
}

static void
ide_flatpak_clone_widget_finalize (GObject *object)
{
  IdeFlatpakCloneWidget *self = (IdeFlatpakCloneWidget *)object;

  g_clear_pointer (&self->child_name, g_free);

  G_OBJECT_CLASS (ide_flatpak_clone_widget_parent_class)->finalize (object);
}

static void
ide_flatpak_clone_widget_set_property (GObject          *object,
                                       guint             prop_id,
                                       const GValue     *value,
                                       GParamSpec       *pspec)
{
  IdeFlatpakCloneWidget *self = IDE_FLATPAK_CLONE_WIDGET(object);
  switch (prop_id)
    {
    case PROP_MANIFEST:
      g_free (self->manifest);
      self->manifest = g_value_dup_string (value);
			gtk_entry_set_text (GTK_WIDGET (self->clone_uri_entry), self->manifest);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
ide_flatpak_clone_widget_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  IdeFlatpakCloneWidget *self = IDE_FLATPAK_CLONE_WIDGET(object);

  switch (prop_id)
    {
    case PROP_IS_READY:
      g_value_set_boolean (value, self->is_ready);
      break;

    case PROP_MANIFEST:
      g_value_set_string (value, self->manifest);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
ide_flatpak_clone_widget_class_init (IdeFlatpakCloneWidgetClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = ide_flatpak_clone_widget_finalize;
  object_class->get_property = ide_flatpak_clone_widget_get_property;
  object_class->set_property = ide_flatpak_clone_widget_set_property;

  g_object_class_install_property (object_class,
                                   PROP_IS_READY,
                                   g_param_spec_boolean ("is-ready",
                                                         "Is Ready",
                                                         "If the widget is ready to continue.",
                                                         FALSE,
                                                         (G_PARAM_READABLE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (object_class,
                                   PROP_MANIFEST,
                                   g_param_spec_string ("manifest",
                                                        "Manifest",
                                                        "Name of the flatpak manifest to load.",
                                                        NULL,
                                                        (G_PARAM_READWRITE)));

  gtk_widget_class_set_css_name (widget_class, "flatpakclonewidget");
  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/builder/plugins/flatpak-plugin/ide-flatpak-clone-widget.ui");
  gtk_widget_class_bind_template_child (widget_class, IdeFlatpakCloneWidget, clone_error_label);
  gtk_widget_class_bind_template_child (widget_class, IdeFlatpakCloneWidget, clone_progress);
  gtk_widget_class_bind_template_child (widget_class, IdeFlatpakCloneWidget, clone_spinner);
  gtk_widget_class_bind_template_child (widget_class, IdeFlatpakCloneWidget, clone_uri_entry);
}

static void
ide_flatpak_clone_widget_init (IdeFlatpakCloneWidget *self)
{
  g_autoptr(GFile) file = NULL;
  g_autofree gchar *path = NULL;
  g_autofree gchar *projects_dir = NULL;

  gtk_widget_init_template (GTK_WIDGET (self));

  g_signal_connect_object (self->clone_uri_entry,
                           "changed",
                           G_CALLBACK (ide_flatpak_clone_widget_uri_changed),
                           self,
                           G_CONNECT_SWAPPED);
}

static gboolean
open_after_timeout (gpointer user_data)
{
  IdeFlatpakCloneWidget *self;
  IdeWorkbench *workbench;
  g_autoptr(GTask) task = user_data;
  g_autoptr(GError) error = NULL;
  CloneRequest *req;

  IDE_ENTRY;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  req = g_task_get_task_data (task);
  workbench = ide_widget_get_workbench (GTK_WIDGET (self));

  g_assert (req != NULL);
  g_assert (IDE_IS_FLATPAK_CLONE_WIDGET (self));
  g_assert (IDE_IS_WORKBENCH (workbench));

  if (error)
    {
      g_warning ("%s", error->message);
      gtk_label_set_label (self->clone_error_label, error->message);
      gtk_widget_show (GTK_WIDGET (self->clone_error_label));
    }
  else
    {
      ide_workbench_open_project_async (workbench, req->project_file, NULL, NULL, NULL);
    }

  g_task_return_boolean (task, TRUE);

  IDE_RETURN (G_SOURCE_REMOVE);
}

static gboolean
finish_animation_in_idle (gpointer data)
{
  g_autoptr(GTask) task = data;
  IdeFlatpakCloneWidget *self;

  IDE_ENTRY;

  g_assert (G_IS_TASK (task));
  self = g_task_get_source_object (task);
  g_assert (IDE_IS_FLATPAK_CLONE_WIDGET (self));

  egg_object_animate_full (self->clone_progress,
                           EGG_ANIMATION_EASE_IN_OUT_QUAD,
                           ANIMATION_DURATION_MSEC,
                           NULL,
                           (GDestroyNotify)ide_widget_hide_with_fade,
                           self->clone_progress,
                           "fraction", 1.0,
                           NULL);

  /*
   * Wait for a second so animations can complete before opening
   * the project. Otherwise, it's pretty jarring to the user.
   */
  g_timeout_add (ANIMATION_DURATION_MSEC, open_after_timeout, g_object_ref (task));

  IDE_RETURN (G_SOURCE_REMOVE);
}

static void
ide_flatpak_clone_widget_worker (GTask        *task,
                                 gpointer      source_object,
                                 gpointer      task_data,
                                 GCancellable *cancellable)
{
  /*
  flatpak-builder work gets here, call
  g_timeout_add (0, finish_animation_in_idle, g_object_ref (task));
  when done.
  */
}

void
ide_flatpak_clone_widget_clone_async (IdeFlatpakCloneWidget   *self,
                                      GCancellable            *cancellable,
                                      GAsyncReadyCallback      callback,
                                      gpointer                 user_data)
{
  /*
  setup flatpak-builder work
  - get filename
  - start spinner
  - set task data
  - run task in thread
  */
}

gboolean
ide_flatpak_clone_widget_clone_finish (IdeFlatpakCloneWidget  *self,
                                       GAsyncResult           *result,
                                       GError                 **error)
{
  g_return_val_if_fail (IDE_IS_FLATPAK_CLONE_WIDGET (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  gtk_spinner_stop (self->clone_spinner);

  gtk_widget_set_sensitive (GTK_WIDGET (self->clone_uri_entry), TRUE);

  return g_task_propagate_boolean (G_TASK (result), error);
}
