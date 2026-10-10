#include "runtime/ps2_pad.h"
#include "runtime/host_input.h"
#include "raylib.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace ps2x_dev { std::atomic<uint32_t> g_padPressedMask{0}; }

namespace fake {
    std::array<bool, 512> keys{}, pressedKeys{}, releasedKeys{};
    std::array<bool, 8> mouse{}, pressedMouse{}, releasedMouse{};
    std::array<bool, 32> gamepadButtons{};
    std::array<float, 8> axes{};
    bool ready = true, focused = true, cursorHidden = false, gamepadAvailable = false;
    Vector2 delta{0,0}, mousePosition{320,240};
    uint64_t tick = 0;
    double time = 0;
    float wheel = 0;
    int exitKey = KEY_ESCAPE;
    std::thread::id uiThread;
    std::atomic<unsigned> hostCalls{0};
    std::atomic<bool> queriedOffUiThread{false};
    struct Block { int x, y, width, height; Color color; };
    std::vector<Block> blocks;

    void observed() {
        ++hostCalls;
        if (std::this_thread::get_id() != uiThread) queriedOffUiThread = true;
    }
    void frame(std::initializer_list<int> nextKeys = {},
               std::initializer_list<int> nextMouse = {}, Vector2 motion = {0,0}) {
        const auto oldKeys = keys;
        const auto oldMouse = mouse;
        keys.fill(false);
        mouse.fill(false);
        for (int k : nextKeys) keys.at(k) = true;
        for (int b : nextMouse) mouse.at(b) = true;
        for (size_t i=0; i<keys.size(); ++i) {
            pressedKeys[i] = keys[i] && !oldKeys[i];
            releasedKeys[i] = !keys[i] && oldKeys[i];
        }
        for (size_t i=0; i<mouse.size(); ++i) {
            pressedMouse[i] = mouse[i] && !oldMouse[i];
            releasedMouse[i] = !mouse[i] && oldMouse[i];
        }
        delta = motion;
        time += 1.0/60.0;
        ps2_host_input::poll(++tick);
        delta = {0,0};
        wheel = 0;
        pressedKeys.fill(false);
        releasedKeys.fill(false);
        pressedMouse.fill(false);
        releasedMouse.fill(false);
    }
}

extern "C" {
bool IsWindowReady(void) { fake::observed(); return fake::ready; }
bool IsWindowFocused(void) { fake::observed(); return fake::focused; }
bool IsWindowMinimized(void) { fake::observed(); return false; }
bool IsWindowState(unsigned int) { fake::observed(); return false; }
void SetExitKey(int key) { fake::observed(); fake::exitKey = key; }
bool IsKeyDown(int key) { fake::observed(); return key >=0 && key<512 && fake::keys[key]; }
bool IsKeyPressed(int key) { fake::observed(); return key >=0 && key<512 && fake::pressedKeys[key]; }
bool IsKeyReleased(int key) { fake::observed(); return key >=0 && key<512 && fake::releasedKeys[key]; }
bool IsKeyUp(int key) { return !IsKeyDown(key); }
bool IsMouseButtonDown(int button) { fake::observed(); return fake::mouse.at(button); }
bool IsMouseButtonPressed(int button) { fake::observed(); return fake::pressedMouse.at(button); }
bool IsMouseButtonReleased(int button) { fake::observed(); return fake::releasedMouse.at(button); }
bool IsMouseButtonUp(int button) { return !IsMouseButtonDown(button); }
Vector2 GetMouseDelta(void) { fake::observed(); return fake::delta; }
float GetMouseWheelMove(void) { fake::observed(); return fake::wheel; }
Vector2 GetMousePosition(void) { fake::observed(); return fake::mousePosition; }
void SetMousePosition(int x,int y) { fake::observed(); fake::mousePosition={float(x),float(y)}; }
void DisableCursor(void) { fake::observed(); fake::cursorHidden=true; }
void EnableCursor(void) { fake::observed(); fake::cursorHidden=false; }
void HideCursor(void) { fake::observed(); fake::cursorHidden=true; }
void ShowCursor(void) { fake::observed(); fake::cursorHidden=false; }
bool IsCursorHidden(void) { fake::observed(); return fake::cursorHidden; }
bool IsCursorOnScreen(void) { fake::observed(); return true; }
bool IsGamepadAvailable(int gamepad) { fake::observed(); return gamepad==0 && fake::gamepadAvailable; }
bool IsGamepadButtonDown(int gamepad,int button) { fake::observed(); return gamepad==0 && fake::gamepadButtons.at(button); }
float GetGamepadAxisMovement(int gamepad,int axis) { fake::observed(); return gamepad==0 ? fake::axes.at(axis) : 0.0f; }
double GetTime(void) { fake::observed(); return fake::time; }
int GetScreenWidth(void) { fake::observed(); return 640; }
int GetScreenHeight(void) { fake::observed(); return 480; }
void DrawRectangle(int x,int y,int width,int height,Color color) {
    fake::observed();
    fake::blocks.push_back({x,y,width,height,color});
}
}

