#include "BottomScreenBridge.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "PlatformOGL.h"

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

    /*
     * The top screen, for a client that asked for it.
     *
     * Built the first time somebody does and left in place afterwards:
     * the server stops encoding it when the last viewer leaves, and the
     * work of producing it is skipped in that case too, so what remains
     * costs only its buffer. Its own dimensions, because the two screens
     * do not have to be the same size -- the OpenGL renderer scales both
     * but a future one need not.
     */
    BsSource* g_top_source = nullptr;
    int       g_top_width  = 0;
    int       g_top_height = 0;
    std::vector<uint8_t> g_topReadBuf;
    BsServer* g_server = nullptr;
    bool      g_tried  = false;

    // What is actually being streamed. Not fixed at 256x192 any more:
    // the OpenGL renderer draws the screens at 256*N by 192*N, and N is
    // a setting the player can move while a game is running.
    int g_width  = 0;
    int g_height = 0;

    // Scratch for reading a screen off the GPU. Created on first use and
    // kept for the life of the process: destroying it would need the GL
    // context current, and Stop is called from wherever the emulator
    // happens to be shutting down.
    GLuint g_readFbo = 0;
    std::vector<unsigned char> g_readBuf;

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

void Start(bool enabled, int port)
{
    if (g_tried)
        return;
    g_tried = true;

    // The variable wins over the setting: a scripted launch should be
    // able to turn this off without editing a config file someone else
    // owns.
    const char* off = getenv("BOTTOM_SCREEN");
    if (off && !strcmp(off, "0"))
        return;
    if (!enabled)
        return;

    if (const char* p = getenv("BOTTOM_SCREEN_PORT"))
    {
        int v = atoi(p);
        if (v > 0 && v < 65536)
            port = v;
    }
    if (port <= 0 || port > 65535)
        port = BS_DEFAULT_PORT;

    /* The DS runs at ~59.83 Hz, not 60. Announcing 60 is close enough
     * for the encoder's rate control and is what every client expects;
     * the real pacing comes from the emulator submitting frames. */
    /* melonDS opens SDL at 48 kHz stereo, which is what Opus wants, so
     * nothing is resampled on this path. */
    if (g_width <= 0 || g_height <= 0)
    {
        // Nothing has been measured yet. With the OpenGL renderer the
        // size depends on the internal resolution, which is only known
        // once a frame has been drawn, so announcing anything now would
        // be a guess -- and a client would have to be told twice.
        g_tried = false;
        return;
    }

    g_source = bs_mailbox_create(BS_CONSOLE_DS, g_width, g_height,
                                 60, BS_PIXFMT_BGRA, 48000, 2);
    if (!g_source)
    {
        fprintf(stderr, "bottom_screen: cannot create the frame mailbox\n");
        return;
    }

    BsServerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = (uint16_t)port;

    char err[256] = "";
    g_server = bs_server_create(g_source, &cfg, err, sizeof(err));
    /* Said now, produced later. The top screen is only read back while
     * somebody is watching it, and nobody may ask for a screen the
     * server has not admitted to -- so the intent is announced here and
     * the source turns up on the first frame after a client asks. */
    if (g_server)
        bs_server_offer_top(g_server);
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
    /* After the server, which is what was reading from it. */
    if (g_top_source)
    {
        g_top_source->destroy(g_top_source->self);
        free(g_top_source);
        g_top_source = nullptr;
    }
    g_top_width = g_top_height = 0;
    g_tried = false;
}

bool IsRunning()
{
    return g_server != nullptr;
}

/*
 * The one place a frame reaches the server, whichever renderer produced
 * it.
 *
 * The size is checked every time rather than assumed, because melonDS
 * can change renderer without restarting: switch from OpenGL at 2x back
 * to software and the picture goes from 512x384 to 256x192 between two
 * frames. Submitting the smaller buffer into a mailbox still sized for
 * the larger one reads well past the end of melonDS's own framebuffer.
 */
static void SubmitBGRA(const void* pixels, int width, int height)
{
    if (!pixels || width <= 0 || height <= 0)
        return;

    if (width != g_width || height != g_height)
    {
        g_width = width;
        g_height = height;
        // The server renegotiates with whoever is watching rather than
        // dropping them over a setting.
        if (g_source)
            bs_mailbox_resize(g_source, width, height);
    }

    if (!g_server)
        return;
    bs_mailbox_submit(g_source, pixels, width * 4);
}

void SubmitFrame(const void* bottomBGRA)
{
    SubmitBGRA(bottomBGRA, BS_DS_WIDTH, BS_DS_HEIGHT);
}

/*
 * The top screen, on its way to whoever asked for it.
 *
 * The mailbox is made on first use rather than at startup, so a DS that
 * nobody has asked the top screen of never allocates one. Everything
 * above this returns early when no client is watching, which is what
 * keeps the second screen free when it is switched off.
 */
