// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include "client.h"

// Usage:
//   shadnet-sample <host> <port> register       <npid> <password> <onlineName> <email>
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
           "  %s <host> <port> register       <npid> <password> <onlineName> <email>\n"
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
           "[pcId]\n",
           prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

static bool pollUntil(shadnet::ShadNetClient& client, bool& done, int timeoutMs = 10000) {
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

    shadnet::ShadNetClient client;
    printf("[connect] %s:%u ...\n", host, port);
    if (!client.connect(host, port)) {
        printf("[connect] FAILED: %s\n", client.lastError().c_str());
        return 1;
    }
    printf("[connect] OK (protocol v%u)\n", shadnet::PROTOCOL_VERSION);

    // register
    if (strcmp(command, "register") == 0) {
        if (argc < 8) {
            printf("register: <npid> <password> <onlineName> <email>\n");
            return 1;
        }
        bool done = false;
        client.onCreateResult = [&done](shadnet::ErrorType) { done = true; };
        client.createAccount(argv[4], argv[5], argv[6], "", argv[7]);
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
        client.onLoginResult = [&done](const shadnet::LoginResult&) { done = true; };
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
                   strcmp(command, "room-create") == 0 || strcmp(command, "room-join") == 0 ||
                   strcmp(command, "room-leave") == 0 || strcmp(command, "room-list") == 0;

    if (!isKnown) {
        printf("Unknown command: %s\n", command);
        printUsage(argv[0]);
        return 1;
    }

    bool isScoreCmd = (strncmp(command, "score-", 6) == 0);
    bool isRoomCmd = (strncmp(command, "room-", 5) == 0);

    if (isScoreCmd && argc < 8) {
        printf("%s: <npid> <password> <comId> <boardId> ...\n", command);
        return 1;
    }
    // room-join and room-leave need a roomId; room-create needs maxSlots; room-list needs nothing
    if (isRoomCmd && strcmp(command, "room-list") != 0 && argc < 7) {
        printf("%s: <npid> <password> <arg>\n", command);
        return 1;
    }
    if (!isScoreCmd && !isRoomCmd && argc < 7) {
        printf("%s: <npid> <password> <target_npid>\n", command);
        return 1;
    }

    const char* npid = argv[4];
    const char* password = argv[5];

    bool loginDone = false;
    bool loginOk = false;
    bool actionDone = false;

    client.onLoginResult = [&](const shadnet::LoginResult& res) {
        loginOk = (res.error == shadnet::ErrorType::NoError);
        loginDone = true;
    };
    client.onFriendResult = [&](const shadnet::FriendResult&) { actionDone = true; };
    client.onBoardInfos = [&](const shadnet::BoardInfo&) { actionDone = true; };
    client.onRecordScore = [&](const shadnet::RecordScoreResult&) { actionDone = true; };
    client.onRecordScoreData = [&](shadnet::ErrorType) { actionDone = true; };
    client.onGetScoreData = [&](shadnet::ErrorType, const std::vector<uint8_t>&) {
        actionDone = true;
    };
    client.onScoreRange = [&](const shadnet::ScoreRangeResult&) { actionDone = true; };
    client.onScoreNpid = [&](const shadnet::ScoreRangeResult&) { actionDone = true; };
    client.onScoreFriends = [&](const shadnet::ScoreRangeResult&) { actionDone = true; };
    client.onRegisterHandlers = [&](shadnet::ErrorType) { actionDone = true; };
    client.onCreateRoom = [&](const shadnet::CreateRoomResult&) { actionDone = true; };
    client.onJoinRoom = [&](const shadnet::JoinRoomResult&) { actionDone = true; };
    client.onLeaveRoom = [&](shadnet::ErrorType, uint64_t) { actionDone = true; };
    client.onRoomList = [&](const shadnet::RoomListResult&) { actionDone = true; };

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

    // Step 2 — register matching handlers (needed for room commands)
    if (isRoomCmd) {
        bool regDone = false;
        client.onRegisterHandlers = [&](shadnet::ErrorType) { regDone = true; };
        client.registerHandlers("", 0, 1, 0, 0x7F);
        if (!pollUntil(client, regDone, 5000))
            printf("[warn] RegisterHandlers timed out — continuing anyway\n");
        // reset so the outer actionDone works
        client.onRegisterHandlers = [&](shadnet::ErrorType) { actionDone = true; };
    }

    // Step 3 — issue command
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
    } else if (strcmp(command, "room-create") == 0) {
        uint16_t maxSlots = argc >= 7 ? static_cast<uint16_t>(atoi(argv[6])) : 4;
        client.createRoom(1, maxSlots);
    } else if (strcmp(command, "room-join") == 0) {
        uint64_t roomId = static_cast<uint64_t>(atoll(argv[6]));
        client.joinRoom(roomId, 1);
    } else if (strcmp(command, "room-leave") == 0) {
        uint64_t roomId = static_cast<uint64_t>(atoll(argv[6]));
        client.leaveRoom(roomId, 1);
    } else if (strcmp(command, "room-list") == 0) {
        client.getRoomList();
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

    if (strcmp(command, "room-create") == 0 || strcmp(command, "room-join") == 0) {
        std::atomic<bool> sessionActive{true};
        std::atomic<bool> commandReady{false};
        std::string pendingCmd;
        std::mutex cmdMutex;

        // Stdin reader thread: blocks on fgets, hands each line to the main thread.
        std::thread stdinThread([&]() {
            char buf[256];
            printf("[session] Commands: room-list  |  room-leave <roomId>  |  quit\n");
            while (sessionActive) {
                printf("> ");
                fflush(stdout);
                if (!fgets(buf, sizeof(buf), stdin))
                    break;

                // Strip \r\n
                size_t len = strlen(buf);
                while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
                    buf[--len] = '\0';

                {
                    std::lock_guard<std::mutex> lk(cmdMutex);
                    pendingCmd = buf;
                    commandReady = true;
                }
                // Wait for main thread to consume before prompting again
                while (commandReady && sessionActive)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

        // Main thread: poll network + dispatch commands handed over by stdin thread.
        while (sessionActive) {
            // Drain socket — this dispatches any received notifications immediately.
            client.update();

            if (commandReady) {
                std::string cmd;
                {
                    std::lock_guard<std::mutex> lk(cmdMutex);
                    cmd = pendingCmd;
                    commandReady = false; // let stdin thread continue
                }

                if (cmd == "quit" || cmd == "exit") {
                    printf("[session] Disconnecting.\n");
                    sessionActive = false;

                } else if (cmd == "room-list") {
                    actionDone = false;
                    client.onRoomList = [&](const shadnet::RoomListResult&) { actionDone = true; };
                    client.getRoomList();
                    pollUntil(client, actionDone, 5000);

                } else if (cmd.rfind("room-leave", 0) == 0) {
                    uint64_t leaveId = 0;
                    if (cmd.size() > 11)
                        leaveId = static_cast<uint64_t>(std::stoull(cmd.substr(11)));
                    if (leaveId == 0) {
                        printf("[session] Usage: room-leave <roomId>\n");
                    } else {
                        actionDone = false;
                        client.onLeaveRoom = [&](shadnet::ErrorType, uint64_t) {
                            actionDone = true;
                        };
                        client.leaveRoom(leaveId, 1);
                        pollUntil(client, actionDone, 5000);
                        // Drain so any final notifications (MemberLeft to others) are sent
                        for (int i = 0; i < 20; ++i) {
                            client.update();
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                        sessionActive = false;
                    }

                } else if (!cmd.empty()) {
                    printf("[session] Unknown: %s\n", cmd.c_str());
                    printf("[session] Commands: room-list  |  room-leave <roomId>  |  quit\n");
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        stdinThread.join();
    }

    client.disconnect();
    return 0;
}
