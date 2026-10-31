// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <QDebug>
#include <QList>

namespace WebApiRoutes {

struct MultipartPart {
    QByteArray contentType;
    QByteArray contentDescription;
    QByteArray data;
};

inline QByteArray ExtractBoundary(const QByteArray& contentType) {
    const int b = contentType.indexOf("boundary=");
    if (b < 0)
        return {};
    QByteArray v = contentType.mid(b + 9).trimmed();
    if (v.startsWith('"')) {
        const int end = v.indexOf('"', 1);
        return end > 0 ? v.mid(1, end - 1) : v.mid(1);
    }
    int end = v.size();
    for (int i = 0; i < v.size(); ++i) {
        const char c = v[i];
        if (c == ';' || c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            end = i;
            break;
        }
    }
    return v.left(end);
}

inline QByteArray NormalizeContentType(QByteArray contentType) {
    contentType = contentType.trimmed();
    if (contentType.isEmpty())
        return contentType;
    QList<QByteArray> segments;
    int start = 0;
    bool inQuotes = false;
    for (int i = 0; i < contentType.size(); ++i) {
        const char c = contentType.at(i);
        if (c == '"')
            inQuotes = !inQuotes;
        else if (c == ',' && !inQuotes) {
            segments << contentType.mid(start, i - start).trimmed();
            start = i + 1;
        }
    }
    segments << contentType.mid(start).trimmed();
    if (segments.size() < 2)
        return contentType;
    for (const QByteArray& seg : segments) {
        if (seg != segments.first())
            return contentType; // genuinely different values; leave untouched
    }
    return segments.first();
}

inline QList<MultipartPart> ParseMultipartMixed(const QByteArray& body,
                                                const QByteArray& boundary) {
    QList<MultipartPart> parts;
    const QByteArray delim = "--" + boundary;
    int pos = body.indexOf(delim);
    if (pos < 0)
        return parts;
    pos += delim.size();
    while (pos < body.size()) {
        if (body.mid(pos, 2) == "--")
            break; // closing delimiter
        if (body.mid(pos, 2) == "\r\n")
            pos += 2;
        else if (pos < body.size() && body[pos] == '\n')
            pos += 1;

        int sep = 4;
        int hdrEnd = body.indexOf("\r\n\r\n", pos);
        if (hdrEnd < 0) {
            hdrEnd = body.indexOf("\n\n", pos);
            sep = 2;
        }
        if (hdrEnd < 0)
            break;
        const QByteArray headers = body.mid(pos, hdrEnd - pos);
        const int bodyStart = hdrEnd + sep;
        int next = body.indexOf(delim, bodyStart);
        bool unterminated = false;
        if (next < 0) {
            qWarning() << "WebAPI: multipart part without closing delimiter, keeping"
                       << (body.size() - bodyStart) << "trailing bytes";
            next = body.size();
            unterminated = true;
        }
        int bodyEnd = next;
        if (bodyEnd >= 2 && body.mid(bodyEnd - 2, 2) == "\r\n")
            bodyEnd -= 2;
        else if (bodyEnd >= 1 && body[bodyEnd - 1] == '\n')
            bodyEnd -= 1;

        MultipartPart part;
        part.data = body.mid(bodyStart, bodyEnd - bodyStart);
        for (const QByteArray& raw : headers.split('\n')) {
            const QByteArray line = raw.trimmed();
            const int c = line.indexOf(':');
            if (c < 0)
                continue;
            const QByteArray name = line.left(c).trimmed().toLower();
            const QByteArray val = line.mid(c + 1).trimmed();
            if (name == "content-type")
                part.contentType = val;
            else if (name == "content-description")
                part.contentDescription = val;
        }
        parts.append(part);
        if (unterminated)
            break;
        pos = next + delim.size();
    }
    return parts;
}

} // namespace WebApiRoutes
