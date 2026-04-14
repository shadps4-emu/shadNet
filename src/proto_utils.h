// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
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
