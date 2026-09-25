#include <gtk/gtk.h>
#include <sqlite3.h>
#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <pango/pangocairo.h>
#include <zip.h>
#include <string.h>

typedef struct {
    GtkApplication *application;
    GtkWindow *window;
    GtkStack *stack;
    GtkSearchEntry *search;
    GtkListBox *patient_list;
    GtkLabel *patient_name;
    GtkLabel *patient_dob;
    GtkLabel *patient_summary;
    GtkListBox *finding_list;
    GtkTextView *editor;
    GtkLabel *editor_title;
    GtkLabel *save_state;
    GtkDropDown *template_dropdown;
    GtkLabel *ai_state;
    sqlite3 *db;
    int patient_id;
    int finding_id;
    guint autosave_source;
    gboolean loading_editor;
} App;

typedef struct {
    char *data;
    size_t size;
} Buffer;

typedef struct {
    char *endpoint;
    char *key;
    char *model;
    char *prompt;
    char *findings;
    char *result;
    char *error;
    int patient_id;
    int finding_count;
} AiRequest;

typedef struct {
    char *text;
    double page_height;
    int pages;
} PrintData;

static void show_patient(App *app, int patient_id);
static void refresh_search(App *app);
static void refresh_findings(App *app);
static char *build_document_text(App *app, int finding_id);
static void generate_ai_summary(App *app, int patient_id);

static void show_error(App *app, const char *title, const char *message) {
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", title);
    gtk_alert_dialog_set_detail(dialog, message);
    gtk_alert_dialog_show(dialog, app->window);
    g_object_unref(dialog);
}

static gboolean exec_sql(App *app, const char *sql) {
    char *error = NULL;
    if (sqlite3_exec(app->db, sql, NULL, NULL, &error) != SQLITE_OK) {
        show_error(app, "Greška baze podataka", error);
        sqlite3_free(error);
        return FALSE;
    }
    return TRUE;
}

static char *setting_get(App *app, const char *key, const char *fallback) {
    sqlite3_stmt *statement;
    char *value = g_strdup(fallback);
    sqlite3_prepare_v2(app->db, "SELECT value FROM settings WHERE key=?", -1, &statement, NULL);
    sqlite3_bind_text(statement, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(statement) == SQLITE_ROW) {
        g_free(value);
        value = g_strdup((const char *)sqlite3_column_text(statement, 0));
    }
    sqlite3_finalize(statement);
    return value;
}

static void setting_set(App *app, const char *key, const char *value) {
    sqlite3_stmt *statement;
    sqlite3_prepare_v2(app->db,
        "INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
        -1, &statement, NULL);
    sqlite3_bind_text(statement, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, value, -1, SQLITE_TRANSIENT);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
}

static char *display_date(const char *value) {
    if (value && strlen(value) == 10 && value[4] == '-' && value[7] == '-')
        return g_strdup_printf("%.2s.%.2s.%.4s", value + 8, value + 5, value);
    return g_strdup(value ? value : "");
}

static char *storage_date(const char *value) {
    if (!value || strlen(value) != 10) return NULL;
    guint day, month, year;
    if (value[2] == '.' && value[5] == '.') {
        for (int i = 0; i < 10; i++)
            if (i != 2 && i != 5 && !g_ascii_isdigit(value[i])) return NULL;
        day = (guint)g_ascii_strtoull(value, NULL, 10);
        month = (guint)g_ascii_strtoull(value + 3, NULL, 10);
        year = (guint)g_ascii_strtoull(value + 6, NULL, 10);
    } else if (value[4] == '-' && value[7] == '-') {
        for (int i = 0; i < 10; i++)
            if (i != 4 && i != 7 && !g_ascii_isdigit(value[i])) return NULL;
        year = (guint)g_ascii_strtoull(value, NULL, 10);
        month = (guint)g_ascii_strtoull(value + 5, NULL, 10);
        day = (guint)g_ascii_strtoull(value + 8, NULL, 10);
    } else {
        return NULL;
    }
    if (!g_date_valid_dmy((GDateDay)day, (GDateMonth)month, (GDateYear)year)) return NULL;
    return g_strdup_printf("%04u-%02u-%02u", year, month, day);
}

static gboolean open_database(App *app) {
    char *directory = g_build_filename(g_get_user_data_dir(), "clinic-notes", NULL);
    char *path = g_build_filename(directory, "clinic.db", NULL);
    g_mkdir_with_parents(directory, 0700);
    int result = sqlite3_open(path, &app->db);
    g_free(path);
    g_free(directory);
    if (result != SQLITE_OK)
        return FALSE;

    exec_sql(app, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL;");
    return exec_sql(app,
        "CREATE TABLE IF NOT EXISTS patients("
        "id INTEGER PRIMARY KEY, first_name TEXT NOT NULL, last_name TEXT NOT NULL,"
        "date_of_birth TEXT NOT NULL, ai_summary TEXT NOT NULL DEFAULT '', created_at TEXT DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS findings("
        "id INTEGER PRIMARY KEY, patient_id INTEGER NOT NULL REFERENCES patients(id) ON DELETE CASCADE,"
        "content TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT 'draft',"
        "visit_date TEXT NOT NULL DEFAULT CURRENT_DATE, updated_at TEXT DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS templates("
        "id INTEGER PRIMARY KEY, name TEXT NOT NULL, body TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "INSERT INTO templates(name,body) SELECT 'Standardni pregled',"
        "'NALAZ LEKARSKOG PREGLEDA\\n\\nPacijent: {patient_name}\\nDatum rođenja: {date_of_birth}\\nDatum posete: {visit_date}\\n\\nNALAZ\\n{finding}\\n\\n\\nPotpis lekara: ____________________' "
        "WHERE NOT EXISTS(SELECT 1 FROM templates);"
        "UPDATE templates SET name='Standardni pregled',"
        "body='NALAZ LEKARSKOG PREGLEDA\\n\\nPacijent: {patient_name}\\nDatum rođenja: {date_of_birth}\\nDatum posete: {visit_date}\\n\\nNALAZ\\n{finding}\\n\\n\\nPotpis lekara: ____________________' "
        "WHERE name='Standard examination' AND body='MEDICAL EXAMINATION FINDING\\n\\nPatient: {patient_name}\\nDate of birth: {date_of_birth}\\nVisit date: {visit_date}\\n\\nFINDING\\n{finding}\\n\\n\\nClinician signature: ____________________';"
    );
}

static GtkWidget *heading(const char *text, const char *css_class) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_add_css_class(label, css_class);
    return label;
}

static GtkWidget *scrolled(GtkWidget *child) {
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), child);
    gtk_widget_set_vexpand(scroll, TRUE);
    return scroll;
}

static void clear_list(GtkListBox *list) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(list))))
        gtk_list_box_remove(list, child);
}

static void search_changed(GtkEditable *editable, gpointer data) {
    (void)editable;
    refresh_search(data);
}

