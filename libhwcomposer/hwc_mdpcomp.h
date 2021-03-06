/*
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

#ifndef HWC_MDP_COMP
#define HWC_MDP_COMP

#include <hwc_utils.h>
#include <idle_invalidator.h>
#include <cutils/properties.h>
#include <overlay.h>

#define MAX_STATIC_PIPES 3
#define MDPCOMP_INDEX_OFFSET 4
#define DEFAULT_IDLE_TIME 2000

#define MAX_VG 2
#define MAX_RGB 2
#define VAR_INDEX 3
#define MAX_PIPES (MAX_VG + MAX_RGB)
#define HWC_MDPCOMP_INDEX_MASK 0x00000030


//struct hwc_context_t;

namespace qhwc {

// pipe status
enum {
    PIPE_UNASSIGNED = 0,
    PIPE_IN_FB_MODE,
    PIPE_IN_COMP_MODE,
};

// pipe request
enum {
    PIPE_NONE = 0,
    PIPE_REQ_VG,
    PIPE_REQ_RGB,
    PIPE_REQ_FB,
};

// MDP Comp Status
enum {
    MDPCOMP_SUCCESS = 0,
    MDPCOMP_FAILURE,
    MDPCOMP_ABORT,
};

//This class manages the status of 4 MDP pipes and keeps
//track of Variable pipe mode.
class PipeMgr {

public:
    PipeMgr() { reset();}
    //reset pipemgr params
    void reset();

    //Based on the preference received, pipe mgr
    //allocates the best available pipe to handle
    //the case
    int req_for_pipe(int pipe_req);

    //Allocate requested pipe and update availablity
    int assign_pipe(int pipe_pref);

    // Get/Set pipe status
    void setStatus(int pipe_index, int pipe_status) {
        mStatus[pipe_index] = pipe_status;
    }
    int getStatus(int pipe_index) {
        return mStatus[pipe_index];
    }
private:
    int mVGPipes;
    int mVGUsed;
    int mVGIndex;
    int mRGBPipes;
    int mRGBUsed;
    int mRGBIndex;
    int mTotalAvail;
    int mStatus[MAX_PIPES];
};


class MDPComp {
    enum State {
        MDPCOMP_ON = 0,
        MDPCOMP_OFF,
        MDPCOMP_OFF_PENDING,
    };

    enum {
        MDPCOMP_LAYER_BLEND = 1,
        MDPCOMP_LAYER_DOWNSCALE = 2,
        MDPCOMP_LAYER_SKIP = 4,
        MDPCOMP_LAYER_UNSUPPORTED_MEM = 8,
    };

    struct mdp_pipe_info {
        int index;
        int z_order;
        bool isVG;
        bool isFG;
        bool vsync_wait;
    };

    struct pipe_layer_pair {
        int layer_index;
        mdp_pipe_info pipe_index;
        native_handle_t* handle;
    };

    struct frame_info {
        int count;
        struct pipe_layer_pair* pipe_layer;

    };

    struct layer_mdp_info {
        bool can_use_mdp;
        int pipe_pref;
    };

    static State sMDPCompState;
    static IdleInvalidator *idleInvalidator;
    static struct frame_info sCurrentFrame;
    static PipeMgr sPipeMgr;
    static int sSkipCount;
    static int sMaxLayers;
    static bool sDebugLogs;
    static bool sIdleFallBack;

public:
    /* Handler to invoke frame redraw on Idle Timer expiry */
    static void timeout_handler(void *udata);

    /* configure/tear-down MDPComp params*/
    static bool init(hwc_context_t *ctx);
    static bool deinit();

    /*sets up mdp comp for the current frame */
    static bool configure(hwc_composer_device_t *ctx,  hwc_layer_list_t* list);

    /* draw */
    static int draw(hwc_context_t *ctx, hwc_layer_list_t *list);

    /* store frame stats */
    static void setStats(int skipCt) { sSkipCount  = skipCt;};

