/*
 * Copyright (C) 2010 Intel, Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Author: Sergey Udaltsov <svu@gnome.org>
 *
 */

#define PAGE_ID "keyboard"

#include "config.h"

#include <locale.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <gtk/gtk.h>

#include "gis-keyboard-page.h"
#include "keyboard-resources.h"
#include "cc-input-chooser.h"

#define GNOME_DESKTOP_USE_UNSTABLE_API
#include <libgnome-desktop/gnome-languages.h>
#include <libgnome-desktop/gnome-xkb-info.h>

#ifdef HAVE_IBUS
#include <ibus.h>
#include "cc-ibus-utils.h"
#endif

#include <act/act.h>

#include <egg-list-box.h>
#include <libgd/gd.h>

#define GNOME_DESKTOP_INPUT_SOURCES_DIR "org.gnome.desktop.input-sources"
#define KEY_CURRENT_INPUT_SOURCE "current"
#define KEY_INPUT_SOURCES        "sources"

#define GNOME_SYSTEM_LOCALE_DIR "org.gnome.system.locale"
#define KEY_REGION "region"

#define INPUT_SOURCE_TYPE_XKB "xkb"
#define INPUT_SOURCE_TYPE_IBUS "ibus"

#define MAX_INPUT_ROWS_VISIBLE 5

G_DEFINE_TYPE (GisKeyboardPage, gis_keyboard_page, GIS_TYPE_PAGE)

#define WID(s) GTK_WIDGET (gtk_builder_get_object (self->priv->builder, s))

#define KEYBOARD_PAGE_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GIS_TYPE_KEYBOARD_PAGE, GisKeyboardPagePrivate))

typedef enum {
        CHOOSE_LANGUAGE,
        ADD_INPUT,
        REMOVE_INPUT
} SystemOp;

struct _GisKeyboardPagePrivate {
	GtkBuilder *builder;

        GtkWidget   *login_button;
        GtkWidget   *login_label;
        gboolean     login;
        GPermission *permission;
        SystemOp     op;
        GDBusProxy  *localed;
        GDBusProxy  *session;
        GCancellable *cancellable;

        GtkWidget *overlay;
        GtkWidget *notification;

        GtkWidget *language_section;
        GtkWidget *language_row;
        GtkWidget *language_label;
        GtkWidget *formats_row;
        GtkWidget *formats_label;

        ActUserManager *user_manager;
        ActUser        *user;
        GSettings      *locale_settings;

        gchar *language;
        gchar *region;
        gchar *system_language;
        gchar *system_region;

        GtkWidget *input_section;
        GtkWidget *input_list;
        GtkWidget *add_input;
        GtkWidget *remove_input;
        GtkWidget *show_config;
        GtkWidget *show_layout;
        GtkWidget *input_scrolledwindow;
        guint n_input_rows;

        GSettings *input_settings;
        GnomeXkbInfo *xkb_info;
#ifdef HAVE_IBUS
        IBusBus *ibus;
        GHashTable *ibus_engines;
        GCancellable *ibus_cancellable;
#endif
};

static void
gis_keyboard_page_finalize (GObject *object)
{
	GisKeyboardPage *self = GIS_KEYBOARD_PAGE (object);
	GisKeyboardPagePrivate *priv = self->priv;

        g_cancellable_cancel (priv->cancellable);
        g_clear_object (&priv->cancellable);

        if (priv->user_manager) {
                g_signal_handlers_disconnect_by_data (priv->user_manager, self);
                priv->user_manager = NULL;
        }

        if (priv->user) {
                g_signal_handlers_disconnect_by_data (priv->user, self);
                priv->user = NULL;
        }

        g_clear_object (&priv->permission);
        g_clear_object (&priv->localed);
        g_clear_object (&priv->session);
        g_clear_object (&priv->builder);
        g_clear_object (&priv->locale_settings);
        g_clear_object (&priv->input_settings);
        g_clear_object (&priv->xkb_info);
#ifdef HAVE_IBUS
        g_clear_object (&priv->ibus);
        if (priv->ibus_cancellable)
                g_cancellable_cancel (priv->ibus_cancellable);
        g_clear_object (&priv->ibus_cancellable);
        g_clear_pointer (&priv->ibus_engines, g_hash_table_destroy);
#endif
        g_free (priv->language);
        g_free (priv->region);
        g_free (priv->system_language);
        g_free (priv->system_region);

	G_OBJECT_CLASS (gis_keyboard_page_parent_class)->finalize (object);
}

static void
gis_keyboard_page_constructed (GObject *object)
{
        GisKeyboardPage *self = GIS_KEYBOARD_PAGE (object);
        GisKeyboardPagePrivate *priv = self->priv;

        G_OBJECT_CLASS (gis_keyboard_page_parent_class)->constructed (object);

        gis_page_set_complete (GIS_PAGE (self), TRUE);

        gtk_widget_show (GTK_WIDGET (self));
}

static const char *
gis_keyboard_page_get_help_uri (GisPage *page)
{
        return "help:gnome-help/prefs-language";
}

static GtkBuilder *
gis_keyboard_page_get_builder (GisPage *page)
{
        GisKeyboardPage *self = GIS_KEYBOARD_PAGE (page);
        GisKeyboardPagePrivate *priv = self->priv;

        return priv->builder;
}

static void
gis_keyboard_page_class_init (GisKeyboardPageClass * klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GisPageClass * page_class = GIS_PAGE_CLASS (klass);

	g_type_class_add_private (klass, sizeof (GisKeyboardPagePrivate));

        page_class->page_id = PAGE_ID;
        page_class->get_builder = gis_keyboard_page_get_builder;

        object_class->constructed = gis_keyboard_page_constructed;
	object_class->finalize = gis_keyboard_page_finalize;
}

static void
restart_now (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = self->priv;

        gd_notification_dismiss (GD_NOTIFICATION (self->priv->notification));

        g_dbus_proxy_call (priv->session,
                           "Logout",
                           g_variant_new ("(u)", 0),
                           G_DBUS_CALL_FLAGS_NONE,
                           -1, NULL, NULL, NULL);
}

