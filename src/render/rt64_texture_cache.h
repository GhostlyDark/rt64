//
// RT64
//

#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

#include <json/json.hpp>

#include "common/rt64_replacement_database.h"
#include "hle/rt64_draw_call.h"

#include "rt64_descriptor_sets.h"
#include "rt64_render_worker.h"
#include "rt64_shader_library.h"
#include "rt64_texture.h"

namespace interop {
    struct alignas(16) TextureDecodeCB {
        uint2 Resolution;
        uint fmt;
        uint siz;
        uint address;
        uint stride;
        uint tlut;
        uint palette;
    };
};

namespace RT64 {
    struct TextureUpload {
        uint64_t hash;
        uint64_t creationFrame;
        uint32_t width;
        uint32_t height;
        uint32_t tlut;
        LoadTile loadTile;
        std::vector<uint8_t> bytesTMEM;
        bool decodeTMEM;
    };

    struct LowMipCacheTexture {
        Texture *texture = nullptr;
        bool transitioned = false;
    };

    typedef std::pair<uint32_t, uint64_t> AccessPair;
    typedef std::list<AccessPair> AccessList;

    struct ReplacementMap {
        ReplacementDatabase db;
        std::vector<Texture *> loadedTextures;
        std::unordered_map<uint64_t, uint32_t> pathHashToLoadMap;
        std::unordered_map<uint64_t, ReplacementResolvedPath> resolvedPathMap;
        std::unordered_map<std::string, LowMipCacheTexture> lowMipCacheTextures;
        std::filesystem::path directoryPath;

        ReplacementMap();
        ~ReplacementMap();
        void clear(std::vector<Texture *> &evictedTextures);
        bool readDatabase(std::istream &stream);
        bool saveDatabase(std::ostream &stream);
        void removeUnusedEntriesFromDatabase();
        bool getInformationFromHash(uint64_t tmemHash, std::string &relativePath, uint32_t &databaseIndex) const;
        void addLoadedTexture(Texture *texture, const std::string &relativePath);
        Texture *getFromRelativePath(const std::string &relativePath) const;
        uint64_t hashFromRelativePath(const std::string &relativePath) const;
    };

    struct ReplacementCheck {
        uint64_t textureHash = 0;
        uint64_t databaseHash = 0;
    };

    struct HashTexturePair {
        uint64_t hash = 0;
        Texture *texture = nullptr;
        bool lowPriorityReplacement = false;
    };

    struct TextureMap {
        std::unordered_map<uint64_t, uint32_t> hashMap;
        std::vector<Texture *> textures;
        std::vector<Texture *> textureReplacements;
        std::vector<interop::float2> textureScales;
        std::vector<uint64_t> hashes;
        std::vector<uint32_t> freeSpaces;
        std::vector<uint32_t> versions;
        std::vector<uint64_t> creationFrames;
        uint32_t globalVersion;
        AccessList accessList;
        std::vector<AccessList::iterator> listIterators;
        std::vector<Texture *> evictedTextures;
        ReplacementMap replacementMap;
        bool replacementMapEnabled;

        TextureMap();
        ~TextureMap();
        void clearReplacements();
        void add(uint64_t hash, uint64_t creationFrame, Texture *texture);
        void replace(uint64_t hash, Texture *texture, bool ignoreIfFull);
        bool use(uint64_t hash, uint64_t submissionFrame, uint32_t &textureIndex, interop::float2 &textureScale, bool &textureReplaced, bool &hasMipmaps);
        bool evict(uint64_t submissionFrame, std::vector<uint64_t> &evictedHashes);
        void incrementLock();
        void decrementLock();
        Texture *get(uint32_t index) const;
        size_t getMaxIndex() const;
    };

    struct TextureCache {
        struct StreamDescription {
            std::filesystem::path filePath;
            std::string relativePath;

            StreamDescription() {
                // Default constructor.
            }

            StreamDescription(const std::filesystem::path &filePath, const std::string &relativePath) {
                this->filePath = filePath;
                this->relativePath = relativePath;
            }
        };

        struct StreamResult {
            Texture *texture = nullptr;
            std::string relativePath;

            StreamResult() {
                // Default constructor.
            }

            StreamResult(Texture *texture, const std::string &relativePath) {
                this->texture = texture;
                this->relativePath = relativePath;
            }
        };

        struct StreamThread {
            std::unique_ptr<RenderWorker> worker;
            TextureCache *textureCache = nullptr;
            std::unique_ptr<std::thread> thread;
            std::atomic<bool> threadRunning;
            std::unique_ptr<RenderBuffer> uploadResource;

