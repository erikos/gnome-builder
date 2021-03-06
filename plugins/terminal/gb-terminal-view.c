/* gb-terminal-view.c
 *
 * Copyright (C) 2015 Christian Hergert <christian@hergert.me>
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

#define G_LOG_DOMAIN "gb-terminal-view"

#include "config.h"

#include <fcntl.h>
#include <glib/gi18n.h>
#include <ide.h>
#include <stdlib.h>
#include <vte/vte.h>
#include <unistd.h>

#include "gb-terminal.h"
#include "gb-terminal-view.h"
#include "gb-terminal-view-private.h"
#include "gb-terminal-view-actions.h"

G_DEFINE_TYPE (GbTerminalView, gb_terminal_view, IDE_TYPE_LAYOUT_VIEW)

enum {
  PROP_0,
  PROP_FONT_NAME,
  LAST_PROP
};

static GParamSpec *properties [LAST_PROP];
static gchar *cached_shell;

/* TODO: allow palette to come from gnome-terminal. */
static const GdkRGBA solarized_palette[] =
{
  /*
   * Solarized palette (1.0.0beta2):
   * http://ethanschoonover.com/solarized
   */
  { 0.02745,  0.211764, 0.258823, 1 },
  { 0.862745, 0.196078, 0.184313, 1 },
  { 0.521568, 0.6,      0,        1 },
  { 0.709803, 0.537254, 0,        1 },
  { 0.149019, 0.545098, 0.823529, 1 },
  { 0.82745,  0.211764, 0.509803, 1 },
  { 0.164705, 0.631372, 0.596078, 1 },
  { 0.933333, 0.909803, 0.835294, 1 },
  { 0,        0.168627, 0.211764, 1 },
  { 0.796078, 0.294117, 0.086274, 1 },
  { 0.345098, 0.431372, 0.458823, 1 },
  { 0.396078, 0.482352, 0.513725, 1 },
  { 0.513725, 0.580392, 0.588235, 1 },
  { 0.423529, 0.443137, 0.768627, 1 },
  { 0.57647,  0.631372, 0.631372, 1 },
  { 0.992156, 0.964705, 0.890196, 1 },
};

static void gb_terminal_view_connect_terminal (GbTerminalView *self,
                                               VteTerminal    *terminal);
static void gb_terminal_respawn               (GbTerminalView *self,
                                               VteTerminal    *terminal);

static gchar *
gb_terminal_view_discover_shell (GCancellable  *cancellable,
                                 GError       **error)
{
  g_autoptr(IdeSubprocessLauncher) launcher = NULL;
  g_autoptr(IdeSubprocess) subprocess = NULL;
  g_autofree gchar *command = NULL;
  g_autofree gchar *stdout_buf = NULL;
  g_auto(GStrv) argv = NULL;

  g_assert (!cancellable || G_IS_CANCELLABLE (cancellable));

  if (cached_shell != NULL)
    return g_strdup (cached_shell);

  command = g_strdup_printf ("sh -c 'getent passwd | grep ^%s: | cut -f 7 -d :'",
                             g_get_user_name ());

  if (!g_shell_parse_argv (command, NULL, &argv, error))
    return NULL;

  launcher = ide_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE);
  ide_subprocess_launcher_set_run_on_host (launcher, TRUE);
  ide_subprocess_launcher_set_clear_env (launcher, FALSE);
  ide_subprocess_launcher_set_cwd (launcher, g_get_home_dir ());
  ide_subprocess_launcher_push_args (launcher, (const gchar * const *)argv);

  subprocess = ide_subprocess_launcher_spawn_sync (launcher, cancellable, error);

  if (subprocess == NULL)
    return NULL;

  if (!ide_subprocess_communicate_utf8 (subprocess, NULL, cancellable, &stdout_buf, NULL, error))
    return NULL;

  if (stdout_buf != NULL)
    {
      g_strstrip (stdout_buf);
      if (stdout_buf[0] == '/')
        cached_shell = g_steal_pointer (&stdout_buf);
    }

  if (cached_shell == NULL)
    g_set_error_literal (error,
                         G_IO_ERROR,
                         G_IO_ERROR_FAILED,
                         "Unknown error when discovering user shell");

  return g_strdup (cached_shell);
}

