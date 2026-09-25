# Clinic Notes for Windows

Native C/Win32 version of the Linux clinic-notes prototype. No .NET runtime,
Electron, browser, or GTK runtime is used for the Windows UI.

## Why C and Win32?

- C keeps the same low-level language as the Linux application and calls the
  operating system directly. `CreateWindowExW` creates the windows and controls,
  and `WndProc` handles events. Unicode Win32 APIs are used for Serbian text.
- Standard `LISTBOX` controls present patients and visits; `EDIT` controls
  provide search, forms, and read-only summaries; the Windows Rich Edit control
  provides multiline finding editing and paginated printing. `COMBOBOX` selects
  print templates. Windows `PrintDlgW` and `GetSaveFileNameW` provide native
  printer and file dialogs. The header uses GDI brushes, not a web renderer.
- SQLite preserves the Linux database schema. PCRE2 handles Unicode-aware local
  redaction, json-c constructs/parses Chat Completions JSON, and libzip packages
  DOCX. These are supporting libraries, **not UI frameworks**. Network requests
  use WinHTTP on a worker thread; printing uses GDI/Rich Edit pagination.

## Build (Windows 10/11, x64)

Install [MSYS2](https://www.msys2.org/), open its **UCRT64** terminal, then:

```bash
pacman -S --needed make mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-sqlite3 mingw-w64-ucrt-x86_64-pcre2 mingw-w64-ucrt-x86_64-json-c mingw-w64-ucrt-x86_64-libzip
cd /path/to/clinic-notes/windows
make
make test
./clinic-notes.exe
```

From a Windows command prompt, run `clinic-notes.exe` after adding MSYS2's
`ucrt64/bin` to `PATH`, or bundle the dependency DLLs found by running
`ldd clinic-notes.exe` in the UCRT64 terminal. The application itself uses
only Win32 controls and does not require the MSYS2 shell at runtime.

## Data and workflow

- SQLite: `%LOCALAPPDATA%\clinic-notes\clinic.db`. Patient names, dates of birth,
  findings, templates, AI summaries and AI settings persist here. All patient
  text is stored locally. Data is **not encrypted**, nor is the saved API key.
- Search patients, add a patient (DOB `DD.MM.GGGG`), double-click a patient and
  create a visit. Drafts autosave after 600 ms, and are saved before navigation,
  printing or export. Finalized visits are read-only. Double-click a visit to
  review, print or export it. The visit list is ordered newest first.
- Add custom print templates with `{patient_name}`, `{date_of_birth}`,
  `{visit_date}`, and `{finding}`. Export creates a Word `.docx` file; print uses
  the selected Windows printer. Printed/exported documents contain identity.
- Configure an OpenAI-compatible Chat Completions URL, model, and optional API
  key under **AI**. For local Ollama use
  `http://127.0.0.1:11434/v1/chat/completions` and an installed model name.
  Remote endpoints require HTTPS. The AI summary updates automatically after
  finalization; the previous summary is kept if the request fails. Only locally
  anonymized finding text and prompt are sent; dates and database IDs are not.
  Names, DOB, email, URL, number and common labeled-identifier patterns are
  redacted before the request. An identity check blocks the request if a known
  identifier survives. Indirect identifiers are not reliably caught by rules.

To move an existing Linux database, **close both applications first** and use
SQLite's `.backup` command to create a consistent copy of the Linux database
(it uses WAL). Place the resulting `clinic.db` at the Windows data path above;
do not commit a database or real patient data to GitHub.

## Clinical notice

This is a prototype, not certified medical-record software. Do not deploy with
real patient records until access control, encryption (including API keys),
audit trails, backups, retention, legal consent, privacy review and a clinical
validation process are in place. A clinician must verify every generated summary.
