// Couch co-op: playing as another character. Link's model still runs every animation and the player
// code, but is not drawn; another character's model is posed from it each frame.
//
// Every model in the cast is authored in the same T-pose facing +z, so a joint's world rotation
// carries over directly: R_target = R_link_animated * R_link_rest^T * R_target_rest. Positions come
// from the target's own bone lengths, so bodies keep their proportions. Link's matrices are then
// moved onto the target's joints, which puts held items, the collision cylinder's height, the head
// top and the hands where the character really is. Each arc is a private copy, so the joint
// callbacks set here never reach the same character standing around as an NPC.
#include "d/dolzel.h" // IWYU pragma: keep
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphBase/J3DTransform.h"
#include "d/actor/d_a_player_main.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_mtx.h"
#include "mods/couch_coop.h"

#include <cstdio>
#include <cstring>

#if COS_WITH_COUCH_COOP

namespace {

struct JointLink {
    const char* target;
    const char* link;
};
struct JointAttach {
    const char* target; // a joint of the extra model
    const char* body;   // the body joint it follows
};

const JointLink kAryllKomali[] = {
    {"center", "center"},          {"backbone", "chest_jnt"},  {"neck", "neck_jnt"},    {"head", "head_jnt"},
    {"shoulderL", "Lshoulder_jnt"}, {"armL1", "LarmA_jnt"},    {"armL2", "LarmB_jnt"},  {"handL", "cl_LhandA"},
    {"shoulderR", "Rshoulder_jnt"}, {"armR1", "RarmA_jnt"},    {"armR2", "RarmB_jnt"},  {"handR", "cl_RhandA"},
    {"waist", "waist_jnt"},        {"legL1", "LlegA_jnt"},     {"legL2", "LlegB_jnt"},  {"footL", "Lfoot_jnt"},
    {"legR1", "RlegA_jnt"},        {"legR2", "RlegB_jnt"},     {"footR", "Rfoot_jnt"},  {NULL, NULL},
};
const JointLink kTetra[] = {
    {"stomach", "stomach_jnt"},     {"chest", "chest_jnt"},    {"neck", "neck_jnt"},    {"head", "head_jnt"},
    {"shoulder_L", "Lshoulder_jnt"}, {"arm_L1", "LarmA_jnt"},  {"arm_L2", "LarmB_jnt"}, {"zl_hand_l", "cl_LhandA"},
    {"shoulder_R", "Rshoulder_jnt"}, {"arm_R1", "RarmA_jnt"},  {"arm_R2", "RarmB_jnt"}, {"zl_hand_r", "cl_RhandA"},
    {"waist", "waist_jnt"},         {"leg_L0", "Lclotch_jnt"}, {"leg_L1", "LlegA_jnt"}, {"leg_L2", "LlegB_jnt"},
    {"foot_L", "Lfoot_jnt"},        {"leg_R0", "Rclotch_jnt"}, {"leg_R1", "RlegA_jnt"}, {"leg_R2", "RlegB_jnt"},
    {"foot_R", "Rfoot_jnt"},        {NULL, NULL},
};
const JointLink kLaruto[] = {
    {"backbone", "chest_jnt"},     {"neck", "neck_jnt"},      {"head", "head_jnt"},   {"shoulderL", "Lshoulder_jnt"},
    {"armL1", "LarmA_jnt"},        {"armL2", "LarmB_jnt"},    {"handL", "cl_LhandA"}, {"shoulderR", "Rshoulder_jnt"},
    {"armR1", "RarmA_jnt"},        {"armR2", "RarmB_jnt"},    {"handR", "cl_RhandA"}, {"waist", "waist_jnt"},
    {"legL2", "LlegA_jnt"},        {"footL", "Lfoot_jnt"},    {"legR2", "RlegA_jnt"}, {"footR", "Rfoot_jnt"},
    {NULL, NULL},
};
const JointLink kMedli[] = {
    {"center", "center"},           {"backbone1", "stomach_jnt"}, {"backbone2", "chest_jnt"}, {"neck", "neck_jnt"},
    {"head", "head_jnt"},           {"shoulderL", "Lshoulder_jnt"}, {"armL", "LarmA_jnt"},
    {"shoulderR", "Rshoulder_jnt"}, {"armR", "RarmA_jnt"},        {"waist", "waist_jnt"},
    {"legL1", "LlegA_jnt"},         {"legL2", "LlegB_jnt"},       {"footL", "Lfoot_jnt"},
    {"legR1", "RlegA_jnt"},         {"legR2", "RlegB_jnt"},       {"footR", "Rfoot_jnt"},
    {"armL1", "LarmA_jnt"},         {"armL2", "LarmB_jnt"},       {"handL", "cl_LhandA"},
    {"armR1", "RarmA_jnt"},         {"armR2", "RarmB_jnt"},       {"handR", "cl_RhandA"},
    {NULL, NULL},
};
const JointLink kMakar[] = {
    {"backbone", "chest_jnt"},     {"sholderL", "Lshoulder_jnt"}, {"armL1", "LarmA_jnt"}, {"armL2", "LarmB_jnt"},
    {"sholderR", "Rshoulder_jnt"}, {"armR1", "RarmA_jnt"},        {"armR2", "RarmB_jnt"}, {"armRend", "cl_RhandA"},
    {"waist", "waist_jnt"},        {"legL", "LlegA_jnt"},         {"legR", "RlegA_jnt"},  {NULL, NULL},
};
const JointAttach kMedliAttach[] = {{"armLloc", "armL"}, {"armRloc", "armR"}, {NULL, NULL}};
const JointAttach kMakarAttach[] = {{"world_root", "backbone"}, {NULL, NULL}};
const JointAttach kAryllAttach[] = {{"ls_handL", "handL"}, {"ls_handR", "handR"}, {NULL, NULL}};

struct CharDef {
    const char* name;
    const char* sourceArc; // NULL: Link himself
    const char* mountName;
    const char* body;
    const char* extra;
    const JointLink* map;
    const JointAttach* attach;
    const char* handL;
    const char* handR;
    const char* chest;
    const char* waist;
    const char* hide;
    f32 itemScale;
    f32 radiusScale;
};

const CharDef kChars[COOP_CHAR_COUNT] = {
    {"Link", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 1.0f, 1.0f},
    {"Medli", "Md", "CcMd", "md.bdl", "mdarm.bdl", kMedli, kMedliAttach, "handL", "handR", "backbone2", "waist", NULL,
     1.0f, 0.9f},
    {"Makar", "Cb", "CcCb", "cb.bdl", "cb_face.bdl", kMakar, kMakarAttach, "armL2", "armRend", "backbone", "waist",
     NULL, 0.55f, 0.65f},
    {"Prince Komali", "Co", "CcCo", "co.bdl", NULL, kAryllKomali, NULL, "handL", "handR", "backbone", "waist", NULL,
     0.85f, 0.85f},
    {"Tetra", "Zl", "CcZl", "zl.bdl", NULL, kTetra, NULL, "zl_hand_l", "zl_hand_r", "chest", "waist", NULL, 1.0f, 1.0f},
    {"Aryll", "Ls", "CcLs", "ls.bdl", "lshand.bdl", kAryllKomali, kAryllAttach, "handL", "handR", "backbone", "waist",
     NULL, 0.8f, 0.8f},
    {"Laruto", "Demo27", "CcZs", "zs.bdl", NULL, kLaruto, NULL, "handL", "handR", "backbone", "waist", "zs_harp", 1.25f,
     1.1f},
};

const int kMaxJoints = 64;
const u32 kModelHeapSize = 0x60000;

struct CharJoint {
    s8 model;
    s16 jnt;
    s16 parent;
    s16 attach; // a joint of this list it copies, or -1
    s16 link;   // Link's joint it follows, or -1
    Mtx rest;   // model space
    Mtx local;  // relative to the parent
    f32 fromLink[3][3]; // R_link_rest^T * R_target_rest
};

struct CharInstance {
    int def = 0;
    JKRSolidHeap* heap = NULL;
    J3DModel* model[2] = {NULL, NULL};
    s16 index[2][kMaxJoints];
    CharJoint joints[kMaxJoints];
    Mtx anim[kMaxJoints];
    int count = 0;
    int root = 0;
    int handL = -1, handR = -1, chest = -1;
    int linkHandL = -1, linkHandR = -1, linkChest = -1, linkCenter = -1, linkBack = -1, linkPod = -1;
    f32 linkCenterRest[3];
    f32 bodyScale = 1.0f;
    f32 handDropY = 0.0f;
};

int sWanted[2] = {0, 0};
int sFailed[2] = {0, 0}; // the character that could not be made, so it is tried once
CharInstance sInstance[2];
bool sMounted[COOP_CHAR_COUNT] = {};

int slotOf(const daPy_lk_c* player) {
    return fopAcM_GetName(const_cast<daPy_lk_c*>(player)) == fpcNm_COOP_P2_e ? 1 : 0;
}

void mul33(const f32 a[3][3], const f32 b[3][3], f32 out[3][3]) {
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
        }
    }
}