private:

    /* get/set pipe index associated with overlay layers */
    static void setLayerIndex(hwc_layer_t* layer, const int pipe_index);
    static int  getLayerIndex(hwc_layer_t* layer);

    /* set/reset flags for MDPComp */
    static void setMDPCompLayerFlags(hwc_layer_list_t* list);
    static void unsetMDPCompLayerFlags(hwc_context_t* ctx,
                                       hwc_layer_list_t* list);

    static void print_info(hwc_layer_t* layer);

    /* configure's overlay pipes for the frame */
    static int  prepare(hwc_context_t *ctx, hwc_layer_t *layer,
                        mdp_pipe_info& mdp_info);

    /* checks for conditions where mdpcomp is not possible */
    static bool is_doable(hwc_composer_device_t *dev, hwc_layer_list_t* list);

    static bool setup(hwc_context_t* ctx, hwc_layer_list_t* list);

    /* parses layer for properties affecting mdp comp */
    static void get_layer_info(hwc_layer_t* layer, int& flags);

    /* iterates through layer list to choose candidate to use overlay */
    static int  mark_layers(hwc_layer_list_t* list, layer_mdp_info* layer_info,
                                                  frame_info& current_frame);
    static bool parse_and_allocate(hwc_context_t* ctx, hwc_layer_list_t* list,
                                                  frame_info& current_frame );

    /* clears layer info struct */
    static void reset_layer_mdp_info(layer_mdp_info* layer_mdp_info,int count);

    /* allocates pipes to selected candidates */
    static bool alloc_layer_pipes(hwc_layer_list_t* list,
                                  layer_mdp_info* layer_info,
                                  frame_info& current_frame);
    /* updates variable pipe mode for the current frame */
    static int  configure_var_pipe(hwc_context_t* ctx);

    /* get/set states */
    static State get_state() { return sMDPCompState; };
    static void set_state(State state) { sMDPCompState = state; };

    /* reset state */
    static void reset( hwc_context_t *ctx, hwc_layer_list_t* list );
    /* reset compostiion type to default */
    static void reset_comp_type(hwc_layer_list_t* list);

    /* Is feature enabled */
    static bool isEnabled() { return sMaxLayers ? true : false; };
    /* Is debug enabled */
    static bool isDebug() { return sDebugLogs ? true : false; };
#define DEFAULT_IDLE_TIME 2000
#define MAX_PIPES_PER_MIXER 4

namespace overlay {
    class Rotator;
};

namespace qhwc {
namespace ovutils = overlay::utils;

class MDPComp {
public:
    virtual ~MDPComp(){};
    /*sets up mdp comp for the current frame */
    bool prepare(hwc_context_t *ctx, hwc_display_contents_1_t* list);
    /* draw */
    virtual bool draw(hwc_context_t *ctx, hwc_display_contents_1_t *list) = 0;

    void dump(android::String8& buf);
    bool isUsed() { return (mState == MDPCOMP_ON); };

    static MDPComp* getObject(const int& width);
    /* Handler to invoke frame redraw on Idle Timer expiry */
    static void timeout_handler(void *udata);
    static bool init(hwc_context_t *ctx);

protected:
    enum eState {
        MDPCOMP_ON = 0,
        MDPCOMP_OFF,
    };

    enum ePipeType {
        MDPCOMP_OV_RGB = ovutils::OV_MDP_PIPE_RGB,
        MDPCOMP_OV_VG = ovutils::OV_MDP_PIPE_VG,
        MDPCOMP_OV_DMA = ovutils::OV_MDP_PIPE_DMA,
        MDPCOMP_OV_ANY,
    };
    struct MdpPipeInfo {
        int zOrder;
        virtual ~MdpPipeInfo(){};
    };
    struct PipeLayerPair {
        MdpPipeInfo *pipeInfo;
        native_handle_t* handle;
        overlay::Rotator* rot;
    };

    /* introduced for mixed mode implementation */
    struct FrameInfo {
        int count;
        struct PipeLayerPair* pipeLayer;
    };

