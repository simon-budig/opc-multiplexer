#ifndef __GMQTT_CLIENT_H__
#define __GMQTT_CLIENT_H__

#define GMQTT_TYPE_CLIENT            (gmqtt_client_get_type ())
#define GMQTT_CLIENT(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), GMQTT_TYPE_CLIENT, GMqttClient))
#define GMQTT_CLIENT_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  GMQTT_TYPE_CLIENT, GMqttClientClass))
#define GMQTT_IS_CLIENT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GMQTT_TYPE_CLIENT))
#define GMQTT_IS_CLIENT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  GMQTT_TYPE_CLIENT))
#define GMQTT_CLIENT_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  GMQTT_TYPE_CLIENT, GMqttClientClass))

typedef struct _GMqttClientClass  GMqttClientClass;

#define QOS_0 (0)
#define QOS_1 (1)
#define QOS_2 (2)

struct _GMqttClient
{
  GObject     parent_instance;

  gchar      *name;
  gchar      *server;
  gint        port;
  gint        default_qos;
  gboolean    connected;

  struct mosquitto *mosq;
  GSource    *mosq_src;
  GList      *subscriptions;

  guint       reconnect_timeout;
};

struct _GMqttClientClass
{
  GObjectClass  parent_class;

  /* signals */
  void       (* message_received)      (GMqttClient              *client,
                                        struct mosquitto_message *msg);

  // void  (*finished)  (GMqttClient *client);
};


GType          gmqtt_client_get_type  (void) G_GNUC_CONST;

GMqttClient *  gmqtt_client_new            (gchar       *name,
                                            gchar       *server,
                                            gint         port);
GMqttClient *  gmqtt_client_new_with_will  (gchar  *name,
                                            gchar  *server,
                                            gint    port,
                                            gchar  *will_topic,
                                            gchar  *will_payload,
                                            gssize  will_payload_len);
gboolean       gmqtt_client_subscribe      (GMqttClient *client,
                                            gchar       *sub_id,
                                            gchar       *subscription);
gboolean       gmqtt_client_subscribe_full (GMqttClient *client,
                                            gchar       *sub_id,
                                            gchar       *subscription,
                                            gint         qos);
gboolean       gmqtt_client_publish        (GMqttClient *client,
                                            gchar       *topic,
                                            gchar       *payload,
                                            gint         payload_len);
gboolean       gmqtt_client_publish_full   (GMqttClient *client,
                                            gchar       *topic,
                                            gchar       *payload,
                                            gint         payload_len,
                                            gint         qos,
                                            gboolean     retain);

#endif  /*  __GMQTT_CLIENT_H__  */