void rot33(const Mtx m, f32 out[3][3]) {
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            out[r][c] = m[r][c];
        }
    }
}

void mulPoint(const Mtx m, const f32 p[3], f32 out[3]) {
    for (int r = 0; r < 3; r++) {
        out[r] = m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3];
    }
}

// Rest pose by walking the joint tree: outRest[jnt], outParent[jnt], in model space.
void walkRest(J3DNode* node, const Mtx parent, int parentJnt, Mtx* outRest, s16* outParent, s16* order, int* count) {
    for (; node != NULL; node = node->getYounger()) {
        J3DJoint* joint = static_cast<J3DJoint*>(node);
        const int jnt = joint->getJntNo();
        Mtx local;
        J3DGetTranslateRotateMtx(joint->getTransformInfo(), local);
        cMtx_concat(const_cast<MtxP>(parent), local, outRest[jnt]);
        outParent[jnt] = (s16)parentJnt;
        order[(*count)++] = (s16)jnt;
        walkRest(node->getChild(), outRest[jnt], jnt, outRest, outParent, order, count);
    }
}

int findJoint(const CharInstance& inst, const char* name) {
    for (int m = 1; m >= 0; m--) {
        if (inst.model[m] == NULL) {
            continue;
        }
        const int jnt = inst.model[m]->getModelData()->getJointName()->getIndex(name);
        if (jnt >= 0 && jnt < kMaxJoints && inst.index[m][jnt] >= 0) {
            return inst.index[m][jnt];
        }
    }
    return -1;
}