static void
gb_terminal_view_wait_cb (GObject      *object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  IdeSubprocess *subprocess = (IdeSubprocess *)object;
  VteTerminal *terminal = user_data;
  GbTerminalView *self;
  g_autoptr(GError) error = NULL;

  IDE_ENTRY;

  g_assert (IDE_IS_SUBPROCESS (subprocess));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (VTE_IS_TERMINAL (terminal));

  if (!ide_subprocess_wait_finish (subprocess, result, &error))
    {
      g_warning ("%s", error->message);
      IDE_GOTO (failure);
    }

  self = (GbTerminalView *)gtk_widget_get_ancestor (GTK_WIDGET (terminal), GB_TYPE_TERMINAL_VIEW);
  if (self == NULL)
    IDE_GOTO (failure);

  if (!ide_widget_action (GTK_WIDGET (self), "view-stack", "close", NULL))
    {
      if (!gtk_widget_in_destruction (GTK_WIDGET (terminal)))
        gb_terminal_respawn (self, terminal);
    }

failure:
  g_clear_object (&terminal);

  IDE_EXIT;
}

static gboolean
terminal_has_notification_signal (void)
{
  GQuark quark;
  guint signal_id;

  return g_signal_parse_name ("notification-received",
                              VTE_TYPE_TERMINAL,
                              &signal_id,
                              &quark,
                              FALSE);
}

static void
gb_terminal_respawn (GbTerminalView *self,
                     VteTerminal    *terminal)
{
  g_autoptr(GPtrArray) args = NULL;
  g_autoptr(IdeSubprocess) subprocess = NULL;
  g_autoptr(IdeSubprocessLauncher) launcher = NULL;
  g_autofree gchar *workpath = NULL;
  g_autofree gchar *shell = NULL;
  GtkWidget *toplevel;
  GError *error = NULL;
  IdeContext *context;
  IdeVcs *vcs;
  VtePty *pty = NULL;
  GFile *workdir;
  gint64 now;
  int master_fd = -1;
  int tty_fd = -1;
  char name[PATH_MAX + 1];

  IDE_ENTRY;

  g_assert (GB_IS_TERMINAL_VIEW (self));

  vte_terminal_reset (terminal, TRUE, TRUE);

  toplevel = gtk_widget_get_toplevel (GTK_WIDGET (self));
  if (!IDE_IS_WORKBENCH (toplevel))
    IDE_EXIT;

  /* Prevent flapping */
  now = g_get_monotonic_time ();
  if ((now - self->last_respawn) < (G_USEC_PER_SEC / 10))
    IDE_EXIT;
  self->last_respawn = now;

  context = ide_workbench_get_context (IDE_WORKBENCH (toplevel));
  vcs = ide_context_get_vcs (context);
  workdir = ide_vcs_get_working_directory (vcs);
  workpath = g_file_get_path (workdir);

  shell = gb_terminal_view_discover_shell (NULL, &error);

  if (shell == NULL)
    {
      g_warning ("Failed to discover user shell: %s", error->message);

      /* We prefer bash in flatpak over sh */
      if (ide_is_flatpak ())
        shell = g_strdup ("/bin/bash");
      else
        shell = vte_get_user_shell ();

      g_clear_error (&error);
    }

  args = g_ptr_array_new ();
  g_ptr_array_add (args, (gchar *)shell);
  g_ptr_array_add (args, NULL);

  pty = vte_terminal_pty_new_sync (terminal,
                                   VTE_PTY_DEFAULT | VTE_PTY_NO_LASTLOG | VTE_PTY_NO_UTMP | VTE_PTY_NO_WTMP,
                                   NULL,
                                   &error);
  if (pty == NULL)
    IDE_GOTO (failure);

  vte_terminal_set_pty (terminal, pty);

  if (-1 == (master_fd = vte_pty_get_fd (pty)))
    IDE_GOTO (failure);

  if (grantpt (master_fd) != 0)
    IDE_GOTO (failure);

  if (unlockpt (master_fd) != 0)
    IDE_GOTO (failure);

  if (ptsname_r (master_fd, name, sizeof name - 1) != 0)
    IDE_GOTO (failure);

  if (-1 == (tty_fd = open (name, O_RDWR | O_CLOEXEC)))
    IDE_GOTO (failure);

  /* XXX: It would be nice to allow using the runtimes launcher */
  launcher = ide_subprocess_launcher_new (0);
  ide_subprocess_launcher_set_run_on_host (launcher, TRUE);
  ide_subprocess_launcher_set_clear_env (launcher, FALSE);
  ide_subprocess_launcher_set_cwd (launcher, workpath);
  ide_subprocess_launcher_push_args (launcher, (const gchar * const *)args->pdata);
  ide_subprocess_launcher_take_stdin_fd (launcher, dup (tty_fd));
  ide_subprocess_launcher_take_stdout_fd (launcher, dup (tty_fd));
  ide_subprocess_launcher_take_stderr_fd (launcher, dup (tty_fd));
  ide_subprocess_launcher_setenv (launcher, "TERM", "xterm-256color", TRUE);
  ide_subprocess_launcher_setenv (launcher, "INSIDE_GNOME_BUILDER", PACKAGE_VERSION, TRUE);
  ide_subprocess_launcher_setenv (launcher, "SHELL", shell, TRUE);

  subprocess = ide_subprocess_launcher_spawn_sync (launcher, NULL, &error);
  if (subprocess == NULL)
    IDE_GOTO (failure);

  ide_subprocess_wait_async (subprocess,
                             NULL,
                             gb_terminal_view_wait_cb,
                             g_object_ref (terminal));

failure:
  if (tty_fd != -1)
    close (tty_fd);

  g_clear_object (&pty);

  if (error != NULL)
    {
      g_warning ("%s", error->message);
      g_clear_error (&error);
    }

  IDE_EXIT;
}

