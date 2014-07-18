/*
 * Copyright (C) 2012, Code Aurora Forum. All rights reserved.
 * Copyright (C) 2012-2013, The Linux Foundation. All rights reserved.
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

#include <cutils/properties.h>
#include <mdp_version.h>
#include "hwc_mdpcomp.h"
#include "hwc_qbuf.h"
#include "hwc_external.h"

#define SUPPORT_4LAYER 0

namespace qhwc {

/****** Class PipeMgr ***********/

void inline PipeMgr::reset() {
    mVGPipes = MAX_VG;
    mVGUsed = 0;
    mVGIndex = 0;
    mRGBPipes = MAX_RGB;
    mRGBUsed = 0;
    mRGBIndex = MAX_VG;
    mTotalAvail = mVGPipes + mRGBPipes;
    memset(&mStatus, 0x0 , sizeof(int)*mTotalAvail);
}

int PipeMgr::req_for_pipe(int pipe_req) {

    switch(pipe_req) {
        case PIPE_REQ_VG: //VG
            if(mVGPipes){
                mVGPipes--;
                mVGUsed++;
                mTotalAvail--;
                return PIPE_REQ_VG;
            }
        case PIPE_REQ_RGB: // RGB
            if(mRGBPipes) {
                mRGBPipes--;
                mRGBUsed++;
                mTotalAvail--;
                return PIPE_REQ_RGB;
            }
            return PIPE_NONE;
        case PIPE_REQ_FB: //FB
            if(mRGBPipes) {
               mRGBPipes--;
               mRGBUsed++;
               mTotalAvail--;
               mStatus[VAR_INDEX] = PIPE_IN_FB_MODE;
               return PIPE_REQ_FB;
           }
        default:
            break;
    };
    return PIPE_NONE;
}

int PipeMgr::assign_pipe(int pipe_pref) {
    switch(pipe_pref) {
        case PIPE_REQ_VG: //VG
            if(mVGUsed) {
                mVGUsed--;
                mStatus[mVGIndex] = PIPE_IN_COMP_MODE;
                return mVGIndex++;
            }
        case PIPE_REQ_RGB: //RGB
            if(mRGBUsed) {
                mRGBUsed--;
                mStatus[mRGBIndex] = PIPE_IN_COMP_MODE;
                return mRGBIndex++;
            }
        default:
            ALOGE("%s: PipeMgr:invalid case in pipe_mgr_assign",
                                                       __FUNCTION__);
            return -1;
    };
}

/****** Class MDPComp ***********/

MDPComp::State MDPComp::sMDPCompState = MDPCOMP_OFF;
struct MDPComp::frame_info MDPComp::sCurrentFrame;
PipeMgr MDPComp::sPipeMgr;
IdleInvalidator *MDPComp::idleInvalidator = NULL;
bool MDPComp::sIdleFallBack = false;
bool MDPComp::sDebugLogs = false;
int MDPComp::sSkipCount = 0;
int MDPComp::sMaxLayers = 0;