static int charNodeCallBack(J3DNode* node, int timing) {
    if (timing == J3DNodeCBCalcTiming_In) {
        J3DModel* model = j3dSys.getModel();
        CharInstance* inst = reinterpret_cast<CharInstance*>(model->getUserArea());
        if (inst != NULL) {
            const int m = model == inst->model[1] ? 1 : 0;
            const int jnt = static_cast<J3DJoint*>(node)->getJntNo();
            const int i = jnt < kMaxJoints ? inst->index[m][jnt] : -1;
            if (i >= 0) {
                cMtx_copy(inst->anim[i], J3DSys::mCurrentMtx);
                model->setAnmMtx(jnt, inst->anim[i]);
            }
        }
    }
    return 1;
}

void destroyInstance(int slot) {
    CharInstance& inst = sInstance[slot];
    if (inst.heap != NULL) {
        mDoExt_destroySolidHeap(inst.heap);
        inst.heap = NULL;
    }
    inst.model[0] = inst.model[1] = NULL;
    inst.def = 0;
}

// Adds one model's joints to the list, parents first.
bool addModel(CharInstance& inst, int m, J3DModelData* data) {
    const int num = data->getJointNum();
    if (inst.count + num > kMaxJoints) {
        return false;
    }
    Mtx identity;
    MTXIdentity(identity);
    static Mtx rest[kMaxJoints];
    static s16 parent[kMaxJoints], order[kMaxJoints];
    int visited = 0;
    walkRest(data->getRootNode(), identity, -1, rest, parent, order, &visited);
    if (visited != num) {
        return false;
    }
    for (int i = 0; i < num; i++) {
        const int jnt = order[i];
        CharJoint& joint = inst.joints[inst.count];
        joint.model = (s8)m;
        joint.jnt = (s16)jnt;
        joint.parent = parent[jnt] >= 0 ? inst.index[m][parent[jnt]] : (s16)-1;
        joint.attach = -1;
        joint.link = -1;
        cMtx_copy(rest[jnt], joint.rest);
        inst.index[m][jnt] = (s16)inst.count;
        inst.count++;
        data->getJointNodePointer(jnt)->setCallBack(charNodeCallBack);
    }
    return true;
}

