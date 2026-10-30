#include "stampy-window-private.h"
#include "stampy-preset-handlers.h"
#include "config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static gint compare_strings_naturally(gconstpointer a, gconstpointer b) {
    const char *str_a = *(const char **)a;
    const char *str_b = *(const char **)b;
    return stampy_natural_strcasecmp(str_a, str_b);
}

/* Sorts files naturally by path with zero heap allocations. */
static gint compare_file_pu_then_basename(gconstpointer a, gconstpointer b) {
    return stampy_compare_files_naturally(a, b);
}

/* Updates the preview when the listbox selection changes. */
static void on_listbox_selection_changed(GtkListBox *box, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    GList *selected_rows;
    guint count;

    if (!self || !box) return;

    selected_rows = gtk_list_box_get_selected_rows(box);
    count = g_list_length(selected_rows);

    if (count == 1) {
        GtkListBoxRow *row = GTK_LIST_BOX_ROW(selected_rows->data);
        int index = gtk_list_box_row_get_index(row);

        if (self->preview_stack) {
            if (gtk_stack_get_child_by_name(GTK_STACK(self->preview_stack), "single")) {
                gtk_stack_set_visible_child_name(GTK_STACK(self->preview_stack), "single");
            }
        }

        if (index >= 0 && self->image_files && (guint)index < self->image_files->len) {
            stampy_render_live_preview(self);
        }
    } else if (count > 1) {
        if (self->preview_stack) {
            if (gtk_stack_get_child_by_name(GTK_STACK(self->preview_stack), "multi")) {
                gtk_stack_set_visible_child_name(GTK_STACK(self->preview_stack), "multi");
            }
        }
    }

    g_list_free(selected_rows);
}

/* Plain left-click resets the selection to the clicked row. */
static void on_listbox_left_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    GtkListBoxRow *row;
    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));

    (void)n_press; (void)x;
    if (!self || !self->file_listbox) return;

    row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(self->file_listbox), (int)y);
    if (!row) return;

    /* Without Shift/Ctrl held, a click selects only this row. */
    if (!(state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK))) {
        GList *selected = gtk_list_box_get_selected_rows(GTK_LIST_BOX(self->file_listbox));
        if (g_list_length(selected) != 1 || selected->data != row) {
            gtk_list_box_unselect_all(GTK_LIST_BOX(self->file_listbox));
            gtk_list_box_select_row(GTK_LIST_BOX(self->file_listbox), row);
        }
        g_list_free(selected);
    }
}

static void on_panel_setting_changed(GtkWidget *widget, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)widget;
    if (!self || !STAMPY_IS_WINDOW(self)) return;

    stampy_render_live_preview(self);
    stampy_schedule_save_last_state(self);
}

static void on_dropdown_setting_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)gobject; (void)pspec;
    if (!self || !STAMPY_IS_WINDOW(self)) return;
    on_panel_setting_changed(NULL, self);
}

static gboolean in_pu_dropdown_update = FALSE;
static void stampy_rebuild_file_listbox(StampyWindow *self);

static void on_pu_folder_selected(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    guint selected;
    const char *pu_name;

    (void)gobject;
    (void)pspec;
    if (!self || in_pu_dropdown_update || !self->pu_folders || !self->drop_pu_folder) return;

    selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->drop_pu_folder));
    if (selected == GTK_INVALID_LIST_POSITION || selected >= self->pu_folders->len) return;

    pu_name = g_ptr_array_index(self->pu_folders, selected);
    if (!pu_name || g_strcmp0(self->selected_pu_folder, pu_name) == 0) return;

    g_free(self->selected_pu_folder);
    self->selected_pu_folder = g_strdup(pu_name);

    stampy_rebuild_file_listbox(self);
}

typedef struct {
    StampyWindow *self;
    GtkWidget *button;
    GtkWidget *popover;
    guint show_timer_id;
    guint hide_timer_id;
} HoverPopoverData;

static gboolean on_hover_show_timeout(gpointer user_data) {
    HoverPopoverData *data = (HoverPopoverData *)user_data;
    data->show_timer_id = 0;
    if (data->popover) {
        if (data->self && data->self->active_hover_popover &&
            data->self->active_hover_popover != data->popover) {
            gtk_popover_popdown(GTK_POPOVER(data->self->active_hover_popover));
        }
        if (!gtk_widget_get_visible(data->popover)) {
            gtk_popover_popup(GTK_POPOVER(data->popover));
            if (data->self) data->self->active_hover_popover = data->popover;
        }
    }
    return G_SOURCE_REMOVE;
}

static gboolean on_hover_hide_timeout(gpointer user_data) {
    HoverPopoverData *data = (HoverPopoverData *)user_data;
    data->hide_timer_id = 0;
    if (data->popover && gtk_widget_get_visible(data->popover)) {
        GtkRoot *root = gtk_widget_get_root(data->popover);
        if (root) {
            GtkWidget *focus = gtk_root_get_focus(root);
            if (focus && gtk_widget_is_ancestor(focus, data->popover)) {
                return G_SOURCE_REMOVE;
            }
        }
        gtk_popover_popdown(GTK_POPOVER(data->popover));
        if (data->self && data->self->active_hover_popover == data->popover) {
            data->self->active_hover_popover = NULL;
        }
    }
    return G_SOURCE_REMOVE;
}

static void on_widget_motion_enter(GtkEventControllerMotion *ctrl, double x, double y, gpointer user_data) {
    HoverPopoverData *data = (HoverPopoverData *)user_data;
    (void)ctrl; (void)x; (void)y;
    if (data->hide_timer_id > 0) {
        g_source_remove(data->hide_timer_id);
        data->hide_timer_id = 0;
    }
    if (data->popover && !gtk_widget_get_visible(data->popover)) {
        if (data->show_timer_id == 0) {
            data->show_timer_id = g_timeout_add(500, on_hover_show_timeout, data);
        }
    }
}

static void on_widget_motion_leave(GtkEventControllerMotion *ctrl, gpointer user_data) {
    HoverPopoverData *data = (HoverPopoverData *)user_data;
    (void)ctrl;
    if (data->show_timer_id > 0) {
        g_source_remove(data->show_timer_id);
        data->show_timer_id = 0;
    }
    if (data->popover && gtk_widget_get_visible(data->popover)) {
        if (data->hide_timer_id == 0) {
            data->hide_timer_id = g_timeout_add(500, on_hover_hide_timeout, data);
        }
    }
}

static void hover_popover_data_free(gpointer user_data) {
    HoverPopoverData *data = (HoverPopoverData *)user_data;
    if (data) {
        if (data->show_timer_id > 0) {
            g_source_remove(data->show_timer_id);
            data->show_timer_id = 0;
        }
        if (data->hide_timer_id > 0) {
            g_source_remove(data->hide_timer_id);
            data->hide_timer_id = 0;
        }
        if (data->popover) {
            if (data->self && data->self->active_hover_popover == data->popover) {
                data->self->active_hover_popover = NULL;
            }
            gtk_widget_unparent(data->popover);
            data->popover = NULL;
        }
        g_free(data);
    }
}