static void refresh_search(App *app) {
    clear_list(app->patient_list);
    const char *query = gtk_editable_get_text(GTK_EDITABLE(app->search));
    char *pattern = g_strdup_printf("%%%s%%", query);
    sqlite3_stmt *statement;
    sqlite3_prepare_v2(app->db,
        "SELECT id,first_name,last_name,date_of_birth FROM patients "
        "WHERE first_name LIKE ? OR last_name LIKE ? OR (first_name||' '||last_name) LIKE ? "
        "ORDER BY last_name,first_name LIMIT 100", -1, &statement, NULL);
    for (int i = 1; i <= 3; i++) sqlite3_bind_text(statement, i, pattern, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(statement) == SQLITE_ROW) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
        GtkWidget *names = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        char *name = g_strdup_printf("%s %s", sqlite3_column_text(statement, 1), sqlite3_column_text(statement, 2));
        GtkWidget *name_label = heading(name, "patient-row-name");
        char *formatted_dob = display_date((const char *)sqlite3_column_text(statement, 3));
        GtkWidget *dob = gtk_label_new(formatted_dob);
        gtk_label_set_xalign(GTK_LABEL(dob), 0);
        gtk_widget_add_css_class(dob, "dim-label");
        gtk_box_append(GTK_BOX(names), name_label);
        gtk_box_append(GTK_BOX(names), dob);
        gtk_box_append(GTK_BOX(box), names);
        GtkWidget *arrow = gtk_image_new_from_icon_name("go-next-symbolic");
        gtk_widget_set_hexpand(arrow, TRUE);
        gtk_widget_set_halign(arrow, GTK_ALIGN_END);
        gtk_box_append(GTK_BOX(box), arrow);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        g_object_set_data(G_OBJECT(row), "patient-id", GINT_TO_POINTER(sqlite3_column_int(statement, 0)));
        gtk_list_box_append(app->patient_list, row);
        g_free(formatted_dob);
        g_free(name);
    }
    sqlite3_finalize(statement);
    g_free(pattern);
}

static void patient_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    (void)box;
    show_patient(data, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "patient-id")));
}

static void add_patient_response(GtkDialog *dialog, int response, gpointer data) {
    App *app = data;
    if (response == GTK_RESPONSE_OK) {
        GtkEntry *first = g_object_get_data(G_OBJECT(dialog), "first");
        GtkEntry *last = g_object_get_data(G_OBJECT(dialog), "last");
        GtkEntry *dob = g_object_get_data(G_OBJECT(dialog), "dob");
        const char *first_text = gtk_editable_get_text(GTK_EDITABLE(first));
        const char *last_text = gtk_editable_get_text(GTK_EDITABLE(last));
        const char *dob_text = gtk_editable_get_text(GTK_EDITABLE(dob));
        if (*first_text && *last_text && *dob_text) {
            char *normalized_dob = storage_date(dob_text);
            if (!normalized_dob) {
                show_error(app, "Neispravan datum", "Unesite datum rođenja u formatu DD.MM.GGGG.");
                return;
            }
            sqlite3_stmt *statement;
            sqlite3_prepare_v2(app->db, "INSERT INTO patients(first_name,last_name,date_of_birth) VALUES(?,?,?)", -1, &statement, NULL);
            sqlite3_bind_text(statement, 1, first_text, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 2, last_text, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 3, normalized_dob, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(statement) == SQLITE_DONE) {
                int id = (int)sqlite3_last_insert_rowid(app->db);
                sqlite3_finalize(statement);
                g_free(normalized_dob);
                show_patient(app, id);
                gtk_window_destroy(GTK_WINDOW(dialog));
                return;
            }
            sqlite3_finalize(statement);
            g_free(normalized_dob);
        } else {
            show_error(app, "Nedostaju podaci", "Ime, prezime i datum rođenja su obavezni.");
        }
    }
    gtk_window_destroy(GTK_WINDOW(dialog));
}

static void add_patient_clicked(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    GtkWidget *dialog = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Novi pacijent");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 580, 430);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), app->window);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Otkaži", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Dodaj pacijenta", GTK_RESPONSE_OK);
    GtkWidget *form = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_top(form, 36); gtk_widget_set_margin_bottom(form, 36);
    gtk_widget_set_margin_start(form, 36); gtk_widget_set_margin_end(form, 36);
    GtkWidget *first = gtk_entry_new(); GtkWidget *last = gtk_entry_new(); GtkWidget *dob = gtk_entry_new();
    gtk_widget_add_css_class(first, "patient-field");
    gtk_widget_add_css_class(last, "patient-field");
    gtk_widget_add_css_class(dob, "patient-field");
    gtk_entry_set_placeholder_text(GTK_ENTRY(first), "Ime");
    gtk_entry_set_placeholder_text(GTK_ENTRY(last), "Prezime");
    gtk_entry_set_placeholder_text(GTK_ENTRY(dob), "Datum rođenja (DD.MM.GGGG)");
    gtk_box_append(GTK_BOX(form), heading("Podaci o pacijentu", "dialog-title"));
    gtk_box_append(GTK_BOX(form), first); gtk_box_append(GTK_BOX(form), last); gtk_box_append(GTK_BOX(form), dob);
    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), form);
    g_object_set_data(G_OBJECT(dialog), "first", first);
    g_object_set_data(G_OBJECT(dialog), "last", last);
    g_object_set_data(G_OBJECT(dialog), "dob", dob);
    g_signal_connect(dialog, "response", G_CALLBACK(add_patient_response), app);
    gtk_window_present(GTK_WINDOW(dialog));
}

static void home_clicked(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (app->autosave_source) {
        g_source_remove(app->autosave_source);
        app->autosave_source = 0;
    }
    app->patient_id = 0;
    app->finding_id = 0;
    refresh_search(app);
    gtk_stack_set_visible_child_name(app->stack, "home");
}

static char *excerpt(const char *content) {
    if (!content || !*content) return g_strdup("Prazan nacrt");
    char *single = g_strdup(content);
    for (char *p = single; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    if (g_utf8_strlen(single, -1) > 150) {
        char *cut = g_utf8_offset_to_pointer(single, 150);
        *cut = '\0';
        char *result = g_strconcat(single, "...", NULL);
        g_free(single);
        return result;
    }
    return single;
}

static gboolean zip_add_text(zip_t *archive, const char *name, const char *text) {
    zip_source_t *source = zip_source_buffer(archive, text, strlen(text), 0);
    if (!source)
        return FALSE;
    if (zip_file_add(archive, name, source, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8) < 0) {
        zip_source_free(source);
        return FALSE;
    }
    return TRUE;
}

static char *word_document_xml(const char *text) {
    GString *xml = g_string_new(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:body>");
    char **lines = g_strsplit(text ? text : "", "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        g_strchomp(lines[i]);
        if (!*lines[i]) {
            g_string_append(xml, "<w:p/>");
            continue;
        }
        char *escaped = g_markup_escape_text(lines[i], -1);
        g_string_append_printf(xml,
            "<w:p><w:r><w:rPr><w:lang w:val=\"sr-Latn-RS\"/></w:rPr>"
            "<w:t xml:space=\"preserve\">%s</w:t></w:r></w:p>", escaped);
        g_free(escaped);
    }
    g_strfreev(lines);
    g_string_append(xml,
        "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/>"
        "<w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\"/>"
        "</w:sectPr></w:body></w:document>");
    return g_string_free(xml, FALSE);
}

static gboolean write_docx(const char *path, const char *text, char **error_message) {
    static const char *content_types =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/word/document.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
        "</Types>";
    static const char *relationships =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
        "Target=\"word/document.xml\"/>"
        "</Relationships>";

    int zip_error = 0;
    zip_t *archive = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, &zip_error);
    if (!archive) {
        zip_error_t detail;
        zip_error_init_with_code(&detail, zip_error);
        *error_message = g_strdup(zip_error_strerror(&detail));
        zip_error_fini(&detail);
        return FALSE;
    }

    char *document = word_document_xml(text);
    gboolean added = zip_add_text(archive, "[Content_Types].xml", content_types) &&
        zip_add_text(archive, "_rels/.rels", relationships) &&
        zip_add_text(archive, "word/document.xml", document);
    if (!added) {
        *error_message = g_strdup(zip_strerror(archive));
        zip_discard(archive);
        g_free(document);
        return FALSE;
    }
    if (zip_close(archive) < 0) {
        *error_message = g_strdup(zip_strerror(archive));
        zip_discard(archive);
        g_free(document);
        return FALSE;
    }
    g_free(document);
    return TRUE;
}

static char *export_filename(App *app, int finding_id) {
    sqlite3_stmt *statement;
    char *filename = g_strdup_printf("nalaz_%d.docx", finding_id);
    sqlite3_prepare_v2(app->db,
        "SELECT p.first_name||'_'||p.last_name,f.visit_date "
        "FROM patients p JOIN findings f ON f.patient_id=p.id WHERE f.id=? AND p.id=?",
        -1, &statement, NULL);
    sqlite3_bind_int(statement, 1, finding_id);
    sqlite3_bind_int(statement, 2, app->patient_id);
    if (sqlite3_step(statement) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(statement, 0);
        char *date = display_date((const char *)sqlite3_column_text(statement, 1));
        g_free(filename);
        filename = g_strdup_printf("nalaz_%s_%s.docx", name, date);
        g_free(date);
        g_strdelimit(filename, " /\\:", '_');
    }
    sqlite3_finalize(statement);
    return filename;
}

static void export_response(GtkNativeDialog *dialog, int response, gpointer data) {
    App *app = data;
    if (response == GTK_RESPONSE_ACCEPT) {
        GFile *file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
        char *path = file ? g_file_get_path(file) : NULL;
        if (!path) {
            show_error(app, "Izvoz nije uspeo", "Izabrana lokacija nije lokalna datoteka.");
        } else {
            char *lower = g_utf8_strdown(path, -1);
            if (!g_str_has_suffix(lower, ".docx")) {
                char *with_extension = g_strconcat(path, ".docx", NULL);
                g_free(path);
                path = with_extension;
            }
            g_free(lower);
            int finding_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dialog), "finding-id"));
            char *text = build_document_text(app, finding_id);
            char *error = NULL;
            if (write_docx(path, text, &error))
                show_error(app, "Izvoz završen", "Word dokument je uspešno sačuvan.");
            else
                show_error(app, "Izvoz nije uspeo", error ? error : "Dokument nije moguće sačuvati.");
            g_free(error);
            g_free(text);
            g_free(path);
        }
        g_clear_object(&file);
    }
    g_object_unref(dialog);
}