// Rest poses of an attached joint and its children start at the body joint.
void resolveRest(CharInstance& inst) {
    for (int i = 0; i < inst.count; i++) {
        CharJoint& joint = inst.joints[i];
        if (joint.attach >= 0) {
            cMtx_copy(inst.joints[joint.attach].rest, joint.rest);
            MTXIdentity(joint.local);
        } else if (joint.parent >= 0) {
            CharJoint& parent = inst.joints[joint.parent];
            Mtx inverse;
            cMtx_inverse(parent.rest, inverse);
            cMtx_concat(inverse, joint.rest, joint.local);
        } else {
            cMtx_copy(joint.rest, joint.local);
        }
    }
}

} // namespace

const char* coop_charName(int character) {
    return character >= 0 && character < COOP_CHAR_COUNT ? kChars[character].name : "";
}

int coop_charWanted(int slot) {
    return sWanted[slot];
}

// Starts loading the private copy of the character's archive; it stays mounted from then on.
void coop_charRequest(int slot, int character) {
    if (character < 0 || character >= COOP_CHAR_COUNT) {
        return;
    }
    sWanted[slot] = character;
    sFailed[slot] = 0;
    const CharDef& def = kChars[character];
    if (def.sourceArc == NULL || sMounted[character]) {
        return;
    }
    dRes_control_c& control = g_dComIfG_gameInfo.mResControl;
    if (control.getObjectResInfo(def.mountName) == NULL) {
        dRes_info_c* info = dRes_control_c::newResInfo(control.mObjectInfo, ARRAY_SIZE(control.mObjectInfo));
        if (info == NULL || !info->set(def.sourceArc, "/res/Object/", JKRArchive::DEFAULT_MOUNT_DIRECTION, NULL)) {
            fprintf(stderr, "[coop] could not mount %s for %s\n", def.sourceArc, def.name);
            sFailed[slot] = character;
            return;
        }
        strncpy(info->getArchiveName(), def.mountName, 8);
        info->incCount();
    }
    sMounted[character] = true;
    fprintf(stderr, "[coop] loading %s for player %d\n", def.name, slot + 1);
}

// Access to Link's private parts for the character code.
struct CoopCharLink {
    static J3DModel* model(daPy_lk_c* p) { return p->mpCLModel; }
    static J3DModelData* data(daPy_lk_c* p) { return p->mpCLModelData; }
    static bool anchored(daPy_lk_c* p) {
        return p->checkModeFlg(daPy_lk_c::ModeFlg_HANG | daPy_lk_c::ModeFlg_CLIMB | daPy_lk_c::ModeFlg_LADDER |
                               daPy_lk_c::ModeFlg_ROPE | daPy_lk_c::ModeFlg_SWIM);
    }
    static void entry(daPy_lk_c* p, J3DModel* model) { p->updateDLSetLight(model, p->checkFreezeState()); }
    static int cameraIdx(daPy_lk_c* p) { return p->mCameraInfoIdx; }
};

