// Couch co-op: a second local player (native/src/pc/features/pc_couch_coop.cpp).
#ifndef MODS_COUCH_COOP_H
#define MODS_COUCH_COOP_H

#if COS_WITH_COUCH_COOP

class fopAc_ac_c;
class dAttention_c;
class daPy_lk_c;
class J3DModel;
struct view_class;
struct view_port_class;
typedef struct Vec Vec;

// True while the camera following player 2 runs: it must not set up the draw pass's view.
extern bool g_coopInCamera2;

// True while player 2's code runs: it may not start events or change the stage.
extern bool g_coopInP2;
// Player 2's lock-on while player 2's code runs, else NULL.
extern dAttention_c* g_coopAttention;

// Once a frame, after the pads are read and before the game runs: player 2 joining and spawning.
void coop_frame();
// After the stock lock-on draw: player 2's.
void coop_drawAttention();
// Player 2 asked for a restart (fell out of the world): true means they are warped to player 1
// instead and the stock restart must not run.
bool coop_restartInstead(const fopAc_ac_c* player);

// The picker and the join messages, drawn with the options menu's ImGui frame.
void coop_uiFrame();

// Characters (pc_couch_coop_char.cpp): 0 is Link.
enum { COOP_CHAR_COUNT = 7 };
const char* coop_charName(int character);
int coop_charWanted(int slot);
void coop_charRequest(int slot, int character);
// Right after Link's model is calculated: poses the character and moves Link's matrices onto it.
void coop_charAfterCalc(daPy_lk_c* player);
// In Link's draw: true means a character is being played and Link's body must not be drawn.
bool coop_charDraw(daPy_lk_c* player, bool firstPerson);
J3DModel* coop_charShadowModel(daPy_lk_c* player, J3DModel* linkModel);
float coop_charRadiusScale(const daPy_lk_c* player);
void coop_charOnPlayerDelete(daPy_lk_c* player);

// The options menu's rows: each player's saved character, and split screen on or off.
void coop_settingsRegister();

// Split screen. The draw lists are painted once per half of the screen. Painting player 2's half
// moves everything by a view delta, the same way the 60 fps mode's repaint does.
bool coop_splitActive();
float coop_splitAspectScale();
void coop_splitViewport(int view, float* x, float* width);
view_class* coop_secondView();
void coop_paintDeltaOn(view_class* first);
void coop_paintDeltaOff();
void coop_paintSkyBegin(view_class* first);
void coop_paintSkyEnd(view_class* first);
void coop_paintParticlesBegin(view_class* first);
void coop_paintParticlesEnd(view_class* first);
// While splitting, an object is only culled when neither camera sees it.
extern const float (*g_coopClipDelta)[4];
int coop_clipSecond(const float (*m)[4], Vec* max, Vec* min);
int coop_clipSecond(const float (*m)[4], const Vec& center, float radius);

// Around the camera process's functions: the second camera runs with player 2's state in place.
class CoopCameraScope {
public:
    explicit CoopCameraScope(void* camera);
    ~CoopCameraScope();
    bool isP2() const { return mP2; }
    bool canRun() const;
    void drawn();
    void gone();

private:
    bool mP2;
    bool mWindow;
    int mWindowNum;
};

// Around an actor's execute: enemies see the nearer player, and player 2's own items (bombs and
// the like) see player 2.
class CoopActorScope {
public:
    explicit CoopActorScope(fopAc_ac_c* actor);
    ~CoopActorScope();

private:
    fopAc_ac_c* mPlayer;
    fopAc_ac_c* mLink;
    bool mActive;
};

#endif

#endif /* MODS_COUCH_COOP_H */
