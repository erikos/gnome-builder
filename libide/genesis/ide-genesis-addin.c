/* ide-genesis-addin.c
 *
 * Copyright (C) 2015 Christian Hergert <chergert@redhat.com>
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

#include "ide-genesis-addin.h"

G_DEFINE_INTERFACE (IdeGenesisAddin, ide_genesis_addin, G_TYPE_OBJECT)

static void
ide_genesis_addin_default_init (IdeGenesisAddinInterface *iface)
{
}

gchar *
ide_genesis_addin_get_title (IdeGenesisAddin *self)
{
  g_return_val_if_fail (IDE_IS_GENESIS_ADDIN (self), NULL);

  return IDE_GENESIS_ADDIN_GET_IFACE (self)->get_title (self);
}

gchar *
ide_genesis_addin_get_icon_name (IdeGenesisAddin *self)
{
  g_return_val_if_fail (IDE_IS_GENESIS_ADDIN (self), NULL);

  return IDE_GENESIS_ADDIN_GET_IFACE (self)->get_icon_name (self);
}

/**
 * ide_genesis_addin_get_widget:
 *
 * Returns: (transfer full): A #GtkWidget.
 */
GtkWidget *
ide_genesis_addin_get_widget (IdeGenesisAddin *self)
{
  g_return_val_if_fail (IDE_IS_GENESIS_ADDIN (self), NULL);

  return IDE_GENESIS_ADDIN_GET_IFACE (self)->get_widget (self);
}