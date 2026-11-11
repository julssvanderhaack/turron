/* Turrón — MPRIS over sd-bus, so desktop bars and media keys can see and control it. */
#include "turron.h"
#include <unistd.h>
#include <systemd/sd-bus.h>

#define OBJ "/org/mpris/MediaPlayer2"
#define IF_ROOT "org.mpris.MediaPlayer2"
#define IF_PLAYER "org.mpris.MediaPlayer2.Player"

static sd_bus *bus;
static struct MprisCallbacks cb;
static char m_title[512], m_url[4200];
static double m_dur;
static bool m_playing;

static int ok(sd_bus_message *m) { return sd_bus_reply_method_return(m, NULL); }
static int h_raise(sd_bus_message *m, void *u, sd_bus_error *e) { (void)u; (void)e; return ok(m); }
static int h_quit(sd_bus_message *m, void *u, sd_bus_error *e) { (void)u; (void)e; cb.stop(); return ok(m); }
static int h_next(sd_bus_message *m, void *u, sd_bus_error *e) { (void)u; (void)e; cb.next(); return ok(m); }
static int h_prev(sd_bus_message *m, void *u, sd_bus_error *e) { (void)u; (void)e; cb.prev(); return ok(m); }
static int h_pause(sd_bus_message *m, void *u, sd_bus_error *e) { (void)u; (void)e; cb.pause(); return ok(m); }
static int h_play(sd_bus_message *m, void *u, sd_bus_error *e) { (void)u; (void)e; cb.play(); return ok(m); }
static int h_pp(sd_bus_message *m, void *u, sd_bus_error *e) { (void)u; (void)e; cb.play_pause(); return ok(m); }
static int h_stop(sd_bus_message *m, void *u, sd_bus_error *e) { (void)u; (void)e; cb.stop(); return ok(m); }
static int h_seek(sd_bus_message *m, void *u, sd_bus_error *e)
{
    (void)u; (void)e;
    int64_t us = 0;
    if (sd_bus_message_read(m, "x", &us) >= 0) cb.seek_rel(us / 1e6);
    return ok(m);
}
static int h_setpos(sd_bus_message *m, void *u, sd_bus_error *e)
{
    (void)u; (void)e;
    const char *id;
    int64_t us = 0;
    if (sd_bus_message_read(m, "ox", &id, &us) >= 0) cb.set_pos(us / 1e6);
    return ok(m);
}
static int h_open(sd_bus_message *m, void *u, sd_bus_error *e) { (void)u; (void)e; return ok(m); }