static void export_word_clicked(GtkButton *button, gpointer data) {
    App *app = data;
    int finding_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "finding-id"));
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new(
        "Izvoz nalaza u Word", app->window, GTK_FILE_CHOOSER_ACTION_SAVE,
        "Sačuvaj", "Otkaži");
    char *filename = export_filename(app, finding_id);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), filename);
    g_free(filename);
    g_object_set_data(G_OBJECT(dialog), "finding-id", GINT_TO_POINTER(finding_id));
    g_signal_connect(dialog, "response", G_CALLBACK(export_response), app);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
}

static void refresh_findings(App *app) {
    clear_list(app->finding_list);
    sqlite3_stmt *statement;
    sqlite3_prepare_v2(app->db,
        "SELECT id,visit_date,content,status,updated_at FROM findings WHERE patient_id=? ORDER BY visit_date DESC,id DESC",
        -1, &statement, NULL);
    sqlite3_bind_int(statement, 1, app->patient_id);
    while (sqlite3_step(statement) == SQLITE_ROW) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        char *formatted_date = display_date((const char *)sqlite3_column_text(statement, 1));
        GtkWidget *date = heading(formatted_date, "visit-date");
        const char *status_text = (const char *)sqlite3_column_text(statement, 3);
        GtkWidget *status = gtk_label_new(g_str_equal(status_text, "draft") ? "NACRT" : "ZAVRŠENO");
        gtk_widget_add_css_class(status, g_str_equal(status_text, "draft") ? "draft-badge" : "signed-badge");
        GtkWidget *export = gtk_button_new_with_label("Izvezi u Word");
        gtk_widget_set_hexpand(export, TRUE);
        gtk_widget_set_halign(export, GTK_ALIGN_END);
        g_object_set_data(G_OBJECT(export), "finding-id",
            GINT_TO_POINTER(sqlite3_column_int(statement, 0)));
        g_signal_connect(export, "clicked", G_CALLBACK(export_word_clicked), app);
        gtk_box_append(GTK_BOX(top), date);
        gtk_box_append(GTK_BOX(top), status);
        gtk_box_append(GTK_BOX(top), export);
        char *short_text = excerpt((const char *)sqlite3_column_text(statement, 2));
        GtkWidget *summary = gtk_label_new(short_text);
        gtk_label_set_xalign(GTK_LABEL(summary), 0); gtk_label_set_wrap(GTK_LABEL(summary), TRUE);
        gtk_widget_add_css_class(summary, "finding-excerpt");
        gtk_box_append(GTK_BOX(box), top); gtk_box_append(GTK_BOX(box), summary);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        g_object_set_data(G_OBJECT(row), "finding-id", GINT_TO_POINTER(sqlite3_column_int(statement, 0)));
        gtk_list_box_append(app->finding_list, row);
        g_free(formatted_date);
        g_free(short_text);
    }
    sqlite3_finalize(statement);
}

static void show_patient(App *app, int patient_id) {
    app->patient_id = patient_id;
    sqlite3_stmt *statement;
    sqlite3_prepare_v2(app->db, "SELECT first_name,last_name,date_of_birth,ai_summary FROM patients WHERE id=?", -1, &statement, NULL);
    sqlite3_bind_int(statement, 1, patient_id);
    if (sqlite3_step(statement) == SQLITE_ROW) {
        char *name = g_strdup_printf("%s %s", sqlite3_column_text(statement, 0), sqlite3_column_text(statement, 1));
        char *formatted_dob = display_date((const char *)sqlite3_column_text(statement, 2));
        char *dob = g_strdup_printf("Datum rođenja  %s", formatted_dob);
        const char *summary = (const char *)sqlite3_column_text(statement, 3);
        gtk_label_set_text(app->patient_name, name);
        gtk_label_set_text(app->patient_dob, dob);
        gtk_label_set_text(app->patient_summary, *summary ? summary : "AI sažetak nije generisan. Istorija poseta u nastavku ostaje izvor kliničkih podataka.");
        gtk_label_set_text(app->ai_state, *summary
            ? "Sačuvan lokalno; automatski se osvežava nakon završenog nalaza"
            : "Automatski se generiše nakon prvog završenog nalaza");
        g_free(name); g_free(dob); g_free(formatted_dob);
    }
    sqlite3_finalize(statement);
    refresh_findings(app);
    gtk_stack_set_visible_child_name(app->stack, "patient");
}

static void load_finding(App *app, int finding_id) {
    app->finding_id = finding_id;
    sqlite3_stmt *statement;
    sqlite3_prepare_v2(app->db, "SELECT content,visit_date FROM findings WHERE id=? AND patient_id=?", -1, &statement, NULL);
    sqlite3_bind_int(statement, 1, finding_id);
    sqlite3_bind_int(statement, 2, app->patient_id);
    if (sqlite3_step(statement) == SQLITE_ROW) {
        app->loading_editor = TRUE;
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(app->editor), (const char *)sqlite3_column_text(statement, 0), -1);
        app->loading_editor = FALSE;
        char *formatted_date = display_date((const char *)sqlite3_column_text(statement, 1));
        char *title = g_strdup_printf("Poseta - %s", formatted_date);
        gtk_label_set_text(app->editor_title, title);
        g_free(title); g_free(formatted_date);
        gtk_label_set_text(app->save_state, "Sačuvano");
        gtk_stack_set_visible_child_name(app->stack, "editor");
        gtk_widget_grab_focus(GTK_WIDGET(app->editor));
    }
    sqlite3_finalize(statement);
}

static void finding_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    (void)box;
    load_finding(data, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "finding-id")));
}

