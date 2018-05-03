AC_INIT([OPC MPX], 1.0)
AM_INIT_AUTOMAKE
AM_SILENT_RULES([yes])
AC_PROG_CC
PKG_PROG_PKG_CONFIG(0.16)
PKG_CHECK_MODULES([GLIB], [glib-2.0 >= 2.36.1])
PKG_CHECK_MODULES([GOBJECT], [gobject-2.0])
AC_CHECK_HEADER(mosquitto.h, enable_mosquitto=yes, enable_mosquitto=no)
AC_SEARCH_LIBS(mosquitto_socket,[mosquitto],[],[AC_MSG_ERROR([unable to find mosquitto library])])
AC_CONFIG_FILES(Makefile)
AC_CONFIG_FILES(src/Makefile)
AC_OUTPUT