static void
show_restart_notification (GisKeyboardPage *self,
                           const gchar   *locale)
{
	GisKeyboardPagePrivate *priv = self->priv;
        GtkWidget *box;
        GtkWidget *label;
        GtkWidget *button;
        gchar *current_locale;

        if (priv->notification)
                return;

        if (locale) {
                current_locale = g_strdup (setlocale (LC_MESSAGES, NULL));
                setlocale (LC_MESSAGES, locale);
        }

        priv->notification = gd_notification_new ();
        g_object_add_weak_pointer (G_OBJECT (priv->notification),
                                   (gpointer *)&priv->notification);
        box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 24);
        gtk_widget_set_margin_left (box, 12);
        gtk_widget_set_margin_right (box, 12);
        gtk_widget_set_margin_top (box, 6);
        gtk_widget_set_margin_bottom (box, 6);
        label = gtk_label_new (_("Your session needs to be restarted for changes to take effect"));
        button = gtk_button_new_with_label (_("Restart Now"));
        g_signal_connect_swapped (button, "clicked",
                                  G_CALLBACK (restart_now), self);
        gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (box), button, FALSE, FALSE, 0);
        gtk_widget_show_all (box);

        gtk_container_add (GTK_CONTAINER (priv->notification), box);
        gtk_overlay_add_overlay (GTK_OVERLAY (self->priv->overlay), priv->notification);
        gtk_widget_show (priv->notification);

        if (locale) {
                setlocale (LC_MESSAGES, current_locale);
                g_free (current_locale);
        }
}

static void
update_separator_func (GtkWidget **separator,
                       GtkWidget  *child,
                       GtkWidget  *before,
                       gpointer    user_data)
{
        if (before == NULL)
                return;

        if (*separator == NULL) {
                *separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
                g_object_ref_sink (*separator);
                gtk_widget_show (*separator);
        }
}

typedef struct {
        GisKeyboardPage *self;
        int category;
        gchar *target_locale;
} MaybeNotifyData;

static void
maybe_notify_finish (GObject      *source,
                     GAsyncResult *res,
                     gpointer      data)
{
        MaybeNotifyData *mnd = data;
        GisKeyboardPage *self = mnd->self;
        GError *error = NULL;
        GVariant *retval = NULL;
        gchar *current_lang_code = NULL;
        gchar *current_country_code = NULL;
        gchar *target_lang_code = NULL;
        gchar *target_country_code = NULL;
        const gchar *current_locale = NULL;

        retval = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
        if (!retval) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_warning ("Failed to get locale: %s\n", error->message);
                goto out;
        }

        g_variant_get (retval, "(&s)", &current_locale);

        if (!gnome_parse_locale (current_locale,
                                 &current_lang_code,
                                 &current_country_code,
                                 NULL,
                                 NULL))
                goto out;

        if (!gnome_parse_locale (mnd->target_locale,
                                 &target_lang_code,
                                 &target_country_code,
                                 NULL,
                                 NULL))
                goto out;

        if (g_str_equal (current_lang_code, target_lang_code) == FALSE ||
            g_str_equal (current_country_code, target_country_code) == FALSE)
                show_restart_notification (self,
                                           mnd->category == LC_MESSAGES ? mnd->target_locale : NULL);
out:
        g_free (target_country_code);
        g_free (target_lang_code);
        g_free (current_country_code);
        g_free (current_lang_code);
        g_clear_pointer (&retval, g_variant_unref);
        g_clear_error (&error);
        g_free (mnd->target_locale);
        g_free (mnd);
}

static void
maybe_notify (GisKeyboardPage *self,
              int            category,
              const gchar   *target_locale)
{
        GisKeyboardPagePrivate *priv = self->priv;
        MaybeNotifyData *mnd;

        mnd = g_new0 (MaybeNotifyData, 1);
        mnd->self = self;
        mnd->category = category;
        mnd->target_locale = g_strdup (target_locale);

        g_dbus_proxy_call (priv->session,
                           "GetLocale",
                           g_variant_new ("(i)", category),
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           priv->cancellable,
                           maybe_notify_finish,
                           mnd);
}

static void set_localed_locale (GisKeyboardPage *self);

static void
update_region (GisKeyboardPage *self,
               const gchar   *region)
{
	GisKeyboardPagePrivate *priv = self->priv;

        if (g_strcmp0 (region, priv->region) == 0)
                return;

        g_settings_set_string (priv->locale_settings, KEY_REGION, region);
        maybe_notify (self, LC_TIME, region);
}

static void show_input_chooser (GisKeyboardPage *self);
static void remove_selected_input (GisKeyboardPage *self);

static void
permission_acquired (GObject      *source,
                     GAsyncResult *res,
                     gpointer      data)
{
        GisKeyboardPage *self = data;
	GisKeyboardPagePrivate *priv = self->priv;
        GError *error = NULL;
        gboolean allowed;

        allowed = g_permission_acquire_finish (priv->permission, res, &error);
        if (error) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_warning ("Failed to acquire permission: %s\n", error->message);
                g_error_free (error);
                return;
        }

        if (allowed) {
                switch (priv->op) {
                case ADD_INPUT:
                        show_input_chooser (self);
                        break;
                case REMOVE_INPUT:
                        remove_selected_input (self);
                        break;
                default:
                        g_warning ("Unknown privileged operation: %d\n", priv->op);
                        break;
                }
        }
}

static void
update_region_label (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = self->priv;
        const gchar *region;
        gchar *name;

        if (priv->region == NULL || priv->region[0] == '\0')
                region = priv->language;
        else
                region = priv->region;

        name = gnome_get_country_from_locale (region, region);
        gtk_label_set_label (GTK_LABEL (priv->formats_label), name);
        g_free (name);
}

