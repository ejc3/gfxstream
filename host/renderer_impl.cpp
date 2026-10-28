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
#include "renderer_impl.h"

#include <assert.h>
#ifdef __linux__
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <limits>
#include <utility>
#include <variant>

#include "frame_buffer.h"
#include "gfxstream/common/logging.h"
#include "gfxstream/host/graphics_driver_lock.h"
#include "gfxstream/host/renderer_operations.h"
#include "gfxstream/host/tracing.h"
#include "gfxstream/system/System.h"
#include "gfxstream/threads/WorkerThread.h"
#if GFXSTREAM_ENABLE_HOST_GLES
#include "host/gl/emulated_egl_fence_sync.h"
#include "host/gl/emulation_gl.h"
#endif
#include "render_channel_impl.h"
#include "render_thread.h"

namespace gfxstream {
namespace host {

// kUseSubwindowThread is used to determine whether the RenderWindow should use
// a separate thread to manage its subwindow GL/GLES context.
// For now, this feature is disabled entirely for the following
// reasons:
//
// - It must be disabled on Windows at all times, otherwise the main window
//   becomes unresponsive after a few seconds of user interaction (e.g. trying
//   to move it over the desktop). Probably due to the subtle issues around
//   input on this platform (input-queue is global, message-queue is
//   per-thread). Also, this messes considerably the display of the
//   main window when running the executable under Wine.
//
// - On Linux/XGL and OSX/Cocoa, this used to be necessary to avoid corruption
//   issues with the GL state of the main window when using the SDL UI.
//   After the switch to Qt, this is no longer necessary and may actually cause
//   undesired interactions between the UI thread and the RenderWindow thread:
//   for example, in a multi-monitor setup the context might be recreated when
//   dragging the window between monitors, triggering a Qt-specific callback
//   in the context of RenderWindow thread, which will become blocked on the UI
//   thread, which may in turn be blocked on something else.
static const bool kUseSubwindowThread = false;
static constexpr auto kCleanupTimeout = std::chrono::milliseconds(15000);

RendererImpl::RendererImpl() = default;

RendererImpl::~RendererImpl() {
    stop(true);
    // We can't finish until the loader render thread has
    // completed else can get a crash at the end of the destructor.
    if (mLoaderRenderThread) {
        mLoaderRenderThread->wait();
    }
    mRenderWindow.reset();
}

bool RendererImpl::initialize(int width, int height, const gfxstream::host::FeatureSet& features,
                              bool useSubWindow) {
    if (mRenderWindow) {
        return false;
    }

    std::unique_ptr<RenderWindow> renderWindow(new RenderWindow(
            width, height, features, kUseSubwindowThread, useSubWindow));
    if (!renderWindow) {
        GFXSTREAM_ERROR("Could not create rendering window class");
        return false;
    }
    if (!renderWindow->isValid()) {
        GFXSTREAM_ERROR("Could not initialize emulated framebuffer\n");
        return false;
    }

    mRenderWindow = std::move(renderWindow);
    mWidth = width;
    mHeight = height;
    mFeatures = features;
    mUseSubWindow = useSubWindow;
    GFXSTREAM_DEBUG("Renderer initialized successfully");

    // This render thread won't do anything but will only preload resources
    // for the real threads to start faster.
    mLoaderRenderThread.reset(new RenderThread(nullptr));
    mLoaderRenderThread->start();

    return true;
}

void RendererImpl::stop(bool wait) {
    std::unique_lock<std::mutex> lock(mChannelsMutex);
    mStopped = true;
    auto channels = std::move(mChannels);
    lock.unlock();

    if (const auto fb = FrameBuffer::getFB()) {
        fb->setShuttingDown();
    }
    for (const auto& c : channels) {
        c->stopFromHost();
    }

    mStoppedChannels.insert(mStoppedChannels.end(),
                            std::make_move_iterator(channels.begin()),
                            std::make_move_iterator(channels.end()));

    // Each render channel is referenced in the corresponing pipe object, so
    // even if we clear the |channels| vector they could still be alive
    // for a while. This means we need to make sure to wait for render thread
    // exit explicitly.
    for (const auto& c : mStoppedChannels) {
        c->renderThread()->waitForFinished();
        {
            gfxstream::base::AutoLock driverLock(*graphicsDriverLock());
            c->renderThread()->sendExitSignal();
            c->renderThread()->wait();
        }
    }

    if (!wait) {
        return;
    }

    waitForProcessCleanup();
    mStoppedChannels.clear();
}

void RendererImpl::finish() {
    {
        std::lock_guard<std::mutex> lock(mChannelsMutex);
        mRenderWindow->setPaused(true);
    }
    cleanupRenderThreads();
    {
        std::lock_guard<std::mutex> lock(mChannelsMutex);
        mRenderWindow->setPaused(false);
    }
}

void RendererImpl::cleanupRenderThreads() {
    std::unique_lock<std::mutex> lock(mChannelsMutex);
    const auto channels = std::move(mChannels);
    assert(mChannels.empty());
    lock.unlock();
    for (const auto& c : channels) {
        // Please DO NOT notify the guest about this event (DO NOT call
        // stopFromHost() ), because this is used to kill old threads when
        // loading from a snapshot, and the newly loaded guest should not
        // be notified for those behavior.
        c->stop();
    }
    for (const auto& c : channels) {
        c->renderThread()->waitForFinished();
        {
            gfxstream::base::AutoLock driverLock(*graphicsDriverLock());
            c->renderThread()->sendExitSignal();
            c->renderThread()->wait();
        }
    }
}

bool RendererImpl::waitForProcessCleanup() {
    if (auto* fb = FrameBuffer::getFB()) {
        return fb->waitForProcessCleanup(kCleanupTimeout);
    }
    return true;
}

RenderChannelPtr RendererImpl::createRenderChannel(
        gfxstream::Stream* loadStream, uint32_t virtioGpuContextId) {
    const auto channel =
        std::make_shared<RenderChannelImpl>(loadStream, virtioGpuContextId);
    {
        std::lock_guard<std::mutex> lock(mChannelsMutex);

        if (mStopped) {
            return nullptr;
        }

        // Clean up the stopped channels.
        mChannels.erase(
                std::remove_if(mChannels.begin(), mChannels.end(),
                               [](const std::shared_ptr<RenderChannelImpl>& c) {
                                   return c->renderThread()->isFinished();
                               }),
                mChannels.end());
        mChannels.emplace_back(channel);

        // Take the time to check if our loader thread is done as well.
        if (mLoaderRenderThread && mLoaderRenderThread->isFinished()) {
            mLoaderRenderThread->wait();
            mLoaderRenderThread.reset();
        }

        GFXSTREAM_DEBUG("Started new RenderThread (total %" PRIu64 ") @%p",
                        static_cast<uint64_t>(mChannels.size()), channel->renderThread());
    }

    return channel;
}

void RendererImpl::addListener(FrameBufferChangeEventListener* listener) {
    mListeners.push_back(listener);
    if (mRenderWindow) {
        mRenderWindow->addListener(listener);
    }
}

void RendererImpl::removeListener(FrameBufferChangeEventListener* listener) {
    mListeners.erase(std::remove(mListeners.begin(), mListeners.end(), listener),
                     mListeners.end());
    if (mRenderWindow) {
        mRenderWindow->removeListener(listener);
    }
}

void* RendererImpl::addressSpaceGraphicsConsumerCreate(
        const AsgConsumerCreateInfo& info, gfxstream::Stream* loadStream) {
    auto thread = new RenderThread(info, loadStream);
    thread->start();
    std::lock_guard<std::mutex> lock(mAddressSpaceRenderThreadMutex);
    mAddressSpaceRenderThreads.emplace(thread);
    return (void*)thread;
}

void RendererImpl::addressSpaceGraphicsConsumerDestroy(void* consumer) {
    RenderThread* thread = (RenderThread*)consumer;
    {
        std::lock_guard<std::mutex> lock(mAddressSpaceRenderThreadMutex);
        mAddressSpaceRenderThreads.erase(thread);
    }

    thread->waitForFinished();
    {
        gfxstream::base::AutoLock driverLock(*graphicsDriverLock());
        thread->sendExitSignal();
        thread->wait();
    }
    delete thread;
}

void RendererImpl::addressSpaceGraphicsConsumerPreSave(void* consumer) {
    RenderThread* thread = (RenderThread*)consumer;
    thread->pausePreSnapshot();
}

void RendererImpl::addressSpaceGraphicsConsumerSave(void* consumer, gfxstream::Stream* stream) {
    RenderThread* thread = (RenderThread*)consumer;
    thread->save(stream);
}

void RendererImpl::addressSpaceGraphicsConsumerPostSave(void* consumer) {
    RenderThread* thread = (RenderThread*)consumer;
    thread->resume();
}

void RendererImpl::addressSpaceGraphicsConsumerRegisterPostLoadRenderThread(void* consumer) {
    RenderThread* thread = (RenderThread*)consumer;
    mAdditionalPostLoadRenderThreads.push_back(thread);
}

void RendererImpl::addressSpaceGraphicsConsumerReloadRingConfig(void* consumer) {
    RenderThread* thread = (RenderThread*)consumer;
    thread->addressSpaceGraphicsReloadRingConfig();
}

bool RendererImpl::pauseAllPreSave() {
    {
        std::lock_guard<std::mutex> lock(mChannelsMutex);
        if (mStopped) {
            return true;
        }
    }
    if (!waitForProcessCleanup()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mChannelsMutex);
        if (mStopped) {
            return true;
        }
        for (const auto& c : mChannels) {
            c->renderThread()->pausePreSnapshot();
        }
    }
    return true;
}

void RendererImpl::resumeAll() {
    {
        std::lock_guard<std::mutex> lock(mAddressSpaceRenderThreadMutex);
        for (const auto t : mAdditionalPostLoadRenderThreads) {
            t->resume();
        }
    }
    {
        std::lock_guard<std::mutex> lock(mChannelsMutex);
        if (mStopped) {
            return;
        }
        for (const auto& c : mChannels) {
            c->renderThread()->resume();
        }
        for (const auto& thread : mAddressSpaceRenderThreads) {
            thread->resume();
        }
        mAdditionalPostLoadRenderThreads.clear();
    }

    repaintOpenGLDisplay();
}

bool RendererImpl::save(gfxstream::Stream* stream,
                        const ITextureSaverPtr& textureSaver) {
    stream->putByte(mStopped);
    if (mStopped) {
        return true;
    }

    // pauseAllPreSave() only asks the render threads to stop; they finish
    // the packet they are decoding first. FrameBuffer::onSave reads texture
    // contents back from its own context, so wait until every paused thread
    // has actually stopped and drained its GPU work (see
    // RenderThread::saveSnapshot), otherwise the readback can race with
    // in-flight uploads and draws and capture stale or partial textures.
    {
        std::vector<std::shared_ptr<RenderChannelImpl>> channels;
        {
            std::lock_guard<std::mutex> lock(mChannelsMutex);
            channels = mChannels;
        }
        for (const auto& c : channels) {
            c->renderThread()->waitForPausePreSnapshot();
        }
    }
    {
        // Hold the lock while waiting: the threads are owned by the address
        // space device and freed by addressSpaceGraphicsConsumerDestroy(),
        // which must not run concurrently with this iteration.
        std::lock_guard<std::mutex> lock(mAddressSpaceRenderThreadMutex);
        for (RenderThread* thread : mAddressSpaceRenderThreads) {
            thread->waitForPausePreSnapshot();
        }
    }

    auto fb = FrameBuffer::getFB();
    assert(fb);
    return fb->onSave(stream, textureSaver);
}

bool RendererImpl::load(gfxstream::Stream* stream,
                        const ITextureLoaderPtr& textureLoader) {
    if (!waitForProcessCleanup()) {
        GFXSTREAM_ERROR("Failed to load snapshot: timed out waiting for process cleanup.");
        return false;
    }

    mStopped = stream->getByte();
    if (mStopped) {
        return true;
    }
    auto fb = FrameBuffer::getFB();
    assert(fb);
    mLastTextureLoader = textureLoader;
    return fb->onLoad(stream, textureLoader);
}

void RendererImpl::fillGLESUsages(android_studio::EmulatorGLESUsages* usages) {
    auto fb = FrameBuffer::getFB();
#if GFXSTREAM_ENABLE_HOST_GLES
    if (fb) fb->fillGLESUsages(usages);
#endif
}

int RendererImpl::getScreenshot(unsigned int nChannels, unsigned int* width, unsigned int* height,
                                uint8_t* pixels, size_t* cPixels, int displayId = 0,
                                int desiredWidth = 0, int desiredHeight = 0,
                                int desiredRotation = 0, Rect rect = {{0, 0}, {0, 0}}) {
    auto fb = FrameBuffer::getFB();
    if (fb) {
        return fb->getScreenshot(nChannels, width, height, pixels, cPixels,
                                 displayId, desiredWidth, desiredHeight,
                                 desiredRotation, rect);
    }
    *cPixels = 0;
    return -1;
}

void RendererImpl::setMultiDisplay(uint32_t id,
                                   int32_t x,
                                   int32_t y,
                                   uint32_t w,
                                   uint32_t h,
                                   uint32_t dpi,
                                   bool add) {
    auto fb = FrameBuffer::getFB();
    if (fb) {
        if (add) {
            fb->createDisplay(&id);
            fb->setDisplayPose(id, x, y, w, h, dpi);
        } else {
            fb->destroyDisplay(id);
        }
    }
}

void RendererImpl::setMultiDisplayColorBuffer(uint32_t id, uint32_t cb) {
    auto fb = FrameBuffer::getFB();
    if (fb) {
        fb->setDisplayColorBuffer(id, cb);
    }
}

RendererImpl::HardwareStrings RendererImpl::getHardwareStrings() {
    if (!mRenderWindow) {
        return {};
    }

    const char* vendor = nullptr;
    const char* renderer = nullptr;
    const char* version = nullptr;
    if (!mRenderWindow->getHardwareStrings(&vendor, &renderer, &version)) {
        return {};
    }
    HardwareStrings res;
    res.vendor = vendor ? vendor : "";
    res.renderer = renderer ? renderer : "";
    res.version = version ? version : "";
    return res;
}

bool RendererImpl::getVulkanEmulationDeviceInfo(char** device_name, char** driver_info,
                                                uint32_t* driver_version, uint32_t* api_version,
                                                uint32_t* vendor_id, uint32_t* device_id,
                                                uint32_t* device_type, uint64_t* device_memory) {
    if (!mRenderWindow) {
        GFXSTREAM_ERROR("%s: invalid state", __func__);
        return false;
    }
    return mRenderWindow->getVulkanEmulationDeviceInfo(device_name, driver_info, driver_version,
                                                       api_version, vendor_id, device_id,
                                                       device_type, device_memory);
}

void RendererImpl::setPostCallback(RendererImpl::OnPostCallback onPost,
                                   void* context,
                                   bool useBgraReadback,
                                   uint32_t displayId) {
    if (onPost) {
        mPostCallbacks[displayId] = {onPost, context, useBgraReadback};
    } else {
        mPostCallbacks.erase(displayId);
    }
    if (mRenderWindow) {
        mRenderWindow->setPostCallback(onPost, context, displayId, useBgraReadback);
    }
}

bool RendererImpl::asyncReadbackSupported() {
    if (!mRenderWindow) {
        return false;
    }
    return mRenderWindow->asyncReadbackSupported();
}

RendererImpl::ReadPixelsCallback
RendererImpl::getReadPixelsCallback() {
    if (!mRenderWindow) {
        return nullptr;
    }
    return mRenderWindow->getReadPixelsCallback();
}

RendererImpl::FlushReadPixelPipeline
RendererImpl::getFlushReadPixelPipeline() {
    if (!mRenderWindow) {
        return nullptr;
    }
    return mRenderWindow->getFlushReadPixelPipeline();
}

bool RendererImpl::showOpenGLSubwindow(FBNativeWindowType window,
                                       int wx,
                                       int wy,
                                       int ww,
                                       int wh,
                                       int fbw,
                                       int fbh,
                                       float dpr,
                                       float zRot,
                                       bool deleteExisting,
                                       bool hideWindow) {
    if (!mRenderWindow) {
        return false;
    }
    return mRenderWindow->setupSubWindow(window, wx, wy, ww, wh, fbw, fbh, dpr,
                                         zRot, deleteExisting, hideWindow);
}

bool RendererImpl::destroyOpenGLSubwindow() {
    if (!mRenderWindow) {
        return false;
    }
    return mRenderWindow->removeSubWindow();
}

void RendererImpl::setOpenGLDisplayRotation(float zRot) {
    if (mRenderWindow) {
        mRenderWindow->setRotation(zRot);
    }
}

void RendererImpl::setOpenGLDisplayTranslation(float px, float py) {
    if (mRenderWindow) {
        mRenderWindow->setTranslation(px, py);
    }
}

void RendererImpl::repaintOpenGLDisplay() {
    // No-op while the host GPU is released.
    if (mRenderWindow) {
        mRenderWindow->repaint();
    }
}

bool RendererImpl::hasGuestPostedAFrame() {
    if (mRenderWindow) {
        return mRenderWindow->hasGuestPostedAFrame();
    }
    return false;
}

void RendererImpl::resetGuestPostedAFrame() {
    if (mRenderWindow) {
        mRenderWindow->resetGuestPostedAFrame();
    }
}

void RendererImpl::setScreenMask(int width, int height, const uint8_t* rgbaData) {
    if (mRenderWindow) {
        mRenderWindow->setScreenMask(width, height, rgbaData);
    }
}

void RendererImpl::setScreenBackground(int width, int height, const uint8_t* rgbaData) {
    if (mRenderWindow) {
        mRenderWindow->setScreenBackground(width, height, rgbaData);
    }
}

void RendererImpl::setDisplayLayout(int screenWidth, int screenHeight, const Rect& displayRect) {
    if(!mRenderWindow) {
        GFXSTREAM_ERROR("%s: invalid render window!", __func__);
        return;
    }
    mRenderWindow->setDisplayLayout(screenWidth, screenHeight, displayRect);
}

void RendererImpl::onGuestGraphicsProcessCreate(uint64_t puid) {
    if (auto* fb = FrameBuffer::getFB()) {
        fb->createGraphicsProcessResources(puid);
    }
}

void RendererImpl::cleanupProcGLObjects(uint64_t puid) {
    if (auto* fb = FrameBuffer::getFB()) {
        fb->signalGuestProcessDead(puid);
    }
}

static struct AndroidVirtioGpuOps sVirtioGpuOps = {
    .create_buffer_with_handle =
        [](uint64_t size, uint32_t handle) {
            FrameBuffer::getFB()->createBufferWithResourceHandle(size, handle);
        },
    .create_color_buffer_with_handle =
        [](uint32_t width, uint32_t height, uint32_t format, uint32_t fwkFormat, uint32_t handle) {
            FrameBuffer::getFB()->createColorBufferWithResourceHandleDeprecated(
                width, height, (GLenum)format, (FrameworkFormat)fwkFormat, handle);
        },
    .open_color_buffer = [](uint32_t handle) { FrameBuffer::getFB()->openColorBuffer(handle); },
    .close_buffer = [](uint32_t handle) { FrameBuffer::getFB()->closeBuffer(handle); },
    .close_color_buffer = [](uint32_t handle) { FrameBuffer::getFB()->closeColorBuffer(handle); },
    .update_buffer =
        [](uint32_t handle, uint64_t offset, uint64_t size, void* bytes) {
            FrameBuffer::getFB()->updateBuffer(handle, offset, size, bytes);
        },
    .update_color_buffer =
        [](uint32_t handle, int x, int y, int width, int height, uint32_t format, uint32_t type,
           void* pixels) {
            FrameBuffer::getFB()->updateColorBufferDeprecated(handle, x, y, width, height, format, type,
                                                              pixels);
        },
    .read_buffer =
        [](uint32_t handle, uint64_t offset, uint64_t size, void* bytes) {
            FrameBuffer::getFB()->readBuffer(handle, offset, size, bytes);
        },
    .read_color_buffer =
        [](uint32_t handle, int x, int y, int width, int height, uint32_t format, uint32_t type,
           void* pixels) {
            FrameBuffer::getFB()->readColorBufferDeprecated(handle, x, y, width, height, format,
                                                            type, pixels,
                                                            std::numeric_limits<uint64_t>::max());
        },
    .read_color_buffer2 =
        [](uint32_t handle, int x, int y, int width, int height, uint32_t format, uint32_t type,
           void* pixels, uint64_t pixels_size) {
            FrameBuffer::getFB()->readColorBufferDeprecated(handle, x, y, width, height, format, type,
                                                            pixels, pixels_size);
        },
    .read_color_buffer_yuv =
        [](uint32_t handle, int x, int y, int width, int height, void* pixels,
           uint32_t pixels_size) {
            FrameBuffer::getFB()->readColorBufferYUV(handle, x, y, width, height, pixels,
                                                     pixels_size);
        },
    .post_color_buffer = [](uint32_t handle) { FrameBuffer::getFB()->post(handle); },
    .async_post_color_buffer =
        [](uint32_t handle, CpuCompletionCallback cb) {
            FrameBuffer::getFB()->postWithCallback(handle, cb);
        },
    .repost = []() { FrameBuffer::getFB()->repost(); },
#if GFXSTREAM_ENABLE_HOST_GLES
    .create_yuv_textures =
        [](uint32_t type, uint32_t count, int width, int height, uint32_t* output) {
            FrameBuffer::getFB()->createYUVTextures(type, count, width, height, output);
        },
    .destroy_yuv_textures =
        [](uint32_t type, uint32_t count, uint32_t* textures) {
            FrameBuffer::getFB()->destroyYUVTextures(type, count, textures);
        },
    .update_yuv_textures =
        [](uint32_t type, uint32_t* textures, void* privData, void* func) {
            FrameBuffer::getFB()->updateYUVTextures(type, textures, privData, func);
        },
    .swap_textures_and_update_color_buffer =
        [](uint32_t colorbufferhandle, int x, int y, int width, int height, uint32_t format,
           uint32_t type, uint32_t texture_type, uint32_t* textures, void* metadata) {
            FrameBuffer::getFB()->swapTexturesAndUpdateColorBuffer(
                colorbufferhandle, x, y, width, height, format, type, texture_type, textures);
        },
#endif
    .get_last_posted_color_buffer =
        []() { return FrameBuffer::getFB()->getLastPostedColorBuffer(); },
#if GFXSTREAM_ENABLE_HOST_GLES
    .bind_color_buffer_to_texture =
        [](uint32_t handle) { FrameBuffer::getFB()->bindColorBufferToTexture2(handle); },
    .get_global_egl_context = []() { return FrameBuffer::getFB()->getGlobalEGLContext(); },
#endif
    .set_guest_managed_color_buffer_lifetime =
        [](bool guestManaged) {
            FrameBuffer::getFB()->setGuestManagedColorBufferLifetime(guestManaged);
        },
#if GFXSTREAM_ENABLE_HOST_GLES
    .async_wait_for_gpu_with_cb =
        [](uint64_t eglsync, FenceCompletionCallback cb) {
            FrameBuffer::getFB()->asyncWaitForGpuWithCb(eglsync, cb);
        },
#endif
    .async_wait_for_gpu_vulkan_with_cb =
        [](uint64_t device, uint64_t fence, FenceCompletionCallback cb) {
            FrameBuffer::getFB()->asyncWaitForGpuVulkanWithCb(device, fence, cb);
        },
    .async_wait_for_gpu_vulkan_qsri_with_cb =
        [](uint64_t image, FenceCompletionCallback cb) {
            FrameBuffer::getFB()->asyncWaitForGpuVulkanQsriWithCb(image, cb);
        },
    .update_color_buffer_from_framework_format =
        [](uint32_t handle, int x, int y, int width, int height, uint32_t fwkFormat,
           uint32_t format, uint32_t type, void* pixels, void*) {
            FrameBuffer::getFB()->updateColorBufferDeprecated(
                handle, x, y, width, height, format, (FrameworkFormat)fwkFormat, pixels);
        },
};

struct AndroidVirtioGpuOps* RendererImpl::getVirtioGpuOps() {
    return &sVirtioGpuOps;
}

void RendererImpl::preLoad() {
    mRenderWindow->setPaused(true);
    cleanupRenderThreads();
}

void RendererImpl::postLoad() {
    mRenderWindow->setPaused(false);
}

void RendererImpl::setVsyncHz(int vsyncHz) {
    mVsyncHz = vsyncHz;
    if (mRenderWindow) {
        mRenderWindow->setVsyncHz(vsyncHz);
    }
}

void RendererImpl::setDisplayConfigs(int configId, int w, int h,
                                     int dpiX, int dpiY) {
    mDisplayConfigs.erase(std::remove_if(mDisplayConfigs.begin(), mDisplayConfigs.end(),
                                         [configId](const DisplayConfig& c) {
                                             return c.configId == configId;
                                         }),
                          mDisplayConfigs.end());
    mDisplayConfigs.push_back({configId, w, h, dpiX, dpiY});
    if (mRenderWindow) {
        mRenderWindow->setDisplayConfigs(configId, w, h, dpiX, dpiY);
    }
}

void RendererImpl::setDisplayActiveConfig(int configId) {
    mDisplayActiveConfig = configId;
    if (mRenderWindow) {
        mRenderWindow->setDisplayActiveConfig(configId);
    }
}

const void* RendererImpl::getEglDispatch() {
#if GFXSTREAM_ENABLE_HOST_GLES
    return FrameBuffer::getFB()->getEglDispatch();
#else
    return nullptr;
#endif
}

const void* RendererImpl::getGles2Dispatch() {
#if GFXSTREAM_ENABLE_HOST_GLES
    return FrameBuffer::getFB()->getGles2Dispatch();
#else
    return nullptr;
#endif
}

// The process's open descriptors and mappings of GPU device files, as text for an error
// ("" when there are none).
static std::string gpuDeviceHandles() {
#ifdef __linux__
    auto isGpuDevice = [](const std::string& path) {
        return path.rfind("/dev/nvidia", 0) == 0 || path.rfind("/dev/dri/", 0) == 0;
    };
    int fds = 0;
    if (DIR* dir = opendir("/proc/self/fd")) {
        while (struct dirent* entry = readdir(dir)) {
            char link[64];
            char target[PATH_MAX];
            snprintf(link, sizeof(link), "/proc/self/fd/%s", entry->d_name);
            ssize_t n = readlink(link, target, sizeof(target) - 1);
            if (n > 0) {
                target[n] = 0;
                if (isGpuDevice(target)) {
                    fds++;
                }
            }
        }
        closedir(dir);
    }
    int mappings = 0;
    if (FILE* maps = fopen("/proc/self/maps", "r")) {
        char line[1024];
        while (fgets(line, sizeof(line), maps)) {
            const char* path = strchr(line, '/');
            if (path && isGpuDevice(path)) {
                mappings++;
            }
        }
        fclose(maps);
    }
    if (fds || mappings) {
        return std::to_string(fds) + " GPU device descriptor(s) and " + std::to_string(mappings) +
               " mapping(s)";
    }
#endif
    return "";
}

// Textures the frame buffer saves for a GPU release, kept in memory.
class MemoryTextureStore : public ITextureLoader, public ITextureSaver {
  public:
    void saveTexture(uint32_t textureId, const saver_t& saver) override {
        MemStream stream;
        saver(&stream, nullptr);
        mTextures[textureId] = stream.buffer();
    }