static void on_button_click_immediate(HoverPopoverData *data) {
    if (data->show_timer_id > 0) {
        g_source_remove(data->show_timer_id);
        data->show_timer_id = 0;
    }
    if (data->hide_timer_id > 0) {
        g_source_remove(data->hide_timer_id);
        data->hide_timer_id = 0;
    }
    if (data->popover) {
        if (data->self && data->self->active_hover_popover &&
            data->self->active_hover_popover != data->popover) {
            gtk_popover_popdown(GTK_POPOVER(data->self->active_hover_popover));
        }
        if (!gtk_widget_get_visible(data->popover)) {
            gtk_popover_popup(GTK_POPOVER(data->popover));
            if (data->self) data->self->active_hover_popover = data->popover;
        } else {
            gtk_popover_popdown(GTK_POPOVER(data->popover));
            if (data->self && data->self->active_hover_popover == data->popover) {
                data->self->active_hover_popover = NULL;
            }
        }
    }
}

static void on_button_right_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    HoverPopoverData *data = (HoverPopoverData *)user_data;
    (void)gesture; (void)n_press; (void)x; (void)y;
    on_button_click_immediate(data);
}

static void on_button_left_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    HoverPopoverData *data = (HoverPopoverData *)user_data;
    (void)n_press; (void)x; (void)y;
    on_button_click_immediate(data);
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void on_popover_closed(GtkPopover *popover, gpointer user_data) {
    HoverPopoverData *data = (HoverPopoverData *)user_data;
    if (data->show_timer_id > 0) {
        g_source_remove(data->show_timer_id);
        data->show_timer_id = 0;
    }
    if (data->hide_timer_id > 0) {
        g_source_remove(data->hide_timer_id);
        data->hide_timer_id = 0;
    }
    if (data->self && data->self->active_hover_popover == GTK_WIDGET(popover)) {
        data->self->active_hover_popover = NULL;
    }
}

static void attach_hover_popover(StampyWindow *self, GtkWidget *button, GtkWidget *popover, GtkWidget *content_box, gboolean intercept_primary_click) {
    HoverPopoverData *data = g_new0(HoverPopoverData, 1);
    GtkEventController *motion_btn;
    GtkEventController *motion_pop;
    GtkGesture *click_sec;

    data->self = self;
    data->button = button;
    data->popover = popover;

    gtk_popover_set_autohide(GTK_POPOVER(popover), FALSE);
    g_signal_connect(popover, "closed", G_CALLBACK(on_popover_closed), data);

    motion_btn = gtk_event_controller_motion_new();
    g_signal_connect(motion_btn, "enter", G_CALLBACK(on_widget_motion_enter), data);
    g_signal_connect(motion_btn, "leave", G_CALLBACK(on_widget_motion_leave), data);
    gtk_widget_add_controller(button, motion_btn);

    motion_pop = gtk_event_controller_motion_new();
    g_signal_connect(motion_pop, "enter", G_CALLBACK(on_widget_motion_enter), data);
    g_signal_connect(motion_pop, "leave", G_CALLBACK(on_widget_motion_leave), data);
    gtk_widget_add_controller(content_box, motion_pop);

    click_sec = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_sec), GDK_BUTTON_SECONDARY);
    g_signal_connect(click_sec, "pressed", G_CALLBACK(on_button_right_click), data);
    gtk_widget_add_controller(button, GTK_EVENT_CONTROLLER(click_sec));

    if (intercept_primary_click) {
        GtkGesture *click_pri = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_pri), GDK_BUTTON_PRIMARY);
        gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click_pri), GTK_PHASE_CAPTURE);
        g_signal_connect(click_pri, "pressed", G_CALLBACK(on_button_left_click), data);
        gtk_widget_add_controller(button, GTK_EVENT_CONTROLLER(click_pri));
    }

    g_object_set_data_full(G_OBJECT(button), "hover-popover-data", data, hover_popover_data_free);
}

typedef struct {
    StampyWindow *self;
    GtkWidget *target_color_btn;
    GdkRGBA rgba;
} SwatchClickData;

static void on_swatch_clicked(GtkButton *btn, gpointer user_data) {
    SwatchClickData *sd = (SwatchClickData *)user_data;
    (void)btn;
    if (sd && sd->target_color_btn) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(sd->target_color_btn), &sd->rgba);
#pragma GCC diagnostic pop
        if (sd->self) on_panel_setting_changed(NULL, sd->self);
    }
}

static void draw_color_swatch_circle(GtkDrawingArea *drawing_area, cairo_t *cr, int width, int height, gpointer user_data) {
    GdkRGBA *color = (GdkRGBA *)user_data;
    double radius;
    (void)drawing_area;

    radius = (MIN(width, height) / 2.0) - 2.0;
    if (radius < 2.0) radius = 2.0;

    cairo_arc(cr, width / 2.0, height / 2.0, radius, 0, 2 * G_PI);
    cairo_set_source_rgba(cr, color->red, color->green, color->blue, color->alpha);
    cairo_fill_preserve(cr);

    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.35);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);
}

static GtkWidget *create_swatch_button(StampyWindow *self, GtkWidget *target_color_btn, const GdkRGBA *rgba, const char *tooltip) {
    GtkWidget *btn = gtk_button_new();
    GtkWidget *area = gtk_drawing_area_new();
    GdkRGBA *color_copy = g_new(GdkRGBA, 1);
    SwatchClickData *sd = g_new0(SwatchClickData, 1);

    *color_copy = *rgba;
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(area), 20);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), 20);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_color_swatch_circle, color_copy, g_free);
    gtk_button_set_child(GTK_BUTTON(btn), area);
    gtk_widget_set_tooltip_text(btn, tooltip);
    gtk_widget_add_css_class(btn, "flat");

    sd->self = self;
    sd->target_color_btn = target_color_btn;
    sd->rgba = *rgba;

    g_signal_connect(btn, "clicked", G_CALLBACK(on_swatch_clicked), sd);
    g_object_set_data_full(G_OBJECT(btn), "swatch-click-data", sd, g_free);

    return btn;
}

static void on_text_color_chooser_rgba_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    GdkRGBA rgba;
    (void)pspec;
    if (!self || !self->btn_t_color) return;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(object), &rgba);
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(self->btn_t_color), &rgba);
#pragma GCC diagnostic pop
    on_panel_setting_changed(NULL, self);
}

static const struct {
    double r, g, b, a;
    const char *name;
} text_color_swatches[] = {
    {1.0, 1.0, 1.0, 1.0, "White"},
    {1.0, 1.0, 0.0, 1.0, "Yellow"},
    {0.0, 1.0, 0.0, 1.0, "Green"},
    {0.0, 1.0, 1.0, 1.0, "Cyan"},
    {1.0, 0.65, 0.0, 1.0, "Orange"},
    {1.0, 0.0, 0.0, 1.0, "Red"},
    {0.0, 0.0, 0.0, 1.0, "Black"},
};