static int p_true(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{ (void)b; (void)p; (void)i; (void)n; (void)u; (void)e; return sd_bus_message_append(r, "b", 1); }
static int p_false(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{ (void)b; (void)p; (void)i; (void)n; (void)u; (void)e; return sd_bus_message_append(r, "b", 0); }
static int p_seekable(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{ (void)b; (void)p; (void)i; (void)n; (void)u; (void)e; return sd_bus_message_append(r, "b", m_dur > 0); }
static int p_identity(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{ (void)b; (void)p; (void)i; (void)n; (void)u; (void)e; return sd_bus_message_append(r, "s", "Turrón"); }
static int p_desktop(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{ (void)b; (void)p; (void)i; (void)n; (void)u; (void)e; return sd_bus_message_append(r, "s", "turron"); }
static int p_strv(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{
    (void)b; (void)p; (void)i; (void)u; (void)e;
    if (!strcmp(n, "SupportedUriSchemes")) return sd_bus_message_append(r, "as", 3, "file", "http", "https");
    return sd_bus_message_append(r, "as", 0);
}
static int p_status(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{ (void)b; (void)p; (void)i; (void)n; (void)u; (void)e; return sd_bus_message_append(r, "s", m_playing ? "Playing" : "Paused"); }
static int p_one(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{ (void)b; (void)p; (void)i; (void)n; (void)u; (void)e; return sd_bus_message_append(r, "d", 1.0); }
static int p_volume(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{ (void)b; (void)p; (void)i; (void)n; (void)u; (void)e; return sd_bus_message_append(r, "d", atomic_load(&g_volume) / 100.0); }
static int s_volume(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *v, void *u, sd_bus_error *e)
{
    (void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
    double d = 1;
    if (sd_bus_message_read(v, "d", &d) < 0) return -EINVAL;
    int vol = (int)(d * 100 + 0.5);
    atomic_store(&g_volume, vol < 0 ? 0 : vol > 150 ? 150 : vol);
    return 0;
}
static int p_position(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{ (void)b; (void)p; (void)i; (void)n; (void)u; (void)e; return sd_bus_message_append(r, "x", (int64_t)(cb.pos() * 1e6)); }
static int p_metadata(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{
    (void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
    int ret = sd_bus_message_open_container(r, 'a', "{sv}");
    if (ret < 0) return ret;
    sd_bus_message_append(r, "{sv}", "mpris:trackid", "o", OBJ "/track");
    sd_bus_message_append(r, "{sv}", "mpris:length", "x", (int64_t)(m_dur * 1e6));
    sd_bus_message_append(r, "{sv}", "xesam:title", "s", m_title);
    if (m_url[0]) sd_bus_message_append(r, "{sv}", "xesam:url", "s", m_url);
    return sd_bus_message_close_container(r);
}

static const sd_bus_vtable vt_root[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Raise", "", "", h_raise, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Quit", "", "", h_quit, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("CanQuit", "b", p_true, 0, 0),
    SD_BUS_PROPERTY("CanRaise", "b", p_false, 0, 0),
    SD_BUS_PROPERTY("HasTrackList", "b", p_false, 0, 0),
    SD_BUS_PROPERTY("Identity", "s", p_identity, 0, 0),
    SD_BUS_PROPERTY("DesktopEntry", "s", p_desktop, 0, 0),
    SD_BUS_PROPERTY("SupportedUriSchemes", "as", p_strv, 0, 0),
    SD_BUS_PROPERTY("SupportedMimeTypes", "as", p_strv, 0, 0),
    SD_BUS_VTABLE_END,
};
static const sd_bus_vtable vt_player[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Next", "", "", h_next, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Previous", "", "", h_prev, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Pause", "", "", h_pause, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("PlayPause", "", "", h_pp, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Stop", "", "", h_stop, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Play", "", "", h_play, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Seek", "x", "", h_seek, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetPosition", "ox", "", h_setpos, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("OpenUri", "s", "", h_open, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("Seeked", "x", 0),
    SD_BUS_PROPERTY("PlaybackStatus", "s", p_status, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Rate", "d", p_one, 0, 0),
    SD_BUS_PROPERTY("MinimumRate", "d", p_one, 0, 0),
    SD_BUS_PROPERTY("MaximumRate", "d", p_one, 0, 0),
    SD_BUS_PROPERTY("Metadata", "a{sv}", p_metadata, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_WRITABLE_PROPERTY("Volume", "d", p_volume, s_volume, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Position", "x", p_position, 0, 0),
    SD_BUS_PROPERTY("CanGoNext", "b", p_true, 0, 0),
    SD_BUS_PROPERTY("CanGoPrevious", "b", p_true, 0, 0),
    SD_BUS_PROPERTY("CanPlay", "b", p_true, 0, 0),
    SD_BUS_PROPERTY("CanPause", "b", p_true, 0, 0),
    SD_BUS_PROPERTY("CanSeek", "b", p_seekable, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanControl", "b", p_true, 0, 0),
    SD_BUS_VTABLE_END,
};

int mpris_init(const struct MprisCallbacks *c)
{
    cb = *c;
    if (sd_bus_open_user(&bus) < 0) { bus = NULL; return -1; }
    if (sd_bus_add_object_vtable(bus, NULL, OBJ, IF_ROOT, vt_root, NULL) < 0 ||
        sd_bus_add_object_vtable(bus, NULL, OBJ, IF_PLAYER, vt_player, NULL) < 0) goto fail;
    if (sd_bus_request_name(bus, "org.mpris.MediaPlayer2.turron", 0) < 0) {
        char name[96];
        snprintf(name, sizeof name, "org.mpris.MediaPlayer2.turron.instance%d", (int)getpid());
        if (sd_bus_request_name(bus, name, 0) < 0) goto fail;
    }
    mpris_dispatch();
    return 0;
fail:
    sd_bus_unref(bus);
    bus = NULL;
    return -1;
}

int mpris_fd(void) { return bus ? sd_bus_get_fd(bus) : -1; }

void mpris_dispatch(void)
{
    if (!bus) return;
    while (sd_bus_process(bus, NULL) > 0) { }
    sd_bus_flush(bus);
}

void mpris_update(const char *title, const char *path, double dur, bool playing)
{
    bool meta = strcmp(m_title, title) || dur != m_dur, st = playing != m_playing;
    snprintf(m_title, sizeof m_title, "%s", title);
    if (strstr(path, "://")) snprintf(m_url, sizeof m_url, "%s", path);
    else {
        char *abs = realpath(path, NULL);
        snprintf(m_url, sizeof m_url, "file://%s", abs ? abs : path);
        free(abs);
    }
    m_dur = dur;
    m_playing = playing;
    if (!bus) return;
    if (meta) sd_bus_emit_properties_changed(bus, OBJ, IF_PLAYER, "Metadata", "CanSeek", NULL);
    if (st) sd_bus_emit_properties_changed(bus, OBJ, IF_PLAYER, "PlaybackStatus", NULL);
    mpris_dispatch();
}

void mpris_seeked(double pos)
{
    if (!bus) return;
    sd_bus_emit_signal(bus, OBJ, IF_PLAYER, "Seeked", "x", (int64_t)(pos * 1e6));
    sd_bus_flush(bus);
}
