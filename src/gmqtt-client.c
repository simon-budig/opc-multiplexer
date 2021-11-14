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

#include <mosquitto.h>

#include <glib-object.h>

#include "gmqtt-source.h"

#include "opc-types.h"
#include "gmqtt-client.h"

#define GMQTT_ERROR  (g_quark_from_static_string  ("gmqtt-error-quark"))

enum
{
  PROP_0,
  PROP_NAME,
  PROP_SERVER,
  PROP_PORT,
  PROP_DEFAULT_QOS,
  PROP_CONNECTED,
};

enum
{
  MESSAGE_RECEIVED,
  LAST_SIGNAL
};

typedef struct _gmqtt_subscription GMqttSubscription;
struct _gmqtt_subscription
{
  gchar  *pattern;
  GQuark  quark;
  gint    qos;
};

static guint gmqtt_client_signals[LAST_SIGNAL] = { 0 };

static void      gmqtt_client_set_property  (GObject      *object,
                                             guint         property_id,
                                             const GValue *value,
                                             GParamSpec   *pspec);
static void      gmqtt_client_get_property  (GObject      *object,
                                             guint         property_id,
                                             GValue       *value,
                                             GParamSpec   *pspec);
static void      gmqtt_client_finalize      (GObject  *object);
static void      gmqtt_client_on_message    (struct mosquitto *mosq,
                                             gpointer          user_data,
                                             const struct mosquitto_message *msg);
static void      gmqtt_client_on_connect    (struct mosquitto *mosq,
                                             gpointer          user_data,
                                             gint              rc);
static void      gmqtt_client_on_disconnect (struct mosquitto *mosq,
                                             gpointer          user_data,
                                             gint              rc);


G_DEFINE_TYPE (GMqttClient, gmqtt_client, G_TYPE_OBJECT)

#define parent_class gmqtt_client_parent_class


