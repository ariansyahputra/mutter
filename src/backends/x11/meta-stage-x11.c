/*
 * Authored By Matthew Allum  <mallum@openedhand.com>
 * Copyright (C) 2006-2007 OpenedHand
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
 *
 */

#include "config.h"

#include <math.h>
#include <stdlib.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <cogl/cogl.h>
#include <clutter/clutter-mutter.h>
#include <clutter/x11/clutter-x11.h>
#include <clutter/x11/clutter-backend-x11.h>

#include "core/display-private.h"
#include "meta/meta-x11-errors.h"
#include "meta-backend-x11.h"
#include "meta-stage-x11.h"

#define STAGE_X11_IS_MAPPED(s)  ((((MetaStageX11 *) (s))->wm_state & STAGE_X11_WITHDRAWN) == 0)

static ClutterStageWindowInterface *clutter_stage_window_parent_iface = NULL;

static void
clutter_stage_window_iface_init (ClutterStageWindowInterface *iface);

static ClutterStageCogl *meta_x11_get_stage_window_from_window (Window win);

static GHashTable *clutter_stages_by_xid = NULL;

G_DEFINE_TYPE_WITH_CODE (MetaStageX11,
                         meta_stage_x11,
                         CLUTTER_TYPE_STAGE_COGL,
                         G_IMPLEMENT_INTERFACE (CLUTTER_TYPE_STAGE_WINDOW,
                                                clutter_stage_window_iface_init));

#define _NET_WM_STATE_REMOVE        0    /* remove/unset property */
#define _NET_WM_STATE_ADD           1    /* add/set property */
#define _NET_WM_STATE_TOGGLE        2    /* toggle property  */

#define META_STAGE_X11_EVENT_MASK \
  StructureNotifyMask | \
  FocusChangeMask | \
  ExposureMask | \
  PropertyChangeMask | \
  EnterWindowMask | \
  LeaveWindowMask | \
  KeyPressMask | \
  KeyReleaseMask | \
  ButtonPressMask | \
  ButtonReleaseMask | \
  PointerMotionMask

static void
send_wmspec_change_state (ClutterBackendX11 *backend_x11,
                          Window             window,
                          Atom               state,
                          gboolean           add)
{
  Display *xdisplay = clutter_x11_get_default_display ();
  XClientMessageEvent xclient;

  memset (&xclient, 0, sizeof (xclient));

  xclient.type         = ClientMessage;
  xclient.window       = window;
  xclient.message_type = backend_x11->atom_NET_WM_STATE;
  xclient.format       = 32;

  xclient.data.l[0] = add ? _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE;
  xclient.data.l[1] = state;
  xclient.data.l[2] = 0;
  xclient.data.l[3] = 0;
  xclient.data.l[4] = 0;

  XSendEvent (xdisplay,
              DefaultRootWindow (xdisplay),
              False,
              SubstructureRedirectMask | SubstructureNotifyMask,
              (XEvent *)&xclient);
}

static void
update_state (MetaStageX11      *stage_x11,
              ClutterBackendX11 *backend_x11,
              Atom              *state,
              gboolean           add)
{
  Display *xdisplay = clutter_x11_get_default_display ();

  if (add)
    {
      /* FIXME: This wont work if we support more states */
      XChangeProperty (xdisplay,
                       stage_x11->xwin,
                       backend_x11->atom_NET_WM_STATE, XA_ATOM, 32,
                       PropModeReplace,
                       (unsigned char *) state, 1);
    }
  else
    {
      /* FIXME: This wont work if we support more states */
      XDeleteProperty (xdisplay,
                       stage_x11->xwin,
                       backend_x11->atom_NET_WM_STATE);
    }
}

static void
meta_stage_x11_fix_window_size (MetaStageX11 *stage_x11,
                                int           new_width,
                                int           new_height)
{
  ClutterStageCogl *stage_cogl = CLUTTER_STAGE_COGL (stage_x11);

  if (stage_x11->xwin != None)
    {
      Display *xdisplay = clutter_x11_get_default_display ();
      guint min_width, min_height;
      XSizeHints *size_hints;
      gboolean resize;

      resize = clutter_stage_get_user_resizable (stage_cogl->wrapper);

      size_hints = XAllocSizeHints();

      clutter_stage_get_minimum_size (stage_cogl->wrapper,
                                      &min_width,
                                      &min_height);

      if (new_width <= 0)
        new_width = min_width;

      if (new_height <= 0)
        new_height = min_height;

      size_hints->flags = 0;

      /* If we are going fullscreen then we don't want any
         restrictions on the window size */
      if (!stage_x11->fullscreening)
        {
          if (resize)
            {
              size_hints->min_width = min_width;
              size_hints->min_height = min_height;
              size_hints->flags = PMinSize;
            }
          else
            {
              size_hints->min_width = new_width;
              size_hints->min_height = new_height;
              size_hints->max_width = new_width;
              size_hints->max_height = new_height;
              size_hints->flags = PMinSize | PMaxSize;
            }
        }

      XSetWMNormalHints (xdisplay, stage_x11->xwin, size_hints);

      XFree(size_hints);
    }
}

