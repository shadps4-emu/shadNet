# Database

## Schema

Database path: `db/shadnet.db`, relative to the executable directory. The `db/` subdirectory is created automatically. Migrations are applied when the database is opened.

### migration

Tracks which schema migrations have been applied.

```sql
CREATE TABLE IF NOT EXISTS migration (
    migration_id UNSIGNED INTEGER PRIMARY KEY,
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
    avatar_url  TEXT    NOT NULL,
    email       TEXT    NOT NULL,
    email_check TEXT    NOT NULL UNIQUE,
    token       TEXT    NOT NULL,
    reset_token TEXT,
    admin       BOOL    NOT NULL,
    stat_agent  BOOL    NOT NULL,
    banned      BOOL    NOT NULL,
    ban_reason  TEXT,
    ban_timestamp INTEGER,
    client_version TEXT,
    client_version_at UNSIGNED BIGINT,
    UNIQUE(username COLLATE NOCASE)
);
```

| Column              | Type            | Notes                                                                                            |
| ------------------- | --------------- | ------------------------------------------------------------------------------------------------ |
| `user_id`           | INTEGER         | Auto-assigned primary key                                                                        |
| `username`          | TEXT            | NP ID, case-insensitive unique constraint                                                        |
| `hash`              | BLOB            | 32-byte hash                                                                                     |
| `salt`              | BLOB            | 64-byte random salt                                                                              |
| `avatar_url`        | TEXT            | May be empty string                                                                              |
| `email`             | TEXT            | Original casing preserved                                                                        |
| `email_check`       | TEXT            | `email.toLower().trimmed()`, used for dedup, unique                                              |
| `token`             | TEXT            | 16-char alphanumeric account token, used by the game WebAPI and optionally checked at game login |
| `reset_token`       | TEXT            | Nullable, currently unused                                                                       |
| `admin`             | BOOL            | 1 = server administrator                                                                         |
| `stat_agent`        | BOOL            | Stored flag, currently unused                                                                    |
| `banned`            | BOOL            | 1 = login rejected                                                                               |
| `ban_reason`        | TEXT            | Reason for the ban                                                                               |
| `ban_timestamp`     | INTEGER         | Unix seconds when the ban was set                                                                |
| `client_version`    | TEXT            | Last reported client version                                                                     |
| `client_version_at` | UNSIGNED BIGINT | Unix seconds when the version was updated                                                        |

### account_timestamp

Tracks account lifecycle timestamps.

```sql
CREATE TABLE IF NOT EXISTS account_timestamp (
    user_id         UNSIGNED BIGINT  NOT NULL PRIMARY KEY,
    creation        UNSIGNED INTEGER NOT NULL,
    last_login      UNSIGNED INTEGER,
    token_last_sent UNSIGNED INTEGER,
    reset_emit      UNSIGNED INTEGER
);
```

| Column            | Type             | Notes                                                                       |
| ----------------- | ---------------- | --------------------------------------------------------------------------- |
| `user_id`         | UNSIGNED BIGINT  | Account ID; no foreign-key constraint is declared here                      |
| `creation`        | UNSIGNED INTEGER | Unix seconds, set once at account creation                                  |
| `last_login`      | UNSIGNED INTEGER | Unix seconds, updated on each successful game login; NULL until first login |
| `token_last_sent` | UNSIGNED INTEGER | Legacy field; no email-token dispatch is implemented                        |
| `reset_emit`      | UNSIGNED INTEGER | Legacy field; no email-reset flow is implemented                            |

---

### friendship

One row per pair of users that have any relationship: a pending request, a confirmed friendship, or a block. The row is deleted entirely when no flags remain on either side.

```sql
CREATE TABLE IF NOT EXISTS friendship (
    user_id_1     INTEGER NOT NULL REFERENCES account(user_id) ON DELETE CASCADE,
    user_id_2     INTEGER NOT NULL REFERENCES account(user_id) ON DELETE CASCADE,
    status_user_1 INTEGER NOT NULL DEFAULT 0,
    status_user_2 INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(user_id_1, user_id_2),
    CHECK(user_id_1 < user_id_2)
);

CREATE INDEX IF NOT EXISTS friendship_user1 ON friendship(user_id_1);
CREATE INDEX IF NOT EXISTS friendship_user2 ON friendship(user_id_2);
```

