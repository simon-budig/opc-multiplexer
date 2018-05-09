#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <error.h>

#include <glib-object.h>

#include "opc-types.h"
#include "opcbroker.h"

#include "pxsource.h"
#include "artnetnode.h"

static gboolean   artnet_node_socket_recv (GIOChannel   *source,
                                           GIOCondition  condition,
                                           gpointer      data);

#define ARTNET_MESSAGE_LEN (18 + 512 + 1)

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

  ret = getaddrinfo (NULL, /* Hostname */
                     "6454" /* port */,
                     &hints, &reslist);

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

          setsockopt (fd, SOL_SOCKET, SO_BROADCAST,
                      (void *) &flag, sizeof (flag));

          flag = 0;
          setsockopt (fd, IPPROTO_IP, IP_MULTICAST_LOOP,
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
  client->inbuf = g_new0 (guint8, ARTNET_MESSAGE_LEN);

  g_io_add_watch (client->gio, G_IO_IN | G_IO_ERR | G_IO_HUP,
                  artnet_node_socket_recv, client);

  return client;
}


static gboolean
artnet_node_send_poll_reply (GIOChannel   *source,
                             GIOCondition  condition,
                             gpointer      data)
{
  ArtnetNode *client = ARTNET_NODE (data);
  struct sockaddr_in dest_addr;
  gint fd, ret;
  guint8 pollreply[] =
    "Art-Net\x00"       // header
    "\x00\x21"          // opcode 0x2100
    "\xc0\xa8\x02\xb7"  // IP-Addr.
    "\x36\x19"          // PortNr. 0x1936
    "\x00\x00\x00\x00"  // Version, NetSwitch, SubSwitch
    "\x00\x00"          // OEM-Info
    "\x00"              // UBEA-Version
    "\xd2"              // Status-Code
    "\xf0\x7f"          // ESTA-Code

    // short name
    "OPC-Multiplexer\x00\x00\x00"

    // long name
    "OPC-Multiplexer\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"

    // node status report
    "\x23\x30\x30\x30\x31\x20\x5b\x31\x5d\x20\x4f\x4c\x41\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"

    "\x00\x04"           // Number of Ports
    "\xc0\xc0\xc0\xc0"   // Port types

    "\x00\x00\x00\x00"   // Input Status
    "\x00\x00\x00\x00"   // Output Status

    "\x01\x02\x03\x04"   // Input SubSwitch
    "\x00\x00\x00\x00"   // Output SubSwitch
    "\x00\x00\x00\x00\x00\x00"  // SwVideo, SwMacro, SwRemote, spare[3]...
    "\x00"                      // Style
    "\x18\x5e\x0f\x20\x64\x3a"  // MAC
    "\xc0\xa8\x02\xb7"          // IP
    "\x00"                      // Bind-Index
    "\x08"                      // Status
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00";

  fd = g_io_channel_unix_get_fd (source);

  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons (6454);
  dest_addr.sin_addr.s_addr = htonl (0xc0a802ff);
  ret = sendto (fd, pollreply, sizeof (pollreply) - 1, 0,
                &dest_addr, sizeof (dest_addr));
  if (ret < 0)
    perror ("sendto");

  return FALSE;
}


static gboolean
artnet_node_socket_recv (GIOChannel   *source,
                        GIOCondition  condition,
                        gpointer      data)
{
  ArtnetNode *client = ARTNET_NODE (data);
  gint opcode, protocol;
  gchar *c;
  ssize_t ret;
  struct sockaddr controller_addr;
  socklen_t len;

  if (condition & G_IO_ERR ||
      condition & G_IO_HUP ||
      condition & G_IO_NVAL)
    {
      g_printerr ("GIO error condition\n");
      PX_SOURCE (client)->is_connected = FALSE;
      g_object_unref (client);
      return FALSE;
    }

  len = sizeof (controller_addr);
  ret = recvfrom (g_io_channel_unix_get_fd (source),
                  client->inbuf, ARTNET_MESSAGE_LEN, 0,
                  &controller_addr, &len);
  len = ret;

  if (ret < 0)
    {
      perror ("recvfrom");
      return TRUE;
    }

  if (ret < 0)
    {
      g_io_channel_unref (source);
      g_printerr ("got empty request.\n");

      return TRUE;
    }

  if (ret < 12 ||
      strncmp (client->inbuf, "Art-Net", 8) != 0)
    {
      g_printerr ("invalid Art-Net package\n");

      return TRUE;
    }

  opcode = client->inbuf[8] + client->inbuf[9] * 256;


  switch (opcode)
    {
      case 0x2000:   /* ArtPoll */
        protocol = client->inbuf[10] * 256 + client->inbuf[11];
        g_printerr ("ArtPoll: V%d\n", protocol);
        g_io_add_watch (source, G_IO_OUT, artnet_node_send_poll_reply, client);
        break;

      case 0x2100:   /* ArtPollReply */
        g_printerr ("ArtPollReply\n");
        /* ignore */
        break;

      case 0x5000:   /* ArtDMX */
        protocol = client->inbuf[10] * 256 + client->inbuf[11];
        g_printerr ("ArtDMX: V%d\n", protocol);

        if (protocol == 14 && len >= 18)
          {
            PxSource *pxsource = PX_SOURCE (client);
            gint n_channels = client->inbuf[17] + client->inbuf[16] * 256;
            gint i;
            gint universe;

            universe = client->inbuf[14] + client->inbuf[15] * 256;
            g_printerr ("universe %d, n_channels: %d\n", universe, n_channels);

            if (n_channels <= 0 ||
                len < 18 + n_channels)
              break;

            for (i = 0; i < pxsource->num_pixels; i++)
              {
                if (i * 4 + 3 > n_channels)
                  break;

                pxsource->cur_frame_rgba[i*4 + 0] = client->inbuf[18 + i*4 + 0] / 255.0f;
                pxsource->cur_frame_rgba[i*4 + 1] = client->inbuf[18 + i*4 + 1] / 255.0f;
                pxsource->cur_frame_rgba[i*4 + 2] = client->inbuf[18 + i*4 + 2] / 255.0f;
                pxsource->cur_frame_rgba[i*4 + 3] = client->inbuf[18 + i*4 + 3] / 255.0f;
              }

            pxsource->timestamp = opc_get_current_time ();
            opc_broker_notify_frame (pxsource->broker, pxsource);
          }

        break;

      default:
        g_printerr ("ignoring Art-Net Opcode %04x\n", opcode);
        break;
    }

  return TRUE;
}