static void
meta_stage_x11_set_wm_protocols (MetaStageX11 *stage_x11)
{
  ClutterStageCogl *stage_cogl = CLUTTER_STAGE_COGL (stage_x11);
  ClutterBackendX11 *backend_x11 = CLUTTER_BACKEND_X11 (stage_cogl->backend);
  Display *xdisplay = clutter_x11_get_default_display ();
  Atom protocols[2];
  int n = 0;
  
  protocols[n++] = backend_x11->atom_WM_DELETE_WINDOW;
  protocols[n++] = backend_x11->atom_NET_WM_PING;

  XSetWMProtocols (xdisplay, stage_x11->xwin, protocols, n);
}

static void
meta_stage_x11_get_geometry (ClutterStageWindow    *stage_window,
                             cairo_rectangle_int_t *geometry)
{
  MetaStageX11 *stage_x11 = META_STAGE_X11 (stage_window);
  ClutterStageCogl *stage_cogl = CLUTTER_STAGE_COGL (stage_x11);
  ClutterBackendX11 *backend_x11 = CLUTTER_BACKEND_X11 (stage_cogl->backend);
  Display *xdisplay = clutter_x11_get_default_display ();

  geometry->x = geometry->y = 0;

  /* If we're fullscreen, return the size of the display.
   *
   * FIXME - this is utterly broken for anything that is not a single
   * head set up; the window manager will give us the right size in a
   * ConfigureNotify, but between the fullscreen signal emission on the
   * stage and the following frame, the size returned by the stage will
   * be wrong.
   */
  if (_clutter_stage_is_fullscreen (stage_cogl->wrapper) &&
      stage_x11->fullscreening)
    {
      geometry->width = DisplayWidth (xdisplay, backend_x11->xscreen_num);
      geometry->height = DisplayHeight (xdisplay, backend_x11->xscreen_num);

      return;
    }

  geometry->width = stage_x11->xwin_width;
  geometry->height = stage_x11->xwin_height;
}

static void
meta_stage_x11_resize (ClutterStageWindow *stage_window,
                       int                 width,
                       int                 height)
{
  MetaStageX11 *stage_x11 = META_STAGE_X11 (stage_window);

  /* If we're going fullscreen, don't mess with the size */
  if (stage_x11->fullscreening)
    return;

  if (width == 0 || height == 0)
    {
      /* Should not happen, if this turns up we need to debug it and
       * determine the cleanest way to fix.
       */
      g_warning ("X11 stage not allowed to have 0 width or height");
      width = 1;
      height = 1;
    }

  if (stage_x11->xwin != None)
    {
      meta_stage_x11_fix_window_size (stage_x11, width, height);

      if (width != stage_x11->xwin_width ||
          height != stage_x11->xwin_height)
        {
          Display *xdisplay = clutter_x11_get_default_display ();

          /* XXX: in this case we can rely on a subsequent
           * ConfigureNotify that will result in the stage
           * being reallocated so we don't actively do anything
           * to affect the stage allocation here. */
          XResizeWindow (xdisplay,
                         stage_x11->xwin,
                         width,
                         height);
        }
    }
  else
    {
      /* if the backing window hasn't been created yet, we just
       * need to store the new window size
       */
      stage_x11->xwin_width = width;
      stage_x11->xwin_height = height;
    }
}

static inline void
set_wm_pid (MetaStageX11 *stage_x11)
{
  ClutterStageCogl *stage_cogl = CLUTTER_STAGE_COGL (stage_x11);
  ClutterBackendX11 *backend_x11 = CLUTTER_BACKEND_X11 (stage_cogl->backend);
  Display *xdisplay = clutter_x11_get_default_display ();
  long pid;

  if (stage_x11->xwin == None)
    return;

  /* this will take care of WM_CLIENT_MACHINE and WM_LOCALE_NAME */
  XSetWMProperties (xdisplay, stage_x11->xwin,
                    NULL,
                    NULL,
                    NULL, 0,
                    NULL, NULL, NULL);

  pid = getpid ();
  XChangeProperty (xdisplay,
                   stage_x11->xwin,
                   backend_x11->atom_NET_WM_PID, XA_CARDINAL, 32,
                   PropModeReplace,
                   (guchar *) &pid, 1);
}

static inline void
set_wm_title (MetaStageX11 *stage_x11)
{
  ClutterStageCogl *stage_cogl = CLUTTER_STAGE_COGL (stage_x11);
  ClutterBackendX11 *backend_x11 = CLUTTER_BACKEND_X11 (stage_cogl->backend);
  Display *xdisplay = clutter_x11_get_default_display ();

  if (stage_x11->xwin == None)
    return;

  if (stage_x11->title == NULL)
    {
      XDeleteProperty (xdisplay,
                       stage_x11->xwin, 
                       backend_x11->atom_NET_WM_NAME);
    }
  else
    {
      XChangeProperty (xdisplay,
                       stage_x11->xwin, 
                       backend_x11->atom_NET_WM_NAME,
                       backend_x11->atom_UTF8_STRING,
                       8, 
                       PropModeReplace, 
                       (unsigned char *) stage_x11->title,
                       (int) strlen (stage_x11->title));
    }
}

