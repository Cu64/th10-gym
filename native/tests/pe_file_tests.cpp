#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "pe_file.h"

namespace {

constexpr DWORD kNtOffset = 0x80;
constexpr DWORD kExportRva = 0x2000;
constexpr DWORD kExportOffset = 0x200;
constexpr DWORD kTextRva = 0x1000;
constexpr DWORD kTextOffset = 0x400;
constexpr DWORD kCodeRva = kTextRva + 0x10;
constexpr DWORD kCodeOffset = kTextOffset + 0x10;
constexpr DWORD kImageSize = 0x3000;
constexpr DWORD kImageBase = 0x50000000;
constexpr WORD kFileFlags = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_DLL;
constexpr WORD kDllFlags = IMAGE_DLLCHARACTERISTICS_NX_COMPAT;
constexpr size_t kSectionOffset = kNtOffset + sizeof(IMAGE_NT_HEADERS32);
constexpr DWORD kNameTableOffset = kExportOffset + 0x40;
constexpr DWORD kFunctionTableOffset = kExportOffset + 0x50;
constexpr std::array<BYTE, 16> kCode = {0x33, 0xc0, 0xc2, 0x04, 0x00, 0xcc,
                                        0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
                                        0xcc, 0xcc, 0xcc, 0xcc};

template <typename T>
void put(std::vector<BYTE>& image, size_t offset, const T& value) {
  assert(offset <= image.size() && sizeof(value) <= image.size() - offset);
  std::memcpy(image.data() + offset, &value, sizeof(value));
}

std::vector<BYTE> make_pe(DWORD text_flags = IMAGE_SCN_MEM_EXECUTE) {
  std::vector<BYTE> image(kTextOffset + 0x200);
  IMAGE_DOS_HEADER dos{};
  dos.e_magic = IMAGE_DOS_SIGNATURE;
  dos.e_lfanew = kNtOffset;
  put(image, 0, dos);

  IMAGE_NT_HEADERS32 nt{};
  nt.Signature = IMAGE_NT_SIGNATURE;
  nt.FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
  nt.FileHeader.NumberOfSections = 2;
  nt.FileHeader.SizeOfOptionalHeader = sizeof(nt.OptionalHeader);
  nt.FileHeader.Characteristics = kFileFlags;
  nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
  nt.OptionalHeader.ImageBase = kImageBase;
  nt.OptionalHeader.SizeOfImage = kImageSize;
  nt.OptionalHeader.SizeOfHeaders = kExportOffset;
  nt.OptionalHeader.DllCharacteristics = kDllFlags;
  nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
  nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = {kExportRva,
                                                                   0x100};
  put(image, kNtOffset, nt);

  IMAGE_SECTION_HEADER text{};
  text.Misc.VirtualSize = text.SizeOfRawData = 0x200;
  text.VirtualAddress = kTextRva;
  text.PointerToRawData = kTextOffset;
  text.Characteristics = text_flags | IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_READ;
  put(image, kSectionOffset, text);

  IMAGE_SECTION_HEADER exports{};
  exports.Misc.VirtualSize = exports.SizeOfRawData = 0x200;
  exports.VirtualAddress = kExportRva;
  exports.PointerToRawData = kExportOffset;
  exports.Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
  put(image, kSectionOffset + sizeof(text), exports);

  IMAGE_EXPORT_DIRECTORY directory{};
  directory.Base = 1;
  directory.NumberOfNames = 2;
  directory.NumberOfFunctions = 1;
  directory.AddressOfNames = kExportRva + 0x40;
  directory.AddressOfNameOrdinals = kExportRva + 0x48;
  directory.AddressOfFunctions = kExportRva + 0x50;
  put(image, kExportOffset, directory);
  const DWORD name_rvas[] = {kExportRva + 0x60, kExportRva + 0x68};
  put(image, kNameTableOffset, name_rvas);
  put(image, kFunctionTableOffset, kCodeRva);
  put(image, kExportOffset + 0x60, "Other");
  put(image, kExportOffset + 0x68, "Target");
  std::copy(kCode.begin(), kCode.end(), image.begin() + kCodeOffset);
  return image;
}

void expect_refused(const std::vector<BYTE>& image, const char* error) {
  th10installer::ExportInfo result{};
  std::string why;
  if (th10installer::pe_export(image, "Target", result, why) || why != error) {
    std::fprintf(stderr, "Expected refusal (%s), got '%s' and %zu code bytes\n",
                 error, why.c_str(), result.bytes.size());
    std::exit(1);
  }
  assert(result.rva == 0 && result.bytes.empty());
}

void test_code_range_at_eof() {
  auto image = make_pe();
  image.resize(kCodeOffset + kCode.size());
  th10installer::ExportInfo result{};
  std::string why;
  assert(th10installer::pe_export(image, "Target", result, why));
  assert(result.rva == kCodeRva && result.imageSize == kImageSize &&
         result.preferredBase == kImageBase);
  assert(result.fileCharacteristics == kFileFlags &&
         result.dllCharacteristics == kDllFlags);
  assert(std::equal(result.bytes.begin(), result.bytes.end(), kCode.begin(),
                    kCode.end()));

  image.pop_back();
  expect_refused(image, "export has no file-backed code");
  image.resize(kCodeOffset);
  expect_refused(image, "export has no file-backed code");
}

void test_export_refusals() {
  auto forwarded = make_pe();
  put(forwarded, kFunctionTableOffset, DWORD{kExportRva + 0x80});
  expect_refused(forwarded, "forwarded hook export refused");
  expect_refused(make_pe(0), "export is not in an executable section");

  auto malformed_name = make_pe();
  put(malformed_name, kNameTableOffset, DWORD{kImageSize});
  expect_refused(malformed_name, "unmapped export name");

  auto wrapped_offset = make_pe();
  IMAGE_SECTION_HEADER exports{};
  const size_t exports_section_offset = kSectionOffset + sizeof(exports);
  std::memcpy(&exports, wrapped_offset.data() + exports_section_offset,
              sizeof(exports));
  exports.VirtualAddress = kExportRva - 0x210;
  exports.SizeOfRawData = 0x400;
  exports.PointerToRawData = 0xfffffff0;
  put(wrapped_offset, exports_section_offset, exports);
  expect_refused(wrapped_offset, "unmapped export directory");
}

}  // namespace

int main() {
  test_code_range_at_eof();
  test_export_refusals();
  std::puts("All PE export fixture cases passed.");
}
