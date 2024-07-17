//
// RT64
//

#include <filesystem>
#include <fstream>
#include <set>

#include <ddspp/ddspp.h>

#include "../../common/rt64_replacement_database.cpp"

static bool loadBytesFromFile(const std::filesystem::path &path, std::vector<uint8_t> &bytes) {
    std::ifstream file(path, std::ios::binary);
    if (file.is_open()) {
        file.seekg(0, std::ios::end);
        bytes.resize(file.tellg());
        file.seekg(0, std::ios::beg);
        file.read((char *)(bytes.data()), bytes.size());
        return !file.bad();
    }
    else {
        return false;
    }
}

void showHelp() {
    fprintf(stderr,
        "texture_packer <path> --create-low-mip-cache\n"
        "\tGenerate the cache used for streaming textures in by extracting the lowest quality mipmaps. Must be 64 pixels or lower in both dimensions.\n\n"
        "texture_packer <path> --create-pack\n"
        "\tCreate the pack by including all the textures supported by the database and the low mip cache.\n\n"
    );
}

uint32_t computePadding(uint32_t position, uint32_t alignment) {
    if ((position % alignment) != 0) {
        return alignment - (position % alignment);
    }
    else {
        return 0;
    }
}

bool extractLowMipsToStream(const std::filesystem::path &directoryPath, const std::string &relativePath, std::ofstream &lowMipCacheStream) {
    thread_local std::vector<uint8_t> mipSourceBytes;
    if (!loadBytesFromFile(directoryPath / std::filesystem::u8path(relativePath), mipSourceBytes)) {
        fprintf(stderr, "Unable to open file at %s\n", relativePath.c_str());
        return false;
    }

    ddspp::Descriptor ddsDescriptor;
    ddspp::Result result = ddspp::decode_header(mipSourceBytes.data(), ddsDescriptor);
    if (result != ddspp::Success) {
        return false;
    }

    // Search for the lowest mipmap to start extracting from.
    const uint32_t minPixelCount = 96 * 96;
    uint32_t mipStart = 0;
    while (mipStart < (ddsDescriptor.numMips - 1)) {
        const uint32_t mipPixelCount = (ddsDescriptor.width >> mipStart) * (ddsDescriptor.height >> mipStart);
        if (mipPixelCount <= minPixelCount) {
            break;
        }

        mipStart++;
    }

    // Write out the cache header.
    const uint32_t dataAlignment = 16;
    RT64::ReplacementMipmapCacheHeader cacheHeader;
    cacheHeader.width = std::max(ddsDescriptor.width >> mipStart, 1U);
    cacheHeader.height = std::max(ddsDescriptor.height >> mipStart, 1U);
    cacheHeader.dxgiFormat = ddsDescriptor.format;
    cacheHeader.mipCount = ddsDescriptor.numMips - mipStart;
    cacheHeader.pathLength = relativePath.size();
    lowMipCacheStream.write(reinterpret_cast<const char *>(&cacheHeader), sizeof(cacheHeader));

    // Compute the padding and offsets for each mipmap that will be dumped.
    thread_local std::vector<uint32_t> mipOffsets;
    thread_local std::vector<uint32_t> mipSizes;
    thread_local std::vector<uint32_t> mipPaddings;
    mipOffsets.clear();
    mipSizes.clear();
    mipPaddings.clear();
    for (uint32_t i = 0; i < cacheHeader.mipCount; i++) {
        bool isLastMip = (i == (cacheHeader.mipCount - 1));
        uint32_t mipOffset = ddspp::get_offset(ddsDescriptor, mipStart + i, 0);
        uint32_t mipSize = (isLastMip ? uint32_t(mipSourceBytes.size() - ddsDescriptor.headerSize) : ddspp::get_offset(ddsDescriptor, mipStart + i + 1, 0)) - mipOffset;
        uint32_t mipPadding = computePadding(mipSize, dataAlignment);
        uint32_t mipTotalSize = mipSize + mipPadding;
        mipOffsets.emplace_back(mipOffset);
        mipSizes.emplace_back(mipSize);
        mipPaddings.emplace_back(mipPadding);
        lowMipCacheStream.write(reinterpret_cast<const char *>(&mipTotalSize), sizeof(mipTotalSize));
    }

    lowMipCacheStream.write(relativePath.c_str(), relativePath.size());

    const uint32_t namePadding = computePadding(std::streamoff(lowMipCacheStream.tellp()), dataAlignment);
    for (uint32_t i = 0; i < namePadding; i++) {
        lowMipCacheStream.put(0);
    }

    for (uint32_t i = 0; i < cacheHeader.mipCount; i++) {
        // Write the mipmap data.
        lowMipCacheStream.write(reinterpret_cast<const char *>(&mipSourceBytes[ddsDescriptor.headerSize + mipOffsets[i]]), mipSizes[i]);

        // Add extra padding as required.
        for (uint32_t j = 0; j < mipPaddings[i]; j++) {
            lowMipCacheStream.put(0);
        }
    }

    return true;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        showHelp();
        return 1;
    }

    std::filesystem::path searchDirectory(argv[1]);
    if (!std::filesystem::is_directory(searchDirectory)) {
        std::string u8string = searchDirectory.u8string();
        fprintf(stderr, "The directory %s does not exist.", u8string.c_str());
        return 1;
    }

    enum class Mode {
        Unknown,
        CreateLowMipCache,
        CreatePack
    };

    Mode mode = Mode::Unknown;
    if (argc > 2) {
        std::string modeString = argv[2];
        if ((modeString == "--create-low-mip-cache") || (modeString == "-m")) {
            fprintf(stdout, "Creating low mip cache.\n");
            mode = Mode::CreateLowMipCache;
        }
        else if ((modeString == "--create-pack") || (modeString == "-p")) {
            fprintf(stdout, "Creating pack.\n");
            mode = Mode::CreatePack;
        }
        else {
            fprintf(stderr, "Unrecognized argument %s.\n\n", modeString.c_str());
            showHelp();
            return 1;
        }
    }

    RT64::ReplacementDatabase database;
    std::filesystem::path databasePath = searchDirectory / RT64::ReplacementDatabaseFilename;
    if (!std::filesystem::exists(databasePath)) {
        fprintf(stderr, "Database file %s is missing.\n", RT64::ReplacementDatabaseFilename.c_str());
        return 1;
    }

    fprintf(stdout, "Opening database file...\n");

    std::ifstream databaseStream(databasePath);
    if (databaseStream.is_open()) {
        try {
            json jroot;
            databaseStream >> jroot;
            database = jroot;
            if (databaseStream.bad()) {
                std::string u8string = databasePath.u8string();
                fprintf(stderr, "Failed to read database file at %s.", u8string.c_str());
                return 1;
            }
        }
        catch (const nlohmann::detail::exception &e) {
            fprintf(stderr, "JSON parsing error: %s\n", e.what());
            return 1;
        }

        databaseStream.close();
    }

    fprintf(stdout, "Resolving database paths...\n");

    // Resolve all paths for the database and build a unique set of files.
    std::set<std::string> resolvedPathSet;
    std::unordered_map<uint64_t, RT64::ReplacementResolvedPath> resolvedPathMap;
    database.resolvePaths(searchDirectory, resolvedPathMap, mode == Mode::CreateLowMipCache);

    for (auto it : resolvedPathMap) {
        // TODO: Ignore entries that don't use streaming.

        resolvedPathSet.insert(it.second.relativePath);
    }

    if (mode == Mode::CreateLowMipCache) {
        std::filesystem::path lowMipCachePath = searchDirectory / RT64::ReplacementLowMipCacheFilename;
        std::ofstream lowMipCacheStream(lowMipCachePath, std::ios::binary);
        if (!lowMipCacheStream.is_open()) {
            std::string u8string = lowMipCachePath.u8string();
            fprintf(stderr, "Failed to open low mip cache file at %s for writing.", u8string.c_str());
            return 1;
        }

        uint32_t processCount = 0;
        uint32_t processTotal = resolvedPathSet.size();
        for (auto it : resolvedPathSet) {
            if ((processCount % 100) == 0 || (processCount == (processTotal - 1))) {
                fprintf(stdout, "Processing (%d/%d): %s.\n", processCount, processTotal, it.c_str());
            }

            if (!extractLowMipsToStream(searchDirectory, it, lowMipCacheStream)) {
                fprintf(stderr, "Failed to extract low mip to cache from file %s.", it.c_str());
                return 1;
            }

            processCount++;
        }
    }
    else if (mode == Mode::CreatePack) {
        assert(false && "Unimplemented.");
    }

    return 0;
}