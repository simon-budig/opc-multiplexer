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

#define CLIENT_RUN_TIME        (20.0)
#define CLIENT_TRANSITION_TIME (2.0)
#define ROUND(x) ((int) ((((x) < 0) ? (x) - 0.5 : (x) + 0.5)))

enum
{
  LAST_SIGNAL
};

// static guint opc_broker_signals[LAST_SIGNAL] = { 0 };

static void      opc_broker_finalize     (GObject  *object);
static gboolean  opc_broker_check_client (gpointer  user_data);

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
opc_broker_new (gint num_pixels)
{
  OpcBroker *broker;

  broker = g_object_new (OPC_TYPE_BROKER, NULL);

  broker->num_pixels = num_pixels;
  broker->outbuf = g_new (guint8, 4 + num_pixels * 3);
  broker->out_len = 0;
  broker->out_pos = 0;

  broker->global_brightness = 1.0;

  return broker;
}


static int
opc_broker_socket_open (OpcBroker    *broker,
                        guint16       portno,
                        GError      **err)
{
  struct addrinfo hints, *res, *reslist;
  int fd, flag, ret;

  memset (&hints, 0, sizeof (hints));
  hints.ai_flags     = AI_PASSIVE;
  hints.ai_family    = AF_UNSPEC;
  hints.ai_socktype  = SOCK_STREAM;

  ret = getaddrinfo (NULL /* Hostname */, "7890" /* port */, &hints, &reslist);

  if (ret <0 )
    {
      fprintf (stderr, "getaddrinfo error: %s\n", gai_strerror (ret));
      return -1;
    }

  for (res = reslist; res; res = res->ai_next)
    {
      /* Check for ipv6 socket, on Linux this also accepts ipv4 */
      if (res->ai_family != AF_INET6)
        continue;

      fd = socket (res->ai_family, res->ai_socktype, res->ai_protocol);

      if (fd >= 0)
        {
          flag = 0;
#ifdef IPV6_V6ONLY
          setsockopt (fd, IPPROTO_IPV6, IPV6_V6ONLY,
                      (void *) &flag, sizeof (flag));
#endif
          flag = 1;
#ifdef SO_REUSEPORT
          setsockopt (fd, SOL_SOCKET, SO_REUSEPORT,
                      (void *) &flag, sizeof (flag));
#endif
          setsockopt (fd, SOL_SOCKET, SO_REUSEADDR,
                      (void *) &flag, sizeof (flag));

          ret = bind (fd, res->ai_addr, res->ai_addrlen);
          if (ret >= 0)
            break;

          close (fd);
          fd = -1;
        }
    }

  freeaddrinfo (reslist);

  if (fd < 0)
    {
      g_set_error_literal (err,
                           OPC_ERROR, 1000,
                           "failed to create opc broker socket");

      return fd;
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

  if (broker->prev_client == (OpcClient *) stale_client)
    broker->prev_client = NULL;

  broker->clients = g_list_remove_all (broker->clients, stale_client);
}


static gboolean
opc_broker_socket_accept (GIOChannel   *source,
                          GIOCondition  condition,
                          gpointer      data)
{
  OpcBroker *broker = data;
  socklen_t len;
  struct sockaddr_in6 sa_client, sa_local;
  OpcClient *client;
  int fd;
  gboolean is_remote = TRUE;

  len = sizeof (sa_client);
  fd = accept (g_io_channel_unix_get_fd (source),
               (struct sockaddr *) &sa_client, &len);

  if (fd < 0)
    return TRUE;   /* accept failed, we need to continue watching though */

  /* check if this is a local client with lower priority */
  len = sizeof (sa_local);
  if (getsockname (fd, (struct sockaddr *) &sa_local, &len) == 0 &&
      memcmp ((char *) &sa_client.sin6_addr,
              (char *) &sa_local.sin6_addr,
              sizeof (struct in6_addr)) == 0)
    {
      is_remote = FALSE;
    }

  client = opc_client_new (broker, is_remote, broker->num_pixels, fd);
  g_printerr ("new %s client: %p\n", is_remote ? "remote" : "local", client);

  g_object_weak_ref (G_OBJECT (client), opc_broker_release_client, broker);

  broker->clients = g_list_append (broker->clients, client);

  opc_broker_check_client (broker);

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


static gboolean
opc_broker_render_frame (void *user_data)
{
  OpcBroker *broker = OPC_BROKER (user_data);
  gdouble now;
  OpcClient *cur, *prev;
  gboolean ret;

  now = opc_get_current_time ();

  cur = broker->cur_client;
  prev = broker->prev_client;

  if (!cur)
    return G_SOURCE_REMOVE;

  if (now - broker->start_time < CLIENT_TRANSITION_TIME)
    {
      gdouble alpha;
      gint i, j;

      broker->out_len = broker->num_pixels * 3 + 4;

      /* copy the channel + command from first client. This is somewhat dodgy */
      broker->outbuf[0] = 0;  /* channel */
      broker->outbuf[1] = 0;  /* command */
      broker->outbuf[2] = (broker->out_len - 4) / 256;
      broker->outbuf[3] = (broker->out_len - 4) % 256;

      alpha = (now - broker->start_time) / CLIENT_TRANSITION_TIME;

      for (i = 0; i < broker->num_pixels; i++)
        {
          for (j = 0; j < 3; j++)
            {
              gdouble val = 0.0;

              if (prev && i < prev->num_pixels)
                {
                  gdouble pa = prev->cur_frame_rgba[i*4 + 3];
                  val += prev->cur_frame_rgba[i*4 + j] * pa * (1.0 - alpha);
                }

              if (i < cur->num_pixels)
                {
                  gdouble na = cur->cur_frame_rgba[i*4 + 3];
                  val += cur->cur_frame_rgba[i*4 + j] * na * alpha;
                }

              broker->outbuf[4 + i*3 + j] = ROUND (CLAMP (val * broker->global_brightness, 0.0, 1.0) * 255.0);
            }
        }

      ret = G_SOURCE_CONTINUE;
    }
  else
    {
      gint i, j;

      broker->out_len = broker->num_pixels * 3 + 4;

      broker->outbuf[0] = 0;  /* channel */
      broker->outbuf[1] = 0;  /* command */
      broker->outbuf[2] = (broker->out_len - 4) / 256;
      broker->outbuf[3] = (broker->out_len - 4) % 256;

      for (i = 0; i < broker->num_pixels; i++)
        {
          for (j = 0; j < 3; j++)
            {
              gdouble val = 0.0;

              if (i < cur->num_pixels)
                {
                  gdouble na = cur->cur_frame_rgba[i*4 + 3];
                  val += cur->cur_frame_rgba[i*4 + j] * na;
                }

              broker->outbuf[4 + i*3 + j] = ROUND (CLAMP (val * broker->global_brightness, 0.0, 1.0) * 255.0);
            }
        }

      ret = G_SOURCE_REMOVE;
    }

  if (!broker->outhandler)
    {
      broker->out_pos = 0;
      broker->outhandler = g_io_add_watch (broker->opc_target, G_IO_OUT,
                                           opc_broker_socket_send, broker);
    }

  if (!ret)
    broker->render_id = 0;

  return ret;
}


void
opc_broker_notify_frame (OpcBroker *broker,
                         OpcClient *client)
{
  if (client != broker->cur_client &&
      client != broker->prev_client)
    {
      return;
    }

  if (!broker->render_id)
    broker->render_id = g_timeout_add (1000 / 60,
                                       opc_broker_render_frame,
                                       broker);
}


gboolean
opc_broker_connect_target (OpcBroker *broker,
                           gchar     *hostport,
                           guint16    default_port)
{
  struct addrinfo *addr, *info;
  gchar *host, *colon;
  gint success = 0;
  gint flag;

  broker->target_fd = -1;

  host = g_strdup (hostport);
  colon = strchr (host, ':');
  /* check for ipv6 */
  if (strrchr (host, ':') != colon)
    colon = NULL;

  if (colon)
    {
      *colon = '\0';
    }

  g_printerr ("host: %s, port: %s\n", *host ? host : "127.0.0.1", colon ? colon + 1 : "15163");

  getaddrinfo (*host ? host : "localhost", colon ? colon + 1 : "15163",
               0, &addr);

  for (info = addr; info; info = info->ai_next)
    {
      broker->target_fd = socket (info->ai_family,
                                  info->ai_socktype,
                                  info->ai_protocol);
      g_printerr ("family: %d, socktype: %d, protocol: %d\n",
                  info->ai_family, info->ai_socktype, info->ai_protocol);

      if (broker->target_fd >= 0)
        {
          if (connect (broker->target_fd,
                       info->ai_addr,
                       info->ai_addrlen) < 0)
            {
              perror ("connect");
              close (broker->target_fd);
              broker->target_fd = -1;
            }
          else
            {
              g_printerr ("connect successful\n");
              break;
            }
        }
    }

  freeaddrinfo (addr);
  g_free (host);

  if (broker->target_fd < 0)
    {
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


static gint
opc_broker_cmp_client (gconstpointer a,
                       gconstpointer b)
{
  OpcClient *ac = OPC_CLIENT (a);
  OpcClient *bc = OPC_CLIENT (b);

  if (ac->num_pixels == 0 || bc->num_pixels == 0)
    {
      return ac->num_pixels - bc->num_pixels;
    }

  if (ac->is_enabled != bc->is_enabled)
    {
      return ac->is_enabled ? -1 : 1;
    }

  if (ac->is_connected != bc->is_connected)
    {
      return ac->is_connected ? -1 : 1;
    }

  /* a preference for remote clients */
  if (ac->is_remote != bc->is_remote)
    {
      return ac->is_remote ? -1 : 1;
    }

  if (ac->last_used < bc->last_used)
    return -1;
  else
    return 1;
}


static gboolean
opc_broker_check_client (gpointer user_data)
{
  OpcBroker *broker = OPC_BROKER (user_data);
  OpcClient *client = NULL;
  GList *l;
  gdouble now;

  now = opc_get_current_time ();

  /* check if the current client still is within its time slot */
  if (now - broker->start_time < CLIENT_RUN_TIME)
    {
      if (broker->cur_client)
        broker->cur_client->last_used = now;

      return TRUE;
    }

  if (broker->prev_client)
    {
      g_object_unref (broker->prev_client);
      broker->prev_client = NULL;
    }

  broker->clients = g_list_sort (broker->clients,
                                 opc_broker_cmp_client);

  /* find a new client */

  client = broker->clients ? broker->clients->data : NULL;

  if (client)
    {
      broker->prev_client = broker->cur_client;
      broker->cur_client = client;
      g_object_ref (broker->cur_client);
      broker->start_time = now;
      broker->cur_client->last_used = now;
    }

  if (broker->cur_client &&
      broker->cur_client->num_pixels > 0)
    {
      if (!broker->render_id)
        broker->render_id = g_timeout_add (1000 / 60,
                                           opc_broker_render_frame,
                                           broker);
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

  broker->client_check_id = g_timeout_add (1000,
                                           opc_broker_check_client,
                                           broker);

  return TRUE;
}


