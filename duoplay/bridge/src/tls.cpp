// SPDX-License-Identifier: AGPL-3.0-only
#include "tls.hpp"
#include "util.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <memory>

namespace duo {

namespace {

using PKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Cert = std::unique_ptr<X509, decltype(&X509_free)>;

PKey newKey() { return PKey(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"), EVP_PKEY_free); }

bool addExt(X509 *cert, X509 *issuer, int nid, const std::string &value) {
	X509V3_CTX ctx;
	X509V3_set_ctx_nodb(&ctx);
	X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
	X509_EXTENSION *ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value.c_str());
	if (!ext)
		return false;
	X509_add_ext(cert, ext, -1);
	X509_EXTENSION_free(ext);
	return true;
}

Cert makeCert(EVP_PKEY *key, const std::string &cn, X509 *issuer, EVP_PKEY *issuerKey, long days) {
	Cert cert(X509_new(), X509_free);
	X509_set_version(cert.get(), 2);
	unsigned char serial[16];
	RAND_bytes(serial, sizeof(serial));
	serial[0] &= 0x7f;
	BIGNUM *bn = BN_bin2bn(serial, sizeof(serial), nullptr);
	BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(cert.get()));
	BN_free(bn);
	X509_gmtime_adj(X509_getm_notBefore(cert.get()), -3600);
	X509_gmtime_adj(X509_getm_notAfter(cert.get()), days * 24 * 3600);
	X509_set_pubkey(cert.get(), key);
	X509_NAME *name = X509_get_subject_name(cert.get());
	X509_NAME_add_entry_by_txt(name, "O", MBSTRING_UTF8, (const unsigned char *)"DuoPlay", -1, -1, 0);
	X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, (const unsigned char *)cn.c_str(), -1, -1, 0);
	X509_set_issuer_name(cert.get(), issuer ? X509_get_subject_name(issuer) : name);
	(void)issuerKey;
	return cert;
}

bool writePem(const std::string &path, X509 *cert, X509 *chain = nullptr) {
	BIO *bio = BIO_new(BIO_s_mem());
	PEM_write_bio_X509(bio, cert);
	if (chain)
		PEM_write_bio_X509(bio, chain);
	char *data;
	long len = BIO_get_mem_data(bio, &data);
	bool ok = writeFileAtomic(path, std::string(data, size_t(len)));
	BIO_free(bio);
	return ok;
}

bool writeKey(const std::string &path, EVP_PKEY *key) {
	BIO *bio = BIO_new(BIO_s_mem());
	PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
	char *data;
	long len = BIO_get_mem_data(bio, &data);
	bool ok = writeFileAtomic(path, std::string(data, size_t(len)));
	BIO_free(bio);
	return ok;
}

X509 *readCert(const std::string &path) {
	std::string pem;
	if (!readFile(path, pem))
		return nullptr;
	BIO *bio = BIO_new_mem_buf(pem.data(), int(pem.size()));
	X509 *c = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
	BIO_free(bio);
	return c;
}

EVP_PKEY *readKey(const std::string &path) {
	std::string pem;
	if (!readFile(path, pem))
		return nullptr;
	BIO *bio = BIO_new_mem_buf(pem.data(), int(pem.size()));
	EVP_PKEY *k = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
	BIO_free(bio);
	return k;
}

} // namespace

bool ensureTls(const std::string &dirIn, const std::vector<std::string> &ipsIn, const std::string &hostname,
               TlsFiles &out, std::string &err) {
	std::string dir = joinPath(dirIn, "tls");
	makeDirs(dir);
	out.caCertPem = joinPath(dir, "duoplay-ca.pem");
	out.caCertDer = joinPath(dir, "duoplay-ca.cer");
	out.certPem = joinPath(dir, "server.pem");
	out.keyPem = joinPath(dir, "server.key");
	std::string caKeyPath = joinPath(dir, "duoplay-ca.key");
	std::string metaPath = joinPath(dir, "server.names");

	// --- CA: created once and kept, so the phone only has to trust it once ---
	PKey caKey(readKey(caKeyPath), EVP_PKEY_free);
	Cert ca(readCert(out.caCertPem), X509_free);
	if (!caKey || !ca) {
		caKey = newKey();
		if (!caKey) {
			err = "Could not generate key";
			return false;
		}
		ca = makeCert(caKey.get(), "DuoPlay Local CA (" + hostname + ")", nullptr, nullptr, 3650);
		addExt(ca.get(), ca.get(), NID_basic_constraints, "critical,CA:TRUE,pathlen:0");
		addExt(ca.get(), ca.get(), NID_key_usage, "critical,keyCertSign,cRLSign");
		addExt(ca.get(), ca.get(), NID_subject_key_identifier, "hash");
		if (!X509_sign(ca.get(), caKey.get(), EVP_sha256())) {
			err = "Could not sign CA";
			return false;
		}
		if (!writeKey(caKeyPath, caKey.get()) || !writePem(out.caCertPem, ca.get())) {
			err = "Could not write CA files to " + dir;
			return false;
		}
		LOGI("tls", "Created local certificate authority in %s", dir.c_str());
	}
	{
		unsigned char *der = nullptr;
		int len = i2d_X509(ca.get(), &der);
		if (len > 0) {
			writeFileAtomic(out.caCertDer, std::string(reinterpret_cast<char *>(der), size_t(len)));
			OPENSSL_free(der);
		}
	}

	// --- server certificate for every name/address the phone might use ---
	std::vector<std::string> ips = ipsIn;
	ips.push_back("127.0.0.1");
	std::sort(ips.begin(), ips.end());
	ips.erase(std::unique(ips.begin(), ips.end()), ips.end());
	std::string san = "DNS:localhost,DNS:" + hostname + ",DNS:" + hostname + ".local";
	for (const auto &ip : ips)
		san += ",IP:" + ip;

	std::string meta;
	Cert existing(readCert(out.certPem), X509_free);
	PKey existingKey(readKey(out.keyPem), EVP_PKEY_free);
	if (existing && existingKey && readFile(metaPath, meta) && meta == san) {
		// still valid for at least another month?
		if (X509_cmp_time(X509_get0_notAfter(existing.get()), nullptr) > 0) {
			ASN1_TIME *soon = X509_gmtime_adj(nullptr, 30L * 24 * 3600);
			bool fresh = ASN1_TIME_compare(X509_get0_notAfter(existing.get()), soon) > 0;
			ASN1_TIME_free(soon);
			if (fresh)
				return true;
		}
	}

	PKey key = newKey();
	// Apple only accepts TLS server certificates valid for at most 825 days.
	Cert cert = makeCert(key.get(), hostname, ca.get(), caKey.get(), 800);
	addExt(cert.get(), ca.get(), NID_basic_constraints, "critical,CA:FALSE");
	addExt(cert.get(), ca.get(), NID_key_usage, "critical,digitalSignature,keyEncipherment");
	addExt(cert.get(), ca.get(), NID_ext_key_usage, "serverAuth");
	addExt(cert.get(), ca.get(), NID_subject_alt_name, san);
	addExt(cert.get(), ca.get(), NID_authority_key_identifier, "keyid:always");
	if (!X509_sign(cert.get(), caKey.get(), EVP_sha256())) {
		err = "Could not sign server certificate";
		return false;
	}
	if (!writeKey(out.keyPem, key.get()) || !writePem(out.certPem, cert.get(), ca.get()) ||
	    !writeFileAtomic(metaPath, san)) {
		err = "Could not write certificate files to " + dir;
		return false;
	}
	LOGI("tls", "Issued HTTPS certificate for %s", san.c_str());
	return true;
}

} // namespace duo
