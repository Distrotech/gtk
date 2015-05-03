#include <gtk/gtk.h>

#include <string.h>

#define ROW_TYPE_DATA (row_data_get_type ())

G_DECLARE_DERIVABLE_TYPE (RowData, row_data, ROW, DATA, GObject)

struct _RowDataClass
{
  GObjectClass parent_class;
};

typedef struct
{
  gint row_type;
  gchar *label;
  gint sort_id;
} RowDataPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (RowData, row_data, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_LABEL,
  PROP_SORT_ID,
  PROP_ROW_TYPE,
  LAST_PROP
};

enum {
  ROW_TYPE_0,
  ROW_TYPE_NORMAL,
  ROW_TYPE_FOCUSABLE,
  ROW_TYPE_DA_BOX,
  LAST_ROW_TYPE
};

static GParamSpec *gParamSpecs [LAST_PROP];

const char *css =
  "GtkListBoxRow {"
  " border-width: 1px;"
  " border-style: solid;"
  " border-color: blue;"
  "}"
  "GtkListBoxRow:prelight {"
  "background-color: green;"
  "}"
  "GtkListBoxRow:active {"
  "background-color: red;"
  "}";


static void
row_data_get_property (GObject    *object,
                       guint       prop_id,
                       GValue     *value,
                       GParamSpec *pspec)
{
  RowData *self = ROW_DATA (object);
  RowDataPrivate *priv = row_data_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_LABEL:
      g_value_set_string (value, priv->label);
      break;

    case PROP_SORT_ID:
      g_value_set_int (value, priv->sort_id);
      break;

    case PROP_ROW_TYPE:
      g_value_set_int (value, priv->row_type);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
row_data_set_property (GObject      *object,
                       guint         prop_id,
                       const GValue *value,
                       GParamSpec   *pspec)
{
  RowData *self = ROW_DATA (object);
  RowDataPrivate *priv = row_data_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_LABEL:
      g_free (priv->label);
      priv->label = g_strdup (g_value_get_string (value));
      break;

    case PROP_SORT_ID:
      priv->sort_id = g_value_get_int (value);
      break;

    case PROP_ROW_TYPE:
      priv->row_type = g_value_get_int (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
row_data_init (RowData *self)
{
}

static void
row_data_class_init (RowDataClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->get_property = row_data_get_property;
  object_class->set_property = row_data_set_property;


  gParamSpecs [PROP_LABEL] =
    g_param_spec_string ("label",
                         "label",
                         "The label text.",
                         NULL,
                         (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_LABEL, gParamSpecs [PROP_LABEL]);
  gParamSpecs [PROP_SORT_ID] =
    g_param_spec_int ("sort-id",
                      "sort id",
                      "The sort position for the UI.",
                      0, G_MAXINT, 0,
                      (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_SORT_ID, gParamSpecs [PROP_SORT_ID]);

  gParamSpecs [PROP_ROW_TYPE] =
    g_param_spec_int ("row-type",
                      "row type",
                      "The row type.",
                      ROW_TYPE_0, LAST_ROW_TYPE, ROW_TYPE_NORMAL,
                      (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_ROW_TYPE, gParamSpecs [PROP_ROW_TYPE]);
}

RowData *
row_new (const gchar* text, gint sort_id, gint row_type) {
  RowData *row_data;

  row_data = g_object_new (row_data_get_type (),
                           "label", text,
                           "sort-id", sort_id,
                           "row-type", row_type,
                           NULL);

  return row_data;
}

static GtkWidget *
create_row_widget (gpointer item,
                   gpointer user_data)
{
  RowData *row_data = ROW_DATA (item);
  GListStore *list_store = G_LIST_STORE (user_data);

  return GTK_WIDGET (gtk_label_new ("Heeeya"));
}

static void
update_rows (GListStore *list_model)
{

}

#if 0
static void
update_header_cb (Row *row, Row *before, gpointer data)
{
  GtkWidget *hbox, *l, *b;
  GList *children;

  if (before == NULL ||
      (row->label != NULL &&
       strcmp (gtk_label_get_text (GTK_LABEL (row->label)), "blah3") == 0))
    {
      /* Create header if needed */
      if (gtk_list_box_row_get_header (GTK_LIST_BOX_ROW (row)) == NULL)
        {
          hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
          l = gtk_label_new ("Header");
          gtk_container_add (GTK_CONTAINER (hbox), l);
          b = gtk_button_new_with_label ("button");
          gtk_container_add (GTK_CONTAINER (hbox), b);
          gtk_widget_show (l);
          gtk_widget_show (b);
          gtk_list_box_row_set_header (GTK_LIST_BOX_ROW (row), hbox);
      }

      hbox = gtk_list_box_row_get_header(GTK_LIST_BOX_ROW (row));

      children = gtk_container_get_children (GTK_CONTAINER (hbox));
      l = children->data;
      g_list_free (children);
      gtk_label_set_text (GTK_LABEL (l), g_strdup_printf ("Header %d", row->sort_id));
    }
  else
    {
      gtk_list_box_row_set_header(GTK_LIST_BOX_ROW (row), NULL);
    }
}

static int
sort_cb (Row *a, Row *b, gpointer data)
{
  return a->sort_id - b->sort_id;
}

static int
reverse_sort_cb (Row *a, Row *b, gpointer data)
{
  return b->sort_id - a->sort_id;
}

static gboolean
filter_cb (Row *row, gpointer data)
{
  const char *text;

  if (row->label != NULL)
    {
      text = gtk_label_get_text (GTK_LABEL (row->label));
      return strcmp (text, "blah3") != 0;
    }

  return TRUE;
}

static void
row_activated_cb (GtkListBox *list_box,
                  GtkListBoxRow *row)
{
  g_print ("activated row %p\n", row);
}

static void
row_selected_cb (GtkListBox *list_box,
                 GtkListBoxRow *row)
{
  g_print ("selected row %p\n", row);
}

static void
sort_clicked_cb (GtkButton *button,
                 gpointer data)
{
  GtkListBox *list = data;

  gtk_list_box_set_sort_func (list, (GtkListBoxSortFunc)sort_cb, NULL, NULL);
}

static void
reverse_sort_clicked_cb (GtkButton *button,
                         gpointer data)
{
  GtkListBox *list = data;

  gtk_list_box_set_sort_func (list, (GtkListBoxSortFunc)reverse_sort_cb, NULL, NULL);
}

static void
filter_clicked_cb (GtkButton *button,
                   gpointer data)
{
  GtkListBox *list = data;

  gtk_list_box_set_filter_func (list, (GtkListBoxFilterFunc)filter_cb, NULL, NULL);
}

static void
unfilter_clicked_cb (GtkButton *button,
                   gpointer data)
{
  GtkListBox *list = data;

  gtk_list_box_set_filter_func (list, NULL, NULL, NULL);
}

static void
change_clicked_cb (GtkButton *button,
                   gpointer data)
{
  Row *row = data;

  if (strcmp (gtk_label_get_text (GTK_LABEL (row->label)), "blah3") == 0)
    {
      gtk_label_set_text (GTK_LABEL (row->label), "blah5");
      row->sort_id = 5;
    }
  else
    {
      gtk_label_set_text (GTK_LABEL (row->label), "blah3");
      row->sort_id = 3;
    }
  gtk_list_box_row_changed (GTK_LIST_BOX_ROW (row));
}

static void
add_clicked_cb (GtkButton *button,
                gpointer data)
{
  GtkListBox *list = data;
  GtkWidget *new_row;
  static int new_button_nr = 1;

  new_row = row_new( g_strdup_printf ("blah2 new %d", new_button_nr), new_button_nr);
  gtk_widget_show_all (new_row);
  gtk_container_add (GTK_CONTAINER (list), new_row);
  new_button_nr++;
}

static void
separate_clicked_cb (GtkButton *button,
                     gpointer data)
{
  GtkListBox *list = data;

  gtk_list_box_set_header_func (list, (GtkListBoxUpdateHeaderFunc)update_header_cb, NULL, NULL);
}

static void
unseparate_clicked_cb (GtkButton *button,
                       gpointer data)
{
  GtkListBox *list = data;

  gtk_list_box_set_header_func (list, NULL, NULL, NULL);
}

static void
visibility_clicked_cb (GtkButton *button,
                       gpointer data)
{
  GtkWidget *row = data;

  gtk_widget_set_visible (row, !gtk_widget_get_visible (row));
}

static void
selection_mode_changed (GtkComboBox *combo, gpointer data)
{
  GtkListBox *list = data;

  gtk_list_box_set_selection_mode (list, gtk_combo_box_get_active (combo));
}

static void
single_click_clicked (GtkButton *check, gpointer data)
{
  GtkListBox *list = data;

  g_print ("single: %d\n", gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (check)));
  gtk_list_box_set_activate_on_single_click (list, gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (check)));
}

#endif

int
main (int argc, char *argv[])
{
  GListStore *list_model;
  GtkCssProvider *provider;
  GtkWidget *window, *hbox, *vbox, *list, *row_vbox, *row_hbox, *l, *row_widget;
  GtkWidget *check, *button, *combo, *scrolled;
  RowData *row, row3;

  gtk_init (NULL, NULL);

  window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_container_add (GTK_CONTAINER (window), hbox);

  provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_data (provider, css, -1, NULL);
  gtk_style_context_add_provider_for_screen (gtk_widget_get_screen (window),
                                             GTK_STYLE_PROVIDER (provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_USER);


  list = gtk_list_box_new ();
  list_model = g_list_store_new (ROW_TYPE_DATA);

  vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add (GTK_CONTAINER (hbox), vbox);

  combo = gtk_combo_box_text_new ();
  gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo),
                                  "GTK_SELECTION_NONE");
  gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo),
                                  "GTK_SELECTION_SINGLE");
  gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo),
                                  "GTK_SELECTION_BROWSE");
