// Couch co-op: a second local player, a real daPy_lk_c on controller port 2.
//
// Player code treats a lot of global state as its own. Player 2's code therefore only ever runs
// inside a P2Window, which exchanges the player pointers, the pads, the status words, the lock-on
// and the HUD's button state with player 2's and puts player 1's back afterwards. Based on the
// puppet Link of ww-online's Seaborne (MIT, datstarkey): own copy of Link.arc, own eye and brow
// packets, kept position. That puppet mirrors a remote game; this one runs the stock procs.
//
// Controls: + on controller 2 opens player 2's picker and joins them. ZL + ZR + Plus on either
// controller opens that player's picker. Up and down choose, A confirms, B cancels.
//
// Rules of this first stage: only player 1 starts events and changes the stage. Player 2 freezes
// during events, is hidden while player 1 sails or is dead, and warps to player 1 when lost.
#include "d/dolzel.h" // IWYU pragma: keep
#include "SSystem/SComponent/c_m3d.h"
#include "d/actor/d_a_player_main.h"
#include "d/actor/d_a_player_main_data.h"
#include "d/d_com_inf_game.h"
#include "d/d_com_lib_game.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "m_Do/m_Do_controller_pad.h"
#include "mods/couch_coop.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#if COS_WITH_COUCH_COOP

bool g_coopInP2 = false;
dAttention_c* g_coopAttention = NULL;

class daCoopP2_c : public daPy_lk_c {
public:
    static cPhs_State phase1(daCoopP2_c* i_this);
    static cPhs_State phase2(daCoopP2_c* i_this);
    static cPhs_State phase3(daCoopP2_c* i_this);
    cPhs_State create();
    BOOL execute();
    BOOL draw();
    BOOL remove();
    bool mountLinkCopy();
    void warpToP1();
    bool parked() const;
    bool ownsActor(fopAc_ac_c* actor);
    bool hidden() const;

    bool mArcMounted;
    bool mPlayerInited;
    bool mWasParked;
    bool mWantWarp;
    bool mAttentionMade;
    int mNoGroundFrames;
    char mArcName[sizeof("Link")];
    u32 mStatus[4];
    daPy_cupPackets_c mCupPackets;
    alignas(dAttention_c) unsigned char mAttentionStore[sizeof(dAttention_c)];
};

