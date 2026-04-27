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

// Read a u32-LE-prefixed blob from StreamExtractor and parse it into a proto message.
template <typename T>
inline bool decodeProto(T& msg, StreamExtractor& data) {
    QByteArray blob = data.getRawData();
    if (data.error())
        return false;
    return msg.ParseFromArray(blob.constData(), blob.size());
}

// Serialize a proto message and append it as a u32-LE-prefixed blob to buf.
template <typename T>
inline void appendProto(QByteArray& buf, const T& msg) {
    std::string s = msg.SerializeAsString();
    appendBlob(buf, QByteArray(s.data(), static_cast<int>(s.size())));
}
