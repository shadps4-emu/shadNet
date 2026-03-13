#include <QDebug>
#include "client_session.h"

ErrorType ClientSession::CmdCreate(StreamExtractor& data, QByteArray& reply) {
    Q_UNUSED(reply);
    QString npid = data.getString(false);
    QString password = data.getString(false);
    QString onlineName = data.getString(false);
    QString avatarUrl = data.getString(true);
    QString email = data.getString(false);
    if (data.error())
        return ErrorType::Malformed;

    if (!IsValidNpid(npid))
        return ErrorType::InvalidInput;

    // Check banned email domain
    int at = email.indexOf('@');
    if (at >= 0) {
        QString domain = email.mid(at + 1).toLower();
        if (m_shared->config->IsBannedDomain(domain))
            return ErrorType::CreationBannedEmailProvider;
    }

    auto err = m_db->CreateAccount(npid, password, onlineName, avatarUrl, email);
    if (err) {
        switch (*err) {
        case DbError::ExistingUsername:
            return ErrorType::CreationExistingUsername;
        case DbError::ExistingEmail:
            return ErrorType::CreationExistingEmail;
        default:
            return ErrorType::CreationError;
        }
    }
    qInfo() << "Account created:" << npid;
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdLogin(StreamExtractor& data, QByteArray& reply) {
    // TODO missing friends relations

    QString npid = data.getString(false);
    QString password = data.getString(false);
    QString token = data.getString(true);
    if (data.error())
        return ErrorType::Malformed;

    bool checkToken = m_shared->config->IsEmailValidated();
    auto userOpt = m_db->CheckUser(npid, password, token, checkToken);
    if (!userOpt) {
        qWarning() << "Login failed for" << npid;
        return ErrorType::LoginInvalidPassword;
    }
    const UserRecord& user = *userOpt;

    if (user.banned) {
        qWarning() << "Banned user attempted login:" << npid;
        return ErrorType::LoginError;
    }

    // Check not already logged in
    {
        QReadLocker lk(&m_shared->clientsLock);
        if (m_shared->clients.contains(user.userId))
            return ErrorType::LoginAlreadyLoggedIn;
    }

    // Update timestamps
    m_db->UpdateLoginTime(user.userId);

    // Populate client info
    m_info.userId = user.userId;
    m_info.npid = npid;
    m_info.onlineName = user.onlineName;
    m_info.avatarUrl = user.avatarUrl;
    m_info.token = user.token;
    m_info.admin = user.admin;
    m_info.statAgent = user.statAgent;
    m_info.banned = user.banned;
    m_authenticated = true;

    // Build reply: onlineName, avatarUrl, userId, friends, friend_requests, requests_received,
    // blocked
    appendCStr(reply, user.onlineName);
    appendCStr(reply, user.avatarUrl);
    appendU64LE(reply, static_cast<uint64_t>(user.userId));

    // dummy TODO
    appendU32LE(reply, 0); // friends
    appendU32LE(reply, 0); // requests sent
    appendU32LE(reply, 0); // requests received
    appendU32LE(reply, 0); // blocked

    // Register in global clients map
    {
        QWriteLocker lk(&m_shared->clientsLock);
        SharedState::ClientEntry entry;
        entry.npid = npid;
        entry.send = [this](QByteArray pkt) {
            QMetaObject::invokeMethod(
                this, [this, pkt]() { SendPacket(pkt); }, Qt::QueuedConnection);
        };
        m_shared->clients[user.userId] = std::move(entry);
    }

    qInfo() << "Authenticated:" << npid;
    return ErrorType::NoError;
}