static void
update_region_from_setting (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = self->priv;

        g_free (priv->region);
        priv->region = g_settings_get_string (priv->locale_settings, KEY_REGION);
        update_region_label (self);
}

static void
update_language_label (GisKeyboardPage *self)
{
	GisKeyboardPagePrivate *priv = self->priv;
        const gchar *language;
        gchar *name;

        if (priv->login)
                language = priv->system_language;
        else
                language = priv->language;
        if (language)
                name = gnome_get_language_from_locale (language, language);
        else
                name = g_strdup (C_("Language", "None"));
        gtk_label_set_label (GTK_LABEL (priv->language_label), name);
        g_free (name);

        /* Formats will change too if not explicitly set. */
        update_region_label (self);
}

#ifdef HAVE_IBUS
static void
update_ibus_active_sources (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = self->priv;
        GList *rows, *l;
        GtkWidget *row;
        const gchar *type;
        const gchar *id;
        IBusEngineDesc *engine_desc;
        gchar *display_name;
        GtkWidget *label;

        rows = gtk_container_get_children (GTK_CONTAINER (priv->input_list));
        for (l = rows; l; l = l->next) {
                row = l->data;
                type = g_object_get_data (G_OBJECT (row), "type");
                id = g_object_get_data (G_OBJECT (row), "id");
                if (g_strcmp0 (type, INPUT_SOURCE_TYPE_IBUS) != 0)
                        continue;

                engine_desc = g_hash_table_lookup (priv->ibus_engines, id);
                if (engine_desc) {
                        display_name = engine_get_display_name (engine_desc);
                        label = GTK_WIDGET (g_object_get_data (G_OBJECT (row), "label"));
                        gtk_label_set_text (GTK_LABEL (label), display_name);
                        g_free (display_name);
                }
        }
        g_list_free (rows);
}

static void
update_input_chooser (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = self->priv;
        GtkWidget *chooser;

        chooser = g_object_get_data (G_OBJECT (self), "input-chooser");
        if (!chooser)
                return;

        cc_input_chooser_set_ibus_engines (chooser, priv->ibus_engines);
}

static void
fetch_ibus_engines_result (GObject       *object,
                           GAsyncResult  *result,
                           GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = self->priv;
        GList *list, *l;
        GError *error;

        error = NULL;
        list = ibus_bus_list_engines_async_finish (priv->ibus, result, &error);
        g_clear_object (&priv->ibus_cancellable);
        if (!list && error) {
                g_warning ("Couldn't finish IBus request: %s", error->message);
                g_error_free (error);
                return;
        }

        /* Maps engine ids to engine description objects */
        priv->ibus_engines = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_object_unref);

        for (l = list; l; l = l->next) {
                IBusEngineDesc *engine = l->data;
                const gchar *engine_id = ibus_engine_desc_get_name (engine);

                if (g_str_has_prefix (engine_id, "xkb:"))
                        g_object_unref (engine);
                else
                        g_hash_table_replace (priv->ibus_engines, (gpointer)engine_id, engine);
        }
        g_list_free (list);

        update_ibus_active_sources (self);
        update_input_chooser (self);
}

static void
fetch_ibus_engines (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = self->priv;

        priv->ibus_cancellable = g_cancellable_new ();

        ibus_bus_list_engines_async (priv->ibus,
                                     -1,
                                     priv->ibus_cancellable,
                                     (GAsyncReadyCallback)fetch_ibus_engines_result,
                                     self);

  /* We've got everything we needed, don't want to be called again. */
  g_signal_handlers_disconnect_by_func (priv->ibus, fetch_ibus_engines, self);
}

static void
maybe_start_ibus (void)
{
        /* IBus doesn't export API in the session bus. The only thing
         * we have there is a well known name which we can use as a
         * sure-fire way to activate it.
         */
        g_bus_unwatch_name (g_bus_watch_name (G_BUS_TYPE_SESSION,
                                              IBUS_SERVICE_IBUS,
                                              G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
                                              NULL,
                                              NULL,
                                              NULL,
                                              NULL));
}

static GDesktopAppInfo *
setup_app_info_for_id (const gchar *id)
{
  GDesktopAppInfo *app_info;
  gchar *desktop_file_name;
  gchar **strv;

  strv = g_strsplit (id, ":", 2);
  desktop_file_name = g_strdup_printf ("ibus-setup-%s.desktop", strv[0]);
  g_strfreev (strv);

  app_info = g_desktop_app_info_new (desktop_file_name);
  g_free (desktop_file_name);

  return app_info;
}
#endif

static void
adjust_input_list_scrolling (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = self->priv;

        if (priv->n_input_rows >= MAX_INPUT_ROWS_VISIBLE) {
                GtkWidget *parent;
                gint height;

                parent = gtk_widget_get_parent (priv->input_scrolledwindow);
                gtk_widget_get_preferred_height (parent, NULL, &height);
                gtk_widget_set_size_request (parent, -1, height);

                gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (priv->input_scrolledwindow),
                                                GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        } else {
                gtk_widget_set_size_request (gtk_widget_get_parent (priv->input_scrolledwindow), -1, -1);
                gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (priv->input_scrolledwindow),
                                                GTK_POLICY_NEVER, GTK_POLICY_NEVER);
        }
}

