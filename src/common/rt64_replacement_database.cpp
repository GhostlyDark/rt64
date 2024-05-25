//
// RT64
//

#include "rt64_replacement_database.h"

namespace RT64 {
    // ReplacementDatabase

    void ReplacementDatabase::addReplacement(const ReplacementTexture &texture) {
        const uint64_t rt64v1 = stringToHash(texture.hashes.rt64v1);
        auto it = tmemHashToReplaceMap.find(rt64v1);
        if (it != tmemHashToReplaceMap.end()) {
            textures[it->second] = texture;
        }
        else {
            textures.emplace_back(texture);
        }
    }

    void ReplacementDatabase::buildHashMaps() {
        tmemHashToReplaceMap.clear();

        for (uint32_t i = 0; i < textures.size(); i++) {
            const ReplacementTexture &texture = textures[i];
            if (!texture.hashes.rt64v1.empty()) {
                const uint64_t rt64v1 = stringToHash(texture.hashes.rt64v1);
                tmemHashToReplaceMap[rt64v1] = i;
            }
        }
    }

    uint64_t ReplacementDatabase::stringToHash(const std::string &str) {
        return strtoull(str.c_str(), nullptr, 16);
    }

    std::string ReplacementDatabase::hashToString(uint32_t hash) {
        char hexStr[32];
        snprintf(hexStr, sizeof(hexStr), "%08lx", hash);
        return std::string(hexStr);
    }

    std::string ReplacementDatabase::hashToString(uint64_t hash) {
        char hexStr[32];
        snprintf(hexStr, sizeof(hexStr), "%016llx", hash);
        return std::string(hexStr);
    }

    void to_json(json &j, const ReplacementConfiguration &config) {
        j["autoPath"] = config.autoPath;
    }

    void from_json(const json &j, ReplacementConfiguration &config) {
        ReplacementConfiguration defaultConfig;
        config.autoPath = j.value("autoPath", defaultConfig.autoPath);
    }

    void to_json(json &j, const ReplacementHashes &hashes) {
        j["rt64v1"] = hashes.rt64v1;
        j["rice"] = hashes.rice;
    }
    
    void from_json(const json &j, ReplacementHashes &hashes) {
        ReplacementHashes defaultHashes;
        hashes.rt64v1 = j.value("rt64v1", defaultHashes.rt64v1);
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