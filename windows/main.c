#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <shlobj.h>
#include <winhttp.h>
#include <process.h>
#include <sqlite3.h>
#include <json-c/json.h>
#include <zip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "privacy.h"

enum { HOME, PATIENT, EDITOR };
enum { SEARCH = 100, PATIENTS, ADD_PATIENT, SETTINGS, HOME_BACK, PATIENT_NAME,
       DOB, SUMMARY, AI_STATE, VISITS, NEW_VISIT, EDIT_BACK, EDIT_TITLE, SAVE_STATE,
       NOTE, FINISH, TEMPLATES, ADD_TEMPLATE, PRINT, EXPORT,
       D_FIRST = 200, D_LAST, D_DOB, D_NAME, D_BODY, D_ENDPOINT, D_KEY, D_MODEL, D_PROMPT, D_SAVE, D_CANCEL };
#define AI_DONE (WM_APP + 1)

typedef struct {
    HWND hwnd, page[3], ctl[400], dialog;
    HFONT font, title_font;
    HBRUSH light, dark, white;
    sqlite3 *db;
    int page_id, patient_id, finding_id, readonly, loading, closing, workers;
    HWND editor;
    UINT_PTR autosave;
    int *patient_ids, *finding_ids, *template_ids;
    int patient_count, finding_count, template_count;
    int dialog_kind;
} App;

typedef struct {
    HWND hwnd;
    int patient_id;
    char *endpoint, *key, *model, *prompt, *findings, *snapshot;
    char *result, *error;
} AiJob;

static App app;
static LRESULT CALLBACK main_proc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK dialog_proc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK page_proc(HWND, UINT, WPARAM, LPARAM);

static wchar_t *wide(const char *s) {
    if (!s) s = "";
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (!n) return NULL;
    wchar_t *w = calloc((size_t)n, sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n);
    return w;
}

static char *utf8(const wchar_t *s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *p = malloc((size_t)n);
    if (p) WideCharToMultiByte(CP_UTF8, 0, s, -1, p, n, NULL, NULL);
    return p;
}

static char *text(HWND hwnd) {
    int n = GetWindowTextLengthW(hwnd) + 1;
    wchar_t *w = calloc((size_t)n, sizeof(wchar_t));
    if (!w) return NULL;
    GetWindowTextW(hwnd, w, n);
    char *p = utf8(w);
    free(w);
    return p;
}

static void set(HWND hwnd, const char *value) {
    wchar_t *w = wide(value);
    if (w) { SetWindowTextW(hwnd, w); free(w); }
}

static void alert(const char *message) {
    wchar_t *w = wide(message);
    MessageBoxW(app.hwnd, w ? w : L"Greška", L"Kartoni pacijenata", MB_OK | MB_ICONWARNING);
    free(w);
}

static void sql_exec(const char *sql) {
    char *error = NULL;
    if (sqlite3_exec(app.db, sql, NULL, NULL, &error) != SQLITE_OK) {
        alert(error ? error : "Greška baze podataka"); sqlite3_free(error);
    }
}

static sqlite3_stmt *prepare(const char *query) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(app.db, query, -1, &s, NULL) != SQLITE_OK) alert(sqlite3_errmsg(app.db));
    return s;
}

static char *value(const unsigned char *p) {
    const char *s = (const char *)(p ? p : (const unsigned char *)"");
    char *copy = malloc(strlen(s) + 1);
    if (copy) strcpy(copy, s);
    return copy;
}

static char *setting(const char *key) {
    sqlite3_stmt *s = prepare("SELECT value FROM settings WHERE key=?");
    if (!s) return value(NULL);
    sqlite3_bind_text(s, 1, key, -1, SQLITE_TRANSIENT);
    char *v = sqlite3_step(s) == SQLITE_ROW ? value(sqlite3_column_text(s, 0)) : value(NULL);
    sqlite3_finalize(s);
    return v;
}

static void put_setting(const char *key, const char *v) {
    sqlite3_stmt *s = prepare("INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    if (!s) return;
    sqlite3_bind_text(s, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, v, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s) != SQLITE_DONE) alert(sqlite3_errmsg(app.db));
    sqlite3_finalize(s);
}

static int open_db(void) {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, dir))) return 0;
    if (wcslen(dir) + 25 >= MAX_PATH) return 0;
    wcscat(dir, L"\\clinic-notes");
    CreateDirectoryW(dir, NULL);
    swprintf(path, MAX_PATH, L"%ls\\clinic.db", dir);
    char *file = utf8(path);
    int ok = file && sqlite3_open(file, &app.db) == SQLITE_OK;
    free(file);
    if (!ok) return 0;
    sql_exec("PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL;");
    sql_exec("CREATE TABLE IF NOT EXISTS patients(id INTEGER PRIMARY KEY,first_name TEXT NOT NULL,last_name TEXT NOT NULL,date_of_birth TEXT NOT NULL,ai_summary TEXT NOT NULL DEFAULT '',created_at TEXT DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS findings(id INTEGER PRIMARY KEY,patient_id INTEGER NOT NULL REFERENCES patients(id) ON DELETE CASCADE,content TEXT NOT NULL DEFAULT '',status TEXT NOT NULL DEFAULT 'draft',visit_date TEXT NOT NULL DEFAULT CURRENT_DATE,updated_at TEXT DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS templates(id INTEGER PRIMARY KEY,name TEXT NOT NULL,body TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "INSERT INTO templates(name,body) SELECT 'Standardni pregled','NALAZ LEKARSKOG PREGLEDA\n\nPacijent: {patient_name}\nDatum rođenja: {date_of_birth}\nDatum posete: {visit_date}\n\nNALAZ\n{finding}\n\n\nPotpis lekara: ____________________' WHERE NOT EXISTS(SELECT 1 FROM templates);");
    return 1;
}

static char *display_date(const char *iso) {
    char buf[32];
    if (strlen(iso) == 10 && iso[4] == '-' && iso[7] == '-') {
        snprintf(buf, sizeof(buf), "%.2s.%.2s.%.4s", iso + 8, iso + 5, iso);
        return value((const unsigned char *)buf);
    }
    return value((const unsigned char *)iso);
}

static int parse_date(const char *str, char iso[11]) {
    int d, m, y; char extra;
    if (strlen(str) != 10 || sscanf(str, "%2d.%2d.%4d%c", &d, &m, &y, &extra) != 3) return 0;
    for (int i = 0; i < 10; i++) if (i != 2 && i != 5 && (str[i] < '0' || str[i] > '9')) return 0;
    if (str[2] != '.' || str[5] != '.' || y < 1 || m < 1 || m > 12) return 0;
    int days[] = { 31, 28 + (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)), 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (d < 1 || d > days[m - 1]) return 0;
    snprintf(iso, 11, "%04d-%02d-%02d", y, m, d);
    return 1;
}

