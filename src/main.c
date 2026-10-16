/* main.c
 *
 * Copyright 2026 Riqi Ramadhan
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
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>
#include <gst/gst.h>

#include "stampy-application.h"

int
main (int   argc,
      char *argv[])
{
    g_autoptr(StampyApplication) app = NULL;
    int ret;

    /* Avoid Glycin sandbox and GSK renderer issues. */
    g_setenv ("GLYCIN_ENABLE_SANDBOX", "0", TRUE);
    g_setenv ("GSK_RENDERER", "cairo", TRUE);

    bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    gst_init (&argc, &argv);

    app = stampy_application_new ("com.cobaltty.stampy", G_APPLICATION_DEFAULT_FLAGS);
    ret = g_application_run (G_APPLICATION (app), argc, argv);

    return ret;
}
