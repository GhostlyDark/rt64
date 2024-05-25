//
// RT64
//

#include "rt64_rdp_tmem.h"

#include <cassert>

#include "rt64_state.h"

namespace RT64 {
    // TextureManager

    uint64_t TextureManager::uploadTMEM(State *state, TextureCache *textureCache, uint64_t creationFrame, uint16_t byteOffset, uint16_t byteCount) {
        XXH3_state_t xxh3;
        XXH3_64bits_reset(&xxh3);
        const uint8_t *TMEM = reinterpret_cast<const uint8_t *>(state->rdp->TMEM);
        XXH3_64bits_update(&xxh3, &TMEM[byteOffset], byteCount);
        XXH3_64bits_update(&xxh3, &byteOffset, sizeof(byteOffset));
        XXH3_64bits_update(&xxh3, &byteCount, sizeof(byteCount));
        const uint64_t hash = XXH3_64bits_digest(&xxh3);
        if (hashSet.find(hash) == hashSet.end()) {
            hashSet.insert(hash);
            textureCache->queueGPUUploadTMEM(hash, creationFrame, TMEM, RDP_TMEM_BYTES, 0, 0, 0, LoadTile());
        }

        return hash;
    }

    uint64_t TextureManager::uploadTexture(State *state, const LoadTile &loadTile, TextureCache *textureCache, uint64_t creationFrame, uint16_t width, uint16_t height, uint32_t tlut) {
        XXH3_state_t xxh3;
        XXH3_64bits_reset(&xxh3);
        const bool RGBA32 = (loadTile.siz == G_IM_SIZ_32b) && (loadTile.fmt == G_IM_FMT_RGBA);
        const uint8_t *TMEM = reinterpret_cast<const uint8_t *>(state->rdp->TMEM);
        const uint32_t tmemSize = RGBA32 ? (RDP_TMEM_BYTES >> 1) : RDP_TMEM_BYTES;
        const uint32_t lastRowBytes = width << std::min(loadTile.siz, uint8_t(G_IM_SIZ_16b)) >> 1;
        const uint32_t bytesToHash = (loadTile.line << 3) * (height - 1) + lastRowBytes;
        const uint32_t tmemMask = RGBA32 ? RDP_TMEM_MASK16 : RDP_TMEM_MASK8;
        const uint32_t tmemAddress = (loadTile.tmem << 3) & tmemMask;
        auto hashTMEM = [&](uint32_t tmemOrAddress) {
            // Too many bytes to hash in a single step. Wrap around TMEM and hash the rest.
            if ((tmemAddress + bytesToHash) > tmemSize) {
                const uint32_t firstBytes = std::min(bytesToHash, std::max(tmemSize - tmemAddress, 0U));
                XXH3_64bits_update(&xxh3, &TMEM[tmemAddress | tmemOrAddress], firstBytes);
                XXH3_64bits_update(&xxh3, &TMEM[tmemOrAddress], std::min(bytesToHash - firstBytes, tmemAddress));
            }
            // Hash as normal.
            else {
                XXH3_64bits_update(&xxh3, &TMEM[tmemAddress | tmemOrAddress], bytesToHash);
            }
        };

        hashTMEM(0x0);

        if (RGBA32) {
            hashTMEM(tmemSize);
        }
        
        // If TLUT is active, we also hash the corresponding palette bytes.
        if (tlut > 0) {
            const bool CI4 = (loadTile.siz == G_IM_SIZ_4b);
            const int32_t paletteOffset = CI4 ? (loadTile.palette << 7) : 0;
            const int32_t bytesToHash = CI4 ? 0x80 : 0x800;
            const int32_t paletteAddress = (RDP_TMEM_BYTES >> 1) + paletteOffset;
            XXH3_64bits_update(&xxh3, &TMEM[paletteAddress], bytesToHash);
        }

        // Encode more parameters into the hash that affect the final RGBA32 output.
        XXH3_64bits_update(&xxh3, &width, sizeof(width));
        XXH3_64bits_update(&xxh3, &height, sizeof(height));
        XXH3_64bits_update(&xxh3, &tlut, sizeof(tlut));
        XXH3_64bits_update(&xxh3, &loadTile.line, sizeof(loadTile.line));
        XXH3_64bits_update(&xxh3, &loadTile.siz, sizeof(loadTile.siz));
        XXH3_64bits_update(&xxh3, &loadTile.fmt, sizeof(loadTile.fmt));

        const uint64_t hash = XXH3_64bits_digest(&xxh3);
        if (hashSet.find(hash) == hashSet.end()) {
            hashSet.insert(hash);
            textureCache->queueGPUUploadTMEM(hash, creationFrame, TMEM, RDP_TMEM_BYTES, width, height, tlut, loadTile);
        }

        // Dump memory contents into a file if the process is active.
        if (!state->dumpingTexturesDirectory.empty()) {
            dumpTexture(hash, state, loadTile, width, height, tlut);
        }

        return hash;
    }