static void create_text_color_popover(StampyWindow *self) {
    GtkWidget *popover;
    GtkWidget *content_box;
    GtkWidget *lbl_title;
    GtkWidget *row_swatches;
    GtkWidget *lbl_swatches;
    GtkWidget *swatches_box;
    GtkWidget *chooser;
    GdkRGBA curr_rgba;
    int k;

    if (!self || !self->btn_t_color) return;

    popover = gtk_popover_new();
    gtk_widget_set_parent(popover, self->btn_t_color);
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
    gtk_popover_set_autohide(GTK_POPOVER(popover), FALSE);

    content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(content_box, 10);
    gtk_widget_set_margin_end(content_box, 10);
    gtk_widget_set_margin_top(content_box, 10);
    gtk_widget_set_margin_bottom(content_box, 10);

    lbl_title = gtk_label_new("Text Color");
    gtk_widget_add_css_class(lbl_title, "heading");
    gtk_label_set_xalign(GTK_LABEL(lbl_title), 0.0);
    gtk_box_append(GTK_BOX(content_box), lbl_title);

    /* Quick Swatches Row */
    row_swatches = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    lbl_swatches = gtk_label_new("Quick");
    gtk_label_set_xalign(GTK_LABEL(lbl_swatches), 0.0);
    gtk_box_append(GTK_BOX(row_swatches), lbl_swatches);

    swatches_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    for (k = 0; k < (int)(sizeof(text_color_swatches) / sizeof(text_color_swatches[0])); k++) {
        GdkRGBA c = {text_color_swatches[k].r, text_color_swatches[k].g, text_color_swatches[k].b, text_color_swatches[k].a};
        GtkWidget *sw_btn = create_swatch_button(self, self->btn_t_color, &c, text_color_swatches[k].name);
        gtk_box_append(GTK_BOX(swatches_box), sw_btn);
    }
    gtk_box_append(GTK_BOX(row_swatches), swatches_box);
    gtk_box_append(GTK_BOX(content_box), row_swatches);

    /* Full in-popover Color Chooser Widget */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    chooser = gtk_color_chooser_widget_new();
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(chooser), FALSE);
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(self->btn_t_color), &curr_rgba);
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(chooser), &curr_rgba);
    g_signal_connect(chooser, "notify::rgba", G_CALLBACK(on_text_color_chooser_rgba_changed), self);
    gtk_box_append(GTK_BOX(content_box), chooser);
#pragma GCC diagnostic pop

    gtk_popover_set_child(GTK_POPOVER(popover), content_box);

    attach_hover_popover(self, self->btn_t_color, popover, content_box, TRUE);
}

static void create_effect_popover(StampyWindow *self,
                                  GtkWidget *anchor_button,
                                  const char *title,
                                  const char *slider_label,
                                  double min_val, double max_val, double step, double default_val,
                                  GtkWidget **inout_color_button,
                                  const GdkRGBA *default_color,
                                  GtkWidget **out_slider) {
    GtkWidget *popover;
    GtkWidget *content_box;
    GtkWidget *lbl_title;
    GtkWidget *row_color;
    GtkWidget *lbl_color;
    GtkWidget *row_slider;
    GtkWidget *lbl_slider;
    GtkWidget *scale;
    GtkWidget *spin;
    GtkAdjustment *adj;

    if (!anchor_button) return;

    popover = gtk_popover_new();
    gtk_widget_set_parent(popover, anchor_button);
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
    gtk_popover_set_autohide(GTK_POPOVER(popover), FALSE);

    content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(content_box, 10);
    gtk_widget_set_margin_end(content_box, 10);
    gtk_widget_set_margin_top(content_box, 10);
    gtk_widget_set_margin_bottom(content_box, 10);

    lbl_title = gtk_label_new(title);
    gtk_widget_add_css_class(lbl_title, "heading");
    gtk_label_set_xalign(GTK_LABEL(lbl_title), 0.0);
    gtk_box_append(GTK_BOX(content_box), lbl_title);

    /* Color picker row */
    row_color = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    lbl_color = gtk_label_new("Color");
    gtk_label_set_xalign(GTK_LABEL(lbl_color), 0.0);
    gtk_widget_set_hexpand(lbl_color, TRUE);
    gtk_box_append(GTK_BOX(row_color), lbl_color);

    if (inout_color_button && *inout_color_button) {
        GtkWidget *parent = gtk_widget_get_parent(*inout_color_button);
        if (parent) {
            g_object_ref(*inout_color_button);
            gtk_box_remove(GTK_BOX(parent), *inout_color_button);
            gtk_box_append(GTK_BOX(row_color), *inout_color_button);
            g_object_unref(*inout_color_button);
        } else {
            gtk_box_append(GTK_BOX(row_color), *inout_color_button);
        }
    } else {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        GtkWidget *new_color_btn = gtk_color_button_new();
        if (default_color) {
            gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(new_color_btn), default_color);
        }
        if (inout_color_button) *inout_color_button = new_color_btn;
        gtk_box_append(GTK_BOX(row_color), new_color_btn);
#pragma GCC diagnostic pop
    }
    gtk_box_append(GTK_BOX(content_box), row_color);

    /* Thickness / Offset slider and typeable spin row */
    row_slider = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    lbl_slider = gtk_label_new(slider_label);
    gtk_label_set_xalign(GTK_LABEL(lbl_slider), 0.0);

    adj = gtk_adjustment_new(default_val, min_val, max_val, step, step * 10.0, 0.0);
    scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, adj);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_widget_set_hexpand(scale, TRUE);
    gtk_widget_set_size_request(scale, 100, -1);

    spin = gtk_spin_button_new(adj, step, 1);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), TRUE);
    gtk_spin_button_set_update_policy(GTK_SPIN_BUTTON(spin), GTK_UPDATE_ALWAYS);
    gtk_widget_set_size_request(spin, 75, -1);

    gtk_box_append(GTK_BOX(row_slider), lbl_slider);
    gtk_box_append(GTK_BOX(row_slider), scale);
    gtk_box_append(GTK_BOX(row_slider), spin);
    gtk_box_append(GTK_BOX(content_box), row_slider);
    if (out_slider) *out_slider = scale;

    gtk_popover_set_child(GTK_POPOVER(popover), content_box);

    attach_hover_popover(self, anchor_button, popover, content_box, FALSE);
}