namespace {

bool createInstance(int slot, daPy_lk_c* player, int character) {
    CharInstance& inst = sInstance[slot];
    const CharDef& def = kChars[character];
    const int loading = dComIfG_syncObjectRes(def.mountName);
    if (loading > 0) {
        return false; // still loading
    }
    sFailed[slot] = character; // anything below that fails is not tried again
    if (loading < 0) {
        fprintf(stderr, "[coop] %s failed to load\n", def.name);
        return false;
    }
    J3DModelData* bodyData = (J3DModelData*)dComIfG_getObjectRes(def.mountName, def.body);
    J3DModelData* extraData = def.extra != NULL ? (J3DModelData*)dComIfG_getObjectRes(def.mountName, def.extra) : NULL;
    if (bodyData == NULL || (def.extra != NULL && extraData == NULL)) {
        fprintf(stderr, "[coop] %s: no model in its archive\n", def.name);
        return false;
    }
    inst.heap = mDoExt_createSolidHeapFromGameToCurrent(kModelHeapSize, 0);
    if (inst.heap == NULL) {
        fprintf(stderr, "[coop] %s: no heap for the model\n", def.name);
        return false;
    }
    inst.model[0] = mDoExt_J3DModel__create(bodyData, 0x80000, 0x11000022);
    inst.model[1] = extraData != NULL ? mDoExt_J3DModel__create(extraData, 0x80000, 0x11000022) : NULL;
    mDoExt_restoreCurrentHeap();
    mDoExt_adjustSolidHeap(inst.heap);
    if (inst.model[0] == NULL || (extraData != NULL && inst.model[1] == NULL)) {
        fprintf(stderr, "[coop] %s: could not create the model\n", def.name);
        destroyInstance(slot);
        return false;
    }
    memset(inst.index, -1, sizeof(inst.index));
    inst.count = 0;
    inst.def = character;
    inst.model[0]->setUserArea((uintptr_t)&inst);
    if (inst.model[1] != NULL) {
        inst.model[1]->setUserArea((uintptr_t)&inst);
    }
    if (!addModel(inst, 0, bodyData) || (extraData != NULL && !addModel(inst, 1, extraData))) {
        fprintf(stderr, "[coop] %s: too many joints\n", def.name);
        destroyInstance(slot);
        return false;
    }
    for (const JointAttach* a = def.attach; a != NULL && a->target != NULL; a++) {
        const int jnt = extraData->getJointName()->getIndex(a->target);
        const int body = bodyData->getJointName()->getIndex(a->body);
        if (jnt < 0 || body < 0) {
            fprintf(stderr, "[coop] %s: no joint %s or %s\n", def.name, a->target, a->body);
            destroyInstance(slot);
            return false;
        }
        inst.joints[inst.index[1][jnt]].attach = inst.index[0][body];
    }
    resolveRest(inst);

    // Link's rest pose.
    J3DModelData* linkData = CoopCharLink::data(player);
    static Mtx linkRest[kMaxJoints];
    static s16 linkParent[kMaxJoints], linkOrder[kMaxJoints];
    Mtx identity;
    MTXIdentity(identity);
    int visited = 0;
    walkRest(linkData->getRootNode(), identity, -1, linkRest, linkParent, linkOrder, &visited);
    JUTNameTab* linkNames = linkData->getJointName();

    for (const JointLink* map = def.map; map->target != NULL; map++) {
        const int i = findJoint(inst, map->target);
        const int link = linkNames->getIndex(map->link);
        if (i < 0 || link < 0) {
            fprintf(stderr, "[coop] %s: no joint %s or %s\n", def.name, map->target, map->link);
            destroyInstance(slot);
            return false;
        }
        CharJoint& joint = inst.joints[i];
        joint.link = (s16)link;
        f32 linkRot[3][3], linkT[3][3], targetRot[3][3];
        rot33(linkRest[link], linkRot);
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                linkT[r][c] = linkRot[c][r];
            }
        }
        rot33(joint.rest, targetRot);
        mul33(linkT, targetRot, joint.fromLink);
    }
    inst.root = 0;
    inst.handL = findJoint(inst, def.handL);
    inst.handR = findJoint(inst, def.handR);
    inst.chest = findJoint(inst, def.chest);
    const int waist = findJoint(inst, def.waist);
    inst.linkHandL = linkNames->getIndex("cl_LhandA");
    inst.linkHandR = linkNames->getIndex("cl_RhandA");
    inst.linkChest = linkNames->getIndex("chest_jnt");
    inst.linkCenter = linkNames->getIndex("center");
    inst.linkBack = linkNames->getIndex("cl_back");
    inst.linkPod = linkNames->getIndex("cl_podA");
    const int linkWaist = linkNames->getIndex("waist_jnt");
    if (inst.handL < 0 || inst.handR < 0 || inst.chest < 0 || waist < 0 || inst.linkHandL < 0 || inst.linkHandR < 0 ||
        inst.linkChest < 0 || inst.linkCenter < 0 || inst.linkBack < 0 || inst.linkPod < 0 || linkWaist < 0) {
        fprintf(stderr, "[coop] %s: missing a key joint\n", def.name);
        destroyInstance(slot);
        return false;
    }
    for (int r = 0; r < 3; r++) {
        inst.linkCenterRest[r] = linkRest[inst.linkCenter][r][3];
    }
    // The body's bounce scales with the height of the hips.
    inst.bodyScale = inst.joints[waist].rest[1][3] / linkRest[linkWaist][1][3];
    // Hanging from a ledge or swimming, the hands stay where Link's are.
    inst.handDropY = linkRest[inst.linkHandL][1][3] - inst.joints[inst.handL].rest[1][3];
    if (def.hide != NULL) {
        const int hide = bodyData->getJointName()->getIndex(def.hide);
        if (hide >= 0) {
            for (J3DMaterial* m = bodyData->getJointNodePointer(hide)->getMesh(); m != NULL; m = m->getNext()) {
                m->getShape()->hide();
            }
        }
    }
    sFailed[slot] = 0;
    fprintf(stderr, "[coop] player %d is %s (%d joints, hips x%.2f)\n", slot + 1, def.name, inst.count,
            inst.bodyScale);
    return true;
}

