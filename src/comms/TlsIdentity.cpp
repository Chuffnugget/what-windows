#include "comms/TlsIdentity.h"
#include <QCryptographicHash>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <windows.h>
#include <wincrypt.h>
#include <memory>
#include <stdexcept>

namespace {
void fail(const char *message) {
    throw std::runtime_error(message);
}

QByteArray readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        fail("Cannot read TLS identity");
    return f.readAll();
}

void save(const QString &path, const QByteArray &data) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
        fail("Cannot save TLS identity");
}

QByteArray pem(BIO *bio) {
    char *p;
    const auto size = BIO_get_mem_data(bio, &p);
    return QByteArray(p, size);
}
} // namespace

QByteArray protectSecret(const QByteArray &plain) {
    DATA_BLOB input{DWORD(plain.size()), reinterpret_cast<BYTE *>(const_cast<char *>(plain.data()))},
        output{};
    if (!CryptProtectData(&input, L"WHAT", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw std::runtime_error(
            QString("Windows could not protect the secret (error %1)").arg(GetLastError()).toStdString());
    QByteArray result(reinterpret_cast<char *>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return result;
}

QByteArray unprotectSecret(const QByteArray &encrypted) {
    DATA_BLOB input{DWORD(encrypted.size()), reinterpret_cast<BYTE *>(const_cast<char *>(encrypted.data()))},
        output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        fail("Cannot decrypt settings; use the original Windows user or a fresh profile");
    QByteArray result(reinterpret_cast<char *>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}

namespace {
TlsIdentity decode(const QByteArray &certificate, const QByteArray &privateKey) {
    TlsIdentity result;
    result.certificate = QSslCertificate(certificate);
    result.key = QSslKey(privateKey, QSsl::Rsa);
    std::unique_ptr<BIO, decltype(&BIO_free)> cb(BIO_new_mem_buf(certificate.data(), certificate.size()),
                                                 BIO_free),
        kb(BIO_new_mem_buf(privateKey.data(), privateKey.size()), BIO_free);
    std::unique_ptr<X509, decltype(&X509_free)> cert(PEM_read_bio_X509(cb.get(), nullptr, nullptr, nullptr),
                                                     X509_free);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        PEM_read_bio_PrivateKey(kb.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    if (result.certificate.isNull() || result.key.isNull() || !cert || !key ||
        X509_check_private_key(cert.get(), key.get()) != 1)
        fail("TLS certificate/private key mismatch or corruption. Restore the profile backup.");
    result.fingerprint =
        QString::fromLatin1(result.certificate.digest(QCryptographicHash::Sha256).toHex(':'));
    return result;
}
} // namespace

TlsIdentity TlsIdentity::renew(const QString &dir, bool testMode) {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY *raw = nullptr;
    if (!ctx || EVP_PKEY_keygen_init(ctx.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048) <= 0 || EVP_PKEY_keygen(ctx.get(), &raw) <= 0)
        fail("TLS key generation failed");
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
    if (!cert)
        fail("TLS certificate allocation failed");
    X509_set_version(cert.get(), 2);
    QByteArray serialBytes(16, 0);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(serialBytes.data()), serialBytes.size()) != 1)
        fail("TLS serial generation failed");
    serialBytes[0] = char((unsigned char)serialBytes[0] & 0x7f);
    auto *serial =
        BN_bin2bn(reinterpret_cast<const unsigned char *>(serialBytes.data()), serialBytes.size(), nullptr);
    BN_to_ASN1_INTEGER(serial, X509_get_serialNumber(cert.get()));
    BN_free(serial);
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -300);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 10L * 365 * 24 * 3600);
    X509_set_pubkey(cert.get(), key.get());
    auto *name = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("WHAT"), -1,
                               -1, 0);
    X509_set_issuer_name(cert.get(), name);
    if (testMode) {
        auto *san = X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name,
                                        const_cast<char *>("DNS:localhost,IP:127.0.0.1"));
        if (!san || X509_add_ext(cert.get(), san, -1) != 1)
            fail("Test SAN creation failed");
        X509_EXTENSION_free(san);
    }
    if (X509_sign(cert.get(), key.get(), EVP_sha256()) <= 0)
        fail("TLS signing failed");
    std::unique_ptr<BIO, decltype(&BIO_free)> cb(BIO_new(BIO_s_mem()), BIO_free),
        kb(BIO_new(BIO_s_mem()), BIO_free);
    if (!cb || !kb || !PEM_write_bio_X509(cb.get(), cert.get()) ||
        !PEM_write_bio_PrivateKey(kb.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr))
        fail("TLS serialization failed");
    const auto certificate = pem(cb.get()), secret = pem(kb.get());
    auto result = decode(certificate, secret);
    const auto payload = QJsonDocument(QJsonObject{{"certificate", QString::fromLatin1(certificate)},
                                                   {"privateKey", QString::fromLatin1(secret)}})
                             .toJson(QJsonDocument::Compact);
    // Both halves are replaced atomically. An interrupted renewal retains the old identity.
    save(dir + (testMode ? "/test-identity.json" : "/identity.dpapi"),
         testMode ? payload : protectSecret(payload));
    // Public export for support and the isolated live-test harness. Not authoritative.
    QSaveFile exportFile(dir + (testMode ? "/test-certificate.pem" : "/certificate.pem"));
    if (exportFile.open(QIODevice::WriteOnly)) {
        exportFile.write(certificate);
        exportFile.commit();
    }
    return result;
}

TlsIdentity TlsIdentity::loadOrCreate(const QString &dir, bool testMode) {
    const auto bundle = dir + (testMode ? "/test-identity.json" : "/identity.dpapi");
    if (QFile::exists(bundle)) {
        const auto bytes = readFile(bundle);
        const auto payload = QJsonDocument::fromJson(testMode ? bytes : unprotectSecret(bytes)).object();
        return decode(payload["certificate"].toString().toLatin1(),
                      payload["privateKey"].toString().toLatin1());
    }
    const auto certPath = dir + (testMode ? "/test-certificate.pem" : "/certificate.pem");
    const auto keyPath = dir + (testMode ? "/test-private-key.pem" : "/private-key.dpapi");
    if (!QFile::exists(certPath) && !QFile::exists(keyPath))
        return renew(dir, testMode);
    if (!QFile::exists(keyPath) || !QFile::exists(certPath))
        fail("TLS identity is incomplete. Restore the profile backup, or use Renew Certificate "
             "in Settings > Pairing and Security and update the fingerprint in Home Assistant.");
    return decode(readFile(certPath), testMode ? readFile(keyPath) : unprotectSecret(readFile(keyPath)));
}
