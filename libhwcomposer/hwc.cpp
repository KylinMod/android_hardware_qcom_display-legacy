/*
 * Copyright (C) 2010 The Android Open Source Project
 * Copyright (C) 2012, Code Aurora Forum. All rights reserved.
 * Copyright (C) 2012-2013, The Linux Foundation. All rights reserved.
 *
 * Not a Contribution, Apache license notifications and license are retained
 * for attribution purposes only.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <fcntl.h>
#include <errno.h>

#include <cutils/log.h>
#include <cutils/atomic.h>
#include <EGL/egl.h>

#include <overlay.h>
#include <fb_priv.h>
#include <mdp_version.h>
#include "hwc_utils.h"
#include "hwc_qbuf.h"
#include "hwc_video.h"
#include "hwc_uimirror.h"
#include "hwc_copybit.h"
#include "hwc_external.h"
#include "hwc_mdpcomp.h"
#include "hwc_extonly.h"
#include "qcom_ui.h"

#define VSYNC_DEBUG 0
using namespace qhwc;
#include <utils/Trace.h>
#include <sys/ioctl.h>
#include <overlay.h>
#include <overlayRotator.h>
#include <mdp_version.h>
#include "hwc_utils.h"
#include "hwc_video.h"
#include "hwc_fbupdate.h"
#include "hwc_mdpcomp.h"
#include "external.h"
#include "hwc_copybit.h"
#include "profiler.h"

using namespace qhwc;
#define VSYNC_DEBUG 0

static int hwc_device_open(const struct hw_module_t* module,
                           const char* name,
                           struct hw_device_t** device);

static struct hw_module_methods_t hwc_module_methods = {
    open: hwc_device_open
};

hwc_module_t HAL_MODULE_INFO_SYM = {
    common: {
        tag: HARDWARE_MODULE_TAG,
        version_major: 2,
        version_minor: 0,
        id: HWC_HARDWARE_MODULE_ID,
        name: "Qualcomm Hardware Composer Module",
        author: "CodeAurora Forum",
        methods: &hwc_module_methods,
        dso: 0,
        reserved: {0},
    }
};

/*
 * Save callback functions registered to HWC
 */