static void new_finding_clicked(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    sqlite3_stmt *statement;
    sqlite3_prepare_v2(app->db, "INSERT INTO findings(patient_id) VALUES(?)", -1, &statement, NULL);
    sqlite3_bind_int(statement, 1, app->patient_id);
    if (sqlite3_step(statement) == SQLITE_DONE) {
        int id = (int)sqlite3_last_insert_rowid(app->db);
        sqlite3_finalize(statement);
        load_finding(app, id);
        return;
    }
    sqlite3_finalize(statement);
}

static gboolean save_finding(gpointer data) {
    App *app = data;
    app->autosave_source = 0;
    if (!app->finding_id) return G_SOURCE_REMOVE;
    GtkTextIter start, end;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(app->editor);
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    sqlite3_stmt *statement;
    sqlite3_prepare_v2(app->db, "UPDATE findings SET content=?,updated_at=CURRENT_TIMESTAMP WHERE id=?", -1, &statement, NULL);
    sqlite3_bind_text(statement, 1, text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, app->finding_id);
    if (sqlite3_step(statement) == SQLITE_DONE)
        gtk_label_set_text(app->save_state, "Sačuvano");
    else
        gtk_label_set_text(app->save_state, "Čuvanje nije uspelo");
    sqlite3_finalize(statement);
    g_free(text);
    return G_SOURCE_REMOVE;
}

static void editor_changed(GtkTextBuffer *buffer, gpointer data) {
    (void)buffer;
    App *app = data;
    if (app->loading_editor) return;
    gtk_label_set_text(app->save_state, "Čuvanje...");
    if (app->autosave_source) g_source_remove(app->autosave_source);
    app->autosave_source = g_timeout_add(600, save_finding, app);
}

static void finish_clicked(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (app->autosave_source) {
        g_source_remove(app->autosave_source);
        app->autosave_source = 0;
    }
    save_finding(app);
    sqlite3_stmt *statement;
    sqlite3_prepare_v2(app->db, "UPDATE findings SET status='final',updated_at=CURRENT_TIMESTAMP WHERE id=?", -1, &statement, NULL);
    sqlite3_bind_int(statement, 1, app->finding_id);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
    int patient_id = app->patient_id;
    app->finding_id = 0;
    show_patient(app, patient_id);
    generate_ai_summary(app, patient_id);
}

static void editor_back_clicked(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (app->autosave_source) {
        g_source_remove(app->autosave_source);
        app->autosave_source = 0;
    }
    save_finding(app);
    app->finding_id = 0;
    show_patient(app, app->patient_id);
}

static char *replace_all(const char *text, const char *needle, const char *replacement) {
    GRegex *regex = g_regex_new(needle, G_REGEX_CASELESS, 0, NULL);
    char *result = g_regex_replace_literal(regex, text, -1, 0, replacement, 0, NULL);
    g_regex_unref(regex);
    return result;
}

static char *replace_pattern(const char *text, const char *pattern, const char *replacement) {
    GRegex *regex = g_regex_new(pattern, G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL);
    char *result = g_regex_replace_literal(regex, text, -1, 0, replacement, 0, NULL);
    g_regex_unref(regex);
    return result;
}

static char *redact_literal(const char *text, const char *value, const char *replacement) {
    if (!value || !*value) return g_strdup(text);
    char *escaped = g_regex_escape_string(value, -1);
    char *pattern = g_strdup_printf("(?<![\\p{L}\\p{N}])%s(?![\\p{L}\\p{N}])", escaped);
    char *result = replace_pattern(text, pattern, replacement);
    g_free(pattern);
    g_free(escaped);
    return result;
}

static char *anonymize_text(const char *text, const char *first_name, const char *last_name, const char *date_of_birth) {
    char *full_name = g_strdup_printf("%s %s", first_name, last_name);
    char *value = redact_literal(text ? text : "", full_name, "[PATIENT]");
    char *next = redact_literal(value, first_name, "[PATIENT]"); g_free(value); value = next;
    next = redact_literal(value, last_name, "[PATIENT]"); g_free(value); value = next;
    next = redact_literal(value, date_of_birth, "[DATE REDACTED]"); g_free(value); value = next;

    const char *patterns[] = {
        "[A-Z0-9._%+-]+@[A-Z0-9.-]+\\.[A-Z]{2,}",
        "https?://[^\\s]+",
        "(?<![\\p{L}\\p{N}])(?:\\+?\\d[\\d .()/\\-]{6,}\\d)(?![\\p{L}\\p{N}])",
        "\\b(?:MRN|medical[ ]+record(?:[ ]+number)?|patient[ ]+id|social[ ]+security|SSN|address|patient[ ]+name|name)[ ]*[:#-]?[ ]*[^\\n,;]+"
    };
    const char *replacements[] = {
        "[EMAIL REDACTED]", "[URL REDACTED]", "[NUMBER REDACTED]", "[DIRECT IDENTIFIER REDACTED]"
    };
    for (guint i = 0; i < G_N_ELEMENTS(patterns); i++) {
        next = replace_pattern(value, patterns[i], replacements[i]);
        g_free(value);
        value = next;
    }
    g_free(full_name);
    return value;
}

static gboolean contains_identity(const char *text, const char *first_name, const char *last_name, const char *date_of_birth) {
    const char *identity[] = { first_name, last_name, date_of_birth };
    gboolean found = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(identity) && !found; i++) {
        if (!identity[i] || !*identity[i]) continue;
        char *escaped = g_regex_escape_string(identity[i], -1);
        char *pattern = i < 2
            ? g_strdup_printf("(?<![\\p{L}\\p{N}])%s(?![\\p{L}\\p{N}])", escaped)
            : g_strdup(escaped);
        GRegex *regex = g_regex_new(pattern, G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL);
        found = g_regex_match(regex, text ? text : "", 0, NULL);
        g_regex_unref(regex);
        g_free(pattern);
        g_free(escaped);
    }
    return found;
}

static char *build_document_text(App *app, int finding_id) {
    guint selected = gtk_drop_down_get_selected(app->template_dropdown);
    sqlite3_stmt *statement;
    sqlite3_prepare_v2(app->db, "SELECT body FROM templates ORDER BY id LIMIT 1 OFFSET ?", -1, &statement, NULL);
    sqlite3_bind_int(statement, 1, (int)selected);
    char *body = g_strdup("{patient_name}\n{finding}");
    if (sqlite3_step(statement) == SQLITE_ROW) {
        g_free(body); body = g_strdup((const char *)sqlite3_column_text(statement, 0));
    }
    sqlite3_finalize(statement);

    char *name = NULL, *dob = NULL, *date = NULL, *finding = NULL;
    sqlite3_prepare_v2(app->db,
        "SELECT p.first_name||' '||p.last_name,p.date_of_birth,f.visit_date,f.content "
        "FROM patients p JOIN findings f ON f.patient_id=p.id WHERE f.id=?", -1, &statement, NULL);
    sqlite3_bind_int(statement, 1, finding_id);
    if (sqlite3_step(statement) == SQLITE_ROW) {
        name = g_strdup((const char *)sqlite3_column_text(statement, 0));
        dob = display_date((const char *)sqlite3_column_text(statement, 1));
        date = display_date((const char *)sqlite3_column_text(statement, 2));
        finding = g_strdup((const char *)sqlite3_column_text(statement, 3));
    }
    sqlite3_finalize(statement);
    char *one = replace_all(body, "\\{patient_name\\}", name ? name : "");
    char *two = replace_all(one, "\\{date_of_birth\\}", dob ? dob : "");
    char *three = replace_all(two, "\\{visit_date\\}", date ? date : "");
    char *four = replace_all(three, "\\{finding\\}", finding ? finding : "");
    g_free(body); g_free(one); g_free(two); g_free(three);
    g_free(name); g_free(dob); g_free(date); g_free(finding);
    return four;
}

