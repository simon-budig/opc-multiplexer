#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

#include <unistd.h>
#include <ctype.h>

#include <glib-object.h>

#include "opc-types.h"

#include "opcclient.h"
#include "opcbroker.h"

#define OPC_ERROR      (g_quark_from_static_string  ("opc-error-quark"))

enum
{
  LAST_SIGNAL
};

// static guint opc_broker_signals[LAST_SIGNAL] = { 0 };

static void    opc_broker_finalize    (GObject    *object);

G_DEFINE_TYPE (OpcBroker, opc_broker, G_TYPE_OBJECT)

#define parent_class opc_broker_parent_class


static void
opc_broker_class_init (OpcBrokerClass *klass)
{
  GObjectClass *object_class     = G_OBJECT_CLASS (klass);

  object_class->finalize = opc_broker_finalize;
}


static void
opc_broker_init (OpcBroker *broker)
{
  broker->clients = NULL;
}


static void
opc_broker_finalize (GObject *object)
{
  OpcBroker *broker = OPC_BROKER (object);

  if (broker->sock_io != NULL)
    g_io_channel_unref (broker->sock_io);
  g_list_free (broker->clients);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}


/**
 * opc_broker_new:
 *
 * Returns: A new #OpcBroker.
 **/
OpcBroker *
opc_broker_new (void)
{
  OpcBroker *broker;

  broker = g_object_new (OPC_TYPE_BROKER, NULL);

  broker->outbuf = g_new (guint8, 4 + 512 * 3);
  broker->out_len = 0;
  broker->out_pos = 0;

  return broker;
}


static int
opc_broker_socket_open (OpcBroker    *broker,
                        guint16       portno,
                        GError      **err)
{
  struct sockaddr_in sa;
  int fd, flag, ret;

  memset (&sa, 0, sizeof (sa));
  sa.sin_family      = AF_INET;
  sa.sin_port        = htons(portno);
  sa.sin_addr.s_addr = INADDR_ANY;

  fd = socket (AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    {
      g_set_error_literal (err,
                           OPC_ERROR, 1000,
                           "failed to create opc broker socket");

      return fd;
    }

  flag = 1;
#ifdef SO_REUSEPORT
  setsockopt (fd, SOL_SOCKET, SO_REUSEPORT, (char*) &flag, sizeof (flag));
#endif
  setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, (char*) &flag, sizeof (flag));

  ret = bind (fd, (struct sockaddr *) &sa, sizeof (sa));
  if (ret < 0)
    {
      g_set_error_literal (err,
                           OPC_ERROR, 1000,
                           "failed to bind to opc broker socket");
      close (fd);
      return ret;
    }

  ret = listen (fd, 20);
  if (ret < 0)
    {
      g_set_error_literal (err,
                           OPC_ERROR, 1000,
                           "failed to listen to opc broker socket");
      close (fd);
      return ret;
    }

  broker->sock_io = g_io_channel_unix_new (fd);
  g_io_channel_set_close_on_unref (broker->sock_io, TRUE);

  return fd;
}


static void
opc_broker_release_client (gpointer  user_data,
                           GObject  *stale_client)
{
  OpcBroker *broker = OPC_BROKER (user_data);

  g_printerr ("releasing client %p\n", stale_client);

  if (broker->cur_client == (OpcClient *) stale_client)
    broker->cur_client = NULL;

  if (broker->next_client == (OpcClient *) stale_client)
    broker->next_client = NULL;

  broker->clients = g_list_remove_all (broker->clients, stale_client);
}


static gboolean
opc_broker_socket_accept (GIOChannel   *source,
                          GIOCondition  condition,
                          gpointer      data)
{
  OpcBroker *broker = data;
  socklen_t len;
  struct sockaddr_in sa;
  OpcClient *client;
  int fd;

  len = sizeof (sa);
  fd = accept (g_io_channel_unix_get_fd (source),
               (struct sockaddr *) &sa, &len);

  if (fd < 0)
    return TRUE;   /* accept failed, we need to continue watching though */

  client = opc_client_new (broker, fd);
  g_printerr ("new client: %p\n", client);

  g_object_weak_ref (G_OBJECT (client), opc_broker_release_client, broker);

  broker->clients = g_list_append (broker->clients, client);

  if (!broker->cur_client)
    {
      broker->cur_client = broker->clients ? broker->clients->data : NULL;
    }

  return TRUE;
}