namespace {
    constexpr uint16_t Up=0x10, Down=0x40, Left=0x80, Right=0x20;
    constexpr uint16_t Start=8, Select=1, Square=0x8000, Cross=0x4000;
    constexpr uint16_t Circle=0x2000, Triangle=0x1000, L1=0x400, R1=0x800;
    PSPadBackend pad;
    int assertions=0;
    void check(bool ok,const std::string &message) {
        ++assertions;
        if (!ok) throw std::runtime_error(message);
    }
    std::array<uint8_t,32> read() {
        std::array<uint8_t,32> result{};
        check(pad.readState(0,0,result.data(),result.size()),"pad snapshot read failed");
        return result;
    }
    uint16_t held(const std::array<uint8_t,32>& data) {
        return uint16_t(~uint16_t(data[2] | (uint16_t(data[3])<<8)));
    }
    uint16_t held() { return held(read()); }
    bool close(float a,float b) { return std::abs(a-b)<0.00002f; }
    void expireButtons() {
        fake::time += 0.11;
        fake::frame();
    }
    void menu() {
        ps2_host_input::setGameplayActive(false);
        fake::focused=true;
        expireButtons();
        ps2_host_input::discardMouseLook();
    }
    void capture() {
        menu();
        // Each test starts a new capture gesture. Separate assertions below
        // cover automatic restoration when closing an in-game menu.
        fake::focused=false;
        fake::frame();
        fake::focused=true;
        fake::frame();
        ps2_host_input::setGameplayActive(true);
        fake::frame({}, {MOUSE_BUTTON_LEFT});
        check(ps2_host_input::takeMouseLook().captured,"click on gameplay did not capture");
        check((held() & Square)==0,"capture click leaked as an attack");
        fake::frame({}, {MOUSE_BUTTON_LEFT});
        check((held() & Square)==0,"held capture click leaked before release");
        fake::frame();
        ps2_host_input::discardMouseLook();
    }

    void keyBindings() {
        menu();
        const std::pair<int,uint16_t> cases[] = {
            {KEY_W,Up},{KEY_S,Down},{KEY_A,L1},{KEY_D,R1},
            {KEY_UP,Up},{KEY_DOWN,Down},{KEY_LEFT,Left},{KEY_RIGHT,Right},
            {KEY_SPACE,Square},{KEY_F,Cross},{KEY_Q,Triangle},
            {KEY_TAB,Circle},{KEY_ESCAPE,Circle},{KEY_ENTER,Start},{KEY_RIGHT_SHIFT,Select}
        };
        for (auto c : cases) {
            fake::frame({c.first});
            check(held()==c.second,"incorrect Verdite binding for key "+std::to_string(c.first));
            fake::frame();
            if ((c.second & uint16_t(Select|Start|Square|Cross|Circle|Triangle))==0)
                check(held()==0,"released movement button stayed pressed");
            expireButtons();
            check(held()==0,"released keyboard command exceeded its 100 ms latch");
        }
        fake::frame({KEY_W,KEY_A,KEY_SPACE,KEY_F,KEY_Q,KEY_TAB,KEY_ENTER});
        check(held()==uint16_t(Up|L1|Square|Cross|Triangle|Circle|Start),"simultaneous keyboard bindings lost input");
    }

    void shortTaps() {
        menu();
        fake::frame({KEY_F});
        fake::frame();
        check(held()==Cross,"short confirmation tap was lost before a slow guest read");
        for (int i=0;i<30;++i) check(held()==Cross,"guest reads consumed the command latch");
        expireButtons();
        check(held()==0,"short confirmation tap did not expire");
        fake::frame({KEY_W,KEY_A});
        fake::frame();
        check(held()==0,"movement keys were extended by the command latch");
    }

