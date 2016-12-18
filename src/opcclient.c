#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>

#include <glib-object.h>

#include "opc-types.h"

#include "opcbroker.h"
#include "opcclient.h"

static gboolean   opc_client_socket_recv (GIOChannel   *source,
                                          GIOCondition  condition,
                                          gpointer      data);

#define OPC_MESSAGE_LEN (4 + 512 * 3)

#define GET_OPC_FRAME_LEN(buf, len) ((len) >= 4 ? (buf)[2] * 256 + (buf)[3] : -1)

enum
{
  LAST_SIGNAL
};

// static guint opc_broker_signals[LAST_SIGNAL] = { 0 };

static void    opc_client_finalize    (GObject    *object);

G_DEFINE_TYPE (OpcClient, opc_client, G_TYPE_OBJECT)

#define parent_class opc_client_parent_class


static void
opc_client_class_init (OpcClientClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = opc_client_finalize;
}


static void
opc_client_init (OpcClient *client)
{
}


static void
opc_client_finalize (GObject *object)
{
  OpcClient *client = OPC_CLIENT (object);

  if (client->gio != NULL)
    g_io_channel_unref (client->gio);

  g_free (client->inbuf);
  g_free (client->cur_frame);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}


/**
 * opc_client_new:
 *
 * Returns: A new #OpcClient.
 **/
OpcClient *
opc_client_new (OpcBroker *broker,
                gint       fd)
{
  OpcClient *client;

  client = g_object_new (OPC_TYPE_CLIENT, NULL);

  client->broker = broker;
  client->gio = g_io_channel_unix_new (fd);
  g_io_channel_set_close_on_unref (client->gio, TRUE);
  client->dump_len = 0;
  client->in_len = 0;
  client->inbuf = g_new (guint8, OPC_MESSAGE_LEN);
  client->cur_len = 0;
  client->cur_frame = g_new (guint8, OPC_MESSAGE_LEN);

  g_io_add_watch (client->gio, G_IO_IN | G_IO_ERR | G_IO_HUP,
                  opc_client_socket_recv, client);

  return client;
}


static gboolean
opc_client_socket_recv (GIOChannel   *source,
                        GIOCondition  condition,
                        gpointer      data)
{
  static gchar dump_buffer[OPC_MESSAGE_LEN];
  OpcClient *client = OPC_CLIENT (data);
  gchar *c;
  ssize_t ret;
  gint total_len = 0;

  if (condition & G_IO_ERR ||
      condition & G_IO_HUP ||
      condition & G_IO_NVAL)
    {
      g_printerr ("GIO error condition\n");
      g_object_unref (client);
      return FALSE;
    }


  /* the following value is only used if we have >= 4 bytes read. */
  total_len = GET_OPC_FRAME_LEN (client->inbuf, client->in_len) + 4;

  /* we need to make sure to not read more than the current opc package */
  if (client->in_len < 3)
    {
      /* opc header (4 bytes) */
      ret = recv (g_io_channel_unix_get_fd (source),
                  client->inbuf + client->in_len,
                  4 - client->in_len,
                  0);
    }
  else if (client->in_len < MIN (total_len, OPC_MESSAGE_LEN))
    {
      ret = recv (g_io_channel_unix_get_fd (source),
                  client->inbuf + client->in_len,
                  MIN (total_len, OPC_MESSAGE_LEN) - client->in_len,
                  0);
    }
  else
    {
      ret = recv (g_io_channel_unix_get_fd (source),
                  dump_buffer,
                  MIN (total_len - OPC_MESSAGE_LEN - client->dump_len,
                       sizeof (dump_buffer)),
                  0);
    }

  if (ret == 0)
    {
      g_printerr ("empty recv - shutting down client %p.\n", client);
      g_object_unref (client);
      return FALSE;
    }

  if (ret < 0)
    {
      g_io_channel_unref (source);
      g_printerr ("got empty request.\n");

      return TRUE;
    }

  if (ret > 0)
    {
      if (client->in_len < OPC_MESSAGE_LEN)
        client->in_len += ret;
      else
        client->dump_len += ret;
    }

  if (client->in_len >= 4)
    {
      total_len = GET_OPC_FRAME_LEN (client->inbuf, client->in_len) + 4;

      if (client->in_len + client->dump_len == total_len)
        {
          guint8 *tmp;
          tmp = client->cur_frame;
          client->cur_frame = client->inbuf;
          client->inbuf = tmp;
          client->cur_len = client->in_len;
          client->in_len = 0;
          client->dump_len = 0;
          client->timestamp = opc_get_current_time ();
          opc_broker_notify_frame (client->broker, client);
        }
    }

  return TRUE;
}


