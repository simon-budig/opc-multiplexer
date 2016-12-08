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

#define OPC_MESSAGE_LEN (4 + 512 * 3 + 1)

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

  g_string_free (client->inbuf, TRUE);

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
  client->inbuf = g_string_new (NULL);

  g_io_add_watch (client->gio, G_IO_IN | G_IO_ERR | G_IO_HUP,
                  opc_client_socket_recv, client);

  return client;
}


static gboolean
opc_client_dispatch_string (OpcClient *client)
{
  gsize len;
  gchar *req_message;

  g_return_val_if_fail (OPC_IS_CLIENT (client), FALSE);

  /* look for the '\0' byte */
  len = strlen (client->inbuf->str);

  g_warn_if_fail (len < client->inbuf->len);

  req_message = g_strdup (client->inbuf->str);
  g_string_erase (client->inbuf, 0, (gssize) len + 1);

  g_printerr ("dispatching: \"%s\"\n", req_message);

  g_free (req_message);

#warning hier!

  return FALSE;
}


static gboolean
opc_client_socket_recv (GIOChannel   *source,
                        GIOCondition  condition,
                        gpointer      data)
{
  static gchar msgbuf[OPC_MESSAGE_LEN] = { 0, };
  OpcClient *client = OPC_CLIENT (data);
  gchar *c;
  ssize_t ret;

  if (condition & G_IO_ERR ||
      condition & G_IO_HUP ||
      condition & G_IO_NVAL)
    {
      g_printerr ("GIO error condition\n");
      g_object_unref (client);
      return FALSE;
    }

  ret = recv (g_io_channel_unix_get_fd (source),
              msgbuf, OPC_MESSAGE_LEN - 1, 0);

  if (ret == 0)
    {
      g_printerr ("empty recv - shutting down client.\n");
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
      g_string_append_len (client->inbuf, msgbuf, ret);

      /* look for a \0, most likely at the end of the transferred bytes */
      for (c = msgbuf + ret - 1; c >= msgbuf; c--)
        {
          if (*c == '\0')
            {
              /* parse and dispatch inbuf message */
              opc_client_dispatch_string (client);
            }
        }
    }

  return TRUE;
}


