#include <windows.h>
#include <commctrl.h>
#include <list>
#include <set>
#include "HelpFunc.h"
#include "Define.h"
#include "PD2Support.hpp"
#include "d2h_module.hpp"
#include "d2ptrs.h"
#include "KeyModule.hpp"
#include "d2structs.h"
#include "d2vars.h"
#include "keyModule.hpp"
#include "log.h"
#include "D2Utils.hpp"
#include "Define.h"
#include "Task.hpp"
#include "Event.hpp"
#include "cConfigLoader.hpp"
#include "hotkey.hpp"

struct SkillTime {
    int         key;
    int         skillId;
    bool        isLeft;
    bool        isFake;     // don't run skill, just start monitoring
    uint64_t    startTime;

    void        clear()
    {
        skillId = -1;
    }

    bool        isExpired() const
    {
        return startTime + 1000 < GetTickCount64();
    }

    bool        isValid() const
    {
        return skillId != -1;
    }
};

#define fcDbg(fmt, ...)         do { if (fastCastDebug) D2Util::showInfo(fmt, ##__VA_ARGS__); } while(0)
#define trace(fmt, ...)         do { if (fastCastDebug) log_trace("%s:" fmt, __FUNCTION__, ##__VA_ARGS__); log_flush(); } while (0)

#define MIN_REPEAT_DELAY        (50)
#define DEFAULT_REPEAT_DELAY    (100)

enum {
    FastCastRepeatStopOnOtherSkill      = (1 << 0),
    FastCastRepeatStopOnLeftMouse       = (1 << 1),
    FastCastRepeatStopOnRightMouse      = (1 << 2),
    FastCastRepeatStopOnSameSkill       = (1 << 3),

    FastCastRepeatStopModeDefault       = 7,
};

static bool fastCastDebug;
static bool fastCastEnabled;
static bool fastCastQuickSwapBack = true;
static uint32_t fastCastToggleKey = Hotkey::Invalid;
static bool fastCastKeepAuraSkills = true;
static bool fastCastRepeatOnDown = true;
static uint32_t fastCastRepeatOnDownDelay = DEFAULT_REPEAT_DELAY;

static bool fastCastRepeatEnbled;
static uint32_t fastCastRepeatToggleKey = Hotkey::Invalid;
static uint32_t fastCastRepeatDelay = DEFAULT_REPEAT_DELAY;
static uint32_t fastCastRepeatStopMode = FastCastRepeatStopModeDefault;

static const std::set<int> defFastCastAttackSkills = {
    0,
    // amazon
    10, 14, 19, 24, 30, 34,

    // sor

    // nec

    // pal
    96, 97, 106,

    // Bar
    131, 142, 126, 133, 139, 144, 147, 151, 152,

    // Dru

    // Asn
    254, 255, 260, 259, 265, 270, 269, 247, 275, 280,
};
static std::set<int> fastCastAttackSkills;

static const std::set<int> defFastCastAuraSkills = {
    99, 100, 104, 105, 109, 110, 115, 120, 124, 125,
    98, 102, 103, 108, 113, 114, 118, 119, 122, 123,
};

static std::set<int> fastCastAuraSkills;

static std::set<int> fastCastRepeatSkills = {};

static WNDPROC getOrigProc(HWND hwnd)
{
    TCHAR       className[64];
    int         classNameLen;
    WNDCLASS    classInfo;
    HINSTANCE   hInst = GetModuleHandle(NULL);
    WNDPROC     orig;

    if (hInst == NULL) {
        return NULL;
    }
    // Get "Diablo II" class name
    classNameLen = GetClassName(hwnd, &className[0], sizeof(className) / sizeof(className[0]));
    if (classNameLen == 0) {
        trace("Failed to get class name\n");
        return FALSE;
    }
    if (!GetClassInfo(hInst, className, &classInfo)) {
        trace("Failed to get class info\n");
        return FALSE;
    }
    orig = classInfo.lpfnWndProc;
    trace("Orig proc: %p\n", orig);
    return orig;
}