static void begin_print(GtkPrintOperation *operation, GtkPrintContext *context, gpointer data) {
    PrintData *print = data;
    PangoLayout *layout = gtk_print_context_create_pango_layout(context);
    pango_layout_set_text(layout, print->text, -1);
    pango_layout_set_width(layout, (int)(gtk_print_context_get_width(context) * PANGO_SCALE));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    PangoFontDescription *font = pango_font_description_from_string("Sans 11");
    pango_layout_set_font_description(layout, font);
    int width, height;
    pango_layout_get_pixel_size(layout, &width, &height);
    (void)width;
    print->page_height = gtk_print_context_get_height(context);
    print->pages = MAX(1, (int)((height + print->page_height - 1) / print->page_height));
    gtk_print_operation_set_n_pages(operation, print->pages);
    pango_font_description_free(font);
    g_object_unref(layout);
}

static void draw_page(GtkPrintOperation *operation, GtkPrintContext *context, int page, gpointer data) {
    (void)operation;
    PrintData *print = data;
    cairo_t *cr = gtk_print_context_get_cairo_context(context);
    PangoLayout *layout = gtk_print_context_create_pango_layout(context);
    pango_layout_set_text(layout, print->text, -1);
    pango_layout_set_width(layout, (int)(gtk_print_context_get_width(context) * PANGO_SCALE));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    PangoFontDescription *font = pango_font_description_from_string("Sans 11");
    pango_layout_set_font_description(layout, font);
    cairo_rectangle(cr, 0, 0, gtk_print_context_get_width(context), print->page_height);
    cairo_clip(cr);
    cairo_translate(cr, 0, -page * print->page_height);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(font);
    g_object_unref(layout);
}

static void print_clicked(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (app->autosave_source) { g_source_remove(app->autosave_source); app->autosave_source = 0; }
    save_finding(app);
    PrintData print = { .text = build_document_text(app, app->finding_id), .page_height = 0, .pages = 1 };
    GtkPrintOperation *operation = gtk_print_operation_new();
    gtk_print_operation_set_job_name(operation, "Nalaz lekarskog pregleda");
    g_signal_connect(operation, "begin-print", G_CALLBACK(begin_print), &print);
    g_signal_connect(operation, "draw-page", G_CALLBACK(draw_page), &print);
    GError *error = NULL;
    gtk_print_operation_run(operation, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG, app->window, &error);
    if (error) { show_error(app, "Štampanje nije uspelo", error->message); g_error_free(error); }
    g_object_unref(operation);
    g_free(print.text);
}

static void refresh_templates(App *app) {
    GtkStringList *items = gtk_string_list_new(NULL);
    sqlite3_stmt *statement;
    sqlite3_prepare_v2(app->db, "SELECT name FROM templates ORDER BY id", -1, &statement, NULL);
    while (sqlite3_step(statement) == SQLITE_ROW)
        gtk_string_list_append(items, (const char *)sqlite3_column_text(statement, 0));
    sqlite3_finalize(statement);
    gtk_drop_down_set_model(app->template_dropdown, G_LIST_MODEL(items));
    g_object_unref(items);
}

static void template_response(GtkDialog *dialog, int response, gpointer data) {
    App *app = data;
    if (response == GTK_RESPONSE_OK) {
        GtkEntry *name = g_object_get_data(G_OBJECT(dialog), "name");
        GtkTextView *body = g_object_get_data(G_OBJECT(dialog), "body");
        GtkTextIter start, end;
        gtk_text_buffer_get_bounds(gtk_text_view_get_buffer(body), &start, &end);
        char *text = gtk_text_buffer_get_text(gtk_text_view_get_buffer(body), &start, &end, FALSE);
        sqlite3_stmt *statement;
        sqlite3_prepare_v2(app->db, "INSERT INTO templates(name,body) VALUES(?,?)", -1, &statement, NULL);
        sqlite3_bind_text(statement, 1, gtk_editable_get_text(GTK_EDITABLE(name)), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, text, -1, SQLITE_TRANSIENT);
        sqlite3_step(statement); sqlite3_finalize(statement); g_free(text);
        refresh_templates(app);
    }
    gtk_window_destroy(GTK_WINDOW(dialog));
}

static void add_template_clicked(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    GtkWidget *dialog = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Novi obrazac za štampu");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 650, 500);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), app->window);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Otkaži", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Sačuvaj obrazac", GTK_RESPONSE_OK);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 18); gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18); gtk_widget_set_margin_end(box, 18);
    GtkWidget *name = gtk_entry_new(); GtkWidget *body = gtk_text_view_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(name), "Naziv obrasca");
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(body)),
        "Pacijent: {patient_name}\nDatum rođenja: {date_of_birth}\nDatum posete: {visit_date}\n\n{finding}", -1);
    gtk_box_append(GTK_BOX(box), heading("Obrazac", "dialog-title"));
    gtk_box_append(GTK_BOX(box), name); gtk_box_append(GTK_BOX(box), scrolled(body));
    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), box);
    g_object_set_data(G_OBJECT(dialog), "name", name); g_object_set_data(G_OBJECT(dialog), "body", body);
    g_signal_connect(dialog, "response", G_CALLBACK(template_response), app);
    gtk_window_present(GTK_WINDOW(dialog));
}

static size_t curl_write(char *contents, size_t size, size_t count, void *user_data) {
    size_t bytes = size * count;
    Buffer *buffer = user_data;
    char *next = g_realloc(buffer->data, buffer->size + bytes + 1);
    buffer->data = next;
    memcpy(buffer->data + buffer->size, contents, bytes);
    buffer->size += bytes;
    buffer->data[buffer->size] = '\0';
    return bytes;
}

static void ai_request_free(AiRequest *request) {
    g_free(request->endpoint); g_free(request->key); g_free(request->model);
    g_free(request->prompt); g_free(request->findings); g_free(request->result);
    g_free(request->error); g_free(request);
}

static void ai_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel) {
    (void)source; (void)cancel;
    AiRequest *request = task_data;
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "model"); json_builder_add_string_value(builder, request->model);
    json_builder_set_member_name(builder, "temperature"); json_builder_add_double_value(builder, 0.1);
    json_builder_set_member_name(builder, "messages"); json_builder_begin_array(builder);
    json_builder_begin_object(builder); json_builder_set_member_name(builder, "role"); json_builder_add_string_value(builder, "system");
    json_builder_set_member_name(builder, "content"); json_builder_add_string_value(builder, request->prompt); json_builder_end_object(builder);
    json_builder_begin_object(builder); json_builder_set_member_name(builder, "role"); json_builder_add_string_value(builder, "user");
    json_builder_set_member_name(builder, "content"); json_builder_add_string_value(builder, request->findings); json_builder_end_object(builder);
    json_builder_end_array(builder); json_builder_end_object(builder);
    JsonGenerator *generator = json_generator_new();
    JsonNode *root = json_builder_get_root(builder); json_generator_set_root(generator, root);
    char *payload = json_generator_to_data(generator, NULL);

    CURL *curl = curl_easy_init(); Buffer response = { g_malloc0(1), 0 };
    struct curl_slist *headers = NULL;
    char *authorization = g_strdup_printf("Authorization: Bearer %s", request->key);
    headers = curl_slist_append(headers, "Content-Type: application/json"); headers = curl_slist_append(headers, authorization);
    curl_easy_setopt(curl, CURLOPT_URL, request->endpoint); curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload); curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response); curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    CURLcode code = curl_easy_perform(curl); long status = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (code != CURLE_OK) request->error = g_strdup(curl_easy_strerror(code));
    else if (status < 200 || status >= 300) request->error = g_strdup_printf("Service returned HTTP %ld: %s", status, response.data);
    else {
        JsonParser *parser = json_parser_new(); GError *error = NULL;
        if (json_parser_load_from_data(parser, response.data, -1, &error)) {
            JsonObject *object = json_node_get_object(json_parser_get_root(parser));
            JsonArray *choices = json_object_get_array_member(object, "choices");
            if (choices && json_array_get_length(choices)) {
                JsonObject *choice = json_array_get_object_element(choices, 0);
                JsonObject *message = json_object_get_object_member(choice, "message");
                request->result = g_strdup(json_object_get_string_member(message, "content"));
            } else request->error = g_strdup("Odgovor servisa ne sadrži sažetak.");
        } else { request->error = g_strdup(error->message); g_error_free(error); }
        g_object_unref(parser);
    }
    curl_slist_free_all(headers); curl_easy_cleanup(curl); g_free(authorization); g_free(response.data); g_free(payload);
    json_node_free(root); g_object_unref(generator); g_object_unref(builder);
    g_task_return_boolean(task, request->error == NULL);
}

