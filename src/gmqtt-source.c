#include <glib.h>
#include <mosquitto.h>

#include "gmqtt-source.h"

typedef struct _GMqttSource GMqttSource;
struct _GMqttSource
{
  GSource           source;
  struct mosquitto *mosq;
  gint              fd;
  gpointer         *fd_tag;
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
  gmqtt->fd = -1;
  gmqtt->fd_tag = NULL;

  gmqtt->fd = mosquitto_socket (gmqtt->mosq);
  if (gmqtt->fd >= 0)
    gmqtt->fd_tag = g_source_add_unix_fd (source, gmqtt->fd, G_IO_IN);

  return &gmqtt->source;
}


static gboolean
gmqtt_source_prepare (GSource *source,
                      gint    *timeout)
{
  GMqttSource *gmqtt = (GMqttSource *) source;
  gboolean want_write;
  gint fd;

  mosquitto_loop_misc (gmqtt->mosq);

  want_write = mosquitto_want_write (gmqtt->mosq);

  *timeout = 500;

  fd = mosquitto_socket (gmqtt->mosq);
  if (fd != gmqtt->fd)
    {
      if (gmqtt->fd_tag)
        g_source_remove_unix_fd (source, gmqtt->fd_tag);
      gmqtt->fd_tag = NULL;
      gmqtt->fd = -1;
    }

  if (fd < 0)
    return FALSE;

  if (gmqtt->fd_tag == NULL)
    {
      gmqtt->fd_tag = g_source_add_unix_fd (source, fd,
                                            want_write ? G_IO_IN | G_IO_OUT :
                                                         G_IO_IN);
      gmqtt->fd = fd;
    }
  else
    {
      g_source_modify_unix_fd (source, gmqtt->fd_tag,
                               want_write ? G_IO_IN | G_IO_OUT : G_IO_IN);
    }

  return want_write ? TRUE : FALSE;
}


static gboolean
gmqtt_source_check (GSource *source)
{
  GMqttSource *gmqtt = (GMqttSource *) source;
  gint dummy_timeout;

  if (gmqtt->fd_tag && g_source_query_unix_fd (source, gmqtt->fd_tag))
    return TRUE;

  return FALSE;
}


static gboolean
gmqtt_source_dispatch (GSource     *source,
                       GSourceFunc  callback,
                       gpointer     data)
{
  GMqttSource *gmqtt = (GMqttSource *) source;
  GIOCondition cond;

  cond = g_source_query_unix_fd (source, gmqtt->fd_tag);

  if (cond & G_IO_IN)
    {
      mosquitto_loop_read (gmqtt->mosq, 1);
    }

  if (cond & G_IO_OUT)
    {
      mosquitto_loop_write (gmqtt->mosq, 1);
    }

  return G_SOURCE_CONTINUE;
}


static void
gmqtt_source_finalize (GSource *source)
{
  GMqttSource *gmqtt = (GMqttSource *) source;

  if (gmqtt->fd_tag)
    g_source_remove_unix_fd (source, gmqtt->fd_tag);
}