__declspec(noinline)
LRESULT WINAPI mySendMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static WNDPROC      origProc;
    static BOOL         isGot;

    LRESULT             ret;

    trace("Get message 0x%x param 0x%x 0x%x\n", msg, wParam, lParam);
    if (!isGot) {
        origProc = getOrigProc(hWnd);
        isGot = TRUE;
    }
    if (origProc != NULL) {
        ret = CallWindowProcA(origProc, hWnd, msg, wParam, lParam);
    } else {
        ret = SendMessageA(hWnd, msg, wParam, lParam);
    }

    return ret;
}


static bool isInGameArea()
{
    LONG    x = MOUSE_POS->x;
    bool    mouseLeft = x <= SCREENSIZE.x / 2;

    if (mouseLeft) {
        if ((UI_PANEL_STATE & UiPanelOpenLeft) != 0) {
            return false;
        }
        return true;
    }

    if ((UI_PANEL_STATE & UiPanelOpenRight) != 0) {
        return false;
    }
    return true;
}

static bool needPause()
{
    return !isInGameArea();
}

static bool isFastCastAble()
{
    if (!fastCastEnabled) {
        return false;
    }
    return D2Util::isGameScreen();
}

class RunTask: public Task {
public:
    RunTask()
    {
    }

    void startSkill(const SkillTime& skill)
    {
        if (mIsRunning) {
            cancel();
        }
        mSkill = skill;
        run();
    }

private:
    bool                isLeft() const
    {
        return mSkill.isLeft;
    }
    int                getSkillId()
    {
        int skill;
        if (isLeft()) {
            skill = D2Util::getLeftSkillId();
        } else {
            skill = D2Util::getRightSkillId();
        }

        return skill;
    }

    virtual void        start() override
    {
        int origId = getSkillId();
        trace("Start skill %d, orig %d\n", mSkill.skillId, origId);
        mOrigSkill = origId;

        // For the map
        DefSubclassProc(origD2Hwnd, WM_KEYDOWN, mSkill.key, 0);
        DefSubclassProc(origD2Hwnd, WM_KEYUP, mSkill.key, 0);

        D2Util::setAndUpdateSkill(mSkill.skillId, isLeft());
        fcDbg(L"Set Skill to %d cur %d", mSkill.skillId, origId);
        //next(&RunTask::preCheck, 5);
        preCheck();
    }

    void                preCheck()
    {
        trace("start wait time");
        mTime.start();
        checkSkill();
    }

    void                checkSkill()
    {
        if (!isFastCastAble()) {
            return done();
        }

        int skillId = getSkillId();

        trace("wait skill %d, cur %d\n", mSkill.skillId, skillId);
        if (skillId != mSkill.skillId) {
            if (mTime.elapsed() >= 250) {
                log_verbose("Failed to set skill id to %d, cur %d\n", mSkill.skillId, skillId);
                return done();
            }
            return next(&RunTask::checkSkill, 1);
        }
        if (skillId == 52) {
            // Enchant needs some delay to select the unit
            return next(&RunTask::delayMouseDown, 300);
        }

        fcDbg(L"Skill %d set", mSkill.skillId);
        trace("wait skill %d done\n", mSkill.skillId);
        //next(&RunActor::sendMouseDown, 0);
        sendMouseDown();
    }

    void delayMouseDown()
    {
        sendMouseDown();
    }

    void sendMouseDown()
    {
        if (!D2Util::isGameScreen()) {
            return done();
        }

        bool isShift = false;
        if (isLeft()) {
            // If shift is held during a melee attack, the player cannot reach monsters
            // Thus force no-shift with melee attacks
            if (fastCastAttackSkills.contains(mSkill.skillId)) {
                isShift = false;
            } else {
                isShift = true;
            }
        }

        dispatchSendMouse(isShift);

        done();
    }

    void dispatchSendMouse(bool isShift)
    {
        if (PD2Enabled()) {
            // PD2 rewrites the message handler.
            // Use its own SendMessage hack
            PD2Click(isLeft()? PD2ClickType::Left : PD2ClickType::Right, isShift, MOUSE_POS->x, MOUSE_POS->y);
            return;
        }
        return mouseDownWithClickMap(isShift);

        #if 0
        // By default prefer sendMessage hack. It is more stable (so far...)
        if (!isShift) {
            mouseDownWithMessage();
            return;
        }

        int holdKey = D2Util::getHoldKey();
        if (holdKey == -1) {
            // user doesn't set the hold key.
            // Need to use clickMap
            return mouseDownWithClickMap(isShift);
        }

        DefSubclassProc(origD2Hwnd, WM_KEYDOWN, holdKey, 0);
        mouseDownWithMessage();
        DefSubclassProc(origD2Hwnd, WM_KEYUP, holdKey, 0);
        #endif
    }