namespace {

// No start event (0xFF), start mode 0, and bit 0x40: makeBgWait keeps the restart room.
const u32 kP2Param = 0xFF000040;
const int kSettleFrames = 30;
const int kNoGroundFrames = 60;
const f32 kTetherXZ = 2000.0f;
const f32 kTetherY = 1500.0f;
const int kRetryFrames = 300;

daCoopP2_c* sP2 = NULL;
fpc_ProcID sP2Id = fpcM_ERROR_PROCESS_ID_e;
bool sJoined = false;
int sInPlayFrames = 0;
int sRetryFrames = 0;
int sPicker[2] = {-1, -1}; // the highlighted character, or -1 when closed
int sPickerStick[2] = {0, 0};
bool sPlayMessageShown = false;
bool sPad2Connected = false;
char sToast[128] = "";
int sToastFrames = 0;

void toast(const char* text, int frames = 240) {
    snprintf(sToast, sizeof(sToast), "%s", text);
    sToastFrames = frames;
}

// The picker takes this player's controller while it is open. Returns once a choice is made.
void pickerInput(int slot) {
    interface_of_controller_pad& pad = g_mDoCPd_cpadInfo[slot];
    int move = 0;
    if (pad.mButtonTrig.up) {
        move = -1;
    } else if (pad.mButtonTrig.down) {
        move = 1;
    }
    const int stick = pad.mMainStickPosY > 0.6f ? -1 : pad.mMainStickPosY < -0.6f ? 1 : 0;
    if (stick != sPickerStick[slot]) {
        sPickerStick[slot] = stick;
        if (stick != 0) {
            move = stick;
        }
    }
    sPicker[slot] = (sPicker[slot] + move + COOP_CHAR_COUNT) % COOP_CHAR_COUNT;
    if (pad.mButtonTrig.a) {
        coop_charRequest(slot, sPicker[slot]);
        char text[64];
        snprintf(text, sizeof(text), "Player %d is %s", slot + 1, coop_charName(sPicker[slot]));
        toast(text);
        sPicker[slot] = -1;
    } else if (pad.mButtonTrig.b) {
        sPicker[slot] = -1;
    }
    memset(&pad.mButtonHold, 0, sizeof(pad.mButtonHold));
    memset(&pad.mButtonTrig, 0, sizeof(pad.mButtonTrig));
    pad.mMainStickPosX = pad.mMainStickPosY = pad.mMainStickValue = 0.0f;
    pad.mCStickPosX = pad.mCStickPosY = pad.mCStickValue = 0.0f;
    pad.mTriggerLeft = pad.mTriggerRight = 0.0f;
    pad.mHoldLockL = pad.mTrigLockL = pad.mHoldLockR = pad.mTrigLockR = 0;
}

bool isLinkP2(const fopAc_ac_c* actor) {
    return actor != NULL && actor == sP2;
}

dAttention_c* attentionOf(daCoopP2_c* p2) {
    return p2->mAttentionMade ? reinterpret_cast<dAttention_c*>(p2->mAttentionStore) : NULL;
}

// Items that need no first-person camera. The camera follows player 1 until split screen.
bool itemAllowed(u8 item) {
    return item == dItemNo_BOMB_BAG_e || item == dItemNo_SKULL_HAMMER_e;
}

void maskP2Items() {
    interface_of_controller_pad& pad = g_mDoCPd_cpadInfo[0];
    if (!itemAllowed(dComIfGp_getSelectItem(dItemBtn_X_e))) {
        pad.mButtonHold.x = 0;
        pad.mButtonTrig.x = 0;
    }
    if (!itemAllowed(dComIfGp_getSelectItem(dItemBtn_Y_e))) {
        pad.mButtonHold.y = 0;
        pad.mButtonTrig.y = 0;
    }
    if (!itemAllowed(dComIfGp_getSelectItem(dItemBtn_Z_e))) {
        pad.mButtonHold.z = 0;
        pad.mButtonTrig.z = 0;
    }
}

class P2Window {
public:
    explicit P2Window(daCoopP2_c* p2)
        : mP2(p2), mKeepPos(p2->current.pos), mKeepShape(p2->shape_angle), mKeepAngle(p2->current.angle) {
        dComIfG_play_c& play = g_dComIfG_gameInfo.play;
        memcpy(mArcName, l_arcName, sizeof(mArcName));
        memcpy(l_arcName, p2->mArcName, sizeof(mArcName));
        memcpy(mStatus, play.mPlayerStatus[0], sizeof(mStatus));
        memcpy(play.mPlayerStatus[0], p2->mStatus, sizeof(mStatus));
        mException = play.getEvtManager().mException;
        daPy_swapDebugKeep(&mKeepPos, &mKeepShape, &mKeepAngle);
        mPlayer = play.getPlayer(0);
        mLink = play.getPlayerPtr(0);
        // Each player's lock-on list must not offer the other player.
        mLinkAttention = mLink != NULL ? mLink->attention_info.flags : 0;
        if (mLink != NULL) {
            mLink->attention_info.flags = 0;
        }
        play.setPlayer(0, p2);
        play.setPlayerPtr(0, p2);
        std::swap(g_mDoCPd_cpadInfo[0], g_mDoCPd_cpadInfo[1]);
        maskP2Items();
        mDoStatus = play.getDoStatus();
        mAStatus = play.getAStatus();
        mRStatus = play.getRStatus();
        mCameraStatus = play.getCameraAttentionStatus(0);
        play.setCameraAttentionStatus(0, mCameraStatus & ~(dCamAttnStts_SUBJECT_e | dCamAttnStts_00000020_e));
        g_coopInP2 = true;
        g_coopAttention = attentionOf(p2);
    }
    ~P2Window() {
        dComIfG_play_c& play = g_dComIfG_gameInfo.play;
        g_coopAttention = NULL;
        g_coopInP2 = false;
        play.setCameraAttentionStatus(0, mCameraStatus);
        play.setRStatus(mRStatus);
        play.setAStatus(mAStatus);
        play.setDoStatus(mDoStatus);
        std::swap(g_mDoCPd_cpadInfo[0], g_mDoCPd_cpadInfo[1]);
        play.setPlayerPtr(0, mLink);
        play.setPlayer(0, mPlayer);
        if (mLink != NULL) {
            mLink->attention_info.flags = mLinkAttention;
        }
        mP2->attention_info.flags = 0;
        daPy_swapDebugKeep(&mKeepPos, &mKeepShape, &mKeepAngle);
        play.getEvtManager().mException = mException;
        memcpy(mP2->mStatus, play.mPlayerStatus[0], sizeof(mStatus));
        memcpy(play.mPlayerStatus[0], mStatus, sizeof(mStatus));
        memcpy(l_arcName, mArcName, sizeof(mArcName));
    }

private:
    daCoopP2_c* mP2;
    char mArcName[sizeof("Link")];
    u32 mStatus[4];
    dEvent_exception_c mException;
    cXyz mKeepPos;
    csXyz mKeepShape;
    csXyz mKeepAngle;
    fopAc_ac_c* mPlayer;
    fopAc_ac_c* mLink;
    u32 mLinkAttention;
    u32 mCameraStatus;
    u8 mDoStatus;
    u8 mAStatus;
    u8 mRStatus;
};

} // namespace