    void loadTexture(uint32_t textureId, const loader_t& callback) override {
        auto it = mTextures.find(textureId);
        if (it == mTextures.end()) {
            return;
        }
        std::vector<char> copy = it->second;
        MemStream stream(std::move(copy));
        callback(&stream);
    }

    bool start() override { return true; }

  private:
    std::unordered_map<uint32_t, std::vector<char>> mTextures;
};

bool RendererImpl::beginGpuRelease(std::string* error) {
    if (mGpuReleased) {
        *error = "the GPU is already released";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mChannelsMutex);
        if (mStopped) {
            *error = "the renderer is stopped";
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mAddressSpaceRenderThreadMutex);
        if (!mAddressSpaceRenderThreads.empty()) {
            *error = "the guest uses address space graphics, which a GPU release does not support";
            return false;
        }
    }
    if (const auto loader = mLastTextureLoader.lock()) {
        // The last load (a retake's or the emulator's own snapshot load) may still be
        // restoring textures on a background thread that refers to the translator's
        // display, which the release deletes: stop it and detach it from the loader. A
        // texture it did not reach is restored when the save below touches it.
        loader->detachAsyncUse();
    }
    if (!pauseAllPreSave()) {
        *error = "timed out waiting for guest process cleanup";
        return false;
    }
    auto state = std::make_unique<MemStream>();
    auto textures = std::make_shared<MemoryTextureStore>();
    ITextureSaverPtr saver = textures;
    if (!save(state.get(), saver)) {
        resumeAll();
        *error = "saving the frame buffer failed";
        return false;
    }
    mReleasedState = std::move(state);
    mReleasedTextures = std::move(textures);
    return true;
}

