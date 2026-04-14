// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <stream_extractor.h>
#include "shadnet.pb.h"

// Encode any protobuf message to a QByteArray.
template <typename T>
inline QByteArray PbEncode(const T& msg) {
    std::string s = msg.SerializeAsString();
    return QByteArray(s.data(), static_cast<int>(s.size()));
}

// Decode a QByteArray into a protobuf message.
// Returns false on parse error.
template <typename T>
inline bool PbDecode(T& msg, const QByteArray& bytes) {
    return msg.ParseFromArray(bytes.constData(), bytes.size());
}

// Read a u32-LE-prefixed protobuf blob from the stream and parse it.
// Returns false and sets data.error() on failure.
template <typename T>
static bool decodeProto(T& msg, StreamExtractor& data) {
    QByteArray blob = data.getRawData();
    if (data.error())
        return false;
    return msg.ParseFromArray(blob.constData(), blob.size());
}

// Serialise a protobuf message and append it as a u32-LE-prefixed blob to reply.
template <typename T>
static void appendProto(QByteArray& reply, const T& msg) {
    std::string s = msg.SerializeAsString();
    appendBlob(reply, QByteArray(s.data(), static_cast<int>(s.size())));
}

// Build a notification QByteArray containing a serialised proto message.
template <typename T>
static QByteArray buildNotifPayload(const T& msg) {
    QByteArray buf;
    appendProto(buf, msg);
    return buf;
}