    void mouseDownWithMessage()
    {
        DWORD pos = ((DWORD)MOUSE_POS->x) | (((DWORD)MOUSE_POS->y) << 16);
        if (isLeft()) {
            mySendMessage(origD2Hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pos);
            mySendMessage(origD2Hwnd, WM_LBUTTONUP, MK_LBUTTON, pos);
        } else {
            mySendMessage(origD2Hwnd, WM_RBUTTONDOWN, MK_RBUTTON, pos);
            mySendMessage(origD2Hwnd, WM_RBUTTONUP, MK_RBUTTON, pos);
        }
    }

    void mouseDownWithClickMap(bool isShift)
    {
        if (isLeft()) {
            // clickMap works without requring a hold key
            D2Util::sendClick(D2Util::LeftDown, MOUSE_POS->x, MOUSE_POS->y, isShift);

            // clickMap won't release the click until a delay is introduced.
            // but adding a delay will cause skill icon to blink.
            // Use the SendMessage trick instead
            DWORD pos = ((DWORD)MOUSE_POS->x) | (((DWORD)MOUSE_POS->y) << 16);
            mySendMessage(origD2Hwnd, WM_LBUTTONUP, MK_LBUTTON, pos);
            //D2Util::sendClick(D2Util::LeftUp, MOUSE_POS->x, MOUSE_POS->y, false);
        } else {
            D2Util::sendClick(D2Util::RightDown, MOUSE_POS->x, MOUSE_POS->y, false);
            D2Util::sendClick(D2Util::RightUp, MOUSE_POS->x, MOUSE_POS->y, false);
        }
    }

    void done()
    {
        complete();
    }

private:
    ElapsedTime mTime;
    SkillTime   mSkill;
    int         mOrigSkill;
};

class RestoreTask: public Task {
public:
    RestoreTask(bool isLeft):
        mIsLeft(isLeft), mIsMonitoring(false), mOrigSkillId(-1), mCurrentSwitch(0xff)
    {
    }

    void                clear()
    {
        cancel();
        mOrigSkillId = -1;
    }

    const char *        getType()
    {
        return mIsLeft? "LEFT" : "RIGHT";
    }

    void                save(bool isForce)
    {
        auto weaponSwitch = D2Util::getWeaponSwitch();

        if (mIsRunning) {
            trace("%s: PAUSED, cur %u", getType(),  mOrigSkillId);
        }
        cancel();
        if (isForce || mOrigSkillId == -1 || mCurrentSwitch != weaponSwitch) {
            mOrigSkillId = getSkillId();
            mCurrentSwitch = weaponSwitch;
            trace("%s: save skill to %u switch %u", getType(),  mOrigSkillId, mCurrentSwitch);
        }
    }

    void                resume()
    {
        trace("%s: resume to restore skill %u", getType(), mOrigSkillId);
        run();
    }

    void                onServerSkillUpdate(int nSkillId)
    {
        if (nSkillId == mOrigSkillId) {
            return;
        }
        if (isRunning() && mIsMonitoring) {
            mTimer.stop();
            execute();
        }
    }

private:
    virtual void        stop() override
    {
        Task::stop();
        mIsMonitoring = false;
    }

    int                 getSkillId()
    {
        if (mIsLeft) {
            return D2Util::getLeftSkillId();
        } else {
            return D2Util::getRightSkillId();
        }
    }

    void                setSkillId(int skillId)
    {
        D2Util::setAndUpdateSkill(skillId, mIsLeft);
    }

    bool                needAbort() const
    {
        if (mOrigSkillId == -1) {
            return true;
        }

        if (D2Util::getWeaponSwitch() != mCurrentSwitch) {
            fcDbg(L"Switch weapon, finish restore");
            return true;
        }

        // User is selecting a new skill or switching weapon
        if (D2Util::uiIsSet(UIVAR_CURRSKILL)) {
            trace("Selecting CURR skill, abort\n");
            return true;
        }

        return false;
    }