static inline void
set_cursor_visible (MetaStageX11 *stage_x11)
{
  Display *xdisplay = clutter_x11_get_default_display ();

  if (stage_x11->xwin == None)
    return;

  if (stage_x11->is_cursor_visible)
    {
      XUndefineCursor (xdisplay, stage_x11->xwin);
    }
  else
    {
      XColor col;
      Pixmap pix;
      Cursor curs;

      pix = XCreatePixmap (xdisplay, stage_x11->xwin, 1, 1, 1);
      memset (&col, 0, sizeof (col));
      curs = XCreatePixmapCursor (xdisplay,
                                  pix, pix,
                                  &col, &col,
                                  1, 1);
      XFreePixmap (xdisplay, pix);
      XDefineCursor (xdisplay, stage_x11->xwin, curs);
    }
}

static void
meta_stage_x11_unrealize (ClutterStageWindow *stage_window)
{
  ClutterStageCogl *stage_cogl = CLUTTER_STAGE_COGL (stage_window);
  MetaStageX11 *stage_x11 = META_STAGE_X11 (stage_window);

  if (clutter_stages_by_xid != NULL)
    {
      g_hash_table_remove (clutter_stages_by_xid,
                           GINT_TO_POINTER (stage_x11->xwin));
    }

  /* Clutter still uses part of the deprecated stateful API of Cogl
   * (in particulart cogl_set_framebuffer). It means Cogl can keep an
   * internal reference to the onscreen object we rendered to. In the
   * case of foreign window, we want to avoid this, as we don't know
   * what's going to happen to that window.
   *
   * The following call sets the current Cogl framebuffer to a dummy
   * 1x1 one if we're unrealizing the current one, so Cogl doesn't
   * keep any reference to the foreign window.
   */
  if (cogl_get_draw_framebuffer () == COGL_FRAMEBUFFER (stage_x11->onscreen))
    _clutter_backend_reset_cogl_framebuffer (stage_cogl->backend);

  if (stage_x11->frame_closure)
    {
      cogl_onscreen_remove_frame_callback (stage_x11->onscreen,
                                           stage_x11->frame_closure);
      stage_x11->frame_closure = NULL;
    }

  clutter_stage_window_parent_iface->unrealize (stage_window);

  g_list_free (stage_x11->legacy_views);
  g_clear_object (&stage_x11->legacy_view);
  g_clear_pointer (&stage_x11->onscreen, cogl_object_unref);
}

static void
meta_stage_x11_set_fullscreen (ClutterStageWindow *stage_window,
                               gboolean            is_fullscreen)
{
  MetaStageX11 *stage_x11 = META_STAGE_X11 (stage_window);
  ClutterStageCogl *stage_cogl = CLUTTER_STAGE_COGL (stage_x11);
  ClutterBackendX11 *backend_x11 = CLUTTER_BACKEND_X11 (stage_cogl->backend);
  ClutterStage *stage = stage_cogl->wrapper;
  gboolean was_fullscreen;

  if (stage == NULL || CLUTTER_ACTOR_IN_DESTRUCTION (stage))
    return;

  was_fullscreen = _clutter_stage_is_fullscreen (stage);
  is_fullscreen = !!is_fullscreen;

  if (was_fullscreen == is_fullscreen)
    return;

  if (is_fullscreen)
    {
#if 0
      int width, height;

      /* FIXME: this will do the wrong thing for dual-headed
         displays. This will return the size of the combined display
         but Metacity (at least) will fullscreen to only one of the
         displays. This will cause the actor to report the wrong size
         until the ConfigureNotify for the correct size is received */
      width  = DisplayWidth (xdisplay, backend_x11->xscreen_num);
      height = DisplayHeight (xdisplay, backend_x11->xscreen_num);
#endif

      /* Set the fullscreen hint so we can retain the old size of the window. */
      stage_x11->fullscreening = TRUE;

      if (stage_x11->xwin != None)
        {
          /* if the actor is not mapped we resize the stage window to match
           * the size of the screen; this is useful for e.g. EGLX to avoid
           * a resize when calling clutter_stage_fullscreen() before showing
           * the stage
           */
          if (!STAGE_X11_IS_MAPPED (stage_x11))
            {
              update_state (stage_x11, backend_x11,
                            &backend_x11->atom_NET_WM_STATE_FULLSCREEN,
                            TRUE);
            }
          else
            {
              /* We need to fix the window size so that it will remove
                 the maximum and minimum window hints. Otherwise
                 metacity will honour the restrictions and not
                 fullscreen correctly. */
              meta_stage_x11_fix_window_size (stage_x11, -1, -1);

              send_wmspec_change_state (backend_x11, stage_x11->xwin,
                                        backend_x11->atom_NET_WM_STATE_FULLSCREEN,
                                        TRUE);
            }
        }
      else
        stage_x11->fullscreen_on_realize = TRUE;
    }
  else
    {
      stage_x11->fullscreening = FALSE;

      if (stage_x11->xwin != None)
        {
          if (!STAGE_X11_IS_MAPPED (stage_x11))
            {
              update_state (stage_x11, backend_x11,
                            &backend_x11->atom_NET_WM_STATE_FULLSCREEN,
                            FALSE);
            }
          else
            {
              send_wmspec_change_state (backend_x11,
                                        stage_x11->xwin,
                                        backend_x11->atom_NET_WM_STATE_FULLSCREEN,
                                        FALSE);

              /* Fix the window size to restore the minimum/maximum
                 restriction */
              meta_stage_x11_fix_window_size (stage_x11,
                                              stage_x11->xwin_width,
                                              stage_x11->xwin_height);
            }
        }
      else
        stage_x11->fullscreen_on_realize = FALSE;
    }

  /* XXX: Note we rely on the ConfigureNotify mechanism as the common
   * mechanism to handle notifications of new X window sizes from the
   * X server so we don't actively change the stage viewport here or
   * queue a relayout etc. */
}