bool coop_restartInstead(const fopAc_ac_c* player) {
    if (!isLinkP2(player)) {
        return false;
    }
    sP2->mWantWarp = true;
    return true;
}

void coop_drawAttention() {
    if (sP2 != NULL && !sP2->hidden()) {
        if (dAttention_c* attention = attentionOf(sP2)) {
            attention->Draw();
        }
    }
}

CoopActorScope::CoopActorScope(fopAc_ac_c* actor) : mPlayer(NULL), mLink(NULL), mActive(false) {
    if (sP2 == NULL || actor == sP2) {
        return;
    }
    dComIfG_play_c& play = g_dComIfG_gameInfo.play;
    fopAc_ac_c* link = play.getPlayerPtr(0);
    if (link == NULL || actor == link) {
        return;
    }
    bool useP2 = sP2->ownsActor(actor);
    if (!useP2 && fopAcM_GetGroup(actor) == fopAc_ENEMY_e && !sP2->parked()) {
        useP2 = (sP2->current.pos - actor->current.pos).abs2() < (link->current.pos - actor->current.pos).abs2();
    }
    if (useP2) {
        mPlayer = play.getPlayer(0);
        mLink = link;
        mActive = true;
        play.setPlayer(0, sP2);
        play.setPlayerPtr(0, sP2);
    }
}

CoopActorScope::~CoopActorScope() {
    if (mActive) {
        dComIfG_play_c& play = g_dComIfG_gameInfo.play;
        play.setPlayerPtr(0, mLink);
        play.setPlayer(0, mPlayer);
    }
}

// The copy of Link.arc, mounted again (a fresh read) and registered under mArcName, so its model
// data and the animators written into it are not player 1's.
bool daCoopP2_c::mountLinkCopy() {
    dRes_control_c& control = g_dComIfG_gameInfo.mResControl;
    if (control.getObjectResInfo(mArcName) != NULL) {
        return false;
    }
    dRes_info_c* info = dRes_control_c::newResInfo(control.mObjectInfo, ARRAY_SIZE(control.mObjectInfo));
    if (info == NULL || !info->set("Link", "/res/Object/", JKRArchive::DEFAULT_MOUNT_DIRECTION, NULL)) {
        return false;
    }
    strncpy(info->getArchiveName(), mArcName, sizeof(mArcName));
    info->incCount();
    return true;
}

