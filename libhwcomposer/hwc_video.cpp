/*
 * Copyright (C) 2010 The Android Open Source Project
 * Copyright (C) 2012, Code Aurora Forum. All rights reserved.
 * Copyright (C) 2012-2013, The Linux Foundation. All rights reserved.
 *
 * Not a Contribution, Apache license notifications and license are retained for
 * attribution purposes only
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

#define VIDEO_DEBUG 0
#include <overlay.h>
#include "hwc_qbuf.h"
#include "hwc_video.h"
#include "hwc_external.h"
#include "qdMetaData.h"

namespace qhwc {

//Static Members
ovutils::eOverlayState VideoOverlay::sState = ovutils::OV_CLOSED;
int VideoOverlay::sYuvCount = 0;
int VideoOverlay::sYuvLayerIndex = -1;
bool VideoOverlay::sIsYuvLayerSkip = false;
int VideoOverlay::sCCLayerIndex = -1;
bool VideoOverlay::sIsModeOn = false;

//Cache stats, figure out the state, config overlay
bool VideoOverlay::prepare(hwc_context_t *ctx, hwc_layer_list_t *list) {
    sIsModeOn = false;
    if(!ctx->mMDP.hasOverlay) {
       ALOGD_IF(VIDEO_DEBUG,"%s, this hw doesnt support overlay", __FUNCTION__);
       return false;
    }
    if(sYuvLayerIndex == -1) {
        return false;
    }
    chooseState(ctx);
    //if the state chosen above is CLOSED, skip this block.
    if(sState != ovutils::OV_CLOSED) {
        hwc_layer_t *yuvLayer = &list->hwLayers[sYuvLayerIndex];
        hwc_layer_t *ccLayer = NULL;
        if(sCCLayerIndex != -1)
            ccLayer = &list->hwLayers[sCCLayerIndex];

        if(configure(ctx, yuvLayer, ccLayer)) {
            markFlags(&list->hwLayers[sYuvLayerIndex]);
            sIsModeOn = true;
        }
    }

    ALOGD_IF(VIDEO_DEBUG, "%s: stats: yuvCount = %d, yuvIndex = %d,"
            "IsYuvLayerSkip = %d, ccLayerIndex = %d, IsModeOn = %d",
            __FUNCTION__, sYuvCount, sYuvLayerIndex,
            sIsYuvLayerSkip, sCCLayerIndex, sIsModeOn);

    return sIsModeOn;
}

void VideoOverlay::chooseState(hwc_context_t *ctx) {
    ALOGD_IF(VIDEO_DEBUG, "%s: old state = %s", __FUNCTION__,
            ovutils::getStateString(sState));

    ovutils::eOverlayState newState = ovutils::OV_CLOSED;

    //Support 1 video layer
    if(sYuvCount == 1) {
        //Skip on primary, display on ext.
        if(sIsYuvLayerSkip && ctx->mExtDisplay->getExternalDisplay()) {
            newState = ovutils::OV_2D_VIDEO_ON_TV;
        } else if(sIsYuvLayerSkip) { //skip on primary, no ext
            newState = ovutils::OV_CLOSED;
        } else if(ctx->mExtDisplay->getExternalDisplay()) {
            //display on both
            newState = ovutils::OV_2D_VIDEO_ON_PANEL_TV;
        } else { //display on primary only
            newState = ovutils::OV_2D_VIDEO_ON_PANEL;
        }
    }
    sState = newState;
    ALOGD_IF(VIDEO_DEBUG, "%s: new chosen state = %s", __FUNCTION__,
            ovutils::getStateString(sState));
}

void VideoOverlay::markFlags(hwc_layer_t *layer) {
    switch(sState) {
        case ovutils::OV_2D_VIDEO_ON_PANEL:
        case ovutils::OV_2D_VIDEO_ON_PANEL_TV:
            layer->compositionType = HWC_OVERLAY;
            layer->hints |= HWC_HINT_CLEAR_FB;
            break;
        case ovutils::OV_2D_VIDEO_ON_TV:
            break; //dont update flags.
        default:
            break;
    }
}

/* Helpers */
bool configPrimVid(hwc_context_t *ctx, hwc_layer_t *layer) {
    overlay::Overlay& ov = *(ctx->mOverlay);
    private_handle_t *hnd = (private_handle_t *)layer->handle;
    ovutils::Whf info(hnd->width, hnd->height, hnd->format, hnd->size);

    ovutils::eMdpFlags mdpFlags = ovutils::OV_MDP_FLAGS_NONE;
    if (hnd->flags & private_handle_t::PRIV_FLAGS_SECURE_BUFFER) {
        ovutils::setMdpFlags(mdpFlags,
                ovutils::OV_MDP_SECURE_OVERLAY_SESSION);
    }
    if(layer->blending == HWC_BLENDING_PREMULT) {
        ovutils::setMdpFlags(mdpFlags,
                ovutils::OV_MDP_BLEND_FG_PREMULT);
    }
    MetaData_t *metadata = (MetaData_t *)hnd->base_metadata;
    if ((metadata->operation & PP_PARAM_INTERLACED) && metadata->interlaced) {
        ovutils::setMdpFlags(mdpFlags, ovutils::OV_MDP_DEINTERLACE);
    }

    ovutils::eIsFg isFgFlag = ovutils::IS_FG_OFF;
    if (ctx->numHwLayers == 1) {
        isFgFlag = ovutils::IS_FG_SET;
    }

    ovutils::PipeArgs parg(mdpFlags,
            info,
            ovutils::ZORDER_0,
            isFgFlag,
            ovutils::ROT_FLAG_DISABLED);
    ovutils::PipeArgs pargs[ovutils::MAX_PIPES] = { parg, parg, parg };
    ov.setSource(pargs, ovutils::OV_PIPE0);

    hwc_rect_t sourceCrop = layer->sourceCrop;
    // x,y,w,h
    ovutils::Dim dcrop(sourceCrop.left, sourceCrop.top,
            sourceCrop.right - sourceCrop.left,
            sourceCrop.bottom - sourceCrop.top);

    ovutils::Dim dpos;
    hwc_rect_t displayFrame = layer->displayFrame;
    dpos.x = displayFrame.left;
    dpos.y = displayFrame.top;
    dpos.w = (displayFrame.right - displayFrame.left);
    dpos.h = (displayFrame.bottom - displayFrame.top);

    //Calculate the rect for primary based on whether the supplied position
    //is within or outside bounds.
    const int fbWidth =
            ovutils::FrameBufferInfo::getInstance()->getWidth();
    const int fbHeight =
            ovutils::FrameBufferInfo::getInstance()->getHeight();

    if( displayFrame.left < 0 ||
            displayFrame.top < 0 ||
            displayFrame.right > fbWidth ||
            displayFrame.bottom > fbHeight) {

        calculate_crop_rects(sourceCrop, displayFrame, fbWidth, fbHeight);

        //Update calculated width and height
        dcrop.w = sourceCrop.right - sourceCrop.left;
        dcrop.h = sourceCrop.bottom - sourceCrop.top;

        dpos.x = displayFrame.left;
        dpos.y = displayFrame.top;
        dpos.w = displayFrame.right - displayFrame.left;
        dpos.h = displayFrame.bottom - displayFrame.top;
    }

    //Only for Primary
    ov.setCrop(dcrop, ovutils::OV_PIPE0);

    ovutils::eTransform orient =
            static_cast<ovutils::eTransform>(layer->transform);
    ov.setTransform(orient, ovutils::OV_PIPE0);

    ov.setPosition(dpos, ovutils::OV_PIPE0);

    if (!ov.commit(ovutils::OV_PIPE0)) {
        ALOGE("%s: commit fails", __FUNCTION__);
        return false;
    }
    return true;
}

