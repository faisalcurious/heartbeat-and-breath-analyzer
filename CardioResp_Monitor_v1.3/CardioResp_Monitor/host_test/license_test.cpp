// license_test.cpp - runs the firmware's token check (LicenseCore.h) with OpenSSL as the AES-GCM primitive.
// usage: license_test <key_hex> <token> <device_mac>   -> prints fields and PASS/FAIL reasons
// build: g++ -O2 -std=c++17 -ICardioRespMonitor host_test/license_test.cpp -lcrypto -o license_test
#include <openssl/evp.h>
#include "LicenseCore.h"
static bool gcmOpenSsl(const uint8_t key[16], const uint8_t iv[12], const uint8_t* ct, size_t n, const uint8_t tag[16], uint8_t* out) {
  EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new(); int l = 0; bool ok = false;
  if (EVP_DecryptInit_ex(c, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) &&
      EVP_DecryptInit_ex(c, nullptr, nullptr, key, iv) && EVP_DecryptUpdate(c, out, &l, ct, (int)n) &&
      EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, (void*)tag)) { int l2 = 0; ok = EVP_DecryptFinal_ex(c, out + l, &l2) > 0; }
  EVP_CIPHER_CTX_free(c); return ok;
}
int main(int argc, char** argv) {
  if (argc < 4) return 2;
  uint8_t key[16]; for (int i = 0; i < 16; i++) key[i] = (uint8_t)(hexv(argv[1][2 * i]) * 16 + hexv(argv[1][2 * i + 1]));
  LicFields f; char err[96] = "";
  if (!licenseDecode(key, argv[2], gcmOpenSsl, f, err, sizeof err)) { printf("REJECT: %s\n", err); return 1; }
  printf("OK user=\"%s\" uid=\"%s\" mac=%s iat=%u exp=%u macMatch=%d\n", f.user, f.uid, f.mac, f.iat, f.exp, strcmp(f.mac, argv[3]) == 0);
  return 0;
}
