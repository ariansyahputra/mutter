/*
 * Clutter.
 *
 * An OpenGL based 'interactive canvas' library.
 *
 * Authored By Matthew Allum  <mallum@openedhand.com>
 *
 * Copyright (C) 2006 OpenedHand
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
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __CLUTTER_MAIN_H__
#define __CLUTTER_MAIN_H__

#if !defined(__CLUTTER_H_INSIDE__) && !defined(CLUTTER_COMPILATION)
#error "Only <clutter/clutter.h> can be included directly."
#endif

#include <clutter/clutter-actor.h>
#include <clutter/clutter-stage.h>
#include <pango/pango.h>

G_BEGIN_DECLS

/**
 * CLUTTER_INIT_ERROR:
 *
 * #GError domain for #ClutterInitError
 */
#define CLUTTER_INIT_ERROR      (clutter_init_error_quark ())

/**
 * ClutterInitError:
 * @CLUTTER_INIT_SUCCESS: Initialisation successful
 * @CLUTTER_INIT_ERROR_UNKNOWN: Unknown error
 * @CLUTTER_INIT_ERROR_THREADS: Thread initialisation failed
 * @CLUTTER_INIT_ERROR_BACKEND: Backend initialisation failed
 * @CLUTTER_INIT_ERROR_INTERNAL: Internal error
 *
 * Error conditions returned by clutter_init() and clutter_init_with_args().
 *
 * Since: 0.2
 */
typedef enum
{
  CLUTTER_INIT_SUCCESS        =  1,
  CLUTTER_INIT_ERROR_UNKNOWN  =  0,
  CLUTTER_INIT_ERROR_THREADS  = -1,
  CLUTTER_INIT_ERROR_BACKEND  = -2,
  CLUTTER_INIT_ERROR_INTERNAL = -3
} ClutterInitError;

CLUTTER_EXPORT
GQuark clutter_init_error_quark (void);

/**
 * CLUTTER_PRIORITY_REDRAW:
 *
 * Priority of the redraws. This is chosen to be lower than the GTK+
 * redraw and resize priorities, because in application with both
 * GTK+ and Clutter it's more likely that the Clutter part will be
 * continually animating (and thus able to starve GTK+) than
 * vice-versa.
 *
 * Since: 0.8
 */
#define CLUTTER_PRIORITY_REDRAW         (G_PRIORITY_HIGH_IDLE + 50)

/* Initialisation */
CLUTTER_EXPORT
void                    clutter_base_init                       (void);
CLUTTER_EXPORT
ClutterInitError        clutter_init                            (int          *argc,
                                                                 char       ***argv) G_GNUC_WARN_UNUSED_RESULT;
CLUTTER_EXPORT
ClutterInitError        clutter_init_with_args                  (int          *argc,
                                                                 char       ***argv,
                                                                 const char   *parameter_string,
                                                                 GOptionEntry *entries,
                                                                 const char   *translation_domain,
                                                                 GError      **error) G_GNUC_WARN_UNUSED_RESULT;

CLUTTER_EXPORT
GOptionGroup *          clutter_get_option_group                (void);
CLUTTER_EXPORT
GOptionGroup *          clutter_get_option_group_without_init   (void);

/* Mainloop */
CLUTTER_EXPORT
void                    clutter_main                            (void);
CLUTTER_EXPORT
void                    clutter_main_quit                       (void);
CLUTTER_EXPORT
gint                    clutter_main_level                      (void);

CLUTTER_EXPORT
void                    clutter_do_event                        (ClutterEvent *event);

/* Debug utility functions */
CLUTTER_EXPORT
gboolean                clutter_get_accessibility_enabled       (void);

CLUTTER_EXPORT
void                    clutter_disable_accessibility           (void);

/* Threading functions */
CLUTTER_EXPORT
void                    clutter_threads_set_lock_functions      (GCallback enter_fn,
                                                                 GCallback leave_fn);
CLUTTER_EXPORT
guint                   clutter_threads_add_idle                (GSourceFunc    func,
                                                                 gpointer       data);
CLUTTER_EXPORT
guint                   clutter_threads_add_idle_full           (gint           priority,
                                                                 GSourceFunc    func,
                                                                 gpointer       data,
                                                                 GDestroyNotify notify);
CLUTTER_EXPORT
guint                   clutter_threads_add_timeout             (guint          interval,
                                                                 GSourceFunc    func,
                                                                 gpointer       data);
CLUTTER_EXPORT
guint                   clutter_threads_add_timeout_full        (gint           priority,
                                                                 guint          interval,
                                                                 GSourceFunc    func,
                                                                 gpointer       data,
                                                                 GDestroyNotify notify);
CLUTTER_EXPORT
guint                   clutter_threads_add_repaint_func        (GSourceFunc    func,
                                                                 gpointer       data,
                                                                 GDestroyNotify notify);
CLUTTER_EXPORT
guint                   clutter_threads_add_repaint_func_full   (ClutterRepaintFlags flags,
                                                                 GSourceFunc    func,
                                                                 gpointer       data,
                                                                 GDestroyNotify notify);
CLUTTER_EXPORT
void                    clutter_threads_remove_repaint_func     (guint          handle_id);

CLUTTER_EXPORT
void                    clutter_grab_pointer                    (ClutterActor  *actor);
CLUTTER_EXPORT
void                    clutter_ungrab_pointer                  (void);
CLUTTER_EXPORT
ClutterActor *          clutter_get_pointer_grab                (void);
CLUTTER_EXPORT
void                    clutter_grab_keyboard                   (ClutterActor  *actor);
CLUTTER_EXPORT
void                    clutter_ungrab_keyboard                 (void);
CLUTTER_EXPORT
ClutterActor *          clutter_get_keyboard_grab               (void);

CLUTTER_EXPORT
PangoFontMap *          clutter_get_font_map                    (void);

CLUTTER_EXPORT
ClutterTextDirection    clutter_get_default_text_direction      (void);

CLUTTER_EXPORT
guint                   clutter_get_default_frame_rate          (void);

CLUTTER_EXPORT
gboolean                clutter_check_windowing_backend         (const char *backend_type);


G_END_DECLS

#endif /* _CLUTTER_MAIN_H__ */