void stampy_setup_hover_effect_popovers(StampyWindow *self) {
    GdkRGBA black_shadow = {0.0, 0.0, 0.0, 0.7};
    GdkRGBA comp_shadow = {0.0, 0.0, 0.0, 0.6};

    if (!self) return;

    /* Main Text Color: in-popover palette swatches + full color chooser */
    create_text_color_popover(self);

    /* Text Stroke: color picker + thickness slider */
    create_effect_popover(self,
                          self->btn_t_stroke,
                          "Text Stroke", "Thickness",
                          0.1, 12.0, 0.1, 2.5,
                          &self->btn_t_stroke_color, NULL,
                          &self->val_t_stroke_width);

    /* Text Shadow: color picker + offset slider */
    create_effect_popover(self,
                          self->btn_t_shadow,
                          "Text Shadow", "Offset",
                          0.1, 12.0, 0.1, 2.0,
                          &self->btn_t_shadow_color, &black_shadow,
                          &self->val_t_shadow_width);

    /* Compass Stroke: color picker + thickness slider */
    create_effect_popover(self,
                          self->btn_c_stroke,
                          "Compass Stroke", "Thickness",
                          0.1, 12.0, 0.1, 2.0,
                          &self->btn_c_stroke_color, NULL,
                          &self->val_c_stroke_width);

    /* Compass Shadow: color picker + offset slider */
    create_effect_popover(self,
                          self->btn_c_shadow,
                          "Compass Shadow", "Offset",
                          0.1, 15.0, 0.1, 4.0,
                          &self->btn_c_shadow_color, &comp_shadow,
                          &self->val_c_shadow_width);
}

void stampy_connect_panel_signals(StampyWindow *self) {
    if (!self) return;

    if (self->drop_pu_folder) g_signal_connect(self->drop_pu_folder, "notify::selected", G_CALLBACK(on_pu_folder_selected), self);

    if (self->val_compass_size) g_signal_connect(self->val_compass_size, "value-changed", G_CALLBACK(on_panel_setting_changed), self);
    if (self->val_compass_margin_x) g_signal_connect(self->val_compass_margin_x, "value-changed", G_CALLBACK(on_panel_setting_changed), self);
    if (self->val_compass_margin_y) g_signal_connect(self->val_compass_margin_y, "value-changed", G_CALLBACK(on_panel_setting_changed), self);
    if (self->val_compass_opacity) g_signal_connect(self->val_compass_opacity, "value-changed", G_CALLBACK(on_panel_setting_changed), self);

    if (self->drop_compass_pos) g_signal_connect(self->drop_compass_pos, "notify::selected", G_CALLBACK(on_dropdown_setting_changed), self);
    if (self->drop_text_pos) g_signal_connect(self->drop_text_pos, "notify::selected", G_CALLBACK(on_dropdown_setting_changed), self);
    if (self->drop_text_font) g_signal_connect(self->drop_text_font, "notify::selected", G_CALLBACK(on_dropdown_setting_changed), self);

    if (self->btn_c_shadow) g_signal_connect(self->btn_c_shadow, "toggled", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_c_stroke) g_signal_connect(self->btn_c_stroke, "toggled", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_c_stroke_color) g_signal_connect(self->btn_c_stroke_color, "color-set", G_CALLBACK(on_panel_setting_changed), self);
    if (self->val_c_stroke_width) g_signal_connect(self->val_c_stroke_width, "value-changed", G_CALLBACK(on_panel_setting_changed), self);
    if (self->val_c_shadow_width) g_signal_connect(self->val_c_shadow_width, "value-changed", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_c_shadow_color) g_signal_connect(self->btn_c_shadow_color, "color-set", G_CALLBACK(on_panel_setting_changed), self);

    if (self->val_text_size) g_signal_connect(self->val_text_size, "value-changed", G_CALLBACK(on_panel_setting_changed), self);
    if (self->val_text_margin_x) g_signal_connect(self->val_text_margin_x, "value-changed", G_CALLBACK(on_panel_setting_changed), self);
    if (self->val_text_margin_y) g_signal_connect(self->val_text_margin_y, "value-changed", G_CALLBACK(on_panel_setting_changed), self);
    if (self->val_text_opacity) g_signal_connect(self->val_text_opacity, "value-changed", G_CALLBACK(on_panel_setting_changed), self);

    if (self->btn_t_align_left) g_signal_connect(self->btn_t_align_left, "toggled", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_t_align_center) g_signal_connect(self->btn_t_align_center, "toggled", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_t_align_right) g_signal_connect(self->btn_t_align_right, "toggled", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_t_bold) g_signal_connect(self->btn_t_bold, "toggled", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_t_italic) g_signal_connect(self->btn_t_italic, "toggled", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_t_underline) g_signal_connect(self->btn_t_underline, "toggled", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_t_shadow) g_signal_connect(self->btn_t_shadow, "toggled", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_t_stroke) g_signal_connect(self->btn_t_stroke, "toggled", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_t_stroke_color) g_signal_connect(self->btn_t_stroke_color, "color-set", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_t_color) g_signal_connect(self->btn_t_color, "color-set", G_CALLBACK(on_panel_setting_changed), self);
    if (self->val_t_stroke_width) g_signal_connect(self->val_t_stroke_width, "value-changed", G_CALLBACK(on_panel_setting_changed), self);
    if (self->val_t_shadow_width) g_signal_connect(self->val_t_shadow_width, "value-changed", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_t_shadow_color) g_signal_connect(self->btn_t_shadow_color, "color-set", G_CALLBACK(on_panel_setting_changed), self);
    if (self->val_text_spacing) g_signal_connect(self->val_text_spacing, "value-changed", G_CALLBACK(on_panel_setting_changed), self);
}

/* Rebuilds the file listbox, sorted naturally. */
static void append_file_row(StampyWindow *self, GFile *file) {
    char *rel_subpath = stampy_get_pu_rel_subpath_for_file(file);
    char *basename = g_file_get_basename(file);
    char *display_name = NULL;
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *label;

    /* If the file is in a subfolder under the PU folder, display "Subfolder/1.jpg" */
    if (rel_subpath && strchr(rel_subpath, '/') != NULL) {
        const char *after_pu = strchr(rel_subpath, '/') + 1;
        display_name = g_strdup_printf("%s/%s", after_pu, basename);
    } else {
        display_name = g_strdup(basename);
    }
    g_free(rel_subpath);

    label = gtk_label_new(display_name);
    g_free(display_name);

    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_set_margin_start(label, 12);
    gtk_widget_set_margin_end(label, 12);
    gtk_widget_set_margin_top(label, 8);
    gtk_widget_set_margin_bottom(label, 8);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
    g_object_set_data_full(G_OBJECT(row), "file", g_object_ref(file), g_object_unref);
    gtk_list_box_append(GTK_LIST_BOX(self->file_listbox), row);
    g_free(basename);
}

static void finish_list_population(StampyWindow *self) {
    GtkListBoxRow *first_row;

    gtk_widget_set_visible(GTK_WIDGET(self->file_listbox), TRUE);
    g_signal_handlers_unblock_matched(self->file_listbox, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, self);

    if (self->left_stack) {
        if (gtk_stack_get_child_by_name(GTK_STACK(self->left_stack), "page_list")) {
            gtk_stack_set_visible_child_name(GTK_STACK(self->left_stack), "page_list");
        }
    }

    first_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->file_listbox), 0);
    if (first_row) {
        gtk_list_box_select_row(GTK_LIST_BOX(self->file_listbox), first_row);
    }

    if (!self->export_active && self->scan_pulse_id == 0) {
        if (self->progress_bar) gtk_widget_set_visible(self->progress_bar, FALSE);
        gtk_window_set_title(GTK_WINDOW(self), "Stampy");
    }

    stampy_check_horizontal_photos(self);
    stampy_check_missing_data(self);
}