void
meta_stage_x11_events_device_changed (MetaStageX11         *stage_x11,
                                      ClutterInputDevice   *device,
                                      ClutterDeviceManager *device_manager)
{
  ClutterStageCogl *stage_cogl = CLUTTER_STAGE_COGL (stage_x11);

  if (clutter_input_device_get_device_mode (device) == CLUTTER_INPUT_MODE_FLOATING)
    _clutter_device_manager_select_stage_events (device_manager,
                                                 stage_cogl->wrapper);
}

static void
stage_events_device_added (ClutterDeviceManager *device_manager,
                           ClutterInputDevice *device,
                           gpointer user_data)
{
  ClutterStageWindow *stage_window = user_data;
  ClutterStageCogl *stage_cogl = CLUTTER_STAGE_COGL (stage_window);

  if (clutter_input_device_get_device_mode (device) == CLUTTER_INPUT_MODE_FLOATING)
    _clutter_device_manager_select_stage_events (device_manager,
                                                 stage_cogl->wrapper);
}

static void
frame_cb (CoglOnscreen  *onscreen,
          CoglFrameEvent frame_event,
          CoglFrameInfo *frame_info,
          void          *user_data)

{
  ClutterStageCogl *stage_cogl = user_data;
  ClutterFrameInfo clutter_frame_info = {
    .frame_counter = cogl_frame_info_get_frame_counter (frame_info),
    .presentation_time = cogl_frame_info_get_presentation_time (frame_info),
    .refresh_rate = cogl_frame_info_get_refresh_rate (frame_info)
  };

  _clutter_stage_cogl_presented (stage_cogl, frame_event, &clutter_frame_info);
}

static gboolean
meta_stage_x11_realize (ClutterStageWindow *stage_window)
{
  MetaStageX11 *stage_x11 = META_STAGE_X11 (stage_window);
  ClutterStageCogl *stage_cogl = CLUTTER_STAGE_COGL (stage_window);
  ClutterBackend *backend = CLUTTER_BACKEND (stage_cogl->backend);
  Display *xdisplay = clutter_x11_get_default_display ();
  ClutterDeviceManager *device_manager;
  gfloat width, height;
  GError *error = NULL;

  clutter_actor_get_size (CLUTTER_ACTOR (stage_cogl->wrapper), &width, &height);

  stage_x11->onscreen = cogl_onscreen_new (backend->cogl_context, width, height);

  stage_x11->frame_closure =
    cogl_onscreen_add_frame_callback (stage_x11->onscreen,
                                      frame_cb,
                                      stage_cogl,
                                      NULL);

  if (stage_x11->legacy_view)
    g_object_set (G_OBJECT (stage_x11->legacy_view),
                  "framebuffer", stage_x11->onscreen,
                  NULL);

  /* We just created a window of the size of the actor. No need to fix
     the size of the stage, just update it. */
  stage_x11->xwin_width = width;
  stage_x11->xwin_height = height;

  if (!cogl_framebuffer_allocate (COGL_FRAMEBUFFER (stage_x11->onscreen), &error))
    {
      g_warning ("Failed to allocate stage: %s", error->message);
      g_error_free (error);
      cogl_object_unref (stage_x11->onscreen);
      abort();
    }

  if (!(clutter_stage_window_parent_iface->realize (stage_window)))
    return FALSE;

  stage_x11->xwin = cogl_x11_onscreen_get_window_xid (stage_x11->onscreen);

  if (clutter_stages_by_xid == NULL)
    clutter_stages_by_xid = g_hash_table_new (NULL, NULL);

  g_hash_table_insert (clutter_stages_by_xid,
                       GINT_TO_POINTER (stage_x11->xwin),
                       stage_x11);

  set_wm_pid (stage_x11);
  set_wm_title (stage_x11);
  set_cursor_visible (stage_x11);

  /* we unconditionally select input events even with event retrieval
   * disabled because we need to guarantee that the Clutter internal
   * state is maintained when calling clutter_x11_handle_event() without
   * requiring applications or embedding toolkits to select events
   * themselves. if we did that, we'd have to document the events to be
   * selected, and also update applications and embedding toolkits each
   * time we added a new mask, or a new class of events.
   *
   * see: http://bugzilla.clutter-project.org/show_bug.cgi?id=998
   * for the rationale of why we did conditional selection. it is now
   * clear that a compositor should clear out the input region, since
   * it cannot assume a perfectly clean slate coming from us.
   *
   * see: http://bugzilla.clutter-project.org/show_bug.cgi?id=2228
   * for an example of things that break if we do conditional event
   * selection.
   */
  XSelectInput (xdisplay, stage_x11->xwin, META_STAGE_X11_EVENT_MASK);

  /* input events also depent on the actual device, so we need to
   * use the device manager to let every device select them, using
   * the event mask we passed to XSelectInput as the template
   */
  device_manager = clutter_device_manager_get_default ();
  if (G_UNLIKELY (device_manager != NULL))
    {
      _clutter_device_manager_select_stage_events (device_manager,
                                                   stage_cogl->wrapper);

      g_signal_connect (device_manager, "device-added",
                        G_CALLBACK (stage_events_device_added),
                        stage_window);
    }

  meta_stage_x11_fix_window_size (stage_x11,
                                  stage_x11->xwin_width,
                                  stage_x11->xwin_height);
  meta_stage_x11_set_wm_protocols (stage_x11);

  if (stage_x11->fullscreen_on_realize)
    {
      stage_x11->fullscreen_on_realize = FALSE;

      meta_stage_x11_set_fullscreen (stage_window, TRUE);
    }

  return TRUE;
}

