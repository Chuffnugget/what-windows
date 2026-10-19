#pragma once
#include <QSslCertificate>
#include <QSslKey>
#include <QString>

struct TlsIdentity {
    QSslCertificate certificate;
    QSslKey key;
    QString fingerprint;
    static TlsIdentity renew(const QString &directory, bool testMode = false);
    static TlsIdentity loadOrCreate(const QString &directory, bool testMode = false);
};

QByteArray protectSecret(const QByteArray &plain);
QByteArray unprotectSecret(const QByteArray &encrypted);
