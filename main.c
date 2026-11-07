// BMX Streets Lite v2 - 2D BMX для PSP (pspdev / PSPSDK)
//
// УПРАВЛЕНИЕ
//  Земля:
//    Долбёжка X ............ разгон
//    Стик вниз -> вверх .... банихоп (+ держать X = высокий банихоп)
//    Крестовина лево/право . разворот на 180 (так можно поехать фейки)
//    Фейки + Треугольник ... руль на 90 градусов, слайд
//       стик влево/вправо в слайде = выход из слайда лицом вперёд
//  Воздух:
//    L (держать) ........... поджать ноги (байк чуть выше)
//    R (держать) ........... руки (отпустить руль)
//    L + X ................. кренкфлип
//    L + O ................. вип
//    стик вверх + L + O .... футджем вип
//    R + O ................. бар
//    стик вниз + R + O ..... чикенбар
//    R + X ................. (пока не назначено)
//    Крестовина лево/право . 180, 360 ... (каждое нажатие = +180)
//    Крестовина вверх/вниз . фронтфлип / бэкфлип
//  START ..................... справка
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

PSP_MODULE_INFO("BMX Streets Lite", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

#define BUFW 512
#define SCRW 480
#define SCRH 272
#define FRAME_SIZE (BUFW * SCRH * 4)
#define DT (1.0f / 60.0f)
#define G 520.0f
#define PI 3.1415927f
#define PI2 6.2831853f
#define COL(r, g, b) (0xFF000000u | ((b) << 16) | ((g) << 8) | (r))

/* ---------- системные колбэки ---------- */
static int exit_callback(int a, int b, void *c) { sceKernelExitGame(); return 0; }
static int cb_thread(SceSize args, void *argp)
{
    int id = sceKernelCreateCallback("exit", exit_callback, NULL);
    sceKernelRegisterExitCallback(id);
    sceKernelSleepThreadCB();
    return 0;
}
static void setup_callbacks(void)
{
    int t = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, 0, 0);
    if (t >= 0) sceKernelStartThread(t, 0, 0);
}

/* ---------- графика ---------- */
static unsigned int *draw_buf;
static int draw_idx = 0;

static void swap_buffers(void)
{
    sceDisplayWaitVblankStart();
    sceDisplaySetFrameBuf((void *)(0x04000000 + draw_idx * FRAME_SIZE), BUFW,
                          PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
    draw_idx ^= 1;
    draw_buf = (unsigned int *)(0x44000000 + draw_idx * FRAME_SIZE);
    pspDebugScreenSetOffset(draw_idx * FRAME_SIZE);
}

static void put(int x, int y, unsigned c)
{
    if (x >= 0 && x < SCRW && y >= 0 && y < SCRH) draw_buf[y * BUFW + x] = c;
}

static void rect(int x, int y, int w, int h, unsigned c)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > SCRW ? SCRW : x + w, y1 = y + h > SCRH ? SCRH : y + h;
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) draw_buf[j * BUFW + i] = c;
}

