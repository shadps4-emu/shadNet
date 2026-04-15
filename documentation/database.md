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

| Column        | Type    | Notes                                           |
| ------------- | ------- | ----------------------------------------------- |
| `user_id`     | INTEGER | Auto-assigned primary key                       |
| `username`    | TEXT    | NP ID,case-insensitive unique constraint        |
| `hash`        | BLOB    | 32-byte  hash                                   |
| `salt`        | BLOB    | 64-byte random salt                             |
| `avatar_url`  | TEXT    | May be empty string                             |
| `email`       | TEXT    | Original casing preserved                       |
| `email_check` | TEXT    | `email.toLower()`,used for dedup, unique        |
| `token`       | TEXT    | 16-char alphanumeric, used for email validation |
| `reset_token` | TEXT    | Nullable,set only during password reset flow    |
| `admin`       | BOOL    | 1 = server administrator                        |
| `stat_agent`  | BOOL    | 1 = allowed to submit stats                     |
| `banned`      | BOOL    | 1 = login rejected                              |

### account_timestamp

Tracks account lifecycle timestamps. Separate table to avoid locking `account` on every login.

```sql
CREATE TABLE IF NOT EXISTS account_timestamp (
    user_id         UNSIGNED BIGINT  NOT NULL PRIMARY KEY,
    creation        UNSIGNED INTEGER NOT NULL,
    last_login      UNSIGNED INTEGER,
    token_last_sent UNSIGNED INTEGER,
    reset_emit      UNSIGNED INTEGER
);
```

| Column            | Type             | Notes                                                                                                            |
| ----------------- | ---------------- | ---------------------------------------------------------------------------------------------------------------- |
| `user_id`         | UNSIGNED BIGINT  | Foreign key to `account.user_id`                                                                                 |
| `creation`        | UNSIGNED INTEGER | Unix seconds,set once at account creation                                                                        |
| `last_login`      | UNSIGNED INTEGER | Unix seconds,updated on each successful login; NULL until first login                                            |
| `token_last_sent` | UNSIGNED INTEGER | Unix seconds,timestamp of the last email validation token dispatch; used to enforce rate limiting on `SendToken` |
| `reset_emit`      | UNSIGNED INTEGER | Unix seconds,timestamp of the last password-reset token email; used to enforce rate limiting on `SendResetToken` |

---

### friendship

One row per pair of users that have any relationship,a pending request, a confirmed friendship, or a block. The row is deleted entirely when no flags remain on either side.

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

| Column          | Type    | Notes                                                            |
| --------------- | ------- | ---------------------------------------------------------------- |
| `user_id_1`     | INTEGER | The lower of the two user IDs,enforced by the `CHECK` constraint |
| `user_id_2`     | INTEGER | The higher of the two user IDs                                   |
| `status_user_1` | INTEGER | Bitmask of `FriendStatus` flags from `user_id_1`'s perspective   |
| `status_user_2` | INTEGER | Bitmask of `FriendStatus` flags from `user_id_2`'s perspective   |

**`FriendStatus` bitmask:**

| Flag      | Value  | Meaning                                         |
| --------- | ------ | ----------------------------------------------- |
| `Friend`  | `0x01` | This user has sent or accepted a friend request |
| `Blocked` | `0x02` | This user has blocked the other                 |

**Relationship states** (interpreting the two status columns from one user's perspective):

| My flags    | Other's flags | State                                        |
| ----------- | ------------- | -------------------------------------------- |
| `Friend=1`  | `Friend=0`    | I sent a request,pending acceptance          |
| `Friend=0`  | `Friend=1`    | They sent me a request,pending my acceptance |
| `Friend=1`  | `Friend=1`    | Mutual friendship                            |
| `Blocked=1` | any           | I have blocked them                          |

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

| Column              | Type    | Notes                                                                                                           |
| ------------------- | ------- | --------------------------------------------------------------------------------------------------------------- |
| `communication_id`  | TEXT    | 12-character PSN Communication ID, e.g. `NPWR12345_00`                                                          |
| `board_id`          | INTEGER | Board number within the communication ID; games typically use 1, 2, 3…                                          |
| `rank_limit`        | INTEGER | Maximum number of scores kept on the board; older/worse scores are dropped                                      |
| `update_mode`       | INTEGER | `0` = NORMAL_UPDATE,only store if the new score is better; `1` = FORCE_UPDATE,always overwrite                  |
| `sort_mode`         | INTEGER | `0` = DESCENDING,higher score ranks better (most games); `1` = ASCENDING,lower score ranks better (time trials) |
| `upload_num_limit`  | INTEGER | Max number of score-data file uploads per user                                                                  |
| `upload_size_limit` | INTEGER | Max bytes per score-data file upload                                                                            |

---

### score

One row per (communication_id, board_id, user_id, character_id) combination,a player's best score for a specific character slot on a specific board.

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
| `user_id`          | INTEGER | References `account.user_id`                                                                                  |
| `character_id`     | INTEGER | Character/slot index; games that don't use multiple characters always use `0`                                 |
| `score`            | INTEGER | The score value; sign and magnitude depend on the game                                                        |
| `comment`          | TEXT    | Optional player-supplied comment submitted with the score; NULL if omitted                                    |
| `game_info`        | BLOB    | Optional small inline game data blob submitted with the score (not the large game data file); NULL if omitted |
| `data_id`          | INTEGER | ID of the associated score-data file in `score_data/`; NULL until `RecordScoreData` is called                 |
| `timestamp`        | INTEGER | PSN timestamp in microseconds since the Common Era epoch (0001-01-01), set at the time of submission          |

**Score lifecycle:**

`RecordScore` inserts or replaces a row depending on `update_mode`. With `NORMAL_UPDATE` the `WHERE excluded.score >= score` (or `<= score` for ASCENDING) clause means the INSERT only takes effect if the new score is better — if not, `numRowsAffected()` returns 0 and the server returns `ScoreNotBest`.

After a successful `RecordScore`, the client may optionally call `RecordScoreData` to attach a binary blob (replay data, ghost car data, etc.). This writes a file to the `score_data/` directory and sets `data_id` to the file's numeric ID. The `data_id` column stays NULL until that second call is made. `GetScoreData` reads the file back given a player's NP ID.

**Score-data files:**

Large per-score blobs are stored on disk as `score_data/<20-digit-id>.sdt` rather than in SQLite. This avoids bloating the database with multi-megabyte BLOBs. The `data_id` column is the link between a score row and its file. On server startup, any `.sdt` files whose IDs are not referenced by any `data_id` column are deleted as orphans.

---