bool RendererImpl::releaseGpu(std::string* error) {
    if (!mReleasedState) {
        *error = "beginGpuRelease() has not saved the frame buffer";
        return false;
    }
    if (mLoaderRenderThread) {
        mLoaderRenderThread->wait();
        mLoaderRenderThread.reset();
    }
    if (const auto fb = FrameBuffer::getFB()) {
        fb->setShuttingDown();
    }
    std::vector<std::shared_ptr<RenderChannelImpl>> channels;
    {
        std::lock_guard<std::mutex> lock(mChannelsMutex);
        channels = std::move(mChannels);
        mChannels.clear();
        mStoppedChannels.clear();
    }
    // Every paused thread leaves its pause by exiting: its contexts stay in the saved
    // frame buffer, and a thread recreated from its saved state carries on.
    for (const auto& c : channels) {
        c->renderThread()->exitFromSnapshotPause();
    }
    for (const auto& c : channels) {
        c->renderThread()->waitForFinished();
        gfxstream::base::AutoLock driverLock(*graphicsDriverLock());
        c->renderThread()->sendExitSignal();
        c->renderThread()->wait();
    }
    channels.clear();
    mAdditionalPostLoadRenderThreads.clear();
    mRenderWindow.reset();
    mGpuReleased = true;
#if GFXSTREAM_ENABLE_HOST_GLES
    if (!gl::EmulationGl::releaseHostDriver()) {
        *error = "the host EGL driver cannot be released (only EGL-on-EGL can)";
        return false;
    }
#endif
    const std::string held = gpuDeviceHandles();
    if (!held.empty()) {
        *error = "after the release the process still has " + held;
        return false;
    }
    GFXSTREAM_INFO("Released the host GPU.");
    return true;
}

