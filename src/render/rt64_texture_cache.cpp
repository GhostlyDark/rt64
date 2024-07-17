//
// RT64
//

#define STB_IMAGE_IMPLEMENTATION

#include "ddspp/ddspp.h"
#include "stb/stb_image.h"
#include "xxHash/xxh3.h"

#include "gbi/rt64_f3d.h"
#include "common/rt64_load_types.h"
#include "common/rt64_thread.h"
#include "common/rt64_tmem_hasher.h"
#include "hle/rt64_workload_queue.h"

#include "rt64_texture_cache.h"

namespace RT64 {
    // ReplacementMap
    
    static const interop::float2 IdentityScale = { 1.0f, 1.0f };

    static size_t computePadding(size_t position, size_t alignment) {
        if ((position % alignment) != 0) {
            return alignment - (position % alignment);
        }
        else {
            return 0;
        }
    }

    ReplacementMap::ReplacementMap() {
        // Empty constructor.
    }

    ReplacementMap::~ReplacementMap() {
        for (Texture *texture : loadedTextures) {
            delete texture;
        }

        for (auto it : lowMipCacheTextures) {
            delete it.second;
        }
    }

    void ReplacementMap::clear(std::vector<Texture *> &evictedTextures) {
        for (Texture *texture : loadedTextures) {
            evictedTextures.emplace_back(texture);
        }

        for (auto it : lowMipCacheTextures) {
            evictedTextures.emplace_back(it.second);
        }

        loadedTextures.clear();
        pathHashToLoadMap.clear();
        resolvedPathMap.clear();
        lowMipCacheTextures.clear();
    }

    bool ReplacementMap::readDatabase(std::istream &stream) {
        try {
            json jroot;
            stream >> jroot;
            db = jroot;
            if (!stream.bad()) {
                return true;
            }
        }
        catch (const nlohmann::detail::exception &e) {
            fprintf(stderr, "JSON parsing error: %s\n", e.what());
            db = ReplacementDatabase();
        }

        return false;
    }

    bool ReplacementMap::saveDatabase(std::ostream &stream) {
        try {
            json jroot = db;
            stream << std::setw(4) << jroot << std::endl;
            return !stream.bad();
        }
        catch (const nlohmann::detail::exception &e) {
            fprintf(stderr, "JSON writing error: %s\n", e.what());
            return false;
        }
    }

    void ReplacementMap::removeUnusedEntriesFromDatabase() {
        std::vector<ReplacementTexture> newTextures;
        for (const ReplacementTexture &texture : db.textures) {
            uint64_t rt64 = ReplacementDatabase::stringToHash(texture.hashes.rt64);
            auto pathIt = resolvedPathMap.find(rt64);

            // Only consider for removal if the entry has no assigned path.
            if (texture.path.empty()) {
                if (pathIt == resolvedPathMap.end()) {
                    continue;
                }

                if (pathIt->second.relativePath.empty()) {
                    continue;
                }
            }
            
            // Update the database index of the resolved path.
            if (pathIt != resolvedPathMap.end()) {
                pathIt->second.databaseIndex = uint32_t(newTextures.size());
            }

            newTextures.emplace_back(texture);
        }

        db.textures = newTextures;
        db.buildHashMaps();
    }

    bool ReplacementMap::getInformationFromHash(uint64_t tmemHash, std::string &relativePath, uint32_t &databaseIndex) const {
        auto pathIt = resolvedPathMap.find(tmemHash);
        if (pathIt != resolvedPathMap.end()) {
            relativePath = pathIt->second.relativePath;
            databaseIndex = pathIt->second.databaseIndex;
            return true;
        }

        return false;
    }

    void ReplacementMap::addLoadedTexture(Texture *texture, const std::string &relativePath) {
        const uint64_t pathHash = hashFromRelativePath(relativePath);
        pathHashToLoadMap[pathHash] = uint32_t(loadedTextures.size());
        loadedTextures.emplace_back(texture);
    }

    Texture *ReplacementMap::getFromRelativePath(const std::string &relativePath) const {
        uint64_t pathHash = hashFromRelativePath(relativePath);
        auto it = pathHashToLoadMap.find(pathHash);
        if (it != pathHashToLoadMap.end()) {
            return loadedTextures[it->second];
        }
        else {
            return nullptr;
        }
    }

    uint64_t ReplacementMap::hashFromRelativePath(const std::string &relativePath) const {
        return XXH3_64bits(relativePath.data(), relativePath.size());
    }

    // TextureMap

    TextureMap::TextureMap() {
        globalVersion = 0;
        replacementMapEnabled = true;
    }

    TextureMap::~TextureMap() {
        for (Texture *texture : textures) {
            delete texture;
        }

        for (Texture *texture : evictedTextures) {
            delete texture;
        }
    }

    void TextureMap::clearReplacements() {
        for (size_t i = 0; i < textureReplacements.size(); i++) {
            if (textureReplacements[i] != nullptr) {
                textureReplacements[i] = nullptr;
                versions[i]++;
            }
        }

        globalVersion++;
    }

    void TextureMap::add(uint64_t hash, uint64_t creationFrame, Texture *texture) {
        assert(hashMap.find(hash) == hashMap.end());

        // Check for free spaces on the LIFO queue first.
        uint32_t textureIndex;
        if (!freeSpaces.empty()) {
            textureIndex = freeSpaces.back();
            freeSpaces.pop_back();
        }
        else {
            textureIndex = static_cast<uint32_t>(textures.size());
            textures.push_back(nullptr);
            textureReplacements.push_back(nullptr);
            textureScales.push_back(IdentityScale);
            hashes.push_back(0);
            versions.push_back(0);
            creationFrames.push_back(0);
            listIterators.push_back(accessList.end());
        }

        hashMap[hash] = textureIndex;
        textures[textureIndex] = texture;
        textureReplacements[textureIndex] = nullptr;
        textureScales[textureIndex] = IdentityScale;
        hashes[textureIndex] = hash;
        versions[textureIndex]++;
        creationFrames[textureIndex] = creationFrame;
        globalVersion++;

        accessList.push_front({ textureIndex, creationFrame });
        listIterators[textureIndex] = accessList.begin();
    }

    void TextureMap::replace(uint64_t hash, Texture *texture, bool ignoreIfFull) {
        const auto it = hashMap.find(hash);
        if (it == hashMap.end()) {
            return;
        }

        if (ignoreIfFull && (textureReplacements[it->second] != nullptr)) {
            return;
        }

        Texture *replacedTexture = textures[it->second];
        textureReplacements[it->second] = texture;
        textureScales[it->second] = { float(texture->width) / float(replacedTexture->width), float(texture->height) / float(replacedTexture->height) };
        versions[it->second]++;
        globalVersion++;
    }

