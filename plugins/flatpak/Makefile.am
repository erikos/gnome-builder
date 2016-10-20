if ENABLE_FLATPAK_PLUGIN

DISTCLEANFILES =
BUILT_SOURCES =
CLEANFILES =
EXTRA_DIST = $(plugin_DATA)

plugindir = $(libdir)/gnome-builder/plugins
plugin_LTLIBRARIES = libflatpak-plugin.la
dist_plugin_DATA = flatpak.plugin

libflatpak_plugin_la_SOURCES = \
       gbp-flatpak-runtime-provider.c \
       gbp-flatpak-runtime-provider.h \
       gbp-flatpak-runtime.c \
       gbp-flatpak-runtime.h \
       ide-flatpak-clone-widget.c \
       ide-flatpak-clone-widget.h \
       ide-flatpak-genesis-addin.c \
       ide-flatpak-genesis-addin.h \
       gbp-flatpak-plugin.c \
       $(NULL)

nodist_libflatpak_plugin_la_SOURCES = \
       ide-flatpak-resources.c \
       ide-flatpak-resources.h

libflatpak_plugin_la_CFLAGS = $(PLUGIN_CFLAGS) $(FLATPAK_CFLAGS)
libflatpak_plugin_la_LIBADD = $(FLATPAK_LIBS)
libflatpak_plugin_la_LDFLAGS = $(PLUGIN_LDFLAGS)

glib_resources_c = ide-flatpak-resources.c
glib_resources_h = ide-flatpak-resources.h
glib_resources_xml = ide-flatpak-resources.gresource.xml
glib_resources_namespace = ide_flatpak
include $(top_srcdir)/build/autotools/Makefile.am.gresources

include $(top_srcdir)/plugins/Makefile.plugin

endif

-include $(top_srcdir)/git.mk