static void ai_finished(GObject *source, GAsyncResult *result, gpointer data) {
    (void)data;
    App *app = g_object_get_data(source, "app-state");
    GTask *task = G_TASK(result); AiRequest *request = g_task_get_task_data(task);
    if (!app) return;
    if (g_task_propagate_boolean(task, NULL) && request->result) {
        sqlite3_stmt *statement;
        sqlite3_prepare_v2(app->db,
            "UPDATE patients SET ai_summary=? WHERE id=? AND ?=(SELECT COUNT(*) FROM findings WHERE patient_id=? AND status='final')",
            -1, &statement, NULL);
        sqlite3_bind_text(statement, 1, request->result, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 2, request->patient_id);
        sqlite3_bind_int(statement, 3, request->finding_count);
        sqlite3_bind_int(statement, 4, request->patient_id);
        sqlite3_step(statement);
        gboolean saved = sqlite3_changes(app->db) > 0;
        sqlite3_finalize(statement);
        if (saved && app->patient_id == request->patient_id) {
            gtk_label_set_text(app->patient_summary, request->result);
            gtk_label_set_text(app->ai_state, "Sažetak je automatski osvežen");
        } else if (!saved) {
            generate_ai_summary(app, request->patient_id);
        }
    } else if (app->patient_id == request->patient_id) {
        gtk_label_set_text(app->ai_state, "Sažetak nije osvežen; prethodni sažetak je sačuvan");
    }
}

static void generate_ai_summary(App *app, int patient_id) {
    char *endpoint = setting_get(app, "ai_endpoint", ""); char *key = setting_get(app, "ai_key", "");
    char *model = setting_get(app, "ai_model", ""); char *prompt = setting_get(app, "ai_prompt", "");
    if (!*endpoint || !*key || !*model) {
        if (app->patient_id == patient_id)
            gtk_label_set_text(app->ai_state, "Automatski sažetak nije podešen");
        g_free(endpoint); g_free(key); g_free(model); g_free(prompt); return;
    }
    char *first_name = NULL, *last_name = NULL, *date_of_birth = NULL;
    sqlite3_stmt *statement;
    sqlite3_prepare_v2(app->db, "SELECT first_name,last_name,date_of_birth FROM patients WHERE id=?", -1, &statement, NULL);
    sqlite3_bind_int(statement, 1, patient_id);
    if (sqlite3_step(statement) == SQLITE_ROW) {
        first_name = g_strdup((const char *)sqlite3_column_text(statement, 0));
        last_name = g_strdup((const char *)sqlite3_column_text(statement, 1));
        date_of_birth = g_strdup((const char *)sqlite3_column_text(statement, 2));
    }
    sqlite3_finalize(statement);

    GString *findings = g_string_new("Sažmi sledeće anonimizovane longitudinalne kliničke nalaze. Ne izmišljaj činjenice i ne pokušavaj da utvrdiš identitet. Jasno označi svaku neizvesnost.\n\n");
    sqlite3_prepare_v2(app->db, "SELECT content FROM findings WHERE patient_id=? AND status='final' ORDER BY visit_date,id", -1, &statement, NULL);
    sqlite3_bind_int(statement, 1, patient_id);
    int finding_number = 1;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        char *anonymous = anonymize_text((const char *)sqlite3_column_text(statement, 0), first_name, last_name, date_of_birth);
        g_string_append_printf(findings, "Nalaz %d:\n%s\n\n", finding_number++, anonymous);
        g_free(anonymous);
    }
    sqlite3_finalize(statement);

    char *anonymous_prompt = anonymize_text(prompt, first_name, last_name, date_of_birth);
    g_free(prompt);
    if (contains_identity(findings->str, first_name, last_name, date_of_birth) ||
        contains_identity(anonymous_prompt, first_name, last_name, date_of_birth)) {
        show_error(app, "AI zahtev je blokiran", "Lokalna anonimizacija nije uklonila sve poznate podatke o identitetu pacijenta. Zahtev nije poslat.");
        g_string_free(findings, TRUE);
        g_free(anonymous_prompt); g_free(first_name); g_free(last_name); g_free(date_of_birth);
        g_free(endpoint); g_free(key); g_free(model);
        return;
    }
    AiRequest *request = g_new0(AiRequest, 1);
    request->endpoint = endpoint; request->key = key; request->model = model; request->prompt = anonymous_prompt;
    request->findings = g_string_free(findings, FALSE);
    request->patient_id = patient_id;
    request->finding_count = finding_number - 1;
    g_free(first_name); g_free(last_name); g_free(date_of_birth);
    if (app->patient_id == patient_id)
        gtk_label_set_text(app->ai_state, "Automatsko osvežavanje sažetka...");
    GTask *task = g_task_new(G_OBJECT(app->window), NULL, ai_finished, NULL); g_task_set_task_data(task, request, (GDestroyNotify)ai_request_free);
    g_task_run_in_thread(task, ai_worker); g_object_unref(task);
}

static void settings_response(GtkDialog *dialog, int response, gpointer data) {
    App *app = data;
    if (response == GTK_RESPONSE_OK) {
        const char *keys[] = { "ai_endpoint", "ai_key", "ai_model", "ai_prompt" };
        for (int i = 0; i < 4; i++) {
            GtkEntry *entry = g_object_get_data(G_OBJECT(dialog), keys[i]);
            setting_set(app, keys[i], gtk_editable_get_text(GTK_EDITABLE(entry)));
        }
    }
    gtk_window_destroy(GTK_WINDOW(dialog));
}

static void settings_clicked(GtkButton *button, gpointer data) {
    (void)button; App *app = data;
    GtkWidget *dialog = gtk_dialog_new(); gtk_window_set_title(GTK_WINDOW(dialog), "Podešavanja AI servisa");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 620, 480); gtk_window_set_transient_for(GTK_WINDOW(dialog), app->window);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Otkaži", GTK_RESPONSE_CANCEL); gtk_dialog_add_button(GTK_DIALOG(dialog), "Sačuvaj", GTK_RESPONSE_OK);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 20); gtk_widget_set_margin_bottom(box, 20); gtk_widget_set_margin_start(box, 20); gtk_widget_set_margin_end(box, 20);
    gtk_box_append(GTK_BOX(box), heading("Spoljni AI servis", "dialog-title"));
    GtkWidget *notice = gtk_label_new("Šalje se samo lokalno anonimizovan tekst nalaza. Sačuvano ime pacijenta, datum rođenja, broj kartona i datumi poseta nikada se ne dodaju AI zahtevima. Poznata imena i uobičajeni direktni identifikatori uneti u nalaz uklanjaju se lokalno. AI rezultat je savetodavan i lekar ga mora proveriti.");
    gtk_label_set_wrap(GTK_LABEL(notice), TRUE); gtk_label_set_xalign(GTK_LABEL(notice), 0); gtk_widget_add_css_class(notice, "warning-box");
    gtk_box_append(GTK_BOX(box), notice);
    const char *keys[] = { "ai_endpoint", "ai_key", "ai_model", "ai_prompt" };
    const char *fallback[] = { "https://api.openai.com/v1/chat/completions", "", "gpt-4o-mini", "Sažimaš longitudinalne kliničke nalaze za lekara. Budi sažet, hronološki precizan i činjeničan. Nikada ne dodaj dijagnoze ili činjenice kojih nema u izvoru." };
    const char *placeholders[] = { "Adresa Chat Completions servisa", "API ključ", "Model", "Sistemsko uputstvo" };
    for (int i = 0; i < 4; i++) {
        GtkWidget *entry = gtk_entry_new(); char *value = setting_get(app, keys[i], fallback[i]);
        gtk_editable_set_text(GTK_EDITABLE(entry), value); gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholders[i]);
        if (i == 1) gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
        gtk_box_append(GTK_BOX(box), entry); g_object_set_data(G_OBJECT(dialog), keys[i], entry); g_free(value);
    }
    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), box);
    g_signal_connect(dialog, "response", G_CALLBACK(settings_response), app);
    gtk_window_present(GTK_WINDOW(dialog));
}

