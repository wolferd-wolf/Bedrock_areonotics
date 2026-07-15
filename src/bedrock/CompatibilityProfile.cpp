#include "bedrock/CompatibilityProfile.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <elf.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

namespace aeronautics::bedrock {
namespace {

struct ParsedMapsLine {
    MemoryRegion region;
    std::uintptr_t fileOffset{};
    std::filesystem::path path;
};

[[nodiscard]] std::string_view trimLeft(std::string_view value) {
    const auto first = value.find_first_not_of(" \t");
    return first == std::string_view::npos ? std::string_view{} : value.substr(first);
}

[[nodiscard]] bool parseHex(std::string_view text, std::uintptr_t& value) {
    value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[nodiscard]] bool parseMapsLine(std::string_view line, ParsedMapsLine& output) {
    std::istringstream stream{std::string(line)};
    std::string addressRange;
    std::string permissions;
    std::string offsetText;
    std::string device;
    std::string inode;
    if (!(stream >> addressRange >> permissions >> offsetText >> device >> inode)) {
        return false;
    }

    const auto separator = addressRange.find('-');
    if (separator == std::string::npos) {
        return false;
    }

    std::uintptr_t start = 0;
    std::uintptr_t end = 0;
    std::uintptr_t fileOffset = 0;
    if (!parseHex(std::string_view(addressRange).substr(0, separator), start) ||
        !parseHex(std::string_view(addressRange).substr(separator + 1), end) ||
        !parseHex(offsetText, fileOffset) || end <= start) {
        return false;
    }

    std::string remainder;
    std::getline(stream, remainder);
    const std::string_view trimmedPath = trimLeft(remainder);

    output.region = MemoryRegion{
        .start = start,
        .end = end,
        .readable = !permissions.empty() && permissions[0] == 'r',
        .executable = permissions.size() >= 3 && permissions[2] == 'x',
    };
    output.fileOffset = fileOffset;
    output.path = std::filesystem::path(trimmedPath);
    return true;
}

[[nodiscard]] std::size_t alignFour(std::size_t value) noexcept {
    constexpr std::size_t alignment = 4;
    return (value + alignment - 1) & ~(alignment - 1);
}

[[nodiscard]] std::string bytesToHex(const std::byte* data, std::size_t size) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < size; ++index) {
        output << std::setw(2)
               << static_cast<unsigned int>(std::to_integer<unsigned char>(data[index]));
    }
    return output.str();
}

[[nodiscard]] std::string readGnuBuildId(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }

    Elf64_Ehdr header{};
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!input || std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB ||
        header.e_machine != EM_AARCH64 ||
        header.e_phentsize != sizeof(Elf64_Phdr) ||
        header.e_phnum == 0) {
        return {};
    }

    std::vector<Elf64_Phdr> programHeaders(header.e_phnum);
    input.seekg(static_cast<std::streamoff>(header.e_phoff), std::ios::beg);
    input.read(
        reinterpret_cast<char*>(programHeaders.data()),
        static_cast<std::streamsize>(programHeaders.size() * sizeof(Elf64_Phdr)));
    if (!input) {
        return {};
    }

    constexpr std::uint64_t maxNoteBytes = 16ULL * 1024ULL * 1024ULL;
    for (const Elf64_Phdr& programHeader : programHeaders) {
        if (programHeader.p_type != PT_NOTE || programHeader.p_filesz == 0 ||
            programHeader.p_filesz > maxNoteBytes ||
            programHeader.p_filesz > std::numeric_limits<std::size_t>::max()) {
            continue;
        }

        std::vector<std::byte> noteBytes(static_cast<std::size_t>(programHeader.p_filesz));
        input.clear();
        input.seekg(static_cast<std::streamoff>(programHeader.p_offset), std::ios::beg);
        input.read(
            reinterpret_cast<char*>(noteBytes.data()),
            static_cast<std::streamsize>(noteBytes.size()));
        if (!input) {
            continue;
        }

        std::size_t cursor = 0;
        while (cursor + sizeof(Elf64_Nhdr) <= noteBytes.size()) {
            Elf64_Nhdr noteHeader{};
            std::memcpy(&noteHeader, noteBytes.data() + cursor, sizeof(noteHeader));
            cursor += sizeof(noteHeader);

            const std::size_t nameSize = noteHeader.n_namesz;
            const std::size_t descriptionSize = noteHeader.n_descsz;
            const std::size_t paddedNameSize = alignFour(nameSize);
            const std::size_t paddedDescriptionSize = alignFour(descriptionSize);

            if (paddedNameSize > noteBytes.size() - cursor) {
                break;
            }
            const std::size_t nameOffset = cursor;
            cursor += paddedNameSize;

            if (paddedDescriptionSize > noteBytes.size() - cursor) {
                break;
            }
            const std::size_t descriptionOffset = cursor;
            cursor += paddedDescriptionSize;

            const bool isGnuName = nameSize >= 3 &&
                std::memcmp(noteBytes.data() + nameOffset, "GNU", 3) == 0;
            if (noteHeader.n_type == NT_GNU_BUILD_ID && isGnuName && descriptionSize > 0) {
                return bytesToHex(noteBytes.data() + descriptionOffset, descriptionSize);
            }
        }
    }

    return {};
}

