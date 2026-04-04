#pragma once

#include <QByteArray>
#include "score_messages.pb.h"

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