    virtual void        start() override
    {
        int     skillId = getSkillId();
        trace("%s: orig %d switch %d cur %d\n",
              getType(), mOrigSkillId, mCurrentSwitch, skillId);
        if (needAbort()) {
            return finishRestore();
        }
        if (skillId == 107) {
            // Special fix for charge
            // Need keep current skill as charge to move smoothly.
            next(&RestoreTask::startRestore, 600);
            return;
        }
        //next(&RestoreTask::startRestore, 50);
        startRestore();
    }

    void                startRestore()
    {
        mTime.start();
        mIsMonitoring = true;
        return sendRestore();
    }

    void                sendRestore()
    {
        if (needAbort()) {
            return finishRestore();
        }

        if (mTime.elapsed() >= 60000) {
            // give up after 60 seconds
            trace("%s: Restore failed after 60 seconds", getType());
            return finishRestore();
        }

        int skillId = getSkillId();
        if (skillId != mOrigSkillId) {
            // Retry
            trace("%s: set kill to %d cur %d\n", getType(), mOrigSkillId, getSkillId());
            setSkillId(mOrigSkillId);
            next(&RestoreTask::sendRestore, 1);
            return;
        }

        if (mTime.elapsed() <= 3000) {
            next(&RestoreTask::sendRestore, 10);
            return;
        }

        // Fix SK triggered by hackmap
        // This possible happens in 3 seconds.
        // Still some very corner case that fails to restore, but good enough now.
        if (mTime.elapsed() <= 30000) {
            next(&RestoreTask::sendRestore, 50);
            return;
        }
        finishRestore();
    }

    void                finishRestore()
    {
        mIsMonitoring = false;
        mOrigSkillId = -1;
        done();
    }

    void                done()
    {
        complete();
    }

private:
    bool                mIsLeft;
    bool                mIsMonitoring;
    int                 mOrigSkillId;
    BYTE                mCurrentSwitch;
    ElapsedTime         mTime;
};

class FastCastActor {
    enum State {
        Idle,
        Skill,
        Restore,
        DelayNext,
    };

    enum Result {
        Continue,
        Wait,
    };

public:
    static FastCastActor& inst()
    {
        static FastCastActor gFastCastActor;
        return gFastCastActor;
    }

    FastCastActor(): mState(Idle), mCrankCount(0),
                     mRestoreTasks{Right, Left}
    {
        auto f = [this](Event::Type, DWORD, DWORD) { stop(); };
        Event::add(Event::GameStart, f);
        Event::add(Event::GameEnd, f);

        mRunSkillTask.setComplete([this](Task *) {
            crank();
        });
    }

    void onServerSkillUpdate(bool isLeft, int nSkillId)
    {
        if (!fastCastEnabled) {
            return;
        }
        trace("Server set skill, left %d, skill %d\n", isLeft, nSkillId);
        if (isLeft) {
            mRestoreTasks[Left].onServerSkillUpdate(nSkillId);
        } else {
            mRestoreTasks[Right].onServerSkillUpdate(nSkillId);
        }
    }

    // call when game exit
    void  stop()
    {
        mRunSkillTask.cancel();
        for (auto& task: mRestoreTasks) {
            task.clear();
        }

        mState = Idle;
        mCrankCount = 0;
        mPendingSkills.clear();
    }

    void startSkill(const SkillTime& newSkill)
    {
        for (auto& sk: mPendingSkills) {
            if (sk.skillId == newSkill.skillId) {
                sk.startTime = newSkill.startTime;
                return;
            }
        }
        mPendingSkills.push_back(newSkill);

        crank();
    }


private:
    void crank()
    {
        int cur = mCrankCount;

        mCrankCount += 1;
        if (cur != 0) {
            trace("Crank: cur %d skip", cur);
            return;
        }

        for (; mCrankCount > 0; mCrankCount -= 1) {
            trace("Start crank cur %d, count %d\n", cur, mCrankCount);
            doCrank();
        }
    }

    void doCrank()
    {
        Result res = Continue;

        while (res != Wait) {
            switch (mState) {
            case Idle:
                res = handleIdle();
                break;
            case Skill:
                res = handleSkill();
                break;
            case Restore:
                res = handleRestore();
                break;
            default:
                log_trace("Unexpected state %d\n", mState);
                mState = Idle;
                return;
            }
        }
    }

