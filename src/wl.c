/* Turrón — native Wayland window: xdg-shell, fractional scale, input. No toolkit. */
#include "turron.h"
#include <errno.h>
#include <math.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell.h"
#include "viewporter.h"
#include "fractional-scale-v1.h"
#include "idle-inhibit-unstable-v1.h"
#include "cursor-shape-v1.h"

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_surface *surface;
static struct xdg_wm_base *wm;
static struct xdg_surface *xsurf;
static struct xdg_toplevel *top;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_keyboard *keyboard;
static struct wp_viewporter *viewporter;
static struct wp_viewport *viewport;
static struct wp_fractional_scale_manager_v1 *frac_mgr;
static struct zwp_idle_inhibit_manager_v1 *idle_mgr;
static struct zwp_idle_inhibitor_v1 *inhibitor;
static struct wp_cursor_shape_manager_v1 *shape_mgr;
static struct wp_cursor_shape_device_v1 *shape_dev;
static struct xkb_context *xkb;
static struct xkb_keymap *keymap;
static struct xkb_state *xstate;

static struct WlCallbacks cb;
static int log_w, log_h, def_w, def_h, px_w, px_h;
static double scale = 1;
static bool configured, fullscreen, cursor_hidden, frame_wanted;
static uint32_t enter_serial;
static double ptr_x, ptr_y;
static int repeat_rate = 25, repeat_delay = 400;
static uint32_t rep_key, rep_sym, rep_mods;
static double rep_next;

static void apply_size(void)
{
    int w = (int)lrint(log_w * scale), h = (int)lrint(log_h * scale);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (viewport) wp_viewport_set_destination(viewport, log_w, log_h);
    else wl_surface_set_buffer_scale(surface, (int)lrint(scale));
    struct wl_region *r = wl_compositor_create_region(comp);
    wl_region_add(r, 0, 0, log_w, log_h);
    wl_surface_set_opaque_region(surface, r);
    wl_region_destroy(r);
    if (w != px_w || h != px_h) {
        px_w = w; px_h = h;
        if (configured && cb.resize) cb.resize(px_w, px_h, scale);
    }
}

/* ---- xdg ---- */
static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void xsurf_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
    (void)d;
    xdg_surface_ack_configure(s, serial);
    bool first = !configured;
    configured = true;
    apply_size();
    if (first && cb.resize) cb.resize(px_w, px_h, scale);
}
static const struct xdg_surface_listener xsurf_listener = { xsurf_configure };

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states)
{
    (void)d; (void)t;
    fullscreen = false;
    uint32_t *s;
    wl_array_for_each(s, states) if (*s == XDG_TOPLEVEL_STATE_FULLSCREEN) fullscreen = true;
    log_w = w > 0 ? w : def_w;
    log_h = h > 0 ? h : def_h;
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; if (cb.close) cb.close(); }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_listener = { top_configure, top_close, top_bounds, top_caps };

static void frac_scale(void *d, struct wp_fractional_scale_v1 *f, uint32_t s)
{
    (void)d; (void)f;
    scale = s / 120.0;
    if (configured) apply_size();
}
static const struct wp_fractional_scale_v1_listener frac_listener = { frac_scale };

/* ---- surface (integer scale fallback) ---- */
static void surf_enter(void *d, struct wl_surface *s, struct wl_output *o) { (void)d; (void)s; (void)o; }
static void surf_leave(void *d, struct wl_surface *s, struct wl_output *o) { (void)d; (void)s; (void)o; }
static void surf_scale(void *d, struct wl_surface *s, int32_t f)
{
    (void)d; (void)s;
    if (frac_mgr && viewporter) return;
    scale = f;
    if (configured) apply_size();
}
static void surf_transform(void *d, struct wl_surface *s, uint32_t t) { (void)d; (void)s; (void)t; }
static const struct wl_surface_listener surf_listener = { surf_enter, surf_leave, surf_scale, surf_transform };

/* ---- frame callback ---- */
static void frame_done(void *d, struct wl_callback *c, uint32_t t)
{
    (void)d; (void)t;
    wl_callback_destroy(c);
    frame_wanted = false;
    if (cb.frame) cb.frame();
}
static const struct wl_callback_listener frame_listener = { frame_done };
void wl_request_frame(void)
{
    if (frame_wanted) return;
    frame_wanted = true;
    wl_callback_add_listener(wl_surface_frame(surface), &frame_listener, NULL);
}

