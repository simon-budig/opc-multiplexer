#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

#include <glib-object.h>

#include "opc-types.h"
#include "opcbroker.h"

#include "pxsource.h"
#include "artnetnode.h"

static gboolean   artnet_node_socket_recv (GIOChannel   *source,
                                           GIOCondition  condition,
                                           gpointer      data);

// bogus
#define ARTNET_MESSAGE_LEN (512*3+12)
#define GET_ARTNET_FRAME_LEN(buf, len) ((len) >= 4 ? (buf)[2] * 256 + (buf)[3] : -1)
enum
{
  LAST_SIGNAL
};

static void    artnet_node_finalize    (GObject    *object);

G_DEFINE_TYPE (ArtnetNode, artnet_node, PX_TYPE_SOURCE)

#define parent_class artnet_node_parent_class


static void
artnet_node_class_init (ArtnetNodeClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = artnet_node_finalize;
}


static void
artnet_node_init (ArtnetNode *client)
{
}


static void
artnet_node_finalize (GObject *object)
{
  ArtnetNode *client = ARTNET_NODE (object);

  if (client->gio != NULL)
    g_io_channel_unref (client->gio);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}


static gint
artnet_node_open_socket (void)
{
  struct addrinfo hints, *res, *reslist;
  int fd, flag, ret;

  memset (&hints, 0, sizeof (hints));
  hints.ai_flags     = AI_PASSIVE;
  hints.ai_family    = AF_UNSPEC;
  hints.ai_socktype  = SOCK_DGRAM;

  ret = getaddrinfo (NULL /* Hostname */, "6454" /* port */, &hints, &reslist);

  if (ret <0 )
    {
      g_printerr ("getaddrinfo error: %s\n", gai_strerror (ret));
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

  return fd;
}


/**
 * artnet_node_new:
 *
 * Returns: A new #ArtnetNode.
 **/
ArtnetNode *
artnet_node_new (OpcBroker *broker,
                 gboolean   is_remote,
                 gint       num_pixels)
{
  PxSource   *pxsource;
  ArtnetNode *client;
  gint fd;

  client = g_object_new (ARTNET_TYPE_NODE, NULL);
  pxsource = PX_SOURCE (client);

  pxsource->broker = broker;
  pxsource->num_pixels = num_pixels;
  pxsource->cur_frame_rgba = g_new0 (gfloat, num_pixels * 4);

  pxsource->is_enabled = TRUE;
  pxsource->is_remote = is_remote;
  pxsource->is_connected = TRUE;

  fd = artnet_node_open_socket ();

  client->gio = g_io_channel_unix_new (fd);
  g_io_channel_set_close_on_unref (client->gio, TRUE);
  client->dump_len = 0;
  client->in_len = 0;
  client->inbuf = g_new (guint8, ARTNET_MESSAGE_LEN);

  g_io_add_watch (client->gio, G_IO_IN | G_IO_ERR | G_IO_HUP,
                  artnet_node_socket_recv, client);

  return client;
}


static gboolean
artnet_node_socket_recv (GIOChannel   *source,
                        GIOCondition  condition,
                        gpointer      data)
{
  static gchar dump_buffer[ARTNET_MESSAGE_LEN];
  ArtnetNode *client = ARTNET_NODE (data);
  gchar *c;
  ssize_t ret;
  gint total_len = 0;

  if (condition & G_IO_ERR ||
      condition & G_IO_HUP ||
      condition & G_IO_NVAL)
    {
      g_printerr ("GIO error condition\n");
      PX_SOURCE (client)->is_connected = FALSE;
      g_object_unref (client);
      return FALSE;
    }

  /* the following value is only used if we have >= 4 bytes read. */
  total_len = GET_ARTNET_FRAME_LEN (client->inbuf, client->in_len) + 4;

  /* we need to make sure to not read more than the current opc package */
  if (client->in_len < 3)
    {
      /* opc header (4 bytes) */
      ret = recv (g_io_channel_unix_get_fd (source),
                  client->inbuf + client->in_len,
                  4 - client->in_len,
                  0);
    }
  else if (client->in_len < MIN (total_len, ARTNET_MESSAGE_LEN))
    {
      ret = recv (g_io_channel_unix_get_fd (source),
                  client->inbuf + client->in_len,
                  MIN (total_len, ARTNET_MESSAGE_LEN) - client->in_len,
                  0);
    }
  else
    {
      ret = recv (g_io_channel_unix_get_fd (source),
                  dump_buffer,
                  MIN (total_len - ARTNET_MESSAGE_LEN - client->dump_len,
                       sizeof (dump_buffer)),
                  0);
    }

  if (ret == 0)
    {
      g_printerr ("empty recv - shutting down client %p.\n", client);
      PX_SOURCE (client)->is_connected = FALSE;
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
      if (client->in_len < ARTNET_MESSAGE_LEN)
        client->in_len += ret;
      else
        client->dump_len += ret;
    }

  if (client->in_len >= 4)
    {
      PxSource *pxsource = PX_SOURCE (client);

      total_len = GET_ARTNET_FRAME_LEN (client->inbuf, client->in_len) + 4;

      if (client->in_len + client->dump_len == total_len)
        {
          gint i;

          for (i = 0; i < pxsource->num_pixels; i++)
            {
              if (i * 3 >= client->in_len - 4)
                break;

              pxsource->cur_frame_rgba[i*4 + 0] = client->inbuf[4 + i*3 + 0] / 255.0f;
              pxsource->cur_frame_rgba[i*4 + 1] = client->inbuf[4 + i*3 + 1] / 255.0f;
              pxsource->cur_frame_rgba[i*4 + 2] = client->inbuf[4 + i*3 + 2] / 255.0f;
              pxsource->cur_frame_rgba[i*4 + 3] = 1.0f;
            }
          client->in_len = 0;
          client->dump_len = 0;

          pxsource->timestamp = opc_get_current_time ();
          opc_broker_notify_frame (pxsource->broker, pxsource);
        }
    }

  return TRUE;
}


