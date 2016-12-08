#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
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
  broker->clients = g_hash_table_new (g_str_hash, g_str_equal);
  broker->ongoing_requests = g_hash_table_new (g_int_hash, g_int_equal);
}


static void
opc_broker_finalize (GObject *object)
{
  OpcBroker *broker = OPC_BROKER (object);

  if (broker->sock_io != NULL)
    g_io_channel_unref (broker->sock_io);
  g_hash_table_unref (broker->clients);
  g_hash_table_unref (broker->ongoing_requests);

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


static gboolean
opc_broker_release_client_helper (gpointer key,
                                  gpointer value,
                                  gpointer user_data)
{
  return value == user_data;
}


static void
opc_broker_release_client (gpointer  user_data,
                           GObject  *stale_client)
{
  OpcBroker *broker = OPC_BROKER (user_data);

  broker->anon_clients = g_list_remove_all (broker->anon_clients, stale_client);

  g_hash_table_foreach_remove (broker->clients,
                               opc_broker_release_client_helper,
                               stale_client);
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

  g_object_weak_ref (G_OBJECT (client), opc_broker_release_client, broker);

  broker->anon_clients = g_list_prepend (broker->anon_clients, client);

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

  return TRUE;
}


