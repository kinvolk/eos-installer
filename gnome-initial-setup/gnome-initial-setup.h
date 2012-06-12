/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#ifndef __GNOME_INITIAL_SETUP_H__
#define __GNOME_INITIAL_SETUP_H__

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _SetupData SetupData;

GtkBuilder *gis_get_builder (SetupData *data);
GtkAssistant *gis_get_assistant (SetupData *data);
GKeyFile *gis_get_overrides (SetupData *data);

#define OBJ(type,name) ((type)gtk_builder_get_object(gis_get_builder(setup),(name)))
#define WID(name) OBJ(GtkWidget*,name)

G_END_DECLS

#endif /* __GNOME_INITIAL_SETUP_H__ */