    bool TextureMap::use(uint64_t hash, uint64_t submissionFrame, uint32_t &textureIndex, interop::float2 &textureScale, bool &textureReplaced, bool &hasMipmaps) {
        // Find the matching texture index in the hash map.
        const auto it = hashMap.find(hash);
        if (it == hashMap.end()) {
            textureIndex = 0;
            textureScale = IdentityScale;
            return false;
        }

        textureIndex = it->second;
        textureReplaced = replacementMapEnabled && (textureReplacements[textureIndex] != nullptr);

        if (textureReplaced) {
            textureScale = textureScales[textureIndex];
            hasMipmaps = (textureReplacements[textureIndex]->mipmaps > 1);
        }
        else {
            textureScale = IdentityScale;
            hasMipmaps = false;
        }

        // Remove the existing entry from the list if it exists.
        AccessList::iterator listIt = listIterators[textureIndex];
        if (listIt != accessList.end()) {
            accessList.erase(listIt);
        }

        // Push a new access entry to the front of the list and store the new iterator.
        accessList.push_front({ textureIndex, submissionFrame });
        listIterators[textureIndex] = accessList.begin();
        return true;
    }

    bool TextureMap::evict(uint64_t submissionFrame, std::vector<uint64_t> &evictedHashes) {
        evictedHashes.clear();

        auto it = accessList.rbegin();
        while (it != accessList.rend()) {
            assert(submissionFrame >= it->second);
            
            // The max age allowed is the difference between the last time the texture was used and the time it was uploaded.
            // Ensure the textures live long enough for the frame queue to use them.
            const uint64_t MinimumMaxAge = WORKLOAD_QUEUE_SIZE * 2;
            const uint64_t age = submissionFrame - it->second;
            const uint64_t maxAge = std::max(it->second - creationFrames[it->first], MinimumMaxAge);

            // Evict all entries that are present in the access list and are older than the frame by the specified margin.
            if (age >= maxAge) {
                const uint32_t textureIndex = it->first;
                const uint64_t textureHash = hashes[textureIndex];
                evictedTextures.emplace_back(textures[textureIndex]);
                textures[textureIndex] = nullptr;
                textureScales[textureIndex] = { 1.0f, 1.0f };
                textureReplacements[textureIndex] = nullptr;
                hashes[textureIndex] = 0;
                creationFrames[textureIndex] = 0;
                freeSpaces.push_back(textureIndex);
                listIterators[textureIndex] = accessList.end();
                hashMap.erase(textureHash);
                evictedHashes.push_back(textureHash);
                it = decltype(it)(accessList.erase(std::next(it).base()));
            }
            // Stop iterating if we reach an entry that has been used in the present.
            else if (age == 0) {
                break;
            }
            else {
                it++;
            }
        }

        return !evictedHashes.empty();
    }

    Texture *TextureMap::get(uint32_t index) const {
        assert(index < textures.size());
        return textures[index];
    }

    size_t TextureMap::getMaxIndex() const {
        return textures.size();
    }

    // TextureCache::StreamThread

    TextureCache::StreamThread::StreamThread(TextureCache *textureCache) {
        assert(textureCache != nullptr);

        this->textureCache = textureCache;

        worker = std::make_unique<RenderWorker>(textureCache->worker->device, "RT64 Stream Worker", RenderCommandListType::COMPUTE);
        thread = std::make_unique<std::thread>(&StreamThread::loop, this);
        threadRunning = false;
    }

    TextureCache::StreamThread::~StreamThread() {
        threadRunning = false;
        textureCache->streamDescQueueChanged.notify_all();
        thread->join();
        thread.reset(nullptr);
    }

    void TextureCache::StreamThread::loop() {
        Thread::setCurrentThreadName("RT64 Stream");

        // Texture streaming threads should have a priority somewhere inbetween the main threads and the shader compilation threads.
        Thread::setCurrentThreadPriority(Thread::Priority::Low);

        threadRunning = true;

        std::vector<uint8_t> replacementBytes;
        while (threadRunning) {
            StreamDescription streamDesc;

            // Check the top of the queue or wait if it's empty.
            {
                std::unique_lock queueLock(textureCache->streamDescQueueMutex);
                textureCache->streamDescQueueActiveCount--;
                textureCache->streamDescQueueChanged.wait(queueLock, [this]() {
                    return !threadRunning || !textureCache->streamDescQueue.empty();
                });

                textureCache->streamDescQueueActiveCount++;

                if (!textureCache->streamDescQueue.empty()) {
                    streamDesc = textureCache->streamDescQueue.front();
                    textureCache->streamDescQueue.pop();
                }
            }

            if (!streamDesc.filePath.empty()) {
                HashTexturePair hashTexturePair;
                hashTexturePair.hash = streamDesc.hash;

                // Check again if the texture file hasn't been loaded into the replacement map yet.
                {
                    std::unique_lock replacementMapLock(textureCache->textureMap.replacementMapMutex);
                    hashTexturePair.texture = textureCache->textureMap.replacementMap.getFromRelativePath(streamDesc.relativePath);
                }
                
                // Load the bytes from the file and decode the texture.
                if ((hashTexturePair.texture == nullptr) && TextureCache::loadBytesFromPath(streamDesc.filePath, replacementBytes)) {
                    RenderWorkerExecution execution(worker.get());
                    hashTexturePair.texture = TextureCache::loadTextureFromBytes(worker.get(), replacementBytes, uploadResource, nullptr, nullptr, streamDesc.minMipWidth, streamDesc.minMipHeight);
                    if (hashTexturePair.texture != nullptr) {
                        std::unique_lock replacementMapLock(textureCache->textureMap.replacementMapMutex);
                        textureCache->textureMap.replacementMap.addLoadedTexture(hashTexturePair.texture, streamDesc.relativePath);
                    }
                }

                // Add the texture to be added back in the next time the texture cache is unlocked.
                if (hashTexturePair.texture != nullptr) {
                    std::unique_lock streamedLock(textureCache->streamedTextureQueueMutex);
                    textureCache->streamedTextureQueue.push(hashTexturePair);
                }
            }
        }
    }

    // TextureCache

    TextureCache::TextureCache(RenderWorker *worker, uint32_t threadCount, const ShaderLibrary *shaderLibrary, bool developerMode) {
        assert(worker != nullptr);

        this->worker = worker;
        this->shaderLibrary = shaderLibrary;
        this->developerMode = developerMode;

        lockCounter = 0;
        uploadThread = new std::thread(&TextureCache::uploadThreadLoop, this);

        RenderPoolDesc poolDesc;
        poolDesc.heapType = RenderHeapType::UPLOAD;
        poolDesc.useLinearAlgorithm = true;
        poolDesc.allowOnlyBuffers = true;
        uploadResourcePool = worker->device->createPool(poolDesc);

        streamDescQueueActiveCount = threadCount;

        for (uint32_t i = 0; i < threadCount; i++) {
            streamThreads.push_back(std::make_unique<StreamThread>(this));
        }
    }

    TextureCache::~TextureCache() {
        if (uploadThread != nullptr) {
            uploadThreadRunning = false;
            uploadQueueChanged.notify_all();
            uploadThread->join();
            delete uploadThread;
        }
        
        descriptorSets.clear();
        tmemUploadResources.clear();
        replacementUploadResources.clear();
        uploadResourcePool.reset(nullptr);
    }
    
