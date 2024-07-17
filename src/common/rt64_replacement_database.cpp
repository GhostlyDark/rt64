//
// RT64
//

#include "rt64_replacement_database.h"

#include <cinttypes>

namespace RT64 {
    const std::string ReplacementDatabaseFilename = "rt64.json";
    const std::string ReplacementLowMipCacheFilename = "rt64-low-mip-cache.bin";
    const std::string ReplacementKnownExtensions[] = { ".dds", ".png" };
    const uint32_t ReplacementMipmapCacheHeaderMagic = 0x434D4F4CU;
    const uint32_t ReplacementMipmapCacheHeaderVersion = 1U;

    // ReplacementDatabase

    uint32_t ReplacementDatabase::addReplacement(const ReplacementTexture &texture) {
        const uint64_t rt64 = stringToHash(texture.hashes.rt64);
        auto it = tmemHashToReplaceMap.find(rt64);
        if (it != tmemHashToReplaceMap.end()) {
            textures[it->second] = texture;
            return it->second;
        }
        else {
            uint32_t textureIndex = uint32_t(textures.size());
            tmemHashToReplaceMap[rt64] = textureIndex;
            textures.emplace_back(texture);
            return textureIndex;
        }
    }

    void ReplacementDatabase::fixReplacement(const std::string &hash, const ReplacementTexture &texture) {
        const uint64_t rt64Old = stringToHash(hash);
        const uint64_t rt64New = stringToHash(texture.hashes.rt64);
        auto it = tmemHashToReplaceMap.find(rt64Old);
        if (it != tmemHashToReplaceMap.end()) {
            textures[it->second] = texture;
            tmemHashToReplaceMap[rt64New] = it->second;
            tmemHashToReplaceMap.erase(it);
        }
    }

    ReplacementTexture ReplacementDatabase::getReplacement(const std::string &hash) const {
        const uint64_t rt64 = stringToHash(hash);
        auto it = tmemHashToReplaceMap.find(rt64);
        if (it != tmemHashToReplaceMap.end()) {
            return textures[it->second];
        }
        else {
            return ReplacementTexture();
        }
    }

    void ReplacementDatabase::buildHashMaps() {
        tmemHashToReplaceMap.clear();

        for (uint32_t i = 0; i < textures.size(); i++) {
            const ReplacementTexture &texture = textures[i];
            if (!texture.hashes.rt64.empty()) {
                const uint64_t rt64 = stringToHash(texture.hashes.rt64);
                tmemHashToReplaceMap[rt64] = i;
            }
        }
    }

    void ReplacementDatabase::resolvePaths(const std::filesystem::path &directoryPath, std::unordered_map<uint64_t, ReplacementResolvedPath> &resolvedPathMap, bool onlyDDS) {
        // Scan all possible candidates on the filesystem first.
        std::unordered_map<std::string, std::string> autoPathMap;
        for (const std::filesystem::directory_entry &entry : std::filesystem::recursive_directory_iterator(directoryPath)) {
            if (entry.is_regular_file()) {
                std::string fileExtension = toLower(entry.path().extension().u8string());
                if (!isExtensionKnown(fileExtension, onlyDDS)) {
                    continue;
                }

                std::string fileName = entry.path().filename().u8string();
                if (config.autoPath == ReplacementAutoPath::Rice) {
                    size_t firstHashSymbol = fileName.find_first_of("#");
                    size_t lastUnderscoreSymbol = fileName.find_last_of("_");
                    if ((firstHashSymbol != std::string::npos) && (lastUnderscoreSymbol != std::string::npos) && (lastUnderscoreSymbol > firstHashSymbol)) {
                        std::string riceHash = toLower(fileName.substr(firstHashSymbol + 1, lastUnderscoreSymbol - firstHashSymbol - 1));
                        autoPathMap[riceHash] = std::filesystem::relative(entry.path(), directoryPath).u8string();
                    }
                }
                else if (config.autoPath == ReplacementAutoPath::RT64) {
                    assert(false && "Unimplemented.");
                }
            }
        }

        // Clear any existing automatic assignments.
        resolvedPathMap.clear();

        // Assign paths to all entries in the database.
        // If the entry already has a relative path, look for textures with extensions that are valid.
        // If the entry doesn't have a path but uses auto-path logic, then it'll try to resolve the path using that scheme.
        uint32_t textureIndex = 0;
        for (const ReplacementTexture &texture : textures) {
            if (!texture.path.empty()) {
                uint64_t rt64 = ReplacementDatabase::stringToHash(texture.hashes.rt64);
                std::string relativePathBase = removeKnownExtension(texture.path);
                uint32_t knownExtensionCount = onlyDDS ? 1 : std::size(ReplacementKnownExtensions);
                for (uint32_t i = 0; i < knownExtensionCount; i++) {
                    const std::string relativePathKnown = relativePathBase + ReplacementKnownExtensions[i];
                    if (std::filesystem::exists(directoryPath / std::filesystem::u8path(relativePathKnown))) {
                        resolvedPathMap[rt64] = { relativePathKnown, textureIndex };
                        break;
                    }
                }
            }
            else {
                // Assign the correct hash as the search string.
                std::string searchString;
                if (config.autoPath == ReplacementAutoPath::Rice) {
                    searchString = texture.hashes.rice;
                }
                else if (config.autoPath == ReplacementAutoPath::RT64) {
                    searchString = texture.hashes.rt64;
                }

                // Find in the auto path map the entry.
                auto it = autoPathMap.find(searchString);
                if (it != autoPathMap.end()) {
                    uint64_t rt64 = ReplacementDatabase::stringToHash(texture.hashes.rt64);
                    resolvedPathMap[rt64] = { it->second, textureIndex };
                }
            }

            textureIndex++;
        }

        // TODO: Resolve operation filters for paths.
    }

