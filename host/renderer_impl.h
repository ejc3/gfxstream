// Copyright (C) 2016 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gfxstream/host/mem_stream.h"
#include "mirror_tap.h"
#include "render_thread.h"
#include "render_window.h"
#include "gfxstream/host/features.h"
#include "render-utils/Renderer.h"

namespace android_studio {
    class EmulatorGLESUsages;
}

namespace gfxstream {
namespace host {

class MemoryTextureStore;

class RendererImpl final : public Renderer {
public:
    RendererImpl();
    ~RendererImpl();

    bool initialize(int width, int height, const gfxstream::host::FeatureSet& features, bool useSubWindow);
    void stop(bool wait) override;
    void finish() override;

public:
    RenderChannelPtr createRenderChannel(
        gfxstream::Stream* loadStream, uint32_t virtioGpuContextId) final;

    void* addressSpaceGraphicsConsumerCreate(
        const AsgConsumerCreateInfo& info, gfxstream::Stream* stream) override final;
    void addressSpaceGraphicsConsumerDestroy(void*) override final;
    void addressSpaceGraphicsConsumerPreSave(void* consumer) override final;
    void addressSpaceGraphicsConsumerSave(void* consumer, gfxstream::Stream* stream) override final;
    void addressSpaceGraphicsConsumerPostSave(void* consumer) override final;
    void addressSpaceGraphicsConsumerRegisterPostLoadRenderThread(void* consumer) override final;
    void addressSpaceGraphicsConsumerReloadRingConfig(void* consumer) override final;

    HardwareStrings getHardwareStrings() final;

    bool getVulkanEmulationDeviceInfo(char** device_name, char** driver_info,
                                      uint32_t* driver_version, uint32_t* api_version,
                                      uint32_t* vendor_id, uint32_t* device_id,
                                      uint32_t* device_type, uint64_t* device_memory) final;

    void setPostCallback(OnPostCallback onPost,
                         void* context,
                         bool useBgraReadback,
                         uint32_t displayId) final;
    bool asyncReadbackSupported() final;
    ReadPixelsCallback getReadPixelsCallback() final;
    FlushReadPixelPipeline getFlushReadPixelPipeline() final;
    bool showOpenGLSubwindow(FBNativeWindowType window,
                             int wx,
                             int wy,
                             int ww,
                             int wh,
                             int fbw,
                             int fbh,
                             float dpr,
                             float zRot,
                             bool deleteExisting,
                             bool hideWindow) final;
    bool destroyOpenGLSubwindow() final;
    void setOpenGLDisplayRotation(float zRot) final;
    void setOpenGLDisplayTranslation(float px, float py) final;
    void repaintOpenGLDisplay() final;

    bool hasGuestPostedAFrame() final;
    void resetGuestPostedAFrame() final;

    void setScreenMask(int width, int height, const uint8_t* rgbaData) final;
    void setScreenBackground(int width, int height, const uint8_t* rgbaData) final;
    void setDisplayLayout(int screenWidth, int screenHeight, const Rect& displayRect) final;
    void setMultiDisplay(uint32_t id,
                         int32_t x,
                         int32_t y,
                         uint32_t w,
                         uint32_t h,
                         uint32_t dpi,
                         bool add) final;
    void setMultiDisplayColorBuffer(uint32_t id, uint32_t cb) override;
    void onGuestGraphicsProcessCreate(uint64_t puid) final;
    // TODO(kaiyili): rename this interface to onGuestGraphicsProcessDestroy.
    void cleanupProcGLObjects(uint64_t puid) final;
    bool waitForProcessCleanup() final;
    struct AndroidVirtioGpuOps* getVirtioGpuOps() final;

    bool pauseAllPreSave() final;
    void resumeAll() final;