    Result handleIdle()
    {
        trace("IDLE: empty %d\n", mPendingSkills.empty());
        if (mPendingSkills.empty()) {
            return Wait;
        }

        return startNextSkill();
    }

    Result startNextSkill()
    {
        mCurrentSkill = mPendingSkills.front();
        mPendingSkills.pop_front();

        saveSkill(false);

        if (!D2Util::isGameScreen() || mCurrentSkill.isFake) {
            mState = Restore;
            return Continue;
        }

        mRunSkillTask.startSkill(mCurrentSkill);
        mState = Skill;
        return Wait;
    }

    Result handleSkill()
    {
        trace("SKILL: running %d empty %d\n", mRunSkillTask.isRunning(), mPendingSkills.empty());
        if (mRunSkillTask.isRunning()) {
            return Wait;
        }

        mState = Restore;
        return Continue;
    }

    Result handleRestore()
    {
        if (fastCastKeepAuraSkills && fastCastAuraSkills.contains(mCurrentSkill.skillId)) {
            saveSkill(true);
        } else {
            restoreSkill();
        }

        if (!mPendingSkills.empty()) {
            return startNextSkill();
        }

        mState = Idle;
        return Continue;
    }

    int getRestoreIndex()
    {
        return mCurrentSkill.isLeft? Left : Right;
    }

    void saveSkill(bool isForce)
    {
        mRestoreTasks[getRestoreIndex()].save(isForce);
    }

    void restoreSkill()
    {
        mRestoreTasks[getRestoreIndex()].resume();
    }

private:
    enum {
        Right,
        Left,
    };

    State               mState;
    int                 mCrankCount;

    std::list<SkillTime> mPendingSkills;
    RunTask             mRunSkillTask;

    SkillTime           mCurrentSkill;

    RestoreTask         mRestoreTasks[2];
};

class RepeatSkillTask: public Task {
public:
    RepeatSkillTask()
    {
        mRepeatSkill.clear();
        auto f = [this](Event::Type, DWORD, DWORD) { clearAll(); };
        Event::add(Event::GameEnd, f);
        if (fastCastRepeatStopMode & FastCastRepeatStopOnLeftMouse) {
            Event::add(Event::LeftMouseDown, f);
        }
        if (fastCastRepeatStopMode & FastCastRepeatStopOnRightMouse) {
            Event::add(Event::RightMouseDown, f);
        }
    }

    static RepeatSkillTask& inst()
    {
        static RepeatSkillTask gRepeatSkillTask;
        return gRepeatSkillTask;
    }

    static bool         isRepeatable(int skill)
    {
        if (!fastCastRepeatEnbled) {
            return false;
        }
        if (fastCastAuraSkills.contains(skill)) {
            return false;
        }
        return fastCastRepeatSkills.contains(skill);
    }


    void                clearAll()
    {
        cancel();
        mRepeatSkill.clear();
    }

    void                toggle(BYTE key, int skill, bool isLeft)
    {
        if (!isRepeatable(skill)) {
            FastCastActor::inst().startSkill({key, skill, isLeft, false, GetTickCount64()});
            if (fastCastRepeatStopMode & FastCastRepeatStopOnOtherSkill) {
                cancel();
            } else {
                if (mTimer.isPending()) {
                    // If another skill is used, we delay 600 to avoid cold down
                    // charge need special handling
                    int         delay = skill == 107? 600 : 200;

                    mTimer.start(delay);
                }
            }
            return;
        }

        if (mRepeatSkill.skillId == skill && (fastCastRepeatStopMode & FastCastRepeatStopOnSameSkill)) {
            fcDbg(L"Stop repeat skill %d", skill);
            cancel();
            return;
        }

        fcDbg(L"Start repeat skill %d", skill);
        cancel();
        mRepeatSkill.key = key;
        mRepeatSkill.skillId = skill;
        mRepeatSkill.isLeft = isLeft;

        run();
    }

private:

    virtual void        start()
    {
        startSkill();
    }
    virtual void        stop()
    {
        mRepeatSkill.clear();
        Task::stop();
    }