static void
gb_terminal_realize (GtkWidget *widget)
{
  GbTerminalView *self = (GbTerminalView *)widget;

  g_assert (GB_IS_TERMINAL_VIEW (self));

  GTK_WIDGET_CLASS (gb_terminal_view_parent_class)->realize (widget);

  if (!self->top_has_spawned)
    {
      self->top_has_spawned = TRUE;
      gb_terminal_respawn (self, self->terminal_top);
    }
}

static void
size_allocate_cb (VteTerminal    *terminal,
                  GtkAllocation  *alloc,
                  GbTerminalView *self)
{
  glong width;
  glong height;
  glong columns;
  glong rows;

  g_assert (VTE_IS_TERMINAL (terminal));
  g_assert (alloc != NULL);
  g_assert (GB_IS_TERMINAL_VIEW (self));

  if ((alloc->width == 0) || (alloc->height == 0))
    return;

  width = vte_terminal_get_char_width (terminal);
  height = vte_terminal_get_char_height (terminal);

  if ((width == 0) || (height == 0))
    return;

  columns = alloc->width / width;
  rows = alloc->height / height;

  if ((columns < 2) || (rows < 2))
    return;

  vte_terminal_set_size (terminal, columns, rows);
}

static void
gb_terminal_get_preferred_width (GtkWidget *widget,
                                 gint      *min_width,
                                 gint      *nat_width)
{
  /*
   * Since we are placing the terminal in a GtkStack, we need
   * to fake the size a bit. Otherwise, GtkStack tries to keep the
   * widget at it's natural size (which prevents us from getting
   * appropriate size requests.
   */
  GTK_WIDGET_CLASS (gb_terminal_view_parent_class)->get_preferred_width (widget, min_width, nat_width);
  *nat_width = *min_width;
}

static void
gb_terminal_get_preferred_height (GtkWidget *widget,
                                  gint      *min_height,
                                  gint      *nat_height)
{
  /*
   * Since we are placing the terminal in a GtkStack, we need
   * to fake the size a bit. Otherwise, GtkStack tries to keep the
   * widget at it's natural size (which prevents us from getting
   * appropriate size requests.
   */
  GTK_WIDGET_CLASS (gb_terminal_view_parent_class)->get_preferred_height (widget, min_height, nat_height);
  *nat_height = *min_height;
}

