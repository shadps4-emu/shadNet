#include "client_session.h"
#include <QDebug>

ErrorType ClientSession::CmdCreate(StreamExtractor& data, QByteArray& reply)
{
	Q_UNUSED(reply);
	QString npid = data.getString(false);
	QString password = data.getString(false);
	QString onlineName = data.getString(false);
	QString avatarUrl = data.getString(true);
	QString email = data.getString(false);
	if (data.error()) return ErrorType::Malformed;

	if (!IsValidNpid(npid)) return ErrorType::InvalidInput;

	// Check banned email domain
	int at = email.indexOf('@');
	if (at >= 0) {
		QString domain = email.mid(at + 1).toLower();
		if (m_shared->config->IsBannedDomain(domain))
			return ErrorType::CreationBannedEmailProvider;
	}
	/* //TODO add database support
	auto err = m_db->createAccount(npid, password, onlineName, avatarUrl, email);
	if (err) {
		switch (*err) {
		case DbError::ExistingUsername: return ErrorType::CreationExistingUsername;
		case DbError::ExistingEmail:    return ErrorType::CreationExistingEmail;
		default:                        return ErrorType::CreationError;
		}
	}*/
	qInfo() << "Account created:" << npid;
	return ErrorType::NoError;
}