static GtkWidget *
add_input_row (GisKeyboardPage   *self,
               const gchar     *type,
               const gchar     *id,
               const gchar     *name,
               GDesktopAppInfo *app_info)
{
        GisKeyboardPagePrivate *priv = self->priv;
        GtkWidget *row;
        GtkWidget *label;
        GtkWidget *image;

        if (priv->login) {
                GList *l;
                l = gtk_container_get_children (GTK_CONTAINER (priv->input_list));
                if (l && l->next == NULL &&
                    g_strcmp0 (g_object_get_data (G_OBJECT (l->data), "type"), "none") == 0)
                        gtk_container_remove (GTK_CONTAINER (priv->input_list), GTK_WIDGET (l->data));
                g_list_free (l);
        }

        row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        label = gtk_label_new (name);
        gtk_misc_set_alignment (GTK_MISC (label), 0, 0.5);
        gtk_widget_set_margin_left (label, 20);
        gtk_widget_set_margin_right (label, 20);
        gtk_widget_set_margin_top (label, 6);
        gtk_widget_set_margin_bottom (label, 6);
        gtk_box_pack_start (GTK_BOX (row), label, TRUE, TRUE, 0);

        if (strcmp (type, INPUT_SOURCE_TYPE_IBUS) == 0) {
                image = gtk_image_new_from_icon_name ("system-run-symbolic", GTK_ICON_SIZE_BUTTON);
                gtk_widget_set_margin_left (image, 20);
                gtk_widget_set_margin_right (image, 20);
                gtk_widget_set_margin_top (image, 6);
                gtk_widget_set_margin_bottom (image, 6);
                gtk_style_context_add_class (gtk_widget_get_style_context (image), "dim-label");
                gtk_box_pack_start (GTK_BOX (row), image, FALSE, TRUE, 0);
        }

        gtk_widget_show_all (row);
        gtk_container_add (GTK_CONTAINER (self->priv->input_list), row);

        g_object_set_data (G_OBJECT (row), "label", label);
        g_object_set_data (G_OBJECT (row), "type", (gpointer)type);
        g_object_set_data_full (G_OBJECT (row), "id", g_strdup (id), g_free);
        if (app_info) {
                g_object_set_data_full (G_OBJECT (row), "app-info", g_object_ref (app_info), g_object_unref);
        }

        priv->n_input_rows += 1;
        adjust_input_list_scrolling (self);

        return row;
}

static void
add_input_sources (GisKeyboardPage *self,
                   GVariant      *sources)
{
        GisKeyboardPagePrivate *priv = self->priv;
        GVariantIter iter;
        const gchar *type;
        const gchar *id;
        const gchar *name;
        gchar *display_name;
        GDesktopAppInfo *app_info;

        g_variant_iter_init (&iter, sources);
        while (g_variant_iter_next (&iter, "(&s&s)", &type, &id)) {
                display_name = NULL;
                app_info = NULL;

                if (g_str_equal (type, INPUT_SOURCE_TYPE_XKB)) {
                        gnome_xkb_info_get_layout_info (priv->xkb_info, id, &name, NULL, NULL, NULL);
                        if (!name) {
                                g_warning ("Couldn't find XKB input source '%s'", id);
                                continue;
                        }
                        display_name = g_strdup (name);
                        type = INPUT_SOURCE_TYPE_XKB;
#ifdef HAVE_IBUS
                } else if (g_str_equal (type, INPUT_SOURCE_TYPE_IBUS)) {
                        IBusEngineDesc *engine_desc = NULL;

                        if (priv->ibus_engines)
                                engine_desc = g_hash_table_lookup (priv->ibus_engines, id);
                        if (engine_desc)
                                display_name = engine_get_display_name (engine_desc);

                        app_info = setup_app_info_for_id (id);
                        type = INPUT_SOURCE_TYPE_IBUS;
#endif
                } else {
                        g_warning ("Unhandled input source type '%s'", type);
                        continue;
                }

                add_input_row (self, type, id, display_name ? display_name : id, app_info);
                g_free (display_name);
                g_clear_object (&app_info);
        }
}

static void
add_input_sources_from_settings (GisKeyboardPage *self)
{
	GisKeyboardPagePrivate *priv = self->priv;
        GVariant *sources;
        sources = g_settings_get_value (priv->input_settings, "sources");
        add_input_sources (self, sources);
        g_variant_unref (sources);
}

static void
clear_input_sources (GisKeyboardPage *self)
{
	GisKeyboardPagePrivate *priv = self->priv;
        GList *list, *l;
        list = gtk_container_get_children (GTK_CONTAINER (priv->input_list));
        for (l = list; l; l = l->next) {
                gtk_container_remove (GTK_CONTAINER (priv->input_list), GTK_WIDGET (l->data));
        }
        g_list_free (list);

        priv->n_input_rows = 0;
        adjust_input_list_scrolling (self);
}

static void
select_by_id (GtkWidget   *row,
              gpointer     data)
{
        const gchar *id = data;
        const gchar *row_id;

        row_id = (const gchar *)g_object_get_data (G_OBJECT (row), "id");
        if (g_strcmp0 (row_id, id) == 0)
                egg_list_box_select_child (EGG_LIST_BOX (gtk_widget_get_parent (row)), row);
}

static void
select_input (GisKeyboardPage *self,
              const gchar   *id)
{
        gtk_container_foreach (GTK_CONTAINER (self->priv->input_list),
                               select_by_id, (gpointer)id);
}

static void
input_sources_changed (GSettings     *settings,
                       const gchar   *key,
                       GisKeyboardPage *self)
{
	GisKeyboardPagePrivate *priv = self->priv;
        GtkWidget *selected;
        gchar *id = NULL;

        selected = egg_list_box_get_selected_child (EGG_LIST_BOX (priv->input_list));
        if (selected)
                id = g_strdup (g_object_get_data (G_OBJECT (selected), "id"));
        clear_input_sources (self);
        add_input_sources_from_settings (self);
        if (id) {
                select_input (self, id);
                g_free (id);
        }
}


static void
update_buttons (GisKeyboardPage *self)
{
	GisKeyboardPagePrivate *priv = self->priv;
        GtkWidget *selected;
        GList *children;
        gboolean multiple_sources;

        children = gtk_container_get_children (GTK_CONTAINER (priv->input_list));
        multiple_sources = g_list_next (children) != NULL;
        g_list_free (children);

        selected = egg_list_box_get_selected_child (EGG_LIST_BOX (priv->input_list));
        if (selected == NULL) {
                gtk_widget_set_visible (priv->show_config, FALSE);
                gtk_widget_set_sensitive (priv->remove_input, FALSE);
                gtk_widget_set_sensitive (priv->show_layout, FALSE);
        } else {
                GDesktopAppInfo *app_info;

                app_info = (GDesktopAppInfo *)g_object_get_data (G_OBJECT (selected), "app-info");

                gtk_widget_set_visible (priv->show_config, app_info != NULL);
                gtk_widget_set_sensitive (priv->show_layout, TRUE);
                gtk_widget_set_sensitive (priv->remove_input, multiple_sources);
        }
}