static void
gb_terminal_set_needs_attention (GbTerminalView  *self,
                                 gboolean         needs_attention,
                                 GtkPositionType  position)
{
  GtkWidget *parent;

  g_assert (GB_IS_TERMINAL_VIEW (self));

  parent = gtk_widget_get_parent (GTK_WIDGET (self));

  if (GTK_IS_STACK (parent) &&
      !gtk_widget_in_destruction (GTK_WIDGET (self)) &&
      !gtk_widget_in_destruction (parent))
    {
      if (position == GTK_POS_TOP &&
          !gtk_widget_in_destruction (GTK_WIDGET (self->terminal_top)))
        {
          self->top_has_needs_attention = TRUE;
        }
      else if (position == GTK_POS_BOTTOM &&
               self->terminal_bottom != NULL &&
               !gtk_widget_in_destruction (GTK_WIDGET (self->terminal_bottom)))
        {
          self->bottom_has_needs_attention = TRUE;
        }

      gtk_container_child_set (GTK_CONTAINER (parent), GTK_WIDGET (self),
                               "needs-attention",
                               !!(self->top_has_needs_attention || self->bottom_has_needs_attention) &&
                               needs_attention,
                               NULL);
    }
}

static void
notification_received_cb (VteTerminal    *terminal,
                          const gchar    *summary,
                          const gchar    *body,
                          GbTerminalView *self)
{
  g_assert (VTE_IS_TERMINAL (terminal));
  g_assert (GB_IS_TERMINAL_VIEW (self));

  if (!gtk_widget_has_focus (GTK_WIDGET (terminal)))
    {
      if (terminal == self->terminal_top)
        gb_terminal_set_needs_attention (self, TRUE, GTK_POS_TOP);
      else if (terminal == self->terminal_bottom)
        gb_terminal_set_needs_attention (self, TRUE, GTK_POS_BOTTOM);
    }
}

static gchar *
gb_terminal_get_title (IdeLayoutView *view)
{
  const gchar *title = NULL;
  GbTerminalView *self = (GbTerminalView *)view;

  g_assert (GB_IS_TERMINAL_VIEW (self));

  if (self->bottom_has_focus)
    title = vte_terminal_get_window_title (self->terminal_bottom);
  else
    title = vte_terminal_get_window_title (self->terminal_top);

  if (title == NULL)
    title = _("Untitled terminal");

  return g_strdup (title);
}

static gboolean
focus_in_event_cb (VteTerminal    *terminal,
                   GdkEvent       *event,
                   GbTerminalView *self)
{
  g_assert (VTE_IS_TERMINAL (terminal));
  g_assert (GB_IS_TERMINAL_VIEW (self));

  self->bottom_has_focus = (terminal != self->terminal_top);

  if (terminal == self->terminal_top)
    {
      self->top_has_needs_attention = FALSE;
      gb_terminal_set_needs_attention (self, FALSE, GTK_POS_TOP);
    }
  else if (terminal == self->terminal_bottom)
    {
      self->bottom_has_needs_attention = FALSE;
      gb_terminal_set_needs_attention (self, FALSE, GTK_POS_BOTTOM);
    }

  return GDK_EVENT_PROPAGATE;
}

static void
window_title_changed_cb (VteTerminal    *terminal,
                         GbTerminalView *self)
{
  g_assert (VTE_IS_TERMINAL (terminal));
  g_assert (GB_IS_TERMINAL_VIEW (self));

  g_object_notify (G_OBJECT (self), "title");
}