typedef struct {
    StampyWindow *self;
    GPtrArray *files;
    guint generation;
    guint index;
    guint total;
} PopulateTask;

static void populate_task_free(gpointer data) {
    PopulateTask *task = data;
    if (task) {
        if (task->files) g_ptr_array_unref(task->files);
        if (task->self) g_object_unref(task->self);
        g_free(task);
    }
}

/* Adds rows in small chunks so a 20k-photo import stays responsive. */
static gboolean populate_chunk_cb(gpointer user_data) {
    PopulateTask *task = user_data;
    StampyWindow *self = task->self;
    guint end;

    if (task->generation != self->populate_generation ||
        !task->files || task->total != task->files->len) {
        self->populate_source_id = 0;
        return G_SOURCE_REMOVE;
    }

    end = MIN(task->index + 250, task->total);
    for (; task->index < end; task->index++) {
        GFile *file = g_ptr_array_index(task->files, task->index);
        if (file) append_file_row(self, file);
    }

    if (self->progress_bar) {
        double fraction = task->total > 0 ? (double)task->index / (double)task->total : 1.0;
        char *title = g_strdup_printf("Loading list… %d%%", (int)(fraction * 100.0 + 0.5));

        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(self->progress_bar), fraction);
        gtk_widget_set_visible(self->progress_bar, TRUE);
        if (!self->export_active) gtk_window_set_title(GTK_WINDOW(self), title);
        g_free(title);
    }

    if (task->index < task->total) return G_SOURCE_CONTINUE;

    self->populate_source_id = 0;
    finish_list_population(self);
    return G_SOURCE_REMOVE;
}

/* Guarantees one list entry per unique (PU_Folder, Filename/No). Duplicate
 * basenames across different PU folders must NEVER be removed. */
static void dedupe_image_files(StampyWindow *self) {
    GHashTable *seen;
    GPtrArray *unique;
    guint i, j;
    guint removed = 0;

    if (!self || !self->image_files || self->image_files->len < 2) return;

    seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    unique = g_ptr_array_new_with_free_func(g_object_unref);

    for (i = 0; i < self->image_files->len; i++) {
        GFile *file = g_ptr_array_index(self->image_files, i);
        char *subpath = NULL;
        char *base = NULL;
        char *key = NULL;
        gboolean duplicate = FALSE;

        if (!file) continue;

        /* Key by (PU_rel_subpath, Filename/No) composite key.
         * Files with the same name in different PU folders or subfolders must NEVER collide. */
        subpath = stampy_get_pu_rel_subpath_for_file(file);
        base = g_file_get_basename(file);

        if (base) {
            char *norm_subpath = subpath ? g_utf8_strdown(subpath, -1) : g_strdup("");
            char *norm_base = g_utf8_strdown(base, -1);
            key = g_strdup_printf("%s|%s", norm_subpath, norm_base);
            g_free(norm_subpath);
            g_free(norm_base);
        }
        g_free(subpath);
        g_free(base);

        if (key) {
            if (g_hash_table_contains(seen, key))
                duplicate = TRUE;
            else
                g_hash_table_add(seen, g_strdup(key));
        } else {
            for (j = 0; j < unique->len; j++) {
                if (g_file_equal(g_ptr_array_index(unique, j), file)) {
                    duplicate = TRUE;
                    break;
                }
            }
        }

        if (duplicate) {
            removed++;
        } else {
            g_ptr_array_add(unique, g_object_ref(file));
        }
        g_free(key);
    }

    g_hash_table_unref(seen);

    if (removed > 0) {
        g_ptr_array_unref(self->image_files);
        self->image_files = unique;
        stampy_append_log(self, "[WARN] Removed duplicate image entries.");
    } else {
        g_ptr_array_unref(unique);
    }
}

static void clear_listbox_safely(GtkListBox *box) {
    GtkWidget *child;

    if (!box) return;
    child = gtk_widget_get_first_child(GTK_WIDGET(box));
    while (child != NULL) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        if (GTK_IS_LIST_BOX_ROW(child)) {
            gtk_list_box_remove(box, child);
        } else {
            gtk_widget_unparent(child);
        }
        child = next;
    }
}

/* Populates the PU Folder Selector dropdown at the top of the file viewer. */
void stampy_populate_pu_dropdown(StampyWindow *self) {
    GHashTable *seen_pu;
    GPtrArray *pu_list;
    GtkStringList *string_list;
    guint i;
    int selected_idx = -1;

    if (!self || !self->drop_pu_folder || !self->box_pu_selector) return;

    if (!self->image_files || self->image_files->len == 0) {
        gtk_widget_set_visible(self->box_pu_selector, FALSE);
        g_clear_pointer(&self->pu_folders, g_ptr_array_unref);
        g_clear_pointer(&self->selected_pu_folder, g_free);
        return;
    }

    seen_pu = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    pu_list = g_ptr_array_new_with_free_func(g_free);

    for (i = 0; i < self->image_files->len; i++) {
        GFile *file = g_ptr_array_index(self->image_files, i);
        char *pu = stampy_get_pu_code_for_file(file);
        if (pu && *pu != '\0') {
            if (!g_hash_table_contains(seen_pu, pu)) {
                g_hash_table_add(seen_pu, g_strdup(pu));
                g_ptr_array_add(pu_list, g_strdup(pu));
            }
        }
        g_free(pu);
    }
    g_hash_table_unref(seen_pu);

    /* Sort PU folder names naturally (e.g. PU III.3 before PU III.12) */
    if (pu_list->len > 1) {
        g_ptr_array_sort(pu_list, (GCompareFunc)compare_strings_naturally);
    }

    /* Only show dropdown if multiple PU folders exist */
    if (pu_list->len > 1) {
        gboolean pu_changed = TRUE;

        /* Check if the set of PU folders actually changed */
        if (self->pu_folders && self->pu_folders->len == pu_list->len) {
            pu_changed = FALSE;
            for (i = 0; i < pu_list->len; i++) {
                if (g_strcmp0(g_ptr_array_index(self->pu_folders, i), g_ptr_array_index(pu_list, i)) != 0) {
                    pu_changed = TRUE;
                    break;
                }
            }
        }

        if (pu_changed) {
            string_list = gtk_string_list_new(NULL);
            for (i = 0; i < pu_list->len; i++) {
                const char *pu = g_ptr_array_index(pu_list, i);
                gtk_string_list_append(string_list, pu);
                if (self->selected_pu_folder && g_strcmp0(self->selected_pu_folder, pu) == 0) {
                    selected_idx = (int)i;
                }
            }

            g_clear_pointer(&self->pu_folders, g_ptr_array_unref);
            self->pu_folders = pu_list;

            if (selected_idx < 0) {
                selected_idx = 0;
                g_free(self->selected_pu_folder);
                self->selected_pu_folder = g_strdup(g_ptr_array_index(self->pu_folders, 0));
            }

            in_pu_dropdown_update = TRUE;
            g_signal_handlers_block_by_func(self->drop_pu_folder, on_pu_folder_selected, self);
            gtk_drop_down_set_model(GTK_DROP_DOWN(self->drop_pu_folder), G_LIST_MODEL(string_list));
            g_object_unref(string_list);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(self->drop_pu_folder), (guint)selected_idx);
            g_signal_handlers_unblock_by_func(self->drop_pu_folder, on_pu_folder_selected, self);
            in_pu_dropdown_update = FALSE;

            gtk_widget_set_visible(self->box_pu_selector, TRUE);
        } else {
            g_ptr_array_unref(pu_list);
        }
    } else {
        /* Single PU folder or flat files: hide dropdown, show all */
        gtk_widget_set_visible(self->box_pu_selector, FALSE);
        g_clear_pointer(&self->pu_folders, g_ptr_array_unref);
        self->pu_folders = pu_list;
        g_clear_pointer(&self->selected_pu_folder, g_free);
    }
}

