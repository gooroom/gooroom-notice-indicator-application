bin_PROGRAMS = gooroom-notice-applet

gooroom_notice_applet_SOURCES = \
	gooroom-notice-applet.h \
	gooroom-notice-applet.c

gooroom_notice_applet_CPPFLAGS =	\
    -I. \
    -I$(srcdir) \
    -I$(top_srcdir)

gooroom_notice_applet_CFLAGS =	\
	-DLOCALEDIR=\"$(localedir)\"	\
	$(GLIB_CFLAGS)	\
	$(GTK_CFLAGS)	\
	$(LIBNOTIFY_CFLAGS)	\
	$(LIBWEBKITGTK_CFLAGS)	\
	$(DBUS_CFLAGS)	\
	$(DBUS_GLIB_CFLAGS)	\
	$(JSON_C_CFLAGS)	\
	$(APPINDICATOR_CFLAGS)

gooroom_notice_applet_LDADD =	\
	$(GLIB_LIBS)	\
	$(GTK_LIBS)	\
	$(LIBNOTIFY_LIBS)	\
	$(LIBWEBKITGTK_LIBS)	\
	$(DBUS_LIBS)	\
	$(DBUS_GLIB_LIBS)	\
	$(JSON_C_LIBS)	\
	$(APPINDICATOR_LIBS)