    void                startSkill()
    {
        if (!mRepeatSkill.isValid()) {
            return complete();
        }
        if (!isFastCastAble()) {
            mRepeatSkill.clear();
            return complete();
        }

        if (!needPause()) {
            mRepeatSkill.startTime = GetTickCount64();
            FastCastActor::inst().startSkill(mRepeatSkill);
        }
        mDelay.start();
        next(&RepeatSkillTask::startSkill, fastCastRepeatDelay);
    }


private:
    ElapsedTime         mDelay;
    SkillTime           mRepeatSkill;
};

class RepeatKeyTask: public Task {
public:
    RepeatKeyTask():
        mKey(-1), mSkill(-1), mIsLeft(false)
    {
        auto f = [this](Event::Type, DWORD, DWORD) { cancel(); };
        Event::add(Event::GameEnd, f);
    }

    static RepeatKeyTask& inst()
    {
        static RepeatKeyTask gRepeatKeyTask;
        return gRepeatKeyTask;
    }

    void                runKey(BYTE key, int skillId, bool isLeft)
    {
        RepeatSkillTask::inst().toggle(key, skillId, isLeft);
        if (!fastCastRepeatOnDown) {
            return;
        }

        cancel();
        if (RepeatSkillTask::isRepeatable(skillId)) {
            // RepeatSkillTask will handle repeat itself
            return;
        }


        mKey = key;
        mSkill = skillId;
        mIsLeft = isLeft;
        run();
    }

private:
    bool                canContinue()
    {
        if (!fastCastRepeatOnDown) {
            return false;
        }
        if (mKey == -1) {
            return false;
        }
        if ((GetAsyncKeyState(mKey) & 0x8000) == 0) {
            return false;
        }
        if (!D2Util::isGameScreen()) {
            return false;
        }
        return true;
    }

    virtual void        start()
    {
        next(&RepeatKeyTask::checkKey, fastCastRepeatOnDownDelay);
    }

    virtual void        stop()
    {
        mKey = -1;
        mSkill = -1;
        mIsLeft = false;
        Task::stop();
    }

    void                checkKey()
    {
        if (!canContinue()) {
            return complete();
        }
        if (!needPause()) {
            RepeatSkillTask::inst().toggle(mKey, mSkill, mIsLeft);
        }
        next(&RepeatKeyTask::checkKey, fastCastRepeatOnDownDelay);
    }


private:
    int                 mKey;
    int                 mSkill;
    bool                mIsLeft;
};

static bool mapKeyToSkill(int key, int *outSkillId, bool * outIsLeft)
{
    int     func;

    if (PD2Enabled()) {
        func = PD2GetFunc(key);
    } else {
        func = D2Util::getKeyFunc(key);
    }

    if (func < 0) {
        return false;
    }

    bool    isLeft;
    int     skillId = D2Util::getSkillId(func, &isLeft);
    if (skillId < 0) {
        fcDbg(L"KEY %u -> %u (unbind)", key, func);
        return false;
    }

    fcDbg(L"KEY %u -> %u -> skill %u, left: %d\n", key, func, skillId, isLeft);
    *outSkillId = skillId;
    *outIsLeft = isLeft;
    return true;
}

static bool doFastCast(BYTE key, BYTE repeat)
{
    if (!fastCastEnabled) {
        return false;
    }
    trace("key: %d, repeat %d, swap 0x%x", key, repeat, WEAPON_SWITCH);

    // We will repeat the key down ourselves, so ignore this message
    if (repeat && fastCastRepeatOnDown) {
       return false;
    }

    int     skillId;
    bool    isLeft;
    bool    bound;

    bound = mapKeyToSkill(key, &skillId, &isLeft);

    if (!bound) {
        return false;
    }

    if (!D2Util::isGameScreen()) {
        return false;
    }
    if (!isInGameArea()) {
        //FastCastActor::inst().startSkill({key, skillId, isLeft, true, GetTickCount64()});
        return false;
    }

    RepeatKeyTask::inst().runKey(key, skillId, isLeft);

    return true;
}

static bool fastCastToggle(struct HotkeyConfig * config)
{
    if (fastCastEnabled) {
        FastCastActor::inst().stop();
        fastCastEnabled = false;
    } else {
        fastCastEnabled = true;
    }
    log_verbose("FastCast: toggle: %d\n", fastCastEnabled);
    D2Util::showInfo(L"QuickCast %s", fastCastEnabled? L"Enabled" : L"Disabled");

    if ((config->hotKey & (Hotkey::Ctrl | Hotkey::Alt)) != 0) {
        return true;
    }

    return false;
}