static HWND control(HWND parent, int id, const wchar_t *type, const wchar_t *label, DWORD style) {
    HWND h = CreateWindowExW((style & WS_BORDER) ? WS_EX_CLIENTEDGE : 0, type, label,
        WS_CHILD | WS_VISIBLE | style, 0, 0, 100, 30, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)app.font, TRUE);
    if (id < 400) app.ctl[id] = h;
    return h;
}

static HWND label(HWND parent, int id, const wchar_t *text, int title) {
    HWND h = control(parent, id, L"STATIC", text, SS_LEFT | SS_NOTIFY);
    if (title) SendMessageW(h, WM_SETFONT, (WPARAM)app.title_font, TRUE);
    return h;
}

static void place(HWND h, int x, int y, int w, int ht) { MoveWindow(h, x, y, w > 0 ? w : 1, ht, TRUE); }

static void layout(int width, int height) {
    int x = 30, w = width - 60;
    place(app.ctl[SEARCH], x, 96, w - 300, 36); place(app.ctl[ADD_PATIENT], width - 250, 96, 120, 36);
    place(app.ctl[SETTINGS], width - 120, 96, 90, 36);
    place(app.ctl[PATIENTS], x, 160, w, height - 190);
    place(app.ctl[HOME_BACK], 30, 25, 100, 32); place(app.ctl[PATIENT_NAME], 150, 18, w - 300, 44);
    place(app.ctl[NEW_VISIT], width - 170, 25, 140, 34);
    place(app.ctl[DOB], 30, 76, w, 30); place(app.ctl[AI_STATE], 30, 124, w, 25);
    place(app.ctl[SUMMARY], 30, 155, w, 140); place(app.ctl[VISITS], 30, 340, w, height - 370);
    place(app.ctl[EDIT_BACK], 30, 25, 100, 32); place(app.ctl[EDIT_TITLE], 145, 16, w - 440, 48);
    place(app.ctl[SAVE_STATE], width - 312, 32, 135, 28); place(app.ctl[FINISH], width - 165, 22, 135, 38);
    place(app.ctl[NOTE], 30, 82, w, height - 208);
    place(app.ctl[TEMPLATES], 30, height - 105, 220, 34);
    place(app.ctl[ADD_TEMPLATE], 265, height - 105, 115, 34);
    place(app.ctl[PRINT], width - 295, height - 105, 125, 34);
    place(app.ctl[EXPORT], width - 160, height - 105, 130, 34);
}

static void page(int id) {
    for (int i = 0; i < 3; i++) ShowWindow(app.page[i], i == id ? SW_SHOW : SW_HIDE);
    app.page_id = id;
    RECT r; GetClientRect(app.hwnd, &r);
    for (int i = 0; i < 3; i++) place(app.page[i], 0, 0, r.right, r.bottom);
    layout(r.right, r.bottom);
}

static void refresh_search(void) {
    char *q = text(app.ctl[SEARCH]);
    if (!q) return;
    size_t n = strlen(q) + 3; char *pattern = malloc(n);
    if (!pattern) { free(q); return; }
    snprintf(pattern, n, "%%%s%%", q); free(q);
    sqlite3_stmt *s = prepare("SELECT id,first_name,last_name,date_of_birth FROM patients WHERE first_name LIKE ? OR last_name LIKE ? OR (first_name||' '||last_name) LIKE ? ORDER BY last_name,first_name LIMIT 100");
    if (!s) { free(pattern); return; }
    for (int i = 1; i <= 3; i++) sqlite3_bind_text(s, i, pattern, -1, SQLITE_TRANSIENT);
    free(pattern); SendMessageW(app.ctl[PATIENTS], LB_RESETCONTENT, 0, 0);
    free(app.patient_ids); app.patient_ids = calloc(100, sizeof(int)); app.patient_count = 0;
    while (app.patient_ids && sqlite3_step(s) == SQLITE_ROW) {
        const char *first = (const char *)sqlite3_column_text(s, 1);
        const char *last = (const char *)sqlite3_column_text(s, 2);
        char *dob = display_date((const char *)sqlite3_column_text(s, 3));
        size_t capacity = strlen(first) + strlen(last) + strlen(dob) + 32;
        char *buf = malloc(capacity);
        if (!buf) { free(dob); break; }
        snprintf(buf, capacity, "%s %s     |     %s", first, last, dob);
        wchar_t *w = wide(buf);
        if (w) {
            SendMessageW(app.ctl[PATIENTS], LB_ADDSTRING, 0, (LPARAM)w);
            app.patient_ids[app.patient_count++] = sqlite3_column_int(s, 0);
            free(w);
        }
        free(buf); free(dob);
    }
    sqlite3_finalize(s);
}

