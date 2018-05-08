#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>

#include <glib-object.h>

#include "opc-types.h"

#include "opcbroker.h"
#include "pxsource.h"


static void    px_source_finalize    (GObject    *object);

G_DEFINE_TYPE (PxSource, px_source, G_TYPE_OBJECT)

#define parent_class px_source_parent_class


static void
px_source_class_init (PxSourceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = px_source_finalize;
}


static void
px_source_init (PxSource *client)
{
}


static void
px_source_finalize (GObject *object)
{
  PxSource *pxs = PX_SOURCE (object);

  g_free (pxs->cur_frame_rgba);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}