static void
style_context_changed (GtkStyleContext *style_context,
                       GbTerminalView  *self)
{
  GtkStateFlags state;
  GdkRGBA fg;
  GdkRGBA bg;

  g_assert (GTK_IS_STYLE_CONTEXT (style_context));
  g_assert (GB_IS_TERMINAL_VIEW (self));

  state = gtk_style_context_get_state (style_context);

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
  gtk_style_context_get_color (style_context, state, &fg);
  gtk_style_context_get_background_color (style_context, state, &bg);
  G_GNUC_END_IGNORE_DEPRECATIONS;

  if (bg.alpha == 0.0)
    {
      gdk_rgba_parse (&bg, "#f6f7f8");
    }

  vte_terminal_set_colors (self->terminal_top, &fg, &bg,
                           solarized_palette,
                           G_N_ELEMENTS (solarized_palette));

  if (self->terminal_bottom)
    vte_terminal_set_colors (self->terminal_bottom, &fg, &bg,
                             solarized_palette,
                             G_N_ELEMENTS (solarized_palette));
}

static IdeLayoutView *
gb_terminal_create_split (IdeLayoutView *view,
                          GFile         *file)
{
  IdeLayoutView *new_view;

  g_assert (GB_IS_TERMINAL_VIEW (view));

  new_view = g_object_new (GB_TYPE_TERMINAL_VIEW,
                          "visible", TRUE,
                          NULL);

  return new_view;
}

static void
gb_terminal_view_set_font_name (GbTerminalView *self,
                                const gchar    *font_name)
{
  PangoFontDescription *font_desc = NULL;

  g_assert (GB_IS_TERMINAL_VIEW (self));

  if (font_name != NULL)
    font_desc = pango_font_description_from_string (font_name);

  if (font_desc != NULL)
    {
      vte_terminal_set_font (self->terminal_top, font_desc);

      if (self->terminal_bottom)
        vte_terminal_set_font (self->terminal_bottom, font_desc);

      pango_font_description_free (font_desc);
    }
}

static void
gb_terminal_set_split_view (IdeLayoutView   *view,
                            gboolean         split_view)
{
  GbTerminalView *self = (GbTerminalView *)view;
  GtkStyleContext *style_context;

  g_assert (GB_IS_TERMINAL_VIEW (self));
  g_return_if_fail (GB_IS_TERMINAL_VIEW (self));

  if (split_view && (self->terminal_bottom != NULL))
    return;

  if (!split_view && (self->terminal_bottom == NULL))
    return;

  if (split_view)
    {
      style_context = gtk_widget_get_style_context (GTK_WIDGET (view));

      self->terminal_bottom = g_object_new (GB_TYPE_TERMINAL,
                                            "audible-bell", FALSE,
                                            "scrollback-lines", G_MAXUINT,
                                            "expand", TRUE,
                                            "visible", TRUE,
                                            NULL);
      gtk_container_add_with_properties (GTK_CONTAINER (self->bottom_container),
                                         GTK_WIDGET (self->terminal_bottom),
                                         "position", 0,
                                         NULL);
      gtk_widget_show (self->bottom_container);

      gb_terminal_view_connect_terminal (self, self->terminal_bottom);
      style_context_changed (style_context, GB_TERMINAL_VIEW (view));

      gtk_widget_grab_focus (GTK_WIDGET (self->terminal_bottom));

      if (!self->bottom_has_spawned)
        {
          self->bottom_has_spawned = TRUE;
          gb_terminal_respawn (self, self->terminal_bottom);
        }
    }
  else
    {
      gtk_container_remove (GTK_CONTAINER (self->bottom_container),
                            GTK_WIDGET (self->terminal_bottom));
      gtk_widget_hide (self->bottom_container);

      self->terminal_bottom = NULL;
      self->bottom_has_focus = FALSE;
      self->bottom_has_spawned = FALSE;
      self->bottom_has_needs_attention = FALSE;
      g_clear_object (&self->save_as_file_bottom);
      gtk_widget_grab_focus (GTK_WIDGET (self->terminal_top));
    }
}

static void
gb_terminal_grab_focus (GtkWidget *widget)
{
  GbTerminalView *self = (GbTerminalView *)widget;

  g_assert (GB_IS_TERMINAL_VIEW (self));

  if (self->bottom_has_focus && self->terminal_bottom)
    gtk_widget_grab_focus (GTK_WIDGET (self->terminal_bottom));
  else
    gtk_widget_grab_focus (GTK_WIDGET (self->terminal_top));
}

