#ifndef __PX_SOURCE_H__
#define __PX_SOURCE_H__


#define PX_TYPE_SOURCE            (px_source_get_type ())
#define PX_SOURCE(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), PX_TYPE_SOURCE, PxSource))
#define PX_SOURCE_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  PX_TYPE_SOURCE, PxSourceClass))
#define PX_IS_SOURCE(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PX_TYPE_SOURCE))
#define PX_IS_SOURCE_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  PX_TYPE_SOURCE))
#define PX_SOURCE_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  PX_TYPE_SOURCE, PxSourceClass))


typedef struct _PxSourceClass  PxSourceClass;

struct _PxSourceClass
{
  GObjectClass  parent_class;
};


struct _PxSource
{
  GObject     parent_instance;
  OpcBroker  *broker;

  gint        num_pixels;
  gfloat     *cur_frame_rgba;

  gboolean    is_enabled;
  gboolean    is_remote;
  gboolean    is_connected;
  gdouble     last_used;
  gdouble     timestamp;
};


GType        px_source_get_type       (void) G_GNUC_CONST;

#endif  /*  __PX_SOURCE_H__  */