bool configExtVid(hwc_context_t *ctx, hwc_layer_t *layer) {
    overlay::Overlay& ov = *(ctx->mOverlay);
    private_handle_t *hnd = (private_handle_t *)layer->handle;
    ovutils::Whf info(hnd->width, hnd->height, hnd->format, hnd->size);

    ovutils::eMdpFlags mdpFlags = ovutils::OV_MDP_FLAGS_NONE;
    if (hnd->flags & private_handle_t::PRIV_FLAGS_SECURE_BUFFER) {
        ovutils::setMdpFlags(mdpFlags,
                ovutils::OV_MDP_SECURE_OVERLAY_SESSION);
    }
    MetaData_t *metadata = (MetaData_t *)hnd->base_metadata;
    if ((metadata->operation & PP_PARAM_INTERLACED) && metadata->interlaced) {
        ovutils::setMdpFlags(mdpFlags, ovutils::OV_MDP_DEINTERLACE);
    }
    ovutils::eIsFg isFgFlag = ovutils::IS_FG_OFF;
    if (ctx->numHwLayers == 1) {
        isFgFlag = ovutils::IS_FG_SET;
    }

    ovutils::PipeArgs parg(mdpFlags,
            info,
            ovutils::ZORDER_0,
            isFgFlag,
            ovutils::ROT_FLAG_DISABLED);
    ovutils::PipeArgs pargs[ovutils::MAX_PIPES] = { parg, parg, parg };
    ov.setSource(pargs, ovutils::OV_PIPE1);

    hwc_rect_t sourceCrop = layer->sourceCrop;
    // x,y,w,h
    ovutils::Dim dcrop(sourceCrop.left, sourceCrop.top,
            sourceCrop.right - sourceCrop.left,
            sourceCrop.bottom - sourceCrop.top);
    //Only for External
    ov.setCrop(dcrop, ovutils::OV_PIPE1);

    //use sourceTransform only for External
    ov.setTransform(0, ovutils::OV_PIPE1);

    ovutils::Dim dpos;
    hwc_rect_t displayFrame = layer->displayFrame;
    dpos.x = displayFrame.left;
    dpos.y = displayFrame.top;
    dpos.w = (displayFrame.right - displayFrame.left);
    dpos.h = (displayFrame.bottom - displayFrame.top);

    //Only for External
    ov.setPosition(dpos, ovutils::OV_PIPE1);

    if (!ov.commit(ovutils::OV_PIPE1)) {
        ALOGE("%s: commit fails", __FUNCTION__);
        return false;
    }
    return true;
}