static void stampy_rebuild_file_listbox(StampyWindow *self) {
    GPtrArray *display_files;
    guint total, i;

    if (!self || !self->file_listbox) return;

    /* Cancel any in-flight chunked population. */
    self->populate_generation++;
    if (self->populate_source_id > 0) {
        g_source_remove(self->populate_source_id);
        self->populate_source_id = 0;
    }

    g_signal_handlers_block_matched(self->file_listbox, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, self);
    gtk_widget_set_visible(GTK_WIDGET(self->file_listbox), FALSE);

    /* Invalidate preview cache on full list reload */
    if (self->preview_cached_pixbuf) {
        g_object_unref(self->preview_cached_pixbuf);
        self->preview_cached_pixbuf = NULL;
    }
    g_free(self->preview_cached_path);
    self->preview_cached_path = NULL;

    /* Remove existing rows cleanly. */
    clear_listbox_safely(GTK_LIST_BOX(self->file_listbox));

    if (!self->image_files || self->image_files->len == 0) {
        g_signal_handlers_unblock_matched(self->file_listbox, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, self);

        if (self->left_stack) {
            if (gtk_stack_get_child_by_name(GTK_STACK(self->left_stack), "page_drop")) {
                gtk_stack_set_visible_child_name(GTK_STACK(self->left_stack), "page_drop");
            }
        }
        if (self->img_preview) {
            gtk_picture_set_paintable(GTK_PICTURE(self->img_preview), NULL);
        }
        stampy_check_horizontal_photos(self);
        return;
    }

    /* Filter images belonging to the selected PU folder (or all if single/none) */
    display_files = g_ptr_array_new_with_free_func(g_object_unref);
    for (i = 0; i < self->image_files->len; i++) {
        GFile *file = g_ptr_array_index(self->image_files, i);
        char *pu = stampy_get_pu_code_for_file(file);
        gboolean match = (self->selected_pu_folder == NULL ||
                          (pu && g_strcmp0(pu, self->selected_pu_folder) == 0));
        g_free(pu);
        if (match) {
            g_ptr_array_add(display_files, g_object_ref(file));
        }
    }

    total = display_files->len;

    if (total <= 200) {
        for (i = 0; i < total; i++) {
            GFile *file = g_ptr_array_index(display_files, i);
            if (file) append_file_row(self, file);
        }
        g_ptr_array_unref(display_files);
        finish_list_population(self);
        return;
    }

    /* Large list: append in chunks on the main loop. */
    {
        PopulateTask *task = g_new0(PopulateTask, 1);

        task->self = g_object_ref(self);
        task->files = display_files;
        task->generation = self->populate_generation;
        task->total = total;
        if (self->left_stack) {
            if (gtk_stack_get_child_by_name(GTK_STACK(self->left_stack), "page_list")) {
                gtk_stack_set_visible_child_name(GTK_STACK(self->left_stack), "page_list");
            }
        }
        self->populate_source_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                                                   populate_chunk_cb, task,
                                                   populate_task_free);
    }
}

void stampy_populate_file_listbox(StampyWindow *self) {
    if (!self || !self->file_listbox) return;

    dedupe_image_files(self);

    /* Sort all images: PU folders naturally first, then filenames naturally. */
    if (self->image_files && self->image_files->len > 0) {
        g_ptr_array_sort(self->image_files, compare_file_pu_then_basename);
    }

    /* Update PU folder dropdown and active selection */
    stampy_populate_pu_dropdown(self);

    stampy_rebuild_file_listbox(self);
}

static void on_listbox_right_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    GdkRectangle rect;
    GtkListBoxRow *row;

    (void)gesture; (void)n_press;
    if (!self || !self->file_listbox) return;

    row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(self->file_listbox), (int)y);
    if (row) {
        GList *selected = gtk_list_box_get_selected_rows(GTK_LIST_BOX(self->file_listbox));
        if (!g_list_find(selected, row)) {
            gtk_list_box_unselect_all(GTK_LIST_BOX(self->file_listbox));
            gtk_list_box_select_row(GTK_LIST_BOX(self->file_listbox), row);
        }
        g_list_free(selected);
    }

    if (!self->listbox_popover) {
        GtkWidget *parent_widget = gtk_widget_get_parent(GTK_WIDGET(self->file_listbox));
        GMenu *context_menu = g_menu_new();

        g_menu_append(context_menu, "Add Images", "win.add-images");
        g_menu_append(context_menu, "Select All", "win.select-all");
        g_menu_append(context_menu, "Deselect All", "win.deselect-all");
        g_menu_append(context_menu, "Remove Selected", "win.remove-selected");

        self->listbox_popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(context_menu));
        gtk_widget_set_parent(self->listbox_popover, parent_widget ? parent_widget : GTK_WIDGET(self));
        g_object_unref(context_menu);
    }

    rect.x = (int)x;
    rect.y = (int)y;
    rect.width = 1;
    rect.height = 1;

    gtk_popover_set_pointing_to(GTK_POPOVER(self->listbox_popover), &rect);
    gtk_popover_popup(GTK_POPOVER(self->listbox_popover));
}

static void on_select_all_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    if (self && self->file_listbox) {
        gtk_list_box_select_all(GTK_LIST_BOX(self->file_listbox));
    }
}

static void on_deselect_all_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    if (self && self->file_listbox) {
        gtk_list_box_unselect_all(GTK_LIST_BOX(self->file_listbox));
    }
}