// The target's pose from Link's, then Link's matrices moved onto the target.
void poseInstance(CharInstance& inst, daPy_lk_c* player) {
    J3DModel* cl = CoopCharLink::model(player);
    const Mtx& base = cl->getBaseTRMtx();
    f32 baseRot[3][3];
    rot33(base, baseRot);

    const MtxP center = cl->getAnmMtx(inst.linkCenter);
    f32 centerRest[3];
    mulPoint(base, inst.linkCenterRest, centerRest);
    const f32 dropY = CoopCharLink::anchored(player) ? inst.handDropY : 0.0f;

    for (int i = 0; i < inst.count; i++) {
        CharJoint& joint = inst.joints[i];
        Mtx& out = inst.anim[i];
        if (joint.attach >= 0) {
            cMtx_copy(inst.anim[joint.attach], out);
            continue;
        }
        if (joint.parent < 0) {
            // A root: the body's position, its rest offset plus Link's movement away from rest.
            f32 pos[3];
            f32 restPos[3] = {joint.rest[0][3], joint.rest[1][3], joint.rest[2][3]};
            mulPoint(base, restPos, pos);
            f32 rot[3][3], restRot[3][3];
            rot33(joint.rest, restRot);
            mul33(baseRot, restRot, rot);
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++) {
                    out[r][c] = rot[r][c];
                }
                out[r][3] = pos[r] + inst.bodyScale * (center[r][3] - centerRest[r]);
            }
            out[1][3] += dropY;
        } else {
            cMtx_concat(inst.anim[joint.parent], joint.local, out);
        }
        if (joint.link >= 0) {
            f32 linkRot[3][3], rot[3][3];
            rot33(cl->getAnmMtx(joint.link), linkRot);
            mul33(linkRot, joint.fromLink, rot);
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++) {
                    out[r][c] = rot[r][c];
                }
            }
        }
    }

    const CharDef& def = kChars[inst.def];
    // Back mounts (shield, sheath) keep their offset from the chest, scaled to the body.
    const MtxP linkChest = cl->getAnmMtx(inst.linkChest);
    f32 chestOld[3] = {linkChest[0][3], linkChest[1][3], linkChest[2][3]};
    const int mounts[2] = {inst.linkBack, inst.linkPod};
    for (int m = 0; m < 2; m++) {
        MtxP mount = cl->getAnmMtx(mounts[m]);
        for (int r = 0; r < 3; r++) {
            mount[r][3] = inst.anim[inst.chest][r][3] + (mount[r][3] - chestOld[r]) * def.itemScale;
        }
    }
    for (int i = 0; i < inst.count; i++) {
        const CharJoint& joint = inst.joints[i];
        if (joint.link >= 0) {
            MtxP m = cl->getAnmMtx(joint.link);
            for (int r = 0; r < 3; r++) {
                m[r][3] = inst.anim[i][r][3];
            }
        }
    }
    // Items are held at the character's hands, scaled through the hand's matrix.
    const int handTarget[2] = {inst.handL, inst.handR};
    const int handLink[2] = {inst.linkHandL, inst.linkHandR};
    for (int h = 0; h < 2; h++) {
        MtxP m = cl->getAnmMtx(handLink[h]);
        for (int r = 0; r < 3; r++) {
            m[r][0] *= def.itemScale;
            m[r][1] *= def.itemScale;
            m[r][2] *= def.itemScale;
            m[r][3] = inst.anim[handTarget[h]][r][3];
        }
    }
    inst.model[0]->setBaseTRMtx(const_cast<MtxP>(base));
    inst.model[0]->calc();
    if (inst.model[1] != NULL) {
        inst.model[1]->setBaseTRMtx(const_cast<MtxP>(base));
        inst.model[1]->calc();
    }
}

} // namespace

