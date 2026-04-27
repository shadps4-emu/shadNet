// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include "client.h"

// Usage:
//   shadnet-sample <host> <port> register       <npid> <password> <email> [secretKey]
//   shadnet-sample <host> <port> login          <npid> <password> [token]
//   shadnet-sample <host> <port> friend-add     <npid> <password> <friend_npid>
//   shadnet-sample <host> <port> friend-remove  <npid> <password> <friend_npid>
//   shadnet-sample <host> <port> block-add      <npid> <password> <target_npid>
//   shadnet-sample <host> <port> block-remove   <npid> <password> <target_npid>
//   shadnet-sample <host> <port> score-board    <npid> <password> <comId> <boardId>
//   shadnet-sample <host> <port> score-record   <npid> <password> <comId> <boardId> <pcId> <score>
//   [comment] shadnet-sample <host> <port> score-range    <npid> <password> <comId> <boardId>
//   <startRank> <numRanks> shadnet-sample <host> <port> score-get-npid <npid> <password> <comId>
//   <boardId> <target_npid> [pcId]

static void printUsage(const char* prog) {
    printf("Usage:\n"
           "  %s <host> <port> register       <npid> <password> <email> [secretKey]\n"
           "  %s <host> <port> login          <npid> <password> [token]\n"
           "  %s <host> <port> friend-add     <npid> <password> <friend_npid>\n"
           "  %s <host> <port> friend-remove  <npid> <password> <friend_npid>\n"
           "  %s <host> <port> block-add      <npid> <password> <target_npid>\n"
           "  %s <host> <port> block-remove   <npid> <password> <target_npid>\n"
           "  %s <host> <port> score-board    <npid> <password> <comId> <boardId>\n"
           "  %s <host> <port> score-record   <npid> <password> <comId> <boardId> <pcId> <score> "
           "[comment]\n"
           "  %s <host> <port> score-range    <npid> <password> <comId> <boardId> <startRank> "
           "<numRanks>\n"
           "  %s <host> <port> score-get-npid <npid> <password> <comId> <boardId> <target_npid> "
           "[pcId]\n"
           "  %s <host> <port> score-get-accountid      <npid> <password> <comId> <boardId> "
           "<target_accountId> [pcId]\n"
           "  %s <host> <port> score-get-data-accountid <npid> <password> <comId> <boardId> "
           "<target_accountId> [pcId]\n",
           prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

static bool pollUntil(shadnetclient::ShadNetClient& client, bool& done, int timeoutMs = 10000) {
    for (int i = 0; i < timeoutMs / 10 && !done; ++i) {
        client.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        printUsage(argv[0]);
        return 1;
    }

    const char* host = argv[1];
    uint16_t port = static_cast<uint16_t>(atoi(argv[2]));
    const char* command = argv[3];

    shadnetclient::ShadNetClient client;
    printf("[connect] %s:%u ...\n", host, port);
    if (!client.connect(host, port)) {
        printf("[connect] FAILED: %s\n", client.lastError().c_str());
        return 1;
    }
    printf("[connect] OK (protocol v%u)\n", shadnetclient::PROTOCOL_VERSION);

    // register
    if (strcmp(command, "register") == 0) {
        if (argc < 7) {
            printf("register: <npid> <password> <email> [secretKey]\n");
            return 1;
        }
        bool done = false;
        client.onCreateResult = [&done](shadnetclient::ErrorType) { done = true; };
        // argv[4]=npid, argv[5]=password, argv[6]=email, argv[7]=secretKey (optional)
        client.createAccount(argv[4], argv[5], "", argv[6], argc >= 8 ? argv[7] : "");
        if (!pollUntil(client, done))
            printf("[timeout] No reply.\n");
        client.disconnect();
        return done ? 0 : 1;
    }

    // login
    if (strcmp(command, "login") == 0) {
        if (argc < 6) {
            printf("login: <npid> <password> [token]\n");
            return 1;
        }
        bool done = false;
        client.onLoginResult = [&done](const shadnetclient::LoginResult&) { done = true; };
        client.login(argv[4], argv[5], argc >= 7 ? argv[6] : "");
        if (!pollUntil(client, done))
            printf("[timeout] No reply.\n");
        client.disconnect();
        return done ? 0 : 1;
    }

    // ── commands that require login first ─────────────────────────────────────
    bool isKnown = strcmp(command, "friend-add") == 0 || strcmp(command, "friend-remove") == 0 ||
                   strcmp(command, "block-add") == 0 || strcmp(command, "block-remove") == 0 ||
                   strcmp(command, "score-board") == 0 || strcmp(command, "score-record") == 0 ||
                   strcmp(command, "score-range") == 0 || strcmp(command, "score-get-npid") == 0 ||
                   strcmp(command, "score-get-accountid") == 0 ||
                   strcmp(command, "score-get-data-accountid") == 0;

    if (!isKnown) {
        printf("Unknown command: %s\n", command);
        printUsage(argv[0]);
        return 1;
    }

    bool isScoreCmd = (strncmp(command, "score-", 6) == 0);
    if (isScoreCmd && argc < 8) {
        printf("%s: <npid> <password> <comId> <boardId> ...\n", command);
        return 1;
    }
    if (!isScoreCmd && argc < 7) {
        printf("%s: <npid> <password> <target_npid>\n", command);
        return 1;
    }

    const char* npid = argv[4];
    const char* password = argv[5];

    bool loginDone = false;
    bool loginOk = false;
    bool actionDone = false;
    uint64_t loginUserId = 0; // our accountId (server user_id)

    client.onLoginResult = [&](const shadnetclient::LoginResult& res) {
        loginOk = (res.error == shadnetclient::ErrorType::NoError);
        loginUserId = res.userId;
        loginDone = true;
    };
    client.onFriendResult = [&](const shadnetclient::FriendResult&) { actionDone = true; };
    client.onBoardInfos = [&](const shadnetclient::BoardInfo&) { actionDone = true; };
    client.onRecordScore = [&](const shadnetclient::RecordScoreResult&) { actionDone = true; };
    client.onRecordScoreData = [&](shadnetclient::ErrorType) { actionDone = true; };
    client.onGetScoreData = [&](shadnetclient::ErrorType, const std::vector<uint8_t>&) {
        actionDone = true;
    };
    client.onScoreRange = [&](const shadnetclient::ScoreRangeResult&) { actionDone = true; };
    client.onScoreNpid = [&](const shadnetclient::ScoreRangeResult&) { actionDone = true; };
    client.onScoreFriends = [&](const shadnetclient::ScoreRangeResult&) { actionDone = true; };
    client.onScoreAccountId = [&](const shadnetclient::ScoreRangeResult&) { actionDone = true; };
    client.onGetScoreGameDataByAccountId = [&](shadnetclient::ErrorType,
                                               const std::vector<uint8_t>&) { actionDone = true; };

    // Step 1 — login
    client.login(npid, password, "");
    if (!pollUntil(client, loginDone)) {
        printf("[timeout] No login reply.\n");
        client.disconnect();
        return 1;
    }
    if (!loginOk) {
        client.disconnect();
        return 1;
    }

    // Step 2 — issue command
    if (strcmp(command, "friend-add") == 0)
        client.addFriend(argv[6]);
    else if (strcmp(command, "friend-remove") == 0)
        client.removeFriend(argv[6]);
    else if (strcmp(command, "block-add") == 0)
        client.addBlock(argv[6]);
    else if (strcmp(command, "block-remove") == 0)
        client.removeBlock(argv[6]);
    else if (strcmp(command, "score-board") == 0) {
        client.getBoardInfos(argv[6], static_cast<uint32_t>(atoi(argv[7])));
    } else if (strcmp(command, "score-record") == 0) {
        if (argc < 10) {
            printf("score-record: <comId> <boardId> <pcId> <score> [comment]\n");
            client.disconnect();
            return 1;
        }
        client.recordScore(argv[6], static_cast<uint32_t>(atoi(argv[7])),
                           static_cast<int32_t>(atoi(argv[8])),
                           static_cast<int64_t>(atoll(argv[9])), argc >= 11 ? argv[10] : "");
    } else if (strcmp(command, "score-range") == 0) {
        if (argc < 10) {
            printf("score-range: <comId> <boardId> <startRank> <numRanks>\n");
            client.disconnect();
            return 1;
        }
        client.getScoreRange(argv[6], static_cast<uint32_t>(atoi(argv[7])),
                             static_cast<uint32_t>(atoi(argv[8])),
                             static_cast<uint32_t>(atoi(argv[9])));
    } else if (strcmp(command, "score-get-npid") == 0) {
        if (argc < 9) {
            printf("score-get-npid: <comId> <boardId> <target_npid> [pcId]\n");
            client.disconnect();
            return 1;
        }
        int32_t pcId = (argc >= 10) ? static_cast<int32_t>(atoi(argv[9])) : 0;
        client.getScoreNpid(argv[6], static_cast<uint32_t>(atoi(argv[7])), {{argv[8], pcId}});
    } else if (strcmp(command, "score-get-accountid") == 0) {
        if (argc < 9) {
            printf("score-get-accountid: <comId> <boardId> <target_accountId> [pcId]\n");
            printf("  Hint: use 0 for <target_accountId> to look up yourself.\n");
            client.disconnect();
            return 1;
        }
        int64_t accountId = atoll(argv[8]);
        int32_t pcId = (argc >= 10) ? static_cast<int32_t>(atoi(argv[9])) : 0;
        if (accountId == 0) {
            accountId = static_cast<int64_t>(loginUserId);
            printf("[score-get-accountid] using our own userId=%lld\n",
                   static_cast<long long>(accountId));
        }
        client.getScoreAccountId(argv[6], static_cast<uint32_t>(atoi(argv[7])),
                                 {{accountId, pcId}});
    } else if (strcmp(command, "score-get-data-accountid") == 0) {
        if (argc < 9) {
            printf("score-get-data-accountid: <comId> <boardId> <target_accountId> [pcId]\n");
            printf("  Hint: use 0 for <target_accountId> to look up your own blob.\n");
            client.disconnect();
            return 1;
        }
        int64_t accountId = atoll(argv[8]);
        int32_t pcId = (argc >= 10) ? static_cast<int32_t>(atoi(argv[9])) : 0;
        if (accountId == 0) {
            accountId = static_cast<int64_t>(loginUserId);
            printf("[score-get-data-accountid] using our own userId=%lld\n",
                   static_cast<long long>(accountId));
        }
        client.getScoreGameDataByAccountId(argv[6], static_cast<uint32_t>(atoi(argv[7])), accountId,
                                           pcId);
    }

    if (!pollUntil(client, actionDone)) {
        printf("[timeout] No reply.\n");
        client.disconnect();
        return 1;
    }

    // Drain briefly for any incoming notifications
    for (int i = 0; i < 20; ++i) {
        client.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    client.disconnect();
    return 0;
}