static void refresh_visits(void) {
    SendMessageW(app.ctl[VISITS], LB_RESETCONTENT, 0, 0);
    free(app.finding_ids); app.finding_ids = NULL; app.finding_count = 0;
    sqlite3_stmt *s = prepare("SELECT id,visit_date,content,status FROM findings WHERE patient_id=? ORDER BY visit_date DESC,id DESC");
    if (!s) return;
    sqlite3_bind_int(s, 1, app.patient_id);
    while (sqlite3_step(s) == SQLITE_ROW) {
        int *p = realloc(app.finding_ids, (size_t)(app.finding_count + 1) * sizeof(int));
        if (!p) break;
        app.finding_ids = p;
        char buf[600]; char *date = display_date((const char *)sqlite3_column_text(s, 1));
        char *excerpt = value(sqlite3_column_text(s, 2));
        for (char *c = excerpt; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
        if (strlen(excerpt) > 160) {
            size_t cut = 160;
            while (cut && ((unsigned char)excerpt[cut] & 0xc0) == 0x80) cut--;
            excerpt[cut] = 0;
        }
        snprintf(buf, sizeof(buf), "%s    [%s]    %.160s", date,
            strcmp((const char *)sqlite3_column_text(s, 3), "draft") == 0 ? "NACRT" : "ZAVRŠENO", excerpt);
        wchar_t *w = wide(buf);
        if (w) {
            SendMessageW(app.ctl[VISITS], LB_ADDSTRING, 0, (LPARAM)w);
            app.finding_ids[app.finding_count++] = sqlite3_column_int(s, 0);
            free(w);
        }
        free(date); free(excerpt);
    }
    sqlite3_finalize(s);
}

static void show_patient(int id) {
    app.patient_id = id; app.finding_id = 0;
    sqlite3_stmt *s = prepare("SELECT first_name,last_name,date_of_birth,ai_summary FROM patients WHERE id=?");
    if (!s) return;
    sqlite3_bind_int(s, 1, id);
    if (sqlite3_step(s) == SQLITE_ROW) {
        char buf[512]; char *dob = display_date((const char *)sqlite3_column_text(s, 2));
        snprintf(buf, sizeof(buf), "%s %s", sqlite3_column_text(s, 0), sqlite3_column_text(s, 1));
        set(app.ctl[PATIENT_NAME], buf);
        snprintf(buf, sizeof(buf), "Datum rođenja: %s", dob); set(app.ctl[DOB], buf);
        const char *summary = (const char *)sqlite3_column_text(s, 3);
        set(app.ctl[SUMMARY], *summary ? summary : "AI sažetak nije generisan. Istorija poseta ostaje izvor kliničkih podataka.");
        set(app.ctl[AI_STATE], *summary ? "Sačuvan lokalno; osvežava se po završenom nalazu" : "Automatski sažetak nakon prvog završenog nalaza");
        free(dob);
    }
    sqlite3_finalize(s); refresh_visits(); page(PATIENT);
}

static void refresh_templates(void) {
    SendMessageW(app.ctl[TEMPLATES], CB_RESETCONTENT, 0, 0);
    free(app.template_ids); app.template_ids = NULL; app.template_count = 0;
    sqlite3_stmt *s = prepare("SELECT id,name FROM templates ORDER BY id");
    if (!s) return;
    while (sqlite3_step(s) == SQLITE_ROW) {
        int *p = realloc(app.template_ids, (size_t)(app.template_count + 1) * sizeof(int));
        if (!p) break;
        app.template_ids = p;
        wchar_t *w = wide((const char *)sqlite3_column_text(s, 1));
        if (w) { SendMessageW(app.ctl[TEMPLATES], CB_ADDSTRING, 0, (LPARAM)w); free(w); }
        app.template_ids[app.template_count++] = sqlite3_column_int(s, 0);
    }
    sqlite3_finalize(s);
    SendMessageW(app.ctl[TEMPLATES], CB_SETCURSEL, 0, 0);
}

static void show_finding(int id) {
    sqlite3_stmt *s = prepare("SELECT content,visit_date,status FROM findings WHERE id=? AND patient_id=?");
    if (!s) return;
    sqlite3_bind_int(s, 1, id); sqlite3_bind_int(s, 2, app.patient_id);
    if (sqlite3_step(s) == SQLITE_ROW) {
        app.finding_id = id; app.loading = 1;
        set(app.editor, (const char *)sqlite3_column_text(s, 0)); app.loading = 0;
        char *date = display_date((const char *)sqlite3_column_text(s, 1));
        char buf[100]; snprintf(buf, sizeof(buf), "Poseta - %s", date); free(date);
        set(app.ctl[EDIT_TITLE], buf); set(app.ctl[SAVE_STATE], "Sačuvano");
        app.readonly = strcmp((const char *)sqlite3_column_text(s, 2), "final") == 0;
        SendMessageW(app.editor, EM_SETREADONLY, app.readonly, 0);
        ShowWindow(app.ctl[FINISH], app.readonly ? SW_HIDE : SW_SHOW);
        page(EDITOR); SetFocus(app.editor);
    }
    sqlite3_finalize(s);
}

static int save_draft(void) {
    if (!app.finding_id || app.readonly) return 1;
    if (app.autosave) { KillTimer(app.hwnd, app.autosave); app.autosave = 0; }
    char *body = text(app.editor);
    if (!body) return 0;
    sqlite3_stmt *s = prepare("UPDATE findings SET content=?,updated_at=CURRENT_TIMESTAMP WHERE id=? AND patient_id=? AND status='draft'");
    if (!s) { free(body); return 0; }
    sqlite3_bind_text(s, 1, body, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, app.finding_id); sqlite3_bind_int(s, 3, app.patient_id);
    int ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(app.db) == 1;
    sqlite3_finalize(s); free(body);
    set(app.ctl[SAVE_STATE], ok ? "Sačuvano" : "Čuvanje nije uspelo");
    if (!ok) alert("Nalaz nije sačuvan. Pokušajte ponovo pre zatvaranja.");
    return ok;
}

static char *snapshot(int id) {
    sqlite3_stmt *s = prepare("SELECT id,content FROM findings WHERE patient_id=? AND status='final' ORDER BY visit_date,id");
    if (!s) return NULL;
    sqlite3_bind_int(s, 1, id);
    char *result = value((const unsigned char *)""); size_t len = 0;
    while (result && sqlite3_step(s) == SQLITE_ROW) {
        const char *body = (const char *)sqlite3_column_text(s, 1); size_t n = strlen(body);
        char prefix[100]; int k = snprintf(prefix, sizeof(prefix), "%d:%zu:", sqlite3_column_int(s, 0), n);
        char *p = realloc(result, len + (size_t)k + n + 1);
        if (!p) { free(result); result = NULL; break; }
        result = p; memcpy(result + len, prefix, (size_t)k); len += (size_t)k;
        memcpy(result + len, body, n); len += n; result[len] = 0;
    }
    sqlite3_finalize(s); return result;
}

static char *replace_literal(const char *input, const char *needle, const char *replacement) {
    if (!input || !needle || !*needle) return NULL;
    size_t a = strlen(needle), b = strlen(replacement), length = strlen(input), count = 0;
    for (const char *p = input; (p = strstr(p, needle)); p += a) count++;
    char *out = malloc(length + count * (b > a ? b - a : 0) + 1);
    if (!out) return NULL;
    char *dest = out; const char *p = input, *next;
    while ((next = strstr(p, needle))) {
        size_t n = (size_t)(next - p); memcpy(dest, p, n); dest += n;
        memcpy(dest, replacement, b); dest += b; p = next + a;
    }
    strcpy(dest, p); return out;
}

static char *document(void) {
    int selected = (int)SendMessageW(app.ctl[TEMPLATES], CB_GETCURSEL, 0, 0);
    if (selected < 0 || selected >= app.template_count || !app.finding_id) return NULL;
    sqlite3_stmt *s = prepare("SELECT body FROM templates WHERE id=?"); if (!s) return NULL;
    sqlite3_bind_int(s, 1, app.template_ids[selected]);
    char *body = sqlite3_step(s) == SQLITE_ROW ? value(sqlite3_column_text(s, 0)) : NULL;
    sqlite3_finalize(s); if (!body) return NULL;
    s = prepare("SELECT p.first_name||' '||p.last_name,p.date_of_birth,f.visit_date,f.content FROM patients p JOIN findings f ON f.patient_id=p.id WHERE p.id=? AND f.id=?");
    if (!s) { free(body); return NULL; }
    sqlite3_bind_int(s, 1, app.patient_id); sqlite3_bind_int(s, 2, app.finding_id);
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *keys[] = { "{patient_name}", "{date_of_birth}", "{visit_date}", "{finding}" };
        for (int i = 0; body && i < 4; i++) {
            char *date = i == 1 || i == 2 ? display_date((const char *)sqlite3_column_text(s, i)) : NULL;
            char *next = replace_literal(body, keys[i], date ? date : (const char *)sqlite3_column_text(s, i));
            free(date); free(body); body = next;
        }
    }
    sqlite3_finalize(s); return body;
}