    void controllerMerge() {
        menu();
        fake::gamepadAvailable=true;
        fake::gamepadButtons.fill(false);
        fake::axes.fill(0);
        fake::frame({KEY_F,KEY_A});
        check(held()==uint16_t(Cross|L1),"idle connected controller suppressed keyboard");
        expireButtons();
        fake::gamepadButtons[GAMEPAD_BUTTON_RIGHT_FACE_LEFT]=true;
        fake::gamepadButtons[GAMEPAD_BUTTON_MIDDLE_RIGHT]=true;
        fake::axes[GAMEPAD_AXIS_RIGHT_X]=0.5f;
        fake::axes[GAMEPAD_AXIS_LEFT_Y]=-0.5f;
        fake::frame({KEY_W,KEY_Q});
        const auto data=read();
        check(held(data)==uint16_t(Square|Start|Up|Triangle),"controller and keyboard were not merged");
        check(data[4]>128 && data[7]<128,"controller analog axes were dropped");
        fake::gamepadButtons.fill(false);
        fake::axes.fill(0);
        fake::gamepadAvailable=false;
        fake::frame();
    }

    void mouseContexts() {
        menu();
        fake::frame({}, {MOUSE_BUTTON_LEFT});
        check(held()==Cross,"menu left click did not confirm");
        check(!ps2_host_input::takeMouseLook().captured,"menu confirmation captured the cursor");
        expireButtons();
        fake::frame({}, {MOUSE_BUTTON_RIGHT});
        check(held()==Circle,"menu right click did not cancel");
        capture();
        fake::frame({}, {MOUSE_BUTTON_LEFT,MOUSE_BUTTON_RIGHT,MOUSE_BUTTON_MIDDLE});
        check(held()==uint16_t(Square|Triangle|Cross),"captured mouse buttons do not match attack/magic/use");
        fake::frame();
        fake::focused=false;
        fake::frame({}, {}, {200,100});
        auto released=ps2_host_input::takeMouseLook();
        check(!released.captured && !fake::cursorHidden,"focus loss did not release cursor capture");
        check(released.yawRadians==0 && released.pitchRadians==0,"focus loss retained mouse motion");
        fake::focused=true;
        fake::frame();
        check(!ps2_host_input::takeMouseLook().captured,"focus regain recaptured without a click");
        capture();
        ps2_host_input::setGameplayActive(false);
        fake::frame();
        check(!ps2_host_input::takeMouseLook().captured && !fake::cursorHidden,"menu transition did not release pointer");
    }

    void mouseWheel() {
        menu();
        fake::wheel=1.0f;
        fake::frame();
        check(held()==Up,"menu wheel up did not navigate up");
        fake::frame();
        check(held()==0,"menu wheel up stayed pressed");
        fake::wheel=-1.0f;
        fake::frame();
        check(held()==Down,"menu wheel down did not navigate down");
        capture();
        fake::wheel=1.0f;
        fake::frame();
        check(held()==0,"captured gameplay wheel injected a pad direction");
        fake::wheel=-1.0f;
        fake::frame();
        check(held()==0,"captured gameplay wheel down injected a pad direction");
    }

    void captureRestoration() {
        capture();
        ps2_host_input::setGameplayActive(false);
        expireButtons();
        fake::frame({}, {MOUSE_BUTTON_LEFT});
        check(held()==Cross,"menu confirmation did not register before gameplay restoration");
        ps2_host_input::setGameplayActive(true);
        fake::frame({}, {MOUSE_BUTTON_LEFT});
        check(ps2_host_input::takeMouseLook().captured,"closing a gameplay menu did not restore capture");
        check((held() & Square)==0,"held menu confirmation leaked as an attack after capture restored");
        fake::frame({}, {MOUSE_BUTTON_LEFT});
        check((held() & Square)==0,"held menu confirmation leaked before its release");
        expireButtons();
        fake::frame({}, {MOUSE_BUTTON_LEFT});
        check((held() & Square)!=0,"fresh attack click was blocked after restored capture");
        expireButtons();
        fake::frame({KEY_ESCAPE});
        check((held() & Circle)!=0 && !ps2_host_input::takeMouseLook().captured,
              "Escape did not emit back and release gameplay capture");
    }