static bool fastCastRepeatToggle(struct HotkeyConfig * config)
{
    RepeatSkillTask::inst().clearAll();
    fastCastRepeatEnbled = !fastCastRepeatEnbled;

    log_verbose("FastRepeatCast: toggle: %d\n", fastCastRepeatEnbled);
    D2Util::showInfo(L"Fast Cast Auto Repeat %s", fastCastRepeatEnbled? L"Enabled" : L"Disabled");

    if ((config->hotKey & (Hotkey::Ctrl | Hotkey::Alt)) != 0) {
        return true;
    }

    return false;
}

static std::vector<int> fastCastLoadSkillList(Config::Section& section, const char * key)
{
    std::vector<std::string>    list;
    std::vector<int>            skills;

    section.loadList(key, list);
    for (auto& s: list) {
        bool ok;
        int skillId = helper::toInt(s, &ok);
        log_verbose("%s %s -> %d, ok %u", key, s.c_str(), skillId, ok);
        if (!ok) {
            continue;
        }
        skills.push_back(skillId);
    }
    return skills;
}

static void fastCastLoadAutoRepeatConfig()
{
    auto repeatSection = CfgLoad::section("helper.autorepeat");

    // Auto repeat skills
    fastCastRepeatEnbled = repeatSection.loadBool("enable", fastCastRepeatEnbled);

    auto keyString = repeatSection.loadString("toggleKey");
    fastCastRepeatToggleKey = Hotkey::parseKey(keyString);
    if (fastCastRepeatToggleKey != Hotkey::Invalid) {
        log_trace("FastCast: auto repeat toggle key %s -> 0x%llx\n",
                  keyString.c_str(), (unsigned long long)fastCastRepeatToggleKey);
    }
    fastCastRepeatDelay = repeatSection.loadInt("delay", 100);
    if (fastCastRepeatDelay < MIN_REPEAT_DELAY) {
        fastCastRepeatDelay = MIN_REPEAT_DELAY;
    }
    fastCastRepeatStopMode = repeatSection.loadInt("stopMode", fastCastRepeatStopMode);
    if (fastCastRepeatStopMode == 0) {
        fastCastRepeatStopMode = FastCastRepeatStopModeDefault;
    }
    auto repeatSkills = fastCastLoadSkillList(repeatSection, "skills");
    log_verbose("Repeat skills %zu", repeatSkills.size());
    fastCastRepeatSkills = {};
    for (auto skillId: repeatSkills) {
        if (skillId < 0) {
            continue;
        }
        fastCastRepeatSkills.insert(skillId);
    }
}

static void fastCastLoadConfig()
{
    auto section = CfgLoad::section("helper.fastcast");

    fastCastDebug = section.loadBool("debug", false);
    fastCastEnabled = section.loadBool("enable", true);

#ifdef PD2_SUPPORT
    auto pd2KeyTable = section.loadInt("PD2KeyTable", 0);
    PD2Setup(pd2KeyTable);
#endif

    fastCastQuickSwapBack = section.loadBool("quickSwapBack", true);
    fastCastKeepAuraSkills = section.loadBool("keepAuraSkills", true);
    fastCastRepeatOnDown = section.loadBool("repeatOnDown", true);
    fastCastRepeatOnDownDelay = section.loadInt("repeatOnDownDelay", DEFAULT_REPEAT_DELAY);
    if (fastCastRepeatOnDownDelay > MIN_REPEAT_DELAY) {
        fastCastRepeatOnDownDelay = MIN_REPEAT_DELAY;
    }

    auto keyString = section.loadString("toggleKey");

    fastCastToggleKey = Hotkey::parseKey(keyString);
    if (fastCastToggleKey != Hotkey::Invalid) {
        log_trace("FastCast: toggle key %s -> 0x%llx\n", keyString.c_str(), (unsigned long long)fastCastToggleKey);
    }

    auto auraSkills = fastCastLoadSkillList(section, "auraSkills");
    fastCastAuraSkills = defFastCastAuraSkills;
    for (auto skillId: auraSkills) {
        if (skillId < 0) {
            fastCastAuraSkills.erase(skillId);
        } else {
            fastCastAuraSkills.insert(skillId);
        }
    }

    auto attackSkills = fastCastLoadSkillList(section, "attackSkills");
    log_verbose("attack skills %zu", attackSkills.size());
    fastCastAttackSkills = defFastCastAttackSkills;
    for (auto skillId: attackSkills) {
        if (skillId < 0) {
            continue;
        }
        fastCastAttackSkills.insert(skillId);
    }

    auto castSkills = fastCastLoadSkillList(section, "castSkills");
    log_verbose("cast skills %zu", castSkills.size());
    for (auto skillId: castSkills) {
        if (skillId < 0) {
            continue;
        }
        fastCastAttackSkills.erase(skillId);
    }
    log_verbose("FastCast: enable: %d, debug %d\n", fastCastEnabled, fastCastDebug);

    fastCastLoadAutoRepeatConfig();
}

