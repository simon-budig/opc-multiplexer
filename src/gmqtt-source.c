#include <glib.h>
#include <mosquitto.h>

#include "gmqtt-source.h"

typedef struct _GMqttSource GMqttSource;
struct _GMqttSource
{
  GSource           source;
  struct mosquitto *mosq;
  GPollFD          *gfd;
};

static gboolean gmqtt_source_prepare (GSource      *source,
                                      gint         *timeout);
static gboolean gmqtt_source_check   (GSource      *source);
static gboolean gmqtt_source_dispatch (GSource     *source,
                                       GSourceFunc  callback,
                                       gpointer     data);
static void     gmqtt_source_finalize (GSource     *source);


static GSourceFuncs gmqtt_sourcefuncs = {
  .prepare  = gmqtt_source_prepare,
  .check    = gmqtt_source_check,
  .dispatch = gmqtt_source_dispatch,
  .finalize = gmqtt_source_finalize,
};


GSource *
gmqtt_source_new (struct mosquitto *mosq)
{
  GMqttSource *gmqtt;
  GSource *source;

  source = g_source_new (&gmqtt_sourcefuncs,
                         sizeof (GMqttSource));

  gmqtt = (GMqttSource *) source;

  gmqtt->mosq = mosq;

  gmqtt->gfd = g_new0 (GPollFD, 1);
  gmqtt->gfd->fd = -1;

  g_source_add_poll (&gmqtt->source, gmqtt->gfd);

  return &gmqtt->source;
}


static gboolean
gmqtt_source_prepare (GSource *source,
                      gint    *timeout)
{
  GMqttSource *gmqtt = (GMqttSource *) source;

  mosquitto_loop_misc (gmqtt->mosq);

  gmqtt->gfd->fd = mosquitto_socket (gmqtt->mosq);
  gmqtt->gfd->events = G_IO_IN;
  if (mosquitto_want_write (gmqtt->mosq))
    {
      gmqtt->gfd->events |= G_IO_OUT;
      return TRUE;
    }

  return FALSE;
}


static gboolean
gmqtt_source_check (GSource *source)
{
  GMqttSource *gmqtt = (GMqttSource *) source;
  gint dummy_timeout;

  if (gmqtt->gfd->revents)
    return TRUE;

  return gmqtt_source_prepare (source, &dummy_timeout);
}


static gboolean
gmqtt_source_dispatch (GSource     *source,
                       GSourceFunc  callback,
                       gpointer     data)
{
  GMqttSource *gmqtt = (GMqttSource *) source;

  if (gmqtt->gfd->revents & G_IO_IN)
    {
      mosquitto_loop_read (gmqtt->mosq, 1);
    }

  if (gmqtt->gfd->revents & G_IO_OUT)
    {
      mosquitto_loop_write (gmqtt->mosq, 1);
    }

  return G_SOURCE_CONTINUE;
}


static void
gmqtt_source_finalize (GSource *source)
{
  GMqttSource *gmqtt = (GMqttSource *) source;

  g_source_remove_poll (source, gmqtt->gfd);
  g_free (gmqtt->gfd);
}