static void SubmitTopBGRA(const void* pixels, int width, int height)
{
    if (!g_server || !pixels || width <= 0 || height <= 0)
        return;

    if (!g_top_source)
    {
        g_top_source = bs_mailbox_create(BS_CONSOLE_DS, width, height,
                                         60, BS_PIXFMT_BGRA, 0, 0);
        if (!g_top_source)
            return;
        g_top_width = width;
        g_top_height = height;
        bs_server_set_top_source(g_server, g_top_source);
    }
    else if (width != g_top_width || height != g_top_height)
    {
        /* melonDS can change renderer without restarting, so this is a
         * real event and not a corrupt frame: 512x384 back to 256x192
         * between two frames. Submitting the smaller buffer into a
         * mailbox still sized for the larger one would read past the end
         * of melonDS's own framebuffer. */
        if (bs_mailbox_resize(g_top_source, width, height))
        {
            g_top_width = width;
            g_top_height = height;
        }
    }

    bs_mailbox_submit(g_top_source, pixels, width * 4);
}

void SubmitTopFrame(const void* topBGRA)
{
    if (!g_server || !bs_server_wants_screen(g_server, BS_SCREEN_TOP))
        return;
    SubmitTopBGRA(topBGRA, BS_DS_WIDTH, BS_DS_HEIGHT);
}

/* One layer of melonDS's screen array, read back into `into`. The two
 * screens differ by the layer index and by nothing else, so they share
 * this rather than the same twenty lines twice. */
static void ReadLayer(unsigned int tex, int layer, int w, int h,
                      std::vector<uint8_t>& into)
{
    GLint prevFbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevFbo);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_readFbo);
    glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              (GLuint)tex, 0, layer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);

    // BGRA rather than RGBA so both paths hand the encoder the same
    // thing and nothing downstream has to know which renderer drew it.
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, into.data());

    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevFbo);
}

void SubmitFrameGL(unsigned int screenTexArray)
{
    if (!screenTexArray)
        return;

    /*
     * Ask the texture how big it is rather than working it out from the
     * scale setting.
     *
     * The same shortcut on the Azahar side -- trusting a size that
     * described the console rather than the texture -- read six times
     * past the end of the buffer. The texture is the only thing that
     * knows, and asking costs one call a frame.
     */
    GLint w = 0, h = 0;
    glBindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)screenTexArray);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &h);
    if (w <= 0 || h <= 0)
        return;

    if (!g_server && (w != g_width || h != g_height))
    {
        // Nothing is running yet; just record the size so Start can
        // announce it.
        g_width = w;
        g_height = h;
        return;
    }

    if (!g_server)
        return;

    if (!g_readFbo)
        glGenFramebuffers(1, &g_readFbo);

    const size_t needed = (size_t)w * (size_t)h * 4;
    if (g_readBuf.size() < needed)
        g_readBuf.resize(needed);

    /*
     * Layer 1 is the bottom screen: the software path uploads the top
     * framebuffer to layer 0 and the bottom to layer 1, and the OpenGL
     * path hands the compositor's own texture to the same shader. Layer
     * 0 is therefore the top screen, which is the only difference
     * between the two readbacks below.
     */
    ReadLayer(screenTexArray, 1, w, h, g_readBuf);
    SubmitBGRA(g_readBuf.data(), w, h);

    /* And the top screen, only if somebody is watching it. A readback
     * costs a stall on the render thread, so this is asked before the
     * work rather than after it: with the option switched off in every
     * client, melonDS does exactly what it did before. */
    if (bs_server_wants_screen(g_server, BS_SCREEN_TOP))
    {
        const size_t topNeeded = (size_t)w * (size_t)h * 4;
        if (g_topReadBuf.size() < topNeeded)
            g_topReadBuf.resize(topNeeded);
        ReadLayer(screenTexArray, 0, w, h, g_topReadBuf);
        SubmitTopBGRA(g_topReadBuf.data(), w, h);

    }
}

void SubmitAudio(const int16_t* samples, int frames)
{
    if (!g_server || !samples || frames <= 0)
        return;
    bs_mailbox_submit_audio(g_source, samples, frames);
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

bool TouchState(uint16_t& x, uint16_t& y)
{
    if (!g_server)
        return false;

    BsInputState in;
    bs_mailbox_input(g_source, &in);
    if (!in.touching)
        return false;

    /*
     * Clients send coordinates in the space the server announced, which
     * is 256x192 only while the internal resolution is 1. Dividing by
     * the console's own size instead would put every tap wrong by
     * exactly the scale -- at 4x the whole screen would fold into its
     * top-left quarter -- and it looks like a calibration problem rather
     * than the arithmetic mistake it is. The Azahar backend had this
     * exact bug.
     */
    const int announced_w = g_width  > 0 ? g_width  : BS_DS_WIDTH;
    const int announced_h = g_height > 0 ? g_height : BS_DS_HEIGHT;

    int cx = in.touch_x * BS_DS_WIDTH  / announced_w;
    int cy = in.touch_y * BS_DS_HEIGHT / announced_h;

    /* A malformed or mis-scaled client could still send something off
     * the screen, and melonDS would happily take it. */
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx >= BS_DS_WIDTH)  cx = BS_DS_WIDTH - 1;
    if (cy >= BS_DS_HEIGHT) cy = BS_DS_HEIGHT - 1;

    x = (uint16_t)cx;
    y = (uint16_t)cy;
    return true;
}

}
