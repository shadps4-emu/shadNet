// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstring>
#include <QByteArray>
#include <QString>
#include "protocol.h"

class StreamExtractor {
public:
    explicit StreamExtractor(const QByteArray& data) : m_data(data), m_pos(0), m_error(false) {}

    bool error() const {
        return m_error;
    }
    int pos() const {
        return m_pos;
    }

    template <typename T>
    T get() {
        static_assert(std::is_integral<T>::value, "get<T> requires integral type");
        if (m_pos + (int)sizeof(T) > m_data.size()) {
            m_error = true;
            return T{};
        }
        T value{};
        std::memcpy(&value, m_data.constData() + m_pos, sizeof(T));
        // convert from little-endian
        value = fromLe(value);
        m_pos += sizeof(T);
        return value;
    }

    // empty=true allows an empty string (just the null terminator)
    QString getString(bool empty = false) {
        QString s;
        while (m_pos < m_data.size() && m_data[m_pos] != '\0') {
            s += QChar(static_cast<unsigned char>(m_data[m_pos]));
            ++m_pos;
        }
        if (m_pos >= m_data.size()) {
            // Ran off the end without finding a null terminator
            m_error = true;
            return {};
        }
        ++m_pos; // consume null terminator
        if (!empty && s.isEmpty())
            m_error = true;
        return s;
    }

    // Read exactly n raw bytes,needed for fixed-size fields like ComId (12 bytes).
    QByteArray getBytes(int n) {
        if (m_pos + n > m_data.size()) {
            m_error = true;
            return {};
        }
        QByteArray out(m_data.constData() + m_pos, n);
        m_pos += n;
        return out;
    }

    QByteArray getRawData() {
        uint32_t size = get<uint32_t>();
        if (m_error)
            return {};
        if (m_pos + (int)size > m_data.size()) {
            m_error = true;
            return {};
        }
        QByteArray out(m_data.constData() + m_pos, size);
        m_pos += size;
        return out;
    }

private:
    template <typename T>
    static T fromLe(T v) {
        // On little-endian hosts this is a no-op
        // Proper impl for portability:
        T result = 0;
        for (size_t i = 0; i < sizeof(T); ++i)
            result |= (T)(((uint8_t*)&v)[i]) << (8 * i);
        return result;
    }

    QByteArray m_data;
    int m_pos;
    bool m_error;
};

// Packet builder helpers
inline void appendU16LE(QByteArray& buf, uint16_t v) {
    buf.append(static_cast<char>(v & 0xFF));
    buf.append(static_cast<char>((v >> 8) & 0xFF));
}
inline void appendU32LE(QByteArray& buf, uint32_t v) {
    buf.append(static_cast<char>(v & 0xFF));
    buf.append(static_cast<char>((v >> 8) & 0xFF));
    buf.append(static_cast<char>((v >> 16) & 0xFF));
    buf.append(static_cast<char>((v >> 24) & 0xFF));
}
inline void appendU64LE(QByteArray& buf, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        buf.append(static_cast<char>((v >> (8 * i)) & 0xFF));
}
inline void appendCStr(QByteArray& buf, const QString& s) {
    buf.append(s.toUtf8());
    buf.append('\0');
}
inline void appendBlob(QByteArray& buf, const QByteArray& blob) {
    appendU32LE(buf, static_cast<uint32_t>(blob.size()));
    buf.append(blob);
}
// Utility: write u32 size at offset 3 after reply is built
inline void fixPacketSize(QByteArray& buf) {
    uint32_t sz = static_cast<uint32_t>(buf.size());
    buf[3] = static_cast<char>(sz & 0xFF);
    buf[4] = static_cast<char>((sz >> 8) & 0xFF);
    buf[5] = static_cast<char>((sz >> 16) & 0xFF);
    buf[6] = static_cast<char>((sz >> 24) & 0xFF);
}