/* ---- pointer ---- */
static void set_cursor(void)
{
    if (!pointer) return;
    if (cursor_hidden) wl_pointer_set_cursor(pointer, enter_serial, NULL, 0, 0);
    else if (shape_dev) wp_cursor_shape_device_v1_set_shape(shape_dev, enter_serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
}
static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)p; (void)s;
    enter_serial = serial;
    ptr_x = wl_fixed_to_double(x) * scale;
    ptr_y = wl_fixed_to_double(y) * scale;
    cursor_hidden = false;
    set_cursor();
    if (cb.motion) cb.motion(ptr_x, ptr_y);
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) { (void)d; (void)p; (void)serial; (void)s; }
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)p; (void)t;
    ptr_x = wl_fixed_to_double(x) * scale;
    ptr_y = wl_fixed_to_double(y) * scale;
    if (cb.motion) cb.motion(ptr_x, ptr_y);
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t button, uint32_t state)
{
    (void)d; (void)p; (void)serial; (void)t;
    if (cb.button) cb.button(button - 0x110, state == WL_POINTER_BUTTON_STATE_PRESSED, ptr_x, ptr_y);
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v)
{
    (void)d; (void)p; (void)t;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL && cb.scroll) cb.scroll(wl_fixed_to_double(v));
}
static void ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static void ptr_axis_v120(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static void ptr_axis_rel(void *d, struct wl_pointer *p, uint32_t a, uint32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener ptr_listener = {
    ptr_enter, ptr_leave, ptr_motion, ptr_button, ptr_axis, ptr_frame, ptr_axis_source, ptr_axis_stop, ptr_axis_discrete, ptr_axis_v120, ptr_axis_rel,
};

/* ---- keyboard ---- */
static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size)
{
    (void)d; (void)k;
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map != MAP_FAILED) {
            xkb_state_unref(xstate);
            xkb_keymap_unref(keymap);
            keymap = xkb_keymap_new_from_buffer(xkb, map, strnlen(map, size), XKB_KEYMAP_FORMAT_TEXT_V1, 0);
            xstate = keymap ? xkb_state_new(keymap) : NULL;
            munmap(map, size);
        }
    }
    close(fd);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *keys) { (void)d; (void)k; (void)s; (void)sf; (void)keys; }
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) { (void)d; (void)k; (void)s; (void)sf; rep_next = 0; }
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t state)
{
    (void)d; (void)k; (void)serial; (void)t;
    if (!xstate) return;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (key == rep_key) rep_next = 0;
        return;
    }
    uint32_t sym = xkb_keysym_to_lower(xkb_state_key_get_one_sym(xstate, key + 8));
    uint32_t mods = 0;
    if (xkb_state_mod_name_is_active(xstate, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) mods |= 1;
    if (xkb_state_mod_name_is_active(xstate, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0) mods |= 2;
    if (xkb_state_mod_name_is_active(xstate, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0) mods |= 4;
    if (xkb_state_mod_name_is_active(xstate, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0) return;
    if (repeat_rate > 0 && xkb_keymap_key_repeats(keymap, key + 8)) {
        rep_key = key; rep_sym = sym; rep_mods = mods;
        rep_next = now_s() + repeat_delay / 1000.0;
    } else rep_next = 0;
    if (cb.key) cb.key(sym, mods);
}
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group)
{
    (void)d; (void)k; (void)serial;
    if (xstate) xkb_state_update_mask(xstate, dep, lat, lock, 0, 0, group);
}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) { (void)d; (void)k; repeat_rate = rate; repeat_delay = delay; }
static const struct wl_keyboard_listener kb_listener = { kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, kb_repeat };

double wl_next_repeat(void) { return rep_next; }
void wl_do_repeat(void)
{
    if (!rep_next) return;
    rep_next += 1.0 / (repeat_rate > 0 ? repeat_rate : 25);
    if (rep_next < now_s()) rep_next = now_s() + 0.02;
    if (cb.key) cb.key(rep_sym, rep_mods);
}

/* ---- seat / registry ---- */
static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &ptr_listener, NULL);
        if (shape_mgr) shape_dev = wp_cursor_shape_manager_v1_get_pointer(shape_mgr, pointer);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(keyboard, &kb_listener, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 6 ? ver : 6);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 5 ? ver : 5);
    else if (!strcmp(iface, wl_seat_interface.name)) seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 5 ? ver : 5);
    else if (!strcmp(iface, wp_viewporter_interface.name)) viewporter = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
    else if (!strcmp(iface, wp_fractional_scale_manager_v1_interface.name)) frac_mgr = wl_registry_bind(r, name, &wp_fractional_scale_manager_v1_interface, 1);
    else if (!strcmp(iface, zwp_idle_inhibit_manager_v1_interface.name)) idle_mgr = wl_registry_bind(r, name, &zwp_idle_inhibit_manager_v1_interface, 1);
    else if (!strcmp(iface, wp_cursor_shape_manager_v1_interface.name)) shape_mgr = wl_registry_bind(r, name, &wp_cursor_shape_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

/* Connects and binds the globals; separate from wl_init so the GPU driver can be
 * loaded in the background while the file is being opened. */
int wl_connect(void)
{
    if (dpy) return 0;
    dpy = wl_display_connect(NULL);
    if (!dpy) return -1;
    xkb = xkb_context_new(0);
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    return 0;
}

int wl_init(const struct WlCallbacks *c, const char *title, int w, int h)
{
    cb = *c;
    def_w = log_w = w;
    def_h = log_h = h;
    if (wl_connect() < 0) return -1;
    if (!comp || !wm) return -1;
    xdg_wm_base_add_listener(wm, &wm_listener, NULL);
    if (seat) wl_seat_add_listener(seat, &seat_listener, NULL);

    surface = wl_compositor_create_surface(comp);
    wl_surface_add_listener(surface, &surf_listener, NULL);
    if (viewporter) viewport = wp_viewporter_get_viewport(viewporter, surface);
    if (frac_mgr && viewport)
        wp_fractional_scale_v1_add_listener(wp_fractional_scale_manager_v1_get_fractional_scale(frac_mgr, surface), &frac_listener, NULL);
    xsurf = xdg_wm_base_get_xdg_surface(wm, surface);
    xdg_surface_add_listener(xsurf, &xsurf_listener, NULL);
    top = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(top, &top_listener, NULL);
    xdg_toplevel_set_app_id(top, "turron");
    xdg_toplevel_set_title(top, title);
    if (O.fullscreen) xdg_toplevel_set_fullscreen(top, NULL);
    wl_surface_commit(surface);
    while (!configured)
        if (wl_display_roundtrip(dpy) < 0) return -1;
    wl_display_roundtrip(dpy);
    return 0;
}

void wl_get_size(int *w, int *h, double *s) { *w = px_w; *h = px_h; *s = scale; }
int wl_fd(void) { return wl_display_get_fd(dpy); }
void *wl_display_ptr(void) { return dpy; }
void *wl_surface_ptr(void) { return surface; }

int wl_prepare(void)
{
    while (wl_display_prepare_read(dpy) != 0)
        if (wl_display_dispatch_pending(dpy) < 0) return -1;
    if (wl_display_flush(dpy) < 0 && errno != EAGAIN) { wl_display_cancel_read(dpy); return -1; }
    return 0;
}
int wl_dispatch(bool readable)
{
    if (readable) { if (wl_display_read_events(dpy) < 0) return -1; }
    else wl_display_cancel_read(dpy);
    return wl_display_dispatch_pending(dpy) < 0 ? -1 : 0;
}

void wl_set_title(const char *t) { xdg_toplevel_set_title(top, t); }
void wl_toggle_fullscreen(void)
{
    if (fullscreen) xdg_toplevel_unset_fullscreen(top);
    else xdg_toplevel_set_fullscreen(top, NULL);
}
bool wl_is_fullscreen(void) { return fullscreen; }
void wl_inhibit_idle(bool on)
{
    if (!idle_mgr) return;
    if (on && !inhibitor) inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(idle_mgr, surface);
    else if (!on && inhibitor) { zwp_idle_inhibitor_v1_destroy(inhibitor); inhibitor = NULL; }
}
void wl_hide_cursor(bool hide)
{
    if (hide == cursor_hidden) return;
    cursor_hidden = hide;
    set_cursor();
}