[[nodiscard]] std::string addressToHex(std::uintptr_t address) {
    std::ostringstream output;
    output << "0x" << std::hex << address;
    return output.str();
}

}  // namespace

std::optional<ModuleFingerprint> inspectLoadedModule(std::string_view moduleName) {
    std::ifstream maps("/proc/self/maps");
    if (!maps) {
        return std::nullopt;
    }

    ModuleFingerprint result;
    result.moduleName = std::string(moduleName);
    result.loadBase = std::numeric_limits<std::uintptr_t>::max();

    std::string line;
    while (std::getline(maps, line)) {
        if (line.find(moduleName) == std::string::npos) {
            continue;
        }

        ParsedMapsLine parsed;
        if (!parseMapsLine(line, parsed)) {
            continue;
        }

        if (parsed.path.empty() ||
            parsed.path.filename().string().find(moduleName) == std::string::npos) {
            continue;
        }

        result.regions.push_back(parsed.region);
        if (parsed.region.start >= parsed.fileOffset) {
            result.loadBase = std::min(result.loadBase, parsed.region.start - parsed.fileOffset);
        }
        if (result.path.empty()) {
            result.path = parsed.path;
        }
    }

    if (result.regions.empty() || result.path.empty()) {
        return std::nullopt;
    }

    if (result.loadBase == std::numeric_limits<std::uintptr_t>::max()) {
        result.loadBase = result.regions.front().start;
    }

    std::error_code fileError;
    result.fileSize = std::filesystem::file_size(result.path, fileError);
    if (fileError) {
        result.fileSize = 0;
    }
    result.buildId = readGnuBuildId(result.path);
    return result;
}

bool isExecutableAddress(
    const ModuleFingerprint& module,
    std::uintptr_t address) noexcept {
    return std::any_of(
        module.regions.begin(),
        module.regions.end(),
        [address](const MemoryRegion& region) {
            return region.readable && region.executable &&
                address >= region.start && address < region.end;
        });
}

std::string readInstructionPrefix(
    const ModuleFingerprint& module,
    std::uintptr_t address,
    std::size_t byteCount) {
    byteCount = std::min<std::size_t>(byteCount, 64);
    const auto matchingRegion = std::find_if(
        module.regions.begin(),
        module.regions.end(),
        [address, byteCount](const MemoryRegion& region) {
            if (!region.readable || !region.executable || address < region.start) {
                return false;
            }
            return byteCount <= region.end - address;
        });
    if (matchingRegion == module.regions.end()) {
        return {};
    }

    const auto* bytes = reinterpret_cast<const std::byte*>(address);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < byteCount; ++index) {
        if (index != 0) {
            output << ' ';
        }
        output << std::setw(2)
               << static_cast<unsigned int>(std::to_integer<unsigned char>(bytes[index]));
    }
    return output.str();
}

bool writeCompatibilityReport(
    const ModuleFingerprint& module,
    std::uintptr_t heartbeatAddress,
    const std::filesystem::path& outputPath,
    std::string& errorMessage) {
    std::ofstream output(outputPath, std::ios::trunc);
    if (!output) {
        errorMessage = "could not open report path";
        return false;
    }

    const std::uintptr_t relativeAddress =
        heartbeatAddress >= module.loadBase ? heartbeatAddress - module.loadBase : 0;
    output << "schema=1\n";
    output << "minecraft_version=" << CompatibilityProfile::minecraftVersion << '\n';
    output << "module_name=" << module.moduleName << '\n';
    output << "module_path=" << module.path.string() << '\n';
    output << "module_file_size=" << module.fileSize << '\n';
    output << "module_load_base=" << addressToHex(module.loadBase) << '\n';
    output << "module_build_id="
           << (module.buildId.empty() ? "unavailable" : module.buildId) << '\n';
    output << "heartbeat_address=" << addressToHex(heartbeatAddress) << '\n';
    output << "heartbeat_relative_address=" << addressToHex(relativeAddress) << '\n';
    output << "heartbeat_prefix="
           << readInstructionPrefix(module, heartbeatAddress, 32) << '\n';
    output << "heartbeat_signature_source=LeviLaunchroid min 1.26.30.00\n";
    output.flush();
    if (!output) {
        errorMessage = "failed while writing report";
        return false;
    }

    errorMessage.clear();
    return true;
}

}  // namespace aeronautics::bedrock
