# Clinic Notes

A desktop prototype for a doctor's office. The Linux application uses C/GTK 4;
the Windows application in [`windows/`](windows/README.md) uses C and native
Win32 controls, WinHTTP and GDI. Both use the same SQLite schema.

## Features

- Patient search and patient creation
- Complete chronological visit history
- New finding editor with debounced autosave
- OpenAI-compatible AI summary configuration with automatic refresh after each completed visit
- Local AI-boundary anonymization with fail-closed identity checks
- Editable print templates using placeholders
- Native GTK print dialog and print preview

## Build and run

```bash
make
./clinic-notes
```

Data is stored in `~/.local/share/clinic-notes/clinic.db`.

For the native Windows build and data location, see [windows/README.md](windows/README.md).

## Print placeholders

Templates support `{patient_name}`, `{date_of_birth}`, `{visit_date}`, and
`{finding}`.

## Clinical and deployment notice

This is a functional prototype, not certified medical-record software. Before
using it with real patient data, add user authentication, database encryption,
role-based access, audit logging, backups, retention policies, consent handling,
and the safeguards required by local health-data law. AI summaries are advisory:
the clinician must verify them.

The Windows native application uses C and Win32 controls, with the Linux
prototype in `src/` using GTK4. Both share the same SQLite schema and logic.
To test the Windows app on Linux, see the "Simulation" below or build
with wine/mingw.

## Windows native app

See [`windows/README.md`](windows/README.md) for build/install details.

## Linux simulation

A GTK4 version of the Windows UI was added in [`simulation/`](simulation) to
allow UI testing and validation on Linux without Wine. It uses the same SQLite
and logic as both platforms, including:

- Patient CRUD, visit management, autosave drafts
- AI summary configuration (same settings schema), automatic refresh on
  finalization
- Local, fail-closed text-only anonymization before any AI request
- Editable print templates (`.docx` export uses libzip; GTK printing uses
  GTK print dialog)
- Window drag, keyboard navigation, focus management, tooltips, proper fonts
- Text extraction from Rich Edit for printing; console simulation of Word
document pagination with use of libcurl/libjson to fetch Chat Completions

Build:
```bash
make -C simulation
./simulation/clinic-notes
```
Test:
```bash
make test -C simulation
```

The AI request contains only finding text anonymized locally. The application
never adds the patient's stored name, date of birth, record ID, or visit dates to
the request. It redacts the known first name, last name, full name, date of birth,
email addresses, URLs, phone/long-number patterns, and common labeled direct
identifiers before networking. A final fail-closed check blocks the request if a
known patient identity remains. Free text can contain indirect or unusual
identifiers that deterministic rules cannot reliably recognize, so deployment
still requires an approved privacy review and provider agreement.