    void mouseDisplacement() {
        capture();
        fake::frame({}, {}, {100,50});
        const auto readsBefore=fake::hostCalls.load();
        for (int i=0;i<200;++i) read();
        check(fake::hostCalls.load()==readsBefore,"pad reads queried physical input instead of published snapshot");
        constexpr float unit=0.15f*3.14159265358979323846f/180.0f;
        const auto first=ps2_host_input::takeMouseLook();
        check(first.captured,"mouse displacement was not captured");
        check(close(first.yawRadians,100*unit),"mouse yaw is not positive 0.15 degrees per pixel");
        check(close(first.pitchRadians,-50*unit),"mouse pitch is not negative 0.15 degrees per pixel");
        const auto second=ps2_host_input::takeMouseLook();
        check(second.yawRadians==0 && second.pitchRadians==0,"mouse displacement was consumed twice");
        fake::frame({}, {}, {-100,-50});
        const auto reverse=ps2_host_input::takeMouseLook();
        check(close(reverse.yawRadians,-first.yawRadians) && close(reverse.pitchRadians,-first.pitchRadians),"opposite mouse movement did not reverse camera displacement");
        fake::frame({}, {}, {0.25f,0.5f});
        fake::frame({}, {}, {0.5f,0.25f});
        const auto accumulated=ps2_host_input::takeMouseLook();
        check(close(accumulated.yawRadians,0.75f*unit) && close(accumulated.pitchRadians,-0.75f*unit),
              "fractional mouse motion did not accumulate across UI polls");
        fake::frame({}, {}, {23,17});
        ps2_host_input::discardMouseLook();
        const auto dropped=ps2_host_input::takeMouseLook();
        check(dropped.yawRadians==0 && dropped.pitchRadians==0,"discard retained accumulated mouse displacement");
        fake::frame({}, {}, {10000,-10000});
        const auto clamped=ps2_host_input::takeMouseLook();
        check(close(std::abs(clamped.yawRadians),3.14159265358979323846f/2),"large mouse yaw was not capped at 90 degrees");
        check(close(std::abs(clamped.pitchRadians),3.14159265358979323846f/2),"large mouse pitch was not capped at 90 degrees");
        fake::frame({}, {}, {20,20});
        std::this_thread::sleep_for(std::chrono::milliseconds(275));
        const auto stale=ps2_host_input::takeMouseLook();
        check(stale.yawRadians==0 && stale.pitchRadians==0,"mouse motion survived a 250 ms guest stall");
    }