cPhs_State daCoopP2_c::phase1(daCoopP2_c* i_this) {
    fopAcM_ct(i_this, daCoopP2_c);
    fopAcM_SetParam(i_this, kP2Param);
    memcpy(i_this->mArcName, "Lnk1", sizeof(i_this->mArcName));
    fopAcM_setStageLayer(i_this);
    i_this->attention_info.flags = 0;
    if (!i_this->mountLinkCopy()) {
        fprintf(stderr, "[coop] could not mount the copy of Link.arc\n");
        return cPhs_ERROR_e;
    }
    i_this->mArcMounted = true;
    return cPhs_NEXT_e;
}

cPhs_State daCoopP2_c::phase2(daCoopP2_c* i_this) {
    const int loading = dComIfG_syncObjectRes(i_this->mArcName);
    if (loading < 0) {
        fprintf(stderr, "[coop] the copy of Link.arc failed to load\n");
        return cPhs_ERROR_e;
    }
    fopAc_ac_c* player = dComIfGp_getLinkPlayer();
    if (loading > 0 || player == NULL || !fopAcM_IsExecuting(fopAcM_GetID(player))) {
        return cPhs_INIT_e;
    }
    P2Window window(i_this);
    i_this->playerInit();
    i_this->mPlayerInited = true;
    return cPhs_NEXT_e;
}

cPhs_State daCoopP2_c::phase3(daCoopP2_c* i_this) {
    P2Window window(i_this);
    return i_this->makeBgWait();
}

cPhs_State daCoopP2_c::create() {
    static cPhs__Handler l_method[] = {
        (cPhs__Handler)phase1,
        (cPhs__Handler)phase2,
        (cPhs__Handler)phase3,
        (cPhs__Handler)NULL,
    };
    return dComLbG_PhaseHandler(getPhase(), l_method, this);
}

// Items player 2 made, and whatever they carry or hold, follow player 2's hands.
bool daCoopP2_c::ownsActor(fopAc_ac_c* actor) {
    return fopAcM_GetLinkId(actor) == fopAcM_GetID(this) || mActorKeepGrab.getActor() == actor ||
           mActorKeepEquip.getActor() == actor;
}

// Player 2's code does not run: an event, the boat, player 1 dead, or the stage changing.
bool daCoopP2_c::parked() const {
    return dComIfGp_event_runCheck() || dComIfGp_checkPlayerStatus0(0, daPyStts0_SHIP_RIDE_e) ||
           dComIfGs_getLife() == 0 || dComIfGp_isEnableNextStage();
}

bool daCoopP2_c::hidden() const {
    return dComIfGp_checkPlayerStatus0(0, daPyStts0_SHIP_RIDE_e) || dComIfGs_getLife() == 0;
}

void daCoopP2_c::warpToP1() {
    fopAc_ac_c* link = dComIfGp_getLinkPlayer();
    if (link == NULL) {
        return;
    }
    cXyz pos = link->current.pos;
    pos.x += 60.0f * cM_ssin(link->shape_angle.y + 0x4000);
    pos.z += 60.0f * cM_scos(link->shape_angle.y + 0x4000);
    pos.y += 20.0f;
    current.pos = pos;
    old.pos = pos;
    current.angle = link->shape_angle;
    shape_angle = link->shape_angle;
    speed.setall(0.0f);
    speedF = 0.0f;
    fopAcM_SetRoomNo(this, fopAcM_GetRoomNo(link));
    mNoGroundFrames = 0;
    mWantWarp = false;
    P2Window window(this);
    procWait_init();
    fprintf(stderr, "[coop] player 2 warped to player 1\n");
    toast("Player 2 warped back", 90);
}

BOOL daCoopP2_c::execute() {
    fopAc_ac_c* link = dComIfGp_getLinkPlayer();
    if (link == NULL) {
        return TRUE;
    }
    if (sP2 != this) {
        new (mAttentionStore) dAttention_c(this, 0); // inside the window the pads are exchanged
        mAttentionMade = true;
        sP2 = this;
        fprintf(stderr, "[coop] player 2 is in the game\n");
        toast("Player 2 is in the game");
    }
    if (parked()) {
        mWasParked = true;
        return TRUE;
    }
    const cXyz toLink = link->current.pos - current.pos;
    const bool lost = toLink.absXZ() > kTetherXZ || fabsf(toLink.y) > kTetherY;
    if (mWantWarp || lost || mNoGroundFrames > kNoGroundFrames || (mWasParked && toLink.absXZ() > 300.0f)) {
        warpToP1();
    }
    mWasParked = false;
    {
        P2Window window(this);
        if (dAttention_c* attention = attentionOf(this)) {
            attention->Run(-1);
        }
        daPy_lk_c::execute();
    }
    mNoGroundFrames = mAcch.GetGroundH() == -G_CM3D_F_INF ? mNoGroundFrames + 1 : 0;
    return TRUE;
}

