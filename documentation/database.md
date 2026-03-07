# Database

## Schema

Default database path: `db/shadnet.db` (relative to the server working directory). The `db/` subdirectory is created automatically by `open()` if it does not exist.

### migration

Tracks which schema migrations have been applied.

```sql
CREATE TABLE IF NOT EXISTS migration (
    migration_id INTEGER PRIMARY KEY,
    description  TEXT NOT NULL
);
```

### account

One row per registered user.

```sql
CREATE TABLE IF NOT EXISTS account (
    user_id     INTEGER PRIMARY KEY AUTOINCREMENT,
    username    TEXT    NOT NULL,
    hash        BLOB    NOT NULL,
    salt        BLOB    NOT NULL,
    online_name TEXT    NOT NULL,
    avatar_url  TEXT    NOT NULL,
    email       TEXT    NOT NULL,
    email_check TEXT    NOT NULL UNIQUE,
    token       TEXT    NOT NULL,
    reset_token TEXT,
    admin       BOOL    NOT NULL DEFAULT 0,
    stat_agent  BOOL    NOT NULL DEFAULT 0,
    banned      BOOL    NOT NULL DEFAULT 0,
    UNIQUE(username COLLATE NOCASE)
);
```

| Column | Type | Notes |
|---|---|---|
| `user_id` | INTEGER | Auto-assigned primary key |
| `username` | TEXT | NP ID — case-insensitive unique constraint |
| `hash` | BLOB | 32-byte  hash |
| `salt` | BLOB | 64-byte random salt |
| `online_name` | TEXT | Display name shown in-game |
| `avatar_url` | TEXT | May be empty string |
| `email` | TEXT | Original casing preserved |
| `email_check` | TEXT | `email.toLower()` — used for dedup, unique |
| `token` | TEXT | 16-char alphanumeric, used for email validation |
| `reset_token` | TEXT | Nullable — set only during password reset flow |
| `admin` | BOOL | 1 = server administrator |
| `stat_agent` | BOOL | 1 = allowed to submit stats |
| `banned` | BOOL | 1 = login rejected |

### account_timestamp

Tracks account lifecycle timestamps. Separate table to avoid locking `account` on every login.

```sql
CREATE TABLE IF NOT EXISTS account_timestamp (
    user_id    INTEGER NOT NULL PRIMARY KEY,
    creation   INTEGER NOT NULL,
    last_login INTEGER
);
```

| Column | Type | Notes |
|---|---|---|
| `user_id` | INTEGER | Foreign key to `account.user_id` |
| `creation` | INTEGER | Unix seconds — set once at account creation |
| `last_login` | INTEGER | Unix seconds — updated on each successful login; NULL until first login |

---