//  g_signal_connect (combo, "changed", G_CALLBACK (selection_mode_changed), list);
  gtk_container_add (GTK_CONTAINER (vbox), combo);
  gtk_combo_box_set_active (GTK_COMBO_BOX (combo), gtk_list_box_get_selection_mode (GTK_LIST_BOX (list)));
  check = gtk_check_button_new_with_label ("single click mode");
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (check), gtk_list_box_get_activate_on_single_click (GTK_LIST_BOX (list)));
 // g_signal_connect (check, "toggled", G_CALLBACK (single_click_clicked), list);
  gtk_container_add (GTK_CONTAINER (vbox), check);

  scrolled = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_container_add (GTK_CONTAINER (scrolled), list);
  gtk_container_add (GTK_CONTAINER (hbox), scrolled);

  // g_signal_connect (list, "row-activated", G_CALLBACK (row_activated_cb), NULL);
  // g_signal_connect (list, "row-selected", G_CALLBACK (row_selected_cb), NULL);

  row = row_new ("blah4", 4, ROW_TYPE_NORMAL);
  g_list_store_append (list_model, row);
  row = row_new ("blah3", 3, ROW_TYPE_NORMAL);
  g_list_store_append (list_model, row);
  row = row_new ("blah1", 1, ROW_TYPE_NORMAL);
  g_list_store_append (list_model, row);
  row = row_new ("blah2", 2, ROW_TYPE_NORMAL);
  g_list_store_append (list_model, row);

  row = row_new (NULL, 0, ROW_TYPE_DA_BOX);
  g_list_store_append (list_model, row);

  row = row_new (NULL, 0, ROW_TYPE_FOCUSABLE);
  g_list_store_append (list_model, row);

  gtk_list_box_bind_model (GTK_LIST_BOX (list), G_LIST_MODEL (list_model),
                           (GtkListBoxCreateWidgetFunc) create_row_widget,
                           list_model,
                           NULL);

  vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add (GTK_CONTAINER (hbox), vbox);

  button = gtk_button_new_with_label ("sort");
  gtk_container_add (GTK_CONTAINER (vbox), button);
  // g_signal_connect (button, "clicked", G_CALLBACK (sort_clicked_cb), list);

  button = gtk_button_new_with_label ("reverse");
  gtk_container_add (GTK_CONTAINER (vbox), button);
  // g_signal_connect (button, "clicked", G_CALLBACK (reverse_sort_clicked_cb), list);

  button = gtk_button_new_with_label ("change");
  gtk_container_add (GTK_CONTAINER (vbox), button);
  // g_signal_connect (button, "clicked", G_CALLBACK (change_clicked_cb), row3);

  button = gtk_button_new_with_label ("filter");
  gtk_container_add (GTK_CONTAINER (vbox), button);
  // g_signal_connect (button, "clicked", G_CALLBACK (filter_clicked_cb), list);

  button = gtk_button_new_with_label ("unfilter");
  gtk_container_add (GTK_CONTAINER (vbox), button);
  // g_signal_connect (button, "clicked", G_CALLBACK (unfilter_clicked_cb), list);

  button = gtk_button_new_with_label ("add");
  gtk_container_add (GTK_CONTAINER (vbox), button);
  // g_signal_connect (button, "clicked", G_CALLBACK (add_clicked_cb), list);

  button = gtk_button_new_with_label ("separate");
  gtk_container_add (GTK_CONTAINER (vbox), button);
  // g_signal_connect (button, "clicked", G_CALLBACK (separate_clicked_cb), list);

  button = gtk_button_new_with_label ("unseparate");
  gtk_container_add (GTK_CONTAINER (vbox), button);
  // g_signal_connect (button, "clicked", G_CALLBACK (unseparate_clicked_cb), list);

  button = gtk_button_new_with_label ("visibility");
  gtk_container_add (GTK_CONTAINER (vbox), button);
  // g_signal_connect (button, "clicked", G_CALLBACK (visibility_clicked_cb), row3);

  gtk_widget_show_all (window);

  gtk_main ();

  return 0;
}
