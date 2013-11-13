/* GTK - The GIMP Toolkit
 * Copyright © 2013 Carlos Garnacho <carlosg@gnome.org>
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

/**
 * SECTION:gtkpopover
 * @Short_description: Context dependent bubbles
 * @Title: GtkPopover
 *
 * GtkPopover is a bubble-like context window, primarily meant to
 * provide context-dependent information or options. Popovers are
 * attached to a widget, passed at construction time on gtk_popover_new(),
 * or updated afterwards through gtk_popover_set_relative_to(), by
 * default they will point to the whole widget area, although this
 * behavior can be changed through gtk_popover_set_pointing_to().
 *
 * The position of a popover relative to the widget it is attached to
 * can also be changed through gtk_popover_set_position().
 *
 * By default, no grabs are performed on #GtkPopover<!-- -->s, if a
 * modal behavior is desired, a GTK+ grab can be added with gtk_grab_add()
 */

#include "config.h"
#include <gdk/gdk.h>
#include <cairo-gobject.h>
#include "gtkpopover.h"
#include "gtktypebuiltins.h"
#include "gtkmain.h"
#include "gtkprivate.h"
#include "gtkintl.h"

#define TAIL_GAP_WIDTH 24
#define TAIL_HEIGHT    12

#define POS_IS_VERTICAL(p) ((p) == GTK_POS_TOP || (p) == GTK_POS_BOTTOM)

typedef struct _GtkPopoverPrivate GtkPopoverPrivate;

enum {
  PROP_RELATIVE_TO = 1,
  PROP_POINTING_TO,
  PROP_POSITION
};

struct _GtkPopoverPrivate
{
  GtkWidget *widget;
  GtkWindow *window;
  cairo_rectangle_int_t pointing_to;
  guint hierarchy_changed_id;
  guint size_allocate_id;
  guint unmap_id;
  gint win_x;
  gint win_y;
  guint has_pointing_to    : 1;
  guint preferred_position : 2;
  guint final_position     : 2;
};

G_DEFINE_TYPE_WITH_PRIVATE (GtkPopover, gtk_popover, GTK_TYPE_BIN)

static void
gtk_popover_init (GtkPopover *popover)
{
  GtkWidget *widget;

  widget = GTK_WIDGET (popover);
  gtk_widget_set_has_window (widget, TRUE);
  popover->priv = gtk_popover_get_instance_private (popover);
  gtk_style_context_add_class (gtk_widget_get_style_context (widget),
                               GTK_STYLE_CLASS_OSD);
}