void coop_charAfterCalc(daPy_lk_c* player) {
    const int slot = slotOf(player);
    CharInstance& inst = sInstance[slot];
    const int wanted = sWanted[slot];
    if (inst.def != wanted) {
        destroyInstance(slot);
    }
    if (wanted == 0) {
        return;
    }
    if (inst.def == 0 && (sFailed[slot] == wanted || !createInstance(slot, player, wanted))) {
        return;
    }
    poseInstance(inst, player);
}

bool coop_charDraw(daPy_lk_c* player, bool firstPerson) {
    CharInstance& inst = sInstance[slotOf(player)];
    if (inst.def == 0) {
        return false;
    }
    if (!firstPerson &&
        !dComIfGp_checkCameraAttentionStatus(CoopCharLink::cameraIdx(player), dCamAttnStts_00000020_e)) {
        dComIfGd_setListP1();
        CoopCharLink::entry(player, inst.model[0]);
        if (inst.model[1] != NULL) {
            CoopCharLink::entry(player, inst.model[1]);
        }
    }
    return true;
}

J3DModel* coop_charShadowModel(daPy_lk_c* player, J3DModel* linkModel) {
    CharInstance& inst = sInstance[slotOf(player)];
    return inst.def != 0 ? inst.model[0] : linkModel;
}

f32 coop_charRadiusScale(const daPy_lk_c* player) {
    return kChars[sInstance[slotOf(player)].def].radiusScale;
}

void coop_charOnPlayerDelete(daPy_lk_c* player) {
    destroyInstance(slotOf(player));
}

#endif