static void
meta_stage_x11_set_cursor_visible (ClutterStageWindow *stage_window,
                                   gboolean            cursor_visible)
{
  MetaStageX11 *stage_x11 = META_STAGE_X11 (stage_window);

  stage_x11->is_cursor_visible = !!cursor_visible;
  set_cursor_visible (stage_x11);
}

static void
meta_stage_x11_set_title (ClutterStageWindow *stage_window,
                          const char         *title)
{
  MetaStageX11 *stage_x11 = META_STAGE_X11 (stage_window);

  g_free (stage_x11->title);
  stage_x11->title = g_strdup (title);
  set_wm_title (stage_x11);
}

static void
meta_stage_x11_set_user_resizable (ClutterStageWindow *stage_window,
                                   gboolean            is_resizable)
{
  MetaStageX11 *stage_x11 = META_STAGE_X11 (stage_window);

  meta_stage_x11_fix_window_size (stage_x11,
                                  stage_x11->xwin_width,
                                  stage_x11->xwin_height);
}

static inline void
update_wm_hints (MetaStageX11 *stage_x11)
{
  Display *xdisplay = clutter_x11_get_default_display ();
  XWMHints wm_hints;

  if (stage_x11->wm_state & STAGE_X11_WITHDRAWN)
    return;

  wm_hints.flags = StateHint | InputHint;
  wm_hints.initial_state = NormalState;
  wm_hints.input = stage_x11->accept_focus ? True : False;

  XSetWMHints (xdisplay, stage_x11->xwin, &wm_hints);
}

static void
meta_stage_x11_set_accept_focus (ClutterStageWindow *stage_window,
                                 gboolean            accept_focus)
{
  MetaStageX11 *stage_x11 = META_STAGE_X11 (stage_window);

  stage_x11->accept_focus = !!accept_focus;
  update_wm_hints (stage_x11);
}

static void
set_stage_x11_state (MetaStageX11      *stage_x11,
                     MetaStageX11State  unset_flags,
                     MetaStageX11State  set_flags)
{
  MetaStageX11State new_stage_state, old_stage_state;

  old_stage_state = stage_x11->wm_state;

  new_stage_state = old_stage_state;
  new_stage_state |= set_flags;
  new_stage_state &= ~unset_flags;

  if (new_stage_state == old_stage_state)
    return;

  stage_x11->wm_state = new_stage_state;
}

static void
meta_stage_x11_show (ClutterStageWindow *stage_window,
                     gboolean            do_raise)
{
  MetaStageX11 *stage_x11 = META_STAGE_X11 (stage_window);
  ClutterStageCogl *stage_cogl = CLUTTER_STAGE_COGL (stage_x11);

  if (stage_x11->xwin != None)
    {
      Display *xdisplay = clutter_x11_get_default_display ();

      if (do_raise)
        {
          XRaiseWindow (xdisplay, stage_x11->xwin);
        }

      if (!STAGE_X11_IS_MAPPED (stage_x11))
        {
          set_stage_x11_state (stage_x11, STAGE_X11_WITHDRAWN, 0);

          update_wm_hints (stage_x11);

          if (stage_x11->fullscreening)
            meta_stage_x11_set_fullscreen (stage_window, TRUE);
          else
            meta_stage_x11_set_fullscreen (stage_window, FALSE);
        }

      g_assert (STAGE_X11_IS_MAPPED (stage_x11));

      clutter_actor_map (CLUTTER_ACTOR (stage_cogl->wrapper));

      XMapWindow (xdisplay, stage_x11->xwin);
    }
}