    bool save(gfxstream::Stream* stream,
              const ITextureSaverPtr& textureSaver) final;
    bool load(gfxstream::Stream* stream,
              const ITextureLoaderPtr& textureLoader) final;
    void fillGLESUsages(android_studio::EmulatorGLESUsages*) final;
    int getScreenshot(unsigned int nChannels, unsigned int* width, unsigned int* height,
                      uint8_t* pixels, size_t* cPixels, int displayId, int desiredWidth,
                      int desiredHeight, int desiredRotation, Rect rect) final;

    void preLoad() override;
    void postLoad() override;

    void addListener(FrameBufferChangeEventListener* listener) override;
    void removeListener(FrameBufferChangeEventListener* listener) override;

    void setVsyncHz(int vsyncHz) final;
    void setDisplayConfigs(int configId, int w, int h, int dpiX, int dpiY) override;
    void setDisplayActiveConfig(int configId) override;

    const void* getEglDispatch() override;
    const void* getGles2Dispatch() override;

    void setShouldSkipDraw(bool skip) override;
    bool getShouldSkipDraw() const override;

    bool beginGpuRelease(std::string* error) override;
    bool releaseGpu(std::string* error) override;
    bool retakeGpu(std::string* error) override;
    bool isGpuReleased() const override;

private:
    DISALLOW_COPY_ASSIGN_AND_MOVE(RendererImpl);

private:
    // Stop all render threads and wait until they exit,
    // and also delete them.
    void cleanupRenderThreads();

    // pauseAllPreSave() and resumeAll() without the join's bookkeeping. pauseRenderThreads()
    // returns the moment the render threads were asked to pause, after the process cleanup
    // wait that precedes it.
    bool pauseRenderThreads(std::chrono::steady_clock::time_point* askedAt = nullptr);
    void resumeRenderThreads();
    // The GPU mirror's join (mirror_tap.h): the render threads paused, the copy started, every
    // color buffer saved, the threads resumed. The tap calls it for a consumer's 'J'.
    bool saveMirrorJoin(mirror::JoinWriter& w);

    // Held by a mirror join from its pause to its resume. pauseAllPreSave() and resumeAll()
    // take it, so an embedder's pause (a snapshot, a GPU release) waits for a running join,
    // and a join refuses while the embedder holds the threads paused.
    std::mutex mJoinMutex;
    bool mPausedByEmbedder = false;

    std::unique_ptr<RenderWindow> mRenderWindow;

    std::mutex mChannelsMutex;
    std::vector<std::shared_ptr<RenderChannelImpl>> mChannels;
    std::vector<std::shared_ptr<RenderChannelImpl>> mStoppedChannels;
    bool mStopped = false;

    std::unique_ptr<RenderThread> mLoaderRenderThread;

    std::vector<RenderThread*> mAdditionalPostLoadRenderThreads;

    std::mutex mAddressSpaceRenderThreadMutex;
    std::unordered_set<RenderThread*> mAddressSpaceRenderThreads;

    // What initialize() was given, and what the emulator set on the frame buffer
    // after it, so retakeGpu() can make an equivalent one.
    int mWidth = 0;
    int mHeight = 0;
    gfxstream::host::FeatureSet mFeatures;
    bool mUseSubWindow = false;
    struct PostCallback {
        OnPostCallback onPost;
        void* context;
        bool useBgraReadback;
    };
    std::map<uint32_t, PostCallback> mPostCallbacks;
    std::optional<int> mVsyncHz;
    struct DisplayConfig {
        int configId, w, h, dpiX, dpiY;
    };
    std::vector<DisplayConfig> mDisplayConfigs;
    std::optional<int> mDisplayActiveConfig;
    std::vector<FrameBufferChangeEventListener*> mListeners;
    bool mGpuReleased = false;
    // The frame buffer's state between beginGpuRelease() and retakeGpu(), and the
    // textures it saved, which the loaded frame buffer may read lazily after.
    std::unique_ptr<MemStream> mReleasedState;
    std::shared_ptr<MemoryTextureStore> mReleasedTextures;
    // The loader of the last load, whose background texture loading a release stops.
    std::weak_ptr<ITextureLoader> mLastTextureLoader;
};

}  // namespace host
}  // namespace gfxstream