static void print_document(void) {
    char *doc = document(); if (!doc) { alert("Nije moguće pripremiti dokument."); return; }
    wchar_t *w = wide(doc); free(doc); if (!w) return;
    PRINTDLGW dialog = { .lStructSize = sizeof(dialog), .hwndOwner = app.hwnd, .Flags = PD_RETURNDC | PD_NOSELECTION | PD_NOPAGENUMS };
    if (PrintDlgW(&dialog)) {
        DOCINFOW info = { .cbSize = sizeof(info), .lpszDocName = L"Nalaz lekarskog pregleda" };
        HWND renderer = CreateWindowExW(0, MSFTEDIT_CLASS, L"", WS_CHILD | ES_MULTILINE,
            0, 0, 0, 0, app.hwnd, NULL, GetModuleHandleW(NULL), NULL);
        if (renderer) {
            SendMessageW(renderer, EM_EXLIMITTEXT, 0, 1024 * 1024);
            SetWindowTextW(renderer, w);
            SendMessageW(renderer, WM_SETFONT, (WPARAM)app.font, FALSE);
        }
        FORMATRANGE range = { .hdc = dialog.hDC, .hdcTarget = dialog.hDC };
        int dpiX = GetDeviceCaps(dialog.hDC, LOGPIXELSX), dpiY = GetDeviceCaps(dialog.hDC, LOGPIXELSY);
        range.rcPage.right = MulDiv(GetDeviceCaps(dialog.hDC, HORZRES), 1440, dpiX);
        range.rcPage.bottom = MulDiv(GetDeviceCaps(dialog.hDC, VERTRES), 1440, dpiY);
        range.rc.left = 1080; range.rc.top = 1080;
        range.rc.right = range.rcPage.right - 1080; range.rc.bottom = range.rcPage.bottom - 1080;
        range.chrg.cpMin = 0; range.chrg.cpMax = -1;
        if (!renderer || StartDocW(dialog.hDC, &info) <= 0) alert("Štampanje nije uspelo.");
        else {
            LONG total = (LONG)SendMessageW(renderer, WM_GETTEXTLENGTH, 0, 0);
            do {
                if (StartPage(dialog.hDC) <= 0) break;
                LONG next = (LONG)SendMessageW(renderer, EM_FORMATRANGE, TRUE, (LPARAM)&range);
                EndPage(dialog.hDC);
                if (next <= range.chrg.cpMin) break;
                range.chrg.cpMin = next;
            } while (range.chrg.cpMin < total);
            SendMessageW(renderer, EM_FORMATRANGE, FALSE, 0);
            EndDoc(dialog.hDC);
        }
        if (renderer) DestroyWindow(renderer);
        DeleteDC(dialog.hDC);
        if (dialog.hDevMode) GlobalFree(dialog.hDevMode);
        if (dialog.hDevNames) GlobalFree(dialog.hDevNames);
    }
    free(w);
}

static void xml_append(char **buffer, size_t *length, const char *source) {
    size_t n = strlen(source); char *p = realloc(*buffer, *length + n + 1);
    if (!p) return;
    *buffer = p; memcpy(p + *length, source, n + 1); *length += n;
}

static int zip_text(zip_t *zip, const char *name, const char *text) {
    zip_source_t *src = zip_source_buffer(zip, text, strlen(text), 0);
    if (!src) return 0;
    if (zip_file_add(zip, name, src, ZIP_FL_ENC_UTF_8) < 0) { zip_source_free(src); return 0; }
    return 1;
}