bool RendererImpl::retakeGpu(std::string* error) {
    if (!mGpuReleased) {
        *error = "the GPU is not released";
        return false;
    }
    std::unique_ptr<RenderWindow> renderWindow(
        new RenderWindow(mWidth, mHeight, mFeatures, kUseSubwindowThread, mUseSubWindow));
    if (!renderWindow->isValid()) {
        *error = "could not initialize a frame buffer on the GPU";
        return false;
    }
    mRenderWindow = std::move(renderWindow);
    for (FrameBufferChangeEventListener* listener : mListeners) {
        mRenderWindow->addListener(listener);
    }
    for (const auto& [displayId, cb] : mPostCallbacks) {
        mRenderWindow->setPostCallback(cb.onPost, cb.context, displayId, cb.useBgraReadback);
    }
    if (mVsyncHz) {
        mRenderWindow->setVsyncHz(*mVsyncHz);
    }
    for (const DisplayConfig& c : mDisplayConfigs) {
        mRenderWindow->setDisplayConfigs(c.configId, c.w, c.h, c.dpiX, c.dpiY);
    }
    if (mDisplayActiveConfig) {
        mRenderWindow->setDisplayActiveConfig(*mDisplayActiveConfig);
    }
    {
        std::lock_guard<std::mutex> lock(mChannelsMutex);
        mStopped = false;
    }
    mGpuReleased = false;
    if (mReleasedState) {
        mReleasedState->rewind();
        ITextureLoaderPtr loader = mReleasedTextures;
        if (!load(mReleasedState.get(), loader)) {
            *error = "loading the saved frame buffer onto the GPU failed";
            return false;
        }
        mReleasedState.reset();
    }
    GFXSTREAM_INFO("Took the host GPU up again.");
    return true;
}

bool RendererImpl::isGpuReleased() const { return mGpuReleased; }

void RendererImpl::setShouldSkipDraw(bool skip) {
    set_gfxstream_should_skip_draw(skip);
}

bool RendererImpl::getShouldSkipDraw() const {
    return get_gfxstream_should_skip_draw();
}

}  // namespace host
}  // namespace gfxstream