    /* calculates pipes needed for the panel */
    virtual int pipesNeeded(hwc_context_t *ctx,
                            hwc_display_contents_1_t* list) = 0;
    /* allocates pipe from pipe book */
    virtual bool allocLayerPipes(hwc_context_t *ctx,
                hwc_display_contents_1_t* list,FrameInfo& current_frame) = 0;
    /* configures MPD pipes */
    virtual int configure(hwc_context_t *ctx, hwc_layer_1_t *layer,
                PipeLayerPair& pipeLayerPair) = 0;


    /* set/reset flags for MDPComp */
    void setMDPCompLayerFlags(hwc_context_t *ctx,
                                       hwc_display_contents_1_t* list);
    void unsetMDPCompLayerFlags(hwc_context_t* ctx,
                                       hwc_display_contents_1_t* list);
    /* get/set states */
    eState getState() { return mState; };
    /* reset state */
    void reset( hwc_context_t *ctx, hwc_display_contents_1_t* list );
    /* allocate MDP pipes from overlay */
    ovutils::eDest getMdpPipe(hwc_context_t *ctx, ePipeType type);
    /* checks for conditions where mdpcomp is not possible */
    bool isDoable(hwc_context_t *ctx, hwc_display_contents_1_t* list);
    /* sets up MDP comp for current frame */
    bool setup(hwc_context_t* ctx, hwc_display_contents_1_t* list);
    /* Is debug enabled */
    static bool isDebug() { return sDebugLogs ? true : false; };
    /* Is feature enabled */
    static bool isEnabled() { return sEnabled; };
    /* checks for mdp comp width limitation */
    bool isValidDimension(hwc_context_t *ctx, hwc_layer_1_t *layer);

    eState mState;

    static bool sEnabled;
    static bool sDebugLogs;
    static bool sIdleFallBack;
    static IdleInvalidator *idleInvalidator;
    struct FrameInfo mCurrentFrame;
};

class MDPCompLowRes : public MDPComp {
public:
     virtual ~MDPCompLowRes(){};
     virtual bool draw(hwc_context_t *ctx, hwc_display_contents_1_t *list);

private:
    struct MdpPipeInfoLowRes : public MdpPipeInfo {
        ovutils::eDest index;
        virtual ~MdpPipeInfoLowRes() {};
    };

    /* configure's overlay pipes for the frame */
    virtual int configure(hwc_context_t *ctx, hwc_layer_1_t *layer,
            PipeLayerPair& pipeLayerPair);

    /* allocates pipes to selected candidates */
    virtual bool allocLayerPipes(hwc_context_t *ctx,
            hwc_display_contents_1_t* list,
            FrameInfo& current_frame);

    virtual int pipesNeeded(hwc_context_t *ctx,
            hwc_display_contents_1_t* list);
};

class MDPCompHighRes : public MDPComp {
public:
    virtual ~MDPCompHighRes(){};
    virtual bool draw(hwc_context_t *ctx, hwc_display_contents_1_t *list);
private:
    struct MdpPipeInfoHighRes : public MdpPipeInfo {
        ovutils::eDest lIndex;
        ovutils::eDest rIndex;
        virtual ~MdpPipeInfoHighRes() {};
    };

    bool acquireMDPPipes(hwc_context_t *ctx, hwc_layer_1_t* layer,
                        MdpPipeInfoHighRes& pipe_info, ePipeType type);

    /* configure's overlay pipes for the frame */
    virtual int configure(hwc_context_t *ctx, hwc_layer_1_t *layer,
            PipeLayerPair& pipeLayerPair);

    /* allocates pipes to selected candidates */
    virtual bool allocLayerPipes(hwc_context_t *ctx,
            hwc_display_contents_1_t* list,
            FrameInfo& current_frame);

    virtual int pipesNeeded(hwc_context_t *ctx, hwc_display_contents_1_t* list);
};
}; //namespace
#endif