static void
gmqtt_client_class_init (GMqttClientClass *klass)
{
  GObjectClass *object_class     = G_OBJECT_CLASS (klass);

  mosquitto_lib_init ();

  gmqtt_client_signals[MESSAGE_RECEIVED] =
    g_signal_new ("message-received",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST | G_SIGNAL_DETAILED,
                  G_STRUCT_OFFSET (GMqttClientClass, message_received),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__POINTER,
                  G_TYPE_NONE, 1,
                  G_TYPE_POINTER);

  object_class->set_property = gmqtt_client_set_property;
  object_class->get_property = gmqtt_client_get_property;
  object_class->finalize     = gmqtt_client_finalize;

  g_object_class_install_property (object_class, PROP_NAME,
                                   g_param_spec_string ("name",
                                                        NULL, NULL,
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class, PROP_SERVER,
                                   g_param_spec_string ("server",
                                                        NULL, NULL,
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class, PROP_PORT,
                                   g_param_spec_int ("port",
                                                     NULL, NULL,
                                                     0, 65535,
                                                     1883,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (object_class, PROP_DEFAULT_QOS,
                                   g_param_spec_int ("default-qos",
                                                     NULL, NULL,
                                                     0, 2,
                                                     0,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_CONSTRUCT));

  g_object_class_install_property (object_class, PROP_CONNECTED,
                                   g_param_spec_boolean ("connected",
                                                         NULL, NULL,
                                                         FALSE,
                                                         G_PARAM_READABLE));
}


static void
gmqtt_client_init (GMqttClient *client)
{
}


static void
gmqtt_client_finalize (GObject *object)
{
  GMqttClient *client = GMQTT_CLIENT (object);

  mosquitto_destroy (client->mosq);
  g_source_unref (client->mosq_src);
  g_free (client->name);
  g_free (client->server);

  while (client->subscriptions)
    {
      GList *l = client->subscriptions;
      GMqttSubscription *sub = l->data;

      client->subscriptions = g_list_next (l);
      g_free (sub->pattern);
      g_free (sub);
      g_free (l);
    }

  G_OBJECT_CLASS (parent_class)->finalize (object);
}


static void
gmqtt_client_set_property (GObject      *object,
                           guint         property_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  GMqttClient *client = GMQTT_CLIENT (object);
  const gchar *name = NULL, *server = NULL;
  gint port, qos;
  gboolean connected;

  switch (property_id)
    {
      case PROP_NAME:
        name = g_value_get_string (value);

        if (!g_strcmp0 (client->name, name))
          return;

        g_free (client->name);

        client->name = g_strdup (name);
        g_object_notify (object, "name");
        break;

      case PROP_SERVER:
        server = g_value_get_string (value);

        if (!g_strcmp0 (client->server, server))
          return;

        g_free (client->server);

        client->server = g_strdup (server);
        g_object_notify (object, "server");
        break;

      case PROP_PORT:
        port = g_value_get_int (value);

        if (port == client->port)
          return;

        client->port = port;
        g_object_notify (object, "port");
        break;

      case PROP_DEFAULT_QOS:
        qos = g_value_get_int (value);

        if (qos == client->default_qos)
          return;

        client->default_qos = qos;
        g_object_notify (object, "default-qos");
        break;

      case PROP_CONNECTED:
        connected = g_value_get_boolean (value);
        g_printerr ("CONNECGTED\n");

        if (connected == client->connected)
          return;

        client->connected = connected;
        g_object_notify (object, "connected");
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}


static void
gmqtt_client_get_property (GObject    *object,
                           guint       property_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  GMqttClient *client = GMQTT_CLIENT (object);

  switch (property_id)
    {
      case PROP_NAME:
        g_value_set_string (value, client->name);
        break;

      case PROP_SERVER:
        g_value_set_string (value, client->server);
        break;

      case PROP_PORT:
        g_value_set_int (value, client->port);
        break;

      case PROP_DEFAULT_QOS:
        g_value_set_int (value, client->default_qos);
        break;

      case PROP_CONNECTED:
        g_value_set_boolean (value, client->connected);
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}


/**
 * gmqtt_client_new:
 *
 * Returns: A new #GMqttClient.
 **/
GMqttClient *
gmqtt_client_new (gchar *name,
                  gchar *server,
                  gint   port)
{
  return gmqtt_client_new_with_will (name, server, port,
                                     NULL, NULL, 0);
}


/**
 * gmqtt_client_new:
 *
 * Returns: A new #GMqttClient.
 **/
GMqttClient *
gmqtt_client_new_with_will (gchar  *name,
                            gchar  *server,
                            gint    port,
                            gchar  *will_topic,
                            gchar  *will_payload,
                            gssize  will_payload_len)
{
  GMqttClient *client;

  client = g_object_new (GMQTT_TYPE_CLIENT,
                         "name",   name,
                         "server", server,
                         "port",   port,
                         NULL);

  client->mosq = mosquitto_new (client->name, TRUE, client);
  if (will_topic)
    {
      if (will_payload == NULL)
        will_payload_len = 0;

      if (will_payload_len < 0)
        will_payload_len = (gssize) strlen (will_payload);

      mosquitto_will_set (client->mosq,
                          will_topic, (gint) will_payload_len, will_payload,
                          QOS_1, TRUE);
    }

  mosquitto_message_callback_set (client->mosq, gmqtt_client_on_message);
  mosquitto_connect_callback_set (client->mosq, gmqtt_client_on_connect);
  mosquitto_disconnect_callback_set (client->mosq, gmqtt_client_on_disconnect);
  mosquitto_connect_async (client->mosq, client->server, client->port, 15);

  client->mosq_src = gmqtt_source_new (client->mosq);
  g_source_attach (client->mosq_src, NULL);

  return client;
}


static void
gmqtt_client_on_message (struct mosquitto               *mosq,
                         gpointer                        user_data,
                         const struct mosquitto_message *msg)
{
  GMqttClient *client = GMQTT_CLIENT (user_data);
  GList *l;

  for (l = client->subscriptions; l; l = g_list_next (l))
    {
      GMqttSubscription *sub = l->data;
      bool match;

      if (mosquitto_topic_matches_sub (sub->pattern,
                                       msg->topic,
                                       &match) == MOSQ_ERR_SUCCESS)
        {
          if (match)
            {
              g_signal_emit (client,
                             gmqtt_client_signals[MESSAGE_RECEIVED],
                             sub->quark,
                             msg);
            }
        }
    }
}


static gboolean
gmqtt_client_reconnect (gpointer user_data)
{
  GMqttClient *client = GMQTT_CLIENT (user_data);

  g_printerr ("gmqtt: attempting reconnect\n");
  mosquitto_reconnect_async (client->mosq);

  return G_SOURCE_CONTINUE;
}


static void
gmqtt_client_on_connect (struct mosquitto *mosq,
                         gpointer          user_data,
                         gint              rc)
{
  GMqttClient *client = GMQTT_CLIENT (user_data);

  g_printerr ("gmqtt: connect (%d)\n", rc);
  if (rc == 0)
    {
      GList *l;

      /* restore subscriptions */
      for (l = client->subscriptions; l; l = g_list_next (l))
        {
          GMqttSubscription *sub = l->data;
          gint res;

          if ((res = mosquitto_subscribe (client->mosq,
                                          NULL,
                                          sub->pattern,
                                          sub->qos)) != MOSQ_ERR_SUCCESS)
            {
              g_warning ("re-subscription to %s failed (%s) \n",
                         sub->pattern, mosquitto_strerror (res));
            }
        }

      client->connected = TRUE;
      g_object_notify (G_OBJECT (client), "connected");
    }

  if (rc == 0 && client->reconnect_timeout != 0)
    {
      g_source_remove (client->reconnect_timeout);
      client->reconnect_timeout = 0;
    }
}


static void
gmqtt_client_on_disconnect (struct mosquitto *mosq,
                            gpointer          user_data,
                            gint              rc)
{
  GMqttClient *client = GMQTT_CLIENT (user_data);

  g_printerr ("gmqtt: disconnect (%d)\n", rc);
  if (client->reconnect_timeout == 0)
    {
      client->reconnect_timeout = g_timeout_add (5 * 1000, gmqtt_client_reconnect, client);

      client->connected = FALSE;
      g_object_notify (G_OBJECT (client), "connected");
    }
}


gboolean
gmqtt_client_subscribe (GMqttClient *client,
                        gchar       *sub_id,
                        gchar       *subscription)
{
  return gmqtt_client_subscribe_full (client, sub_id, subscription,
                                      client->default_qos);
}


gboolean
gmqtt_client_subscribe_full (GMqttClient *client,
                             gchar       *sub_id,
                             gchar       *subscription,
                             gint         qos)
{
  GMqttSubscription *sub;
  gint res;

  if ((res = mosquitto_subscribe (client->mosq,
                                  NULL,
                                  subscription,
                                  qos)) != MOSQ_ERR_SUCCESS)
    {
      g_warning ("subscription to %s failed (%s) \n",
                 subscription, mosquitto_strerror (res));

      return FALSE;
    }

  sub = g_new0 (GMqttSubscription, 1);
  sub->pattern = g_strdup (subscription);
  sub->quark = g_quark_from_string (sub_id);
  sub->qos = qos;

  client->subscriptions = g_list_append (client->subscriptions, sub);

  return TRUE;
}


gboolean
gmqtt_client_publish (GMqttClient *client,
                      gchar       *topic,
                      gchar       *payload,
                      gint         payload_len)
{
  return gmqtt_client_publish_full (client, topic, payload, payload_len,
                                    client->default_qos, FALSE);
}


gboolean
gmqtt_client_publish_full (GMqttClient *client,
                           gchar       *topic,
                           gchar       *payload,
                           gint         payload_len,
                           gint         qos,
                           gboolean     retain)
{
  gint res;

  if (payload_len < 0)
    payload_len = payload ? (gint) strlen (payload) : 0;

  if ((res = mosquitto_publish (client->mosq,
                                NULL,
                                topic,
                                payload_len,
                                payload,
                                qos,
                                retain)) != MOSQ_ERR_SUCCESS)
    {
      g_warning ("publishing %s failed (%s)\n",
                 topic, mosquitto_strerror (res));

      return FALSE;
    }

  return TRUE;

}