static void
gtk_popover_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
  switch (prop_id)
    {
    case PROP_RELATIVE_TO:
      gtk_popover_set_relative_to (GTK_POPOVER (object),
                                   g_value_get_object (value));
      break;
    case PROP_POINTING_TO:
      gtk_popover_set_pointing_to (GTK_POPOVER (object),
                                   g_value_get_boxed (value));
      break;
    case PROP_POSITION:
      gtk_popover_set_position (GTK_POPOVER (object),
                                g_value_get_enum (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gtk_popover_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  GtkPopoverPrivate *priv = GTK_POPOVER (object)->priv;

  switch (prop_id)
    {
    case PROP_RELATIVE_TO:
      g_value_set_object (value, priv->widget);
      break;
    case PROP_POINTING_TO:
      g_value_set_boxed (value, &priv->pointing_to);
      break;
    case PROP_POSITION:
      g_value_set_enum (value, priv->preferred_position);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gtk_popover_finalize (GObject *object)
{
  GtkPopover *popover = GTK_POPOVER (object);

  G_OBJECT_CLASS (gtk_popover_parent_class)->finalize (object);
}

static void
gtk_popover_dispose (GObject *object)
{
  GtkPopover *popover = GTK_POPOVER (object);
  GtkPopoverPrivate *priv = popover->priv;

  if (priv->window)
    gtk_window_remove_popover (priv->window, GTK_WIDGET (object));

  priv->window = NULL;
  priv->widget = NULL;

  G_OBJECT_CLASS (gtk_popover_parent_class)->dispose (object);
}

static void
gtk_popover_realize (GtkWidget *widget)
{
  GtkAllocation allocation;
  GdkWindowAttr attributes;
  gint attributes_mask;
  GdkWindow *window;

  gtk_widget_get_allocation (widget, &allocation);

  attributes.x = 0;
  attributes.y = 0;
  attributes.width = allocation.width;
  attributes.height = allocation.height;
  attributes.window_type = GDK_WINDOW_CHILD;
  attributes.visual = gtk_widget_get_visual (widget);
  attributes.wclass = GDK_INPUT_OUTPUT;
  attributes.event_mask =
    gtk_widget_get_events (widget) |
    GDK_BUTTON_MOTION_MASK |
    GDK_BUTTON_PRESS_MASK |
    GDK_BUTTON_RELEASE_MASK |
    GDK_EXPOSURE_MASK |
    GDK_ENTER_NOTIFY_MASK |
    GDK_LEAVE_NOTIFY_MASK;

  attributes_mask = GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL;
  window = gdk_window_new (gtk_widget_get_parent_window (widget),
                           &attributes, attributes_mask);
  gtk_widget_set_window (widget, window);
  gtk_widget_register_window (widget, window);
  gtk_widget_set_realized (widget, TRUE);
}

static void
gtk_popover_map (GtkWidget *widget)
{
  gdk_window_show (gtk_widget_get_window (widget));
  GTK_WIDGET_CLASS (gtk_popover_parent_class)->map (widget);
}

static void
gtk_popover_unmap (GtkWidget *widget)
{
  gtk_grab_remove (widget);
  gdk_window_hide (gtk_widget_get_window (widget));
  GTK_WIDGET_CLASS (gtk_popover_parent_class)->unmap (widget);
}

static void
gtk_popover_get_pointed_to_coords (GtkPopover            *popover,
                                   gint                  *x,
                                   gint                  *y,
                                   cairo_rectangle_int_t *rect_out)
{
  GtkPopoverPrivate *priv = popover->priv;
  cairo_rectangle_int_t rect;
  GtkAllocation window_alloc;

  gtk_popover_get_pointing_to (popover, &rect);
  gtk_widget_get_allocation (GTK_WIDGET (priv->window), &window_alloc);
  gtk_widget_translate_coordinates (priv->widget, GTK_WIDGET (priv->window),
                                    rect.x, rect.y, &rect.x, &rect.y);

  if (POS_IS_VERTICAL (priv->final_position))
    {
      *x = CLAMP (rect.x + (rect.width / 2), 0, window_alloc.width);
      *y = rect.y;

      if (priv->final_position == GTK_POS_BOTTOM)
        (*y) += rect.height;
    }
  else
    {
      *y = CLAMP (rect.y + (rect.height / 2), 0, window_alloc.height);
      *x = rect.x;

      if (priv->final_position == GTK_POS_RIGHT)
        (*x) += rect.width;
    }

  if (rect_out)
    *rect_out = rect;
}

static void
gtk_popover_get_gap_coords (GtkPopover      *popover,
                            gint            *initial_x_out,
                            gint            *initial_y_out,
                            gint            *tip_x_out,
                            gint            *tip_y_out,
                            gint            *final_x_out,
                            gint            *final_y_out,
                            GtkPositionType *gap_side_out)
{
  GtkPopoverPrivate *priv = popover->priv;
  gint base, tip, x, y;
  gint initial_x, initial_y;
  gint tip_x, tip_y;
  gint final_x, final_y;
  GtkPositionType gap_side;
  GtkAllocation allocation;
  gint border_radius;

  gtk_popover_get_pointed_to_coords (popover, &x, &y, NULL);
  gtk_widget_get_allocation (GTK_WIDGET (popover), &allocation);

  gtk_style_context_get (gtk_widget_get_style_context (GTK_WIDGET (popover)),
                         gtk_widget_get_state_flags (GTK_WIDGET (popover)),
                         GTK_STYLE_PROPERTY_BORDER_RADIUS, &border_radius,
                         NULL);

  base = tip = 0;
  gap_side = GTK_POS_LEFT;

  if (priv->final_position == GTK_POS_BOTTOM ||
      priv->final_position == GTK_POS_RIGHT)
    {
      base = TAIL_HEIGHT;
      tip = 0;

      gap_side = (priv->final_position == GTK_POS_BOTTOM) ? GTK_POS_TOP : GTK_POS_LEFT;
    }
  else if (priv->final_position == GTK_POS_TOP)
    {
      base = allocation.height - TAIL_HEIGHT;
      tip = allocation.height;
      gap_side = GTK_POS_BOTTOM;
    }
  else if (priv->final_position == GTK_POS_LEFT)
    {
      base = allocation.width - TAIL_HEIGHT;
      tip = allocation.width;
      gap_side = GTK_POS_RIGHT;
    }

  if (POS_IS_VERTICAL (priv->final_position))
    {
      initial_x = CLAMP (x - priv->win_x - TAIL_GAP_WIDTH / 2,
                         border_radius, allocation.width - TAIL_GAP_WIDTH);
      initial_y = base;

      tip_x = CLAMP (x - priv->win_x, 0, allocation.width);
      tip_y = tip;

      final_x = CLAMP (x - priv->win_x + TAIL_GAP_WIDTH / 2,
                       TAIL_GAP_WIDTH, allocation.width - border_radius);
      final_y = base;
    }
  else
    {
      initial_x = base;
      initial_y = CLAMP (y - priv->win_y - TAIL_GAP_WIDTH / 2,
                         border_radius, allocation.height - TAIL_GAP_WIDTH);

      tip_x = tip;
      tip_y = CLAMP (y - priv->win_y, 0, allocation.height);

      final_x = base;
      final_y = CLAMP (y - priv->win_y + TAIL_GAP_WIDTH / 2,
                       TAIL_GAP_WIDTH, allocation.height - border_radius);
    }

  if (initial_x_out)
    *initial_x_out = initial_x;
  if (initial_y_out)
    *initial_y_out = initial_y;

  if (tip_x_out)
    *tip_x_out = tip_x;
  if (tip_y_out)
    *tip_y_out = tip_y;

  if (final_x_out)
    *final_x_out = final_x;
  if (final_y_out)
    *final_y_out = final_y;

  if (gap_side_out)
    *gap_side_out = gap_side;
}

static void
gtk_popover_get_rect_coords (GtkPopover *popover,
                             gint       *x1_out,
                             gint       *y1_out,
                             gint       *x2_out,
                             gint       *y2_out)
{
  GtkPopoverPrivate *priv = popover->priv;
  gint x1, x2, y1, y2;
  GtkAllocation allocation;

  x1 = y1 = x2 = y2 = 0;
  gtk_widget_get_allocation (GTK_WIDGET (popover), &allocation);

  if (priv->final_position == GTK_POS_TOP)
    {
      x1 = 0;
      y1 = 0;
      x2 = allocation.width;
      y2 = allocation.height - TAIL_HEIGHT;
    }
  else if (priv->final_position == GTK_POS_BOTTOM)
    {
      x1 = 0;
      y1 = TAIL_HEIGHT;
      x2 = allocation.width;
      y2 = allocation.height;
    }
  else if (priv->final_position == GTK_POS_LEFT)
    {
      x1 = 0;
      y1 = 0;
      x2 = allocation.width - TAIL_HEIGHT;
      y2 = allocation.height;
    }
  else if (priv->final_position == GTK_POS_RIGHT)
    {
      x1 = TAIL_HEIGHT;
      y1 = 0;
      x2 = allocation.width;
      y2 = allocation.height;
    }

  if (x1_out)
    *x1_out = x1;
  if (y1_out)
    *y1_out = y1;
  if (x2_out)
    *x2_out = x2;
  if (y2_out)
    *y2_out = y2;
}

static void
gtk_popover_apply_tail_path (GtkPopover *popover,
                             cairo_t    *cr)
{
  gint initial_x, initial_y;
  gint tip_x, tip_y;
  gint final_x, final_y;

  gtk_popover_get_gap_coords (popover,
                              &initial_x, &initial_y,
                              &tip_x, &tip_y,
                              &final_x, &final_y,
                              NULL);

  cairo_move_to (cr, initial_x, initial_y);
  cairo_line_to (cr, tip_x, tip_y);
  cairo_line_to (cr, final_x, final_y);
}

static void
gtk_popover_apply_border_path (GtkPopover *popover,
                               cairo_t    *cr)
{
  GtkPopoverPrivate *priv;
  GtkAllocation allocation;
  gint x1, y1, x2, y2;

  priv = popover->priv;
  gtk_widget_get_allocation (GTK_WIDGET (popover), &allocation);

  gtk_popover_apply_tail_path (popover, cr);
  gtk_popover_get_rect_coords (popover, &x1, &y1, &x2, &y2);

  if (priv->final_position == GTK_POS_TOP)
    {
      cairo_line_to (cr, x2, y2);
      cairo_line_to (cr, x2, y1);
      cairo_line_to (cr, x1, y1);
      cairo_line_to (cr, x1, y2);
    }
  else if (priv->final_position == GTK_POS_BOTTOM)
    {
      cairo_line_to (cr, x2, y1);
      cairo_line_to (cr, x2, y2);
      cairo_line_to (cr, x1, y2);
      cairo_line_to (cr, x1, y1);
    }
  else if (priv->final_position == GTK_POS_LEFT)
    {
      cairo_line_to (cr, x2, y2);
      cairo_line_to (cr, x1, y2);
      cairo_line_to (cr, x1, y1);
      cairo_line_to (cr, x2, y1);
    }
  else if (priv->final_position == GTK_POS_RIGHT)
    {
      cairo_line_to (cr, x1, y1);
      cairo_line_to (cr, x2, y1);
      cairo_line_to (cr, x2, y2);
      cairo_line_to (cr, x1, y2);
    }

  cairo_close_path (cr);
}

static void
gtk_popover_update_shape (GtkPopover *popover)
{
  cairo_surface_t *surface;
  cairo_region_t *region;
  GdkWindow *win;
  cairo_t *cr;

  win = gtk_widget_get_window (GTK_WIDGET (popover));
  surface =
    gdk_window_create_similar_surface (win,
                                       CAIRO_CONTENT_COLOR_ALPHA,
                                       gdk_window_get_width (win),
                                       gdk_window_get_height (win));

  cr = cairo_create (surface);
  gtk_popover_apply_border_path (popover, cr);
  cairo_fill (cr);
  cairo_destroy (cr);

  region = gdk_cairo_region_create_from_surface (surface);
  cairo_surface_destroy (surface);

  gtk_widget_shape_combine_region (GTK_WIDGET (popover), region);
  cairo_region_destroy (region);
}

static void
gtk_popover_update_position (GtkPopover *popover)
{
  GtkAllocation allocation, window_alloc;
  GtkPopoverPrivate *priv;
  cairo_rectangle_int_t rect;
  gint win_x, win_y, x, y;

  priv = popover->priv;
  gtk_widget_get_allocation (GTK_WIDGET (popover), &allocation);
  gtk_widget_get_allocation (GTK_WIDGET (priv->window), &window_alloc);
  priv->final_position = priv->preferred_position;
  gtk_popover_get_pointing_to (popover, &rect);

  gtk_popover_get_pointed_to_coords (popover, &x, &y, &rect);

  /* Check whether there's enough room on the
   * preferred side, move to the opposite one if not.
   */
  if (priv->preferred_position == GTK_POS_TOP && rect.y < allocation.height)
    priv->final_position = GTK_POS_BOTTOM;
  else if (priv->preferred_position == GTK_POS_BOTTOM &&
           rect.y > window_alloc.height - allocation.height)
    priv->final_position = GTK_POS_TOP;
  else if (priv->preferred_position == GTK_POS_LEFT && rect.x < allocation.width)
    priv->final_position = GTK_POS_RIGHT;
  else if (priv->preferred_position == GTK_POS_RIGHT &&
           rect.x > window_alloc.width - allocation.width)
    priv->final_position = GTK_POS_LEFT;

  if (POS_IS_VERTICAL (priv->final_position))
    {
      win_x = CLAMP (x - allocation.width / 2,
                     0, window_alloc.width - allocation.width);
      win_y = y;

      if (priv->final_position == GTK_POS_TOP)
        win_y -= allocation.height;
      else if (priv->final_position == GTK_POS_BOTTOM)
        win_y += rect.height;
    }
  else
    {
      win_y = CLAMP (y - allocation.height / 2,
		     0, window_alloc.height - allocation.height);
      win_x = x;

      if (priv->final_position == GTK_POS_LEFT)
        win_x -= allocation.width;
      else if (priv->final_position == GTK_POS_RIGHT)
        win_x += rect.width;
    }

  priv->win_x = win_x;
  priv->win_y = win_y;

  gtk_window_set_popover_position (priv->window,
                                   GTK_WIDGET (popover),
                                   priv->final_position,
                                   win_x, win_y);

  gtk_popover_update_shape (popover);
}

static gboolean
gtk_popover_draw (GtkWidget *widget,
                  cairo_t   *cr)
{
  GtkStyleContext *context;
  GtkAllocation allocation;
  GtkWidget *child;
  GtkBorder border;
  GdkRGBA border_color;
  gint rect_x1, rect_x2, rect_y1, rect_y2;
  gint initial_x, initial_y, final_x, final_y;
  gint gap_start, gap_end;
  GtkPositionType gap_side;
  GtkStateFlags state;

  context = gtk_widget_get_style_context (widget);
  state = gtk_widget_get_state_flags (widget);
  gtk_widget_get_allocation (widget, &allocation);

  gtk_popover_get_rect_coords (GTK_POPOVER (widget),
                               &rect_x1, &rect_y1,
                               &rect_x2, &rect_y2);

  /* Render the rect background */
  gtk_render_background (context, cr,
                         rect_x1, rect_y1,
                         rect_x2 - rect_x1, rect_y2 - rect_y1);

  gtk_popover_get_gap_coords (GTK_POPOVER (widget),
                              &initial_x, &initial_y,
                              NULL, NULL,
                              &final_x, &final_y,
                              &gap_side);

  if (POS_IS_VERTICAL (gap_side))
    {
      gap_start = initial_x;
      gap_end = final_x;
    }
  else
    {
      gap_start = initial_y;
      gap_end = final_y;
    }

  /* Now render the frame, without the gap for the arrow tip */
  gtk_render_frame_gap (context, cr,
                        rect_x1, rect_y1,
                        rect_x2 - rect_x1, rect_y2 - rect_y1,
                        gap_side,
                        gap_start, gap_end);

  /* Clip to the arrow shape */
  cairo_save (cr);

  gtk_popover_apply_tail_path (GTK_POPOVER (widget), cr);
  cairo_clip (cr);

  /* Render the arrow background */
  gtk_render_background (context, cr,
                         0, 0,
                         allocation.width, allocation.height);

  /* Render the border of the arrow tip */
  gtk_style_context_get_border (context, state, &border);

  if (border.bottom > 0)
    {
      gtk_style_context_get_border_color (context, state, &border_color);
      gtk_popover_apply_tail_path (GTK_POPOVER (widget), cr);
      gdk_cairo_set_source_rgba (cr, &border_color);

      cairo_set_line_width (cr, border.bottom);
      cairo_stroke (cr);
    }

  /* We're done */
  cairo_restore (cr);

  child = gtk_bin_get_child (GTK_BIN (widget));

  if (child)
    gtk_container_propagate_draw (GTK_CONTAINER (widget), child, cr);

  return TRUE;
}

static void
get_padding_and_border (GtkWidget *widget,
                        GtkBorder *border)
{
  GtkStyleContext *context;
  GtkStateFlags state;
  gint border_width;
  GtkBorder tmp;

  context = gtk_widget_get_style_context (widget);
  state = gtk_widget_get_state_flags (widget);

  border_width = gtk_container_get_border_width (GTK_CONTAINER (widget));

  gtk_style_context_get_padding (context, state, border);
  gtk_style_context_get_border (context, state, &tmp);
  border->top += tmp.top + border_width;
  border->right += tmp.right + border_width;
  border->bottom += tmp.bottom + border_width;
  border->left += tmp.left + border_width;
}

static void
gtk_popover_get_preferred_width (GtkWidget *widget,
                                 gint      *minimum_width,
                                 gint      *natural_width)
{
  GtkPopoverPrivate *priv;
  GtkWidget *child;
  gint min, nat;
  GtkBorder border;

  priv = GTK_POPOVER (widget)->priv;
  child = gtk_bin_get_child (GTK_BIN (widget));
  min = nat = 0;

  if (child)
    gtk_widget_get_preferred_width (child, &min, &nat);

  get_padding_and_border (widget, &border);
  min += border.left + border.right;
  nat += border.left + border.right;

  if (!POS_IS_VERTICAL (priv->preferred_position))
    {
      min += TAIL_HEIGHT;
      nat += TAIL_HEIGHT;
    }

  if (minimum_width)
    *minimum_width = MAX (min, TAIL_GAP_WIDTH);

  if (natural_width)
    *natural_width = MAX (nat, TAIL_GAP_WIDTH);
}

static void
gtk_popover_get_preferred_height (GtkWidget *widget,
                                  gint      *minimum_height,
                                  gint      *natural_height)
{
  GtkPopoverPrivate *priv;
  GtkWidget *child;
  gint min, nat;
  GtkBorder border;

  priv = GTK_POPOVER (widget)->priv;
  child = gtk_bin_get_child (GTK_BIN (widget));
  min = nat = 0;

  if (child)
    gtk_widget_get_preferred_height (child, &min, &nat);

  get_padding_and_border (widget, &border);
  min += border.top + border.bottom;
  nat += border.top + border.bottom;

  if (POS_IS_VERTICAL (priv->preferred_position))
    {
      min += TAIL_HEIGHT;
      nat += TAIL_HEIGHT;
    }

  if (minimum_height)
    *minimum_height = MAX (min, TAIL_GAP_WIDTH);

  if (natural_height)
    *natural_height = MAX (nat, TAIL_GAP_WIDTH);
}

static void
gtk_popover_size_allocate (GtkWidget     *widget,
                           GtkAllocation *allocation)
{
  GtkPopoverPrivate *priv;
  GtkWidget *child;

  priv = GTK_POPOVER (widget)->priv;
  gtk_widget_set_allocation (widget, allocation);
  child = gtk_bin_get_child (GTK_BIN (widget));

  if (gtk_widget_get_visible (widget))
    gtk_popover_update_position (GTK_POPOVER (widget));

  if (child)
    {
      GtkAllocation child_alloc;
      GtkBorder border;

      get_padding_and_border (widget, &border);

      child_alloc.x = allocation->x + border.left;
      child_alloc.y = allocation->y + border.top;
      child_alloc.width = allocation->width - border.left - border.right;
      child_alloc.height = allocation->height - border.top - border.bottom;

      if (POS_IS_VERTICAL (priv->final_position))
        child_alloc.height -= TAIL_HEIGHT;
      else
        child_alloc.width -= TAIL_HEIGHT;

      if (priv->final_position == GTK_POS_BOTTOM)
        child_alloc.y += TAIL_HEIGHT;
      else if (priv->final_position == GTK_POS_RIGHT)
        child_alloc.x += TAIL_HEIGHT;

      gtk_widget_size_allocate (child, &child_alloc);
    }

  if (gtk_widget_get_realized (widget))
    {
      gdk_window_move_resize (gtk_widget_get_window (widget),
                              0, 0, allocation->width, allocation->height);
      gtk_popover_update_shape (GTK_POPOVER (widget));
    }
}

static gboolean
gtk_popover_button_press (GtkWidget      *widget,
                          GdkEventButton *event)
{
  GtkWidget *child;

  child = gtk_bin_get_child (GTK_BIN (widget));

  if (child && event->window == gtk_widget_get_window (widget))
    {
      GtkAllocation child_alloc;

      gtk_widget_get_allocation (child, &child_alloc);

      if (event->x < child_alloc.x ||
          event->x > child_alloc.x + child_alloc.width ||
          event->y < child_alloc.y ||
          event->y > child_alloc.y + child_alloc.height)
        gtk_widget_hide (widget);
    }
  else
    gtk_widget_hide (widget);

  return GDK_EVENT_PROPAGATE;
}

static gboolean
gtk_popover_key_press (GtkWidget   *widget,
                       GdkEventKey *event)
{
  if (event->keyval == GDK_KEY_Escape)
    {
      gtk_widget_hide (widget);
      return GDK_EVENT_STOP;
    }

  return GDK_EVENT_PROPAGATE;
}

static void
gtk_popover_class_init (GtkPopoverClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = gtk_popover_set_property;
  object_class->get_property = gtk_popover_get_property;
  object_class->finalize = gtk_popover_finalize;
  object_class->dispose = gtk_popover_dispose;

  widget_class->realize = gtk_popover_realize;
  widget_class->map = gtk_popover_map;
  widget_class->unmap = gtk_popover_unmap;
  widget_class->get_preferred_width = gtk_popover_get_preferred_width;
  widget_class->get_preferred_height = gtk_popover_get_preferred_height;
  widget_class->size_allocate = gtk_popover_size_allocate;
  widget_class->draw = gtk_popover_draw;
  widget_class->button_press_event = gtk_popover_button_press;
  widget_class->key_press_event = gtk_popover_key_press;

  /**
   * GtkPopover:relative-to:
   *
   * Sets the attached widget.
   *
   * Since: 3.12
   */
  g_object_class_install_property (object_class,
                                   PROP_RELATIVE_TO,
                                   g_param_spec_object ("relative-to",
                                                        P_("Relative to"),
                                                        P_("Widget the bubble window points to"),
                                                        GTK_TYPE_WIDGET,
                                                        GTK_PARAM_READWRITE));
  /**
   * GtkPopover:pointing-to:
   *
   * Marks a specific rectangle to be pointed.
   *
   * Since: 3.12
   */
  g_object_class_install_property (object_class,
                                   PROP_POINTING_TO,
                                   g_param_spec_boxed ("pointing-to",
                                                       P_("Pointing to"),
                                                       P_("Rectangle the bubble window points to"),
                                                       CAIRO_GOBJECT_TYPE_RECTANGLE_INT,
                                                       GTK_PARAM_READWRITE));
  /**
   * GtkPopover:position
   *
   * Sets the preferred position of the popover.
   *
   * Since: 3.12
   */
  g_object_class_install_property (object_class,
                                   PROP_POSITION,
                                   g_param_spec_enum ("position",
                                                      P_("Position"),
                                                      P_("Position to place the bubble window"),
                                                      GTK_TYPE_POSITION_TYPE, GTK_POS_TOP,
                                                      GTK_PARAM_READWRITE | G_PARAM_CONSTRUCT));
}

static void
_gtk_popover_parent_hierarchy_changed (GtkWidget  *widget,
                                       GtkWidget  *previous_toplevel,
                                       GtkPopover *popover)
{
  GtkPopoverPrivate *priv;
  GtkWindow *new_window;

  priv = popover->priv;
  new_window = GTK_WINDOW (gtk_widget_get_ancestor (widget, GTK_TYPE_WINDOW));

  if (priv->window == new_window)
    return;

  if (priv->window)
    gtk_window_remove_popover (priv->window, GTK_WIDGET (popover));

  if (new_window)
    gtk_window_add_popover (new_window, GTK_WIDGET (popover));

  priv->window = new_window;

  if (gtk_widget_is_visible (GTK_WIDGET (popover)))
    gtk_popover_update_position (popover);
}

static void
_gtk_popover_parent_unmap (GtkWidget *widget,
                           GtkPopover *popover)
{
  gtk_widget_unmap (GTK_WIDGET (popover));
}

static void
_gtk_popover_parent_size_allocate (GtkWidget     *widget,
                                   GtkAllocation *allocation,
                                   GtkPopover    *popover)
{
  if (gtk_widget_is_visible (GTK_WIDGET (popover)))
    gtk_popover_update_position (popover);
}

static void
gtk_popover_update_relative_to (GtkPopover *popover,
                                GtkWidget  *relative_to)
{
  GtkPopoverPrivate *priv;

  priv = popover->priv;

  if (priv->widget == relative_to)
    return;

  if (priv->window)
    {
      gtk_window_remove_popover (priv->window, GTK_WIDGET (popover));
      priv->window = NULL;
    }

  if (priv->widget)
    {
      g_signal_handler_disconnect (priv->widget, priv->hierarchy_changed_id);
      g_signal_handler_disconnect (priv->widget, priv->size_allocate_id);
      g_signal_handler_disconnect (priv->widget, priv->unmap_id);
    }

  priv->widget = relative_to;
  g_object_notify (G_OBJECT (popover), "relative-to");

  if (priv->widget)
    {
      priv->window =
        GTK_WINDOW (gtk_widget_get_ancestor (priv->widget, GTK_TYPE_WINDOW));

      priv->hierarchy_changed_id =
        g_signal_connect (priv->widget, "hierarchy-changed",
                          G_CALLBACK (_gtk_popover_parent_hierarchy_changed),
                          popover);
      priv->size_allocate_id =
        g_signal_connect (priv->widget, "size-allocate",
                          G_CALLBACK (_gtk_popover_parent_size_allocate),
                          popover);
      priv->unmap_id =
        g_signal_connect (priv->widget, "unmap",
                          G_CALLBACK (_gtk_popover_parent_unmap),
                          popover);
    }

  if (priv->window)
    gtk_window_add_popover (priv->window, GTK_WIDGET (popover));
}

static void
gtk_popover_update_pointing_to (GtkPopover            *popover,
                                cairo_rectangle_int_t *pointing_to)
{
  GtkPopoverPrivate *priv;

  priv = popover->priv;

  if (pointing_to)
    {
      priv->pointing_to = *pointing_to;
      priv->has_pointing_to = TRUE;
    }
  else
    priv->has_pointing_to = FALSE;

  g_object_notify (G_OBJECT (popover), "pointing-to");
}

static void
gtk_popover_update_preferred_position (GtkPopover      *popover,
                                       GtkPositionType  position)
{
  GtkPopoverPrivate *priv;

  priv = popover->priv;
  priv->preferred_position = position;
  g_object_notify (G_OBJECT (popover), "position");
}

/**
 * gtk_popover_new:
 * @relative_to: #GtkWidget the popover is related to
 *
 * Creates a new popover to point to @relative_to
 *
 * Returns: a new #GtkPopover
 *
 * Since: 3.12
 **/
GtkWidget *
gtk_popover_new (GtkWidget *relative_to)
{
  return g_object_new (GTK_TYPE_POPOVER,
                       "relative-to", relative_to,
                       NULL);
}

/**
 * gtk_popover_set_relative_to:
 * @popover: a #GtkPopover
 * @relative_to: a #GtkWidget
 *
 * Sets a new widget to be attached to @popover. If @popover is
 * visible, the position will be updated.
 *
 * Since: 3.12
 **/
void
gtk_popover_set_relative_to (GtkPopover *popover,
                             GtkWidget  *relative_to)
{
  g_return_if_fail (GTK_IS_POPOVER (popover));
  g_return_if_fail (GTK_IS_WIDGET (relative_to));

  gtk_popover_update_relative_to (popover, relative_to);

  if (gtk_widget_get_visible (GTK_WIDGET (popover)))
    gtk_popover_update_position (popover);
}

/**
 * gtk_popover_get_relative_to:
 * @popover: a #GtkPopover
 *
 * Returns the wigdet @popover is currently attached to
 *
 * Returns: a #GtkWidget
 *
 * Since: 3.12
 **/
GtkWidget *
gtk_popover_get_relative_to (GtkPopover *popover)
{
  GtkPopoverPrivate *priv;

  g_return_val_if_fail (GTK_IS_POPOVER (popover), NULL);

  priv = popover->priv;

  return priv->widget;
}

/**
 * gtk_popover_set_pointing_to:
 * @popover: a #GtkPopover
 * @rect: rectangle to point to
 *
 * Sets the rectangle that @popover will point to, in the coordinate
 * space of the widget @popover is attached to, see
 * gtk_popover_set_relative_to()
 *
 * Since: 3.12
 **/
void
gtk_popover_set_pointing_to (GtkPopover            *popover,
                             cairo_rectangle_int_t *rect)
{
  g_return_if_fail (GTK_IS_POPOVER (popover));
  g_return_if_fail (rect != NULL);

  gtk_popover_update_pointing_to (popover, rect);

  if (gtk_widget_get_visible (GTK_WIDGET (popover)))
    gtk_popover_update_position (popover);
}

/**
 * gtk_popover_get_pointing_to:
 * @popover: a #GtkPopover
 * @rect: (out): location to store the rectangle
 *
 * If a rectangle to point to has been set, this function will
 * return %TRUE and fill in @rect with such rectangle, otherwise
 * it will return %FALSE and fill in @rect with the attached
 * widget coordinates.
 *
 * Returns: %TRUE if a rectangle to point to was set.
 **/
gboolean
gtk_popover_get_pointing_to (GtkPopover            *popover,
                             cairo_rectangle_int_t *rect)
{
  GtkPopoverPrivate *priv;

  g_return_val_if_fail (GTK_IS_POPOVER (popover), FALSE);

  priv = popover->priv;

  if (rect)
    {
      if (priv->has_pointing_to)
        *rect = priv->pointing_to;
      else if (priv->widget)
        {
          gtk_widget_get_allocation (priv->widget, rect);
          rect->x = rect->y = 0;
        }
    }

  return priv->has_pointing_to;
}

/**
 * gtk_popover_set_position:
 * @popover: a #GtkPopover
 * @position: preferred popover position
 *
 * Sets the preferred position for @popover to appear. If the @popover
 * is currently visible, it will be immediately updated.
 *
 * <note>
 *   This preference will be respected where possible, although
 *   on lack of space (eg. if close to the window edges), the
 *   #GtkPopover may choose to appear on the opposite side
 * </note>
 *
 * Since: 3.12
 **/
void
gtk_popover_set_position (GtkPopover      *popover,
                          GtkPositionType  position)
{
  g_return_if_fail (GTK_IS_POPOVER (popover));
  g_return_if_fail (position >= GTK_POS_LEFT && position <= GTK_POS_BOTTOM);

  gtk_popover_update_preferred_position (popover, position);

  if (gtk_widget_get_visible (GTK_WIDGET (popover)))
    gtk_popover_update_position (popover);
}

/**
 * gtk_popover_get_position:
 * @popover: a #GtkPopover
 *
 * Returns the preferred position of @popover.
 *
 * Returns: The preferred position.
 **/
GtkPositionType
gtk_popover_get_position (GtkPopover *popover)
{
  GtkPopoverPrivate *priv;

  g_return_val_if_fail (GTK_IS_POPOVER (popover), GTK_POS_TOP);

  priv = popover->priv;

  return priv->preferred_position;
}