static void export_docx(void) {
    char *doc = document(); if (!doc) { alert("Nije moguće pripremiti dokument."); return; }
    wchar_t filename[MAX_PATH] = L"nalaz.docx";
    OPENFILENAMEW ofn = { .lStructSize = sizeof(ofn), .hwndOwner = app.hwnd, .lpstrFilter = L"Word dokument (*.docx)\0*.docx\0\0",
        .lpstrFile = filename, .nMaxFile = MAX_PATH, .Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST,
        .lpstrDefExt = L"docx" };
    if (!GetSaveFileNameW(&ofn)) { free(doc); return; }
    char *path = utf8(filename); if (!path) { free(doc); return; }
    int error; zip_t *zip = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (!zip) { alert("Nije moguće otvoriti Word datoteku."); free(path); free(doc); return; }
    const char *types = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/xml\"/><Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/></Types>";
    const char *rels = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/></Relationships>";
    char *xml = NULL; size_t length = 0;
    xml_append(&xml, &length, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>");
    if (!xml) { zip_discard(zip); free(path); free(doc); return; }
    for (const char *p = doc; *p;) {
        const char *end = strchr(p, '\n'); if (!end) end = p + strlen(p);
        xml_append(&xml, &length, "<w:p><w:r><w:rPr><w:lang w:val=\"sr-Latn-RS\"/></w:rPr><w:t xml:space=\"preserve\">");
        for (const char *c = p; c < end; c++) {
            switch (*c) {
            case '&': xml_append(&xml, &length, "&amp;"); break;
            case '<': xml_append(&xml, &length, "&lt;"); break;
            case '>': xml_append(&xml, &length, "&gt;"); break;
            case '"': xml_append(&xml, &length, "&quot;"); break;
            default: { char ch[2] = { *c, 0 }; xml_append(&xml, &length, ch); }
            }
        }
        xml_append(&xml, &length, "</w:t></w:r></w:p>"); p = *end ? end + 1 : end;
    }
    xml_append(&xml, &length, "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/><w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\"/></w:sectPr></w:body></w:document>");
    int ok = zip_text(zip, "[Content_Types].xml", types) && zip_text(zip, "_rels/.rels", rels) && zip_text(zip, "word/document.xml", xml);
    if (ok) { if (zip_close(zip) != 0) { zip_discard(zip); ok = 0; } }
    else zip_discard(zip);
    if (!ok) alert("Izvoz Word dokumenta nije uspeo.");
    else alert("Word dokument je sačuvan.");
    free(path); free(doc); free(xml);
}

static void free_job(AiJob *j) {
    free(j->endpoint); free(j->key); free(j->model); free(j->prompt);
    free(j->findings); free(j->snapshot); free(j->result); free(j->error); free(j);
}

static unsigned __stdcall ai_worker(void *arg) {
    AiJob *j = arg;
    wchar_t *url = wide(j->endpoint);
    URL_COMPONENTS parts = { .dwStructSize = sizeof(parts), .dwSchemeLength = (DWORD)-1,
        .dwHostNameLength = (DWORD)-1, .dwUrlPathLength = (DWORD)-1, .dwExtraInfoLength = (DWORD)-1 };
    HINTERNET session = NULL, server = NULL, request = NULL;
    if (!url || !WinHttpCrackUrl(url, 0, 0, &parts)) {
        j->error = value((const unsigned char *)"Neispravna adresa AI servisa."); goto done;
    }
    wchar_t *host = calloc(parts.dwHostNameLength + 1, sizeof(wchar_t));
    wchar_t *path = calloc(parts.dwUrlPathLength + parts.dwExtraInfoLength + 2, sizeof(wchar_t));
    if (!host || !path) { j->error = value((const unsigned char *)"Nema dovoljno memorije."); free(host); free(path); goto done; }
    wcsncpy(host, parts.lpszHostName, parts.dwHostNameLength);
    if (parts.dwUrlPathLength) wcsncpy(path, parts.lpszUrlPath, parts.dwUrlPathLength);
    else wcscpy(path, L"/");
    if (parts.dwExtraInfoLength) wcsncat(path, parts.lpszExtraInfo, parts.dwExtraInfoLength);
    int secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && !(parts.nScheme == INTERNET_SCHEME_HTTP &&
        (!wcscmp(host, L"localhost") || !wcscmp(host, L"127.0.0.1") || !wcscmp(host, L"::1")))) {
        j->error = value((const unsigned char *)"HTTP je dozvoljen samo za localhost; za udaljeni servis koristite HTTPS.");
        free(host); free(path); goto done;
    }
    session = WinHttpOpen(L"ClinicNotes/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session) WinHttpSetTimeouts(session, 10000, 10000, 30000, 60000);
    if (session) server = WinHttpConnect(session, host, parts.nPort, 0);
    if (server) request = WinHttpOpenRequest(server, L"POST", path, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    free(host); free(path);
    if (!request) { j->error = value((const unsigned char *)"AI servis nije dostupan."); goto done; }
    struct json_object *payload = json_object_new_object(), *messages = json_object_new_array();
    struct json_object *system = json_object_new_object(), *user = json_object_new_object();
    json_object_object_add(payload, "model", json_object_new_string(j->model));
    json_object_object_add(payload, "temperature", json_object_new_double(0.1));
    json_object_object_add(system, "role", json_object_new_string("system"));
    json_object_object_add(system, "content", json_object_new_string(j->prompt));
    json_object_object_add(user, "role", json_object_new_string("user"));
    json_object_object_add(user, "content", json_object_new_string(j->findings));
    json_object_array_add(messages, system); json_object_array_add(messages, user);
    json_object_object_add(payload, "messages", messages);
    const char *body = json_object_to_json_string_ext(payload, JSON_C_TO_STRING_PLAIN);
    wchar_t *key = wide(j->key); size_t hlen = key ? wcslen(key) + 100 : 0;
    wchar_t *headers = calloc(hlen, sizeof(wchar_t));
    if (headers && key) swprintf(headers, hlen, L"Content-Type: application/json\r\nAuthorization: Bearer %ls", key);
    free(key);
    BOOL ok = headers && WinHttpSendRequest(request, headers, (DWORD)-1L, (LPVOID)body,
        (DWORD)strlen(body), (DWORD)strlen(body), 0) && WinHttpReceiveResponse(request, NULL);
    free(headers); json_object_put(payload);
    if (!ok) { j->error = value((const unsigned char *)"AI zahtev nije uspeo ili je istekao."); goto done; }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300) { j->error = value((const unsigned char *)"AI servis je vratio grešku."); goto done; }
    char *response = value((const unsigned char *)""); size_t length = 0;
    for (;;) {
        char chunk[4096]; DWORD read = 0;
        if (!WinHttpReadData(request, chunk, sizeof(chunk), &read)) { free(response); response = NULL; break; }
        if (!read) break;
        if (length + read > 4 * 1024 * 1024) { free(response); response = NULL; break; }
        char *next = realloc(response, length + read + 1);
        if (!next) { free(response); response = NULL; break; }
        response = next; memcpy(response + length, chunk, read); length += read; response[length] = 0;
    }
    if (response) {
        struct json_object *root = json_tokener_parse(response), *choices, *first, *message, *content;
        if (root && json_object_object_get_ex(root, "choices", &choices) &&
            json_object_is_type(choices, json_type_array) && json_object_array_length(choices) &&
            (first = json_object_array_get_idx(choices, 0)) &&
            json_object_object_get_ex(first, "message", &message) &&
            json_object_object_get_ex(message, "content", &content) && json_object_is_type(content, json_type_string))
            j->result = value((const unsigned char *)json_object_get_string(content));
        if (root) json_object_put(root);
    }
    free(response);
    if (!j->result || !*j->result) { free(j->result); j->result = NULL;
        j->error = value((const unsigned char *)"AI servis nije vratio sažetak."); }
done:
    if (request) WinHttpCloseHandle(request);
    if (server) WinHttpCloseHandle(server);
    if (session) WinHttpCloseHandle(session);
    free(url);
    if (!PostMessageW(j->hwnd, AI_DONE, 0, (LPARAM)j)) free_job(j);
    return 0;
}

static void generate_summary(int id) {
    AiJob *j = calloc(1, sizeof(*j)); if (!j) return;
    j->hwnd = app.hwnd; j->patient_id = id;
    j->endpoint = setting("ai_endpoint"); j->key = setting("ai_key"); j->model = setting("ai_model");
    j->prompt = setting("ai_prompt"); j->snapshot = snapshot(id);
    if (!j->endpoint || !*j->endpoint || !j->model || !*j->model || !j->snapshot) {
        set(app.ctl[AI_STATE], "Automatski sažetak nije podešen"); free_job(j); return;
    }
    sqlite3_stmt *s = prepare("SELECT first_name,last_name,date_of_birth FROM patients WHERE id=?");
    if (!s) { free_job(j); return; }
    sqlite3_bind_int(s, 1, id);
    if (sqlite3_step(s) != SQLITE_ROW) { sqlite3_finalize(s); free_job(j); return; }
    char *first = value(sqlite3_column_text(s, 0)), *last = value(sqlite3_column_text(s, 1));
    char *dob = value(sqlite3_column_text(s, 2));
    sqlite3_finalize(s);
    if (j->prompt && !*j->prompt) {
        free(j->prompt);
        j->prompt = value((const unsigned char *)"Sažmi kliničke nalaze za lekara. Budi sažet i činjeničan. Ne dodaj dijagnoze ni činjenice kojih nema u izvoru.");
    }
    char *clean_prompt = anonymize(j->prompt, first, last, dob);
    free(j->prompt); j->prompt = clean_prompt;
    s = prepare("SELECT content FROM findings WHERE patient_id=? AND status='final' ORDER BY visit_date,id");
    if (!s) { free(first); free(last); free(dob); free_job(j); return; }
    sqlite3_bind_int(s, 1, id);
    size_t capacity = 1024, length = 0; j->findings = calloc(capacity, 1);
    const char *intro = "Sažmi sledeće anonimizovane kliničke nalaze hronološki. Ne izmišljaj činjenice ili identitet.\n\n";
    if (j->findings) { strcpy(j->findings, intro); length = strlen(intro); }
    int count = 0;
    while (j->findings && sqlite3_step(s) == SQLITE_ROW) {
        char *clean = anonymize((const char *)sqlite3_column_text(s, 0), first, last, dob);
        if (!clean) { free(j->findings); j->findings = NULL; break; }
        char number[40]; snprintf(number, sizeof(number), "Nalaz %d:\n", ++count);
        size_t needed = length + strlen(number) + strlen(clean) + 3;
        if (needed > capacity) {
            capacity = needed * 2; char *p = realloc(j->findings, capacity);
            if (!p) { free(j->findings); j->findings = NULL; free(clean); break; }
            j->findings = p;
        }
        strcat(j->findings, number); strcat(j->findings, clean); strcat(j->findings, "\n\n");
        length = strlen(j->findings); free(clean);
    }
    sqlite3_finalize(s);
    if (!j->prompt || !j->findings || !count || contains_identity(j->prompt, first, last, dob) ||
        contains_identity(j->findings, first, last, dob)) {
        alert("AI zahtev je blokiran: anonimizacija nije uspela ili nema završenih nalaza.");
        free(first); free(last); free(dob); free_job(j); return;
    }
    free(first); free(last); free(dob);
    set(app.ctl[AI_STATE], "Automatsko osvežavanje sažetka...");
    InterlockedIncrement((volatile LONG *)&app.workers);
    uintptr_t thread = _beginthreadex(NULL, 0, ai_worker, j, 0, NULL);
    if (thread) CloseHandle((HANDLE)thread);
    else { InterlockedDecrement((volatile LONG *)&app.workers); free_job(j); alert("Nije moguće pokrenuti AI zahtev."); }
}

static void ai_finished(AiJob *j) {
    char *now = snapshot(j->patient_id);
    int current = now && !strcmp(now, j->snapshot);
    free(now);
    if (j->result && current) {
        sqlite3_stmt *s = prepare("UPDATE patients SET ai_summary=? WHERE id=?");
        if (s) {
            sqlite3_bind_text(s, 1, j->result, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(s, 2, j->patient_id);
            if (sqlite3_step(s) == SQLITE_DONE && app.patient_id == j->patient_id) {
                set(app.ctl[SUMMARY], j->result); set(app.ctl[AI_STATE], "Sažetak je automatski osvežen");
            }
            sqlite3_finalize(s);
        }
    } else if (!current && !app.closing) generate_summary(j->patient_id);
    else if (app.patient_id == j->patient_id) set(app.ctl[AI_STATE], "Sažetak nije osvežen; prethodni sažetak je sačuvan");
    free_job(j);
    if (InterlockedDecrement((volatile LONG *)&app.workers) == 0 && app.closing) DestroyWindow(app.hwnd);
}

static void dialog_open(int kind) {
    if (app.dialog) { SetForegroundWindow(app.dialog); return; }
    app.dialog_kind = kind;
    const wchar_t *title = kind == ADD_PATIENT ? L"Novi pacijent" : kind == ADD_TEMPLATE ? L"Novi obrazac" : L"AI podešavanja";
    app.dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ClinicNotesDialog", title,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 640,
        kind == SETTINGS ? 530 : kind == ADD_TEMPLATE ? 450 : 310,
        app.hwnd, NULL, GetModuleHandleW(NULL), NULL);
    ShowWindow(app.dialog, SW_SHOW); SetForegroundWindow(app.dialog);
}

static void dialog_save(HWND h) {
    if (app.dialog_kind == ADD_PATIENT) {
        char *first = text(app.ctl[D_FIRST]), *last = text(app.ctl[D_LAST]), *dob = text(app.ctl[D_DOB]);
        char iso[11];
        if (!first || !last || !dob || !*first || !*last || !parse_date(dob, iso)) {
            alert("Unesite ime, prezime i ispravan datum rođenja (DD.MM.GGGG).");
        } else {
            sqlite3_stmt *s = prepare("INSERT INTO patients(first_name,last_name,date_of_birth) VALUES(?,?,?)");
            if (s) {
                sqlite3_bind_text(s, 1, first, -1, SQLITE_TRANSIENT); sqlite3_bind_text(s, 2, last, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 3, iso, -1, SQLITE_TRANSIENT);
                if (sqlite3_step(s) == SQLITE_DONE) { int id = (int)sqlite3_last_insert_rowid(app.db);
                    sqlite3_finalize(s); DestroyWindow(h); show_patient(id); free(first); free(last); free(dob); return; }
                sqlite3_finalize(s); alert("Pacijent nije dodat.");
            }
        }
        free(first); free(last); free(dob);
    } else if (app.dialog_kind == ADD_TEMPLATE) {
        char *name = text(app.ctl[D_NAME]), *body = text(app.ctl[D_BODY]);
        if (!name || !body || !*name || !*body) alert("Naziv i sadržaj obrasca su obavezni.");
        else {
            sqlite3_stmt *s = prepare("INSERT INTO templates(name,body) VALUES(?,?)");
            if (s) {
                sqlite3_bind_text(s, 1, name, -1, SQLITE_TRANSIENT); sqlite3_bind_text(s, 2, body, -1, SQLITE_TRANSIENT);
                if (sqlite3_step(s) == SQLITE_DONE) { sqlite3_finalize(s); free(name); free(body);
                    DestroyWindow(h); refresh_templates(); return; }
                sqlite3_finalize(s); alert("Obrazac nije sačuvan.");
            }
        }
        free(name); free(body);
    } else {
        const char *keys[] = { "ai_endpoint", "ai_key", "ai_model", "ai_prompt" };
        int ids[] = { D_ENDPOINT, D_KEY, D_MODEL, D_PROMPT };
        char *values[4]; for (int i = 0; i < 4; i++) values[i] = text(app.ctl[ids[i]]);
        int valid = values[0] && (!strncmp(values[0], "https://", 8) ||
            !strncmp(values[0], "http://localhost:", 17) || !strncmp(values[0], "http://127.0.0.1:", 17) || !*values[0]);
        if (!valid) alert("Koristite HTTPS ili lokalni HTTP (localhost / 127.0.0.1).");
        else { for (int i = 0; i < 4; i++) put_setting(keys[i], values[i] ? values[i] : ""); DestroyWindow(h); }
        for (int i = 0; i < 4; i++) free(values[i]);
    }
}

static LRESULT CALLBACK dialog_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case WM_CREATE: {
        int kind = app.dialog_kind;
        if (kind == ADD_PATIENT) {
            place(label(h, 0, L"Ime", 0), 24, 20, 100, 24);
            place(control(h, D_FIRST, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL), 24, 45, 570, 30);
            place(label(h, 0, L"Prezime", 0), 24, 85, 100, 24);
            place(control(h, D_LAST, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL), 24, 110, 570, 30);
            place(label(h, 0, L"Datum rođenja (DD.MM.GGGG)", 0), 24, 150, 350, 24);
            place(control(h, D_DOB, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL), 24, 175, 570, 30);
        } else if (kind == ADD_TEMPLATE) {
            place(label(h, 0, L"Naziv obrasca", 0), 24, 20, 350, 24);
            place(control(h, D_NAME, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL), 24, 45, 570, 30);
            place(control(h, D_BODY, L"EDIT", L"Pacijent: {patient_name}\r\nDatum rođenja: {date_of_birth}\r\nDatum posete: {visit_date}\r\n\r\n{finding}",
                WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN), 24, 90, 570, 235);
        } else {
            place(label(h, 0, L"Šalje se samo anonimizovan tekst. Lekar mora proveriti AI sažetak.", 0), 24, 10, 580, 42);
            int ids[] = { D_ENDPOINT, D_KEY, D_MODEL, D_PROMPT };
            const wchar_t *names[] = { L"Chat Completions URL", L"API ključ (nije šifrovan u bazi)", L"Model", L"Sistemsko uputstvo" };
            const char *keys[] = { "ai_endpoint", "ai_key", "ai_model", "ai_prompt" };
            for (int i = 0; i < 4; i++) {
                place(label(h, 0, names[i], 0), 24, 57 + i * 90, 570, 22);
                DWORD style = WS_BORDER | (i == 3 ? ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL : ES_AUTOHSCROLL);
                if (i == 1) style |= ES_PASSWORD;
                HWND field = control(h, ids[i], L"EDIT", L"", style);
                place(field, 24, 82 + i * 90, 570, i == 3 ? 67 : 29);
                char *v = setting(keys[i]); set(field, v); free(v);
            }
        }
        place(control(h, D_SAVE, L"BUTTON", L"Sačuvaj", BS_DEFPUSHBUTTON), 390,
            kind == SETTINGS ? 427 : kind == ADD_TEMPLATE ? 340 : 217, 96, 32);
        place(control(h, D_CANCEL, L"BUTTON", L"Otkaži", BS_PUSHBUTTON), 498,
            kind == SETTINGS ? 427 : kind == ADD_TEMPLATE ? 340 : 217, 96, 32);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == D_SAVE) dialog_save(h);
        else if (LOWORD(wp) == D_CANCEL) DestroyWindow(h);
        return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: app.dialog = NULL; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void make_ui(HWND h) {
    for (int i = 0; i < 3; i++)
        app.page[i] = CreateWindowExW(0, L"ClinicNotesPage", L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 100, 100,
            h, NULL, GetModuleHandleW(NULL), NULL);
    label(app.page[HOME], 0, L"Kartoni pacijenata", 1);
    place(GetWindow(app.page[HOME], GW_CHILD), 30, 22, 640, 54);
    control(app.page[HOME], SEARCH, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL);
    control(app.page[HOME], ADD_PATIENT, L"BUTTON", L"Novi pacijent", BS_PUSHBUTTON);
    control(app.page[HOME], SETTINGS, L"BUTTON", L"AI", BS_PUSHBUTTON);
    control(app.page[HOME], PATIENTS, L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT);
    control(app.page[PATIENT], HOME_BACK, L"BUTTON", L"Nazad", BS_PUSHBUTTON);
    label(app.page[PATIENT], PATIENT_NAME, L"Pacijent", 1);
    control(app.page[PATIENT], NEW_VISIT, L"BUTTON", L"Novi nalaz", BS_PUSHBUTTON);
    label(app.page[PATIENT], DOB, L"", 0);
    label(app.page[PATIENT], AI_STATE, L"", 0);
    control(app.page[PATIENT], SUMMARY, L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL);
    control(app.page[PATIENT], VISITS, L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT);
    control(app.page[EDITOR], EDIT_BACK, L"BUTTON", L"Nazad", BS_PUSHBUTTON);
    label(app.page[EDITOR], EDIT_TITLE, L"Poseta", 1);
    label(app.page[EDITOR], SAVE_STATE, L"Sačuvano", 0);
    control(app.page[EDITOR], FINISH, L"BUTTON", L"Završi nalaz", BS_PUSHBUTTON);
    app.editor = control(app.page[EDITOR], NOTE, MSFTEDIT_CLASS, L"",
        WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN);
    SendMessageW(app.editor, EM_EXLIMITTEXT, 0, 1024 * 1024);
    control(app.page[EDITOR], TEMPLATES, L"COMBOBOX", L"", WS_VSCROLL | CBS_DROPDOWNLIST);
    control(app.page[EDITOR], ADD_TEMPLATE, L"BUTTON", L"Novi obrazac", BS_PUSHBUTTON);
    control(app.page[EDITOR], PRINT, L"BUTTON", L"Štampaj", BS_PUSHBUTTON);
    control(app.page[EDITOR], EXPORT, L"BUTTON", L"Izvezi Word", BS_PUSHBUTTON);
    refresh_templates(); refresh_search(); page(HOME);
}

static LRESULT CALLBACK main_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        app.hwnd = h;
        if (!open_db()) { alert("Baza podataka nije dostupna."); return -1; }
        app.font = CreateFontW(-17, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        app.title_font = CreateFontW(-30, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        app.light = CreateSolidBrush(RGB(244, 241, 234)); app.dark = CreateSolidBrush(RGB(23, 63, 53));
        app.white = CreateSolidBrush(RGB(255, 253, 248)); make_ui(h); return 0;
    case WM_SIZE:
        if (app.page[HOME]) {
            for (int i = 0; i < 3; i++) place(app.page[i], 0, 0, LOWORD(lp), HIWORD(lp));
            layout(LOWORD(lp), HIWORD(lp));
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (app.closing) return 0;
        if (id == SEARCH && HIWORD(wp) == EN_CHANGE) refresh_search();
        else if (id == PATIENTS && HIWORD(wp) == LBN_DBLCLK) {
            int selected = (int)SendMessageW(app.ctl[PATIENTS], LB_GETCURSEL, 0, 0);
            if (selected >= 0 && selected < app.patient_count) show_patient(app.patient_ids[selected]);
        } else if (id == VISITS && HIWORD(wp) == LBN_DBLCLK) {
            int selected = (int)SendMessageW(app.ctl[VISITS], LB_GETCURSEL, 0, 0);
            if (selected >= 0 && selected < app.finding_count) show_finding(app.finding_ids[selected]);
        } else if (id == NOTE && HIWORD(wp) == EN_CHANGE && !app.loading && !app.readonly && app.finding_id) {
            set(app.ctl[SAVE_STATE], "Čuvanje...");
            if (app.autosave) KillTimer(h, app.autosave);
            app.autosave = SetTimer(h, 1, 600, NULL);
        } else if (HIWORD(wp) == BN_CLICKED) {
            switch (id) {
            case ADD_PATIENT: dialog_open(ADD_PATIENT); break;
            case SETTINGS: dialog_open(SETTINGS); break;
            case HOME_BACK: app.patient_id = 0; refresh_search(); page(HOME); break;
            case NEW_VISIT: {
                sqlite3_stmt *s = prepare("INSERT INTO findings(patient_id) VALUES(?)");
                if (s) { sqlite3_bind_int(s, 1, app.patient_id);
                    if (sqlite3_step(s) == SQLITE_DONE) { int n = (int)sqlite3_last_insert_rowid(app.db);
                        sqlite3_finalize(s); show_finding(n); break; } sqlite3_finalize(s); alert("Nalaz nije dodat."); }
                break;
            }
            case EDIT_BACK: if (save_draft()) show_patient(app.patient_id); break;
            case FINISH: if (save_draft()) {
                sqlite3_stmt *s = prepare("UPDATE findings SET status='final' WHERE id=? AND patient_id=? AND status='draft'");
                if (s) { sqlite3_bind_int(s, 1, app.finding_id); sqlite3_bind_int(s, 2, app.patient_id);
                    int ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(app.db) == 1;
                    sqlite3_finalize(s); if (ok) { int patient = app.patient_id;
                        show_patient(patient); generate_summary(patient); } else alert("Nalaz nije završen."); }
            } break;
            case ADD_TEMPLATE: if (save_draft()) dialog_open(ADD_TEMPLATE); break;
            case PRINT: if (save_draft()) print_document(); break;
            case EXPORT: if (save_draft()) export_docx(); break;
            }
        }
        return 0;
    }
    case WM_TIMER: if (wp == 1) save_draft(); return 0;
    case AI_DONE: ai_finished((AiJob *)lp); return 0;
    case WM_CTLCOLOREDIT: SetBkColor((HDC)wp, RGB(255, 253, 248)); return (LRESULT)app.white;
    case WM_CTLCOLORSTATIC: SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, RGB(22, 39, 32)); return (LRESULT)app.light;
    case WM_CLOSE:
        if (app.page_id == EDITOR && !save_draft()) return 0;
        if (app.dialog) DestroyWindow(app.dialog);
        if (app.workers) { app.closing = 1; EnableWindow(h, FALSE); ShowWindow(h, SW_HIDE); return 0; }
        DestroyWindow(h); return 0;
    case WM_DESTROY:
        if (app.db) sqlite3_close(app.db);
        free(app.patient_ids); free(app.finding_ids); free(app.template_ids);
        DeleteObject(app.font); DeleteObject(app.title_font);
        DeleteObject(app.light); DeleteObject(app.dark); DeleteObject(app.white);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK page_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        RECT rect; GetClientRect(h, &rect);
        FillRect((HDC)wp, &rect, app.light);
        rect.bottom = 72; FillRect((HDC)wp, &rect, app.dark);
        return 1;
    }
    if (msg == WM_CTLCOLORSTATIC) {
        RECT rect; GetWindowRect((HWND)lp, &rect);
        MapWindowPoints(HWND_DESKTOP, h, (POINT *)&rect, 2);
        if (rect.top < 72) {
            SetBkMode((HDC)wp, TRANSPARENT);
            SetTextColor((HDC)wp, RGB(248, 246, 239));
            return (LRESULT)app.dark;
        }
    }
    if (msg == WM_COMMAND || msg == WM_NOTIFY || msg == WM_CTLCOLORSTATIC ||
        msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX)
        return SendMessageW(GetParent(h), msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show) {
    (void)previous; (void)command;
    LoadLibraryW(L"Msftedit.dll");
    INITCOMMONCONTROLSEX init = { .dwSize = sizeof(init), .dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&init);
    WNDCLASSW cls = { .lpfnWndProc = main_proc, .hInstance = instance,
        .lpszClassName = L"ClinicNotesMain", .hCursor = LoadCursorW(NULL, IDC_ARROW),
        .hbrBackground = (HBRUSH)(COLOR_WINDOW + 1) };
    RegisterClassW(&cls);
    cls.lpfnWndProc = dialog_proc; cls.lpszClassName = L"ClinicNotesDialog";
    RegisterClassW(&cls);
    cls.lpfnWndProc = page_proc; cls.lpszClassName = L"ClinicNotesPage";
    RegisterClassW(&cls);
    HWND window = CreateWindowExW(0, L"ClinicNotesMain", L"Kartoni pacijenata", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1050, 760, NULL, NULL, instance, NULL);
    if (!window) return 1;
    ShowWindow(window, show); UpdateWindow(window);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (app.dialog && IsDialogMessageW(app.dialog, &msg)) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
