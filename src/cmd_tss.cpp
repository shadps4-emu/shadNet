// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDebug>
#include "client_session.h"
#include "proto_utils.h"
#include "shadnet.pb.h"
#include "tss_files.h"

// Title Small Storage.
ErrorType ClientSession::CmdTssGetData(StreamExtractor& data, QByteArray& reply) {
    const QByteArray comId = data.getBytes(12);
    shadnet::TssGetDataRequest req;
    if (!decodeProto(req, data) || data.error()) {
        return ErrorType::Malformed;
    }

    const int32_t slot = req.slotid();
    if (slot < 0 || slot > TssFiles::MAX_SLOT) {
        qWarning() << "CmdTssGetData: slot out of range" << slot;
        return ErrorType::Invalid;
    }

    const QString cid = QString::fromLatin1(comId.constData(), comId.size());
    const TssFiles::Entry entry = TssFiles::Read(cid, slot);

    shadnet::TssGetDataResponse resp;
    if (!entry.exists) {
        resp.set_statuscodetype(0); // OK
        resp.set_lastmodified(0);
        resp.set_contentlength(0);
        appendProto(reply, resp);
        return ErrorType::NoError;
    }
    const int64_t cap = TssFiles::MaxSizeForSlot(slot);
    if (entry.data.size() > cap) {
        qWarning() << "CmdTssGetData: slot" << slot << "file is" << entry.data.size()
                   << "bytes, exceeds cap" << cap;
        return ErrorType::TooLarge;
    }

    bool applyRange = req.hasoffset() || req.haslastbyte();
    if (req.hasifparam()) {
        const bool fileIsNewer = entry.lastModified > req.iflastmodified();
        if (req.iftype() == 0) { // IF_MODIFIED_SINCE
            if (!fileIsNewer) {
                resp.set_statuscodetype(2); // NOT_MODIFIED
                resp.set_lastmodified(entry.lastModified);
                resp.set_contentlength(0);
                appendProto(reply, resp);
                return ErrorType::NoError;
            }
        } else { // IF_RANGE
            if (fileIsNewer) {
                applyRange = false;
            }
        }
    }

    QByteArray payload = entry.data;
    bool partial = false;
    if (applyRange) {
        const int64_t total = payload.size();
        const int64_t from = req.hasoffset() ? static_cast<int64_t>(req.offset()) : 0;
        // lastByte is inclusive, matching HTTP range semantics.
        const int64_t to = req.haslastbyte() ? static_cast<int64_t>(req.lastbyte()) : total - 1;
        if (from >= total || from < 0 || to < from) {
            qWarning() << "CmdTssGetData: bad range" << from << to << "for size" << total;
            return ErrorType::Invalid;
        }
        const int64_t end = std::min(to, total - 1);
        payload = payload.mid(static_cast<int>(from), static_cast<int>(end - from + 1));
        partial = true;
    }

    resp.set_statuscodetype(partial ? 1 : 0); // PARTIAL : OK
    resp.set_lastmodified(entry.lastModified);
    resp.set_contentlength(static_cast<uint64_t>(payload.size()));
    resp.set_data(payload.constData(), payload.size());
    appendProto(reply, resp);
    return ErrorType::NoError;
}