static GtkWidget *build_home(App *app) {
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *hero = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); gtk_widget_add_css_class(hero, "hero");
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(titles), heading("Kartoni pacijenata", "app-title"));
    gtk_box_append(GTK_BOX(titles), heading("Evidencija pacijenata i nalazi pregleda", "app-subtitle"));
    GtkWidget *settings = gtk_button_new_from_icon_name("emblem-system-symbolic"); gtk_widget_set_tooltip_text(settings, "AI podešavanja");
    gtk_widget_set_hexpand(settings, TRUE); gtk_widget_set_halign(settings, GTK_ALIGN_END);
    g_signal_connect(settings, "clicked", G_CALLBACK(settings_clicked), app);
    gtk_box_append(GTK_BOX(top), titles); gtk_box_append(GTK_BOX(top), settings); gtk_box_append(GTK_BOX(hero), top);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    app->search = GTK_SEARCH_ENTRY(gtk_search_entry_new()); gtk_widget_set_hexpand(GTK_WIDGET(app->search), TRUE);
    gtk_search_entry_set_placeholder_text(app->search, "Pretraga po imenu pacijenta");
    GtkWidget *add = gtk_button_new_with_label("Novi pacijent"); gtk_widget_add_css_class(add, "suggested-action");
    g_signal_connect(app->search, "search-changed", G_CALLBACK(search_changed), app); g_signal_connect(add, "clicked", G_CALLBACK(add_patient_clicked), app);
    gtk_box_append(GTK_BOX(actions), GTK_WIDGET(app->search)); gtk_box_append(GTK_BOX(actions), add); gtk_box_append(GTK_BOX(hero), actions);
    gtk_box_append(GTK_BOX(outer), hero);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12); gtk_widget_add_css_class(content, "content");
    gtk_box_append(GTK_BOX(content), heading("Pacijenti", "section-title"));
    app->patient_list = GTK_LIST_BOX(gtk_list_box_new()); gtk_list_box_set_selection_mode(app->patient_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(app->patient_list), "card-list"); g_signal_connect(app->patient_list, "row-activated", G_CALLBACK(patient_activated), app);
    gtk_box_append(GTK_BOX(content), scrolled(GTK_WIDGET(app->patient_list))); gtk_box_append(GTK_BOX(outer), content);
    return outer;
}

static GtkWidget *build_patient(App *app) {
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12); gtk_widget_add_css_class(header, "patient-header");
    GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10); GtkWidget *back = gtk_button_new_from_icon_name("go-previous-symbolic");
    gtk_widget_set_tooltip_text(back, "Svi pacijenti"); g_signal_connect(back, "clicked", G_CALLBACK(home_clicked), app); gtk_box_append(GTK_BOX(nav), back);
    app->patient_name = GTK_LABEL(heading("Pacijent", "patient-title")); gtk_widget_set_hexpand(GTK_WIDGET(app->patient_name), TRUE);
    gtk_box_append(GTK_BOX(nav), GTK_WIDGET(app->patient_name));
    GtkWidget *new_button = gtk_button_new_with_label("Novi nalaz"); gtk_widget_add_css_class(new_button, "suggested-action");
    g_signal_connect(new_button, "clicked", G_CALLBACK(new_finding_clicked), app); gtk_box_append(GTK_BOX(nav), new_button); gtk_box_append(GTK_BOX(header), nav);
    app->patient_dob = GTK_LABEL(heading("", "app-subtitle")); gtk_box_append(GTK_BOX(header), GTK_WIDGET(app->patient_dob)); gtk_box_append(GTK_BOX(outer), header);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16); gtk_widget_add_css_class(content, "content");
    GtkWidget *summary_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10); gtk_widget_add_css_class(summary_card, "summary-card");
    GtkWidget *summary_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10); gtk_box_append(GTK_BOX(summary_top), heading("Sažetak", "section-title"));
    app->ai_state = GTK_LABEL(gtk_label_new("Automatski se osvežava nakon svakog završenog nalaza"));
    gtk_widget_set_hexpand(GTK_WIDGET(app->ai_state), TRUE); gtk_widget_set_halign(GTK_WIDGET(app->ai_state), GTK_ALIGN_END);
    gtk_widget_add_css_class(GTK_WIDGET(app->ai_state), "ai-state");
    gtk_box_append(GTK_BOX(summary_top), GTK_WIDGET(app->ai_state));
    app->patient_summary = GTK_LABEL(gtk_label_new("")); gtk_label_set_xalign(app->patient_summary, 0); gtk_label_set_wrap(app->patient_summary, TRUE); gtk_label_set_selectable(app->patient_summary, TRUE);
    gtk_box_append(GTK_BOX(summary_card), summary_top); gtk_box_append(GTK_BOX(summary_card), GTK_WIDGET(app->patient_summary));
    gtk_box_append(GTK_BOX(content), summary_card); gtk_box_append(GTK_BOX(content), heading("Istorija poseta", "section-title"));
    app->finding_list = GTK_LIST_BOX(gtk_list_box_new()); gtk_list_box_set_selection_mode(app->finding_list, GTK_SELECTION_NONE); gtk_widget_add_css_class(GTK_WIDGET(app->finding_list), "card-list");
    g_signal_connect(app->finding_list, "row-activated", G_CALLBACK(finding_activated), app); gtk_box_append(GTK_BOX(content), scrolled(GTK_WIDGET(app->finding_list)));
    gtk_box_append(GTK_BOX(outer), content); return outer;
}