static void
set_input_settings (GisKeyboardPage *self)
{
	GisKeyboardPagePrivate *priv = self->priv;
        const gchar *type;
        const gchar *id;
        GVariantBuilder builder;
        GVariant *old_sources;
        const gchar *old_current_type;
        const gchar *old_current_id;
        guint old_current;
        guint old_n_sources;
        guint index;
        GList *list, *l;

        old_sources = g_settings_get_value (priv->input_settings, KEY_INPUT_SOURCES);
        old_current = g_settings_get_uint (priv->input_settings, KEY_CURRENT_INPUT_SOURCE);
        old_n_sources = g_variant_n_children (old_sources);

        if (old_n_sources > 0 && old_current < old_n_sources) {
                g_variant_get_child (old_sources, old_current,
                                     "(&s&s)", &old_current_type, &old_current_id);
        } else {
                old_current_type = "";
                old_current_id = "";
        }

        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ss)"));
        index = 0;
        list = gtk_container_get_children (GTK_CONTAINER (priv->input_list));
        for (l = list; l; l = l->next) {
                type = (const gchar *)g_object_get_data (G_OBJECT (l->data), "type");
                id = (const gchar *)g_object_get_data (G_OBJECT (l->data), "id");
                if (index != old_current &&
                    g_str_equal (type, old_current_type) &&
                    g_str_equal (id, old_current_id)) {
                        g_settings_set_uint (priv->input_settings, KEY_CURRENT_INPUT_SOURCE, index);
                }
                g_variant_builder_add (&builder, "(ss)", type, id);
                index += 1;
        }
        g_list_free (list);

        g_settings_set_value (priv->input_settings, KEY_INPUT_SOURCES, g_variant_builder_end (&builder));
        g_settings_apply (priv->input_settings);

        g_variant_unref (old_sources);
}

static void set_localed_input (GisKeyboardPage *self);

static void
update_input (GisKeyboardPage *self)
{
	GisKeyboardPagePrivate *priv = self->priv;

        if (priv->login) {
                set_localed_input (self);
        } else {
                set_input_settings (self);
        }
}

static void
apologize_for_no_ibus_login (GisKeyboardPage *self)
{
        GtkWidget *dialog;
        GtkWidget *toplevel;
        GtkWidget *image;

        toplevel = gtk_widget_get_toplevel (GTK_WIDGET (self));

        dialog = gtk_message_dialog_new (GTK_WINDOW (toplevel),
                                         GTK_DIALOG_MODAL,
                                         GTK_MESSAGE_OTHER,
                                         GTK_BUTTONS_OK,
                                         _("Sorry"));
        gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
                                                  "%s", _("Input methods can't be used on the login screen"));
        image = gtk_image_new_from_icon_name ("face-sad-symbolic",
                                              GTK_ICON_SIZE_DIALOG);
        gtk_widget_show (image);
        gtk_message_dialog_set_image (GTK_MESSAGE_DIALOG (dialog), image);

        gtk_dialog_run (GTK_DIALOG (dialog));
        gtk_widget_destroy (dialog);
}

static gboolean
input_source_already_added (GisKeyboardPage *self,
                            const gchar   *id)
{
        GisKeyboardPagePrivate *priv = self->priv;
        GList *list, *l;
        gboolean retval = FALSE;

        list = gtk_container_get_children (GTK_CONTAINER (priv->input_list));
        for (l = list; l; l = l->next)
                if (g_str_equal (id, (const gchar *) g_object_get_data (G_OBJECT (l->data), "id"))) {
                        retval = TRUE;
                        break;
                }
        g_list_free (list);

        return retval;
}

static void
input_response (GtkWidget *chooser, gint response_id, gpointer data)
{
	GisKeyboardPage *self = data;
        GisKeyboardPagePrivate *priv = self->priv;
        gchar *type;
        gchar *id;
        gchar *name;
        GDesktopAppInfo *app_info = NULL;

        if (response_id == GTK_RESPONSE_OK) {
                if (cc_input_chooser_get_selected (chooser, &type, &id, &name) &&
                    !input_source_already_added (self, id)) {
                        if (g_str_equal (type, INPUT_SOURCE_TYPE_IBUS)) {
                                g_free (type);
                                type = INPUT_SOURCE_TYPE_IBUS;
#ifdef HAVE_IBUS
                                app_info = setup_app_info_for_id (id);
#endif
                        } else {
                                g_free (type);
                                type = INPUT_SOURCE_TYPE_XKB;
                        }

                        if (priv->login && g_str_equal (type, INPUT_SOURCE_TYPE_IBUS)) {
                                apologize_for_no_ibus_login (self);
                        } else {
                                add_input_row (self, type, id, name, app_info);
                                update_buttons (self);
                                update_input (self);
                        }
                        g_free (id);
                        g_free (name);
                        g_clear_object (&app_info);
                }
        }
        gtk_widget_destroy (chooser);
        g_object_set_data (G_OBJECT (self), "input-chooser", NULL);
}

static void
show_input_chooser (GisKeyboardPage *self)
{
	GisKeyboardPagePrivate *priv = self->priv;
        GtkWidget *chooser;
        GtkWidget *toplevel;

        toplevel = gtk_widget_get_toplevel (GTK_WIDGET (self));
        chooser = cc_input_chooser_new (GTK_WINDOW (toplevel),
                                        priv->xkb_info,
#ifdef HAVE_IBUS
                                        priv->ibus_engines
#else
                                        NULL
#endif
                );
        g_signal_connect (chooser, "response",
                          G_CALLBACK (input_response), self);
        gtk_window_present (GTK_WINDOW (chooser));

        g_object_set_data (G_OBJECT (self), "input-chooser", chooser);
}

