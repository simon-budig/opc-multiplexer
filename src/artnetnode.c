#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <linux/if_packet.h>
#include <linux/wireless.h>
#include <netdb.h>
#include <unistd.h>
#include <error.h>
#include <endian.h>
#include <ifaddrs.h>

#include <glib-object.h>

#include "opc-types.h"
#include "opcbroker.h"

#include "pxsource.h"
#include "artnetnode.h"

#define INET_NTOP(src, dst, size)                                                   \
              inet_ntop ((*((struct sockaddr_storage *) &(src))).ss_family,                                           \
                         (*((struct sockaddr_storage *) &(src))).ss_family == AF_INET ?                               \
                           (void *) &(((struct sockaddr_in *) &(src))->sin_addr) :  \
                           (void *) &(((struct sockaddr_in6 *)&(src))->sin6_addr),  \
                         (dst), (size))

static gboolean   artnet_node_socket_recv (GIOChannel   *source,
                                           GIOCondition  condition,
                                           gpointer      data);

typedef struct _artpollreply ArtPollReply;
struct _artpollreply {
  guint8  header[8];
  guint16 opcode;
  guint32 v4ip;
  guint16 port;
  guint16 version;
  guint8  netswitch;
  guint8  subswitch;
  guint16 oeminfo;
  guint8  ubea;
  guint8  status;
  guint16 estacode;
  guint8  shortname[18];
  guint8  longname[64];
  guint8  nodereport[64];
  guint16 numports;
  guint8  port_types[4];
  guint8  input_status[4];
  guint8  output_status[4];
  guint8  input_switch[4];
  guint8  output_switch[4];
  guint8  swvideo;
  guint8  swmacro;
  guint8  swremote;
  guint8  spare[3];
  guint8  style;
  guint8  mac[6];
  guint32 v4ip_2;
  guint8  bind_index;
  guint8  status_2;
  guint8  filler[26];
} __attribute__((packed));

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
artnet_node_get_network_parameters (gint                     sock_fd,
                                    struct sockaddr_storage *addr,
                                    struct sockaddr_storage *broadcast,
                                    guint8                  *hwaddr)
{
  struct ifaddrs *ifaddrs = NULL, *ifa, *target_if;
  gchar *if_name;

  if (getifaddrs (&ifaddrs) < 0)
    {
      perror ("getifaddrs");
      return -1;
    }

  target_if = NULL;

  /* look up an interface that is
   *   - configured for AF_INET
   *   - is active
   *   - is not a loopback device
   *   - has a valid Broadcast address set
   *
   *   we also check for wireless extensions
   *   and prefer interfaces without them.
   */
  for (ifa = ifaddrs; ifa; ifa = ifa->ifa_next)
    {
      if (ifa->ifa_addr                       &&
          ifa->ifa_addr->sa_family == AF_INET &&
          ifa->ifa_flags & IFF_UP             &&
          !(ifa->ifa_flags & IFF_LOOPBACK)    &&
          ifa->ifa_flags & IFF_BROADCAST)
        {
          /* check if this interface is wireless */
          struct iwreq pwrq;
          memset (&pwrq, 0, sizeof (pwrq));
          strncpy (pwrq.ifr_name, ifa->ifa_name, IFNAMSIZ);

          if (ioctl (sock_fd, SIOCGIWNAME, &pwrq) != -1)
            {
              g_printerr ("interface %s is wireless with protocol %s\n",
                          ifa->ifa_name, pwrq.u.name);

              if (!target_if)
                target_if = ifa;
            }
          else
            {
              g_printerr ("interface %s is not wireless\n",
                          ifa->ifa_name, pwrq.u.name);
              target_if = ifa;

              break;
            }
        }
    }

  if (target_if)
    {
      g_printerr ("choosing %s\n\n", target_if->ifa_name);

      /* look for the MAC-Address (via AF_PACKET interface).
       * Match to target-IF by interface name.
       */
      for (ifa = ifaddrs; ifa; ifa = ifa->ifa_next)
        {
          /* MAC Address... */
          if (ifa->ifa_addr                          &&
              ifa->ifa_addr->sa_family == AF_PACKET  &&
              !strcmp (target_if->ifa_name, ifa->ifa_name))
            {
              memcpy (hwaddr, ((struct sockaddr_ll *) ifa->ifa_addr)->sll_addr, 6);
              break;
            }
        }

      memcpy (addr, target_if->ifa_addr, sizeof (*target_if->ifa_addr));
      memcpy (broadcast, target_if->ifa_broadaddr, sizeof (*target_if->ifa_broadaddr));
      /* set artnet port number 0x1936 */
      ((struct sockaddr_in *) broadcast)->sin_port = htons (6454);
    }

  freeifaddrs (ifaddrs);

  return target_if ? 0 : -1;
}


