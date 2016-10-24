/* ide-greeter-perspective.h
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

#ifndef IDE_GREETER_PERSPECTIVE_H
#define IDE_GREETER_PERSPECTIVE_H

#include <gtk/gtk.h>

#include "projects/ide-recent-projects.h"

G_BEGIN_DECLS

#define IDE_TYPE_GREETER_PERSPECTIVE (ide_greeter_perspective_get_type())

G_DECLARE_FINAL_TYPE (IdeGreeterPerspective, ide_greeter_perspective, IDE, GREETER_PERSPECTIVE, GtkBin)

void ide_greeter_perspective_show_genesis_view (IdeGreeterPerspective *self,
                                                const gchar *genesis_addin_name);
void ide_greeter_perspective_set_genesis_property (IdeGreeterPerspective *self,
						   const gchar *genesis_addin_name,
						   const gchar *manifest);

G_END_DECLS

#endif /* IDE_GREETER_PERSPECTIVE_H */
