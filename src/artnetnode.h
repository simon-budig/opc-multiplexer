#ifndef __ARTNET_NODE_H__
#define __ARTNET_NODE_H__


#define ARTNET_TYPE_NODE            (artnet_node_get_type ())
#define ARTNET_NODE(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), ARTNET_TYPE_NODE, ArtnetNode))
#define ARTNET_NODE_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  ARTNET_TYPE_NODE, ArtnetNodeClass))
#define ARTNET_IS_NODE(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), ARTNET_TYPE_NODE))
#define ARTNET_IS_NODET_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  ARTNET_TYPE_NODE))
#define ARTNET_NODE_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  ARTNET_TYPE_NODE, ArtnetNodeClass))


typedef struct _ArtnetNodeClass  ArtnetNodeClass;

struct _ArtnetNodeClass
{
  PxSourceClass  parent_class;
};


struct _ArtnetNode
{
  PxSource    parent_instance;
  OpcBroker  *broker;

  GIOChannel *gio;

  struct sockaddr_storage addr;
  struct sockaddr_storage broadcast;
  guint8                  hwaddr[6];

  guint8      last_seqno;
};


GType         artnet_node_get_type     (void) G_GNUC_CONST;

ArtnetNode *  artnet_node_new          (OpcBroker *broker,
                                        gboolean   is_remote,
                                        gint       num_pixels);

#endif  /*  __ARTNET_NODE_H__  */