static void on_remove_selected_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    GList *selected_rows, *l;

    (void)action; (void)parameter;
    if (!self || !self->file_listbox || !self->image_files) return;

    selected_rows = gtk_list_box_get_selected_rows(GTK_LIST_BOX(self->file_listbox));
    if (!selected_rows) return;

    for (l = selected_rows; l != NULL; l = l->next) {
        GtkListBoxRow *row = GTK_LIST_BOX_ROW(l->data);
        GFile *file = G_FILE(g_object_get_data(G_OBJECT(row), "file"));
        if (file) {
            g_ptr_array_remove(self->image_files, file);
        }
    }
    g_list_free(selected_rows);

    /* The loaded set no longer matches the cached folder tree. */
    g_clear_pointer(&self->scan_cache_sources, g_ptr_array_unref);
    g_clear_pointer(&self->scan_cache_dirs, g_hash_table_unref);

    stampy_populate_file_listbox(self);

    if (self->image_files && self->image_files->len > 0) {
        stampy_render_live_preview(self);
    }

    stampy_update_clear_button_visibility(self);

    stampy_show_toast(self, "Selected images removed!");
    stampy_append_log(self, "[INFO] Selected images removed.");
}

static void on_drop_area_frame_clicked(GtkButton *btn, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)btn;
    if (self) stampy_open_image_picker(self);
}

void stampy_update_clear_button_visibility(StampyWindow *self) {
    gboolean has_asset;

    if (!self) return;

    has_asset = (self->file_compass || self->file_watermark || self->file_geolocation ||
                 (self->image_files && self->image_files->len > 0));

    /* Lock state is shown by the button icon. */
    if (self->btn_compass_lock)
        gtk_widget_set_visible(self->btn_compass_lock, TRUE);
    if (self->btn_header_clear)
        gtk_widget_set_visible(self->btn_header_clear, has_asset);
}

static void on_clear_all_clicked(GtkButton *btn, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)btn;
    if (!self) return;

    if (self->image_files) {
        g_ptr_array_remove_range(self->image_files, 0, self->image_files->len);
    }

    g_clear_pointer(&self->pu_folders, g_ptr_array_unref);
    g_clear_pointer(&self->selected_pu_folder, g_free);
    if (self->box_pu_selector) gtk_widget_set_visible(self->box_pu_selector, FALSE);

    if (self->geo_table) {
        g_hash_table_remove_all(self->geo_table);
    }

    g_clear_object(&self->default_export_folder);
    g_clear_object(&self->import_root);
    g_clear_pointer(&self->scan_cache_sources, g_ptr_array_unref);
    g_clear_pointer(&self->scan_cache_dirs, g_hash_table_unref);
    if (self->btn_header_export)
        gtk_widget_set_tooltip_text(self->btn_header_export, "Export Final Output");

    if (!self->compass_locked) {
        g_clear_object(&self->file_compass);
        if (self->lbl_compass) gtk_label_set_text(GTK_LABEL(self->lbl_compass), "Add Compass");
        if (self->btn_compass_lock) {
            self->compass_locked = FALSE;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_compass_lock), FALSE);
        }
        /* Also forget the remembered folder (it was not locked). */
        stampy_config_set_string(STAMPY_CONFIG_KEY_COMPASS_FOLDER, "");
    }

    g_clear_object(&self->file_watermark);
    g_clear_object(&self->file_geolocation);
    stampy_invalidate_docx_cache(self);

    if (self->banner_critical_coords) {
        adw_banner_set_revealed(ADW_BANNER(self->banner_critical_coords), FALSE);
    }
    g_clear_pointer(&self->critical_coords_details, g_free);

    if (self->banner_critical_docx) {
        adw_banner_set_revealed(ADW_BANNER(self->banner_critical_docx), FALSE);
    }
    g_clear_pointer(&self->critical_docx_details, g_free);

    if (self->lbl_watermark) gtk_label_set_text(GTK_LABEL(self->lbl_watermark), "Add Watermark(.docx)");
    if (self->lbl_geolocation) gtk_label_set_text(GTK_LABEL(self->lbl_geolocation), "Add Geolocation(.xlsx)");

    stampy_populate_file_listbox(self);
    stampy_check_missing_data(self);

    if (self->img_preview) {
        gtk_picture_set_paintable(GTK_PICTURE(self->img_preview), NULL);
    }

    stampy_show_toast(self, "All files removed!");
    stampy_append_log(self, "[INFO] Removed all images, compass, watermark, and geolocation data.");
    stampy_update_clear_button_visibility(self);
}

/* Shows the "What's New" dialog. */
#define STAMPY_RELEASE_DATE "2026-10-25"

static GtkWidget *whats_new_bullet(const char *text) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *dot = gtk_label_new("•");
    GtkWidget *body = gtk_label_new(text);

    gtk_widget_set_valign(dot, GTK_ALIGN_START);
    gtk_widget_set_hexpand(body, TRUE);
    gtk_label_set_xalign(GTK_LABEL(body), 0.0);
    gtk_label_set_wrap(GTK_LABEL(body), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(body), PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(row), dot);
    gtk_box_append(GTK_BOX(row), body);
    return row;
}

static GtkWidget *whats_new_heading(const char *text) {
    GtkWidget *label = gtk_label_new(text);

    gtk_widget_add_css_class(label, "heading");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    return label;
}

static void on_whats_new_close_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    adw_dialog_close(ADW_DIALOG(user_data));
}

static void on_whats_new_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    AdwDialog *dialog;
    GtkWidget *box, *topbar, *title, *subtitle, *scroll, *content, *close_btn;
    g_autofree char *title_text = NULL;

    (void)action; (void)parameter;
    if (!self) return;

    dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, "What's New");
    adw_dialog_set_content_width(dialog, 480);
    adw_dialog_set_content_height(dialog, 560);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    gtk_widget_set_margin_top(box, 24);
    gtk_widget_set_margin_bottom(box, 24);
    adw_dialog_set_child(dialog, box);

    topbar = gtk_center_box_new();
    gtk_box_append(GTK_BOX(box), topbar);

    title_text = g_strdup_printf("What's New in %s?", PACKAGE_VERSION);
    title = gtk_label_new(title_text);
    gtk_widget_add_css_class(title, "title-2");
    gtk_label_set_xalign(GTK_LABEL(title), 0.5);
    gtk_label_set_wrap(GTK_LABEL(title), TRUE);
    gtk_label_set_justify(GTK_LABEL(title), GTK_JUSTIFY_CENTER);
    gtk_center_box_set_center_widget(GTK_CENTER_BOX(topbar), title);

    close_btn = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(close_btn, "flat");
    gtk_widget_add_css_class(close_btn, "circular");
    gtk_widget_set_tooltip_text(close_btn, "Close");
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_whats_new_close_clicked), dialog);
    gtk_center_box_set_end_widget(GTK_CENTER_BOX(topbar), close_btn);

    subtitle = gtk_label_new("Released on " STAMPY_RELEASE_DATE);
    gtk_widget_add_css_class(subtitle, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.5);
    gtk_box_append(GTK_BOX(box), subtitle);

    scroll = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(box), scroll);

    content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_hexpand(content, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content);

    gtk_box_append(GTK_BOX(content), whats_new_heading("Metadata Injection"));
    gtk_box_append(GTK_BOX(content), whats_new_bullet("Fine-tune metadata injection with toggles for Title, Subject, Tags, Date, and Time."));
    gtk_box_append(GTK_BOX(content), whats_new_bullet("Configurable Date source switcher (Auto, Geolocation, Watermark)."));
    gtk_box_append(GTK_BOX(content), whats_new_heading("Data Validation & Formats"));
    gtk_box_append(GTK_BOX(content), whats_new_bullet("Dynamic missing data checks tailored to folder structure mode (Regular vs. PU)."));
    gtk_box_append(GTK_BOX(content), whats_new_bullet("Added CSV geolocation parser with flexible date support."));

    adw_dialog_present(dialog, GTK_WIDGET(self));
}

