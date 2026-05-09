# Vendored dependencies

These files are pulled verbatim from upstream and committed into the repo so
the server builds offline. Do not edit them. To bump versions, see
`make update-mongoose`, `make update-cjson`, and `make update-sqlite` in the
top-level Makefile.

| File(s)                                | Library       | Version    | Source |
|----------------------------------------|---------------|------------|--------|
| `mongoose.c`, `mongoose.h`             | mongoose      | 7.21       | https://raw.githubusercontent.com/cesanta/mongoose/7.21/ |
| `cJSON.c`, `cJSON.h`                   | cJSON         | 1.7.19     | https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.19/ |
| `sqlite3.c`, `sqlite3.h`, `sqlite3ext.h` | SQLite amalgamation | 3.53.1 (3530100) | https://sqlite.org/2026/sqlite-amalgamation-3530100.zip |

## Licenses (summary)

- mongoose: MIT (see header in `mongoose.c`).
- cJSON: MIT (see header in `cJSON.c`).
- SQLite: Public domain.