BOOL daCoopP2_c::draw() {
    if (hidden()) {
        return TRUE;
    }
    P2Window window(this);
    daPy_cupPackets_c* packets = daPy_cupPackets;
    daPy_cupPackets = &mCupPackets;
    daPy_lk_c::draw();
    daPy_cupPackets = packets;
    return TRUE;
}

BOOL daCoopP2_c::remove() {
    fprintf(stderr, "[coop] player 2 removed\n");
    if (sP2 == this) {
        sP2 = NULL;
        sP2Id = fpcM_ERROR_PROCESS_ID_e; // back in the next scene without the retry wait
    }
    if (mAttentionMade) {
        attentionOf(this)->~dAttention_c();
        mAttentionMade = false;
    }
    if (mPlayerInited) {
        P2Window window(this);
        playerDelete();
    }
    if (mArcMounted) {
        dComIfG_deleteObjectRes(mArcName);
    }
    return TRUE;
}

static cPhs_State daCoopP2_Create(fopAc_ac_c* i_this) {
    return static_cast<daCoopP2_c*>(i_this)->create();
}

static BOOL daCoopP2_Delete(daCoopP2_c* i_this) {
    return i_this->remove();
}

static BOOL daCoopP2_Execute(daCoopP2_c* i_this) {
    return i_this->execute();
}

static BOOL daCoopP2_IsDelete(daCoopP2_c*) {
    return TRUE;
}

static BOOL daCoopP2_Draw(daCoopP2_c* i_this) {
    return i_this->draw();
}

static actor_method_class l_daCoopP2_Method = {
    (process_method_func)daCoopP2_Create,
    (process_method_func)daCoopP2_Delete,
    (process_method_func)daCoopP2_Execute,
    (process_method_func)daCoopP2_IsDelete,
    (process_method_func)daCoopP2_Draw,
};

actor_process_profile_definition g_profile_COOP_P2 = {
    /* Layer ID     */ fpcLy_CURRENT_e,
    /* List ID      */ 5,
    /* List Prio    */ fpcPi_CURRENT_e,
    /* Proc Name    */ fpcNm_COOP_P2_e,
    /* Proc SubMtd  */ &g_fpcLf_Method.base,
    /* Size         */ sizeof(daCoopP2_c),
    /* Size Other   */ 0,
    /* Parameters   */ 0,
    /* Leaf SubMtd  */ &g_fopAc_Method.base,
    /* Draw Prio    */ fpcDwPi_PLAYER_e,
    /* Actor SubMtd */ &l_daCoopP2_Method,
    /* Status       */ fopAcStts_FREEZE_e,
    /* Group        */ fopAc_ACTOR_e,
    /* Cull Type    */ fopAc_CULLBOX_0_e,
};

