#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <error.h>

#include <glib.h>
#include <glib-object.h>

#include <mosquitto.h>

#include "opc-types.h"

#include "gmqtt-client.h"
#include "opcbroker.h"
#include "pxsource.h"
#include "artnetnode.h"

int verbosity_level = 0;
static GMainLoop *mainloop = NULL;
static gboolean exit_requested = FALSE;

static void
signal_term_handler (int sig)
{
  g_main_loop_quit (mainloop);
  if (sig == SIGTERM)
    exit_requested = TRUE;
}


gdouble
opc_get_current_time ()
{
  struct timespec ts;

  clock_gettime (CLOCK_MONOTONIC_RAW, &ts);
  return (gdouble) ts.tv_sec + (gdouble) ts.tv_nsec / 1.0e9;
}


static void
mqtt_connected_update (GObject    *object,
                       GParamSpec *pspec,
                       gpointer    user_data)
{
  GMqttClient *gmqtt = GMQTT_CLIENT (object);
  gboolean connected;

  g_object_get (object, "connected", &connected, NULL);

  if (connected)
    {
      gmqtt_client_publish (gmqtt, "hasi/lights/balldachin/online", "true", -1);
    }
}


static void
mqtt_message_received_brightness_callback (GMqttClient              *client,
                                           struct mosquitto_message *msg,
                                           gpointer                  user_data)
{
  OpcBroker *broker = OPC_BROKER (user_data);
  gchar *payload = g_new0 (gchar, msg->payloadlen + 1);
  gdouble brightness;

  memcpy (payload, msg->payload, msg->payloadlen);

  brightness = g_ascii_strtod (payload, NULL);
  g_free (payload);

  broker->global_brightness = CLAMP (brightness / 100.0, 0.0, 1.0);

  g_printerr ("mosquitto-message: %s: %s\n", msg->topic, msg->payload);
}


int
main (int   argc,
      char *argv[])
{
  OpcBroker *broker;
  GMqttClient *gmqtt;
  int ret;

  if (argc > 1 && !strcmp (argv[1], "--debug"))
    verbosity_level = 2;

  broker = opc_broker_new (512);

  broker->overlay_pxsource = PX_SOURCE (artnet_node_new (broker, FALSE, 512));

  while (!opc_broker_connect_target (broker,
                                     "127.0.0.1:15163",
                                     15163))
    {
      perror ("can't connect to target, trying again in 5s");
      sleep (5);
    }

  ret = opc_broker_run (broker,
                        7890,
                        NULL);

  if (ret < 0)
    perror ("opc-mpx run");

  gmqtt = gmqtt_client_new_with_will ("balldachin",
                                      "mqtt.hasi", 1883,
                                      "hasi/lights/balldachin/online",
                                      "false", -1);

  g_signal_connect (gmqtt, "notify::connected",
                    G_CALLBACK (mqtt_connected_update),
                    broker);
  g_signal_connect (gmqtt, "message-received::brightness",
                    G_CALLBACK (mqtt_message_received_brightness_callback),
                    broker);

  gmqtt_client_subscribe_full (gmqtt, "brightness",
                               "hasi/lights/balldachin/brightness", QOS_1);

  /* mainloop */

  mainloop = g_main_loop_new (NULL, FALSE);

  signal (SIGTERM, signal_term_handler);
  signal (SIGHUP,  signal_term_handler);

  while (!exit_requested)
    {
      g_main_loop_run (mainloop);

      /* after a signal we land here */
      g_printerr ("signal caught!\n");
    }

  return 0;
}