static void line(int x0, int y0, int x1, int y1, unsigned c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ---------- уровень ---------- */
#define LEN 3200.0f
static const float pts[][2] = {
    {0, 220},    {500, 220},  {650, 150},  {655, 220},
    {1000, 220}, {1060, 170}, {1260, 170}, {1320, 220},
    {1700, 220}, {1860, 140}, {1865, 220},
    {2400, 220}, {2480, 180}, {2600, 180}, {2680, 220},
    {2800, 220}, {2900, 195}, {3000, 220}, {3200, 220}
};
#define NPTS ((int)(sizeof(pts) / sizeof(pts[0])))

static float groundY(float x, float *slope)
{
    float wx = fmodf(x, LEN);
    if (wx < 0) wx += LEN;
    for (int i = 0; i < NPTS - 1; i++) {
        if (wx >= pts[i][0] && wx <= pts[i + 1][0]) {
            float dx = pts[i + 1][0] - pts[i][0];
            float dy = pts[i + 1][1] - pts[i][1];
            if (slope) *slope = dy / dx;
            return pts[i][1] + dy * (wx - pts[i][0]) / dx;
        }
    }
    if (slope) *slope = 0;
    return 220;
}

static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float approach(float c, float t, float r)
{
    if (c < t) { c += r; if (c > t) c = t; }
    else if (c > t) { c -= r; if (c < t) c = t; }
    return c;
}

/* ---------- состояние ---------- */
enum { T_NONE, T_WHIP, T_CRANK, T_FOOTJAM, T_BAR, T_CHICKEN };
static const char *tname[] = {"", "WHIP", "CRANKFLIP", "FOOTJAM WHIP", "BAR", "CHICKENBAR"};
static const int tpts[] = {0, 100, 130, 150, 120, 180};
static const float tdur[] = {0, 0.5f, 0.55f, 0.5f, 0.45f, 0.5f};

static float x = 100, y = 220, vx = 0, vy = 0;
static float angBase = 0, flipCur = 0, flipTgt = 0;     /* наклон + флипы */
static float yawCur = 0, yawTgt = 0, yawTakeoff = 0;     /* развороты (0 или PI = лицом назад) */
static float tuck = 0, armUp = 0;                        /* ноги / руки */
static int air = 0, slide = 0;
static int pending = 0, score = 0, combo = 0;
static float trickT = 0, trickDur = 0, comboT = 0, crashT = 0, msgT = 0, camx = 0;
static float downT = 0;
static int downHeld = 0, trickType = 0;
static const char *lastName = "";
static char msg[64] = "";

static int facing(void) { return cosf(yawTgt) > 0 ? 1 : -1; }

static void bank(int total, const char *name)
{
    combo = comboT > 0 ? combo + 1 : 1;
    score += total * combo;
    comboT = 3.0f;
    snprintf(msg, sizeof(msg), "%s+%d x%d", name, total, combo);
    msgT = 1.6f;
}

static void respawn(void)
{
    float s;
    for (int k = 0; k < 400; k++) {
        groundY(x - k * 5, &s);
        if (fabsf(s) < 0.001f) { x -= k * 5; break; }
    }
    y = groundY(x, &s);
    vx = vy = 0; air = 0; slide = 0; trickT = 0; pending = 0;
    angBase = flipCur = flipTgt = 0;
    yawCur = yawTgt = 0; tuck = armUp = 0;
}

static void takeoff(void)
{
    air = 1; flipCur = flipTgt = 0; yawTakeoff = yawTgt; trickT = 0; pending = 0;
}

static void end_slide(int out)
{
    slide = 0;
    if (out) { yawTgt += PI; bank(300, "FAKIE SLIDE OUT "); }
    else bank(100, "SLIDE ");
}

static void start_trick(int t) { trickType = t; trickDur = trickT = tdur[t]; }

static void do_land(float gy, float s)
{
    float tgt = atanf(clampf(s, -1.2f, 1.2f));
    float d = remainderf(angBase - tgt, PI2);
    float fe = fabsf(flipTgt - flipCur), ye = fabsf(yawTgt - yawCur);
    y = gy; air = 0;
    if (trickT > 0 || fabsf(d) > 0.75f || fe > 0.7f || ye > 0.9f) {
        crashT = 1.2f; pending = 0; combo = 0; comboT = 0; vx = 0; vy = 0;
        snprintf(msg, sizeof(msg), "CRASH!"); msgT = 1.2f;
        return;
    }
    int flips = (int)(fabsf(flipTgt) / PI2 + 0.5f);
    int spins = (int)(fabsf(yawTgt - yawTakeoff) / PI + 0.5f);
    int total = pending + flips * 300 + spins * 150;
    if (total > 0) {
        char nm[48]; int o = 0; nm[0] = 0;
        if (pending > 0) o += snprintf(nm + o, sizeof(nm) - o, "%s ", lastName);
        if (flips) o += snprintf(nm + o, sizeof(nm) - o, "FLIP ");
        if (spins && o < (int)sizeof(nm) - 8) snprintf(nm + o, sizeof(nm) - o, "%d ", spins * 180);
        bank(total, nm);
    }
    pending = 0;
    angBase = tgt; flipCur = flipTgt = 0;
    yawTgt = (((int)lroundf(yawTgt / PI)) & 1) ? PI : 0;
    yawCur = yawTgt;
}

static void update(unsigned btn, unsigned pressed, int lx, int ly)
{
    float s;
    int stickUp = ly < 64, stickDown = ly > 192, stickL = lx < 64, stickR = lx > 192;
    int L = (btn & PSP_CTRL_LTRIGGER) != 0, R = (btn & PSP_CTRL_RTRIGGER) != 0;

    if (msgT > 0) msgT -= DT;
    yawCur = approach(yawCur, yawTgt, 11.0f * DT);
    flipCur = approach(flipCur, flipTgt, 9.0f * DT);
    tuck = approach(tuck, (air && L) ? 1.0f : 0.0f, 10.0f * DT);
    armUp = approach(armUp, (R && (air || trickT > 0)) ? 1.0f : 0.0f, 10.0f * DT);

    if (crashT > 0) {
        angBase += 8 * DT;
        crashT -= DT;
        if (crashT <= 0) respawn();
        return;
    }

    if (!air) {
        int f = facing();
        int fakie = (f * vx < -15);
        groundY(x, &s);
        float sc = clampf(s, -1.2f, 1.2f);

        if (slide) {
            vx -= (vx > 0 ? 1 : -1) * 160 * DT;
            if (fabsf(vx) < 8) { vx = 0; end_slide(0); }
            else if (stickL || stickR) end_slide(1);
            else if (!(btn & PSP_CTRL_TRIANGLE)) end_slide(0);
        } else {
            if (pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) yawTgt += PI;
            if (pressed & PSP_CTRL_CROSS) vx += 24;
            if ((pressed & PSP_CTRL_TRIANGLE) && fakie && fabsf(vx) > 20) slide = 1;
        }
        vx -= vx * 0.45f * DT;
        vx += G * sc / (1 + sc * sc) * DT;
        vx = clampf(vx, -80, 280);
        if (comboT > 0) { comboT -= DT; if (comboT <= 0) combo = 0; }

        /* банихоп: стик вниз -> вверх */
        if (stickDown) { downHeld = 1; downT = 0.45f; }
        else if (downHeld) { downT -= DT; if (downT <= 0) downHeld = 0; }

        if (downHeld && stickUp && !slide) {
            float hop = (btn & PSP_CTRL_CROSS) ? 300.0f : 190.0f;
            downHeld = 0;
            takeoff();
            vy = vx * sc - hop; y -= 2; x += vx * DT; y += vy * DT;
        } else {
            vy = vx * sc;
            x += vx * DT; y += vy * DT;
            float s2, gy = groundY(x, &s2);
            if (y < gy - 5) takeoff();
            else { y = gy; angBase = atanf(clampf(s2, -1.2f, 1.2f)); }
        }
    } else {
        int f = facing();
        vy += G * DT; x += vx * DT; y += vy * DT;

        float tr = clampf(atan2f(vy, fmaxf(vx, 30)), -1.0f, 1.0f);
        angBase = approach(angBase, tr, 2.5f * DT);

        /* крестовина: развороты и флипы */
        if (pressed & PSP_CTRL_LEFT) yawTgt -= PI;
        if (pressed & PSP_CTRL_RIGHT) yawTgt += PI;
        if (pressed & PSP_CTRL_UP) flipTgt += f * PI2;
        if (pressed & PSP_CTRL_DOWN) flipTgt -= f * PI2;

        /* трюки */
        if (trickT <= 0) {
            if (pressed & PSP_CTRL_CIRCLE) {
                if (L && stickUp) start_trick(T_FOOTJAM);
                else if (L) start_trick(T_WHIP);
                else if (R && stickDown) start_trick(T_CHICKEN);
                else if (R) start_trick(T_BAR);
            } else if (pressed & PSP_CTRL_CROSS) {
                if (L) start_trick(T_CRANK);
                /* R + X: пока не назначено */
            }
        } else {
            trickT -= DT;
            if (trickT <= 0) {
                trickT = 0;
                pending += tpts[trickType];
                lastName = tname[trickType];
                snprintf(msg, sizeof(msg), "%s", lastName); msgT = 0.8f;
            }
        }

        float gy = groundY(x, &s);
        if (y >= gy) do_land(gy, s);
    }
}

/* ---------- рисование ----------
   Вся отрисовка райдера в draw_rider(): сюда подключаются модельки/спрайты. */
static float g_xs = 1, g_oy = 0;

static void T(float cx, float cy, float a, float px, float py, int *ox, int *oy)
{
    px *= g_xs; py += g_oy;
    float c = cosf(a), s = sinf(a);
    *ox = (int)(cx + px * c - py * s);
    *oy = (int)(cy + px * s + py * c);
}
static void LN(float cx, float cy, float a, float x0, float y0, float x1, float y1, unsigned c)
{
    int ax, ay, bx, by;
    T(cx, cy, a, x0, y0, &ax, &ay);
    T(cx, cy, a, x1, y1, &bx, &by);
    line(ax, ay, bx, by, c);
}
static void WH(float cx, float cy, float a, float px, float py, float r, unsigned c)
{
    int ox, oy, nx, ny;
    T(cx, cy, a, px + r, py, &ox, &oy);
    for (int i = 1; i <= 12; i++) {
        float t = i * PI2 / 12;
        T(cx, cy, a, px + cosf(t) * r, py + sinf(t) * r, &nx, &ny);
        line(ox, oy, nx, ny, c);
        ox = nx; oy = ny;
    }
}
static float minxs(float v) { return fabsf(v) < 0.12f ? (v < 0 ? -0.12f : 0.12f) : v; }

static void draw_rider(float sx, float sy)
{
    float cx = sx, cy = sy - 8;
    float prog = trickT > 0 ? 1.0f - trickT / trickDur : 0;
    int tt = trickT > 0 ? trickType : T_NONE;
    float xsR = minxs(cosf(yawCur)), xsB = xsR;
    float barPh = 0, crankRot = 0, crankPh = 0;
    int arms = armUp > 0.5f;

    if (tt == T_WHIP || tt == T_FOOTJAM) xsB = minxs(xsR * cosf(prog * PI2));
    if (tt == T_CRANK) { crankRot = prog * PI2 * facing(); crankPh = prog * PI2 * 2; }
    if (tt == T_BAR) { barPh = prog * PI2 * 2; arms = 1; }
    if (tt == T_CHICKEN) { barPh = prog * PI2 * 3; arms = 1; }
    if (slide) xsB = minxs(xsR * 0.2f);

    float a = angBase + flipCur;
    float fa = a + crankRot;
    float bw = 5 * cosf(barPh);
    unsigned bike = COL(230, 40, 40), body = COL(250, 250, 250), tire = COL(20, 20, 20);

    /* байк */
    g_xs = xsB; g_oy = -5.0f * tuck;
    WH(cx, cy, fa, -13, 2, 6, tire);
    WH(cx, cy, fa, 13, 2, 6, tire);
    LN(cx, cy, fa, -13, 2, -3, -5, bike);
    LN(cx, cy, fa, -3, -5, 8, -5, bike);
    LN(cx, cy, fa, -13, 2, 0, 2, bike);
    LN(cx, cy, fa, 0, 2, -3, -5, bike);
    LN(cx, cy, fa, 0, 2, 8, -5, bike);
    LN(cx, cy, fa, 8, -5, 13, 2, bike);
    LN(cx, cy, fa, 8, -5, 8 + bw, -12, bike);
    LN(cx, cy, fa, 0, 2, 5 * cosf(crankPh), 2 + 5 * sinf(crankPh), COL(200, 200, 60));

    /* райдер */
    g_xs = xsR; g_oy = 0;
    float footY = 2 - 5.0f * tuck;
    LN(cx, cy, a, -3, -7, 1, -19, body);                       /* корпус */
    if (arms) LN(cx, cy, a, 1, -18, 7, -28, body);             /* руки вверх (R) */
    else LN(cx, cy, a, 1, -18, 8 + bw, -12, body);             /* руки на руле */
    LN(cx, cy, a, -3, -7, 0, footY, body);                     /* ноги (L = поджаты) */
    WH(cx, cy, a, 2, -24, 4, COL(255, 200, 120));              /* голова */
}

static void draw_world(void)
{
    rect(0, 0, SCRW, SCRH, COL(120, 180, 235));
    float par = camx * 0.3f;
    int base = (int)floorf(par / 70.0f);
    float off = par - base * 70.0f;
    for (int i = -1; i < 8; i++) {
        int id = abs(base + i);
        int h = 50 + (id * 37) % 70;
        rect((int)(i * 70 - off), 200 - h, 62, h, COL(90 + (id * 13) % 40, 100, 130));
    }
    for (int sx = 0; sx < SCRW; sx++) {
        int gy = (int)groundY(camx + sx, NULL);
        rect(sx, gy, 1, SCRH - gy, COL(70, 70, 75));
        rect(sx, gy, 1, 3, COL(200, 200, 205));
    }
}

static void draw_help(void)
{
    rect(14, 10, 452, 252, COL(10, 10, 20));
    pspDebugScreenSetXY(3, 2);  pspDebugScreenPrintf("GROUND");
    pspDebugScreenSetXY(3, 3);  pspDebugScreenPrintf(" mash X ........ speed up");
    pspDebugScreenSetXY(3, 4);  pspDebugScreenPrintf(" stick down>up . bunnyhop (+X = high)");
    pspDebugScreenSetXY(3, 5);  pspDebugScreenPrintf(" D-pad L/R ..... turn 180 (fakie)");
    pspDebugScreenSetXY(3, 6);  pspDebugScreenPrintf(" fakie + /\\ .... slide; stick L/R = exit");
    pspDebugScreenSetXY(3, 8);  pspDebugScreenPrintf("AIR   (L = legs, R = arms)");
    pspDebugScreenSetXY(3, 9);  pspDebugScreenPrintf(" L+X crankflip     L+O whip");
    pspDebugScreenSetXY(3, 10); pspDebugScreenPrintf(" up+L+O footjam    R+O bar");
    pspDebugScreenSetXY(3, 11); pspDebugScreenPrintf(" down+R+O chickenbar");
    pspDebugScreenSetXY(3, 12); pspDebugScreenPrintf(" D-pad L/R spins, U/D flips");
    pspDebugScreenSetXY(3, 14); pspDebugScreenPrintf("Land on wheels, finish all tricks!");
    pspDebugScreenSetXY(3, 16); pspDebugScreenPrintf("START - close");
}

int main(void)
{
    setup_callbacks();
    pspDebugScreenInit();
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    pspDebugScreenEnableBackColor(0);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    draw_buf = (unsigned int *)(0x44000000);
    pspDebugScreenSetOffset(0);

    SceCtrlData pad;
    unsigned oldBtn = 0;
    int help = 1;
    x = 100; y = groundY(x, NULL);
    camx = x - 150;

    while (1) {
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned btn = pad.Buttons, pressed = btn & ~oldBtn;
        oldBtn = btn;
        if (pressed & PSP_CTRL_START) help = !help;

        if (!help) update(btn, pressed, pad.Lx, pad.Ly);
        camx += ((x - 150 + vx * 0.3f) - camx) * 0.1f;

        draw_world();
        draw_rider(x - camx, y);

        pspDebugScreenSetXY(1, 1);  pspDebugScreenPrintf("SCORE %d", score);
        if (combo > 1) { pspDebugScreenSetXY(1, 2); pspDebugScreenPrintf("COMBO x%d", combo); }
        if (pending > 0) { pspDebugScreenSetXY(1, 3); pspDebugScreenPrintf("TRICKS +%d", pending); }
        if (slide) { pspDebugScreenSetXY(1, 4); pspDebugScreenPrintf("FAKIE SLIDE"); }
        if (msgT > 0) { pspDebugScreenSetXY(22, 8); pspDebugScreenPrintf("%s", msg); }
        pspDebugScreenSetXY(1, 33); pspDebugScreenPrintf("START - help");
        if (help) draw_help();

        swap_buffers();
    }
    return 0;
}