| Column          | Type    | Notes                                                             |
| --------------- | ------- | ----------------------------------------------------------------- |
| `user_id_1`     | INTEGER | The lower of the two user IDs, enforced by the `CHECK` constraint |
| `user_id_2`     | INTEGER | The higher of the two user IDs                                    |
| `status_user_1` | INTEGER | Bitmask of `FriendStatus` flags from `user_id_1`'s perspective    |
| `status_user_2` | INTEGER | Bitmask of `FriendStatus` flags from `user_id_2`'s perspective    |

**`FriendStatus` bitmask:**

| Flag      | Value  | Meaning                                         |
| --------- | ------ | ----------------------------------------------- |
| `Friend`  | `0x01` | This user has sent or accepted a friend request |
| `Blocked` | `0x02` | This user has blocked the other                 |

**Relationship states** (interpreting the two status columns from one user's perspective):

| My flags    | Other's flags | State                                         |
| ----------- | ------------- | --------------------------------------------- |
| `Friend=1`  | `Friend=0`    | I sent a request, pending acceptance          |
| `Friend=0`  | `Friend=1`    | They sent me a request, pending my acceptance |
| `Friend=1`  | `Friend=1`    | Mutual friendship                             |
| `Blocked=1` | any           | I have blocked them                           |

A single row can represent both a block and a pending/confirmed friendship simultaneously (e.g. one user blocks the other after accepting a request), though the `AddFriend` command rejects any row where either side has `Blocked` set.

The `CHECK(user_id_1 < user_id_2)` constraint means there is always exactly one row per pair regardless of who initiated. The DB layer (`GetRelStatus`, `SetRelStatus`) handles the column-order swap transparently so callers never need to think about which column belongs to which user.

---

### score_table

One row per leaderboard, identified by the combination of `communication_id` and `board_id`. Stores the board configuration. Rows are seeded at startup from `scoreboards.cfg` and can also be created on demand by `RecordScore` and `GetBoardInfos` when no matching row exists.

```sql
CREATE TABLE IF NOT EXISTS score_table (
    communication_id  TEXT    NOT NULL,
    board_id          INTEGER NOT NULL,
    rank_limit        INTEGER NOT NULL DEFAULT 100,
    update_mode       INTEGER NOT NULL DEFAULT 0,
    sort_mode         INTEGER NOT NULL DEFAULT 0,
    upload_num_limit  INTEGER NOT NULL DEFAULT 10,
    upload_size_limit INTEGER NOT NULL DEFAULT 6000000,
    PRIMARY KEY(communication_id, board_id)
);
```

| Column              | Type    | Notes                                                                                                             |
| ------------------- | ------- | ----------------------------------------------------------------------------------------------------------------- |
| `communication_id`  | TEXT    | 12-character PSN Communication ID, e.g. `NPWR12345_00`                                                            |
| `board_id`          | INTEGER | Board number within the communication ID; games typically use 1, 2, 3…                                            |
| `rank_limit`        | INTEGER | Maximum number of scores in the ranked cache; other database records are retained                                 |
| `update_mode`       | INTEGER | `0` = NORMAL_UPDATE, store equal or better scores; `1` = FORCE_UPDATE, always overwrite                           |
| `sort_mode`         | INTEGER | `0` = DESCENDING, higher score ranks better (most games); `1` = ASCENDING, lower score ranks better (time trials) |
| `upload_num_limit`  | INTEGER | Advertised upload count; not currently enforced                                                                   |
| `upload_size_limit` | INTEGER | Advertised upload size; not currently enforced                                                                    |

---

### score

One row per (communication_id, board_id, user_id, character_id) combination, a player's stored score for a specific character slot on a specific board.

```sql
CREATE TABLE IF NOT EXISTS score (
    communication_id TEXT    NOT NULL,
    board_id         INTEGER NOT NULL,
    user_id          INTEGER NOT NULL,
    character_id     INTEGER NOT NULL,
    score            INTEGER NOT NULL,
    comment          TEXT,
    game_info        BLOB,
    data_id          INTEGER,
    timestamp        INTEGER NOT NULL,
    PRIMARY KEY(communication_id, board_id, user_id, character_id)
);
```

| Column             | Type    | Notes                                                                                                         |
| ------------------ | ------- | ------------------------------------------------------------------------------------------------------------- |
| `communication_id` | TEXT    | Matches `score_table.communication_id`                                                                        |
| `board_id`         | INTEGER | Matches `score_table.board_id`                                                                                |
| `user_id`          | INTEGER | Account ID; no foreign-key constraint is declared here                                                        |
| `character_id`     | INTEGER | Character/slot index; games that don't use multiple characters always use `0`                                 |
| `score`            | INTEGER | The score value; sign and magnitude depend on the game                                                        |
| `comment`          | TEXT    | Optional player-supplied comment submitted with the score; NULL if omitted                                    |
| `game_info`        | BLOB    | Optional small inline game data blob submitted with the score (not the large game data file); NULL if omitted |
| `data_id`          | INTEGER | ID of the associated score-data file in `score_data/`; NULL until `RecordScoreData` is called                 |
| `timestamp`        | INTEGER | PSN timestamp in microseconds since the Common Era epoch (0001-01-01), set at the time of submission          |

**Score lifecycle:**

`RecordScore` inserts or replaces a row depending on `update_mode`. With `NORMAL_UPDATE` the `WHERE excluded.score >= score` (or `<= score` for ASCENDING) clause means the INSERT only takes effect if the new score is equal or better — if not, `numRowsAffected()` returns 0 and the server returns `ScoreNotBest`. An accepted replacement clears any existing score-data attachment.

After a successful `RecordScore`, the client may optionally call `RecordScoreData` to attach a binary blob (replay data, ghost car data, etc.). This writes a file to the `score_data/` directory and sets `data_id` to the file's numeric ID. The `data_id` column stays NULL until that second call is made. `GetScoreData` reads the file back given a player's NP ID.

**Score-data files:**

Large per-score blobs are stored on disk as `score_data/<20-digit-id>.sdt` rather than in SQLite. This avoids bloating the database with multi-megabyte BLOBs. The `data_id` column is the link between a score row and its file. On server startup, any `.sdt` files whose IDs are not referenced by any `data_id` column are deleted as orphans.

---

### entitlement

Entitlements granted to accounts (Entitlements Web API).  Rows arecreated and removed through the admin API;,games only read them and consume uses.

```sql
CREATE TABLE IF NOT EXISTS entitlement (
    user_id          INTEGER NOT NULL,
    title_id         TEXT    NOT NULL DEFAULT '',
    service_label    INTEGER NOT NULL DEFAULT 0,
    label            TEXT    NOT NULL,
    entitlement_type TEXT    NOT NULL DEFAULT 'service',
    active_date      INTEGER NOT NULL,
    inactive_date    INTEGER,
    is_consumable    INTEGER NOT NULL DEFAULT 0,
    use_count        INTEGER NOT NULL DEFAULT 0,
    use_limit        INTEGER NOT NULL DEFAULT 0,
    granted_by       TEXT,
    granted_at       INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(user_id, title_id, service_label, label)
);
```

| Column             | Type    | Notes                                                                           |
| ------------------ | ------- | ------------------------------------------------------------------------------- |
| `user_id`          | INTEGER | Account ID                                                                      |
| `title_id`         | TEXT    | Title serial the game reports at sign-in (e.g. `CUSA00003`); `''` = every title |
| `service_label`    | INTEGER | NP service label; games pass it as `service_label` (default `0`)                |
| `label`            | TEXT    | Entitlement label, returned to the game as `id`                                 |
| `entitlement_type` | TEXT    | `service` or `unified`                                                          |
| `active_date`      | INTEGER | Unix seconds when it becomes usable                                             |
| `inactive_date`    | INTEGER | Unix seconds when it stops being usable; NULL = permanent                       |
| `is_consumable`    | INTEGER | `1` for consumables                                                             |
| `use_count`        | INTEGER | Uses consumed so far                                                            |
| `use_limit`        | INTEGER | Uses remaining; `use_count + use_limit` is the total granted                    |
| `granted_by`       | TEXT    | Admin who last granted or changed it                                            |
| `granted_at`       | INTEGER | Unix seconds of that change                                                     |

A game sees the rows for its own `title_id` plus the `''` rows,when both have the same label,
the title-specific row wins. Deleting an account deletes its rows.

### commerce_label

Store labels games have asked about through the Commerce Web API (GET Container). One row per title serial, service label and label,written by the server on every
container request and listed by `GET /admin/v1/commerce/labels`.

```sql
CREATE TABLE IF NOT EXISTS commerce_label (
    title_id      TEXT    NOT NULL DEFAULT '',
    service_label INTEGER NOT NULL DEFAULT 0,
    label         TEXT    NOT NULL,
    first_seen    INTEGER NOT NULL,
    last_seen     INTEGER NOT NULL,
    requests      INTEGER NOT NULL DEFAULT 1,
    PRIMARY KEY(title_id, service_label, label)
);
```

| Column          | Type    | Notes                                                                      |
| --------------- | ------- | -------------------------------------------------------------------------- |
| `title_id`      | TEXT    | Title serial the request came from; `''` when the client didn't report one |
| `service_label` | INTEGER | NP service label of the request (default `0`)                              |
| `label`         | TEXT    | Product label the game asked about                                         |
| `first_seen`    | INTEGER | Unix seconds of the first request                                          |
| `last_seen`     | INTEGER | Unix seconds of the latest request                                         |
| `requests`      | INTEGER | How many requests included it                                              |

### game_custom_data

Game custom data sent through the Game Custom Data Web API (`POST /v1/gameCustomData/items`):
one row per sent item, shared by all its recipients. Recipients only see items sent from the same
game: the same NP Communication ID when both sides have one, otherwise the same title serial.

```sql
CREATE TABLE IF NOT EXISTS game_custom_data (
    item_id                INTEGER PRIMARY KEY AUTOINCREMENT,
    from_user_id           INTEGER NOT NULL,
    communication_id       TEXT    NOT NULL DEFAULT '',
    title_id               TEXT    NOT NULL DEFAULT '',
    message                TEXT    NOT NULL DEFAULT '',
    data_name              TEXT    NOT NULL,
    data_description       TEXT    NOT NULL,
    localized_names        TEXT    NOT NULL DEFAULT '{}',
    localized_descriptions TEXT    NOT NULL DEFAULT '{}',
    attachment_type        TEXT    NOT NULL DEFAULT 'gameData',
    url                    TEXT,
    game_data              BLOB,
    thumbnail              BLOB,
    platforms              TEXT    NOT NULL DEFAULT 'PS4',
    created_at             INTEGER NOT NULL,
    expires_at             INTEGER
);
```

| Column                   | Type    | Notes                                                                 |
| ------------------------ | ------- | --------------------------------------------------------------------- |
| `item_id`                | INTEGER | The game custom data ID games see                                     |
| `from_user_id`           | INTEGER | Sender's account                                                      |
| `communication_id`       | TEXT    | Sender's NP Communication ID at send time; `''` when unknown          |
| `title_id`               | TEXT    | Sender's title serial at send time; `''` when unknown                 |
| `message`                | TEXT    | The sender's message (up to 512 characters)                           |
| `data_name`              | TEXT    | Default name (up to 128 characters)                                   |
| `data_description`       | TEXT    | Default description (up to 512 characters)                            |
| `localized_names`        | TEXT    | JSON object, `npLanguage` -> name                                     |
| `localized_descriptions` | TEXT    | JSON object, `npLanguage` -> description                              |
| `attachment_type`        | TEXT    | `gameData` or `url`                                                   |
| `url`                    | TEXT    | Attached URL, with `itemId=<id>` appended when asked; URL items only |
| `game_data`              | BLOB    | Attached game data (up to 1 MiB); game data items only               |
| `thumbnail`              | BLOB    | Thumbnail image (up to 160 KiB)                                       |
| `platforms`              | TEXT    | Comma list of `PS4` / `PSVITA`                                        |
| `created_at`             | INTEGER | Unix seconds; reported as `receivedDate`                              |
| `expires_at`             | INTEGER | Unix seconds; `NULL` = never expires                                  |

### game_custom_data_recipient

Who received each item, with their own "used" flag (set by `PUT .../items/<id>`). Each user keeps
their newest 100 items; older ones are dropped, and an item nobody holds any more is deleted.
Deleting an account deletes the items it sent and its received rows.

```sql
CREATE TABLE IF NOT EXISTS game_custom_data_recipient (
    item_id   INTEGER NOT NULL REFERENCES game_custom_data(item_id) ON DELETE CASCADE,
    user_id   INTEGER NOT NULL,
    data_used INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(item_id, user_id)
);
CREATE INDEX IF NOT EXISTS game_custom_data_recipient_user
    ON game_custom_data_recipient(user_id, item_id);
```