static gboolean
opc_broker_socket_send (GIOChannel   *source,
                        GIOCondition  condition,
                        gpointer      data)
{
  OpcBroker *broker = OPC_BROKER (data);
  gssize ret;

  g_return_val_if_fail (broker->opc_target == source, FALSE);

  if (broker->out_pos < broker->out_len)
    {
      ret = send (g_io_channel_unix_get_fd (broker->opc_target),
                  broker->outbuf + broker->out_pos,
                  broker->out_len - broker->out_pos, 0);
      if (ret < 0)
        {
          perror ("opc send() failed");
          exit (1);
        }
      else
        {
          broker->out_pos += ret;
        }
    }

  if (broker->out_pos == broker->out_len)
    {
      broker->outhandler = 0;
      return FALSE;
    }

  return TRUE;
}



void
opc_broker_notify_frame (OpcBroker *broker,
                         OpcClient *client)
{
  if (broker->cur_client != client)
    return;

  if (!broker->outhandler)
    {
      broker->out_len = MIN (client->cur_len, 4 + 3 * 512);
      broker->out_pos = 0;
      memcpy (broker->outbuf, client->cur_frame, broker->out_len);
      broker->outbuf[2] = (broker->out_len - 4) / 256;
      broker->outbuf[3] = (broker->out_len - 4) % 256;
      broker->outhandler = g_io_add_watch (broker->opc_target, G_IO_OUT,
                                           opc_broker_socket_send, broker);
    }
  else
    {
      broker->out_needed = TRUE;
    }
}


gboolean
opc_broker_connect_target (OpcBroker *broker,
                           gchar     *hostport,
                           guint16    default_port)
{
  gchar *host, *colon;
  gint port;
  gint success = 0;
  gint flag;

  broker->target_fd = -1;

  host = g_strdup (hostport);
  colon = strchr (host, ':');
  port = default_port;

  if (colon)
    {
      *colon = '\0';
      port = g_ascii_strtoll (colon + 1, NULL, 10);
    }

  if (port)
    {
      struct addrinfo *addr, *i;
      getaddrinfo (*host ? host : "localhost", 0, 0, &addr);

      for (i = addr; i; i = i->ai_next)
        {
          if (i->ai_family == PF_INET)
            {
              memcpy (&broker->target_address,
                      i->ai_addr, sizeof (broker->target_address));
              broker->target_address.sin_port = htons (port);
              success = 1;
              break;
            }
        }

      freeaddrinfo (addr);
    }

  g_free (host);

  if (!success)
    return FALSE;

  broker->target_fd = socket (PF_INET, SOCK_STREAM, IPPROTO_TCP);

  if (connect (broker->target_fd,
               (struct sockaddr *) &broker->target_address,
               sizeof (broker->target_address)) < 0)
    {
      close (broker->target_fd);
      g_printerr ("connect failed\n");
      return FALSE;
    }

  flag = 1;
  setsockopt (broker->target_fd,
              IPPROTO_TCP,
              TCP_NODELAY,
              (char *) &flag,
              sizeof (flag));

  signal (SIGPIPE, SIG_IGN);

  broker->opc_target = g_io_channel_unix_new (broker->target_fd);
  g_io_channel_set_close_on_unref (broker->opc_target, TRUE);

  return TRUE;
}


static gboolean
opc_broker_next_client (gpointer user_data)
{
  OpcBroker *broker = OPC_BROKER (user_data);

  g_printerr ("currently %d clients\n", g_list_length (broker->clients));

  broker->clients = g_list_remove_all (broker->clients,
                                       broker->cur_client);
  broker->clients = g_list_append (broker->clients,
                                   broker->cur_client);

  broker->cur_client = broker->clients ? broker->clients->data : NULL;

  while (broker->cur_client &&
         broker->cur_client->cur_len == 0)
    {
      broker->clients = g_list_remove_all (broker->clients,
                                           broker->cur_client);
      broker->clients = g_list_append (broker->clients,
                                       broker->cur_client);

      broker->cur_client = broker->clients ? broker->clients->data : NULL;
    }

  if (broker->cur_client &&
      broker->cur_client->cur_len > 0)
    {
      opc_broker_notify_frame (broker, broker->cur_client);
    }

  return TRUE;
}


gboolean
opc_broker_run (OpcBroker    *broker,
                guint16       portno,
                GError      **err)
{
  g_return_val_if_fail (OPC_IS_BROKER (broker), FALSE);
  g_return_val_if_fail (err == NULL || *err == NULL, FALSE);

  if (opc_broker_socket_open (broker, portno, err) < 0)
    return FALSE;

  g_io_add_watch (broker->sock_io,
                  G_IO_IN | G_IO_ERR | G_IO_HUP,
                  opc_broker_socket_accept, broker);

  broker->timeout_id = g_timeout_add (10000, opc_broker_next_client, broker);

  return TRUE;
}


