#include "pe_file.h"

#include <algorithm>
#include <cstring>

#include "remote_process.h"

namespace th10installer {

bool load_bytes(const std::wstring& path, std::vector<BYTE>& b, DWORD& err) {
  auto f = own_handle(
      CreateFileW(path.c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!f) {
    err = GetLastError();
    return false;
  }
  LARGE_INTEGER z{};
  if (!GetFileSizeEx(f.get(), &z)) {
    err = GetLastError();
    return false;
  }
  if (z.QuadPart < sizeof(IMAGE_DOS_HEADER) ||
      z.QuadPart > 128ll * 1024 * 1024) {
    err = ERROR_BAD_EXE_FORMAT;
    return false;
  }
  b.resize((size_t)z.QuadPart);
  DWORD got = 0;
  if (!ReadFile(f.get(), b.data(), static_cast<DWORD>(b.size()), &got,
                nullptr)) {
    err = GetLastError();
    return false;
  }
  if (got != b.size()) {
    err = ERROR_HANDLE_EOF;
    return false;
  }
  return true;
}

namespace {

struct PeHeaders {
  IMAGE_NT_HEADERS32 nt{};
  size_t section_offset = 0;
};

bool contains_bytes(size_t total, size_t offset, size_t count) {
  return offset <= total && count <= total - offset;
}

bool parse_headers(const std::vector<BYTE>& image, PeHeaders& headers,
                   std::string& why) {
  IMAGE_DOS_HEADER dos{};
  if (image.size() < sizeof(dos)) {
    why = "short PE";
    return false;
  }
  std::memcpy(&dos, image.data(), sizeof(dos));
  auto& nt = headers.nt;
  if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
      !contains_bytes(image.size(), dos.e_lfanew, sizeof(nt))) {
    why = "invalid DOS/NT header";
    return false;
  }
  std::memcpy(&nt, image.data() + dos.e_lfanew, sizeof(nt));
  if (nt.Signature != IMAGE_NT_SIGNATURE ||
      nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
      nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
    why = "DLL is not PE32 i386";
    return false;
  }
  headers.section_offset = dos.e_lfanew + sizeof(DWORD) +
                           sizeof(IMAGE_FILE_HEADER) +
                           nt.FileHeader.SizeOfOptionalHeader;
  return true;
}

bool read_section(const std::vector<BYTE>& image, const PeHeaders& headers,
                  unsigned index, IMAGE_SECTION_HEADER& section) {
  const size_t offset = headers.section_offset + index * sizeof(section);
  if (!contains_bytes(image.size(), offset, sizeof(section))) return false;
  std::memcpy(&section, image.data() + offset, sizeof(section));
  return true;
}

// Translate a range of an RVA to its file-backed bytes.
bool file_offset(const std::vector<BYTE>& image, const PeHeaders& headers,
                 DWORD rva, size_t count, size_t& offset) {
  if (rva < headers.nt.OptionalHeader.SizeOfHeaders &&
      contains_bytes(image.size(), rva, count)) {
    offset = rva;
    return true;
  }
  for (unsigned i = 0; i < headers.nt.FileHeader.NumberOfSections; ++i) {
    IMAGE_SECTION_HEADER section{};
    if (!read_section(image, headers, i, section)) return false;
    if (rva < section.VirtualAddress) continue;
    const DWORD delta = rva - section.VirtualAddress;
    if (!contains_bytes(section.SizeOfRawData, delta, count)) continue;
    if (!contains_bytes(image.size(), section.PointerToRawData, delta))
      return false;
    offset = size_t{section.PointerToRawData} + delta;
    return contains_bytes(image.size(), offset, count);
  }
  return false;
}

bool find_export_rva(const std::vector<BYTE>& image, const PeHeaders& headers,
                     const char* name, DWORD& rva, std::string& why) {
  const IMAGE_DATA_DIRECTORY dir =
      headers.nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) {
    why = "missing export directory";
    return false;
  }
  size_t directory_offset = 0;
  IMAGE_EXPORT_DIRECTORY exports{};
  if (!file_offset(image, headers, dir.VirtualAddress, sizeof(exports),
                   directory_offset)) {
    why = "unmapped export directory";
    return false;
  }
  std::memcpy(&exports, image.data() + directory_offset, sizeof(exports));
  if (exports.NumberOfNames > 65536 || exports.NumberOfFunctions > 65536) {
    why = "unreasonable export table";
    return false;
  }
  size_t names_offset = 0, ordinals_offset = 0, functions_offset = 0;
  if (!file_offset(image, headers, exports.AddressOfNames,
                   exports.NumberOfNames * sizeof(DWORD), names_offset) ||
      !file_offset(image, headers, exports.AddressOfNameOrdinals,
                   exports.NumberOfNames * sizeof(WORD), ordinals_offset) ||
      !file_offset(image, headers, exports.AddressOfFunctions,
                   exports.NumberOfFunctions * sizeof(DWORD),
                   functions_offset)) {
    why = "unmapped export arrays";
    return false;
  }
  for (DWORD i = 0; i < exports.NumberOfNames; ++i) {
    DWORD name_rva = 0;
    std::memcpy(&name_rva, image.data() + names_offset + i * sizeof(name_rva),
                sizeof(name_rva));
    size_t name_offset = 0;
    if (!file_offset(image, headers, name_rva, 1, name_offset)) {
      why = "unmapped export name";
      return false;
    }
    const auto* export_name =
        reinterpret_cast<const char*>(image.data() + name_offset);
    if (!std::memchr(export_name, 0, image.size() - name_offset)) {
      why = "unterminated export name";
      return false;
    }
    if (std::strcmp(export_name, name) != 0) continue;

    WORD ordinal = 0;
    std::memcpy(&ordinal, image.data() + ordinals_offset + i * sizeof(ordinal),
                sizeof(ordinal));
    if (ordinal >= exports.NumberOfFunctions) {
      why = "bad export ordinal";
      return false;
    }
    DWORD candidate = 0;
    std::memcpy(&candidate,
                image.data() + functions_offset + ordinal * sizeof(candidate),
                sizeof(candidate));
    if (candidate >= dir.VirtualAddress &&
        candidate - dir.VirtualAddress < dir.Size) {
      why = "forwarded hook export refused";
      return false;
    }
    if (candidate == 0 || candidate >= headers.nt.OptionalHeader.SizeOfImage) {
      why = "invalid export RVA";
      return false;
    }
    rva = candidate;
    return true;
  }
  why = std::string("missing export ") + name;
  return false;
}

bool read_export_code(const std::vector<BYTE>& image, const PeHeaders& headers,
                      DWORD rva, std::vector<BYTE>& code, std::string& why) {
  for (unsigned i = 0; i < headers.nt.FileHeader.NumberOfSections; ++i) {
    IMAGE_SECTION_HEADER section{};
    if (!read_section(image, headers, i, section)) {
      why = "truncated section table";
      return false;
    }
    const DWORD virtual_size =
        std::max(section.Misc.VirtualSize, section.SizeOfRawData);
    if (rva < section.VirtualAddress ||
        rva - section.VirtualAddress >= virtual_size)
      continue;
    if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
      why = "export is not in an executable section";
      return false;
    }
    const DWORD delta = rva - section.VirtualAddress;
    if (delta >= section.SizeOfRawData ||
        !contains_bytes(image.size(), section.PointerToRawData, delta)) {
      why = "export has no file-backed code";
      return false;
    }
    const size_t count = std::min<DWORD>(16, section.SizeOfRawData - delta);
    if (count < 5) {
      why = "too few file-backed export bytes to validate";
      return false;
    }
    const size_t offset = size_t{section.PointerToRawData} + delta;
    if (!contains_bytes(image.size(), offset, count)) {
      why = "export has no file-backed code";
      return false;
    }
    code.assign(image.begin() + offset, image.begin() + offset + count);
    return true;
  }
  why = "export does not map to an executable section";
  return false;
}

}  // namespace

bool pe_export(const std::vector<BYTE>& image, const char* name,
               ExportInfo& result, std::string& why) {
  PeHeaders headers;
  DWORD rva = 0;
  if (!parse_headers(image, headers, why) ||
      !find_export_rva(image, headers, name, rva, why) ||
      !read_export_code(image, headers, rva, result.bytes, why))
    return false;

  result.rva = rva;
  result.imageSize = headers.nt.OptionalHeader.SizeOfImage;
  result.preferredBase = headers.nt.OptionalHeader.ImageBase;
  result.fileCharacteristics = headers.nt.FileHeader.Characteristics;
  result.dllCharacteristics = headers.nt.OptionalHeader.DllCharacteristics;
  return true;
}

}  // namespace th10installer