static void __stdcall setLeftActiveSkillHook(UnitAny * unit, int nSkillId, DWORD ownerGUID)
{
    D2SetLeftActiveSkill(unit, nSkillId, ownerGUID);
    FastCastActor::inst().onServerSkillUpdate(true, nSkillId);
}

static void __stdcall setRightActiveSkillHook(UnitAny * unit, int nSkillId, DWORD ownerGUID)
{
    D2SetRightActiveSkill(unit, nSkillId, ownerGUID);
    FastCastActor::inst().onServerSkillUpdate(false, nSkillId);
}

static void patch()
{
    static bool         isPatched;

    if (isPatched) {
        return;
    }
    // CPU Disasm
    // Address   Hex dump          Command                                                              Comments
    // 6FB5C7E0  |.  E8 29FDF5FF   CALL <JMP.&D2Common.#10546>                                          ; |\D2Common.#10546
    // 6FB5C7E5  |.  5F            POP EDI                                                              ; |
    // 6FB5C7E6  |.  5E            POP ESI                                                              ; |
    // 6FB5C7E7  |.  C3            RETN                                                                 ; |
    // base = D2Client(0x6FAB0000) offset = 0x6FB5C7E0 - base = 0xAC7E0
    DWORD offset = GetDllOffset("D2CLIENT.DLL", 0x6FB5C7E0 - DLLBASE_D2CLIENT);
    PatchCALL(offset, (DWORD)(uintptr_t)setLeftActiveSkillHook, 5);

    // CPU Disasm
    // Address   Hex dump          Command                                                              Comments
    // 6FB5C7ED  |.  E8 3AFDF5FF   CALL <JMP.&D2Common.#10978>                                          ; \D2Common.#10978
    // 6FB5C7F2  |>  5F            POP EDI
    // 6FB5C7F3  |.  5E            POP ESI
    // 6FB5C7F4  \.  C3            RETN
    // base = D2Client(0x6FAB0000) offset = 0x6FB5C7ED - base = 0xAC7ED
    offset = GetDllOffset("D2CLIENT.DLL", 0x6FB5C7ED - DLLBASE_D2CLIENT);
    PatchCALL(offset, (DWORD)(uintptr_t)setRightActiveSkillHook, 5);
    isPatched = true;
}

static void fastCastRegister()
{
    fastCastLoadConfig();
    keyRegisterHandler(doFastCast);

    if (fastCastToggleKey != Hotkey::Invalid) {
        keyRegisterHotkey(fastCastToggleKey, fastCastToggle, nullptr);
    }
    if (fastCastRepeatToggleKey != Hotkey::Invalid) {
        keyRegisterHotkey(fastCastRepeatToggleKey, fastCastRepeatToggle, nullptr);
    }

    if (fastCastQuickSwapBack) {
        patch();
    }
}

static int fastCastModuleInstall()
{
    FastCastActor::inst();
    RepeatSkillTask::inst();
    RepeatSkillTask::inst();

    fastCastRegister();

    return 0;
}

static void fastCastModuleUninstall()
{
}

static void fastCastModuleOnLoad()
{
}

static void fastCastModuleReload()
{
    FastCastActor::inst().stop();
    RepeatSkillTask::inst().clearAll();
    RepeatKeyTask::inst().cancel();
    fastCastRegister();
}

D2HModule fastCastModule = {
    "Fast Cast",
    fastCastModuleInstall,
    fastCastModuleUninstall,
    fastCastModuleOnLoad,
    fastCastModuleReload,
};