static void
meta_stage_x11_hide (ClutterStageWindow *stage_window)
{
  MetaStageX11 *stage_x11 = META_STAGE_X11 (stage_window);
  ClutterStageCogl *stage_cogl = CLUTTER_STAGE_COGL (stage_x11);

  if (stage_x11->xwin != None)
    {
      Display *xdisplay = clutter_x11_get_default_display ();

      if (STAGE_X11_IS_MAPPED (stage_x11))
        set_stage_x11_state (stage_x11, 0, STAGE_X11_WITHDRAWN);

      g_assert (!STAGE_X11_IS_MAPPED (stage_x11));

      clutter_actor_unmap (CLUTTER_ACTOR (stage_cogl->wrapper));

      XWithdrawWindow (xdisplay, stage_x11->xwin, 0);
    }
}

static gboolean
meta_stage_x11_can_clip_redraws (ClutterStageWindow *stage_window)
{
  MetaStageX11 *stage_x11 = META_STAGE_X11 (stage_window);

  /* while resizing a window, clipped redraws are disabled in order to
   * avoid artefacts.
   */
  return stage_x11->clipped_redraws_cool_off == 0;
}

static void
ensure_legacy_view (ClutterStageWindow *stage_window)
{
  MetaStageX11 *stage_x11 = META_STAGE_X11 (stage_window);
  cairo_rectangle_int_t view_layout;
  CoglFramebuffer *framebuffer;

  if (stage_x11->legacy_view)
    return;

  _clutter_stage_window_get_geometry (stage_window, &view_layout);
  framebuffer = COGL_FRAMEBUFFER (stage_x11->onscreen);
  stage_x11->legacy_view = g_object_new (CLUTTER_TYPE_STAGE_VIEW_COGL,
                                         "layout", &view_layout,
                                         "framebuffer", framebuffer,
                                         NULL);
  stage_x11->legacy_views = g_list_append (stage_x11->legacy_views,
                                           stage_x11->legacy_view);
}

static GList *
meta_stage_x11_get_views (ClutterStageWindow *stage_window)
{
  MetaStageX11 *stage_x11 = META_STAGE_X11 (stage_window);

  ensure_legacy_view (stage_window);

  return stage_x11->legacy_views;
}

static int64_t
meta_stage_x11_get_frame_counter (ClutterStageWindow *stage_window)
{
  MetaStageX11 *stage_x11 = META_STAGE_X11 (stage_window);

  return cogl_onscreen_get_frame_counter (stage_x11->onscreen);
}

static void
meta_stage_x11_finalize (GObject *gobject)
{
  MetaStageX11 *stage_x11 = META_STAGE_X11 (gobject);

  g_free (stage_x11->title);

  G_OBJECT_CLASS (meta_stage_x11_parent_class)->finalize (gobject);
}

static void
meta_stage_x11_class_init (MetaStageX11Class *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = meta_stage_x11_finalize;
}

static void
meta_stage_x11_init (MetaStageX11 *stage)
{
  stage->xwin = None;
  stage->xwin_width = 640;
  stage->xwin_height = 480;

  stage->wm_state = STAGE_X11_WITHDRAWN;

  stage->fullscreening = FALSE;
  stage->is_cursor_visible = TRUE;
  stage->accept_focus = TRUE;

  stage->title = NULL;
}

static void
clutter_stage_window_iface_init (ClutterStageWindowInterface *iface)
{
  clutter_stage_window_parent_iface = g_type_interface_peek_parent (iface);

  iface->set_title = meta_stage_x11_set_title;
  iface->set_fullscreen = meta_stage_x11_set_fullscreen;
  iface->set_cursor_visible = meta_stage_x11_set_cursor_visible;
  iface->set_user_resizable = meta_stage_x11_set_user_resizable;
  iface->set_accept_focus = meta_stage_x11_set_accept_focus;
  iface->show = meta_stage_x11_show;
  iface->hide = meta_stage_x11_hide;
  iface->resize = meta_stage_x11_resize;
  iface->get_geometry = meta_stage_x11_get_geometry;
  iface->realize = meta_stage_x11_realize;
  iface->unrealize = meta_stage_x11_unrealize;
  iface->can_clip_redraws = meta_stage_x11_can_clip_redraws;
  iface->get_views = meta_stage_x11_get_views;
  iface->get_frame_counter = meta_stage_x11_get_frame_counter;
}

static inline void
set_user_time (ClutterBackendX11 *backend_x11,
               MetaStageX11      *stage_x11,
               long               timestamp)
{
  if (timestamp != CLUTTER_CURRENT_TIME)
    {
      Display *xdisplay = clutter_x11_get_default_display ();

      XChangeProperty (xdisplay,
                       stage_x11->xwin,
                       backend_x11->atom_NET_WM_USER_TIME,
                       XA_CARDINAL, 32,
                       PropModeReplace,
                       (unsigned char *) &timestamp, 1);
    }
}