static gint
artnet_node_open_socket (ArtnetNode *client)
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
      if (res->ai_family != AF_INET)
        continue;

      fd = socket (res->ai_family, res->ai_socktype, res->ai_protocol);

      if (fd >= 0)
        {
          flag = 1;
#ifdef SO_REUSEPORT
          ret = setsockopt (fd, SOL_SOCKET, SO_REUSEPORT,
                            (void *) &flag, sizeof (flag));
          if (ret < 0)
            perror ("setsockopt (SO_REUSEPORT)");
#endif
          ret = setsockopt (fd, SOL_SOCKET, SO_REUSEADDR,
                            (void *) &flag, sizeof (flag));
          if (ret < 0)
            perror ("setsockopt (SO_REUSEADDR)");

          ret = setsockopt (fd, SOL_SOCKET, SO_BROADCAST,
                            (void *) &flag, sizeof (flag));
          if (ret < 0)
            perror ("setsockopt (SO_REUSEADDR)");

          ret = artnet_node_get_network_parameters (fd,
                                                    &client->addr,
                                                    &client->broadcast,
                                                    client->hwaddr);
          if (ret < 0)
            {
              perror ("get_np");
            }

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


static gboolean
artnet_node_watchdog_timeout (gpointer data)
{
  ArtnetNode *client = ARTNET_NODE (data);
  PxSource *pxsource = PX_SOURCE (data);
  gint i;
  gfloat max_alpha = 0;

  for (i = 0; i < pxsource->num_pixels; i++)
    {
      pxsource->cur_frame_rgba[i*4 + 3] = MAX (0.0, pxsource->cur_frame_rgba[i*4 + 3] - 0.015);
      max_alpha = MAX (max_alpha, pxsource->cur_frame_rgba[i*4 + 3]);
    }

  if (client->watchdog_id)
    g_source_remove (client->watchdog_id);

  client->watchdog_id = 0;

  if (max_alpha > 0.001)
    {
      client->watchdog_id = g_timeout_add (20, artnet_node_watchdog_timeout,
                                           client);
    }

  opc_broker_notify_frame (pxsource->broker, pxsource);

  return FALSE;
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
  gchar controller_name[128];

  client = g_object_new (ARTNET_TYPE_NODE, NULL);
  pxsource = PX_SOURCE (client);

  pxsource->broker = broker;
  pxsource->num_pixels = num_pixels;
  pxsource->cur_frame_rgba = g_new0 (gfloat, num_pixels * 4);

  pxsource->is_enabled = TRUE;
  pxsource->is_remote = is_remote;
  pxsource->is_connected = TRUE;

  fd = artnet_node_open_socket (client);
  if (fd >= 0)
    {
      g_printerr ("local IP: %s\n",
                  INET_NTOP (client->addr, controller_name, sizeof (controller_name)));
      g_printerr ("broadcast: %s\n",
                  INET_NTOP (client->broadcast, controller_name, sizeof (controller_name)));
      g_printerr ("hwaddr: %02x:%02x:%02x:%02x:%02x:%02x\n\n",
                  client->hwaddr[0], client->hwaddr[1], client->hwaddr[2],
                  client->hwaddr[3], client->hwaddr[4], client->hwaddr[5]);
    }

  client->gio = g_io_channel_unix_new (fd);
  g_io_channel_set_close_on_unref (client->gio, TRUE);

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
  gint fd, ret;
  ArtPollReply replypkt = { 0, };

  /* build ArtPollReply-Packet */
  strncpy (replypkt.header, "Art-Net", sizeof (replypkt.header) - 1);
  replypkt.opcode     = htole16 (0x2100);
  replypkt.v4ip       = ((struct sockaddr_in *) &client->addr)->sin_addr.s_addr;
  replypkt.port       = htole16 (0x1936);
  replypkt.version    = 0x00;
  replypkt.netswitch  = 0x00;
  replypkt.subswitch  = 0x00;
  replypkt.oeminfo    = htobe16 (0x00);
  replypkt.ubea       = 0x00;
  replypkt.status     = 0xd2;
  replypkt.estacode   = htole16 (0x7ff0);

  strncpy (replypkt.shortname,  "OPC-Multiplexer",
           sizeof (replypkt.shortname) - 1);
  strncpy (replypkt.longname,   "OPC-Multiplexer",
           sizeof (replypkt.longname) - 1);
  strncpy (replypkt.nodereport, "#0001 [1] Artnet-OPC",
           sizeof (replypkt.nodereport) - 1);

  replypkt.numports   = htobe16 (4);
  replypkt.port_types[0] = 0xc0;
  replypkt.port_types[1] = 0xc0;
  replypkt.port_types[2] = 0xc0;
  replypkt.port_types[3] = 0xc0;

  replypkt.input_status[0] = 0x08;
  replypkt.input_status[1] = 0x08;
  replypkt.input_status[2] = 0x08;
  replypkt.input_status[3] = 0x08;

  replypkt.output_status[0] = 0x01;
  replypkt.output_status[1] = 0x01;
  replypkt.output_status[2] = 0x01;
  replypkt.output_status[3] = 0x01;

  replypkt.input_switch[0] = 0x00;
  replypkt.input_switch[1] = 0x00;
  replypkt.input_switch[2] = 0x00;
  replypkt.input_switch[3] = 0x00;

  replypkt.output_switch[0] = 0x01;
  replypkt.output_switch[1] = 0x02;
  replypkt.output_switch[2] = 0x03;
  replypkt.output_switch[3] = 0x04;

  replypkt.swvideo = 0x00;
  replypkt.swmacro = 0x00;
  replypkt.swremote = 0x00;

  replypkt.style = 0x00;

  memcpy (replypkt.mac, client->hwaddr, sizeof (replypkt.mac));

  replypkt.v4ip_2 = replypkt.v4ip;
  replypkt.bind_index = 0;
  replypkt.status_2 = 0x08;

  fd = g_io_channel_unix_get_fd (source);

  ret = sendto (fd, &replypkt, sizeof (replypkt), 0,
                (struct sockaddr *) &client->broadcast,
                sizeof (client->broadcast));
  if (ret < 0)
    perror ("sendto");

  return FALSE;
}


static gboolean
artnet_node_socket_recv (GIOChannel   *source,
                        GIOCondition  condition,
                        gpointer      data)
{
  static guchar inbuf[ARTNET_MESSAGE_LEN];
  ArtnetNode *client = ARTNET_NODE (data);
  gint opcode, protocol;
  gchar *c;
  ssize_t ret;
  struct sockaddr_storage controller_addr;
  gchar controller_name[128];
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
                  inbuf, ARTNET_MESSAGE_LEN, 0,
                  (struct sockaddr *) &controller_addr, &len);
  len = ret;

  if (ret < 0)
    {
      perror ("recvfrom");
      return TRUE;
    }

  if (ret < 12 ||
      strncmp (inbuf, "Art-Net", 8) != 0)
    {
      g_printerr ("invalid Art-Net package\n");

      return TRUE;
    }

  opcode = inbuf[8] + inbuf[9] * 256;

//g_printerr ("got opcode %04x, family %d from %s\n",
//            opcode, controller_addr.ss_family,
//            INET_NTOP (controller_addr, controller_name, sizeof (controller_name)));

  switch (opcode)
    {
      case 0x2000:   /* ArtPoll */
        protocol = inbuf[10] * 256 + inbuf[11];
        g_printerr ("ArtPoll: V%d\n", protocol);
        g_io_add_watch (source, G_IO_OUT, artnet_node_send_poll_reply, client);
        break;

      case 0x2100:   /* ArtPollReply */
        g_printerr ("ArtPollReply\n");
        /* ignore */
        break;

      case 0x5000:   /* ArtDMX */
        protocol = inbuf[10] * 256 + inbuf[11];

        if (protocol == 14 && len >= 18)
          {
            PxSource *pxsource = PX_SOURCE (client);
            gint n_channels = inbuf[17] + inbuf[16] * 256;
            gint i;
            gint universe;
            guint8 seqno;
            gint px_idx, dmx_idx;

            universe = inbuf[14] + inbuf[15] * 256;
            // g_printerr ("universe %d, n_channels: %d\n", universe, n_channels);

            seqno = inbuf[12];

            if (opc_get_current_time () - pxsource->timestamp > 5.0)
              client->last_seqno = 0;

         // if (seqno != 0 &&
         //     client->last_seqno != 0 &&
         //     (gint) seqno - client->last_seqno < 0)
         //   {
         //     break;
         //   }

            if (n_channels <= 0 ||
                len < 18 + n_channels)
              break;

            client->last_seqno = seqno;

#if 0
            /* RGBA-Mode
             *
             * each universe has 128 LEDs with 4 RGBA channels each
             */

            /* DMX data starts at byte 18. */
            cur_idx = 18;
            for (i = universe * 128; i < pxsource->num_pixels; i++)
              {
                if (cur_idx + 3 > n_channels)
                  break;

                pxsource->cur_frame_rgba[i*4 + 0] = inbuf[cur_idx + 0] / 255.0f;
                pxsource->cur_frame_rgba[i*4 + 1] = inbuf[cur_idx + 1] / 255.0f;
                pxsource->cur_frame_rgba[i*4 + 2] = inbuf[cur_idx + 2] / 255.0f;
                pxsource->cur_frame_rgba[i*4 + 3] = inbuf[cur_idx + 3] / 255.0f;

                cur_idx += 4;
              }
#else
            /* Party-Mode
             * Universe 1: 0-509: 170 RGB lights
             * Universe 2: 0-509: 170 RGB lights
             * Universe 3: 0-509: 170 RGB lights
             * Universe 4: 0-5:     2 RGB lights
             *             6: global alpha
             */

            switch (universe)
              {
                case 1:
                  if (n_channels > 510)
                    {
                      for (px_idx = 0; px_idx < pxsource->num_pixels; px_idx++)
                        {
                          pxsource->cur_frame_rgba[px_idx*4 + 3] = inbuf[18 + 510] / 255.0f;
                        }
                    }

                case 2:
                case 3:
                  dmx_idx = 0;
                  px_idx = (universe - 1) * 170;

                  while (1)
                    {
                      if (dmx_idx + 2 >= n_channels ||
                          px_idx >= pxsource->num_pixels)
                        break;

                      pxsource->cur_frame_rgba[px_idx*4 + 0] = inbuf[18 + dmx_idx + 0] / 255.0f;
                      pxsource->cur_frame_rgba[px_idx*4 + 1] = inbuf[18 + dmx_idx + 1] / 255.0f;
                      pxsource->cur_frame_rgba[px_idx*4 + 2] = inbuf[18 + dmx_idx + 2] / 255.0f;
                      /* alpha untouched */

                      dmx_idx += 3;
                      px_idx += 1;
                    }
                  break;

                case 4:
                  dmx_idx = 0;
                  px_idx = (universe - 1) * 170;

                  while (1)
                    {
                      if (dmx_idx + 2 >= n_channels ||
                          px_idx >= pxsource->num_pixels)
                        break;

                      pxsource->cur_frame_rgba[px_idx*4 + 0] = inbuf[18 + dmx_idx + 0] / 255.0f;
                      pxsource->cur_frame_rgba[px_idx*4 + 1] = inbuf[18 + dmx_idx + 1] / 255.0f;
                      pxsource->cur_frame_rgba[px_idx*4 + 2] = inbuf[18 + dmx_idx + 2] / 255.0f;
                      /* alpha untouched */

                      dmx_idx += 3;
                      px_idx += 1;
                    }

                  break;

                default:
                  break;
              }
#endif

            if (client->watchdog_id)
              g_source_remove (client->watchdog_id);
            client->watchdog_id = g_timeout_add (5555, artnet_node_watchdog_timeout,
                                                 client);
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