static void
add_input (GisKeyboardPage *self)
{
	GisKeyboardPagePrivate *priv = self->priv;

        if (!priv->login) {
                show_input_chooser (self);
        } else if (g_permission_get_allowed (priv->permission)) {
                show_input_chooser (self);
        } else if (g_permission_get_can_acquire (priv->permission)) {
                priv->op = ADD_INPUT;
                g_permission_acquire_async (priv->permission,
                                            NULL,
                                            permission_acquired,
                                            self);
        }
}

static GtkWidget *
find_sibling (GtkContainer *container, GtkWidget *child)
{
        GList *list, *c;
        GList *l;
        GtkWidget *sibling;

        list = gtk_container_get_children (container);
        c = g_list_find (list, child);

        for (l = c->next; l; l = l->next) {
                sibling = l->data;
                if (gtk_widget_get_visible (sibling) && gtk_widget_get_child_visible (sibling))
                        goto out;
        }

        for (l = c->prev; l; l = l->prev) {
                sibling = l->data;
                if (gtk_widget_get_visible (sibling) && gtk_widget_get_child_visible (sibling))
                        goto out;
        }

        sibling = NULL;

out:
        g_list_free (list);

        return sibling;
}

static void
do_remove_selected_input (GisKeyboardPage *self)
{
	GisKeyboardPagePrivate *priv = self->priv;
        GtkWidget *selected;
        GtkWidget *sibling;

        selected = egg_list_box_get_selected_child (EGG_LIST_BOX (priv->input_list));
        if (selected == NULL)
                return;

        sibling = find_sibling (GTK_CONTAINER (priv->input_list), selected);
        gtk_container_remove (GTK_CONTAINER (priv->input_list), selected);
        egg_list_box_select_child (EGG_LIST_BOX (priv->input_list), sibling);

        priv->n_input_rows -= 1;
        adjust_input_list_scrolling (self);

        update_buttons (self);
        update_input (self);
}

static void
remove_selected_input (GisKeyboardPage *self)
{
	GisKeyboardPagePrivate *priv = self->priv;

        if (!priv->login) {
                do_remove_selected_input (self);
        } else if (g_permission_get_allowed (priv->permission)) {
                do_remove_selected_input (self);
        } else if (g_permission_get_can_acquire (priv->permission)) {
                priv->op = REMOVE_INPUT;
                g_permission_acquire_async (priv->permission,
                                            NULL,
                                            permission_acquired,
                                            self);
        }
}

static void
show_selected_settings (GisKeyboardPage *self)
{
	GisKeyboardPagePrivate *priv = self->priv;
        GtkWidget *selected;
        GdkAppLaunchContext *ctx;
        GDesktopAppInfo *app_info;
        const gchar *id;
        GError *error = NULL;

        selected = egg_list_box_get_selected_child (EGG_LIST_BOX (priv->input_list));
        if (selected == NULL)
                return;

        app_info = (GDesktopAppInfo *)g_object_get_data (G_OBJECT (selected), "app-info");
        if  (app_info == NULL)
                return;

        ctx = gdk_display_get_app_launch_context (gdk_display_get_default ());
        gdk_app_launch_context_set_timestamp (ctx, gtk_get_current_event_time ());

        id = (const gchar *)g_object_get_data (G_OBJECT (selected), "id");
        g_app_launch_context_setenv (G_APP_LAUNCH_CONTEXT (ctx),
                                     "IBUS_ENGINE_NAME", id);

        if (!g_app_info_launch (G_APP_INFO (app_info), NULL, G_APP_LAUNCH_CONTEXT (ctx), &error)) {
                g_warning ("Failed to launch input source setup: %s", error->message);
                g_error_free (error);
        }

        g_object_unref (ctx);
}

static void
show_selected_layout (GisKeyboardPage *self)
{
	GisKeyboardPagePrivate *priv = self->priv;
        GtkWidget *selected;
        const gchar *type;
        const gchar *id;
        const gchar *layout;
        const gchar *variant;
        gchar *commandline;

        selected = egg_list_box_get_selected_child (EGG_LIST_BOX (priv->input_list));
        if (selected == NULL)
                return;

        type = (const gchar *)g_object_get_data (G_OBJECT (selected), "type");
        id = (const gchar *)g_object_get_data (G_OBJECT (selected), "id");

        if (g_str_equal (type, INPUT_SOURCE_TYPE_XKB)) {
                gnome_xkb_info_get_layout_info (priv->xkb_info,
                                                id, NULL, NULL,
                                                &layout, &variant);

                if (!layout || !layout[0]) {
                        g_warning ("Couldn't find XKB input source '%s'", id);
                        return;
                }
#ifdef HAVE_IBUS
        } else if (g_str_equal (type, INPUT_SOURCE_TYPE_IBUS)) {
                IBusEngineDesc *engine_desc = NULL;

                if (priv->ibus_engines)
                        engine_desc = g_hash_table_lookup (priv->ibus_engines, id);

                if (engine_desc) {
                        layout = ibus_engine_desc_get_layout (engine_desc);
                        variant = "";
                } else {
                        g_warning ("Couldn't find IBus input source '%s'", id);
                        return;
                }
#endif
        } else {
                g_warning ("Unhandled input source type '%s'", type);
                return;
        }

        if (variant[0])
                commandline = g_strdup_printf ("gkbd-keyboard-display -l \"%s\t%s\"",
                                               layout, variant);
        else
                commandline = g_strdup_printf ("gkbd-keyboard-display -l %s",
                                               layout);

        g_spawn_command_line_async (commandline, NULL);
        g_free (commandline);
}

