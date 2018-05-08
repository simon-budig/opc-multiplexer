#ifndef __OPC_BROKER_H__
#define __OPC_BROKER_H__

#define OPC_TYPE_BROKER            (opc_broker_get_type ())
#define OPC_BROKER(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), OPC_TYPE_BROKER, OpcBroker))
#define OPC_BROKER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  OPC_TYPE_BROKER, OpcBrokerClass))
#define OPC_IS_BROKER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), OPC_TYPE_BROKER))
#define OPC_IS_BROKER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  OPC_TYPE_BROKER))
#define OPC_BROKER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  OPC_TYPE_BROKER, OpcBrokerClass))

typedef struct _OpcBrokerClass  OpcBrokerClass;

struct _OpcBroker
{
  GObject     parent_instance;

  gint                target_fd;
  struct sockaddr_in  target_address;
  GIOChannel         *opc_target;
  gint                port_number;
  GIOChannel         *sock_io;

  PxSource           *overlay_pxsource;
  gdouble             global_brightness;
  gint                num_pixels;
  gint                out_pos;
  gint                out_len;
  gchar              *outbuf;

  guint               outhandler;

  gdouble             start_time;
  PxSource           *cur_pxsource;
  PxSource           *prev_pxsource;
  guint               render_id;
  guint               pxsource_check_id;

  GList              *pxsources;
};

struct _OpcBrokerClass
{
  GObjectClass  parent_class;

  /* signals */
  // void  (*finished)  (OpcBroker *broker);
};


GType        opc_broker_get_type       (void) G_GNUC_CONST;

OpcBroker *  opc_broker_new            (gint          num_pixels);
gboolean     opc_broker_connect_target (OpcBroker    *broker,
                                        gchar        *hostname,
                                        guint16       port);
gboolean     opc_broker_run            (OpcBroker    *broker,
                                        guint16       port,
                                        GError      **err);
void         opc_broker_notify_frame   (OpcBroker    *broker,
                                        PxSource     *pxs);


#endif  /*  __OPC_BROKER_H__  */
