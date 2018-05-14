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
#include <unistd.h>

#include <glib-object.h>

#define INET_NTOP(src, dst, size)                                                   \
              inet_ntop ((*((struct sockaddr_storage *) &(src))).ss_family,                                           \
                         (*((struct sockaddr_storage *) &(src))).ss_family == AF_INET ?                               \
                           (void *) &(((struct sockaddr_in *) &(src))->sin_addr) :  \
                           (void *) &(((struct sockaddr_in6 *)&(src))->sin6_addr),  \
                         (dst), (size))


typedef struct _artdmx ArtDMX;
struct _artdmx {
  guint8  header[8];
  guint16 opcode;
  guint16 version;
  guint8  sequence;
  guint8  physical;
  guint16 universe;
  guint16 length;
  guint8  dmxdata[512];
} __attribute__((packed));


#define ARTNET_MESSAGE_LEN (18 + 512 + 1)

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
artnet_node_open_socket (struct sockaddr_storage *addr,
                         struct sockaddr_storage *broadcast,
                         guint8                  *hwaddr)
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
                                                    addr,
                                                    broadcast,
                                                    hwaddr);
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


gint
main (gint argc,
      gchar *argv[])
{
  guchar controller_name[128];
  struct sockaddr_storage addr;
  struct sockaddr_storage broadcast;
  guint8 hwaddr[6];
  gint fd;
  ArtDMX fb[4] = { 0, };
  gint i, j, a;

  fd = artnet_node_open_socket (&addr, &broadcast, hwaddr);
  if (fd >= 0)
    {
      g_printerr ("local IP: %s\n",
                  INET_NTOP (addr, controller_name, sizeof (controller_name)));
      g_printerr ("broadcast: %s\n",
                  INET_NTOP (broadcast, controller_name, sizeof (controller_name)));
      g_printerr ("hwaddr: %02x:%02x:%02x:%02x:%02x:%02x\n\n",
                  hwaddr[0], hwaddr[1], hwaddr[2],
                  hwaddr[3], hwaddr[4], hwaddr[5]);
    }

#if 0
  for (i = 0; i < 4; i++)
    {
      strncpy (fb[i].header, "Art-Net", sizeof (fb[i].header) - 1);
      fb[i].opcode = htole16 (0x5000);
      fb[i].version = htobe16 (14);

      fb[i].sequence = 0;
      fb[i].physical = 0;
      fb[i].universe = htole16 (i + 1);
      fb[i].sequence = a * 4 + i;

      fb[i].length = htobe16 (512);

      for (j = 0; j < 512; j += 4)
        {
          fb[i].dmxdata[j + 0] = 0;
          fb[i].dmxdata[j + 1] = 0;
          fb[i].dmxdata[j + 2] = 0;
          fb[i].dmxdata[j + 3] = (i * 128 + j / 4) / 2;
        }
    }

  for (i = 0; i < 4; i++)
    {
      gint ret;

      ret = sendto (fd, &fb[i], sizeof (fb[i]), 0,
                    (struct sockaddr *) &broadcast,
                    sizeof (broadcast));
      if (ret < 0)
        perror ("sendto");
    }

#else
  for (a = 255; a >= 0; a--)
    {
      for (i = 0; i < 4; i++)
        {
          strncpy (fb[i].header, "Art-Net", sizeof (fb[i].header) - 1);
          fb[i].opcode = htole16 (0x5000);
          fb[i].version = htobe16 (14);

          fb[i].sequence = 0;
          fb[i].physical = 0;
          fb[i].universe = htole16 (i + 1);
          fb[i].sequence = a * 4 + i;

          fb[i].length = htobe16 (i < 3 ? 512 : 8);

          for (j = 0; j < 512; j++)
            {
              fb[i].dmxdata[j] = a;
            }
        }

      for (i = 0; i < 4; i++)
        {
          gint ret;

          ret = sendto (fd, &fb[i], sizeof (fb[i]), 0,
                        (struct sockaddr *) &broadcast,
                        sizeof (broadcast));
          if (ret < 0)
            perror ("sendto");
        }

      usleep (5 * 1000);
    }
#endif

  return 0;
}