static void
setup_input_section (GisKeyboardPage *self)
{
	GisKeyboardPagePrivate *priv = self->priv;

        priv->input_settings = g_settings_new (GNOME_DESKTOP_INPUT_SOURCES_DIR);
        g_settings_delay (priv->input_settings);

        priv->xkb_info = gnome_xkb_info_new ();

#ifdef HAVE_IBUS
        ibus_init ();
        if (!priv->ibus) {
                priv->ibus = ibus_bus_new_async ();
                if (ibus_bus_is_connected (priv->ibus))
                        fetch_ibus_engines (self);
                else
                        g_signal_connect_swapped (priv->ibus, "connected",
                                                  G_CALLBACK (fetch_ibus_engines), self);
        }
        maybe_start_ibus ();
#endif

        priv->input_section = WID ("input_section");
        priv->input_list = WID ("input_list");
        priv->add_input = WID ("input_source_add");
        priv->remove_input = WID ("input_source_remove");
        priv->show_config = WID ("input_source_config");
        priv->show_layout = WID ("input_source_layout");
        priv->input_scrolledwindow = WID ("input_scrolledwindow");

        g_signal_connect_swapped (priv->add_input, "clicked",
                                  G_CALLBACK (add_input), self);
        g_signal_connect_swapped (priv->remove_input, "clicked",
                                  G_CALLBACK (remove_selected_input), self);
        g_signal_connect_swapped (priv->show_config, "clicked",
                                  G_CALLBACK (show_selected_settings), self);
        g_signal_connect_swapped (priv->show_layout, "clicked",
                                  G_CALLBACK (show_selected_layout), self);

        egg_list_box_set_selection_mode (EGG_LIST_BOX (priv->input_list),
                                         GTK_SELECTION_SINGLE);
        egg_list_box_set_separator_funcs (EGG_LIST_BOX (priv->input_list),
                                          update_separator_func,
                                          NULL, NULL);
        g_signal_connect_swapped (priv->input_list, "child-selected",
                                  G_CALLBACK (update_buttons), self);

        g_signal_connect (priv->input_settings, "changed::" KEY_INPUT_SOURCES,
                          G_CALLBACK (input_sources_changed), self);

        add_input_sources_from_settings (self);
        update_buttons (self);
}

static void
on_localed_properties_changed (GDBusProxy     *proxy,
                               GVariant       *changed_properties,
                               const gchar   **invalidated_properties,
                               GisKeyboardPage  *self)
{
	GisKeyboardPagePrivate *priv = self->priv;
        GVariant *v;

        v = g_dbus_proxy_get_cached_property (proxy, "Locale");
        if (v) {
                const gchar **strv;
                gsize len;
                gint i;
                const gchar *lang, *messages, *time;

                strv = g_variant_get_strv (v, &len);

                lang = messages = time = NULL;
                for (i = 0; strv[i]; i++) {
                        if (g_str_has_prefix (strv[i], "LANG=")) {
                                lang = strv[i] + strlen ("LANG=");
                        } else if (g_str_has_prefix (strv[i], "LC_MESSAGES=")) {
                                messages = strv[i] + strlen ("LC_MESSAGES=");
                        } else if (g_str_has_prefix (strv[i], "LC_TIME=")) {
                                time = strv[i] + strlen ("LC_TIME=");
                        }
                }
                if (!lang) {
                        lang = "";
                }
                if (!messages) {
                        messages = lang;
                }
                if (!time) {
                        time = lang;
                }
                g_free (priv->system_language);
                priv->system_language = g_strdup (messages);
                g_free (priv->system_region);
                priv->system_region = g_strdup (time);
                g_variant_unref (v);

                update_language_label (self);
        }
}

static void
add_input_sources_from_localed (GisKeyboardPage *self)
{
	GisKeyboardPagePrivate *priv = self->priv;
        GVariant *v;
        const gchar *s;
        gchar **layouts = NULL;
        gchar **variants = NULL;
        gint i, n;

        if (!priv->localed)
                return;

        v = g_dbus_proxy_get_cached_property (priv->localed, "X11Layout");
        if (v) {
                s = g_variant_get_string (v, NULL);
                layouts = g_strsplit (s, ",", -1);
                g_variant_unref (v);
        }

        v = g_dbus_proxy_get_cached_property (priv->localed, "X11Variant");
        if (v) {
                s = g_variant_get_string (v, NULL);
                if (s && *s)
                        variants = g_strsplit (s, ",", -1);
                g_variant_unref (v);
        }

        if (variants && variants[0])
                n = MIN (g_strv_length (layouts), g_strv_length (variants));
        else if (layouts && layouts[0])
                n = g_strv_length (layouts);
        else
                n = 0;

        for (i = 0; i < n && layouts[i][0]; i++) {
                const gchar *name;
                gchar *id;

                if (variants && variants[i] && variants[i][0])
                        id = g_strdup_printf ("%s+%s", layouts[i], variants[i]);
                else
                        id = g_strdup (layouts[i]);

                gnome_xkb_info_get_layout_info (priv->xkb_info, id, &name, NULL, NULL, NULL);

                add_input_row (self, INPUT_SOURCE_TYPE_XKB, id, name ? name : id, NULL);

                g_free (id);
        }
        if (n == 0) {
                add_input_row (self, "none", "none", _("No input source selected"), NULL);
        }

        g_strfreev (variants);
        g_strfreev (layouts);
}

