#include "file_sha256.h"

#include <bcrypt.h>
#include <cstddef>
#include <memory>
#include <new>

namespace th10 {

DWORD sha256_file_hex(const wchar_t* path, Sha256Hex& hex) {
  HANDLE handle =
      CreateFileW(path, GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return GetLastError();
  const std::unique_ptr<void, decltype(&CloseHandle)> file(handle,
                                                           &CloseHandle);

  BCRYPT_ALG_HANDLE provider = nullptr;
  NTSTATUS status = BCryptOpenAlgorithmProvider(
      &provider, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
  if (status < 0) return static_cast<DWORD>(status);
  const auto close_provider = [](BCRYPT_ALG_HANDLE algorithm_handle) {
    BCryptCloseAlgorithmProvider(algorithm_handle, 0);
  };
  const std::unique_ptr<void, decltype(close_provider)> algorithm(
      provider, close_provider);

  DWORD object_size = 0, result_size = 0;
  status = BCryptGetProperty(provider, BCRYPT_OBJECT_LENGTH,
                             reinterpret_cast<PUCHAR>(&object_size),
                             sizeof(object_size), &result_size, 0);
  if (status < 0) return static_cast<DWORD>(status);
  const std::unique_ptr<BYTE[]> object(new (std::nothrow) BYTE[object_size]);
  if (!object) return ERROR_OUTOFMEMORY;

  BCRYPT_HASH_HANDLE hash_handle = nullptr;
  status = BCryptCreateHash(provider, &hash_handle, object.get(), object_size,
                            nullptr, 0, 0);
  if (status < 0) return static_cast<DWORD>(status);
  const std::unique_ptr<void, decltype(&BCryptDestroyHash)> hash(
      hash_handle, &BCryptDestroyHash);

  BYTE buffer[64 * 1024];
  for (;;) {
    DWORD read = 0;
    if (!ReadFile(file.get(), buffer, sizeof(buffer), &read, nullptr))
      return GetLastError();
    if (read == 0) break;
    status = BCryptHashData(hash.get(), buffer, read, 0);
    if (status < 0) return static_cast<DWORD>(status);
  }

  BYTE digest[32];
  status = BCryptFinishHash(hash.get(), digest, sizeof(digest), 0);
  if (status < 0) return static_cast<DWORD>(status);
  static constexpr char digits[] = "0123456789abcdef";
  for (std::size_t i = 0; i < sizeof(digest); ++i) {
    hex[i * 2] = digits[digest[i] >> 4];
    hex[i * 2 + 1] = digits[digest[i] & 0x0f];
  }
  hex[sizeof(hex) - 1] = '\0';
  return ERROR_SUCCESS;
}

}  // namespace th10