bool MDPComp::deinit() {
    //XXX: Tear down MDP comp state
#include "hwc_mdpcomp.h"
#include <sys/ioctl.h>
#include "external.h"
#include "qdMetaData.h"
#include "mdp_version.h"
#include <overlayRotator.h>

using overlay::Rotator;
using namespace overlay::utils;
namespace ovutils = overlay::utils;

namespace qhwc {

//==============MDPComp========================================================

IdleInvalidator *MDPComp::idleInvalidator = NULL;
bool MDPComp::sIdleFallBack = false;
bool MDPComp::sDebugLogs = false;
bool MDPComp::sEnabled = false;

MDPComp* MDPComp::getObject(const int& width) {
    if(width <= MAX_DISPLAY_DIM) {
        return new MDPCompLowRes();
    } else {
        return new MDPCompHighRes();
    }
}

void MDPComp::dump(android::String8& buf)
{
    dumpsys_log(buf, "  MDP Composition: ");
    dumpsys_log(buf, "MDPCompState=%d\n", mState);
    //XXX: Log more info
}

bool MDPComp::init(hwc_context_t *ctx) {

    if(!ctx) {
        ALOGE("%s: Invalid hwc context!!",__FUNCTION__);
        return false;
    }

    char property[PROPERTY_VALUE_MAX];

    sEnabled = false;
    if((property_get("persist.hwc.mdpcomp.enable", property, NULL) > 0) &&
            (!strncmp(property, "1", PROPERTY_VALUE_MAX ) ||
             (!strncasecmp(property,"true", PROPERTY_VALUE_MAX )))) {
        sEnabled = true;
    }

    sDebugLogs = false;
    if(property_get("debug.mdpcomp.logs", property, NULL) > 0) {
        if(atoi(property) != 0)
            sDebugLogs = true;
    }

    unsigned long idle_timeout = DEFAULT_IDLE_TIME;
    if(property_get("debug.mdpcomp.idletime", property, NULL) > 0) {
        if(atoi(property) != 0)
            idle_timeout = atoi(property);
    }

    //create Idle Invalidator
    idleInvalidator = IdleInvalidator::getInstance();

    if(idleInvalidator == NULL) {
        ALOGE("%s: failed to instantiate idleInvalidator  object", __FUNCTION__);
    } else {
        idleInvalidator->init(timeout_handler, ctx, idle_timeout);
    }
    return true;
}

void MDPComp::timeout_handler(void *udata) {
    struct hwc_context_t* ctx = (struct hwc_context_t*)(udata);

    if(!ctx) {
        ALOGE("%s: received empty data in timer callback", __FUNCTION__);
        return;
    }

    hwc_procs* proc = (hwc_procs*)ctx->device.reserved_proc[0];

    if(!proc) {
    if(!ctx->proc) {
        ALOGE("%s: HWC proc not registered", __FUNCTION__);
        return;
    }
    sIdleFallBack = true;
    /* Trigger SF to redraw the current frame */
    proc->invalidate(proc);
}

void MDPComp::reset_comp_type(hwc_layer_list_t* list) {
    for(uint32_t i = 0 ; i < list->numHwLayers; i++ ) {
        hwc_layer_t* l = &list->hwLayers[i];

        if(l->compositionType == HWC_OVERLAY)
            l->compositionType = HWC_FRAMEBUFFER;
    }
}

void MDPComp::reset( hwc_context_t *ctx, hwc_layer_list_t* list ) {
    sCurrentFrame.count = 0;
    free(sCurrentFrame.pipe_layer);
    sCurrentFrame.pipe_layer = NULL;

    //Reset MDP pipes
    sPipeMgr.reset();
    sPipeMgr.setStatus(VAR_INDEX, PIPE_IN_FB_MODE);

#if SUPPORT_4LAYER
    configure_var_pipe(ctx);
#endif

    //Reset flags and states
    unsetMDPCompLayerFlags(ctx, list);
    if(sMDPCompState == MDPCOMP_ON) {
        sMDPCompState = MDPCOMP_OFF_PENDING;
    }
}

void MDPComp::setLayerIndex(hwc_layer_t* layer, const int pipe_index)
{
    layer->flags &= ~HWC_MDPCOMP_INDEX_MASK;
    layer->flags |= pipe_index << MDPCOMP_INDEX_OFFSET;
}

int MDPComp::getLayerIndex(hwc_layer_t* layer)
{
    int byp_index = -1;

    if(layer->flags & HWC_MDPCOMP) {
        byp_index = ((layer->flags & HWC_MDPCOMP_INDEX_MASK) >>
                                               MDPCOMP_INDEX_OFFSET);
        byp_index = (byp_index < sMaxLayers ? byp_index : -1 );
    }
    return byp_index;
}
void MDPComp::print_info(hwc_layer_t* layer)
{
     hwc_rect_t sourceCrop = layer->sourceCrop;
     hwc_rect_t displayFrame = layer->displayFrame;

     int s_l = sourceCrop.left;
     int s_t = sourceCrop.top;
     int s_r = sourceCrop.right;
     int s_b = sourceCrop.bottom;

     int d_l = displayFrame.left;
     int d_t = displayFrame.top;
     int d_r = displayFrame.right;
     int d_b = displayFrame.bottom;

     ALOGD_IF(isDebug(), "src:[%d,%d,%d,%d] (%d x %d) \
                             dst:[%d,%d,%d,%d] (%d x %d)",
                             s_l, s_t, s_r, s_b, (s_r - s_l), (s_b - s_t),
                             d_l, d_t, d_r, d_b, (d_r - d_l), (d_b - d_t));
}
/*
 * Configures pipe(s) for MDP composition
 */
int MDPComp::prepare(hwc_context_t *ctx, hwc_layer_t *layer,
                                            mdp_pipe_info& mdp_info) {

    int nPipeIndex = mdp_info.index;

    if (ctx) {

        private_handle_t *hnd = (private_handle_t *)layer->handle;

        overlay::Overlay& ov = *(ctx->mOverlay);

        if(!hnd) {
            ALOGE("%s: layer handle is NULL", __FUNCTION__);
            return -1;
        }


        int hw_w = ctx->mFbDev->width;
        int hw_h = ctx->mFbDev->height;


        hwc_rect_t sourceCrop = layer->sourceCrop;
        hwc_rect_t displayFrame = layer->displayFrame;

        const int src_w = sourceCrop.right - sourceCrop.left;
        const int src_h = sourceCrop.bottom - sourceCrop.top;

        hwc_rect_t crop = sourceCrop;
        int crop_w = crop.right - crop.left;
        int crop_h = crop.bottom - crop.top;

        hwc_rect_t dst = displayFrame;
        int dst_w = dst.right - dst.left;
        int dst_h = dst.bottom - dst.top;

        //REDUNDANT ??
        if(hnd != NULL &&
               (hnd->flags & private_handle_t::PRIV_FLAGS_NONCONTIGUOUS_MEM )) {
            ALOGE("%s: failed due to non-pmem memory",__FUNCTION__);
            return -1;
        }

        if(dst.left < 0 || dst.top < 0 ||
               dst.right > hw_w || dst.bottom > hw_h) {
            ALOGD_IF(isDebug(),"%s: Destination has negative coordinates",
                                                                  __FUNCTION__);

            qhwc::calculate_crop_rects(crop, dst, hw_w, hw_h);

            //Update calulated width and height
            crop_w = crop.right - crop.left;
            crop_h = crop.bottom - crop.top;

            dst_w = dst.right - dst.left;
            dst_h = dst.bottom - dst.top;
        }

        if( (dst_w > hw_w)|| (dst_h > hw_h)) {
            ALOGD_IF(isDebug(),"%s: Dest rect exceeds FB", __FUNCTION__);
            print_info(layer);
            dst_w = hw_w;
            dst_h = hw_h;
        }

        // Determine pipe to set based on pipe index
        ovutils::eDest dest = ovutils::OV_PIPE_ALL;
        if (nPipeIndex == 0) {
            dest = ovutils::OV_PIPE0;
        } else if (nPipeIndex == 1) {
            dest = ovutils::OV_PIPE1;
        } else if (nPipeIndex == 2) {
            dest = ovutils::OV_PIPE2;
        }

        ovutils::eZorder zOrder = ovutils::ZORDER_0;

        if(mdp_info.z_order == 0 ) {
            zOrder = ovutils::ZORDER_0;
        } else if(mdp_info.z_order == 1 ) {
            zOrder = ovutils::ZORDER_1;
        } else if(mdp_info.z_order == 2 ) {
            zOrder = ovutils::ZORDER_2;
        }

        // Order order order
        // setSource - just setting source
        // setParameter - changes src w/h/f accordingly
        // setCrop - ROI - src_rect
        // setPosition - dst_rect
        // commit - commit changes to mdp driver
        // queueBuffer - not here, happens when draw is called

        ovutils::eTransform orient =
            static_cast<ovutils::eTransform>(layer->transform);

        ov.setTransform(orient, dest);
        ovutils::Whf info(hnd->width, hnd->height, hnd->format, hnd->size);
        ovutils::eMdpFlags mdpFlags = mdp_info.isVG ? ovutils::OV_MDP_PIPE_SHARE
                                                   : ovutils::OV_MDP_FLAGS_NONE;
        ovutils::eIsFg isFG = mdp_info.isFG ? ovutils::IS_FG_SET
                                                    : ovutils::IS_FG_OFF;

        if(layer->blending == HWC_BLENDING_PREMULT) {
            ovutils::setMdpFlags(mdpFlags,
                    ovutils::OV_MDP_BLEND_FG_PREMULT);
        }

        ovutils::PipeArgs parg(mdpFlags,
                               info,
                               zOrder,
                               isFG,
                               ovutils::ROT_FLAG_DISABLED);

        ovutils::PipeArgs pargs[MAX_PIPES] = { parg, parg, parg };
        if (!ov.setSource(pargs, dest)) {
            ALOGE("%s: setSource failed", __FUNCTION__);
            return -1;
        }

        ovutils::Dim dcrop(crop.left, crop.top, crop_w, crop_h);
        if (!ov.setCrop(dcrop, dest)) {
            ALOGE("%s: setCrop failed", __FUNCTION__);
            return -1;
        }

        ovutils::Dim dim(dst.left, dst.top, dst_w, dst_h);
        if (!ov.setPosition(dim, dest)) {
            ALOGE("%s: setPosition failed", __FUNCTION__);
            return -1;
        }

        ALOGD_IF(isDebug(),"%s: MDP set: crop[%d,%d,%d,%d] dst[%d,%d,%d,%d] \
                       nPipe: %d isFG: %d zorder: %d",__FUNCTION__, dcrop.x,
                       dcrop.y,dcrop.w, dcrop.h, dim.x, dim.y, dim.w, dim.h,
                       nPipeIndex,mdp_info.isFG, mdp_info.z_order);

        if (!ov.commit(dest)) {
            ALOGE("%s: commit failed", __FUNCTION__);
            return -1;
        }
    }
    return 0;
}

/*
 * MDPComp not possible when
 * 1. We have more than sMaxLayers
 * 2. External display connected
 * 3. Composition is triggered by
 *    Idle timer expiry
 * 4. Rotation is  needed
 * 5. Overlay in use
 */

bool MDPComp::is_doable(hwc_composer_device_t *dev, hwc_layer_list_t* list) {
    hwc_context_t* ctx = (hwc_context_t*)(dev);

    if(!ctx) {
        ALOGE("%s: hwc context is NULL", __FUNCTION__);
        return false;
    }

    //Number of layers
    if(list->numHwLayers < 1 || list->numHwLayers > (uint32_t) sMaxLayers) {
        ALOGD_IF(isDebug(), "%s: Unsupported number of layers",__FUNCTION__);
        return false;
    }

    //Disable MDPComp when ext display connected
    if(ctx->mExtDisplay->getExternalDisplay()|| (ctx->hdmi_pending == true)) {
        ALOGD_IF(isDebug(), "%s: External display connected.", __FUNCTION__);
    ctx->proc->invalidate(ctx->proc);
}

void MDPComp::setMDPCompLayerFlags(hwc_context_t *ctx,
        hwc_display_contents_1_t* list) {
    const int dpy = HWC_DISPLAY_PRIMARY;
    LayerProp *layerProp = ctx->layerProp[dpy];

    for(int index = 0; index < ctx->listStats[dpy].numAppLayers; index++ ) {
        hwc_layer_1_t* layer = &(list->hwLayers[index]);
        layerProp[index].mFlags |= HWC_MDPCOMP;
        layer->compositionType = HWC_OVERLAY;
        layer->hints |= HWC_HINT_CLEAR_FB;
    }
}

void MDPComp::unsetMDPCompLayerFlags(hwc_context_t* ctx,
        hwc_display_contents_1_t* list) {
    const int dpy = HWC_DISPLAY_PRIMARY;
    LayerProp *layerProp = ctx->layerProp[dpy];

    for (int index = 0 ;
            index < ctx->listStats[dpy].numAppLayers; index++) {
        if(layerProp[index].mFlags & HWC_MDPCOMP) {
            layerProp[index].mFlags &= ~HWC_MDPCOMP;
        }

        if(list->hwLayers[index].compositionType == HWC_OVERLAY) {
            list->hwLayers[index].compositionType = HWC_FRAMEBUFFER;
        }
    }
}

void MDPComp::reset(hwc_context_t *ctx,
        hwc_display_contents_1_t* list ) {
    //Reset flags and states
    unsetMDPCompLayerFlags(ctx, list);
    if(mCurrentFrame.pipeLayer) {
        for(int i = 0 ; i < mCurrentFrame.count; i++ ) {
            if(mCurrentFrame.pipeLayer[i].pipeInfo) {
                delete mCurrentFrame.pipeLayer[i].pipeInfo;
                mCurrentFrame.pipeLayer[i].pipeInfo = NULL;
                //We dont own the rotator
                mCurrentFrame.pipeLayer[i].rot = NULL;
            }
        }
        free(mCurrentFrame.pipeLayer);
        mCurrentFrame.pipeLayer = NULL;
    }
    mCurrentFrame.count = 0;
}

bool MDPComp::isValidDimension(hwc_context_t *ctx, hwc_layer_1_t *layer) {

    const int dpy = HWC_DISPLAY_PRIMARY;
    private_handle_t *hnd = (private_handle_t *)layer->handle;

    if(!hnd) {
        ALOGE("%s: layer handle is NULL", __FUNCTION__);
        return false;
    }

    int hw_w = ctx->dpyAttr[dpy].xres;
    int hw_h = ctx->dpyAttr[dpy].yres;

    hwc_rect_t sourceCrop = layer->sourceCrop;
    hwc_rect_t displayFrame = layer->displayFrame;

    hwc_rect_t crop =  sourceCrop;
    int crop_w = crop.right - crop.left;
    int crop_h = crop.bottom - crop.top;

    hwc_rect_t dst = displayFrame;
    int dst_w = dst.right - dst.left;
    int dst_h = dst.bottom - dst.top;

    if(dst.left < 0 || dst.top < 0 || dst.right > hw_w || dst.bottom > hw_h) {
       hwc_rect_t scissor = {0, 0, hw_w, hw_h };
       qhwc::calculate_crop_rects(crop, dst, scissor, layer->transform);
       crop_w = crop.right - crop.left;
       crop_h = crop.bottom - crop.top;
    }

    //Workaround for MDP HW limitation in DSI command mode panels where
    //FPS will not go beyond 30 if buffers on RGB pipes are of width < 5

    if(crop_w < 5)
        return false;

    // There is a HW limilation in MDP, minmum block size is 2x2
    // Fallback to GPU if height is less than 2.
    if(crop_h < 2)
        return false;

    return true;
}

ovutils::eDest MDPComp::getMdpPipe(hwc_context_t *ctx, ePipeType type) {
    const int dpy = HWC_DISPLAY_PRIMARY;
    overlay::Overlay& ov = *ctx->mOverlay;
    ovutils::eDest mdp_pipe = ovutils::OV_INVALID;

    switch(type) {
        case MDPCOMP_OV_DMA:
            mdp_pipe = ov.nextPipe(ovutils::OV_MDP_PIPE_DMA, dpy);
            if(mdp_pipe != ovutils::OV_INVALID) {
                ctx->mDMAInUse = true;
                return mdp_pipe;
            }
        case MDPCOMP_OV_ANY:
        case MDPCOMP_OV_RGB:
            mdp_pipe = ov.nextPipe(ovutils::OV_MDP_PIPE_RGB, dpy);
            if(mdp_pipe != ovutils::OV_INVALID) {
                return mdp_pipe;
            }

            if(type == MDPCOMP_OV_RGB) {
                //Requested only for RGB pipe
                break;
            }
        case  MDPCOMP_OV_VG:
            return ov.nextPipe(ovutils::OV_MDP_PIPE_VG, dpy);
        default:
            ALOGE("%s: Invalid pipe type",__FUNCTION__);
            return ovutils::OV_INVALID;
    };
    return ovutils::OV_INVALID;
}

bool MDPComp::isDoable(hwc_context_t *ctx,
        hwc_display_contents_1_t* list) {
    //Number of layers
    const int dpy = HWC_DISPLAY_PRIMARY;
    int numAppLayers = ctx->listStats[dpy].numAppLayers;
    int numDMAPipes = qdutils::MDPVersion::getInstance().getDMAPipes();

    overlay::Overlay& ov = *ctx->mOverlay;
    int availablePipes = ov.availablePipes(dpy);

    if(ctx->mNeedsRotator)
        availablePipes -= numDMAPipes;

    if(numAppLayers < 1 || numAppLayers > MAX_PIPES_PER_MIXER ||
                           pipesNeeded(ctx, list) > availablePipes) {
        ALOGD_IF(isDebug(), "%s: Unsupported number of layers",__FUNCTION__);
        return false;
    }

    if(ctx->mExtDispConfiguring) {
        ALOGD_IF( isDebug(),"%s: External Display connection is pending",
                __FUNCTION__);
        return false;
    }

    if(isSecuring(ctx)) {
        ALOGD_IF(isDebug(), "%s: MDP securing is active", __FUNCTION__);
        return false;
    }

    if(ctx->mSecureMode)
        return false;

    //Check for skip layers
    if(isSkipPresent(ctx, dpy)) {
        ALOGD_IF(isDebug(), "%s: Skip layers are present",__FUNCTION__);
        return false;
    }

    if(ctx->listStats[dpy].needsAlphaScale
                     && ctx->mMDP.version < qdutils::MDSS_V5) {
        ALOGD_IF(isDebug(), "%s: frame needs alpha downscaling",__FUNCTION__);
        return false;
    }

    //FB composition on idle timeout
    if(sIdleFallBack) {
        reset_comp_type(list);
        ctx->mLayerCache[dpy]->resetLayerCache(list->numHwLayers);
        sIdleFallBack = false;
        ALOGD_IF(isDebug(), "%s: idle fallback",__FUNCTION__);
        return false;
    }

    //MDP composition is not efficient if rotation is needed.
    for(unsigned int i = 0; i < list->numHwLayers; ++i) {
        if(list->hwLayers[i].transform) {
                ALOGD_IF(isDebug(), "%s: orientation involved",__FUNCTION__);
                return false;
        }
    }

    return true;
}

void MDPComp::setMDPCompLayerFlags(hwc_layer_list_t* list) {

    for(int index = 0 ; index < sCurrentFrame.count; index++ )
    {
        int layer_index = sCurrentFrame.pipe_layer[index].layer_index;
        if(layer_index >= 0) {
            hwc_layer_t* layer = &(list->hwLayers[layer_index]);

            layer->flags |= HWC_MDPCOMP;
            layer->compositionType = HWC_OVERLAY;
            layer->hints |= HWC_HINT_CLEAR_FB;
        }
    }
}

void MDPComp::get_layer_info(hwc_layer_t* layer, int& flags) {

    private_handle_t* hnd = (private_handle_t*)layer->handle;

    if(layer->flags & HWC_SKIP_LAYER) {
        flags |= MDPCOMP_LAYER_SKIP;
    } else if(hnd != NULL &&
        (hnd->flags & private_handle_t::PRIV_FLAGS_NONCONTIGUOUS_MEM )) {
        flags |= MDPCOMP_LAYER_UNSUPPORTED_MEM;
    }

    if(layer->blending != HWC_BLENDING_NONE)
        flags |= MDPCOMP_LAYER_BLEND;

    int dst_w, dst_h;
    getLayerResolution(layer, dst_w, dst_h);

    hwc_rect_t sourceCrop = layer->sourceCrop;
    const int src_w = sourceCrop.right - sourceCrop.left;
    const int src_h = sourceCrop.bottom - sourceCrop.top;
    if(((src_w > dst_w) || (src_h > dst_h))) {
        flags |= MDPCOMP_LAYER_DOWNSCALE;
    }
}

int MDPComp::mark_layers(hwc_layer_list_t* list, layer_mdp_info* layer_info,
                                                    frame_info& current_frame) {

    int layer_count = list->numHwLayers;

    if(layer_count > sMaxLayers) {
        if(!sPipeMgr.req_for_pipe(PIPE_REQ_FB)) {
            ALOGE("%s: binding var pipe to FB failed!!", __FUNCTION__);
            return 0;
        }
    }

    //Parse layers from higher z-order
    for(int index = layer_count - 1 ; index >= 0; index-- ) {
        hwc_layer_t* layer = &list->hwLayers[index];

        int layer_prop = 0;
        get_layer_info(layer, layer_prop);

        ALOGD_IF(isDebug(),"%s: prop for layer [%d]: %x", __FUNCTION__,
                                                             index, layer_prop);

        //Both in cases of NON-CONTIGUOUS memory or SKIP layer,
        //current version of mdp composition falls back completely to FB
        //composition.
        //TO DO: Support dual mode composition

        if(layer_prop & MDPCOMP_LAYER_UNSUPPORTED_MEM) {
            ALOGD_IF(isDebug(), "%s: Non contigous memory",__FUNCTION__);
            return MDPCOMP_ABORT;
        }

        if(layer_prop & MDPCOMP_LAYER_SKIP) {
            ALOGD_IF(isDebug(), "%s:skip layer",__FUNCTION__);
            return MDPCOMP_ABORT;
        }

        //Request for MDP pipes
        int pipe_pref = PIPE_REQ_VG;

        if((layer_prop & MDPCOMP_LAYER_DOWNSCALE) &&
                        (layer_prop & MDPCOMP_LAYER_BLEND)) {
            if (qdutils::MDPVersion::getInstance().getMDPVersion() >=
                    qdutils::MDP_V4_2) {
                pipe_pref = PIPE_REQ_RGB;
            } else {
                return MDPCOMP_ABORT;
            }
         }

        int allocated_pipe = sPipeMgr.req_for_pipe( pipe_pref);
        if(allocated_pipe) {
          layer_info[index].can_use_mdp = true;
          layer_info[index].pipe_pref = allocated_pipe;
          current_frame.count++;
        }else {
            ALOGE("%s: pipe marking in mark layer fails for : %d",
                                          __FUNCTION__, allocated_pipe);
            return MDPCOMP_FAILURE;
        }
    }
    return MDPCOMP_SUCCESS;
}

void MDPComp::reset_layer_mdp_info(layer_mdp_info* layer_info, int count) {
    for(int i = 0 ; i < count; i++ ) {
        layer_info[i].can_use_mdp = false;
        layer_info[i].pipe_pref = PIPE_NONE;
    }
}

bool MDPComp::alloc_layer_pipes(hwc_layer_list_t* list,
                        layer_mdp_info* layer_info, frame_info& current_frame) {

    int layer_count = list->numHwLayers;
    int mdp_count = current_frame.count;
    int fallback_count = layer_count - mdp_count;
    int frame_pipe_count = 0;

    ALOGD_IF(isDebug(), "%s:  dual mode: %d  total count: %d \
                                mdp count: %d fallback count: %d",
                            __FUNCTION__, (layer_count != mdp_count),
                            layer_count, mdp_count, fallback_count);

    for(int index = 0 ; index < layer_count ; index++ ) {
        hwc_layer_t* layer = &list->hwLayers[index];

        if(layer_info[index].can_use_mdp) {
             pipe_layer_pair& info = current_frame.pipe_layer[frame_pipe_count];
             mdp_pipe_info& pipe_info = info.pipe_index;

             pipe_info.index = sPipeMgr.assign_pipe(layer_info[index].pipe_pref);
             pipe_info.isVG = (layer_info[index].pipe_pref == PIPE_REQ_VG);
             pipe_info.isFG = (frame_pipe_count == 0);
             /* if VAR pipe is attached to FB, FB will be updated with
                VSYNC WAIT flag, so no need to set VSYNC WAIT for any
                bypass pipes. if not, set VSYNC WAIT to the last updating pipe*/
             pipe_info.vsync_wait =
                 (sPipeMgr.getStatus(VAR_INDEX) == PIPE_IN_FB_MODE) ? false:
                                      (frame_pipe_count == (mdp_count - 1));
             /* All the layers composed on FB will have MDP zorder 0, so start
                assigning from  1*/
                pipe_info.z_order = index -
                        (fallback_count ? fallback_count - 1 : fallback_count);

             info.layer_index = index;
             frame_pipe_count++;
        }
    }
    return 1;
}

//returns array of layers and their allocated pipes
bool MDPComp::parse_and_allocate(hwc_context_t* ctx, hwc_layer_list_t* list,
                                                  frame_info& current_frame ) {

    int layer_count = list->numHwLayers;

    /* clear pipe status */
    sPipeMgr.reset();

    layer_mdp_info* bp_layer_info = (layer_mdp_info*)
                                   malloc(sizeof(layer_mdp_info)* layer_count);

    reset_layer_mdp_info(bp_layer_info, layer_count);

    /* iterate through layer list to mark candidate */
    if(mark_layers(list, bp_layer_info, current_frame) == MDPCOMP_ABORT) {
        free(bp_layer_info);
        current_frame.count = 0;
        ALOGE_IF(isDebug(), "%s:mark_layers failed!!", __FUNCTION__);
        return false;
    }
    current_frame.pipe_layer = (pipe_layer_pair*)
                          malloc(sizeof(pipe_layer_pair) * current_frame.count);

    /* allocate MDP pipes for marked layers */
    alloc_layer_pipes( list, bp_layer_info, current_frame);

    free(bp_layer_info);
    return true;
}
#if SUPPORT_4LAYER
int MDPComp::configure_var_pipe(hwc_context_t* ctx) {

    if(!ctx) {
       ALOGE("%s: invalid context", __FUNCTION__);
       return -1;
    }

    framebuffer_device_t *fbDev = ctx->fbDev;
    if (!fbDev) {
        ALOGE("%s: fbDev is NULL", __FUNCTION__);
        return -1;
    }

    int new_mode = -1, cur_mode;
    fbDev->perform(fbDev,EVENT_GET_VAR_PIPE_MODE, (void*)&cur_mode);

    if(sPipeMgr.getStatus(VAR_INDEX) == PIPE_IN_FB_MODE) {
        new_mode = VAR_PIPE_FB_ATTACH;
    } else if(sPipeMgr.getStatus(VAR_INDEX) == PIPE_IN_BYP_MODE) {
        new_mode = VAR_PIPE_FB_DETACH;
        fbDev->perform(fbDev,EVENT_WAIT_POSTBUFFER,NULL);
    }

    ALOGD_IF(isDebug(),"%s: old_mode: %d new_mode: %d", __FUNCTION__,
                                                      cur_mode, new_mode);

    if((new_mode != cur_mode) && (new_mode >= 0)) {
       if(fbDev->perform(fbDev,EVENT_SET_VAR_PIPE_MODE,(void*)&new_mode) < 0) {
           ALOGE("%s: Setting var pipe mode failed", __FUNCTION__);
       }
    }

    return 0;
}
#endif

bool MDPComp::setup(hwc_context_t* ctx, hwc_layer_list_t* list) {
    int nPipeIndex, vsync_wait, isFG;
    int numHwLayers = list->numHwLayers;

    frame_info &current_frame = sCurrentFrame;
    current_frame.count = 0;

    if(!ctx) {
       ALOGE("%s: invalid context", __FUNCTION__);
       return -1;
    }

    framebuffer_device_t *fbDev = ctx->mFbDev;
    if (!fbDev) {
        ALOGE("%s: fbDev is NULL", __FUNCTION__);
        return -1;
    }

    if(!parse_and_allocate(ctx, list, current_frame)) {
#if SUPPORT_4LAYER
       int mode = VAR_PIPE_FB_ATTACH;
       if(fbDev->perform(fbDev,EVENT_SET_VAR_PIPE_MODE,(void*)&mode) < 0 ) {
           ALOGE("%s: setting var pipe mode failed", __FUNCTION__);
       }
#endif
       ALOGD_IF(isDebug(), "%s: Falling back to FB", __FUNCTION__);
       return false;
    }
#if SUPPORT_4LAYER
    configure_var_pipe(ctx);
#endif

    overlay::Overlay& ov = *(ctx->mOverlay);
    ovutils::eOverlayState state = ov.getState();

    if (current_frame.count == 1) {
         state = ovutils::OV_BYPASS_1_LAYER;
    } else if (current_frame.count == 2) {
         state = ovutils::OV_BYPASS_2_LAYER;
    } else if (current_frame.count == 3) {
         state = ovutils::OV_BYPASS_3_LAYER;
   }

      ov.setState(state);


    for (int index = 0 ; index < current_frame.count; index++) {
        int layer_index = current_frame.pipe_layer[index].layer_index;
        hwc_layer_t* layer = &list->hwLayers[layer_index];
        mdp_pipe_info& cur_pipe = current_frame.pipe_layer[index].pipe_index;

        if( prepare(ctx, layer, cur_pipe) != 0 ) {
           ALOGD_IF(isDebug(), "%s: MDPComp failed to configure overlay for \
                                    layer %d with pipe index:%d",__FUNCTION__,
                                    index, cur_pipe.index);
           return false;
         } else {
            setLayerIndex(layer, index);
         }
    }
    return true;
}

void MDPComp::unsetMDPCompLayerFlags(hwc_context_t* ctx, hwc_layer_list_t* list)
{
    if (!list)
        return;

    for (int index = 0 ; index < sCurrentFrame.count; index++) {
        int l_index = sCurrentFrame.pipe_layer[index].layer_index;
        if(list->hwLayers[l_index].flags & HWC_MDPCOMP) {
            list->hwLayers[l_index].flags &= ~HWC_MDPCOMP;
        }
    }
}

int MDPComp::draw(hwc_context_t *ctx, hwc_layer_list_t* list) {

    if(!isEnabled()) {
        ALOGD_IF(isDebug(),"%s: MDP Comp. not enabled",__FUNCTION__);
        return 0;
     }

    if(!ctx || !list) {
        ALOGE("%s: invalid contxt or list",__FUNCTION__);
        return -1;
    }

    overlay::Overlay& ov = *(ctx->mOverlay);

    for(unsigned int i = 0; i < list->numHwLayers; i++ )
    {
        hwc_layer_t *layer = &list->hwLayers[i];

        if(!(layer->flags & HWC_MDPCOMP)) {
            ALOGD_IF(isDebug(), "%s: Layer Not flagged for MDP comp",
                                                                __FUNCTION__);
            continue;
        }

        int data_index = getLayerIndex(layer);
        mdp_pipe_info& pipe_info =
                          sCurrentFrame.pipe_layer[data_index].pipe_index;
        int index = pipe_info.index;

        if(index < 0) {
            ALOGE("%s: Invalid pipe index (%d)", __FUNCTION__, index);
            return -1;
        }

        /* reset Invalidator */
        if(idleInvalidator)
        idleInvalidator->markForSleep();

        ovutils::eDest dest;

        if (index == 0) {
            dest = ovutils::OV_PIPE0;
        } else if (index == 1) {
            dest = ovutils::OV_PIPE1;
        } else if (index == 2) {
            dest = ovutils::OV_PIPE2;
        }

        if (ctx ) {
            private_handle_t *hnd = (private_handle_t *)layer->handle;
            if(!hnd) {
                ALOGE("%s handle null", __FUNCTION__);
                return -1;
            }

            //lock buffer before queue
            //XXX: Handle lock failure
            if (ctx->swapInterval != 0) {
                ctx->qbuf->lockAndAdd(hnd);
            }

            ALOGD_IF(isDebug(),"%s: MDP Comp: Drawing layer: %p hnd: %p \
                                 using  pipe: %d", __FUNCTION__, layer,
                                 hnd, index );

            if (!ov.queueBuffer(hnd->fd, hnd->offset, dest)) {
                ALOGE("%s: queueBuffer failed for external", __FUNCTION__);
                return -1;
            }
        }
        layer->flags &= ~HWC_MDPCOMP;
        layer->flags |= HWC_MDPCOMP_INDEX_MASK;
    }
    return 0;
}

bool MDPComp::init(hwc_context_t *dev) {

    if(!dev) {
        ALOGE("%s: Invalid hwc context!!",__FUNCTION__);
        return false;
    }

#if SUPPORT_4LAYER
    if(MAX_MDPCOMP_LAYERS > MAX_STATIC_PIPES) {
        framebuffer_device_t *fbDev = dev->fbDevice;
        if(fbDev == NULL) {
            ALOGE("%s: FATAL: framebuffer device is NULL", __FUNCTION__);
            return false;
        }

        //Receive VAR pipe object from framebuffer
        if(fbDev->perform(fbDev,EVENT_GET_VAR_PIPE,(void*)&ov) < 0) {
            ALOGE("%s: FATAL: getVariablePipe failed!!", __FUNCTION__);
            return false;
        }

        sPipeMgr.setStatus(VAR_INDEX, PIPE_IN_FB_MODE);
    }
#endif
    char property[PROPERTY_VALUE_MAX];

    sMaxLayers = 0;
    if(property_get("debug.mdpcomp.maxlayer", property, NULL) > 0) {
        if(atoi(property) != 0)
           sMaxLayers = atoi(property);
    }

    sDebugLogs = false;
    if(property_get("debug.mdpcomp.logs", property, NULL) > 0) {
        if(atoi(property) != 0)
           sDebugLogs = true;
    }

    unsigned long idle_timeout = DEFAULT_IDLE_TIME;
    if(property_get("debug.mdpcomp.idletime", property, NULL) > 0) {
        if(atoi(property) != 0)
           idle_timeout = atoi(property);
    }

    //create Idle Invalidator
    idleInvalidator = IdleInvalidator::getInstance();

    if(idleInvalidator == NULL) {
       ALOGE("%s: failed to instantiate idleInvalidator  object", __FUNCTION__);
    } else {
       idleInvalidator->init(timeout_handler, dev, idle_timeout);
    }
    return true;
}

bool MDPComp::configure(hwc_composer_device_t *dev,  hwc_layer_list_t* list) {

    if(!isEnabled()) {
        ALOGD_IF(isDebug(),"%s: MDP Comp. not enabled.", __FUNCTION__);
        return false;
    }

    hwc_context_t* ctx = (hwc_context_t*)(dev);

    bool isMDPCompUsed = true;
    bool doable = is_doable(dev, list);

    if(doable) {
        char value[PROPERTY_VALUE_MAX];
        property_get("debug.egl.swapinterval", value, "1");
        ctx->swapInterval = atoi(value);

        if(setup(ctx, list)) {
            setMDPCompLayerFlags(list);
            sMDPCompState = MDPCOMP_ON;
        } else {
            ALOGD_IF(isDebug(),"%s: MDP Comp Failed",__FUNCTION__);
            isMDPCompUsed = false;
        }
     } else {
        ALOGD_IF( isDebug(),"%s: MDP Comp not possible[%d]",__FUNCTION__,
                   doable);
        isMDPCompUsed = false;
     }

     //Reset states
     if(!isMDPCompUsed) {
        //Reset current frame
         reset(ctx, list);
     }

     sIdleFallBack = false;

     return isMDPCompUsed;
}
    if(ctx->mNeedsRotator && ctx->mDMAInUse) {
        ALOGD_IF(isDebug(), "%s: DMA not available for Rotator",__FUNCTION__);
        return false;
    }

    //MDP composition is not efficient if layer needs rotator.
    for(int i = 0; i < numAppLayers; ++i) {
        // As MDP h/w supports flip operation, use MDP comp only for
        // 180 transforms. Fail for any transform involving 90 (90, 270).
        hwc_layer_1_t* layer = &list->hwLayers[i];
        private_handle_t *hnd = (private_handle_t *)layer->handle;

        if(layer->transform & HWC_TRANSFORM_ROT_90 && !isYuvBuffer(hnd)) {
            ALOGD_IF(isDebug(), "%s: orientation involved",__FUNCTION__);
            return false;
        }

        if(!isYuvBuffer(hnd) && !isValidDimension(ctx,layer)) {
            ALOGD_IF(isDebug(), "%s: Buffer is of invalid width",__FUNCTION__);
            return false;
        }
    }
    return true;
}

bool MDPComp::setup(hwc_context_t* ctx, hwc_display_contents_1_t* list) {
    const int dpy = HWC_DISPLAY_PRIMARY;
    if(!ctx) {
        ALOGE("%s: invalid context", __FUNCTION__);
        return -1;
    }

    ctx->mDMAInUse = false;
    if(!allocLayerPipes(ctx, list, mCurrentFrame)) {
        ALOGD_IF(isDebug(), "%s: Falling back to FB", __FUNCTION__);
        return false;
    }

    for (int index = 0 ; index < mCurrentFrame.count; index++) {
        hwc_layer_1_t* layer = &list->hwLayers[index];
        if(configure(ctx, layer, mCurrentFrame.pipeLayer[index]) != 0 ) {
            ALOGD_IF(isDebug(), "%s: MDPComp failed to configure overlay for \
                    layer %d",__FUNCTION__, index);
            return false;
        }
    }
    return true;
}

bool MDPComp::prepare(hwc_context_t *ctx,
        hwc_display_contents_1_t* list) {
    if(!isEnabled()) {
        ALOGE_IF(isDebug(),"%s: MDP Comp. not enabled.", __FUNCTION__);
        return false;
    }

    overlay::Overlay& ov = *ctx->mOverlay;
    bool isMDPCompUsed = true;

    //reset old data
    reset(ctx, list);

    bool doable = isDoable(ctx, list);
    if(doable) {
        if(setup(ctx, list)) {
            setMDPCompLayerFlags(ctx, list);
        } else {
            ALOGD_IF(isDebug(),"%s: MDP Comp Failed",__FUNCTION__);
            isMDPCompUsed = false;
        }
    } else {
        ALOGD_IF( isDebug(),"%s: MDP Comp not possible[%d]",__FUNCTION__,
                doable);
        isMDPCompUsed = false;
    }

    //Reset states
    if(!isMDPCompUsed) {
        //Reset current frame
        reset(ctx, list);
    }

    mState = isMDPCompUsed ? MDPCOMP_ON : MDPCOMP_OFF;
    return isMDPCompUsed;
}

//=============MDPCompLowRes===================================================

/*
 * Configures pipe(s) for MDP composition
 */
int MDPCompLowRes::configure(hwc_context_t *ctx, hwc_layer_1_t *layer,
        PipeLayerPair& pipeLayerPair) {
    const int dpy = HWC_DISPLAY_PRIMARY;
    MdpPipeInfoLowRes& mdp_info =
            *(static_cast<MdpPipeInfoLowRes*>(pipeLayerPair.pipeInfo));
    eMdpFlags mdpFlags = OV_MDP_BACKEND_COMPOSITION;
    eZorder zOrder = static_cast<eZorder>(mdp_info.zOrder);
    eIsFg isFg = (zOrder == ovutils::ZORDER_0)?IS_FG_SET:IS_FG_OFF;
    eDest dest = mdp_info.index;

    return configureLowRes(ctx, layer, dpy, mdpFlags, zOrder, isFg, dest,
            &pipeLayerPair.rot);
}

int MDPCompLowRes::pipesNeeded(hwc_context_t *ctx,
                        hwc_display_contents_1_t* list) {
    const int dpy = HWC_DISPLAY_PRIMARY;
    return ctx->listStats[dpy].numAppLayers;
}

bool MDPCompLowRes::allocLayerPipes(hwc_context_t *ctx,
        hwc_display_contents_1_t* list,
        FrameInfo& currentFrame) {
    const int dpy = HWC_DISPLAY_PRIMARY;
    overlay::Overlay& ov = *ctx->mOverlay;
    int layer_count = ctx->listStats[dpy].numAppLayers;

    currentFrame.count = layer_count;
    currentFrame.pipeLayer = (PipeLayerPair*)
            malloc(sizeof(PipeLayerPair) * currentFrame.count);

    if(isYuvPresent(ctx, dpy)) {
        int nYuvCount = ctx->listStats[dpy].yuvCount;

        for(int index = 0; index < nYuvCount; index ++) {
            int nYuvIndex = ctx->listStats[dpy].yuvIndices[index];
            hwc_layer_1_t* layer = &list->hwLayers[nYuvIndex];
            PipeLayerPair& info = currentFrame.pipeLayer[nYuvIndex];
            info.pipeInfo = new MdpPipeInfoLowRes;
            info.rot = NULL;
            MdpPipeInfoLowRes& pipe_info = *(MdpPipeInfoLowRes*)info.pipeInfo;
            pipe_info.index = getMdpPipe(ctx, MDPCOMP_OV_VG);
            if(pipe_info.index == ovutils::OV_INVALID) {
                ALOGD_IF(isDebug(), "%s: Unable to get pipe for Videos",
                        __FUNCTION__);
                return false;
            }
            pipe_info.zOrder = nYuvIndex;
        }
    }

    for(int index = 0 ; index < layer_count ; index++ ) {
        hwc_layer_1_t* layer = &list->hwLayers[index];
        private_handle_t *hnd = (private_handle_t *)layer->handle;

        if(isYuvBuffer(hnd))
            continue;

        PipeLayerPair& info = currentFrame.pipeLayer[index];
        info.pipeInfo = new MdpPipeInfoLowRes;
        info.rot = NULL;
        MdpPipeInfoLowRes& pipe_info = *(MdpPipeInfoLowRes*)info.pipeInfo;

        ePipeType type = MDPCOMP_OV_ANY;

        if(!qhwc::needsScaling(layer) && !ctx->mNeedsRotator
                             && ctx->mMDP.version >= qdutils::MDSS_V5) {
            type = MDPCOMP_OV_DMA;
        }

        pipe_info.index = getMdpPipe(ctx, type);
        if(pipe_info.index == ovutils::OV_INVALID) {
            ALOGD_IF(isDebug(), "%s: Unable to get pipe for UI", __FUNCTION__);
            return false;
        }
        pipe_info.zOrder = index;
    }
    return true;
}

bool MDPCompLowRes::draw(hwc_context_t *ctx, hwc_display_contents_1_t* list) {

    if(!isEnabled() || !isUsed()) {
        ALOGD_IF(isDebug(),"%s: MDP Comp not configured", __FUNCTION__);
        return true;
    }

    if(!ctx || !list) {
        ALOGE("%s: invalid contxt or list",__FUNCTION__);
        return false;
    }

    /* reset Invalidator */
    if(idleInvalidator)
        idleInvalidator->markForSleep();

    const int dpy = HWC_DISPLAY_PRIMARY;
    overlay::Overlay& ov = *ctx->mOverlay;
    LayerProp *layerProp = ctx->layerProp[dpy];

    int numHwLayers = ctx->listStats[dpy].numAppLayers;
    for(int i = 0; i < numHwLayers; i++ )
    {
        hwc_layer_1_t *layer = &list->hwLayers[i];
        private_handle_t *hnd = (private_handle_t *)layer->handle;
        if(!hnd) {
            ALOGE("%s handle null", __FUNCTION__);
            return false;
        }

        MdpPipeInfoLowRes& pipe_info =
                *(MdpPipeInfoLowRes*)mCurrentFrame.pipeLayer[i].pipeInfo;
        ovutils::eDest dest = pipe_info.index;
        if(dest == ovutils::OV_INVALID) {
            ALOGE("%s: Invalid pipe index (%d)", __FUNCTION__, dest);
            return false;
        }

        if(!(layerProp[i].mFlags & HWC_MDPCOMP)) {
            continue;
        }

        ALOGD_IF(isDebug(),"%s: MDP Comp: Drawing layer: %p hnd: %p \
                using  pipe: %d", __FUNCTION__, layer,
                hnd, dest );

        int fd = hnd->fd;
        uint32_t offset = hnd->offset;
        Rotator *rot = mCurrentFrame.pipeLayer[i].rot;
        if(rot) {
            if(!rot->queueBuffer(fd, offset))
                return false;
            fd = rot->getDstMemId();
            offset = rot->getDstOffset();
        }

        if (!ov.queueBuffer(fd, offset, dest)) {
            ALOGE("%s: queueBuffer failed for external", __FUNCTION__);
            return false;
        }

        layerProp[i].mFlags &= ~HWC_MDPCOMP;
    }
    return true;
}

//=============MDPCompHighRes===================================================

int MDPCompHighRes::pipesNeeded(hwc_context_t *ctx,
                        hwc_display_contents_1_t* list) {
    const int dpy = HWC_DISPLAY_PRIMARY;
    int numAppLayers = ctx->listStats[dpy].numAppLayers;
    int pipesNeeded = 0;

    int hw_w = ctx->dpyAttr[dpy].xres;

    for(int i = 0; i < numAppLayers; ++i) {
        hwc_layer_1_t* layer = &list->hwLayers[i];
        hwc_rect_t dst = layer->displayFrame;
      if(dst.left > hw_w/2) {
          pipesNeeded++;
      } else if(dst.right <= hw_w/2) {
          pipesNeeded++;
      } else {
          pipesNeeded += 2;
      }
    }
    return pipesNeeded;
}

bool MDPCompHighRes::acquireMDPPipes(hwc_context_t *ctx, hwc_layer_1_t* layer,
                        MdpPipeInfoHighRes& pipe_info, ePipeType type) {
     const int dpy = HWC_DISPLAY_PRIMARY;
     int hw_w = ctx->dpyAttr[dpy].xres;

     hwc_rect_t dst = layer->displayFrame;
     if(dst.left > hw_w/2) {
         pipe_info.lIndex = ovutils::OV_INVALID;
         pipe_info.rIndex = getMdpPipe(ctx, type);
         if(pipe_info.rIndex == ovutils::OV_INVALID)
             return false;
     } else if (dst.right <= hw_w/2) {
         pipe_info.rIndex = ovutils::OV_INVALID;
         pipe_info.lIndex = getMdpPipe(ctx, type);
         if(pipe_info.lIndex == ovutils::OV_INVALID)
             return false;
     } else {
         pipe_info.rIndex = getMdpPipe(ctx, type);
         pipe_info.lIndex = getMdpPipe(ctx, type);
         if(pipe_info.rIndex == ovutils::OV_INVALID ||
            pipe_info.lIndex == ovutils::OV_INVALID)
             return false;
     }
     return true;
}

bool MDPCompHighRes::allocLayerPipes(hwc_context_t *ctx,
        hwc_display_contents_1_t* list,
        FrameInfo& currentFrame) {
    const int dpy = HWC_DISPLAY_PRIMARY;
    overlay::Overlay& ov = *ctx->mOverlay;
    int layer_count = ctx->listStats[dpy].numAppLayers;

    currentFrame.count = layer_count;
    currentFrame.pipeLayer = (PipeLayerPair*)
            malloc(sizeof(PipeLayerPair) * currentFrame.count);

    if(isYuvPresent(ctx, dpy)) {
        int nYuvCount = ctx->listStats[dpy].yuvCount;

        for(int index = 0; index < nYuvCount; index ++) {
            int nYuvIndex = ctx->listStats[dpy].yuvIndices[index];
            hwc_layer_1_t* layer = &list->hwLayers[nYuvIndex];
            PipeLayerPair& info = currentFrame.pipeLayer[nYuvIndex];
            info.pipeInfo = new MdpPipeInfoHighRes;
            MdpPipeInfoHighRes& pipe_info = *(MdpPipeInfoHighRes*)info.pipeInfo;
            if(!acquireMDPPipes(ctx, layer, pipe_info,MDPCOMP_OV_VG)) {
                ALOGD_IF(isDebug(),"%s: Unable to get pipe for videos",
                                                            __FUNCTION__);
                //TODO: windback pipebook data on fail
                return false;
            }
            pipe_info.zOrder = nYuvIndex;
        }
    }

    for(int index = 0 ; index < layer_count ; index++ ) {
        hwc_layer_1_t* layer = &list->hwLayers[index];
        private_handle_t *hnd = (private_handle_t *)layer->handle;

        if(isYuvBuffer(hnd))
            continue;

        PipeLayerPair& info = currentFrame.pipeLayer[index];
        info.pipeInfo = new MdpPipeInfoHighRes;
        MdpPipeInfoHighRes& pipe_info = *(MdpPipeInfoHighRes*)info.pipeInfo;

        ePipeType type = MDPCOMP_OV_ANY;

        if(!qhwc::needsScaling(layer) && !ctx->mNeedsRotator
                             && ctx->mMDP.version >= qdutils::MDSS_V5)
            type = MDPCOMP_OV_DMA;

        if(!acquireMDPPipes(ctx, layer, pipe_info, type)) {
            ALOGD_IF(isDebug(), "%s: Unable to get pipe for UI", __FUNCTION__);
            //TODO: windback pipebook data on fail
            return false;
        }
        pipe_info.zOrder = index;
    }
    return true;
}
/*
 * Configures pipe(s) for MDP composition
 */
int MDPCompHighRes::configure(hwc_context_t *ctx, hwc_layer_1_t *layer,
        PipeLayerPair& pipeLayerPair) {
    const int dpy = HWC_DISPLAY_PRIMARY;
    MdpPipeInfoHighRes& mdp_info =
            *(static_cast<MdpPipeInfoHighRes*>(pipeLayerPair.pipeInfo));
    eZorder zOrder = static_cast<eZorder>(mdp_info.zOrder);
    eIsFg isFg = (zOrder == ovutils::ZORDER_0)?IS_FG_SET:IS_FG_OFF;
    eMdpFlags mdpFlagsL = OV_MDP_BACKEND_COMPOSITION;
    eDest lDest = mdp_info.lIndex;
    eDest rDest = mdp_info.rIndex;
    return configureHighRes(ctx, layer, dpy, mdpFlagsL, zOrder, isFg, lDest,
            rDest, &pipeLayerPair.rot);
}

bool MDPCompHighRes::draw(hwc_context_t *ctx, hwc_display_contents_1_t* list) {

    if(!isEnabled() || !isUsed()) {
        ALOGD_IF(isDebug(),"%s: MDP Comp not configured", __FUNCTION__);
        return true;
    }

    if(!ctx || !list) {
        ALOGE("%s: invalid contxt or list",__FUNCTION__);
        return false;
    }

    /* reset Invalidator */
    if(idleInvalidator)
        idleInvalidator->markForSleep();

    const int dpy = HWC_DISPLAY_PRIMARY;
    overlay::Overlay& ov = *ctx->mOverlay;
    LayerProp *layerProp = ctx->layerProp[dpy];

    int numHwLayers = ctx->listStats[dpy].numAppLayers;
    for(int i = 0; i < numHwLayers; i++ )
    {
        hwc_layer_1_t *layer = &list->hwLayers[i];
        private_handle_t *hnd = (private_handle_t *)layer->handle;
        if(!hnd) {
            ALOGE("%s handle null", __FUNCTION__);
            return false;
        }

        if(!(layerProp[i].mFlags & HWC_MDPCOMP)) {
            continue;
        }

        MdpPipeInfoHighRes& pipe_info =
                *(MdpPipeInfoHighRes*)mCurrentFrame.pipeLayer[i].pipeInfo;
        Rotator *rot = mCurrentFrame.pipeLayer[i].rot;

        ovutils::eDest indexL = pipe_info.lIndex;
        ovutils::eDest indexR = pipe_info.rIndex;
        int fd = hnd->fd;
        int offset = hnd->offset;

        if(rot) {
            rot->queueBuffer(fd, offset);
            fd = rot->getDstMemId();
            offset = rot->getDstOffset();
        }

        //************* play left mixer **********
        if(indexL != ovutils::OV_INVALID) {
            ovutils::eDest destL = (ovutils::eDest)indexL;
            ALOGD_IF(isDebug(),"%s: MDP Comp: Drawing layer: %p hnd: %p \
                    using  pipe: %d", __FUNCTION__, layer, hnd, indexL );
            if (!ov.queueBuffer(fd, offset, destL)) {
                ALOGE("%s: queueBuffer failed for left mixer", __FUNCTION__);
                return false;
            }
        }

        //************* play right mixer **********
        if(indexR != ovutils::OV_INVALID) {
            ovutils::eDest destR = (ovutils::eDest)indexR;
            ALOGD_IF(isDebug(),"%s: MDP Comp: Drawing layer: %p hnd: %p \
                    using  pipe: %d", __FUNCTION__, layer, hnd, indexR );
            if (!ov.queueBuffer(fd, offset, destR)) {
                ALOGE("%s: queueBuffer failed for right mixer", __FUNCTION__);
                return false;
            }
        }

        layerProp[i].mFlags &= ~HWC_MDPCOMP;
    }

    return true;
}
}; //namespace