    void TextureManager::dumpTexture(uint64_t hash, State *state, const LoadTile &loadTile, uint16_t width, uint16_t height, uint32_t tlut) {
        if (dumpedSet.find(hash) != dumpedSet.end()) {
            return;
        }

        // Insert into set regardless of whether the dump is successful or not.
        dumpedSet.insert(hash);
        
        // Dump the entirety of TMEM.
        char hexStr[64];
        snprintf(hexStr, sizeof(hexStr), "%016llx", hash);
        std::filesystem::path dumpTmemPath = state->dumpingTexturesDirectory / (std::string(hexStr) + ".tmem");
        std::ofstream dumpTmemStream(dumpTmemPath, std::ios::binary);
        if (dumpTmemStream.is_open()) {
            const char *TMEM = reinterpret_cast<const char *>(state->rdp->TMEM);
            dumpTmemStream.write(TMEM, RDP_TMEM_BYTES);
            dumpTmemStream.close();
        }

        // Dump the RDRAM last loaded into the TMEM address pointed to by the tile. Required for generating hashes used by Rice.
        const LoadOperation &loadOp = state->rdp->rice.lastLoadOpByTMEM[loadTile.tmem];
        uint32_t rdramStart = loadOp.texture.address;
        uint32_t rdramCount = 0;
        uint32_t commonBytesOffset = (loadOp.tile.uls >> 2) << loadOp.texture.siz >> 1;
        uint32_t commonBytesPerRow = loadOp.texture.width << loadOp.texture.siz >> 1;
        if (loadOp.type == LoadOperation::Type::Block) {
            uint32_t wordCount = ((loadOp.tile.lrs - loadOp.tile.uls) >> (4 - loadOp.tile.siz)) + 1;
            rdramStart = loadOp.texture.address + commonBytesOffset + commonBytesPerRow * loadOp.tile.ult;
            rdramCount = (wordCount << 3);
        }
        else if (loadOp.type == LoadOperation::Type::Tile) {
            uint32_t rowCount = 1 + ((loadOp.tile.lrt >> 2) - (loadOp.tile.ult >> 2));
            uint32_t tileWidth = ((loadOp.tile.lrs >> 2) - (loadOp.tile.uls >> 2));
            uint32_t wordsPerRow = (tileWidth >> (4 - loadOp.tile.siz)) + 1;
            rdramStart = loadOp.texture.address + commonBytesOffset + commonBytesPerRow * (loadOp.tile.ult >> 2);
            rdramCount = rowCount * commonBytesPerRow;
        }
        
        // Dump more RDRAM if necessary if it doesn't cover what the tile could possibly sample.
        uint32_t loadTileBpr = width << loadTile.siz >> 1;
        rdramCount = std::max(rdramCount, std::max(loadTileBpr, commonBytesPerRow) * height);

        if (rdramCount > 0) {
            std::filesystem::path dumpRdramPath = state->dumpingTexturesDirectory / (std::string(hexStr) + ".rice.rdram");
            std::ofstream dumpRdramStream(dumpRdramPath, std::ios::binary);
            if (dumpRdramStream.is_open()) {
                const char *RDRAM = reinterpret_cast<const char *>(state->RDRAM);
                dumpRdramStream.write(&RDRAM[rdramStart], rdramCount);
                dumpRdramStream.close();
            }

            std::filesystem::path dumpRdramInfoPath = state->dumpingTexturesDirectory / (std::string(hexStr) + ".rice.json");
            std::ofstream dumpRdramInfoStream(dumpRdramInfoPath);
            if (dumpRdramInfoStream.is_open()) {
                json jroot;
                jroot["tile"] = loadOp.tile;
                jroot["type"] = loadOp.type;
                jroot["texture"] = loadOp.texture;
                dumpRdramInfoStream << std::setw(4) << jroot << std::endl;
                dumpRdramInfoStream.close();
            }
        }
        
        // Repeat a similar process for dumping the palette.
        if (tlut > 0) {
            const bool CI4 = (loadTile.siz == G_IM_SIZ_4b);
            const int32_t paletteTMEM = (RDP_TMEM_WORDS >> 1) + (CI4 ? (loadTile.palette << 4) : 0);
            const LoadOperation &paletteLoadOp = state->rdp->rice.lastLoadOpByTMEM[paletteTMEM];
            uint32_t paletteBytesOffset = (paletteLoadOp.tile.uls >> 2) << paletteLoadOp.texture.siz >> 1;
            uint32_t paletteBytesPerRow = paletteLoadOp.texture.width << paletteLoadOp.texture.siz >> 1;
            const uint32_t rowCount = 1 + ((paletteLoadOp.tile.lrt >> 2) - (paletteLoadOp.tile.ult >> 2));
            const uint32_t wordsPerRow = ((paletteLoadOp.tile.lrs >> 2) - (paletteLoadOp.tile.uls >> 2)) + 1;
            uint32_t paletteRdramStart = paletteLoadOp.texture.address + paletteBytesOffset + paletteBytesPerRow * (paletteLoadOp.tile.ult >> 2);
            uint32_t paletteRdramCount = (rowCount - 1) * paletteBytesPerRow + (wordsPerRow << 3);
            if (paletteRdramCount > 0) {
                std::filesystem::path dumpPaletteRdramPath = state->dumpingTexturesDirectory / (std::string(hexStr) + ".rice.palette.rdram");
                std::ofstream dumpPaletteRdramStream(dumpPaletteRdramPath, std::ios::binary);
                if (dumpPaletteRdramStream.is_open()) {
                    const char *RDRAM = reinterpret_cast<const char *>(state->RDRAM);
                    dumpPaletteRdramStream.write(&RDRAM[paletteRdramStart], paletteRdramCount);
                    dumpPaletteRdramStream.close();
                }
            }

            std::filesystem::path dumpPaletteRdramInfoPath = state->dumpingTexturesDirectory / (std::string(hexStr) + ".rice.palette.json");
            std::ofstream dumpPaletteRdramInfoStream(dumpPaletteRdramInfoPath);
            if (dumpPaletteRdramInfoStream.is_open()) {
                json jroot;
                jroot["tile"] = paletteLoadOp.tile;
                jroot["type"] = paletteLoadOp.type;
                jroot["texture"] = paletteLoadOp.texture;
                dumpPaletteRdramInfoStream << std::setw(4) << jroot << std::endl;
                dumpPaletteRdramInfoStream.close();
            }
        }

        // Dump the parameters of the tile into a JSON file.
        std::filesystem::path dumpTilePath = state->dumpingTexturesDirectory / (std::string(hexStr) + ".tile.json");
        std::ofstream dumpTileStream(dumpTilePath);
        if (dumpTileStream.is_open()) {
            json jroot;
            jroot["tile"] = loadTile;
            jroot["width"] = width;
            jroot["height"] = height;

            // Serialize the TLUT into an enum instead.
            if (tlut == G_TT_RGBA16) {
                jroot["tlut"] = LoadTLUT::RGBA16;
            }
            else if (tlut == G_TT_IA16) {
                jroot["tlut"] = LoadTLUT::IA16;
            }
            else {
                jroot["tlut"] = LoadTLUT::None;
            }

            dumpTileStream << std::setw(4) << jroot << std::endl;
            dumpTileStream.close();
        }
    }

    void TextureManager::removeHashes(const std::vector<uint64_t> &hashes) {
        for (uint64_t hash : hashes) {
            hashSet.erase(hash);
        }
    }

    bool TextureManager::requiresRawTMEM(const LoadTile &loadTile, uint16_t width, uint16_t height) {
        const bool RGBA32 = (loadTile.siz == G_IM_SIZ_32b) && (loadTile.fmt == G_IM_FMT_RGBA);
        const uint32_t tmemSize = RGBA32 ? (RDP_TMEM_BYTES >> 1) : RDP_TMEM_BYTES;
        const uint32_t lastRowBytes = width << std::min(loadTile.siz, uint8_t(G_IM_SIZ_16b)) >> 1;
        const uint32_t bytesToHash = (loadTile.line << 3) * (height - 1) + lastRowBytes;
        return (bytesToHash > tmemSize);
    }
};