# Clinic Notes

A native Linux desktop prototype for a doctor's office, built with C, GTK 4,
SQLite, libcurl, and JSON-GLib.

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

## Print placeholders

Templates support `{patient_name}`, `{date_of_birth}`, `{visit_date}`, and
`{finding}`.

## Clinical and deployment notice

This is a functional prototype, not certified medical-record software. Before
using it with real patient data, add user authentication, database encryption,
role-based access, audit logging, backups, retention policies, consent handling,
and the safeguards required by local health-data law. AI summaries are advisory:
the clinician must verify them.

The AI request contains only finding text anonymized locally. The application
never adds the patient's stored name, date of birth, record ID, or visit dates to
the request. It redacts the known first name, last name, full name, date of birth,
email addresses, URLs, phone/long-number patterns, and common labeled direct
identifiers before networking. A final fail-closed check blocks the request if a
known patient identity remains. Free text can contain indirect or unusual
identifiers that deterministic rules cannot reliably recognize, so deployment
still requires an approved privacy review and provider agreement.