static GtkWidget *build_editor(App *app) {
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10); gtk_widget_add_css_class(bar, "editor-bar");
    GtkWidget *back = gtk_button_new_from_icon_name("go-previous-symbolic"); g_signal_connect(back, "clicked", G_CALLBACK(editor_back_clicked), app);
    gtk_box_append(GTK_BOX(bar), back);
    app->editor_title = GTK_LABEL(heading("Poseta", "section-title"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(app->editor_title));
    app->save_state = GTK_LABEL(gtk_label_new("Sačuvano")); gtk_widget_add_css_class(GTK_WIDGET(app->save_state), "save-state");
    gtk_widget_set_hexpand(GTK_WIDGET(app->save_state), TRUE); gtk_widget_set_halign(GTK_WIDGET(app->save_state), GTK_ALIGN_END); gtk_box_append(GTK_BOX(bar), GTK_WIDGET(app->save_state));
    GtkWidget *finish = gtk_button_new_with_label("Završi nalaz"); gtk_widget_add_css_class(finish, "suggested-action"); g_signal_connect(finish, "clicked", G_CALLBACK(finish_clicked), app); gtk_box_append(GTK_BOX(bar), finish);
    gtk_box_append(GTK_BOX(outer), bar);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12); gtk_widget_add_css_class(content, "editor-content");
    app->editor = GTK_TEXT_VIEW(gtk_text_view_new()); gtk_text_view_set_wrap_mode(app->editor, GTK_WRAP_WORD_CHAR); gtk_text_view_set_top_margin(app->editor, 24);
    gtk_text_view_set_bottom_margin(app->editor, 24); gtk_text_view_set_left_margin(app->editor, 28); gtk_text_view_set_right_margin(app->editor, 28);
    gtk_widget_add_css_class(GTK_WIDGET(app->editor), "finding-editor"); g_signal_connect(gtk_text_view_get_buffer(app->editor), "changed", G_CALLBACK(editor_changed), app);
    gtk_box_append(GTK_BOX(content), scrolled(GTK_WIDGET(app->editor)));
    GtkWidget *print_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10); gtk_box_append(GTK_BOX(print_bar), heading("Obrazac za štampu", "small-heading"));
    app->template_dropdown = GTK_DROP_DOWN(gtk_drop_down_new(NULL, NULL)); gtk_widget_set_hexpand(GTK_WIDGET(app->template_dropdown), TRUE); gtk_box_append(GTK_BOX(print_bar), GTK_WIDGET(app->template_dropdown));
    GtkWidget *add_template = gtk_button_new_from_icon_name("list-add-symbolic"); gtk_widget_set_tooltip_text(add_template, "Novi obrazac"); g_signal_connect(add_template, "clicked", G_CALLBACK(add_template_clicked), app); gtk_box_append(GTK_BOX(print_bar), add_template);
    GtkWidget *print = gtk_button_new_with_label("Štampaj nalaz"); g_signal_connect(print, "clicked", G_CALLBACK(print_clicked), app); gtk_box_append(GTK_BOX(print_bar), print);
    gtk_box_append(GTK_BOX(content), print_bar); gtk_box_append(GTK_BOX(outer), content); return outer;
}

static void apply_css(void) {
    const char *css =
        "window { background: #f4f1ea; color: #121c18; }"
        "dialog, dialog box { color: #121c18; }"
        "dialog entry, dialog label { color: #121c18; }"
        "dialog button { color: #121c18; }"
        ".hero { padding: 34px 42px 28px; background: #173f35; color: #f8f6ef; }"
        ".app-title { font-size: 30px; font-weight: 800; letter-spacing: -1px; }"
        ".app-subtitle { color: #b9cdc6; font-size: 14px; }"
        ".content { padding: 26px 42px 34px; color: #121c18; }"
        ".section-title { font-size: 18px; font-weight: 700; }"
        ".dialog-title { font-size: 22px; font-weight: 800; margin-bottom: 8px; }"
        ".card-list { background: transparent; }"
        ".card-list row { background: #fffdf8; color: #121c18; border: 1px solid #ddd9cf; border-radius: 10px; padding: 15px 18px; margin-bottom: 8px; }"
        ".card-list row:hover { background: #f0eee6; }"
        ".card-list row label, .content label { color: #121c18; }"
        ".dim-label { color: #1b2923; opacity: 1; }"
        ".patient-row-name, .visit-date { font-weight: 700; font-size: 16px; }"
        ".patient-header { padding: 24px 42px; background: #173f35; color: #f8f6ef; }"
        ".patient-title { font-size: 26px; font-weight: 800; }"
        ".summary-card { background: #e5eee8; color: #121c18; border-radius: 12px; padding: 18px 20px; }"
        ".summary-card label { color: #121c18; }"
        ".draft-badge, .signed-badge { font-size: 10px; font-weight: 800; padding: 3px 7px; border-radius: 5px; }"
        ".draft-badge { color: #84530d; background: #f4dfb7; } .signed-badge { color: #28543e; background: #cee5d7; }"
        ".finding-excerpt { color: #1b2923; }"
        ".editor-bar { padding: 16px 28px; background: #173f35; color: #f8f6ef; }"
        ".editor-content { padding: 22px 42px 30px; color: #121c18; }"
        ".editor-content label { color: #121c18; }"
        ".editor-content button, .content button { color: #121c18; }"
        "entry, searchentry, dropdown { background-color: #fffdf8; color: #121c18; }"
        "entry text, searchentry text { background-color: #fffdf8; color: #121c18; caret-color: #173f35; }"
        "entry placeholder, searchentry placeholder { color: #4d5b55; opacity: 1; }"
        "dropdown button { background-color: #fffdf8; color: #121c18; }"
        "popover contents { background: #fffdf8; color: #121c18; }"
        "popover contents label { color: #121c18; }"
        ".finding-editor { background: #fffdf8; border: 1px solid #d6d1c6; border-radius: 8px; font-family: serif; font-size: 16px; }"
        ".finding-editor text { background-color: #fffdf8; color: #18231f; caret-color: #173f35; }"
        ".finding-editor text selection { background-color: #b8d8ca; color: #10231c; }"
        ".patient-field { min-height: 28px; padding: 10px 14px; font-size: 16px; }"
        ".ai-state { color: #344b42; font-size: 12px; }"
        ".save-state { color: #b9cdc6; } .small-heading { font-weight: 700; }"
        ".warning-box { background: #f7e8c8; color: #62420b; padding: 12px; border-radius: 8px; }";
    GtkCssProvider *provider = gtk_css_provider_new(); gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void activate(GtkApplication *application, gpointer data) {
    (void)data;
    App *app = g_new0(App, 1); app->application = application;
    if (!open_database(app)) { g_free(app); return; }
    app->window = GTK_WINDOW(gtk_application_window_new(application)); gtk_window_set_title(app->window, "Kartoni pacijenata");
    gtk_window_set_default_size(app->window, 1050, 760); gtk_widget_set_size_request(GTK_WIDGET(app->window), 760, 560);
    app->stack = GTK_STACK(gtk_stack_new()); gtk_stack_set_transition_type(app->stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_add_named(app->stack, build_home(app), "home"); gtk_stack_add_named(app->stack, build_patient(app), "patient"); gtk_stack_add_named(app->stack, build_editor(app), "editor");
    gtk_window_set_child(app->window, GTK_WIDGET(app->stack)); apply_css(); refresh_templates(app); refresh_search(app);
    g_object_set_data_full(G_OBJECT(app->window), "app-state", app, g_free); gtk_window_present(app->window);
}

int main(int argc, char **argv) {
    if (argc == 3 && g_str_equal(argv[1], "--test-docx")) {
        char *error = NULL;
        gboolean passed = write_docx(argv[2],
            "NALAZ LEKARSKOG PREGLEDA\n\nPacijent: Petar Petrović\n"
            "Datum posete: 2026-09-25\n\nNALAZ\nKontrolni pregled je uredan.",
            &error);
        if (!passed)
            g_printerr("docx: %s\n", error ? error : "FAILED");
        g_free(error);
        return passed ? 0 : 1;
    }
    if (argc == 2 && g_str_equal(argv[1], "--test-anonymizer")) {
        const char *source = "JANE DOE (Jane's record), DOB 1980-01-02, email jane.doe@example.com, phone +381 64 123 456, MRN: 998877\nBP stable.";
        char *anonymous = anonymize_text(source, "Jane", "Doe", "1980-01-02");
        gboolean passed = !contains_identity(anonymous, "Jane", "Doe", "1980-01-02") &&
            strstr(anonymous, "jane.doe@example.com") == NULL && strstr(anonymous, "998877") == NULL &&
            strstr(anonymous, "BP stable.") != NULL;
        g_print("anonymizer: %s\n", passed ? "ok" : "FAILED");
        if (!passed) g_printerr("output: %s\n", anonymous);
        g_free(anonymous);
        return passed ? 0 : 1;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    GtkApplication *application = gtk_application_new("com.clinicnotes.desktop", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application); curl_global_cleanup(); return status;
}