static void hwc_registerProcs(struct hwc_composer_device* dev,
                              hwc_procs_t const* procs)
{
static void hwc_registerProcs(struct hwc_composer_device_1* dev,
                              hwc_procs_t const* procs)
{
    ALOGI("%s", __FUNCTION__);
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    if(!ctx) {
        ALOGE("%s: Invalid context", __FUNCTION__);
        return;
    }
    ctx->device.reserved_proc[0] = (void*)procs;
    ctx->proc = procs;

    // Now that we have the functions needed, kick off
    // the uevent & vsync threads
    init_uevent_thread(ctx);
    init_vsync_thread(ctx);
}

static int hwc_prepare(hwc_composer_device_t *dev, hwc_layer_list_t* list)
{
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    ctx->overlayInUse = false;

    if(ctx->mExtDisplay->getExternalDisplay())
        ovutils::setExtType(ctx->mExtDisplay->getExternalDisplay());
    if (ctx->hdmi_pending == true) {
        if ((qdutils::MDPVersion::getInstance().getMDPVersion() >=
            qdutils::MDP_V4_2) ||((ctx->mOverlay->getState() !=
            ovutils::OV_BYPASS_3_LAYER) && (ctx->mOverlay->getState() !=
            ovutils::OV_BYPASS_2_LAYER) && (ctx->mOverlay->getState() !=
                                ovutils::OV_BYPASS_1_LAYER))) {
            ctx->mExtDisplay->processUEventOnline((const char*)ctx->mHDMIEvent);
            ctx->hdmi_pending = false;
        }
    }
    if (LIKELY(list)) {
        //reset for this draw round
        VideoOverlay::reset();
        ExtOnly::reset();

        getLayerStats(ctx, list);
        // Mark all layers to COPYBIT initially
        CopyBit::prepare(ctx, list);
        if(VideoOverlay::prepare(ctx, list)) {
            ctx->overlayInUse = true;
            //Nothing here
        } else if(ExtOnly::prepare(ctx, list)) {
            ctx->overlayInUse = true;
        } else if(UIMirrorOverlay::prepare(ctx, list)) {
            ctx->overlayInUse = true;
        } else if(MDPComp::configure(dev, list)) {
            ctx->overlayInUse = true;
        } else if (0) {
            //Other features
            ctx->overlayInUse = true;
        } else { // Else set this flag to false, otherwise video cases
                 // fail in non-overlay targets.
            ctx->overlayInUse = false;
            ctx->mOverlay->setState(ovutils::OV_CLOSED);
        }

        qdutils::CBUtils::checkforGPULayer(list);
//Helper
static void reset(hwc_context_t *ctx, int numDisplays,
                  hwc_display_contents_1_t** displays) {
    memset(ctx->listStats, 0, sizeof(ctx->listStats));
    for(int i = 0; i < HWC_NUM_DISPLAY_TYPES; i++) {
        hwc_display_contents_1_t *list = displays[i];
        // XXX:SurfaceFlinger no longer guarantees that this
        // value is reset on every prepare. However, for the layer
        // cache we need to reset it.
        // We can probably rethink that later on
        if (LIKELY(list && list->numHwLayers > 1)) {
            for(uint32_t j = 0; j < list->numHwLayers; j++) {
                if(list->hwLayers[j].compositionType != HWC_FRAMEBUFFER_TARGET)
                    list->hwLayers[j].compositionType = HWC_FRAMEBUFFER;
            }
        }

        if(ctx->mFBUpdate[i])
            ctx->mFBUpdate[i]->reset();
        if(ctx->mVidOv[i])
            ctx->mVidOv[i]->reset();
        if(ctx->mCopyBit[i])
            ctx->mCopyBit[i]->reset();
        if(ctx->mLayerRotMap[i])
            ctx->mLayerRotMap[i]->reset();

    }
}

//clear prev layer prop flags and realloc for current frame
static void reset_layer_prop(hwc_context_t* ctx, int dpy, int numAppLayers) {
    if(ctx->layerProp[dpy]) {
       delete[] ctx->layerProp[dpy];
       ctx->layerProp[dpy] = NULL;
    }
    ctx->layerProp[dpy] = new LayerProp[numAppLayers];
}

static int display_commit(hwc_context_t *ctx, int dpy) {
    struct mdp_display_commit commit_info;
    memset(&commit_info, 0, sizeof(struct mdp_display_commit));
    commit_info.flags = MDP_DISPLAY_COMMIT_OVERLAY;
    if(ioctl(ctx->dpyAttr[dpy].fd, MSMFB_DISPLAY_COMMIT, &commit_info) == -1) {
       ALOGE("%s: MSMFB_DISPLAY_COMMIT for primary failed", __FUNCTION__);
       return -errno;
    }
    return 0;
}

static int hwc_prepare_primary(hwc_composer_device_1 *dev,
        hwc_display_contents_1_t *list) {
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    const int dpy = HWC_DISPLAY_PRIMARY;
    if(UNLIKELY(!ctx->mBasePipeSetup) && 
            qdutils::MDPVersion::getInstance().getMDPVersion() >= qdutils::MDP_V4_2)
        setupBasePipe(ctx);
    if (LIKELY(list && list->numHwLayers > 1) &&
            ctx->dpyAttr[dpy].isActive) {
        reset_layer_prop(ctx, dpy, list->numHwLayers - 1);
        uint32_t last = list->numHwLayers - 1;
        hwc_layer_1_t *fbLayer = &list->hwLayers[last];
        if(fbLayer->handle) {
            if(list->numHwLayers > MAX_NUM_LAYERS) {
                ctx->mFBUpdate[dpy]->prepare(ctx, list);
                return 0;
            }
            setListStats(ctx, list, dpy);
            bool ret = ctx->mMDPComp->prepare(ctx, list);
            if(!ret) {
                // IF MDPcomp fails use this route
                ctx->mVidOv[dpy]->prepare(ctx, list);
                ctx->mFBUpdate[dpy]->prepare(ctx, list);
                // Use Copybit, when MDP comp fails
                if(ctx->mCopyBit[dpy])
                    ctx->mCopyBit[dpy]->prepare(ctx, list, dpy);
                ctx->mLayerCache[dpy]->updateLayerCache(list);
            }
        }
    }
    return 0;
}

static int hwc_prepare_external(hwc_composer_device_1 *dev,
        hwc_display_contents_1_t *list, int dpy) {

    hwc_context_t* ctx = (hwc_context_t*)(dev);
    Locker::Autolock _l(ctx->mExtLock);

    if (LIKELY(list && list->numHwLayers > 1) &&
            ctx->dpyAttr[dpy].isActive &&
            ctx->dpyAttr[dpy].connected) {
        reset_layer_prop(ctx, dpy, list->numHwLayers - 1);
        uint32_t last = list->numHwLayers - 1;
        hwc_layer_1_t *fbLayer = &list->hwLayers[last];
        if(!ctx->dpyAttr[dpy].isPause) {
            if(fbLayer->handle) {
                ctx->mExtDispConfiguring = false;
                if(list->numHwLayers > MAX_NUM_LAYERS) {
                    ctx->mFBUpdate[dpy]->prepare(ctx, list);
                    return 0;
                }
                setListStats(ctx, list, dpy);
                ctx->mVidOv[dpy]->prepare(ctx, list);
                ctx->mFBUpdate[dpy]->prepare(ctx, list);
                ctx->mLayerCache[dpy]->updateLayerCache(list);
                if(ctx->mCopyBit[dpy])
                    ctx->mCopyBit[dpy]->prepare(ctx, list, dpy);
            }
        } else {
            // External Display is in Pause state.
            // ToDo:
            // Mark all application layers as OVERLAY so that
            // GPU will not compose. This is done for power
            // optimization
        }
    }
    return 0;
}

static int hwc_eventControl(struct hwc_composer_device* dev,
                             int event, int value)
{
    int ret = 0;
    static int prev_value, temp;

    hwc_context_t* ctx = (hwc_context_t*)(dev);
    private_module_t* m = reinterpret_cast<private_module_t*>(
                ctx->mFbDev->common.module);
    switch(event) {
#ifndef NO_HW_VSYNC
        case HWC_EVENT_VSYNC:
            if (value == prev_value){
                //TODO see why HWC gets repeated events
                ALOGD_IF(VSYNC_DEBUG, "%s - VSYNC is already %s",
                        __FUNCTION__, (value)?"ENABLED":"DISABLED");
            }
            temp = ctx->vstate.enable;
            if(ioctl(m->framebuffer->fd, MSMFB_OVERLAY_VSYNC_CTRL, &value) < 0)
                ret = -errno;

            /* vsync state change logic */
            if (value == 1) {
                //unblock vsync thread
                pthread_mutex_lock(&ctx->vstate.lock);
                ctx->vstate.enable = true;
                pthread_cond_signal(&ctx->vstate.cond);
                pthread_mutex_unlock(&ctx->vstate.lock);
            }
            if (value == 0 && temp) {
                //vsync thread will block
                pthread_mutex_lock(&ctx->vstate.lock);
                ctx->vstate.enable = false;
                pthread_mutex_unlock(&ctx->vstate.lock);
            }
            ALOGD_IF (VSYNC_DEBUG, "VSYNC state changed from %s to %s",
              (prev_value)?"ENABLED":"DISABLED", (value)?"ENABLED":"DISABLED");
            prev_value = value;
            /* vsync state change logic - end*/

             if(ctx->mExtDisplay->isHDMIConfigured() &&
                (ctx->mExtDisplay->getExternalDisplay()==EXTERN_DISPLAY_FB1)) {
                ret = ctx->mExtDisplay->enableHDMIVsync(value);
             }
           break;
#endif
       case HWC_EVENT_ORIENTATION:
             ctx->deviceOrientation = value;
           break;
        default:
            ret = -EINVAL;
    }
    return ret;
}

static int hwc_query(struct hwc_composer_device* dev,
                     int param, int* value)
{
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    private_module_t* m = reinterpret_cast<private_module_t*>(
        ctx->mFbDev->common.module);
static int hwc_prepare(hwc_composer_device_1 *dev, size_t numDisplays,
                       hwc_display_contents_1_t** displays)
{
    int ret = 0;
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    Locker::Autolock _l(ctx->mBlankLock);
    reset(ctx, numDisplays, displays);

    ctx->mOverlay->configBegin();
    ctx->mRotMgr->configBegin();
    ctx->mNeedsRotator = false;

    for (int32_t i = numDisplays; i >= 0; i--) {
        hwc_display_contents_1_t *list = displays[i];
        switch(i) {
            case HWC_DISPLAY_PRIMARY:
                ret = hwc_prepare_primary(dev, list);
                break;
            case HWC_DISPLAY_EXTERNAL:
            case HWC_DISPLAY_VIRTUAL:
                ret = hwc_prepare_external(dev, list, i);
                break;
            default:
                ret = -EINVAL;
        }
    }

    ctx->mOverlay->configDone();
    ctx->mRotMgr->configDone();

    return ret;
}

static int hwc_eventControl(struct hwc_composer_device_1* dev, int dpy,
                             int event, int enable)
{
    int ret = 0;
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    pthread_mutex_lock(&ctx->vstate.lock);
    switch(event) {
        case HWC_EVENT_VSYNC:
            if (ctx->vstate.enable == enable)
                break;
            ret = hwc_vsync_control(ctx, dpy, enable);
            if(ret == 0) {
                ctx->vstate.enable = !!enable;
                pthread_cond_signal(&ctx->vstate.cond);
            }
            ALOGD_IF (VSYNC_DEBUG, "VSYNC state changed to %s",
                      (enable)?"ENABLED":"DISABLED");
            break;
        default:
            ret = -EINVAL;
    }
    pthread_mutex_unlock(&ctx->vstate.lock);
    return ret;
}

static int hwc_blank(struct hwc_composer_device_1* dev, int dpy, int blank)
{
    ATRACE_CALL();
    hwc_context_t* ctx = (hwc_context_t*)(dev);

    Locker::Autolock _l(ctx->mBlankLock);
    int ret = 0;
    ALOGD("%s: %s display: %d", __FUNCTION__,
          blank==1 ? "Blanking":"Unblanking", dpy);
    if(blank) {
        // free up all the overlay pipes in use
        // when we get a blank for either display
        // makes sure that all pipes are freed
        ctx->mOverlay->configBegin();
        ctx->mOverlay->configDone();
        ctx->mRotMgr->clear();
    }
    switch(dpy) {
        case HWC_DISPLAY_PRIMARY:
            if(blank) {
                ret = ioctl(ctx->dpyAttr[dpy].fd, FBIOBLANK,FB_BLANK_POWERDOWN);

                if(ctx->dpyAttr[HWC_DISPLAY_VIRTUAL].connected == true) {
                    // Surfaceflinger does not send Blank/unblank event to hwc
                    // for virtual display, handle it explicitly when blank for
                    // primary is invoked, so that any pipes unset get committed
                    if (display_commit(ctx, HWC_DISPLAY_VIRTUAL) < 0) {
                        ret = -1;
                        ALOGE("%s:post failed for virtual display !!",
                                                            __FUNCTION__);
                    } else {
                        ctx->dpyAttr[HWC_DISPLAY_VIRTUAL].isActive = !blank;
                    }
                }
            } else {
                ret = ioctl(ctx->dpyAttr[dpy].fd, FBIOBLANK, FB_BLANK_UNBLANK);
                if(ctx->dpyAttr[HWC_DISPLAY_VIRTUAL].connected == true) {
                    ctx->dpyAttr[HWC_DISPLAY_VIRTUAL].isActive = !blank;
                }
            }
            break;
        case HWC_DISPLAY_EXTERNAL:
            if(blank) {
                // call external framebuffer commit on blank,
                // so that any pipe unsets gets committed
                if (display_commit(ctx, dpy) < 0) {
                    ret = -1;
                    ALOGE("%s:post failed for external display !! ", __FUNCTION__);
                }
            } else {
            }
            break;
        default:
            return -EINVAL;
    }
    // Enable HPD here, as during bootup unblank is called
    // when SF is completely initialized
    ctx->mExtDisplay->setHPD(1);
    if(ret == 0){
        ctx->dpyAttr[dpy].isActive = !blank;
    } else {
        ALOGE("%s: Failed in %s display: %d error:%s", __FUNCTION__,
              blank==1 ? "blanking":"unblanking", dpy, strerror(errno));
        return ret;
    }

    ALOGD("%s: Done %s display: %d", __FUNCTION__,
          blank==1 ? "blanking":"unblanking", dpy);
    return 0;
}

static int hwc_query(struct hwc_composer_device_1* dev,
                     int param, int* value)
{
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    int supported = HWC_DISPLAY_PRIMARY_BIT;

    switch (param) {
    case HWC_BACKGROUND_LAYER_SUPPORTED:
        // Not supported for now
        value[0] = 0;
        break;
    case HWC_VSYNC_PERIOD:
        value[0] = 1000000000.0 / m->fps;
        ALOGI("fps: %d", value[0]);
    case HWC_DISPLAY_TYPES_SUPPORTED:
        if(ctx->mMDP.hasOverlay)
            supported |= HWC_DISPLAY_EXTERNAL_BIT;
        value[0] = supported;
        break;
    default:
        return -EINVAL;
    }
    return 0;

}

static int hwc_set(hwc_composer_device_t *dev,
                   hwc_display_t dpy,
                   hwc_surface_t sur,
                   hwc_layer_list_t* list)
{
    int ret = 0;
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    if (LIKELY(list)) {
        VideoOverlay::draw(ctx, list);
        ExtOnly::draw(ctx, list);
        CopyBit::draw(ctx, list, (EGLDisplay)dpy, (EGLSurface)sur);
        MDPComp::draw(ctx, list);
        EGLBoolean sucess = eglSwapBuffers((EGLDisplay)dpy, (EGLSurface)sur);
        if(ctx->mMDP.hasOverlay) {
            wait4fbPost(ctx);
            //Can draw to HDMI only when fb_post is reached
            UIMirrorOverlay::draw(ctx);
            //HDMI commit and primary commit (PAN) happening in parallel
            if(ctx->mExtDisplay->getExternalDisplay())
                ctx->mExtDisplay->commit();
            //Virtual barrier for threads to finish
            wait4Pan(ctx);
        }
    } else {
        ctx->mOverlay->setState(ovutils::OV_CLOSED);
        ctx->qbuf->unlockAll();
    }


    ctx->qbuf->unlockAllPrevious();
    return ret;
}


static int hwc_set_primary(hwc_context_t *ctx, hwc_display_contents_1_t* list) {
    ATRACE_CALL();
    int ret = 0;
    const int dpy = HWC_DISPLAY_PRIMARY;
    if (LIKELY(list) && ctx->dpyAttr[dpy].isActive) {
        uint32_t last = list->numHwLayers - 1;
        hwc_layer_1_t *fbLayer = &list->hwLayers[last];
        int fd = -1; //FenceFD from the Copybit(valid in async mode)
        bool copybitDone = false;
        if(ctx->mCopyBit[dpy])
            copybitDone = ctx->mCopyBit[dpy]->draw(ctx, list, dpy, &fd);
        if(list->numHwLayers > 1)
            hwc_sync(ctx, list, dpy, fd);
        if (!ctx->mVidOv[dpy]->draw(ctx, list)) {
            ALOGE("%s: VideoOverlay draw failed", __FUNCTION__);
            ret = -1;
        }
        if (!ctx->mMDPComp->draw(ctx, list)) {
            ALOGE("%s: MDPComp draw failed", __FUNCTION__);
            ret = -1;
        }

        //TODO We dont check for SKIP flag on this layer because we need PAN
        //always. Last layer is always FB
        private_handle_t *hnd = (private_handle_t *)fbLayer->handle;
        if(copybitDone) {
            hnd = ctx->mCopyBit[dpy]->getCurrentRenderBuffer();
        }

        if(hnd) {
            if (!ctx->mFBUpdate[dpy]->draw(ctx, hnd)) {
                ALOGE("%s: FBUpdate draw failed", __FUNCTION__);
                ret = -1;
            }
        }

        if (display_commit(ctx, dpy) < 0) {
            ALOGE("%s: display commit fail!", __FUNCTION__);
            return -1;
        }
    }

    closeAcquireFds(list);
    return ret;
}

static int hwc_set_external(hwc_context_t *ctx,
                            hwc_display_contents_1_t* list, int dpy)
{
    ATRACE_CALL();
    int ret = 0;
    Locker::Autolock _l(ctx->mExtLock);

    if (LIKELY(list) && ctx->dpyAttr[dpy].isActive &&
        !ctx->dpyAttr[dpy].isPause &&
        ctx->dpyAttr[dpy].connected) {
        uint32_t last = list->numHwLayers - 1;
        hwc_layer_1_t *fbLayer = &list->hwLayers[last];
        int fd = -1; //FenceFD from the Copybit(valid in async mode)
        bool copybitDone = false;
        if(ctx->mCopyBit[dpy])
            copybitDone = ctx->mCopyBit[dpy]->draw(ctx, list, dpy, &fd);

        if(list->numHwLayers > 1)
            hwc_sync(ctx, list, dpy, fd);

        if (!ctx->mVidOv[dpy]->draw(ctx, list)) {
            ALOGE("%s: VideoOverlay::draw fail!", __FUNCTION__);
            ret = -1;
        }

        private_handle_t *hnd = (private_handle_t *)fbLayer->handle;
        if(copybitDone) {
            hnd = ctx->mCopyBit[dpy]->getCurrentRenderBuffer();
        }

        if(hnd) {
            if (!ctx->mFBUpdate[dpy]->draw(ctx, hnd)) {
                ALOGE("%s: FBUpdate::draw fail!", __FUNCTION__);
                ret = -1;
            }
        }

        if (display_commit(ctx, dpy) < 0) {
            ALOGE("%s: display commit fail!", __FUNCTION__);
            ret = -1;
        }
    }

    closeAcquireFds(list);
    return ret;
}

static int hwc_set(hwc_composer_device_1 *dev,
                   size_t numDisplays,
                   hwc_display_contents_1_t** displays)
{
    int ret = 0;
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    Locker::Autolock _l(ctx->mBlankLock);
    for (uint32_t i = 0; i <= numDisplays; i++) {
        hwc_display_contents_1_t* list = displays[i];
        switch(i) {
            case HWC_DISPLAY_PRIMARY:
                ret = hwc_set_primary(ctx, list);
                break;
            case HWC_DISPLAY_EXTERNAL:
            case HWC_DISPLAY_VIRTUAL:
            /* ToDo: We are using hwc_set_external path for both External and
                     Virtual displays on HWC1.1. Eventually, we will have
                     separate functions when we move to HWC1.2
            */
                ret = hwc_set_external(ctx, list, i);
                break;
            default:
                ret = -EINVAL;
        }
    }
    // This is only indicative of how many times SurfaceFlinger posts
    // frames to the display.
    CALC_FPS();
    return ret;
}

int hwc_getDisplayConfigs(struct hwc_composer_device_1* dev, int disp,
        uint32_t* configs, size_t* numConfigs) {
    int ret = 0;
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    //in 1.1 there is no way to choose a config, report as config id # 0
    //This config is passed to getDisplayAttributes. Ignore for now.
    switch(disp) {
        case HWC_DISPLAY_PRIMARY:
            if(*numConfigs > 0) {
                configs[0] = 0;
                *numConfigs = 1;
            }
            ret = 0; //NO_ERROR
            break;
        case HWC_DISPLAY_EXTERNAL:
        case HWC_DISPLAY_VIRTUAL:
            ret = -1; //Not connected
            if(ctx->dpyAttr[disp].connected) {
                ret = 0; //NO_ERROR
                if(*numConfigs > 0) {
                    configs[0] = 0;
                    *numConfigs = 1;
                }
            }
            break;
    }
    return ret;
}

int hwc_getDisplayAttributes(struct hwc_composer_device_1* dev, int disp,
        uint32_t config, const uint32_t* attributes, int32_t* values) {

    hwc_context_t* ctx = (hwc_context_t*)(dev);
    //If hotpluggable displays(i.e, HDMI, WFD) are inactive return error
    if( (disp >= HWC_DISPLAY_EXTERNAL) && !ctx->dpyAttr[disp].connected) {
        return -1;
    }

    //From HWComposer
    static const uint32_t DISPLAY_ATTRIBUTES[] = {
        HWC_DISPLAY_VSYNC_PERIOD,
        HWC_DISPLAY_WIDTH,
        HWC_DISPLAY_HEIGHT,
        HWC_DISPLAY_DPI_X,
        HWC_DISPLAY_DPI_Y,
        HWC_DISPLAY_NO_ATTRIBUTE,
    };

    const int NUM_DISPLAY_ATTRIBUTES = (sizeof(DISPLAY_ATTRIBUTES) /
            sizeof(DISPLAY_ATTRIBUTES)[0]);

    for (size_t i = 0; i < NUM_DISPLAY_ATTRIBUTES - 1; i++) {
        switch (attributes[i]) {
        case HWC_DISPLAY_VSYNC_PERIOD:
            values[i] = ctx->dpyAttr[disp].vsync_period;
            break;
        case HWC_DISPLAY_WIDTH:
            values[i] = ctx->dpyAttr[disp].xres;
            ALOGD("%s disp = %d, width = %d",__FUNCTION__, disp,
                    ctx->dpyAttr[disp].xres);
            break;
        case HWC_DISPLAY_HEIGHT:
            values[i] = ctx->dpyAttr[disp].yres;
            ALOGD("%s disp = %d, height = %d",__FUNCTION__, disp,
                    ctx->dpyAttr[disp].yres);
            break;
        case HWC_DISPLAY_DPI_X:
            values[i] = (int32_t) (ctx->dpyAttr[disp].xdpi*1000.0);
            break;
        case HWC_DISPLAY_DPI_Y:
            values[i] = (int32_t) (ctx->dpyAttr[disp].ydpi*1000.0);
            break;
        default:
            ALOGE("Unknown display attribute %d",
                    attributes[i]);
            return -EINVAL;
        }
    }
    return 0;
}

void hwc_dump(struct hwc_composer_device_1* dev, char *buff, int buff_len)
{
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    android::String8 aBuf("");
    dumpsys_log(aBuf, "Qualcomm HWC state:\n");
    dumpsys_log(aBuf, "  MDPVersion=%d\n", ctx->mMDP.version);
    dumpsys_log(aBuf, "  DisplayPanel=%c\n", ctx->mMDP.panel);
    ctx->mMDPComp->dump(aBuf);
    char ovDump[2048] = {'\0'};
    ctx->mOverlay->getDump(ovDump, 2048);
    dumpsys_log(aBuf, ovDump);
    ovDump[0] = '\0';
    ctx->mRotMgr->getDump(ovDump, 2048);
    dumpsys_log(aBuf, ovDump);
    strlcpy(buff, aBuf.string(), buff_len);
}

static int hwc_device_close(struct hw_device_t *dev)
{
    if(!dev) {
        ALOGE("%s: NULL device pointer", __FUNCTION__);
        return -1;
    }
    closeContext((hwc_context_t*)dev);
    free(dev);

    return 0;
}

static int hwc_device_open(const struct hw_module_t* module, const char* name,
                           struct hw_device_t** device)
{
    int status = -EINVAL;

    if (!strcmp(name, HWC_HARDWARE_COMPOSER)) {
        struct hwc_context_t *dev;
        dev = (hwc_context_t*)malloc(sizeof(*dev));
        memset(dev, 0, sizeof(*dev));

        //Initialize hwc context
        initContext(dev);

        //Setup HWC methods
        hwc_methods_t *methods;
        methods = (hwc_methods_t *)malloc(sizeof(*methods));
        memset(methods, 0, sizeof(*methods));
        methods->eventControl = hwc_eventControl;

        dev->device.common.tag     = HARDWARE_DEVICE_TAG;
#ifndef NO_HW_VSYNC
        //XXX: This disables hardware vsync on 8x55
        // Fix when HW vsync is available on 8x55
        if(dev->mMDP.version == 400 || (dev->mMDP.version >= 500))
#endif
            dev->device.common.version = 0;
#ifndef NO_HW_VSYNC
        else
            dev->device.common.version = HWC_DEVICE_API_VERSION_0_3;
#endif
        dev->device.common.module  = const_cast<hw_module_t*>(module);
        dev->device.common.close   = hwc_device_close;
        dev->device.prepare        = hwc_prepare;
        dev->device.set            = hwc_set;
        dev->device.registerProcs  = hwc_registerProcs;
        dev->device.query          = hwc_query;
        dev->device.methods        = methods;
        *device                    = &dev->device.common;
        dev->device.common.tag          = HARDWARE_DEVICE_TAG;
        dev->device.common.version      = HWC_DEVICE_API_VERSION_1_1;
        dev->device.common.module       = const_cast<hw_module_t*>(module);
        dev->device.common.close        = hwc_device_close;
        dev->device.prepare             = hwc_prepare;
        dev->device.set                 = hwc_set;
        dev->device.eventControl        = hwc_eventControl;
        dev->device.blank               = hwc_blank;
        dev->device.query               = hwc_query;
        dev->device.registerProcs       = hwc_registerProcs;
        dev->device.dump                = hwc_dump;
        dev->device.getDisplayConfigs   = hwc_getDisplayConfigs;
        dev->device.getDisplayAttributes = hwc_getDisplayAttributes;
        *device = &dev->device.common;
        status = 0;
    }
    return status;
}
