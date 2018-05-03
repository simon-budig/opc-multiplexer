bin_PROGRAMS = opc-mpx
opc_mpx_SOURCES =   \
	gmqtt-source.c  \
	gmqtt-client.c  \
	opc-mpx.c   \
	opc-types.h \
	opcbroker.c \
	opcbroker.h \
	opcclient.c \
	opcclient.h

AM_CPPFLAGS =             \
	-D_GNU_SOURCE     \
	$(GLIB_CFLAGS)    \
	$(GOBJECT_CFLAGS) \
	$(CFLAGS)

opc_mpx_LDADD =         \
	$(GLIB_LIBS)    \
	$(GOBJECT_LIBS)
