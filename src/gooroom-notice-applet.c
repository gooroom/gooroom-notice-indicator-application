/*
 * Copyright (c) 2018 - 2019 Gooroom <gooroom@gooroom.kr>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <libappindicator/app-indicator.h>
#include <libnotify/notify.h>

#include <webkit2/webkit2.h>
#include <json-c/json.h>

#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "gooroom-notice-applet.h"

#define NOTIFICATION_LIMIT       (5)
#define NOTIFICATION_TEXT_LIMIT  (17)
#define NOTIFICATION_TIMEOUT     (5000)
#define NOTIFICATION_SIGNAL      "set_noti"
#define NOTIFICATION_MSG_ICON    "notice-indicator-msg"
#define NOTIFICATION_MSG_URGENCY_ICON    "notice-indicator-msg-urgency"
#define DEFAULT_TRAY_ICON        "notice-indicator-panel"
#define DEFAULT_NOTICE_TRAY_ICON "notice-indicator-event-panel"

struct _GooroomNoticeAppletPrivate
{
    GtkWidget    *window;
    gboolean      img_status;

    GQueue       *queue;
    GHashTable   *data_list;
    gint          total;

    gchar    *signing;
    gchar    *session_id;
    gchar    *client_id;
    gchar    *default_domain;
    gint      disabled_cnt;
};

typedef struct
{
    gchar    *url;
    gchar    *title;
    gchar    *icon;
}NoticeData;

typedef struct
{
    gchar    *client_id;
    gchar    *session_id;
    gchar    *signing;
    gchar    *lang;
}CookieData;

G_DEFINE_TYPE_WITH_PRIVATE (GooroomNoticeApplet, gooroom_notice_applet, G_TYPE_OBJECT)

static GtkWidget    *menuitem;
static AppIndicator *indicator;
static uint          log_handler = 0;
static gboolean      is_job = FALSE;
static gboolean      is_agent = FALSE;
static gboolean      is_connected = FALSE;

void
gooroom_log_handler(const gchar *log_domain,
        GLogLevelFlags log_level,
        const gchar *message,
        gpointer user_data)
{
#ifdef DEBUG_MSG
    FILE *file = NULL;
    file = fopen ("/var/tmp/notice.debug", "a");
    if (file == NULL)
        return;

    fputs(message, file);
    fclose (file);
#endif
}

static void
gooroom_tray_icon_change (gpointer user_data)
{
    g_return_if_fail (user_data != NULL);

    GooroomNoticeApplet *applet = GOOROOM_NOTICE_APPLET (user_data);
    GooroomNoticeAppletPrivate *priv = applet->priv;

    if (is_connected && is_agent)
    {
         if (priv->img_status)
            app_indicator_set_status (indicator, APP_INDICATOR_STATUS_ATTENTION);
        else
            app_indicator_set_status (indicator, APP_INDICATOR_STATUS_ACTIVE);
    }
    else
    {
       app_indicator_set_status (indicator, APP_INDICATOR_STATUS_PASSIVE);
    }
}

static gchar*
gooroom_notice_limit_text (gchar* text, gint limit, gint other_cnt)
{
    gchar *title = g_strstrip (text);
    glong len = g_utf8_strlen (title, -1);

    if (other_cnt != 0)
        limit -= 4;

    if (limit < len)
    {
        g_autofree gchar *t = g_utf8_substring (title, 0, limit);
        g_autofree gchar *other = g_strdup_printf (_("other %d cases"), other_cnt);
        title = g_strdup_printf ("%s...", t);
    }

    if (other_cnt != 0)
    {
        g_autofree gchar *other = g_strdup_printf (_("other %d cases"), other_cnt);
        title = g_strdup_printf ("%s %s", title, other);
    }

    return title;
}

static gchar*
gooroom_notice_other_text (gchar *text, gint other_cnt)
{
    gchar *title = g_strstrip (text);

    if (1 < other_cnt)
    {
        g_autofree gchar *other = g_strdup_printf (_("other %d cases"), other_cnt);
        title = g_strdup_printf ("%s %s", title, other);
    }
    return title;
}

json_object *
JSON_OBJECT_GET (json_object *root_obj, const char *key)
{
    if (!root_obj) return NULL;

    json_object *ret_obj = NULL;

    json_object_object_get_ex (root_obj, key, &ret_obj);

    return ret_obj;
}

static gboolean
is_systemd_service_available (const gchar *service_name)
{
    gboolean    ret = TRUE;
    GVariant   *variant;
    GDBusProxy *proxy;
    GError     *error = NULL;

    proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
            G_DBUS_CALL_FLAGS_NONE, NULL,
            "org.freedesktop.systemd1",
            "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager",
            NULL, &error);

    if (!proxy)
    {
        g_error_free (error);
        return FALSE;
    }

    variant = g_dbus_proxy_call_sync (proxy, "GetUnitFileState",
            g_variant_new ("(s)", service_name),
            G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

    if (!variant)
    {
        g_error_free (error);
        ret = FALSE;
    }

    g_object_unref (proxy);

    return ret;
}

static gboolean
is_gooroom_agent_service_available ()
{
    gboolean ret = is_systemd_service_available ("kr.gooroom.agent");

    if (ret)
    {
        return FALSE;
    }
    return TRUE;
}

static GDBusProxy   *agent_proxy = NULL;

static void
gooroom_agent_signal_cb (GDBusProxy *proxy,
                         gchar *sender_name,
                         gchar *signal_name,
                         GVariant *parameters,
                         gpointer user_data)
{

    g_autofree gchar* signal = g_strdup (NOTIFICATION_SIGNAL);
    if (g_strcmp0 (signal_name, signal) == 0)
    {
        g_return_if_fail (user_data != NULL);

        GVariant *v;
        g_variant_get (parameters, "(v)", &v);
        gchar *res = g_variant_dup_string (v, NULL);

        g_debug ("gooroom_agent_signal_cb : [%s]\n", res);
        gooroom_application_notice_get_data_from_json (user_data, res, TRUE);

        GooroomNoticeApplet *applet = GOOROOM_NOTICE_APPLET (user_data);
        GooroomNoticeAppletPrivate *priv = applet->priv;

        guint total = g_queue_get_length (priv->queue);
        if (0 < total || 0 < priv->disabled_cnt)
        {
            priv->img_status = TRUE;
            gooroom_tray_icon_change (user_data);

            if (!is_job)
                g_timeout_add (500, (GSourceFunc) gooroom_notice_applet_job,(gpointer)user_data);
        }
    }
}

static GDBusProxy*
gooroom_agent_proxy_get (void)
{
    if (agent_proxy == NULL)
    {
        agent_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                      G_DBUS_CALL_FLAGS_NONE,
                      NULL,
                      "kr.gooroom.agent",
                      "/kr/gooroom/agent",
                      "kr.gooroom.agent",
                      NULL,
                      NULL);
    }

    return agent_proxy;
}

static void
gooroom_agent_bind_signal (gpointer data)
{
    agent_proxy = gooroom_agent_proxy_get();
    if (agent_proxy)
        g_signal_connect (agent_proxy, "g-signal", G_CALLBACK (gooroom_agent_signal_cb), data);
}

void
gooroom_application_notice_get_data_from_json (gpointer user_data, const gchar *data, gboolean urgency)
{
    g_return_if_fail (user_data != NULL);

    GooroomNoticeApplet *applet = GOOROOM_NOTICE_APPLET (user_data);
    GooroomNoticeAppletPrivate *priv = applet->priv;

    gchar *ret = NULL;

    enum json_tokener_error jerr = json_tokener_success;
    json_object *root_obj = json_tokener_parse_verbose (data, &jerr);

    if (jerr != json_tokener_success)
        goto done;

    json_object *obj1 = NULL, *obj2 = NULL, *obj3 = NULL, *obj4 = NULL, *noti_obj = NULL;

    if (!urgency)
    {
        obj1 = JSON_OBJECT_GET (root_obj, "module");
        obj2 = JSON_OBJECT_GET (obj1, "task");
        obj3 = JSON_OBJECT_GET (obj2, "out");
        obj4 = JSON_OBJECT_GET (obj3, "status");

        if (!obj4)
            goto done;

        const char *val = json_object_get_string (obj4);
        if (val && g_strcmp0 (val, "200") != 0)
            goto done;

        noti_obj = JSON_OBJECT_GET (obj3, "noti_info");
        if (!noti_obj)
            goto done;
    }
    else
    {
        noti_obj = root_obj;
    }

    json_object *enable_view = JSON_OBJECT_GET (noti_obj, "enabled_title_view_notis");
    json_object *disable_view = JSON_OBJECT_GET (noti_obj, "disabled_title_view_cnt");
    json_object *signing = JSON_OBJECT_GET (noti_obj, "signing");
    json_object *client_id = JSON_OBJECT_GET (noti_obj, "client_id");
    json_object *session_id = JSON_OBJECT_GET (noti_obj, "session_id");
    json_object *default_domain = JSON_OBJECT_GET (noti_obj, "default_noti_domain");

    if (signing)
        priv->signing = g_strdup_printf ("%s", json_object_get_string (signing));

    if (client_id)
        priv->client_id = g_strdup_printf ("%s", json_object_get_string (client_id));

    if (session_id)
        priv->session_id = g_strdup_printf ("%s", json_object_get_string (session_id));

    if (disable_view)
        priv->disabled_cnt = json_object_get_int (disable_view);

    if (default_domain)
        priv->default_domain = g_strdup_printf ("%s", json_object_get_string (default_domain));

    if (enable_view)
    {
        gint i = 0;
        gint len = json_object_array_length (enable_view);

        for (i = 0; i < len; i++)
        {
            json_object *v_obj = json_object_array_get_idx (enable_view, i);

            if (!v_obj)
                continue;

            json_object *title = JSON_OBJECT_GET (v_obj, "title");
            json_object *url = JSON_OBJECT_GET (v_obj, "url");

            NoticeData *n;
            n = g_try_new0 (NoticeData, 1);
            n->title = g_strdup_printf ("%s", json_object_get_string (title));
            n->url = g_strdup_printf ("%s", json_object_get_string (url));
            n->icon = urgency ? g_strdup (NOTIFICATION_MSG_URGENCY_ICON) : g_strdup (NOTIFICATION_MSG_ICON);
            g_queue_push_tail (priv->queue, n);
        }
    }
    json_object_put (root_obj);
done:
    json_object_put (root_obj);
}

static void
gooroom_application_notice_done_cb (GObject *source_object,
        GAsyncResult *res,
        gpointer user_data)
{
    g_return_if_fail (user_data != NULL);

    GVariant *variant;
    gchar *data = NULL;
    GError *err = NULL;
    variant = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, &err);
    if (err != NULL)
    {
        const gchar *msg = g_strdup (err->message);
        g_error_free (err);

        g_timeout_add (500, (GSourceFunc) gooroom_application_notice_update_delay, user_data);
        return;
    }

    gooroom_agent_bind_signal (user_data);
    g_error_free (err);

    if (variant)
    {
        GVariant *v;
        g_variant_get (variant, "(v)", &v);
        if (v)
        {
            data = g_variant_dup_string (v, NULL);
            g_variant_unref (v);
        }
        g_variant_unref (v);
    }

    if (data)
    {
        g_debug ("gooroom_application_notice_done_cb : agent param [%s]\n", data);

        gooroom_application_notice_get_data_from_json (user_data, data, FALSE);

        GooroomNoticeApplet *applet = GOOROOM_NOTICE_APPLET (user_data);
        GooroomNoticeAppletPrivate *priv = applet->priv;
        guint total = g_queue_get_length (priv->queue);
        if (0 < total || 0 < priv->disabled_cnt)
        {
            priv->img_status = TRUE;

            if (!is_job)
                g_timeout_add (500, (GSourceFunc) gooroom_notice_applet_job,(gpointer)user_data);
        }

        is_agent = TRUE;
        gooroom_tray_icon_change (user_data);
    }
}

static void
gooroom_application_notice_update (gpointer user_data)
{
    g_return_if_fail (user_data != NULL);

    agent_proxy = gooroom_agent_proxy_get ();

    if (agent_proxy)
    {
        const gchar *json = "{\"module\":{\"module_name\":\"noti\",\"task\":{\"task_name\":\"get_noti\",\"in\":{\"login_id\":\"%s\"}}}}";

        const gchar *user = g_get_user_name();
#if 0
        if (g_strcmp0 (user, "lightdm") == 0)
            user = "";
#endif
        gchar *arg = g_strdup_printf (json, user);
        g_dbus_proxy_call (agent_proxy,
                "do_task",
                g_variant_new ("(s)", arg),
                G_DBUS_CALL_FLAGS_NONE,
                -1,
                NULL,
                gooroom_application_notice_done_cb,
                user_data);

        g_free (arg);
    }
}

gboolean
gooroom_application_notice_update_delay (gpointer user_data)
{
    gooroom_application_notice_update (user_data);
    return FALSE;
}

static void
on_notification_closed (NotifyNotification *notification, gpointer user_data)
{
    g_return_if_fail (user_data != NULL);

    GooroomNoticeApplet *applet = GOOROOM_NOTICE_APPLET (user_data);
    GooroomNoticeAppletPrivate *priv = applet->priv;
    priv->total--;

    if (!priv->window)
    {
        g_hash_table_remove (priv->data_list, notification);
    }
}

static void
on_notification_popup_cookie_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
}

static gboolean
on_notification_popup_closed (GtkWidget *widget, gpointer user_data)
{
    g_return_val_if_fail (user_data != NULL, TRUE);

    GooroomNoticeApplet *applet = GOOROOM_NOTICE_APPLET (user_data);
    GooroomNoticeAppletPrivate *priv = applet->priv;
    if (priv->window != NULL)
    {
        gtk_widget_destroy (priv->window);
        priv->window = NULL;
    }

    return TRUE;
}

static void
on_notification_popup_webview_load_cb (WebKitWebView* view, WebKitLoadEvent load_event, gpointer user_data)
{
    switch (load_event)
    {
        case WEBKIT_LOAD_COMMITTED:
            {
                CookieData *cookie = (CookieData *)user_data;
                if (g_utf8_strlen(cookie->client_id, -1) != 0)
                {
                    g_autofree gchar *script = g_strdup_printf ("document.cookie ='CLIENT_ID=%s;1'", cookie->client_id);
                    webkit_web_view_run_javascript (view, script, NULL, NULL, NULL);
                }

                if (g_utf8_strlen(cookie->session_id, -1) != 0)
                {
                    g_autofree gchar *script = g_strdup_printf ("document.cookie ='SESSION_ID=%s;1'", cookie->session_id);
                    webkit_web_view_run_javascript (view, script, NULL, NULL, NULL);
                }

                if (g_utf8_strlen(cookie->signing, -1) != 0)
                {
                    g_autofree gchar *script = g_strdup_printf ("document.cookie ='SIGNING=%s;1'", cookie->signing);
                    webkit_web_view_run_javascript (view, script, NULL, NULL, NULL);
                }

                if (g_utf8_strlen(cookie->lang, -1) != 0)
                {
                    g_autofree gchar *script = g_strdup_printf ("document.cookie ='LANG_CODE=%s;1'", cookie->lang);
                    webkit_web_view_run_javascript (view, script, NULL, NULL, NULL);
                    g_free (cookie->lang);
                }

                break;
            }
    }
}

static gboolean
on_notification_popup_webview_closed (WebKitWebView* web_view, GtkWidget* window)
{
    gtk_widget_destroy (window);
    return TRUE;
}

static gchar*
gooroom_notice_get_language ()
{
    gchar *lang = NULL;

    PangoLanguage *language = gtk_get_default_language();

    if (language)
    {
        const gchar *plang = pango_language_to_string (language);

        if (g_strcmp0 (plang, "ko-kr") == 0)
            lang = g_strdup ("ko");
        else
            lang = g_strdup ("en");
    }

    return lang;
}

static void
gooroom_notice_popup (gchar *url, gpointer user_data)
{
    GooroomNoticeApplet *applet = GOOROOM_NOTICE_APPLET(user_data);
    GooroomNoticeAppletPrivate *priv = applet->priv;

    if (priv->window != NULL)
    {
        gtk_widget_destroy (priv->window);
        priv->window = NULL;
    }

    if (url == NULL)
        url = priv->default_domain;

    priv->img_status = FALSE;
    gooroom_tray_icon_change (user_data);

    GtkWidget *window;

    window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_container_set_border_width (GTK_CONTAINER (window), 5);
    gtk_window_set_type_hint (GTK_WINDOW (window), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_skip_taskbar_hint (GTK_WINDOW (window), TRUE);
    gtk_window_set_position (GTK_WINDOW (window), GTK_WIN_POS_CENTER);
    gtk_window_set_title (GTK_WINDOW (window), _("Notice"));

    GtkWidget *main_vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add (GTK_CONTAINER (window), main_vbox);
    gtk_widget_show (main_vbox);

    GtkWidget *scroll_window = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start (GTK_BOX (main_vbox), scroll_window, TRUE, TRUE, 0);
    gtk_widget_show (scroll_window);

    WebKitWebView *view = WEBKIT_WEB_VIEW (webkit_web_view_new());
    gtk_container_add (GTK_CONTAINER (scroll_window), GTK_WIDGET(view));

    gchar* lang = gooroom_notice_get_language();

    CookieData *cookie;
    cookie = g_try_new0 (CookieData,1);
    cookie->client_id = priv->client_id;
    cookie->session_id = priv->session_id;
    cookie->signing = priv->signing;
    cookie->lang = lang;

    g_signal_connect (window, "destroy", G_CALLBACK (on_notification_popup_closed), user_data);
    g_signal_connect (view, "close", G_CALLBACK (on_notification_popup_webview_closed), window);
    g_signal_connect (view, "load-changed", G_CALLBACK (on_notification_popup_webview_load_cb), cookie);

    webkit_web_view_load_uri (view, url);

    WebKitWebContext *context = webkit_web_view_get_context (view);
    WebKitCookieManager *manager = webkit_web_context_get_cookie_manager (context);

    gtk_widget_grab_focus (GTK_WIDGET (view));
    gtk_widget_show (GTK_WIDGET(view));

    GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_end (GTK_BOX (main_vbox), hbox, FALSE, TRUE, 0);
    gtk_widget_show (hbox);

    GtkWidget *button = gtk_button_new_with_label (_("Close"));
    gtk_widget_set_can_focus (button, TRUE);
    gtk_box_pack_end (GTK_BOX (hbox), button, FALSE, FALSE, 0);
    g_signal_connect (G_OBJECT (button), "clicked", G_CALLBACK (on_notification_popup_closed), user_data);
    gtk_widget_show (button);

    gtk_window_set_default_size (GTK_WINDOW (window), 600, 550);
    gtk_widget_show_all (window);

    priv->window = window;

    g_queue_clear (priv->queue);
    g_hash_table_remove_all (priv->data_list);
}

static void
on_notification_popup_opened (NotifyNotification *notification, char *action, gpointer user_data)
{
    g_return_if_fail (user_data != NULL);

    GooroomNoticeApplet *applet = GOOROOM_NOTICE_APPLET(user_data);
    GooroomNoticeAppletPrivate *priv = applet->priv;

    NoticeData *data;
    NotifyNotification *key;
    if (!g_hash_table_lookup_extended (priv->data_list, (gpointer)notification, (gpointer)&key, (gpointer)&data))
        return;

    gooroom_notice_popup (data->url, user_data);
}

static NotifyNotification *
notification_opened (gpointer user_data, gchar *title, gchar *icon)
{
    g_return_val_if_fail (user_data != NULL, NULL);

    NotifyNotification *notification;
    notify_init (PACKAGE_NAME);
    notification = notify_notification_new (title, "", icon);
    notify_notification_add_action (notification, "default", _("detail view"), (NotifyActionCallback)on_notification_popup_opened, user_data, NULL);
    notify_notification_set_urgency (notification, NOTIFY_URGENCY_NORMAL);
    notify_notification_set_timeout (notification, NOTIFICATION_TIMEOUT);
    notify_notification_show (notification, NULL);

    return notification;
}

static void
on_notice_applet_hash_key_destroy (gpointer user_data)
{
    g_return_if_fail (user_data != NULL);

    NotifyNotification *n = (NotifyNotification*)user_data;
    notify_notification_close (n, NULL);
}

static void
on_notice_applet_hash_value_destroy (gpointer user_data)
{
    g_return_if_fail (user_data != NULL);

    NoticeData *n = (NoticeData *)user_data;

    if (n->title)
        g_free (n->title);

    if (n->url)
        g_free (n->url);

    if (n->icon)
        g_free (n->icon);
}

static void
on_notice_applet_menuitem_activate_cb (GtkWidget *menuitem, gpointer user_data)
{
    g_return_if_fail (user_data != NULL);

    gooroom_notice_popup (NULL, user_data);
}

gboolean
gooroom_notice_applet_job (gpointer user_data)
{
    g_return_val_if_fail (user_data != NULL, FALSE);

    GooroomNoticeApplet *applet = GOOROOM_NOTICE_APPLET(user_data);
    GooroomNoticeAppletPrivate *priv = applet->priv;

    is_job = TRUE;

    if (NOTIFICATION_LIMIT <= priv->total)
    {
        guint total = g_queue_get_length (priv->queue);
        if (0 == total)
            is_job = FALSE;

        return is_job;
    }

    NoticeData *n;
    while ((n = g_queue_pop_head (priv->queue)))
    {
        if (!n)
            continue;

        priv->total++;

        gchar *title = NULL;
        title = gooroom_notice_limit_text (n->title, NOTIFICATION_TEXT_LIMIT, 0);

        NotifyNotification *notification =  notification_opened (user_data, title, n->icon);
        g_hash_table_insert (priv->data_list, notification, n);
        g_signal_connect (G_OBJECT (notification), "closed", G_CALLBACK (on_notification_closed), user_data);

        return is_job;
    }

    guint total = g_queue_get_length (priv->queue);

    if (0 != total)
        return is_job;

    if (0 != priv->disabled_cnt)
    {
        priv->total++;

        gchar *tmp = g_strdup (_("Notice"));
        gchar *no_title = gooroom_notice_other_text (tmp, priv->disabled_cnt);

        NoticeData *n;
        n = g_try_new0 (NoticeData, 1);
        n->title = no_title;
        n->url = g_strdup_printf ("%s", priv->default_domain);
        n->icon = g_strdup (NOTIFICATION_MSG_ICON);

        NotifyNotification *notification = notification_opened (user_data, no_title, n->icon);
        g_hash_table_insert (priv->data_list, notification, n);
        g_signal_connect (G_OBJECT (notification), "closed", G_CALLBACK (on_notification_closed), user_data);
    }

    is_job = FALSE;
    return is_job;
}

static void
gooroom_notice_applet_network_changed (GNetworkMonitor *monitor,
                                       gboolean network_available,
                                       gpointer user_data)
{
    if (is_connected == network_available)
        return;

    is_connected = network_available;
    gooroom_tray_icon_change (user_data);

    if (is_connected)
    {
        if (agent_proxy == NULL && !is_agent)
            g_timeout_add (500, (GSourceFunc)gooroom_application_notice_update_delay, user_data);
    }
}

static void
gooroom_notice_applet_finalize (GObject *object)
{
    G_OBJECT_CLASS (gooroom_notice_applet_parent_class)->finalize (object);

    GooroomNoticeApplet *applet = GOOROOM_NOTICE_APPLET(object);
    GooroomNoticeAppletPrivate *priv = applet->priv;

    if (priv->signing)
        g_free (priv->signing);

    if (priv->session_id)
        g_free (priv->session_id);

    if (priv->client_id)
        g_free (priv->client_id);

    if (priv->default_domain)
        g_free (priv->default_domain);

    if (priv->window != NULL)
        gtk_widget_destroy (priv->window);

    if (priv->queue)
    {
        g_queue_clear (priv->queue);
        g_queue_free (priv->queue);
        priv->queue = NULL;
    }

    if (priv->data_list)
    {
        g_hash_table_destroy (priv->data_list);
        priv->data_list = NULL;
    }

    if (agent_proxy)
        g_object_unref (agent_proxy);

    if (log_handler != 0)
    {
        g_log_remove_handler (NULL, log_handler);
        log_handler = 0;
    }
}

static void
gooroom_notice_applet_init (GooroomNoticeApplet *applet)
{
    GooroomNoticeAppletPrivate *priv;
    priv = applet->priv = gooroom_notice_applet_get_instance_private (applet);
    priv->window     = NULL;
    priv->img_status = FALSE;

    priv->total      = 0;
    priv->queue      = g_queue_new ();
    priv->data_list  = g_hash_table_new_full (g_direct_hash, g_direct_equal, (GDestroyNotify)on_notice_applet_hash_key_destroy, (GDestroyNotify)on_notice_applet_hash_value_destroy);

    priv->signing    = NULL;
    priv->session_id = NULL;
    priv->client_id  = NULL;
    priv->default_domain = NULL;
    priv->disabled_cnt = 0;
}

static void
gooroom_notice_applet_class_init (GooroomNoticeAppletClass *class)
{
    GObjectClass *object_class;
    object_class = G_OBJECT_CLASS (class);
    object_class->finalize = gooroom_notice_applet_finalize;
}

int
main (int argc, char **argv)
{
    bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    gtk_init (&argc, &argv);

    agent_proxy = NULL;

    GooroomNoticeApplet *applet = g_object_new (TYPE_GOOROOM_NOTICE_APPLET,
            0,
            "kr.gooroom.noticeapplet",
            "flags",
            G_APPLICATION_HANDLES_OPEN,
            NULL);

    indicator = app_indicator_new ("gooroom-notice-applet",
            DEFAULT_TRAY_ICON,
            APP_INDICATOR_CATEGORY_APPLICATION_STATUS);

    app_indicator_set_title(indicator, "gooroom-notice-applet");
    app_indicator_set_attention_icon (indicator, DEFAULT_NOTICE_TRAY_ICON);
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_PASSIVE);

    GtkWidget *menu = gtk_menu_new ();
    menuitem = gtk_menu_item_new_with_label ("dummy");
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), menuitem);
    app_indicator_set_menu (indicator, GTK_MENU (menu));

    gtk_widget_show_all (menu);

    g_signal_connect (menuitem, "activate", G_CALLBACK (on_notice_applet_menuitem_activate_cb), applet);

    log_handler = g_log_set_handler (NULL,
            G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION,
            gooroom_log_handler, NULL);

    GNetworkMonitor *monitor = g_network_monitor_get_default();
    g_signal_connect (monitor, "network-changed", G_CALLBACK (gooroom_notice_applet_network_changed), applet);

    is_connected = g_network_monitor_get_network_available (monitor);

    if (is_connected)
        g_timeout_add (500, (GSourceFunc) gooroom_application_notice_update_delay, (gpointer)applet);

    gtk_main();
}