/* Keyboard shortcut action callbacks. */
static void on_watermark_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    stampy_open_watermark_picker(self);
}

static void on_geolocation_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    stampy_open_geolocation_picker(self);
}

static void on_compass_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    stampy_open_compass_picker(self);
}

static void on_compass_lock_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    stampy_toggle_compass_lock(self);
}

static void on_export_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    stampy_start_export(self);
}

static void on_cancel_export_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    stampy_cancel_export(self);
}

static void on_remove_all_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    if (self) on_clear_all_clicked(NULL, self);
}

static void on_select_preset_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    if (self && self->drop_presets) {
        gtk_widget_activate(GTK_WIDGET(self->drop_presets));
    }
}

static void on_preferences_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    if (self) stampy_show_preferences(self);
}

void stampy_setup_actions_and_menus(StampyWindow *self) {
    GSimpleAction *act_add;
    GSimpleAction *act_sel_all;
    GSimpleAction *act_desel_all;
    GSimpleAction *act_rem_sel;
    GSimpleAction *act_whats_new;
    GSimpleAction *act_watermark;
    GSimpleAction *act_geolocation;
    GSimpleAction *act_compass;
    GSimpleAction *act_compass_lock;
    GSimpleAction *act_export;
    GSimpleAction *act_cancel_export;
    GSimpleAction *act_remove_all;
    GSimpleAction *act_select_preset;
    GSimpleAction *act_preferences;

    if (!self) return;

    if (self->drop_area_frame) {
        g_signal_connect(self->drop_area_frame, "clicked", G_CALLBACK(on_drop_area_frame_clicked), self);
    }

    if (self->btn_header_clear) {
        g_signal_connect(self->btn_header_clear, "clicked", G_CALLBACK(on_clear_all_clicked), self);
        gtk_widget_set_visible(self->btn_header_clear, FALSE);
    }

    if (self->file_listbox) {
        GtkGesture *left_click;
        GtkGesture *right_click;

        gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->file_listbox), GTK_SELECTION_MULTIPLE);
        g_signal_connect(self->file_listbox, "selected-rows-changed", G_CALLBACK(on_listbox_selection_changed), self);

        left_click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(left_click), GDK_BUTTON_PRIMARY);
        g_signal_connect(left_click, "pressed", G_CALLBACK(on_listbox_left_click), self);
        gtk_widget_add_controller(GTK_WIDGET(self->file_listbox), GTK_EVENT_CONTROLLER(left_click));

        right_click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
        g_signal_connect(right_click, "pressed", G_CALLBACK(on_listbox_right_click), self);
        gtk_widget_add_controller(GTK_WIDGET(self->file_listbox), GTK_EVENT_CONTROLLER(right_click));
    }

    if (self->drop_presets) {
        g_signal_connect(self->drop_presets, "notify::selected", G_CALLBACK(on_preset_dropdown_changed), self);
    }
    if (self->btn_add_preset) {
        g_signal_connect(self->btn_add_preset, "clicked", G_CALLBACK(on_btn_add_preset_clicked), self);
    }
    if (self->btn_remove_preset) {
        g_signal_connect(self->btn_remove_preset, "clicked", G_CALLBACK(on_btn_remove_preset_clicked), self);
    }
    if (self->btn_edit_preset) {
        g_signal_connect(self->btn_edit_preset, "clicked", G_CALLBACK(on_btn_edit_preset_clicked), self);
    }
    if (self->btn_apply_preset) {
        g_signal_connect(self->btn_apply_preset, "clicked", G_CALLBACK(on_btn_apply_preset_clicked), self);
    }
    if (self->btn_import_preset) {
        g_signal_connect(self->btn_import_preset, "clicked", G_CALLBACK(on_btn_import_preset_clicked), self);
    }
    if (self->btn_export_preset) {
        g_signal_connect(self->btn_export_preset, "clicked", G_CALLBACK(on_btn_export_preset_clicked), self);
    }

    act_add = g_simple_action_new("add-images", NULL);
    g_signal_connect(act_add, "activate", G_CALLBACK(on_add_images_action_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_add));

    act_sel_all = g_simple_action_new("select-all", NULL);
    g_signal_connect(act_sel_all, "activate", G_CALLBACK(on_select_all_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_sel_all));

    act_desel_all = g_simple_action_new("deselect-all", NULL);
    g_signal_connect(act_desel_all, "activate", G_CALLBACK(on_deselect_all_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_desel_all));

    act_rem_sel = g_simple_action_new("remove-selected", NULL);
    g_signal_connect(act_rem_sel, "activate", G_CALLBACK(on_remove_selected_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_rem_sel));

    /* Register the "What's New" action. */
    act_whats_new = g_simple_action_new("whats-new", NULL);
    g_signal_connect(act_whats_new, "activate", G_CALLBACK(on_whats_new_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_whats_new));

    /* Keyboard shortcut actions. */
    act_watermark = g_simple_action_new("watermark", NULL);
    g_signal_connect(act_watermark, "activate", G_CALLBACK(on_watermark_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_watermark));

    act_geolocation = g_simple_action_new("geolocation", NULL);
    g_signal_connect(act_geolocation, "activate", G_CALLBACK(on_geolocation_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_geolocation));

    act_compass = g_simple_action_new("compass", NULL);
    g_signal_connect(act_compass, "activate", G_CALLBACK(on_compass_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_compass));

    act_compass_lock = g_simple_action_new("compass-lock", NULL);
    g_signal_connect(act_compass_lock, "activate", G_CALLBACK(on_compass_lock_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_compass_lock));

    act_export = g_simple_action_new("export", NULL);
    g_signal_connect(act_export, "activate", G_CALLBACK(on_export_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_export));

    act_cancel_export = g_simple_action_new("cancel-export", NULL);
    g_signal_connect(act_cancel_export, "activate", G_CALLBACK(on_cancel_export_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_cancel_export));

    act_remove_all = g_simple_action_new("remove-all", NULL);
    g_signal_connect(act_remove_all, "activate", G_CALLBACK(on_remove_all_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_remove_all));

    act_select_preset = g_simple_action_new("select-preset", NULL);
    g_signal_connect(act_select_preset, "activate", G_CALLBACK(on_select_preset_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_select_preset));

    act_preferences = g_simple_action_new("preferences", NULL);
    g_signal_connect(act_preferences, "activate", G_CALLBACK(on_preferences_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_preferences));
}