bool configExtCC(hwc_context_t *ctx, hwc_layer_t *layer) {
    if(layer == NULL)
        return true;

    overlay::Overlay& ov = *(ctx->mOverlay);
    private_handle_t *hnd = (private_handle_t *)layer->handle;
    ovutils::Whf info(hnd->width, hnd->height, hnd->format, hnd->size);
    ovutils::eIsFg isFgFlag = ovutils::IS_FG_OFF;
    ovutils::eMdpFlags mdpFlags = ovutils::OV_MDP_FLAGS_NONE;
    ovutils::PipeArgs parg(mdpFlags,
            info,
            ovutils::ZORDER_1,
            isFgFlag,
            ovutils::ROT_FLAG_DISABLED);
    ovutils::PipeArgs pargs[ovutils::MAX_PIPES] = { parg, parg, parg };
    ov.setSource(pargs, ovutils::OV_PIPE2);

    hwc_rect_t sourceCrop = layer->sourceCrop;
    // x,y,w,h
    ovutils::Dim dcrop(sourceCrop.left, sourceCrop.top,
            sourceCrop.right - sourceCrop.left,
            sourceCrop.bottom - sourceCrop.top);
    //Only for External
    ov.setCrop(dcrop, ovutils::OV_PIPE2);

    // FIXME: Use source orientation for TV when source is portrait
    //Only for External
    ov.setTransform(0, ovutils::OV_PIPE2);

    //Setting position same as crop
    //FIXME stretch to full screen
    ov.setPosition(dcrop, ovutils::OV_PIPE2);

    if (!ov.commit(ovutils::OV_PIPE2)) {
        ALOGE("%s: commit fails", __FUNCTION__);
        return false;
    }
    return true;
}

bool VideoOverlay::configure(hwc_context_t *ctx, hwc_layer_t *yuvLayer,
        hwc_layer_t *ccLayer) {

    bool ret = true;
    if (LIKELY(ctx->mOverlay)) {
        overlay::Overlay& ov = *(ctx->mOverlay);
        // Set overlay state
        ov.setState(sState);
        switch(sState) {
            case ovutils::OV_2D_VIDEO_ON_PANEL:
                ret &= configPrimVid(ctx, yuvLayer);
                break;
            case ovutils::OV_2D_VIDEO_ON_PANEL_TV:
                ret &= configExtVid(ctx, yuvLayer);
                ret &= configExtCC(ctx, ccLayer);
                ret &= configPrimVid(ctx, yuvLayer);
                break;
            case ovutils::OV_2D_VIDEO_ON_TV:
                ret &= configExtVid(ctx, yuvLayer);
                ret &= configExtCC(ctx, ccLayer);
                break;
            default:
                return false;
        }
    } else {
        //Ov null
        return false;
    }
    return ret;
}