static gboolean
handle_wm_protocols_event (ClutterBackendX11 *backend_x11,
                           MetaStageX11      *stage_x11,
                           XEvent            *xevent)
{
  Atom atom = (Atom) xevent->xclient.data.l[0];

  if (atom == backend_x11->atom_WM_DELETE_WINDOW &&
      xevent->xany.window == stage_x11->xwin)
    {
      set_user_time (backend_x11, stage_x11, xevent->xclient.data.l[1]);

      return TRUE;
    }
  else if (atom == backend_x11->atom_NET_WM_PING &&
           xevent->xany.window == stage_x11->xwin)
    {
      XClientMessageEvent xclient = xevent->xclient;
      Display *xdisplay = clutter_x11_get_default_display ();

      xclient.window = backend_x11->xwin_root;
      XSendEvent (xdisplay, xclient.window,
                  False,
                  SubstructureRedirectMask | SubstructureNotifyMask,
                  (XEvent *) &xclient);
      return FALSE;
    }

  /* do not send any of the WM_PROTOCOLS events to the queue */
  return FALSE;
}

static gboolean
clipped_redraws_cool_off_cb (void *data)
{
  MetaStageX11 *stage_x11 = data;

  stage_x11->clipped_redraws_cool_off = 0;

  return G_SOURCE_REMOVE;
}

