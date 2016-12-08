#ifndef __OPC_CLIENT_H__
#define __OPC_CLIENT_H__


#define OPC_TYPE_CLIENT            (opc_client_get_type ())
#define OPC_CLIENT(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), OPC_TYPE_CLIENT, OpcClient))
#define OPC_CLIENT_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  OPC_TYPE_CLIENT, OpcClientClass))
#define OPC_IS_CLIENT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), OPC_TYPE_CLIENT))
#define OPC_IS_CLIENT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  OPC_TYPE_CLIENT))
#define OPC_CLIENT_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  OPC_TYPE_CLIENT, OpcClientClass))


typedef struct _OpcClientClass  OpcClientClass;

struct _OpcClientClass
{
  GObjectClass  parent_class;
};


struct _OpcClient
{
  GObject     parent_instance;
  OpcBroker  *broker;
  GIOChannel *gio;
  GString    *inbuf;
};


GType        opc_client_get_type       (void) G_GNUC_CONST;

OpcClient *  opc_client_new            (OpcBroker *broker,
                                        gint       fd);

#endif  /*  __OPC_CLIENT_H__  */