bool VideoOverlay::draw(hwc_context_t *ctx, hwc_layer_list_t *list)
{
    if(!sIsModeOn || sYuvLayerIndex == -1) {
#include "hwc_video.h"
#include "hwc_utils.h"
#include "qdMetaData.h"
#include "mdp_version.h"
#include <overlayRotator.h>

using overlay::Rotator;

namespace qhwc {

namespace ovutils = overlay::utils;

//===========IVideoOverlay=========================
IVideoOverlay* IVideoOverlay::getObject(const int& width, const int& dpy) {
    if(width > MAX_DISPLAY_DIM) {
        return new VideoOverlayHighRes(dpy);
    }
    return new VideoOverlayLowRes(dpy);
}

//===========VideoOverlayLowRes=========================

VideoOverlayLowRes::VideoOverlayLowRes(const int& dpy): IVideoOverlay(dpy) {}

//Cache stats, figure out the state, config overlay
bool VideoOverlayLowRes::prepare(hwc_context_t *ctx,
        hwc_display_contents_1_t *list) {

    if(ctx->listStats[mDpy].yuvCount > 1)
        return false;

    int yuvIndex =  ctx->listStats[mDpy].yuvIndices[0];
    int hw_w = ctx->dpyAttr[mDpy].xres;
    mModeOn = false;

    if(hw_w > MAX_DISPLAY_DIM) {
       ALOGD_IF(VIDEO_DEBUG,"%s, \
                      Cannot use video path for High Res Panels", __FUNCTION__);
       return false;
    }

    if((!ctx->mMDP.hasOverlay) ||
                            (qdutils::MDPVersion::getInstance().getMDPVersion()
                             <= qdutils::MDP_V4_0)) {
       ALOGD_IF(VIDEO_DEBUG,"%s, this hw doesnt support overlay", __FUNCTION__);
       return false;
    }

    if(isSecuring(ctx)) {
       ALOGD_IF(VIDEO_DEBUG,"%s: MDP Secure is active", __FUNCTION__);
       return false;
    }

    if(yuvIndex == -1 || ctx->listStats[mDpy].yuvCount != 1) {
        return false;
    }

    //index guaranteed to be not -1 at this point
    hwc_layer_1_t *layer = &list->hwLayers[yuvIndex];
    if (isSecureModePolicy(ctx->mMDP.version)) {
        private_handle_t *hnd = (private_handle_t *)layer->handle;
        if(ctx->mSecureMode) {
            if (! isSecureBuffer(hnd)) {
                ALOGD_IF(VIDEO_DEBUG, "%s: Handle non-secure video layer"
                         "during secure playback gracefully", __FUNCTION__);
                return false;
            }
        } else {
            if (isSecureBuffer(hnd)) {
                ALOGD_IF(VIDEO_DEBUG, "%s: Handle secure video layer"
                         "during non-secure playback gracefully", __FUNCTION__);
                return false;
            }
        }
    }

    if((layer->transform & HWC_TRANSFORM_ROT_90) && ctx->mDMAInUse) {
        ctx->mDMAInUse = false;
        ALOGD_IF(VIDEO_DEBUG, "%s: Rotator not available since \
                  DMA Pipe(s) are in use",__FUNCTION__);
        return false;
    }

    if(configure(ctx, layer)) {
        markFlags(layer);
        mModeOn = true;
    }

    return mModeOn;
}

void VideoOverlayLowRes::markFlags(hwc_layer_1_t *layer) {
    if(layer) {
        layer->compositionType = HWC_OVERLAY;
        layer->hints |= HWC_HINT_CLEAR_FB;
    }
}

bool VideoOverlayLowRes::configure(hwc_context_t *ctx,
        hwc_layer_1_t *layer) {

    overlay::Overlay& ov = *(ctx->mOverlay);
    private_handle_t *hnd = (private_handle_t *)layer->handle;
    ovutils::Whf info(hnd->width, hnd->height,
            ovutils::getMdpFormat(hnd->format), hnd->size);

    //Request a VG pipe
    ovutils::eDest dest = ov.nextPipe(ovutils::OV_MDP_PIPE_VG, mDpy);
    if(dest == ovutils::OV_INVALID) { //None available
        return false;
    }

    mDest = dest;
    ovutils::eMdpFlags mdpFlags = ovutils::OV_MDP_FLAGS_NONE;
    ovutils::eZorder zOrder = ovutils::ZORDER_0;
    ovutils::eIsFg isFg = ovutils::IS_FG_OFF;
    if (ctx->listStats[mDpy].numAppLayers == 1) {
        isFg = ovutils::IS_FG_SET;
    }

    return (configureLowRes(ctx, layer, mDpy, mdpFlags, zOrder, isFg, dest,
            &mRot) == 0 );
}

bool VideoOverlayLowRes::draw(hwc_context_t *ctx,
        hwc_display_contents_1_t *list) {
    if(!mModeOn) {
        return true;
    }

    int yuvIndex = ctx->listStats[mDpy].yuvIndices[0];
    if(yuvIndex == -1) {
        return true;
    }

    private_handle_t *hnd = (private_handle_t *)
            list->hwLayers[yuvIndex].handle;

    overlay::Overlay& ov = *(ctx->mOverlay);
    int fd = hnd->fd;
    uint32_t offset = hnd->offset;
    Rotator *rot = mRot;

    if(rot) {
        if(!rot->queueBuffer(fd, offset))
            return false;
        fd = rot->getDstMemId();
        offset = rot->getDstOffset();
    }

    if (!ov.queueBuffer(fd, offset, mDest)) {
        ALOGE("%s: queueBuffer failed for dpy=%d", __FUNCTION__, mDpy);
        return false;
    }

    return true;
}

bool VideoOverlayLowRes::isModeOn() {
    return mModeOn;
}

//===========VideoOverlayHighRes=========================

VideoOverlayHighRes::VideoOverlayHighRes(const int& dpy): IVideoOverlay(dpy) {}

//Cache stats, figure out the state, config overlay
bool VideoOverlayHighRes::prepare(hwc_context_t *ctx,
        hwc_display_contents_1_t *list) {

    int yuvIndex =  ctx->listStats[mDpy].yuvIndices[0];
    int hw_w = ctx->dpyAttr[mDpy].xres;
    mModeOn = false;

    if(!ctx->mMDP.hasOverlay) {
       ALOGD_IF(VIDEO_DEBUG,"%s, this hw doesnt support overlay", __FUNCTION__);
       return false;
    }

    if(yuvIndex == -1 || ctx->listStats[mDpy].yuvCount != 1) {
        return false;
    }

    //index guaranteed to be not -1 at this point
    hwc_layer_1_t *layer = &list->hwLayers[yuvIndex];
    if(configure(ctx, layer)) {
        markFlags(layer);
        mModeOn = true;
    }

    return mModeOn;
}

void VideoOverlayHighRes::markFlags(hwc_layer_1_t *layer) {
    if(layer) {
        layer->compositionType = HWC_OVERLAY;
        layer->hints |= HWC_HINT_CLEAR_FB;
    }
}

bool VideoOverlayHighRes::configure(hwc_context_t *ctx,
        hwc_layer_1_t *layer) {

    int hw_w = ctx->dpyAttr[mDpy].xres;
    overlay::Overlay& ov = *(ctx->mOverlay);
    private_handle_t *hnd = (private_handle_t *)layer->handle;
    ovutils::Whf info(hnd->width, hnd->height,
            ovutils::getMdpFormat(hnd->format), hnd->size);

    //Request a VG pipe
    mDestL = ovutils::OV_INVALID;
    mDestR = ovutils::OV_INVALID;
    hwc_rect_t dst = layer->displayFrame;
    if(dst.left > hw_w/2) {
        mDestR = ov.nextPipe(ovutils::OV_MDP_PIPE_VG, mDpy);
        if(mDestR == ovutils::OV_INVALID)
            return false;
    } else if (dst.right <= hw_w/2) {
        mDestL = ov.nextPipe(ovutils::OV_MDP_PIPE_VG, mDpy);
        if(mDestL == ovutils::OV_INVALID)
            return false;
    } else {
        mDestL = ov.nextPipe(ovutils::OV_MDP_PIPE_VG, mDpy);
        mDestR = ov.nextPipe(ovutils::OV_MDP_PIPE_VG, mDpy);
        if(mDestL == ovutils::OV_INVALID ||
                mDestR == ovutils::OV_INVALID)
            return false;
    }

    ovutils::eMdpFlags mdpFlags = ovutils::OV_MDP_FLAGS_NONE;
    ovutils::eZorder zOrder = ovutils::ZORDER_0;
    ovutils::eIsFg isFg = ovutils::IS_FG_OFF;
    if (ctx->listStats[mDpy].numAppLayers == 1) {
        isFg = ovutils::IS_FG_SET;
    }

    return (configureHighRes(ctx, layer, mDpy, mdpFlags, zOrder, isFg, mDestL,
            mDestR, &mRot) == 0 );
}

bool VideoOverlayHighRes::draw(hwc_context_t *ctx,
        hwc_display_contents_1_t *list) {
    if(!mModeOn) {
        return true;
    }

    int yuvIndex = ctx->listStats[mDpy].yuvIndices[0];
    if(yuvIndex == -1) {
        return true;
    }

    private_handle_t *hnd = (private_handle_t *)
            list->hwLayers[sYuvLayerIndex].handle;

    private_handle_t *cchnd = NULL;
    if(sCCLayerIndex != -1) {
        cchnd = (private_handle_t *)list->hwLayers[sCCLayerIndex].handle;
        ctx->qbuf->lockAndAdd(cchnd);
    }

    // Lock this buffer for read.
    ctx->qbuf->lockAndAdd(hnd);

    bool ret = true;
    overlay::Overlay& ov = *(ctx->mOverlay);
    ovutils::eOverlayState state = ov.getState();

    switch (state) {
        case ovutils::OV_2D_VIDEO_ON_PANEL_TV:
            // Play external
            if (!ov.queueBuffer(hnd->fd, hnd->offset, ovutils::OV_PIPE1)) {
                ALOGE("%s: queueBuffer failed for external", __FUNCTION__);
                ret = false;
            }
            //Play CC on external
            if (cchnd && !ov.queueBuffer(cchnd->fd, cchnd->offset,
                        ovutils::OV_PIPE2)) {
                ALOGE("%s: queueBuffer failed for cc external", __FUNCTION__);
                ret = false;
            }
            // Play primary
            if (!ov.queueBuffer(hnd->fd, hnd->offset, ovutils::OV_PIPE0)) {
                ALOGE("%s: queueBuffer failed for primary", __FUNCTION__);
                ret = false;
            }
            break;
        case ovutils::OV_2D_VIDEO_ON_PANEL:
            // Play primary
            if (!ov.queueBuffer(hnd->fd, hnd->offset, ovutils::OV_PIPE0)) {
                ALOGE("%s: queueBuffer failed for primary", __FUNCTION__);
                ret = false;
            }
            break;
        case ovutils::OV_2D_VIDEO_ON_TV:
            // Play external
            if (!ov.queueBuffer(hnd->fd, hnd->offset, ovutils::OV_PIPE1)) {
                ALOGE("%s: queueBuffer failed for external", __FUNCTION__);
                ret = false;
            }
            //Play CC on external
            if (cchnd && !ov.queueBuffer(cchnd->fd, cchnd->offset,
                        ovutils::OV_PIPE2)) {
                ALOGE("%s: queueBuffer failed for cc external", __FUNCTION__);
                ret = false;
            }
            break;
        default:
            ALOGE("%s Unused state %s", __FUNCTION__,
                    ovutils::getStateString(state));
            break;
    }

    return ret;
            list->hwLayers[yuvIndex].handle;

    overlay::Overlay& ov = *(ctx->mOverlay);
    int fd = hnd->fd;
    uint32_t offset = hnd->offset;
    Rotator *rot = mRot;

    if(rot) {
        if(!rot->queueBuffer(fd, offset))
            return false;
        fd = rot->getDstMemId();
        offset = rot->getDstOffset();
    }

    if(mDestL != ovutils::OV_INVALID) {
        if (!ov.queueBuffer(fd, offset, mDestL)) {
            ALOGE("%s: queueBuffer failed for dpy=%d's left mixer",
                    __FUNCTION__, mDpy);
            return false;
        }
    }

    if(mDestR != ovutils::OV_INVALID) {
        if (!ov.queueBuffer(fd, offset, mDestR)) {
            ALOGE("%s: queueBuffer failed for dpy=%d's right mixer"
                    , __FUNCTION__, mDpy);
            return false;
        }
    }

    return true;
}

bool VideoOverlayHighRes::isModeOn() {
    return mModeOn;
}

}; //namespace qhwc