static void
gb_terminal_view_connect_terminal (GbTerminalView *self,
                                   VteTerminal    *terminal)
{
  GtkAdjustment *vadj;

  vadj = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (terminal));

  if (terminal == self->terminal_top)
    gtk_range_set_adjustment (GTK_RANGE (self->top_scrollbar), vadj);
  else
    gtk_range_set_adjustment (GTK_RANGE (self->bottom_scrollbar), vadj);

  g_signal_connect_object (terminal,
                           "size-allocate",
                           G_CALLBACK (size_allocate_cb),
                           self,
                           0);

  g_signal_connect_object (terminal,
                           "focus-in-event",
                           G_CALLBACK (focus_in_event_cb),
                           self,
                           0);

  g_signal_connect_object (terminal,
                           "window-title-changed",
                           G_CALLBACK (window_title_changed_cb),
                           self,
                           0);

  if (terminal_has_notification_signal ())
    {
      g_signal_connect_object (terminal,
                               "notification-received",
                               G_CALLBACK (notification_received_cb),
                               self,
                               0);
    }
}

static void
gb_terminal_view_finalize (GObject *object)
{
  GbTerminalView *self = GB_TERMINAL_VIEW (object);

  g_clear_object (&self->save_as_file_top);
  g_clear_object (&self->save_as_file_bottom);
  g_clear_pointer (&self->selection_buffer, g_free);

  G_OBJECT_CLASS (gb_terminal_view_parent_class)->finalize (object);
}

static void
gb_terminal_view_set_property (GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  GbTerminalView *self = GB_TERMINAL_VIEW (object);

  switch (prop_id)
    {
    case PROP_FONT_NAME:
      gb_terminal_view_set_font_name (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_terminal_view_class_init (GbTerminalViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  IdeLayoutViewClass *view_class = IDE_LAYOUT_VIEW_CLASS (klass);

  object_class->finalize = gb_terminal_view_finalize;
  object_class->set_property = gb_terminal_view_set_property;

  widget_class->realize = gb_terminal_realize;
  widget_class->get_preferred_width = gb_terminal_get_preferred_width;
  widget_class->get_preferred_height = gb_terminal_get_preferred_height;
  widget_class->grab_focus = gb_terminal_grab_focus;

  view_class->get_title = gb_terminal_get_title;
  view_class->create_split = gb_terminal_create_split;
  view_class->set_split_view =  gb_terminal_set_split_view;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/builder/plugins/terminal/gb-terminal-view.ui");
  gtk_widget_class_bind_template_child (widget_class, GbTerminalView, terminal_top);
  gtk_widget_class_bind_template_child (widget_class, GbTerminalView, bottom_container);
  gtk_widget_class_bind_template_child (widget_class, GbTerminalView, top_scrollbar);
  gtk_widget_class_bind_template_child (widget_class, GbTerminalView, bottom_scrollbar);

  g_type_ensure (VTE_TYPE_TERMINAL);

  properties [PROP_FONT_NAME] =
    g_param_spec_string ("font-name",
                         "Font Name",
                         "Font Name",
                         NULL,
                         (G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, LAST_PROP, properties);

  g_type_ensure (GB_TYPE_TERMINAL);
}

static void
gb_terminal_view_init (GbTerminalView *self)
{
  GtkStyleContext *style_context;
  g_autoptr(GSettings) settings = NULL;

  gtk_widget_init_template (GTK_WIDGET (self));

  gb_terminal_view_connect_terminal (self, self->terminal_top);
  gb_terminal_view_actions_init (self);

  settings = g_settings_new ("org.gnome.builder.terminal");
  g_settings_bind (settings, "font-name", self, "font-name", G_SETTINGS_BIND_GET);

  style_context = gtk_widget_get_style_context (GTK_WIDGET (self));
  gtk_style_context_add_class (style_context, "terminal");
  g_signal_connect_object (style_context,
                           "changed",
                           G_CALLBACK (style_context_changed),
                           self,
                           0);
  style_context_changed (style_context, self);

  gtk_widget_set_can_focus (GTK_WIDGET (self->terminal_top), TRUE);
}