    void mouseIndicator() {
        capture();
        fake::time += 0.16;
        fake::frame({}, {}, {20,10});
        const auto before=fake::hostCalls.load();
        const auto engaged=ps2_host_input::mouseCaptureIndicator();
        check(engaged.captured && close(engaged.alpha,1.0f),"captured mouse indicator did not finish fading in");
        check(fake::hostCalls.load()==before,"mouse indicator query called a device API");
        const auto motion=ps2_host_input::takeMouseLook();
        check(motion.yawRadians>0 && motion.pitchRadians<0,"mouse indicator consumed camera displacement");
        fake::blocks.clear();
        ps2_host_input::drawMouseCaptureIndicator(100,50,640,480);
        check(fake::blocks.size()==126,"engaged icon differs from Verdite2's 63-cell mouse and shadow");
        auto whiteAt=[](int x,int y) {
            for (const auto &b : fake::blocks)
                if (b.x==x && b.y==y && b.color.r==255 && b.color.g==255 && b.color.b==255) return true;
            return false;
        };
        check(whiteAt(711,64),"mouse indicator was not placed at the game picture's top-right cable cell");
        check(whiteAt(705,82),"captured icon unexpectedly contained the released diagonal cut");
        bool blocksInPicture=true, integerCells=true;
        for (const auto &b : fake::blocks) {
            blocksInPicture &= b.x>=100 && b.x+b.width<=740 && b.y>=50 && b.y+b.height<=530;
            integerCells &= b.width==3 && b.height==3;
        }
        check(blocksInPicture && integerCells,"mouse icon escaped the game picture or lost whole-pixel cells");
        check(fake::blocks.front().color.r==0 && fake::blocks.front().color.a==140 &&
              fake::blocks.back().color.r==255 && fake::blocks.back().color.a==255,
              "mouse icon lost its translucent black shadow beneath the white glyph");

        fake::frame({KEY_ESCAPE});
        check(!ps2_host_input::mouseCaptureIndicator().captured,"Escape indicator reported mouse engagement after release");
        check(close(ps2_host_input::mouseCaptureIndicator().alpha,0.0f),"release did not restart the indicator fade");
        fake::time += 0.16;
        fake::frame();
        fake::blocks.clear();
        ps2_host_input::drawMouseCaptureIndicator(100,50,640,480);
        check(fake::blocks.size()==102,"released icon differs from Verdite2's 51-cell diagonal-cut mouse and shadow");
        check(!whiteAt(705,82) && whiteAt(711,64),"released diagonal removed the cable or left the off cut filled");

        ps2_host_input::setGameplayActive(false);
        check(!ps2_host_input::mouseCaptureIndicator().captured,"menu/pause gate left the mouse indicator engaged");
        fake::frame();
        ps2_host_input::setGameplayActive(true);
        fake::frame();
        check(ps2_host_input::mouseCaptureIndicator().captured,"resuming gameplay did not restore the engaged indicator");
        fake::focused=false;
        fake::frame();
        check(!ps2_host_input::mouseCaptureIndicator().captured,"focus loss left the mouse indicator engaged");
        fake::focused=true;
        fake::frame();
        check(!ps2_host_input::mouseCaptureIndicator().captured,"focus regain falsely showed captured mouse look");

        capture();
        fake::time += 1.35;
        fake::frame();
        const auto fading=ps2_host_input::mouseCaptureIndicator();
        check(fading.captured && fading.alpha>0 && fading.alpha<1,"mouse indicator did not fade out after its hold");
        fake::time += 0.5;
        fake::frame();
        fake::blocks.clear();
        ps2_host_input::drawMouseCaptureIndicator(100,50,640,480);
        check(ps2_host_input::mouseCaptureIndicator().captured && fake::blocks.empty(),
              "mouse indicator stayed drawn after 1.66 seconds or changed capture while fading");
    }

    void snapshotAndThreads() {
        menu();
        fake::frame({KEY_W,KEY_A});
        fake::keys.fill(false);
        fake::keys[KEY_S]=true;
        const unsigned before=fake::hostCalls.load();
        check(held()==uint16_t(Up|L1),"physical changes leaked into a guest read before the next UI poll");
        check(fake::hostCalls.load()==before,"guest read called host APIs");
        // The EE/render clock can stop in a menu or between expensive frames;
        // window input must still update on each UI poll at the same guest tick.
        ps2_host_input::poll(fake::tick);
        check(held()==Down,"UI poll stopped updating keys while guest vsync tick was unchanged");
        fake::frame({KEY_W,KEY_A});
        std::atomic<bool> finish{false}, bad{false}, guestReady{false};
        std::atomic<unsigned> guestReads{0};
        std::thread guest([&] {
            std::array<uint8_t,32> data{};
            guestReady=true;
            while (!finish.load(std::memory_order_relaxed)) {
                if (!pad.readState(0,0,data.data(),data.size())) { bad=true; break; }
                ++guestReads;
                const auto mask=held(data);
                if (mask!=uint16_t(Up|L1) && mask!=uint16_t(Down|R1)) { bad=true; break; }
            }
        });
        while (!guestReady.load()) std::this_thread::yield();
        for (int i=0;i<2000;++i) {
            if (i%2) fake::frame({KEY_W,KEY_A});
            else fake::frame({KEY_S,KEY_D});
        }
        finish=true;
        guest.join();
        check(guestReads>0,"snapshot concurrency test did not execute a guest read");
        check(!bad,"concurrent guest observed a torn input snapshot");
        check(!fake::queriedOffUiThread,"guest thread called a Raylib input API");
    }
}

int main() {
    fake::uiThread=std::this_thread::get_id();
    try {
        ps2_host_input::enableVerditeControls();
        keyBindings();
        shortTaps();
        controllerMerge();
        mouseContexts();
        mouseWheel();
        captureRestoration();
        mouseDisplacement();
        mouseIndicator();
        snapshotAndThreads();
        std::cout << "PASS: " << assertions << " checks; Verdite bindings, mixed devices, capture, mouse indicator, mouse displacement and UI/guest snapshots\n";
        return 0;
    } catch(const std::exception &e) {
        std::cerr << "FAIL after " << assertions << " checks: " << e.what() << '\n';
        return 1;
    }
}