    void TextureCache::setRGBA32(Texture *dstTexture, RenderWorker *worker, const uint8_t *bytes, size_t byteCount, uint32_t width, uint32_t height, uint32_t rowPitch, std::unique_ptr<RenderBuffer> &dstUploadResource, RenderPool *uploadResourcePool, std::mutex *uploadResourcePoolMutex) {
        assert(dstTexture != nullptr);
        assert(worker != nullptr);
        assert(bytes != nullptr);
        assert(width > 0);
        assert(height > 0);

        dstTexture->format = RenderFormat::R8G8B8A8_UNORM;
        dstTexture->width = width;
        dstTexture->height = height;
        dstTexture->mipmaps = 1;

        // Calculate the minimum row width required to store the texture.
        uint32_t rowByteWidth, rowBytePadding;
        CalculateTextureRowWidthPadding(rowPitch, rowByteWidth, rowBytePadding);

        dstTexture->texture = worker->device->createTexture(RenderTextureDesc::Texture2D(width, height, 1, dstTexture->format));

        if (uploadResourcePool != nullptr) {
            assert(uploadResourcePoolMutex != nullptr);
            std::unique_lock queueLock(*uploadResourcePoolMutex);
            dstUploadResource = uploadResourcePool->createBuffer(RenderBufferDesc::UploadBuffer(rowByteWidth * height));
        }
        else {
            dstUploadResource = worker->device->createBuffer(RenderBufferDesc::UploadBuffer(rowByteWidth * height));
        }

        uint8_t *dstData = reinterpret_cast<uint8_t *>(dstUploadResource->map());
        if (rowBytePadding == 0) {
            memcpy(dstData, bytes, byteCount);
        }
        else {
            const uint8_t *srcData = reinterpret_cast<const uint8_t *>(bytes);
            size_t offset = 0;
            while ((offset + size_t(rowPitch)) <= byteCount) {
                memcpy(dstData, srcData, rowPitch);
                srcData += rowPitch;
                offset += rowPitch;
                dstData += rowByteWidth;
            }
        }

        dstUploadResource->unmap();

        uint32_t rowWidth = rowByteWidth / RenderFormatSize(dstTexture->format);
        worker->commandList->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(dstTexture->texture.get(), RenderTextureLayout::COPY_DEST));
        worker->commandList->copyTextureRegion(RenderTextureCopyLocation::Subresource(dstTexture->texture.get()), RenderTextureCopyLocation::PlacedFootprint(dstUploadResource.get(), dstTexture->format, width, height, 1, rowWidth));
        worker->commandList->barriers(RenderBarrierStage::COMPUTE, RenderTextureBarrier(dstTexture->texture.get(), RenderTextureLayout::SHADER_READ));
    }

    static RenderTextureDimension toRenderDimension(ddspp::TextureType type) {
        switch (type) {
        case ddspp::Texture1D:
            return RenderTextureDimension::TEXTURE_1D;
        case ddspp::Texture2D:
            return RenderTextureDimension::TEXTURE_2D;
        case ddspp::Texture3D:
            return RenderTextureDimension::TEXTURE_3D;
        default:
            assert(false && "Unknown texture type from DDS.");
            return RenderTextureDimension::UNKNOWN;
        }
    }

    static RenderFormat toRenderFormat(ddspp::DXGIFormat format) {
        switch (format) {
        case ddspp::R32G32B32A32_TYPELESS:
            return RenderFormat::R32G32B32A32_TYPELESS;
        case ddspp::R32G32B32A32_FLOAT:
            return RenderFormat::R32G32B32A32_FLOAT;
        case ddspp::R32G32B32A32_UINT:
            return RenderFormat::R32G32B32A32_UINT;
        case ddspp::R32G32B32A32_SINT:
            return RenderFormat::R32G32B32A32_SINT;
        case ddspp::R32G32B32_TYPELESS:
            return RenderFormat::R32G32B32_TYPELESS;
        case ddspp::R32G32B32_FLOAT:
            return RenderFormat::R32G32B32_FLOAT;
        case ddspp::R32G32B32_UINT:
            return RenderFormat::R32G32B32_UINT;
        case ddspp::R32G32B32_SINT:
            return RenderFormat::R32G32B32_SINT;
        case ddspp::R16G16B16A16_TYPELESS:
            return RenderFormat::R16G16B16A16_TYPELESS;
        case ddspp::R16G16B16A16_FLOAT:
            return RenderFormat::R16G16B16A16_FLOAT;
        case ddspp::R16G16B16A16_UNORM:
            return RenderFormat::R16G16B16A16_UNORM;
        case ddspp::R16G16B16A16_UINT:
            return RenderFormat::R16G16B16A16_UINT;
        case ddspp::R16G16B16A16_SNORM:
            return RenderFormat::R16G16B16A16_SNORM;
        case ddspp::R16G16B16A16_SINT:
            return RenderFormat::R16G16B16A16_SINT;
        case ddspp::R32G32_TYPELESS:
            return RenderFormat::R32G32_TYPELESS;
        case ddspp::R32G32_FLOAT:
            return RenderFormat::R32G32_FLOAT;
        case ddspp::R32G32_UINT:
            return RenderFormat::R32G32_UINT;
        case ddspp::R32G32_SINT:
            return RenderFormat::R32G32_SINT;
        case ddspp::R8G8B8A8_TYPELESS:
            return RenderFormat::R8G8B8A8_TYPELESS;
        case ddspp::R8G8B8A8_UNORM:
            return RenderFormat::R8G8B8A8_UNORM;
        case ddspp::R8G8B8A8_UINT:
            return RenderFormat::R8G8B8A8_UINT;
        case ddspp::R8G8B8A8_SNORM:
            return RenderFormat::R8G8B8A8_SNORM;
        case ddspp::R8G8B8A8_SINT:
            return RenderFormat::R8G8B8A8_SINT;
        case ddspp::B8G8R8A8_UNORM:
            return RenderFormat::B8G8R8A8_UNORM;
        case ddspp::R16G16_TYPELESS:
            return RenderFormat::R16G16_TYPELESS;
        case ddspp::R16G16_FLOAT:
            return RenderFormat::R16G16_FLOAT;
        case ddspp::R16G16_UNORM:
            return RenderFormat::R16G16_UNORM;
        case ddspp::R16G16_UINT:
            return RenderFormat::R16G16_UINT;
        case ddspp::R16G16_SNORM:
            return RenderFormat::R16G16_SNORM;
        case ddspp::R16G16_SINT:
            return RenderFormat::R16G16_SINT;
        case ddspp::R32_TYPELESS:
            return RenderFormat::R32_TYPELESS;
        case ddspp::D32_FLOAT:
            return RenderFormat::D32_FLOAT;
        case ddspp::R32_FLOAT:
            return RenderFormat::R32_FLOAT;
        case ddspp::R32_UINT:
            return RenderFormat::R32_UINT;
        case ddspp::R32_SINT:
            return RenderFormat::R32_SINT;
        case ddspp::R8G8_TYPELESS:
            return RenderFormat::R8G8_TYPELESS;
        case ddspp::R8G8_UNORM:
            return RenderFormat::R8G8_UNORM;
        case ddspp::R8G8_UINT:
            return RenderFormat::R8G8_UINT;
        case ddspp::R8G8_SNORM:
            return RenderFormat::R8G8_SNORM;
        case ddspp::R8G8_SINT:
            return RenderFormat::R8G8_SINT;
        case ddspp::R16_TYPELESS:
            return RenderFormat::R16_TYPELESS;
        case ddspp::R16_FLOAT:
            return RenderFormat::R16_FLOAT;
        case ddspp::D16_UNORM:
            return RenderFormat::D16_UNORM;
        case ddspp::R16_UNORM:
            return RenderFormat::R16_UNORM;
        case ddspp::R16_UINT:
            return RenderFormat::R16_UINT;
        case ddspp::R16_SNORM:
            return RenderFormat::R16_SNORM;
        case ddspp::R16_SINT:
            return RenderFormat::R16_SINT;
        case ddspp::R8_TYPELESS:
            return RenderFormat::R8_TYPELESS;
        case ddspp::R8_UNORM:
            return RenderFormat::R8_UNORM;
        case ddspp::R8_UINT:
            return RenderFormat::R8_UINT;
        case ddspp::R8_SNORM:
            return RenderFormat::R8_SNORM;
        case ddspp::R8_SINT:
            return RenderFormat::R8_SINT;
        case ddspp::BC1_TYPELESS:
            return RenderFormat::BC1_TYPELESS;
        case ddspp::BC1_UNORM:
            return RenderFormat::BC1_UNORM;
        case ddspp::BC1_UNORM_SRGB:
            return RenderFormat::BC1_UNORM_SRGB;
        case ddspp::BC2_TYPELESS:
            return RenderFormat::BC2_TYPELESS;
        case ddspp::BC2_UNORM:
            return RenderFormat::BC2_UNORM;
        case ddspp::BC2_UNORM_SRGB:
            return RenderFormat::BC2_UNORM_SRGB;
        case ddspp::BC3_TYPELESS:
            return RenderFormat::BC3_TYPELESS;
        case ddspp::BC3_UNORM:
            return RenderFormat::BC3_UNORM;
        case ddspp::BC3_UNORM_SRGB:
            return RenderFormat::BC3_UNORM_SRGB;
        case ddspp::BC4_TYPELESS:
            return RenderFormat::BC4_TYPELESS;
        case ddspp::BC4_UNORM:
            return RenderFormat::BC4_UNORM;
        case ddspp::BC4_SNORM:
            return RenderFormat::BC4_SNORM;
        case ddspp::BC5_TYPELESS:
            return RenderFormat::BC5_TYPELESS;
        case ddspp::BC5_UNORM:
            return RenderFormat::BC5_UNORM;
        case ddspp::BC5_SNORM:
            return RenderFormat::BC5_SNORM;
        case ddspp::BC6H_TYPELESS:
            return RenderFormat::BC6H_TYPELESS;
        case ddspp::BC6H_UF16:
            return RenderFormat::BC6H_UF16;
        case ddspp::BC6H_SF16:
            return RenderFormat::BC6H_SF16;
        case ddspp::BC7_TYPELESS:
            return RenderFormat::BC7_TYPELESS;
        case ddspp::BC7_UNORM:
            return RenderFormat::BC7_UNORM;
        case ddspp::BC7_UNORM_SRGB:
            return RenderFormat::BC7_UNORM_SRGB;
        default:
            assert(false && "Unsupported format from DDS.");
            return RenderFormat::UNKNOWN;
        }
    }

    bool TextureCache::setDDS(Texture *dstTexture, RenderWorker *worker, const uint8_t *bytes, size_t byteCount, std::unique_ptr<RenderBuffer> &dstUploadResource, RenderPool *uploadResourcePool, std::mutex *uploadResourcePoolMutex, uint32_t minMipWidth, uint32_t minMipHeight) {
        assert(dstTexture != nullptr);
        assert(worker != nullptr);
        assert(bytes != nullptr);

        ddspp::Descriptor ddsDescriptor;
        ddspp::Result result = ddspp::decode_header((unsigned char *)(bytes), ddsDescriptor);
        if (result != ddspp::Success) {
            return false;
        }

        RenderTextureDesc desc;
        desc.dimension = toRenderDimension(ddsDescriptor.type);
        desc.width = ddsDescriptor.width;
        desc.height = ddsDescriptor.height;
        desc.depth = 1;
        desc.mipLevels = 1;
        desc.format = toRenderFormat(ddsDescriptor.format);

        // Only load mipmaps as long as they're above a certain width and height.
        for (uint32_t mip = 1; mip < ddsDescriptor.numMips; mip++) {
            uint32_t mipWidth = std::max(desc.width >> mip, 1U);
            uint32_t mipHeight = std::max(desc.height >> mip, 1U);
            if ((mipWidth < minMipWidth) || (mipHeight < minMipHeight)) {
                break;
            }
            
            desc.mipLevels++;
        }

        dstTexture->texture = worker->device->createTexture(desc);
        dstTexture->width = ddsDescriptor.width;
        dstTexture->height = ddsDescriptor.height;
        dstTexture->mipmaps = desc.mipLevels;
        dstTexture->format = desc.format;

        const uint8_t *imageData = &bytes[ddsDescriptor.headerSize];
        size_t imageDataSize = byteCount - ddsDescriptor.headerSize;

        // Compute the additional padding that will be required on the buffer to align the mipmap data.
        std::vector<uint32_t> mipmapOffsets;
        uint32_t imageDataPadding = 0;
        const uint32_t imageDataAlignment = 16;
        for (uint32_t mip = 0; mip < desc.mipLevels; mip++) {
            uint32_t ddsOffset = ddspp::get_offset(ddsDescriptor, mip, 0);
            uint32_t alignedOffset = ddsOffset + imageDataPadding;
            if ((alignedOffset % imageDataAlignment) != 0) {
                imageDataPadding += imageDataAlignment - (alignedOffset % imageDataAlignment);
            }

            mipmapOffsets.emplace_back(ddsOffset + imageDataPadding);
        }

        const size_t uploadBufferSize = imageDataSize + imageDataPadding;
        if (uploadResourcePool != nullptr) {
            assert(uploadResourcePoolMutex != nullptr);
            std::unique_lock queueLock(*uploadResourcePoolMutex);
            dstUploadResource = uploadResourcePool->createBuffer(RenderBufferDesc::UploadBuffer(uploadBufferSize));
        }
        else {
            dstUploadResource = worker->device->createBuffer(RenderBufferDesc::UploadBuffer(uploadBufferSize));
        }

        // Copy each mipmap into the buffer with the correct padding applied.
        uint32_t mipmapOffset = 0;
        uint8_t *dstData = reinterpret_cast<uint8_t *>(dstUploadResource->map());
        memset(dstData, 0, uploadBufferSize);
        for (uint32_t mip = 0; mip < desc.mipLevels; mip++) {
            uint32_t ddsOffset = ddspp::get_offset(ddsDescriptor, mip, 0);
            uint32_t ddsSize = ((mip + 1) < ddsDescriptor.numMips) ? (ddspp::get_offset(ddsDescriptor, mip + 1, 0) - ddsOffset) : (imageDataSize - ddsOffset);
            uint32_t mipOffset = mipmapOffsets[mip];
            memcpy(&dstData[mipOffset], &imageData[ddsOffset], ddsSize);
        }

        dstUploadResource->unmap();
        worker->commandList->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(dstTexture->texture.get(), RenderTextureLayout::COPY_DEST));

        for (uint32_t mip = 0; mip < desc.mipLevels; mip++) {
            uint32_t offset = mipmapOffsets[mip];
            uint32_t mipWidth = std::max(desc.width >> mip, 1U);
            uint32_t mipHeight = std::max(desc.height >> mip, 1U);
            uint32_t rowWidth = mipWidth;
            worker->commandList->copyTextureRegion(RenderTextureCopyLocation::Subresource(dstTexture->texture.get(), mip), RenderTextureCopyLocation::PlacedFootprint(dstUploadResource.get(), desc.format, mipWidth, mipHeight, 1, rowWidth, offset));
        }

        worker->commandList->barriers(RenderBarrierStage::COMPUTE, RenderTextureBarrier(dstTexture->texture.get(), RenderTextureLayout::SHADER_READ));

        return true;
    }

    bool TextureCache::setLowMipCache(std::unordered_map<std::string, Texture *> &dstTextureMap, RenderWorker *worker, const uint8_t *bytes, size_t byteCount, std::unique_ptr<RenderBuffer> &dstUploadResource) {
        dstUploadResource = worker->device->createBuffer(RenderBufferDesc::UploadBuffer(byteCount));

        // Upload the entire file to the GPU to copy data from it directly.
        void *uploadData = dstUploadResource->map();
        memcpy(uploadData, bytes, byteCount);
        dstUploadResource->unmap();

        std::vector<RenderTextureBarrier> beforeCopyBarriers;
        std::vector<RenderTextureCopyLocation> copyDestinations;
        std::vector<RenderTextureCopyLocation> copySources;
        std::vector<RenderTextureBarrier> afterCopyBarriers;
        size_t byteCursor = 0;
        while (byteCursor < byteCount) {
            const ReplacementMipmapCacheHeader *cacheHeader = reinterpret_cast<const ReplacementMipmapCacheHeader *>(&bytes[byteCursor]);
            byteCursor += sizeof(ReplacementMipmapCacheHeader);

            if (cacheHeader->magic != ReplacementMipmapCacheHeaderMagic) {
                return false;
            }

            if (cacheHeader->version > ReplacementMipmapCacheHeaderVersion) {
                return false;
            }

            const uint32_t *mipSizes = reinterpret_cast<const uint32_t *>(&bytes[byteCursor]);
            byteCursor += cacheHeader->mipCount * sizeof(uint32_t);

            std::string cachePath(reinterpret_cast<const char *>(&bytes[byteCursor]), cacheHeader->pathLength);
            byteCursor += cacheHeader->pathLength;

            const size_t dataAlignment = 16;
            byteCursor += computePadding(byteCursor, dataAlignment);

            RenderFormat renderFormat = toRenderFormat(ddspp::DXGIFormat(cacheHeader->dxgiFormat));
            RenderTextureDesc textureDesc = RenderTextureDesc::Texture2D(cacheHeader->width, cacheHeader->height, cacheHeader->mipCount, renderFormat);
            Texture *newTexture = new Texture();
            newTexture->texture = worker->device->createTexture(textureDesc);
            newTexture->format = renderFormat;
            newTexture->width = cacheHeader->width;
            newTexture->height = cacheHeader->height;
            newTexture->mipmaps = cacheHeader->mipCount;

            for (uint32_t i = 0; i < cacheHeader->mipCount; i++) {
                uint32_t mipWidth = std::max(cacheHeader->width >> i, 1U);
                uint32_t mipHeight = std::max(cacheHeader->height >> i, 1U);
                uint32_t rowWidth = mipWidth;
                beforeCopyBarriers.emplace_back(RenderTextureBarrier(newTexture->texture.get(), RenderTextureLayout::COPY_DEST));
                copyDestinations.emplace_back(RenderTextureCopyLocation::Subresource(newTexture->texture.get(), i));
                copySources.emplace_back(RenderTextureCopyLocation::PlacedFootprint(dstUploadResource.get(), renderFormat, mipWidth, mipHeight, 1, rowWidth, byteCursor));
                afterCopyBarriers.emplace_back(RenderTextureBarrier(newTexture->texture.get(), RenderTextureLayout::SHADER_READ));
                byteCursor += mipSizes[i];
            }

            dstTextureMap[cachePath] = newTexture;
        }

        // Execute all texture copies together.
        {
            RenderWorkerExecution execution(worker);
            worker->commandList->barriers(RenderBarrierStage::COPY, beforeCopyBarriers);

            for (size_t i = 0; i < copyDestinations.size(); i++) {
                worker->commandList->copyTextureRegion(copyDestinations[i], copySources[i]);
            }

            worker->commandList->barriers(RenderBarrierStage::COMPUTE, afterCopyBarriers);
        }

        return true;
    }

    bool TextureCache::loadBytesFromPath(const std::filesystem::path &path, std::vector<uint8_t> &bytes) {
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

    Texture *TextureCache::loadTextureFromBytes(RenderWorker *worker, const std::vector<uint8_t> &fileBytes, std::unique_ptr<RenderBuffer> &dstUploadResource, RenderPool *resourcePool, std::mutex *uploadResourcePoolMutex, uint32_t minMipWidth, uint32_t minMipHeight) {
        const uint32_t PNG_MAGIC = 0x474E5089;
        Texture *replacementTexture = new Texture();
        uint32_t magicNumber = *reinterpret_cast<const uint32_t *>(fileBytes.data());
        bool loadedTexture = false;
        switch (magicNumber) {
        case ddspp::DDS_MAGIC:
            loadedTexture = TextureCache::setDDS(replacementTexture, worker, fileBytes.data(), fileBytes.size(), dstUploadResource, resourcePool, uploadResourcePoolMutex, minMipWidth, minMipHeight);
            break;
        case PNG_MAGIC: {
            int width, height;
            stbi_uc *data = stbi_load_from_memory(fileBytes.data(), fileBytes.size(), &width, &height, nullptr, 4);
            if (data != nullptr) {
                uint32_t rowPitch = uint32_t(width) * 4;
                size_t byteCount = uint32_t(height) * rowPitch;
                TextureCache::setRGBA32(replacementTexture, worker, data, byteCount, uint32_t(width), uint32_t(height), rowPitch, dstUploadResource, resourcePool);
                stbi_image_free(data);
                loadedTexture = true;
            }

            break;
        }
        default:
            // Unknown format.
            break;
        }

        if (loadedTexture) {
            return replacementTexture;
        }
        else {
            delete replacementTexture;
            return nullptr;
        }
    }

    void TextureCache::uploadThreadLoop() {
        Thread::setCurrentThreadName("RT64 Texture");

        uploadThreadRunning = true;

        std::vector<TextureUpload> queueCopy;
        std::vector<TextureUpload> newQueue;
        std::vector<ReplacementCheck> replacementQueueCopy;
        std::vector<HashTexturePair> texturesUploaded;
        std::vector<HashTexturePair> texturesReplaced;
        std::vector<RenderTextureBarrier> beforeCopyBarriers;
        std::vector<RenderTextureBarrier> beforeDecodeBarriers;
        std::vector<RenderTextureBarrier> afterDecodeBarriers;
        std::vector<uint8_t> replacementBytes;

        while (uploadThreadRunning) {
            replacementQueueCopy.clear();

            // Check the top of the queue or wait if it's empty.
            {
                std::unique_lock queueLock(uploadQueueMutex);
                uploadQueueChanged.wait(queueLock, [this]() {
                    return !uploadThreadRunning || !uploadQueue.empty() || !replacementQueue.empty();
                });

                if (!uploadQueue.empty()) {
                    queueCopy = uploadQueue;
                }

                if (!replacementQueue.empty()) {
                    replacementQueueCopy.insert(replacementQueueCopy.end(), replacementQueue.begin(), replacementQueue.end());
                    replacementQueue.clear();
                }
            }

            if (!queueCopy.empty() || !replacementQueueCopy.empty()) {
                // Create new upload buffers and descriptor heaps to fill out the required size.
                const size_t queueSize = queueCopy.size();
                const uint64_t TMEMSize = 0x1000;
                {
                    std::unique_lock queueLock(uploadResourcePoolMutex);
                    for (size_t i = tmemUploadResources.size(); i < queueSize; i++) {
                        tmemUploadResources.emplace_back(uploadResourcePool->createBuffer(RenderBufferDesc::UploadBuffer(TMEMSize)));
                    }
                }

                for (size_t i = descriptorSets.size(); i < queueSize; i++) {
                    descriptorSets.emplace_back(std::make_unique<TextureDecodeDescriptorSet>(worker->device));
                }

                // Upload all textures in the queue.
                {
                    RenderWorkerExecution execution(worker);
                    texturesUploaded.clear();
                    beforeCopyBarriers.clear();
                    for (size_t i = 0; i < queueSize; i++) {
                        static uint32_t TMEMGlobalCounter = 0;
                        const TextureUpload &upload = queueCopy[i];
                        Texture *newTexture = new Texture();
                        newTexture->creationFrame = upload.creationFrame;
                        texturesUploaded.emplace_back(HashTexturePair{ upload.hash, newTexture, false });

                        if (developerMode) {
                            newTexture->bytesTMEM = upload.bytesTMEM;
                        }

                        newTexture->format = RenderFormat::R8_UINT;
                        newTexture->width = upload.width;
                        newTexture->height = upload.height;
                        newTexture->tmem = worker->device->createTexture(RenderTextureDesc::Texture1D(uint32_t(upload.bytesTMEM.size()), 1, newTexture->format));
                        newTexture->tmem->setName("Texture Cache TMEM #" + std::to_string(TMEMGlobalCounter++));

                        void *dstData = tmemUploadResources[i]->map();
                        memcpy(dstData, upload.bytesTMEM.data(), upload.bytesTMEM.size());
                        tmemUploadResources[i]->unmap();

                        beforeCopyBarriers.emplace_back(RenderTextureBarrier(newTexture->tmem.get(), RenderTextureLayout::COPY_DEST));
                    }

                    worker->commandList->barriers(RenderBarrierStage::COPY, beforeCopyBarriers);

                    beforeDecodeBarriers.clear();
                    for (size_t i = 0; i < queueSize; i++) {
                        const TextureUpload &upload = queueCopy[i];
                        const uint32_t byteCount = uint32_t(upload.bytesTMEM.size());
                        Texture *dstTexture = texturesUploaded[i].texture;
                        worker->commandList->copyTextureRegion(
                            RenderTextureCopyLocation::Subresource(dstTexture->tmem.get()),
                            RenderTextureCopyLocation::PlacedFootprint(tmemUploadResources[i].get(), RenderFormat::R8_UINT, byteCount, 1, 1, byteCount)
                        );

                        beforeDecodeBarriers.emplace_back(RenderTextureBarrier(dstTexture->tmem.get(), RenderTextureLayout::SHADER_READ));

                        if (upload.decodeTMEM) {
                            static uint32_t TextureGlobalCounter = 0;
                            TextureDecodeDescriptorSet *descSet = descriptorSets[i].get();
                            dstTexture->format = RenderFormat::R8G8B8A8_UNORM;
                            dstTexture->texture = worker->device->createTexture(RenderTextureDesc::Texture2D(upload.width, upload.height, 1, dstTexture->format, RenderTextureFlag::STORAGE | RenderTextureFlag::UNORDERED_ACCESS));
                            dstTexture->texture->setName("Texture Cache RGBA32 #" + std::to_string(TextureGlobalCounter++));
                            descSet->setTexture(descSet->TMEM, dstTexture->tmem.get(), RenderTextureLayout::SHADER_READ);
                            descSet->setTexture(descSet->RGBA32, dstTexture->texture.get(), RenderTextureLayout::GENERAL);
                            beforeDecodeBarriers.emplace_back(RenderTextureBarrier(dstTexture->texture.get(), RenderTextureLayout::GENERAL));
                        }
                    }

                    worker->commandList->barriers(RenderBarrierStage::COMPUTE, beforeDecodeBarriers);

                    const ShaderRecord &textureDecode = shaderLibrary->textureDecode;
                    bool pipelineSet = false;
                    afterDecodeBarriers.clear();
                    for (size_t i = 0; i < queueSize; i++) {
                        const TextureUpload &upload = queueCopy[i];
                        if (upload.decodeTMEM) {
                            if (!pipelineSet) {
                                worker->commandList->setPipeline(textureDecode.pipeline.get());
                                worker->commandList->setComputePipelineLayout(textureDecode.pipelineLayout.get());
                            }

                            interop::TextureDecodeCB decodeCB;
                            decodeCB.Resolution.x = upload.width;
                            decodeCB.Resolution.y = upload.height;
                            decodeCB.fmt = upload.loadTile.fmt;
                            decodeCB.siz = upload.loadTile.siz;
                            decodeCB.address = interop::uint(upload.loadTile.tmem) << 3;
                            decodeCB.stride = interop::uint(upload.loadTile.line) << 3;
                            decodeCB.tlut = upload.tlut;
                            decodeCB.palette = upload.loadTile.palette;

                            // Dispatch compute shader for decoding texture.
                            const uint32_t ThreadGroupSize = 8;
                            const uint32_t dispatchX = (decodeCB.Resolution.x + ThreadGroupSize - 1) / ThreadGroupSize;
                            const uint32_t dispatchY = (decodeCB.Resolution.y + ThreadGroupSize - 1) / ThreadGroupSize;
                            worker->commandList->setComputePushConstants(0, &decodeCB);
                            worker->commandList->setComputeDescriptorSet(descriptorSets[i]->get(), 0);
                            worker->commandList->dispatch(dispatchX, dispatchY, 1);

                            afterDecodeBarriers.emplace_back(RenderTextureBarrier(texturesUploaded[i].texture->texture.get(), RenderTextureLayout::SHADER_READ));
                        }

                        if ((upload.width > 0) && (upload.height > 0)) {
                            // If the database uses an older hash version, we hash TMEM again with the version corresponding to the database.
                            uint32_t databaseVersion = textureMap.replacementMap.db.config.hashVersion;
                            uint64_t databaseHash = upload.hash;
                            if (databaseVersion < TMEMHasher::CurrentHashVersion) {
                                databaseHash = TMEMHasher::hash(upload.bytesTMEM.data(), upload.loadTile, upload.width, upload.height, upload.tlut, databaseVersion);
                            }

                            // Add this hash so it's checked for a replacement.
                            replacementQueueCopy.emplace_back(ReplacementCheck{ upload.hash, databaseHash, uint32_t(upload.width), uint32_t(upload.height) });
                        }
                    }

                    if (!afterDecodeBarriers.empty()) {
                        worker->commandList->barriers(RenderBarrierStage::COMPUTE, afterDecodeBarriers);
                    }

                    texturesReplaced.clear();

                    for (const ReplacementCheck &replacementCheck : replacementQueueCopy) {
                        std::string relativePath;
                        uint32_t databaseIndex = 0;
                        if (textureMap.replacementMap.getInformationFromHash(replacementCheck.databaseHash, relativePath, databaseIndex)) {
                            const ReplacementTexture &databaseTexture = textureMap.replacementMap.db.textures[databaseIndex];
                            Texture *replacementTexture = nullptr;
                            Texture *lowMipCacheTexture = nullptr;
                            {
                                std::unique_lock replacementMapLock(textureMap.replacementMapMutex);
                                replacementTexture = textureMap.replacementMap.getFromRelativePath(relativePath);
                                
                                // Look for the low mip cache version if it exists if we can't use the real replacement yet.
                                if ((replacementTexture == nullptr) && (databaseTexture.load == ReplacementLoad::Stream)) {
                                    auto lowMipCacheIt = textureMap.replacementMap.lowMipCacheTextures.find(relativePath);
                                    if (lowMipCacheIt != textureMap.replacementMap.lowMipCacheTextures.end()) {
                                        lowMipCacheTexture = lowMipCacheIt->second;
                                    }
                                }
                            }

                            // Replacement texture hasn't been loaded yet.
                            if (replacementTexture == nullptr) {
                                std::filesystem::path filePath = textureMap.replacementMap.directoryPath / std::filesystem::u8path(relativePath);

                                // Queue the texture for being loaded from a texture cache streaming thread.
                                if ((databaseTexture.load == ReplacementLoad::Stream) || (databaseTexture.load == ReplacementLoad::Async)) {
                                    {
                                        std::unique_lock queueLock(streamDescQueueMutex);
                                        streamDescQueue.push(StreamDescription(replacementCheck.textureHash, filePath, relativePath, replacementCheck.minMipWidth, replacementCheck.minMipHeight));
                                    }

                                    streamDescQueueChanged.notify_all();

                                    // Use the low mip cache texture if it exists.
                                    replacementTexture = lowMipCacheTexture;
                                }
                                // Load the texture directly on this thread.
                                else if (TextureCache::loadBytesFromPath(filePath, replacementBytes)) {
                                    replacementUploadResources.emplace_back();
                                    replacementTexture = TextureCache::loadTextureFromBytes(worker, replacementBytes, replacementUploadResources.back(), nullptr, nullptr, replacementCheck.minMipWidth, replacementCheck.minMipHeight);

                                    {
                                        std::unique_lock replacementMapLock(textureMap.replacementMapMutex);
                                        textureMap.replacementMap.addLoadedTexture(replacementTexture, relativePath);
                                    }
                                }
                            }

                            if (replacementTexture != nullptr) {
                                bool lowPriorityReplacement = (replacementTexture == lowMipCacheTexture);
                                texturesReplaced.emplace_back(HashTexturePair{ replacementCheck.textureHash, replacementTexture, lowPriorityReplacement });
                            }
                        }
                    }
                }

                replacementUploadResources.clear();

                // Add all the textures to the map once they're ready.
                {
                    std::unique_lock lock(textureMapMutex);
                    for (const HashTexturePair &pair : texturesUploaded) {
                        textureMap.add(pair.hash, pair.texture->creationFrame, pair.texture);
                    }
                    
                    for (const HashTexturePair &pair : texturesReplaced) {
                        textureMap.replace(pair.hash, pair.texture, pair.lowPriorityReplacement);
                    }
                }

                // Make the new queue the remaining subsection of the upload queue that wasn't processed in this batch.
                {
                    std::unique_lock queueLock(uploadQueueMutex);
                    newQueue = std::vector<TextureUpload>(uploadQueue.begin() + queueSize, uploadQueue.end());
                    uploadQueue = std::move(newQueue);
                }

                queueCopy.clear();
                uploadQueueFinished.notify_all();
            }
        }
    }

    void TextureCache::queueGPUUploadTMEM(uint64_t hash, uint64_t creationFrame, const uint8_t *bytes, int bytesCount, int width, int height, uint32_t tlut, const LoadTile &loadTile, bool decodeTMEM) {
        assert(bytes != nullptr);
        assert(bytesCount > 0);
        assert(!decodeTMEM || ((width > 0) && (height > 0)));

        TextureUpload newUpload;
        newUpload.hash = hash;
        newUpload.creationFrame = creationFrame;
        newUpload.width = width;
        newUpload.height = height;
        newUpload.tlut = tlut;
        newUpload.loadTile = loadTile;
        newUpload.bytesTMEM = std::vector<uint8_t>(bytes, bytes + bytesCount);
        newUpload.decodeTMEM = decodeTMEM;

        {
            std::unique_lock queueLock(uploadQueueMutex);
            uploadQueue.emplace_back(newUpload);
        }

        uploadQueueChanged.notify_all();
    }

    void TextureCache::waitForGPUUploads() {
        std::unique_lock queueLock(uploadQueueMutex);
        uploadQueueFinished.wait(queueLock, [this]() {
            return uploadQueue.empty();
        });
    }

    bool TextureCache::useTexture(uint64_t hash, uint64_t submissionFrame, uint32_t &textureIndex, interop::float2 &textureScale, bool &textureReplaced, bool &hasMipmaps) {
        std::unique_lock lock(textureMapMutex);
        return textureMap.use(hash, submissionFrame, textureIndex, textureScale, textureReplaced, hasMipmaps);
    }

    bool TextureCache::useTexture(uint64_t hash, uint64_t submissionFrame, uint32_t &textureIndex) {
        interop::float2 textureScale;
        bool textureReplaced;
        bool hasMipmaps;
        return useTexture(hash, submissionFrame, textureIndex, textureScale, textureReplaced, hasMipmaps);
    }
    
    bool TextureCache::addReplacement(uint64_t hash, const std::string &relativePath) {
        // TODO: The case where a replacement is reloaded needs to be handled correctly. Multiple hashes can point to the same path. All hashes pointing to that path must be reloaded correctly.

        std::unique_lock lock(textureMapMutex);
        std::vector<uint8_t> replacementBytes;
        if (!TextureCache::loadBytesFromPath(textureMap.replacementMap.directoryPath / std::filesystem::u8path(relativePath), replacementBytes)) {
            return false;
        }

        // Load texture replacement immediately.
        std::unique_ptr<RenderBuffer> dstUploadBuffer;
        Texture *newTexture = nullptr;
        {
            RenderWorkerExecution execution(worker);
            newTexture = TextureCache::loadTextureFromBytes(worker, replacementBytes, dstUploadBuffer);
        }

        // Add the loaded texture to the replacement map.
        if (newTexture != nullptr) {
            std::unique_lock replacementMapLock(textureMap.replacementMapMutex);
            textureMap.replacementMap.addLoadedTexture(newTexture, relativePath);
        }
        else {
            return false;
        }

        // Store replacement in the replacement database.
        ReplacementTexture replacement;
        replacement.hashes.rt64 = ReplacementDatabase::hashToString(hash);
        replacement.path = ReplacementDatabase::removeKnownExtension(relativePath);

        // Add the replacement's index to the resolved path map as well.
        uint32_t databaseIndex = textureMap.replacementMap.db.addReplacement(replacement);
        textureMap.replacementMap.resolvedPathMap[hash] = { relativePath, databaseIndex };

        // Replace the texture in the cache.
        textureMap.replace(hash, newTexture, false);
        return true;
    }
    
    bool TextureCache::loadReplacementDirectory(const std::filesystem::path &directoryPath) {
        // Wait for the streaming threads to be finished.
        waitForAllStreamThreads();
        
        // Clear the current queue of streamed textures.
        streamedTextureQueueMutex.lock();
        streamedTextureQueue = std::queue<HashTexturePair>();
        streamedTextureQueueMutex.unlock();
        
        // Lock the texture map and start changing replacements. This function is assumed to be called from the only
        // thread that is capable of submitting new textures and must've waited beforehand for all textures to be uploaded.
        std::unique_lock lock(textureMapMutex);
        textureMap.clearReplacements();
        textureMap.replacementMap.clear(textureMap.evictedTextures);
        textureMap.replacementMap.directoryPath = directoryPath;

        std::ifstream databaseFile(directoryPath / ReplacementDatabaseFilename);
        if (databaseFile.is_open()) {
            textureMap.replacementMap.readDatabase(databaseFile);
        }
        else {
            textureMap.replacementMap.db = ReplacementDatabase();
        }

        textureMap.replacementMap.db.resolvePaths(directoryPath, textureMap.replacementMap.resolvedPathMap, false);
        
        // Preload the low mip cache if it exists.
        std::vector<uint8_t> mipCacheBytes;
        if (loadBytesFromPath(directoryPath / ReplacementLowMipCacheFilename, mipCacheBytes)) {
            std::unique_lock replacementMapLock(textureMap.replacementMapMutex);
            std::unique_ptr<RenderBuffer> uploadBuffer;
            if (!setLowMipCache(textureMap.replacementMap.lowMipCacheTextures, worker, mipCacheBytes.data(), mipCacheBytes.size(), uploadBuffer)) {
                // Delete the textures that were loaded into the low mip cache.
                for (auto it : textureMap.replacementMap.lowMipCacheTextures) {
                    delete it.second;
                }

                textureMap.replacementMap.lowMipCacheTextures.clear();
                fprintf(stderr, "Failed to load low mip cache.\n");
            }
        }

        // Queue all currently loaded hashes to detect replacements with.
        {
            std::unique_lock queueLock(uploadQueueMutex);
            replacementQueue.clear();
            for (size_t i = 0; i < textureMap.hashes.size(); i++) {
                if (textureMap.hashes[i] != 0) {
                    uint32_t minMipWidth = 0;
                    uint32_t minMipHeight = 0;
                    if (textureMap.textures[i] != nullptr) {
                        minMipWidth = textureMap.textures[i]->width;
                        minMipHeight = textureMap.textures[i]->height;
                    }

                    replacementQueue.emplace_back(ReplacementCheck{ textureMap.hashes[i], textureMap.hashes[i], minMipWidth, minMipHeight });
                }
            }
        }

        uploadQueueChanged.notify_all();

        return true;
    }

    bool TextureCache::saveReplacementDatabase() {
        std::unique_lock lock(textureMapMutex);
        if (textureMap.replacementMap.directoryPath.empty()) {
            return false;
        }

        const std::filesystem::path databasePath = textureMap.replacementMap.directoryPath / ReplacementDatabaseFilename;
        const std::filesystem::path databaseNewPath = textureMap.replacementMap.directoryPath / (ReplacementDatabaseFilename + ".new");
        const std::filesystem::path databaseOldPath = textureMap.replacementMap.directoryPath / (ReplacementDatabaseFilename + ".old");
        std::ofstream databaseNewFile(databaseNewPath);
        if (!textureMap.replacementMap.saveDatabase(databaseNewFile)) {
            return false;
        }

        databaseNewFile.close();

        std::error_code ec;
        if (std::filesystem::exists(databasePath)) {
            if (std::filesystem::exists(databaseOldPath)) {
                std::filesystem::remove(databaseOldPath, ec);
                if (ec) {
                    fprintf(stderr, "%s\n", ec.message().c_str());
                    return false;
                }
            }

            std::filesystem::rename(databasePath, databaseOldPath, ec);
            if (ec) {
                fprintf(stderr, "%s\n", ec.message().c_str());
                return false;
            }
        }

        std::filesystem::rename(databaseNewPath, databasePath, ec);
        if (ec) {
            fprintf(stderr, "%s\n", ec.message().c_str());
            return false;
        }

        return true;
    }

    void TextureCache::removeUnusedEntriesFromDatabase() {
        std::unique_lock lock(textureMapMutex);
        if (textureMap.replacementMap.directoryPath.empty()) {
            return;
        }

        textureMap.replacementMap.removeUnusedEntriesFromDatabase();
    }

    Texture *TextureCache::getTexture(uint32_t textureIndex) {
        std::unique_lock lock(textureMapMutex);
        return textureMap.get(textureIndex);
    }

    bool TextureCache::evict(uint64_t submissionFrame, std::vector<uint64_t> &evictedHashes) {
        std::unique_lock lock(textureMapMutex);
        return textureMap.evict(submissionFrame, evictedHashes);
    }

    void TextureCache::incrementLock() {
        std::unique_lock lock(textureMapMutex);
        lockCounter++;
    }

    void TextureCache::decrementLock() {
        std::unique_lock lock(textureMapMutex);
        lockCounter--;

        if (lockCounter == 0) {
            // Delete evicted textures from texture map.
            for (Texture *texture : textureMap.evictedTextures) {
                delete texture;
            }

            textureMap.evictedTextures.clear();

            // Add any replacements loaded by the streaming threads.
            {
                std::unique_lock streamedLock(streamedTextureQueueMutex);
                HashTexturePair hashTexturePair;
                while (!streamedTextureQueue.empty()) {
                    hashTexturePair = streamedTextureQueue.front();
                    textureMap.replace(hashTexturePair.hash, hashTexturePair.texture, false);
                    streamedTextureQueue.pop();
                }
            }
        }
    }

    void TextureCache::waitForAllStreamThreads() {
        {
            std::unique_lock<std::mutex> queueLock(streamDescQueueMutex);
            streamDescQueue = std::queue<StreamDescription>();
        }

        bool keepWaiting = false;
        do {
            std::unique_lock<std::mutex> queueLock(streamDescQueueMutex);
            keepWaiting = (streamDescQueueActiveCount > 0);
        } while (keepWaiting);
    }
};