static void
set_localed_locale (GisKeyboardPage *self)
{
	GisKeyboardPagePrivate *priv = self->priv;
        GVariantBuilder *b;
        gchar *s;

        b = g_variant_builder_new (G_VARIANT_TYPE ("as"));
        s = g_strconcat ("LANG=", priv->system_language, NULL);
        g_variant_builder_add (b, "s", s);
        g_free (s);

        if (g_strcmp0 (priv->system_language, priv->system_region) != 0) {
                s = g_strconcat ("LC_TIME=", priv->system_region, NULL);
                g_variant_builder_add (b, "s", s);
                g_free (s);
                s = g_strconcat ("LC_NUMERIC=", priv->system_region, NULL);
                g_variant_builder_add (b, "s", s);
                g_free (s);
                s = g_strconcat ("LC_MONETARY=", priv->system_region, NULL);
                g_variant_builder_add (b, "s", s);
                g_free (s);
                s = g_strconcat ("LC_MEASUREMENT=", priv->system_region, NULL);
                g_variant_builder_add (b, "s", s);
                g_free (s);
                s = g_strconcat ("LC_PAPER=", priv->system_region, NULL);
                g_variant_builder_add (b, "s", s);
                g_free (s);
        }
        g_dbus_proxy_call (priv->localed,
                           "SetLocale",
                           g_variant_new ("(asb)", b, TRUE),
                           G_DBUS_CALL_FLAGS_NONE,
                           -1, NULL, NULL, NULL);
        g_variant_builder_unref (b);
}

static void
set_localed_input (GisKeyboardPage *self)
{
	GisKeyboardPagePrivate *priv = self->priv;
        GString *layouts;
        GString *variants;
        const gchar *type, *id;
        GList *list, *li;
        const gchar *l, *v;

        layouts = g_string_new ("");
        variants = g_string_new ("");

        list = gtk_container_get_children (GTK_CONTAINER (priv->input_list));
        for (li = list; li; li = li->next) {
                type = (const gchar *)g_object_get_data (G_OBJECT (li->data), "type");
                id = (const gchar *)g_object_get_data (G_OBJECT (li->data), "id");
                if (g_str_equal (type, INPUT_SOURCE_TYPE_IBUS))
                        continue;

                if (gnome_xkb_info_get_layout_info (priv->xkb_info, id, NULL, NULL, &l, &v)) {
                        if (layouts->str[0]) {
                                g_string_append_c (layouts, ',');
                                g_string_append_c (variants, ',');
                        }
                        g_string_append (layouts, l);
                        g_string_append (variants, v);
                }
        }
        g_list_free (list);

        g_dbus_proxy_call (priv->localed,
                           "SetX11Keyboard",
                           g_variant_new ("(ssssbb)", layouts->str, "", variants->str, "", TRUE, TRUE),
                           G_DBUS_CALL_FLAGS_NONE,
                           -1, NULL, NULL, NULL);

        g_string_free (layouts, TRUE);
        g_string_free (variants, TRUE);
}

static void
localed_proxy_ready (GObject      *source,
                     GAsyncResult *res,
                     gpointer      data)
{
        GisKeyboardPage *self = data;
        GisKeyboardPagePrivate *priv;
        GDBusProxy *proxy;
        GError *error = NULL;

        proxy = g_dbus_proxy_new_finish (res, &error);

        if (!proxy) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_warning ("Failed to contact localed: %s\n", error->message);
                g_error_free (error);
                return;
        }

        priv = self->priv;
        priv->localed = proxy;

        gtk_widget_set_sensitive (priv->login_button, TRUE);

        g_signal_connect (priv->localed, "g-properties-changed",
                          G_CALLBACK (on_localed_properties_changed), self);
        on_localed_properties_changed (priv->localed, NULL, NULL, self);
}

static void
login_changed (GisKeyboardPage *self)
{
	GisKeyboardPagePrivate *priv = self->priv;
        gboolean can_acquire;

        priv->login = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (priv->login_button));
        gtk_widget_set_visible (priv->formats_row, !priv->login);
        gtk_widget_set_visible (priv->login_label, priv->login);

        can_acquire = priv->permission &&
                (g_permission_get_allowed (priv->permission) ||
                 g_permission_get_can_acquire (priv->permission));
        /* FIXME: insensitive doesn't look quite right for this */
        gtk_widget_set_sensitive (priv->language_section, !priv->login || can_acquire);
        gtk_widget_set_sensitive (priv->input_section, !priv->login || can_acquire);

        clear_input_sources (self);
        if (priv->login)
                add_input_sources_from_localed (self);
        else
                add_input_sources_from_settings (self);

        update_language_label (self);
        update_buttons (self);
}


static void
session_proxy_ready (GObject      *source,
                     GAsyncResult *res,
                     gpointer      data)
{
        GisKeyboardPage *self = data;
        GDBusProxy *proxy;
        GError *error = NULL;

        proxy = g_dbus_proxy_new_for_bus_finish (res, &error);

        if (!proxy) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_warning ("Failed to contact gnome-session: %s\n", error->message);
                g_error_free (error);
                return;
        }

        self->priv->session = proxy;
}

static void
gis_keyboard_page_init (GisKeyboardPage *self)
{
	GisKeyboardPagePrivate *priv;
	GError *error = NULL;

	priv = self->priv = KEYBOARD_PAGE_PRIVATE (self);
        g_resources_register (keyboard_get_resource ());

	priv->builder = gtk_builder_new ();

	gtk_builder_add_from_resource (priv->builder,
                                       "/org/gnome/initial-setup/gis-keyboard-page.ui",
                                       &error);
	if (error != NULL) {
		g_warning ("Error loading UI file: %s", error->message);
		g_error_free (error);
		return;
	}

        priv->user_manager = act_user_manager_get_default ();

        priv->cancellable = g_cancellable_new ();

        g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                  G_DBUS_PROXY_FLAGS_NONE,
                                  NULL,
                                  "org.gnome.SessionManager",
                                  "/org/gnome/SessionManager",
                                  "org.gnome.SessionManager",
                                  priv->cancellable,
                                  session_proxy_ready,
                                  self);

        setup_input_section (self);

        priv->overlay = GTK_WIDGET (gtk_builder_get_object (priv->builder,
                                                            "keyboard_page"));
        gtk_container_add (GTK_CONTAINER (self), priv->overlay);
}

void
gis_prepare_keyboard_page (GisDriver *driver)
{
  gis_driver_add_page (driver,
                       g_object_new (GIS_TYPE_KEYBOARD_PAGE,
                                     "driver", driver,
                                     NULL));
}