gboolean
meta_stage_x11_translate_event (MetaStageX11 *stage_x11,
                                XEvent       *xevent,
                                ClutterEvent *event)
{
  ClutterStageCogl *stage_cogl;
  gboolean res = FALSE;
  ClutterBackendX11 *backend_x11;
  Window stage_xwindow;
  ClutterStage *stage;

  stage_cogl = meta_x11_get_stage_window_from_window (xevent->xany.window);
  if (stage_cogl == NULL)
    return FALSE;

  stage = stage_cogl->wrapper;
  backend_x11 = CLUTTER_BACKEND_X11 (stage_cogl->backend);
  stage_xwindow = stage_x11->xwin;

  switch (xevent->type)
    {
    case ConfigureNotify:
        {
          gboolean size_changed = FALSE;
          int stage_width;
          int stage_height;

          /* When fullscreen, we'll keep the xwin_width/height
             variables to track the old size of the window and we'll
             assume all ConfigureNotifies constitute a size change */
          if (_clutter_stage_is_fullscreen (stage))
            size_changed = TRUE;
          else if ((stage_x11->xwin_width != xevent->xconfigure.width) ||
                   (stage_x11->xwin_height != xevent->xconfigure.height))
            {
              size_changed = TRUE;
              stage_x11->xwin_width = xevent->xconfigure.width;
              stage_x11->xwin_height = xevent->xconfigure.height;
            }

          stage_width = xevent->xconfigure.width;
          stage_height = xevent->xconfigure.height;
          clutter_actor_set_size (CLUTTER_ACTOR (stage), stage_width, stage_height);

          if (size_changed)
            {
              /* XXX: This is a workaround for a race condition when
               * resizing windows while there are in-flight
               * glXCopySubBuffer blits happening.
               *
               * The problem stems from the fact that rectangles for the
               * blits are described relative to the bottom left of the
               * window and because we can't guarantee control over the X
               * window gravity used when resizing so the gravity is
               * typically NorthWest not SouthWest.
               *
               * This means if you grow a window vertically the server
               * will make sure to place the old contents of the window
               * at the top-left/north-west of your new larger window, but
               * that may happen asynchronous to GLX preparing to do a
               * blit specified relative to the bottom-left/south-west of
               * the window (based on the old smaller window geometry).
               *
               * When the GLX issued blit finally happens relative to the
               * new bottom of your window, the destination will have
               * shifted relative to the top-left where all the pixels you
               * care about are so it will result in a nasty artefact
               * making resizing look very ugly!
               *
               * We can't currently fix this completely, in-part because
               * the window manager tends to trample any gravity we might
               * set.  This workaround instead simply disables blits for a
               * while if we are notified of any resizes happening so if
               * the user is resizing a window via the window manager then
               * they may see an artefact for one frame but then we will
               * fallback to redrawing the full stage until the cooling
               * off period is over.
               */
              if (stage_x11->clipped_redraws_cool_off)
                g_source_remove (stage_x11->clipped_redraws_cool_off);

              stage_x11->clipped_redraws_cool_off =
                clutter_threads_add_timeout (1000,
                                             clipped_redraws_cool_off_cb,
                                             stage_x11);

              /* Queue a relayout - we want glViewport to be called
               * with the correct values, and this is done in ClutterStage
               * via cogl_onscreen_clutter_backend_set_size ().
               *
               * We queue a relayout, because if this ConfigureNotify is
               * in response to a size we set in the application, the
               * set_size() call above is essentially a null-op.
               *
               * Make sure we do this only when the size has changed,
               * otherwise we end up relayouting on window moves.
               */
              clutter_actor_queue_relayout (CLUTTER_ACTOR (stage));

              /* the resize process is complete, so we can ask the stage
               * to set up the GL viewport with the new size
               */
              clutter_stage_ensure_viewport (stage);

              /* If this was a result of the Xrandr change when running as a
               * X11 compositing manager, we need to reset the legacy
               * stage view, now that it has a new size.
               */
              if (stage_x11->legacy_view)
                {
                  cairo_rectangle_int_t view_layout = {
                    .width = stage_width,
                    .height = stage_height
                  };

                  g_object_set (G_OBJECT (stage_x11->legacy_view),
                                "layout", &view_layout,
                                NULL);
                }
            }
        }
      break;

    case PropertyNotify:
      if (xevent->xproperty.atom == backend_x11->atom_NET_WM_STATE &&
          xevent->xproperty.window == stage_xwindow)
        {
          Display *xdisplay = clutter_x11_get_default_display ();
          Atom     type;
          int      format;
          gulong   n_items, bytes_after;
          guchar  *data = NULL;
          gboolean fullscreen_set = FALSE;

          clutter_x11_trap_x_errors ();
          XGetWindowProperty (xdisplay, stage_xwindow,
                              backend_x11->atom_NET_WM_STATE,
                              0, G_MAXLONG,
                              False, XA_ATOM,
                              &type, &format, &n_items,
                              &bytes_after, &data);
          clutter_x11_untrap_x_errors ();

          if (type != None && data != NULL)
            {
              gboolean is_fullscreen = FALSE;
              Atom *atoms = (Atom *) data;
              gulong i;

              for (i = 0; i < n_items; i++)
                {
                  if (atoms[i] == backend_x11->atom_NET_WM_STATE_FULLSCREEN)
                    fullscreen_set = TRUE;
                }

              is_fullscreen = _clutter_stage_is_fullscreen (stage_cogl->wrapper);

              if (fullscreen_set != is_fullscreen)
                {
                  if (fullscreen_set)
                    _clutter_stage_update_state (stage_cogl->wrapper,
                                                 0,
                                                 CLUTTER_STAGE_STATE_FULLSCREEN);
                  else
                    _clutter_stage_update_state (stage_cogl->wrapper,
                                                 CLUTTER_STAGE_STATE_FULLSCREEN,
                                                 0);
                }

              XFree (data);
            }
        }
      break;

    case FocusIn:
      if (!_clutter_stage_is_activated (stage_cogl->wrapper))
        {
          _clutter_stage_update_state (stage_cogl->wrapper,
                                       0,
                                       CLUTTER_STAGE_STATE_ACTIVATED);
        }
      break;

    case FocusOut:
      if (_clutter_stage_is_activated (stage_cogl->wrapper))
        {
          _clutter_stage_update_state (stage_cogl->wrapper,
                                       CLUTTER_STAGE_STATE_ACTIVATED,
                                       0);
        }
      break;

    case Expose:
      {
        XExposeEvent *expose = (XExposeEvent *) xevent;
        cairo_rectangle_int_t clip;

        clip.x = expose->x;
        clip.y = expose->y;
        clip.width = expose->width;
        clip.height = expose->height;
        clutter_actor_queue_redraw_with_clip (CLUTTER_ACTOR (stage), &clip);
      }
      break;

    case DestroyNotify:
      event->any.type = CLUTTER_DESTROY_NOTIFY;
      event->any.stage = stage;
      res = TRUE;
      break;

    case ClientMessage:
      if (handle_wm_protocols_event (backend_x11, stage_x11, xevent))
        {
          event->any.type = CLUTTER_DELETE;
          event->any.stage = stage;
          res = TRUE;
        }
      break;

    default:
      res = FALSE;
      break;
    }

  return res;
}

Window
meta_x11_get_stage_window (ClutterStage *stage)
{
  ClutterStageWindow *impl;

  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), None);

  impl = _clutter_stage_get_window (stage);
  g_assert (META_IS_STAGE_X11 (impl));

  return META_STAGE_X11 (impl)->xwin;
}

static ClutterStageCogl *
meta_x11_get_stage_window_from_window (Window win)
{
  if (clutter_stages_by_xid == NULL)
    return NULL;

  return g_hash_table_lookup (clutter_stages_by_xid,
                              GINT_TO_POINTER (win));
}

ClutterStage *
meta_x11_get_stage_from_window (Window win)
{
  ClutterStageCogl *stage_cogl;

  stage_cogl = meta_x11_get_stage_window_from_window (win);

  if (stage_cogl != NULL)
    return stage_cogl->wrapper;

  return NULL;
}

void
meta_stage_x11_set_user_time (MetaStageX11 *stage_x11,
                              guint32       user_time)
{
  ClutterStageCogl *stage_cogl = CLUTTER_STAGE_COGL (stage_x11);
  ClutterBackendX11 *backend_x11 = CLUTTER_BACKEND_X11 (stage_cogl->backend);

  set_user_time (backend_x11, stage_x11, user_time);
}