            StreamThread(TextureCache *textureCache);
            ~StreamThread();
            void loop();
        };

        const ShaderLibrary *shaderLibrary;
        std::vector<TextureUpload> uploadQueue;
        std::vector<ReplacementCheck> replacementQueue;
        std::vector<StreamResult> streamResultQueue;
        std::vector<std::unique_ptr<RenderBuffer>> tmemUploadResources;
        std::vector<std::unique_ptr<RenderBuffer>> replacementUploadResources;
        std::vector<std::unique_ptr<TextureDecodeDescriptorSet>> descriptorSets;
        std::mutex uploadQueueMutex;
        std::condition_variable uploadQueueChanged;
        std::condition_variable uploadQueueFinished;
        std::thread *uploadThread;
        std::atomic<bool> uploadThreadRunning;
        std::queue<StreamDescription> streamDescQueue;
        std::mutex streamDescQueueMutex;
        std::condition_variable streamDescQueueChanged;
        int32_t streamDescQueueActiveCount = 0;
        std::unordered_set<std::string> streamDescQueueSet;
        std::list<std::unique_ptr<StreamThread>> streamThreads;
        std::unordered_multimap<std::string, ReplacementCheck> streamPendingReplacementChecks;
        TextureMap textureMap;
        std::mutex textureMapMutex;
        RenderWorker *directWorker;
        RenderWorker *copyWorker;
        std::unique_ptr<RenderCommandList> loaderCommandList;
        std::unique_ptr<RenderCommandFence> loaderCommandFence;
        std::unique_ptr<RenderCommandSemaphore> copyToDirectSemaphore;
        std::unique_ptr<RenderPool> uploadResourcePool;
        std::mutex uploadResourcePoolMutex;
        uint32_t lockCounter;
        bool developerMode;

        TextureCache(RenderWorker *directWorker, RenderWorker *copyWorker, uint32_t threadCount, const ShaderLibrary *shaderLibrary, bool developerMode);
        ~TextureCache();
        void uploadThreadLoop();
        void queueGPUUploadTMEM(uint64_t hash, uint64_t creationFrame, const uint8_t *bytes, int bytesCount, int width, int height, uint32_t tlut, const LoadTile &loadTile, bool decodeTMEM);
        void waitForGPUUploads();
        bool useTexture(uint64_t hash, uint64_t submissionFrame, uint32_t &textureIndex, interop::float2 &textureScale, bool &textureReplaced, bool &hasMipmaps);
        bool useTexture(uint64_t hash, uint64_t submissionFrame, uint32_t &textureIndex);
        bool addReplacement(uint64_t hash, const std::string &relativePath);
        bool loadReplacementDirectory(const std::filesystem::path &directoryPath);
        bool saveReplacementDatabase();
        void removeUnusedEntriesFromDatabase();
        bool evict(uint64_t submissionFrame, std::vector<uint64_t> &evictedHashes);
        void incrementLock();
        void decrementLock();
        void waitForAllStreamThreads();
        Texture *getTexture(uint32_t textureIndex);
        static void setRGBA32(Texture *dstTexture, RenderDevice *device, RenderCommandList *commandList, const uint8_t *bytes, size_t byteCount, uint32_t width, uint32_t height, uint32_t rowPitch, std::unique_ptr<RenderBuffer> &dstUploadResource, RenderPool *uploadResourcePool = nullptr, std::mutex *uploadResourcePoolMutex = nullptr);
        static bool setDDS(Texture *dstTexture, RenderDevice *device, RenderCommandList *commandList, const uint8_t *bytes, size_t byteCount, std::unique_ptr<RenderBuffer> &dstUploadResource, RenderPool *uploadResourcePool = nullptr, std::mutex *uploadResourcePoolMutex = nullptr);
        static bool setLowMipCache(RenderDevice *device, RenderCommandList *commandList, const uint8_t *bytes, size_t byteCount, std::unique_ptr<RenderBuffer> &dstUploadResource, std::unordered_map<std::string, LowMipCacheTexture> &dstTextureMap);
        static bool loadBytesFromPath(const std::filesystem::path &path, std::vector<uint8_t> &bytes);
        static Texture *loadTextureFromBytes(RenderDevice *device, RenderCommandList *commandList, const std::vector<uint8_t> &fileBytes, std::unique_ptr<RenderBuffer> &dstUploadResource, RenderPool *resourcePool = nullptr, std::mutex *uploadResourcePoolMutex = nullptr);
    };
};