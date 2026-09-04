#include "BottomScreenBridge.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "bs_mailbox.h"
#include "bs_protocol.h"
#include "bs_server.h"
#include "bs_source.h"
}

namespace BottomScreen
{

namespace
{
    BsSource* g_source = nullptr;
    BsServer* g_server = nullptr;
    bool      g_tried  = false;

    /*
     * BsButton -> melonDS key bit. melonDS's frontend orders its twelve
     * keys A, B, Select, Start, Right, Left, Up, Down, R, L, X, Y, and
     * NDS::SetKeyMask maps bits 10 and 11 to the DSi's X and Y.
     *
     * The DS has no ZL, ZR or HOME. A shared protocol means clients will
     * send them anyway -- a 3DS profile on a phone talking to a DS, say
     * -- so they map to nothing and are dropped rather than treated as
     * an error.
     */
    int keyBitFor(int bsButton)
    {
        switch (bsButton)
        {
        case BS_BTN_A:      return 0;
        case BS_BTN_B:      return 1;
        case BS_BTN_SELECT: return 2;
        case BS_BTN_START:  return 3;
        case BS_BTN_RIGHT:  return 4;
        case BS_BTN_LEFT:   return 5;
        case BS_BTN_UP:     return 6;
        case BS_BTN_DOWN:   return 7;
        case BS_BTN_R:      return 8;
        case BS_BTN_L:      return 9;
        case BS_BTN_X:      return 10;
        case BS_BTN_Y:      return 11;
        default:            return -1;
        }
    }
}

void Start()
{
    if (g_tried)
        return;
    g_tried = true;

    const char* off = getenv("BOTTOM_SCREEN");
    if (off && !strcmp(off, "0"))
        return;

    uint16_t port = BS_DEFAULT_PORT;
    if (const char* p = getenv("BOTTOM_SCREEN_PORT"))
    {
        int v = atoi(p);
        if (v > 0 && v < 65536)
            port = (uint16_t)v;
    }

    /* The DS runs at ~59.83 Hz, not 60. Announcing 60 is close enough
     * for the encoder's rate control and is what every client expects;
     * the real pacing comes from the emulator submitting frames. */
    g_source = bs_mailbox_create(BS_CONSOLE_DS, BS_DS_WIDTH, BS_DS_HEIGHT,
                                 60, BS_PIXFMT_BGRA);
    if (!g_source)
    {
        fprintf(stderr, "bottom_screen: cannot create the frame mailbox\n");
        return;
    }

    BsServerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = port;

    char err[256] = "";
    g_server = bs_server_create(g_source, &cfg, err, sizeof(err));
    if (!g_server)
    {
        fprintf(stderr, "bottom_screen: %s\n", err);
        g_source->destroy(g_source->self);
        free(g_source);
        g_source = nullptr;
    }
}

void Stop()
{
    if (g_server)
    {
        bs_server_destroy(g_server);
        g_server = nullptr;
    }
    if (g_source)
    {
        g_source->destroy(g_source->self);
        free(g_source);
        g_source = nullptr;
    }
    g_tried = false;
}

bool IsRunning()
{
    return g_server != nullptr;
}

void SubmitFrame(const void* bottomBGRA)
{
    if (!g_tried)
        Start();
    if (!g_server || !bottomBGRA)
        return;
    bs_mailbox_submit(g_source, bottomBGRA, BS_DS_WIDTH * 4);
}

uint32_t PressedKeys()
{
    if (!g_server)
        return 0;

    BsInputState in;
    bs_mailbox_input(g_source, &in);

    uint32_t mask = 0;
    for (int b = 1; b <= 15; b++)
    {
        if (!(in.buttons & (1u << (b - 1))))
            continue;
        int bit = keyBitFor(b);
        if (bit >= 0)
            mask |= (1u << bit);
    }
    return mask;
}

void ReportGpuRenderer()
{
    static bool warned = false;
    if (warned || !g_server)
        return;
    warned = true;
    fprintf(stderr,
        "bottom_screen: the OpenGL renderer keeps the bottom screen on the\n"
        "               GPU, so there is nothing in RAM to stream. Switch to\n"
        "               the software renderer, or wait for the GPU readback\n"
        "               path.\n");
}

bool TouchState(uint16_t& x, uint16_t& y)
{
    if (!g_server)
        return false;

    BsInputState in;
    bs_mailbox_input(g_source, &in);
    if (!in.touching)
        return false;

    /* Clients send console pixel coordinates, but a malformed or
     * mis-scaled client could still send something outside the screen,
     * and melonDS would happily take it. */
    int cx = in.touch_x, cy = in.touch_y;
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx >= BS_DS_WIDTH)  cx = BS_DS_WIDTH - 1;
    if (cy >= BS_DS_HEIGHT) cy = BS_DS_HEIGHT - 1;

    x = (uint16_t)cx;
    y = (uint16_t)cy;
    return true;
}

}