void coop_uiFrame() {
    ImGuiIO& io = ImGui::GetIO();
    const float scale = io.DisplaySize.y / 720.0f * 1.5f;
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize;
    if (sToastFrames > 0) {
        sToastFrames--;
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.06f), ImGuiCond_Always,
                                ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.75f);
        if (ImGui::Begin("##coop_toast", nullptr, kFlags)) {
            ImGui::SetWindowFontScale(scale);
            ImGui::TextUnformatted(sToast);
        }
        ImGui::End();
    }
    for (int slot = 0; slot < 2; slot++) {
        if (sPicker[slot] < 0) {
            continue;
        }
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * (slot == 0 ? 0.25f : 0.75f), io.DisplaySize.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowBgAlpha(0.88f);
        char id[32];
        snprintf(id, sizeof(id), "##coop_picker%d", slot);
        if (ImGui::Begin(id, nullptr, kFlags)) {
            ImGui::SetWindowFontScale(scale);
            ImGui::Text("Player %d: pick a character", slot + 1);
            ImGui::Separator();
            for (int i = 0; i < COOP_CHAR_COUNT; i++) {
                const bool on = i == sPicker[slot];
                ImGui::TextColored(on ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f) : ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "%s %s",
                                   on ? ">" : " ", coop_charName(i));
            }
            ImGui::Separator();
            ImGui::TextUnformatted("A: choose   B: cancel");
        }
        ImGui::End();
    }
}

void coop_frame() {
    const bool pad2 = g_mDoCPd_cpadInfo[1].mGamepadErrorFlags == 0;
    if (pad2 != sPad2Connected) {
        sPad2Connected = pad2;
        fprintf(stderr, "[coop] controller 2 %s\n", pad2 ? "found" : "gone");
        if (pad2 && !sJoined) {
            toast("Controller 2 found: press + on it to join");
        }
    }
    fopAc_ac_c* link = dComIfGp_getLinkPlayer();
    const char* stage = dComIfGp_getStartStageName();
    const bool inPlay = link != NULL && stage[0] != '\0' && strcmp(stage, "sea_T") != 0 && strcmp(stage, "Name") != 0 &&
                        !dComIfGp_isEnableNextStage() && !dComIfGp_event_runCheck();
    sInPlayFrames = inPlay ? sInPlayFrames + 1 : 0;
    if (sInPlayFrames == kSettleFrames && !sPlayMessageShown) {
        sPlayMessageShown = true;
        toast(sPad2Connected ? "Co-op: + on controller 2 joins. ZL+ZR+Plus: change character"
                             : "Co-op: connect controller 2 to add a player. ZL+ZR+Plus: change character",
              360);
    }
    for (int slot = 0; slot < 2; slot++) {
        if (sPicker[slot] >= 0) {
            if (inPlay) {
                pickerInput(slot);
            } else {
                sPicker[slot] = -1;
            }
            continue;
        }
        interface_of_controller_pad& pad = g_mDoCPd_cpadInfo[slot];
        const bool combo = pad.mButtonTrig.start && pad.mHoldLockL && pad.mHoldLockR;
        if (sInPlayFrames >= kSettleFrames && (combo || (slot == 1 && pad.mButtonTrig.start))) {
            if (slot == 1 && !sJoined) {
                sJoined = true;
                fprintf(stderr, "[coop] player 2 joined\n");
            }
            sPicker[slot] = coop_charWanted(slot);
            pad.mButtonTrig.start = 0;
            pad.mButtonHold.start = 0;
            fprintf(stderr, "[coop] picker open for player %d\n", slot + 1);
        }
    }

    if (sP2Id != fpcM_ERROR_PROCESS_ID_e) {
        if (sP2 == NULL && !fpcM_IsCreating(sP2Id)) {
            sP2Id = fpcM_ERROR_PROCESS_ID_e; // deleted with its scene, or the create failed
            sRetryFrames = kRetryFrames;
        }
        return;
    }
    if (sRetryFrames > 0) {
        sRetryFrames--;
        return;
    }
    if (!sJoined || sInPlayFrames < kSettleFrames) {
        return;
    }
    // From the player's layer, as an actor the player spawned would be.
    cXyz pos = link->current.pos;
    csXyz angle(0, link->shape_angle.y, 0);
    layer_class* saved = fpcLy_CurrentLayer();
    fpcLy_SetCurrentLayer(static_cast<base_process_class*>(link)->mLyTg.mpLayer);
    sP2Id = fopAcM_create(fpcNm_COOP_P2_e, 0, &pos, fopAcM_GetRoomNo(link), &angle);
    fpcLy_SetCurrentLayer(saved);
    fprintf(stderr, "[coop] player 2 requested in %s (process %u)\n", stage, (unsigned)sP2Id);
}

#endif