    uint64_t ReplacementDatabase::stringToHash(const std::string &str) {
        return strtoull(str.c_str(), nullptr, 16);
    }

    std::string ReplacementDatabase::hashToString(uint32_t hash) {
        char hexStr[32];
        snprintf(hexStr, sizeof(hexStr), "%08x", hash);
        return std::string(hexStr);
    }

    std::string ReplacementDatabase::hashToString(uint64_t hash) {
        char hexStr[32];
        snprintf(hexStr, sizeof(hexStr), "%016" PRIx64, hash);
        return std::string(hexStr);
    }

    bool ReplacementDatabase::isExtensionKnown(const std::string &extension, bool onlyDDS) {
        uint32_t knownExtensionCount = onlyDDS ? 1 : std::size(ReplacementKnownExtensions);
        for (uint32_t i = 0; i < knownExtensionCount; i++) {
            if (extension == ReplacementKnownExtensions[i]) {
                return true;
            }
        }

        return false;
    }

    bool ReplacementDatabase::endsWith(const std::string &str, const std::string &end) {
        if (str.length() >= end.length()) {
            return (str.compare(str.length() - end.length(), end.length(), end) == 0);
        }
        else {
            return false;
        }
    }

    std::string ReplacementDatabase::toLower(std::string str) {
        std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return std::tolower(c); });
        return str;
    };

    std::string ReplacementDatabase::removeKnownExtension(const std::string &path) {
        const std::string lowerCasePath = toLower(path);
        for (uint32_t i = 0; i < std::size(ReplacementKnownExtensions); i++) {
            if (endsWith(lowerCasePath, ReplacementKnownExtensions[i])) {
                return path.substr(0, path.size() - ReplacementKnownExtensions[i].size());
            }
        }

        return path;
    }

    void to_json(json &j, const ReplacementConfiguration &config) {
        // Always update the configuration version to the latest one when saving.
        ReplacementConfiguration defaultConfig;
        j["autoPath"] = config.autoPath;
        j["configurationVersion"] = defaultConfig.configurationVersion;
        j["hashVersion"] = config.hashVersion;
    }

    void from_json(const json &j, ReplacementConfiguration &config) {
        ReplacementConfiguration defaultConfig;
        config.autoPath = j.value("autoPath", defaultConfig.autoPath);
        config.configurationVersion = j.value("configurationVersion", 1);
        config.hashVersion = j.value("hashVersion", 1);
    }

    void to_json(json &j, const ReplacementHashes &hashes) {
        j["rt64"] = hashes.rt64;
        j["rice"] = hashes.rice;
    }
    
    void from_json(const json &j, ReplacementHashes &hashes) {
        ReplacementHashes defaultHashes;

        // First version of the replacement database specified the hash version directly in the key name.
        // Later versions choose to keep the version global to the file and make RT64 the unique key.
        hashes.rt64 = j.value("rt64v1", defaultHashes.rt64);
        hashes.rt64 = j.value("rt64", hashes.rt64);
        hashes.rice = j.value("rice", defaultHashes.rice);
    }

    void to_json(json &j, const ReplacementTexture &texture) {
        j["path"] = texture.path;
        j["load"] = texture.load;
        j["life"] = texture.life;
        j["hashes"] = texture.hashes;
    }

    void from_json(const json &j, ReplacementTexture &texture) {
        ReplacementTexture defaultTexture;
        texture.path = j.value("path", defaultTexture.path);
        texture.load = j.value("load", defaultTexture.load);
        texture.life = j.value("life", defaultTexture.life);
        texture.hashes = j.value("hashes", defaultTexture.hashes);
    }

    void to_json(json &j, const ReplacementDatabase &db) {
        j["configuration"] = db.config;
        j["textures"] = db.textures;
    }

    void from_json(const json &j, ReplacementDatabase &db) {
        db.config = j.value("configuration", ReplacementConfiguration());
        db.textures = j.value("textures", std::vector<ReplacementTexture>());
        db.buildHashMaps();
    }
};