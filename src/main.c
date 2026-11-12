/* Turrón — main loop: A/V sync, input, playlist, resume. */
#include "turron.h"
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <libavutil/log.h>

void wl_get_size(int *w, int *h, double *s);
bool wl_is_fullscreen(void);
void osd_seek(void);

bool g_es;
struct Opts O = { .start = -1, .quit_after = -1, .shot_at = -1, .volume = -1 };

static char **files;
static int n_files, cur_file = -1;
static bool quit, gl_ready, need_redraw, frame_pending, ended;
static int win_w, win_h;
static double win_scale = 1;
static int last_serial;
static char title[512];

static double bar_until, toast_until, cursor_until;
static char toast[256], stats[1024];
static bool show_stats, dragging, drag_vol;
static int menu_kind, menu_scroll, hover_hit;      /* menu: 0 closed, 1 audio, 2 subtitles */
static double ptr_x, ptr_y, last_click, last_drag_seek, scroll_acc;
static double last_seek_at, last_seek_target, stats_at;
static double t_start;

void tr_stage(const char *what)
{
    static int on = -1;
    static double t0, last;
    if (on < 0) { on = getenv("TURRON_TRACE") != NULL; t0 = last = now_s(); }
    if (!on) return;
    double t = now_s();
    fprintf(stderr, "stage %-22s +%6.1f ms  (total %7.1f ms)\n", what, (t - last) * 1e3, (t - t0) * 1e3);
    last = t;
}

/* ------------------------------------------------------------- resume */
static char *resume_path(void)
{
    static char p[1024];
    const char *st = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    if (st && *st) snprintf(p, sizeof p, "%s/turron", st);
    else snprintf(p, sizeof p, "%s/.local/state/turron", home ? home : ".");
    mkdir(p, 0755);
    strncat(p, "/resume.tsv", sizeof p - strlen(p) - 1);
    return p;
}
static char *abs_path(const char *f)
{
    if (strstr(f, "://")) return strdup(f);
    char *r = realpath(f, NULL);
    return r ? r : strdup(f);
}
/* pos < 0: look up (returns stored position or -1). pos >= 0: store; pos == 0 removes. */
static double resume_io(const char *file, double pos)
{
    char *key = abs_path(file), *lines[400];
    int n = 0;
    double found = -1;
    FILE *f = fopen(resume_path(), "r");
    if (f) {
        char *line = NULL;
        size_t cap = 0;
        while (getline(&line, &cap, f) > 0 && n < 400) {
            line[strcspn(line, "\n")] = 0;
            char *tab = strchr(line, '\t');
            if (!tab) continue;
            if (!strcmp(tab + 1, key)) { found = atof(line); continue; }
            lines[n++] = strdup(line);
        }
        free(line);
        fclose(f);
    }
    if (pos >= 0 && (pos > 0 || found >= 0) && (f = fopen(resume_path(), "w"))) {
        for (int i = n > 300 ? n - 300 : 0; i < n; i++) fprintf(f, "%s\n", lines[i]);
        if (pos > 0) fprintf(f, "%.1f\t%s\n", pos, key);
        fclose(f);
    }
    for (int i = 0; i < n; i++) free(lines[i]);
    free(key);
    return found;
}
static void save_resume(void)
{
    if (!P.opened || O.no_resume || cur_file < 0 || P.vi < 0) return;
    double pos = player_pos(), dur = P.duration;
    if (dur < 300) return;
    resume_io(files[cur_file], (pos > 60 && pos < dur - 60 && !ended) ? pos : 0);
}

/* ---------------------------------------------------------------- UI */
static void show_bar(double secs)
{
    double t = now_s() + secs;
    if (t > bar_until) bar_until = t;
    if (t > cursor_until) cursor_until = t;
}
static void set_toast(const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    vsnprintf(toast, sizeof toast, fmt, va);
    va_end(va);
    toast_until = now_s() + 1.8;
}
static void update_title(void)
{
    const char *f = files[cur_file], *base = strrchr(f, '/');
    AVDictionaryEntry *t = P.ic ? av_dict_get(P.ic->metadata, "title", NULL, 0) : NULL;
    if (strstr(f, "://") && t) snprintf(title, sizeof title, "%s", t->value);
    else snprintf(title, sizeof title, "%s", base && base[1] && !strstr(f, "://") ? base + 1 : f);
    char wt[600];
    snprintf(wt, sizeof wt, "%s — Turrón", title);
    if (gl_ready) wl_set_title(wt);
}
/* Clean title for MPRIS: no extension and no [group]/(tags) decorations. */
static void short_title(char *out, size_t n)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%s", title);
    bool file = cur_file >= 0 && !strstr(files[cur_file], "://");
    char *dot = strrchr(buf, '.');
    if (file && dot && dot != buf && strlen(dot) <= 6) *dot = 0;
    char *p = buf;
    for (;;) {                                  /* leading [group] */
        while (*p == ' ' || *p == '_') p++;
        char *e = *p == '[' ? strchr(p, ']') : NULL;
        if (!e || !e[1]) break;
        p = e + 1;
    }
    for (;;) {                                  /* trailing [id] / (tags) */
        size_t l = strlen(p);
        while (l && (p[l - 1] == ' ' || p[l - 1] == '_')) p[--l] = 0;
        if (!l) break;
        char close = p[l - 1], open = close == ']' ? '[' : close == ')' ? '(' : 0;
        char *o = open ? strrchr(p, open) : NULL;
        if (!o || o == p) break;
        *o = 0;
    }
    if (!strchr(p, ' ')) for (char *c = p; *c; c++) if (*c == '.' || *c == '_') *c = ' ';
    if (!*p) p = title;
    snprintf(out, n, "%.250s", p);
}

static void sync_state(void)
{
    bool playing = P.opened && !P.paused && !ended;
    if (gl_ready) wl_inhibit_idle(playing && P.vi >= 0);
    char st[256];
    short_title(st, sizeof st);
    mpris_update(st, cur_file >= 0 ? files[cur_file] : "", P.duration, playing);
}

static bool open_file(int i, double start, int atrack)
{
    bool was_paused = P.opened && P.paused && atrack >= 0;
    save_resume();
    player_close();
    osd_reset();
    ended = false;
    menu_kind = 0;
    cur_file = i;
    double resumed = -1;
    if (start < 0) {
        start = 0;
        if (!O.no_resume && (resumed = resume_io(files[i], -1)) > 0) start = resumed;
    }
    if (player_open(files[i], start, atrack) < 0) return false;
    last_serial = -1;
    update_title();
    if (was_paused) player_set_paused(true);
    if (resumed > 0) {
        int s = (int)resumed;
        set_toast("%s %d:%02d:%02d", T("Reanudando en", "Resuming at"), s / 3600, s / 60 % 60, s % 60);
    }
    show_bar(2.5);
    sync_state();
    need_redraw = true;
    return true;
}
static void open_next(int dir)
{
    for (int i = cur_file + dir; i >= 0 && i < n_files; i += dir)
        if (open_file(i, -1, -1)) return;
    if (!P.opened) quit = true;
}

static void toggle_pause(void)
{
    if (!P.opened) return;
    if (ended) { ended = false; player_seek(0, true); player_set_paused(false); }
    else player_set_paused(!P.paused);
    show_bar(2.5);
    sync_state();
}
static void seek_to(double pos, bool exact)
{
    if (!P.opened) return;
    ended = false;
    last_seek_at = now_s();
    last_seek_target = pos < 0 ? 0 : pos;
    player_seek(pos, exact);
    show_bar(2.5);
    mpris_seeked(last_seek_target);
}
static void seek_rel(double d, bool exact)
{
    double base = now_s() - last_seek_at < 0.6 ? last_seek_target : player_pos();
    seek_to(base + d, exact);
}
static void change_volume(int d)
{
    int v = atomic_load(&g_volume) + d;
    if (v < 0) v = 0;
    if (v > 150) v = 150;
    atomic_store(&g_volume, v);
    g_mute = false;
    set_toast("%s %d %%", T("Volumen", "Volume"), v);
}
static void cycle_subs(void)
{
    if (!P.n_stracks) { set_toast("%s", T("Sin subtítulos", "No subtitles")); return; }
    int n = P.want_strack + 1;
    if (n >= P.n_stracks) n = -1;
    P.want_strack = n;
    if (n < 0) set_toast("%s", T("Subtítulos: desactivados", "Subtitles: off"));
    else set_toast("%s %s (%d/%d)", T("Subtítulos:", "Subtitles:"), P.stracks[n].label, n + 1, P.n_stracks);
}
static void select_audio(int n)
{
    if (n < 0 || n >= P.n_atracks || n == P.cur_atrack) return;
    int st = P.want_strack;
    if (open_file(cur_file, player_pos(), n)) {
        P.want_strack = st;
        set_toast("%s %s (%d/%d)", T("Audio:", "Audio:"), P.atracks[n].label, n + 1, P.n_atracks);
    }
}
static void select_subs(int n)              /* -1 = off */
{
    if (n >= P.n_stracks) return;
    P.want_strack = n;
    if (n < 0) set_toast("%s", T("Subtítulos: desactivados", "Subtitles: off"));
    else set_toast("%s %s (%d/%d)", T("Subtítulos:", "Subtitles:"), P.stracks[n].label, n + 1, P.n_stracks);
}
static void cycle_audio(void)
{
    if (P.n_atracks < 2) { set_toast("%s", T("Solo hay una pista de audio", "Only one audio track")); return; }
    select_audio((P.cur_atrack + 1) % P.n_atracks);
}
static int menu_count(void)
{
    return menu_kind == 1 ? P.n_atracks : menu_kind == 2 ? P.n_stracks + 1 : 0;
}
static void open_menu(int kind)
{
    menu_kind = menu_kind == kind ? 0 : kind;
    menu_scroll = 0;
    if (menu_kind == 1 && !P.n_atracks) { menu_kind = 0; set_toast("%s", T("Sin pistas de audio", "No audio tracks")); }
    if (menu_kind == 2 && !P.n_stracks) { menu_kind = 0; set_toast("%s", T("Este vídeo no tiene subtítulos", "This video has no subtitles")); }
    show_bar(2.5);
}
static void menu_pick(int i)
{
    int kind = menu_kind;
    menu_kind = 0;
    if (kind == 1) select_audio(i);
    else if (kind == 2) select_subs(i - 1);
}

static void on_key(uint32_t sym, uint32_t mods)
{
    bool shift = mods & 1, ctrl = mods & 2;
    switch (sym) {
    case XKB_KEY_space: case XKB_KEY_p: case XKB_KEY_k: toggle_pause(); break;
    case XKB_KEY_q: quit = true; break;
    case XKB_KEY_Escape:
        if (menu_kind) menu_kind = 0;
        else if (wl_is_fullscreen()) wl_toggle_fullscreen();
        else quit = true;
        break;
    case XKB_KEY_f: case XKB_KEY_F11: wl_toggle_fullscreen(); break;
    case XKB_KEY_Left: seek_rel(ctrl ? -60 : shift ? -1 : -5, !ctrl); break;
    case XKB_KEY_Right: seek_rel(ctrl ? 60 : shift ? 1 : 5, !ctrl); break;
    case XKB_KEY_j: seek_rel(-10, true); break;
    case XKB_KEY_l: seek_rel(10, true); break;
    case XKB_KEY_Page_Up: seek_rel(60, false); break;
    case XKB_KEY_Page_Down: seek_rel(-60, false); break;
    case XKB_KEY_Home: seek_to(0, true); break;
    case XKB_KEY_Up: case XKB_KEY_plus: case XKB_KEY_KP_Add: change_volume(5); break;
    case XKB_KEY_Down: case XKB_KEY_minus: case XKB_KEY_KP_Subtract: change_volume(-5); break;
    case XKB_KEY_m: g_mute = !g_mute; set_toast("%s", g_mute ? T("Silencio", "Muted") : T("Sonido activado", "Unmuted")); break;
    case XKB_KEY_s: cycle_subs(); break;
    case XKB_KEY_a: cycle_audio(); break;
    case XKB_KEY_v: open_menu(2); break;
    case XKB_KEY_u: open_menu(1); break;
    case XKB_KEY_n: case XKB_KEY_greater: if (cur_file + 1 < n_files) open_next(1); break;
    case XKB_KEY_b: case XKB_KEY_less: if (cur_file > 0) open_next(-1); break;
    case XKB_KEY_i: case XKB_KEY_Tab: show_stats = !show_stats; stats_at = 0; break;
    case XKB_KEY_o: show_bar(3); break;
    default:
        if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9 && P.duration > 0) seek_to(P.duration * (sym - XKB_KEY_0) / 10.0, false);
        break;
    }
    need_redraw = true;
}

static void set_volume_frac(double f)
{
    atomic_store(&g_volume, (int)lrint(f * 100));
    g_mute = false;
}
static void on_motion(double x, double y)
{
    ptr_x = x; ptr_y = y;
    show_bar(2.5);
    wl_hide_cursor(false);
    hover_hit = osd_hit(x, y);
    if (dragging && P.duration > 0 && now_s() - last_drag_seek > 0.12) {
        last_drag_seek = now_s();
        seek_to(P.duration * osd_frac(HIT_SEEK, x), false);
    }
    if (drag_vol) set_volume_frac(osd_frac(HIT_VOL, x));
    need_redraw = true;
}
static void on_button(int btn, bool pressed, double x, double y)
{
    if (btn == 0 && !pressed) {
        if (dragging) { dragging = false; if (P.duration > 0) seek_to(P.duration * osd_frac(HIT_SEEK, x), true); }
        drag_vol = false;
        return;
    }
    if (!pressed) return;
    int hit = osd_hit(x, y);
    hover_hit = hit;
    need_redraw = true;
    if (btn != 0) {
        if (menu_kind) menu_kind = 0;
        else if (btn == 1 && hit == HIT_NONE) toggle_pause();
        return;
    }
    if (hit >= HIT_ROW0) { menu_pick(hit - HIT_ROW0); return; }
    switch (hit) {
    case HIT_MENU: return;
    case HIT_SEEK:
        menu_kind = 0;
        if (P.duration > 0) { dragging = true; last_drag_seek = now_s(); seek_to(P.duration * osd_frac(HIT_SEEK, x), true); }
        return;
    case HIT_PLAY: toggle_pause(); return;
    case HIT_VOL_ICON: g_mute = !g_mute; return;
    case HIT_VOL: drag_vol = true; set_volume_frac(osd_frac(HIT_VOL, x)); return;
    case HIT_AUDIO: open_menu(1); return;
    case HIT_SUBS: open_menu(2); return;
    case HIT_FS: wl_toggle_fullscreen(); return;
    default: break;
    }
    if (menu_kind) { menu_kind = 0; return; }       /* click outside closes the menu */
    double t = now_s();
    if (t - last_click < 0.35) { toggle_pause(); wl_toggle_fullscreen(); last_click = 0; }
    else { toggle_pause(); last_click = t; }
}
static void on_scroll(double dy)
{
    scroll_acc += dy;
    if (menu_kind && hover_hit >= HIT_MENU) {
        int max = menu_count() - osd_menu_rows();
        while (scroll_acc <= -10) { if (menu_scroll > 0) menu_scroll--; scroll_acc += 10; }
        while (scroll_acc >= 10) { if (menu_scroll < max) menu_scroll++; scroll_acc -= 10; }
        need_redraw = true;
        return;
    }
    while (scroll_acc <= -10) { change_volume(2); scroll_acc += 10; }
    while (scroll_acc >= 10) { change_volume(-2); scroll_acc -= 10; }
}
static void on_resize(int w, int h, double s)
{
    win_w = w; win_h = h; win_scale = s;
    if (!gl_ready) return;
    gl_resize(w, h);
    osd_resize(w, h, s);
    need_redraw = true;
    frame_pending = false;
}
static void on_close(void) { quit = true; }
static int n_callbacks;
static double last_callback;
static void on_frame(void) { frame_pending = false; n_callbacks++; last_callback = now_s(); }

/* ------------------------------------------------------------- MPRIS */
static void m_play(void) { if (P.paused || ended) toggle_pause(); }
static void m_pause(void) { if (!P.paused && !ended) toggle_pause(); }
static void m_stop(void) { quit = true; }
static void m_next(void) { if (cur_file + 1 < n_files) open_next(1); }
static void m_prev(void) { if (cur_file > 0) open_next(-1); else seek_to(0, true); }
static void m_seek_rel(double s) { seek_rel(s, true); }
static void m_set_pos(double s) { seek_to(s, true); }
static double m_pos(void) { return P.opened ? player_pos() : 0; }

/* --------------------------------------------------------- video tick */
static double video_refresh(void)
{
    VFrame v;
    double next;
    for (;;) {
        if (!fq_peek(&v, &next)) return 1e9;
        if (P.force_show) {
            P.force_show = false;
            P.await_video = false;
            tr_stage("first frame popped");
            fq_pop();
            gl_set_frame(v.f, P.rotation);
            P.shown++;
            need_redraw = true;
            if (!player_audio_alive()) {
                clock_set(v.pts);
                P.await = false;
                clock_run(!P.paused);
            }
            continue;
        }
        if (P.paused) return 1e9;
        if (P.await) return 0.005;
        double clk = player_clock(), d = v.pts - clk;
        if (d > 10) { clock_set(v.pts); d = 0; }          /* timestamp jump */
        if (d > 0.001) return d;
        if (!isnan(next) && next <= clk) {                /* late and a newer one is due too */
            av_frame_free(&v.f);
            fq_pop();
            P.dropped++;
            continue;
        }
        fq_pop();
        gl_set_frame(v.f, P.rotation);
        P.shown++;
        need_redraw = true;
    }
}

static void build_stats(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    double cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6;
    static double last_cpu, last_t, pct;
    double t = now_s();
    if (last_t && t - last_t > 0.5) pct = 100 * (cpu - last_cpu) / (t - last_t);
    if (!last_t || t - last_t > 0.5) { last_cpu = cpu; last_t = t; }
    char *o = stats;
    size_t left = sizeof stats;
    int n = snprintf(o, left, "Turrón %s\n", TURRON_VERSION);
    o += n; left -= n;
    if (P.vctx) {
        static const char *deg_es[] = { "", " · modo ahorro", " · modo ahorro máximo" };
        static const char *deg_en[] = { "", " · saving mode", " · maximum saving mode" };
        int dg = P.degraded;
        char how[96];
        snprintf(how, sizeof how, "%s%s", P.hw ? "VA-API (GPU)" : "software (CPU)", g_es ? deg_es[dg] : deg_en[dg]);
        n = snprintf(o, left, "%s: %s · %.3f fps\n%s: %s\n%s: %s\n", T("Vídeo", "Video"), P.vcodec, P.fps,
                     T("Decodificación", "Decoding"), how,
                     T("Imagen", "Frame"), gl_frame_desc());
        o += n; left -= n;
    }
    if (P.actx) {
        n = snprintf(o, left, "Audio: %s · %d Hz · %d ch\n", P.acodec, P.a_rate, P.a_ch);
        o += n; left -= n;
    }
    int stn = P.want_strack;
    n = snprintf(o, left, "%s: %s\n", T("Subtítulos", "Subtitles"), stn >= 0 && stn < P.n_stracks ? P.stracks[stn].label : T("desactivados", "off"));
    o += n; left -= n;
    snprintf(o, left, "%s: %d · %s: %d\nCPU: %.0f %% %s\n%s: %dx%d @%.2f", T("Mostrados", "Shown"), atomic_load(&P.shown),
             T("descartados", "dropped"), atomic_load(&P.dropped), pct, T("de un núcleo", "of one core"),
             T("Ventana", "Window"), win_w, win_h, win_scale);
}

static double tick(void)
{
    double t = now_s(), wait = 1e9;
    if (!P.opened) return wait;

    for (SubMsg *m = player_take_subs(), *nx; m; m = nx) { nx = m->next; osd_sub_msg(m); }
    if (P.serial != last_serial) { if (last_serial >= 0) osd_seek(); last_serial = P.serial; }
    if (P.await && !P.force_show && !P.paused && !player_audio_alive() && P.vi < 0) { P.await = false; clock_run(true); }

    static double trace_at;
    static int trace_on = -1;
    if (trace_on < 0) trace_on = getenv("TURRON_TRACE") != NULL;
    if (trace_on && t - trace_at >= 1.0) {
        static int l_dec, l_shown, l_drop, l_cb;
        extern int gl_n_render;
        static int l_r;
        fprintf(stderr, "t=%5.1f pos=%7.2f dec=%3d shown=%3d drop=%3d renders=%3d cbs=%3d jumps=%d deg=%d await=%d\n", t - t_start, player_pos(),
                P.decoded - l_dec, P.shown - l_shown, P.dropped - l_drop, gl_n_render - l_r, n_callbacks - l_cb, (int)P.jumps, (int)P.degraded, (int)P.await);
        void player_debug(int *v, int *a, int *f);
        int qv, qa, qf;
        player_debug(&qv, &qa, &qf);
        struct rusage tru;
        getrusage(RUSAGE_SELF, &tru);
        static double l_cpu;
        double cpu = tru.ru_utime.tv_sec + tru.ru_utime.tv_usec * 1e-6 + tru.ru_stime.tv_sec + tru.ru_stime.tv_usec * 1e-6;
        fprintf(stderr, "        vq=%d aq=%d fq=%d cpu=%.2f await_video=%d\n", qv, qa, qf, cpu - l_cpu, (int)P.await_video);
        l_cpu = cpu;
        l_dec = P.decoded; l_shown = P.shown; l_drop = P.dropped; l_r = gl_n_render; l_cb = n_callbacks;
        trace_at = t;
    }
    double d = video_refresh();
    if (d < wait) wait = d;
    static int seen_jumps;
    if (P.jumps < seen_jumps) seen_jumps = 0;
    if (P.jumps > seen_jumps) {
        if (seen_jumps == 0) {
            set_toast("%s", T("Vídeo demasiado pesado para este equipo: la imagen irá a saltos",
                              "Video too heavy for this computer: the picture will skip"));
            toast_until = t + 5;
        }
        seen_jumps = P.jumps;
    }
    if (P.await_video) {
        if (t - P.await_video_since > 1.5 || P.v_eof_serial == P.serial) P.await_video = false;
        else if (wait > 0.05) wait = 0.05;
    }

    if (!ended && player_at_end()) {
        if (cur_file + 1 < n_files) { open_next(1); return 0; }
        if (!O.keep_open) { quit = true; return 0; }
        ended = true;
        player_set_paused(true);
        show_bar(1e7);
        sync_state();
    }

    /* overlay */
    bool bar = t < bar_until || P.paused || dragging || drag_vol || menu_kind;
    if (toast[0] && t >= toast_until) toast[0] = 0;
    if (show_stats && t - stats_at >= 1.0) { stats_at = t; build_stats(); }
    if (!bar && t >= cursor_until) wl_hide_cursor(true);
    if (!bar) hover_hit = HIT_NONE;
    double pos = now_s() - last_seek_at < 0.4 && P.await ? last_seek_target : player_pos();
    static const char *items[MAX_TRACKS + 1];
    int n_items = 0, cur_item = -1;
    if (menu_kind == 1) {
        for (int i = 0; i < P.n_atracks; i++) items[n_items++] = P.atracks[i].label;
        cur_item = P.cur_atrack;
    } else if (menu_kind == 2) {
        items[n_items++] = T("Desactivados", "Off");
        for (int i = 0; i < P.n_stracks; i++) items[n_items++] = P.stracks[i].label;
        cur_item = P.want_strack + 1;
    }
    if (menu_kind && !n_items) menu_kind = 0;
    struct OsdState st = {
        .bar = bar, .paused = P.paused, .mute = g_mute, .fullscreen = wl_is_fullscreen(),
        .pos = pos, .dur = P.duration, .volume = atomic_load(&g_volume),
        .hover = bar ? hover_hit : HIT_NONE, .menu = menu_kind, .items = items, .n_items = n_items,
        .cur_item = cur_item, .menu_scroll = menu_scroll,
        .title = title, .toast = toast[0] ? toast : NULL, .stats = show_stats ? stats : NULL,
    };
    if (gl_ready) {
        int x, y, w, h, vw, vh;
        gl_video_rect(&x, &y, &w, &h, &vw, &vh);
        osd_set_video_rect(x, y, w, h, vw, vh);
        const uint8_t *pix = NULL;
        int stride = 0, dirty[4], bbox[4];
        if (osd_update(&st, player_clock(), &pix, &stride, dirty, bbox)) {
            gl_osd_upload(pix, stride, dirty, bbox);
            need_redraw = true;
        }
        /* Frame callbacks arrive a refresh late on some compositors, so waiting for
         * one before every draw halves the frame rate. Draw whenever a picture is due
         * and use the callbacks only to notice that the window is hidden. */
        bool visible = t - last_callback < 0.25;
        if (need_redraw && (!frame_pending || visible)) {
            need_redraw = false;
            frame_pending = true;
            wl_request_frame();
            if (O.shot_path && O.shot_at >= 0 && t - t_start >= O.shot_at) { gl_shot(O.shot_path); O.shot_at = -1; }
            gl_render();
            { static int fr; if (fr < 2 && P.shown > 0) { fr++; tr_stage("frame rendered"); } }
        }
    }

    if (bar && !P.paused && wait > 0.25) wait = 0.25;
    if (toast[0] && toast_until - t < wait) wait = toast_until - t;
    if (bar && !P.paused && bar_until - t < wait) wait = bar_until - t;
    if ((show_stats || trace_on) && wait > 1.0) wait = 1.0;
    if (O.quit_after >= 0) {
        if (t - t_start >= O.quit_after) quit = true;
        else if (O.quit_after - (t - t_start) < wait) wait = O.quit_after - (t - t_start);
    }
    if (O.shot_at >= 0 && wait > 0.1) wait = 0.1;
    return wait < 0 ? 0 : wait;
}

/* Test hook: TURRON_KEYS="space@2,Right@3.5,s@5" presses keys at the given seconds. */
static double inject_keys(double t)
{
    static char *list;
    static bool init;
    if (!init) { init = true; const char *e = getenv("TURRON_KEYS"); list = e ? strdup(e) : NULL; }
    while (list && *list) {
        char *at = strchr(list, '@');
        if (!at) { list = NULL; break; }
        double when = atof(at + 1);
        if (t - t_start < when) return when - (t - t_start);
        char name[48];
        snprintf(name, sizeof name, "%.*s", (int)(at - list), list);
        double cx, cy;
        if (sscanf(name, "click:%lf:%lf", &cx, &cy) == 2 || sscanf(name, "move:%lf:%lf", &cx, &cy) == 2) {
            cx = cx < 0 ? win_w + cx * win_scale : cx * win_scale;
            cy = cy < 0 ? win_h + cy * win_scale : cy * win_scale;
            on_motion(cx, cy);
            if (name[0] == 'c') { on_button(0, true, cx, cy); on_button(0, false, cx, cy); }
            char *cm = strchr(at, ',');
            list = cm ? cm + 1 : NULL;
            continue;
        }
        uint32_t mods = 0;
        const char *n = name;
        if (!strncmp(n, "C-", 2)) { mods = 2; n += 2; } else if (!strncmp(n, "S-", 2)) { mods = 1; n += 2; }
        xkb_keysym_t sym = xkb_keysym_from_name(n, XKB_KEYSYM_NO_FLAGS);
        if (sym != XKB_KEY_NoSymbol) on_key(sym, mods);
        char *comma = strchr(at, ',');
        list = comma ? comma + 1 : NULL;
    }
    return 1e9;
}

static void on_signal(int s) { (void)s; quit = true; player_wake_main(); }

static void usage(void)
{
    printf("Turrón %s — %s\n\n%s: turron [%s] %s...\n\n"
           "  --start=SEG        %s\n  --volume=N         %s\n  --fs               %s\n"
           "  --no-hw            %s\n  --fast             %s\n  --keep-open        %s\n"
           "  --no-resume        %s\n  --stats            %s\n  --audio-target=X   %s\n\n%s\n",
           TURRON_VERSION, T("reproductor de vídeo ligero", "lightweight video player"),
           T("Uso", "Usage"), T("opciones", "options"), T("archivo|URL", "file|URL"),
           T("empezar en ese segundo", "start at that second"), T("volumen inicial (0-150)", "initial volume (0-150)"),
           T("pantalla completa", "fullscreen"), T("no usar la GPU para decodificar", "do not decode on the GPU"),
           T("decodificación rápida (menos calidad) para vídeos que no llegan a tiempo real", "fast, lower quality decoding for videos that cannot keep up"),
           T("no cerrar al terminar", "do not close at the end"), T("no recordar la posición", "do not remember the position"),
           T("mostrar estadísticas (tecla i)", "show statistics (key i)"), T("salida de PipeWire", "PipeWire sink"),
           T("Teclas: espacio pausa · ←/→ 5 s (Mayús 1 s, Ctrl 60 s) · ↑/↓ volumen · f pantalla completa\n"
             "        s subtítulos · a audio · m silencio · n/b siguiente/anterior · i estadísticas · 0-9 saltar · q salir",
             "Keys: space pause · ←/→ 5 s (Shift 1 s, Ctrl 60 s) · ↑/↓ volume · f fullscreen\n"
             "      s subtitles · a audio · m mute · n/b next/previous · i statistics · 0-9 jump · q quit"));
}

/* File managers hand over file:// URIs with percent-escapes; FFmpeg wants the plain path. */
static char *local_path(char *arg)
{
    if (strncmp(arg, "file://", 7)) return arg;
    const char *p = arg + 7;
    if (*p != '/') { p = strchr(p, '/'); if (!p) return arg; }      /* skip "localhost" */
    char *out = malloc(strlen(p) + 1), *o = out;
    for (; *p; p++) {
        unsigned v;
        if (*p == '%' && p[1] && p[2] && sscanf(p + 1, "%2x", &v) == 1) { *o++ = (char)v; p += 2; }
        else *o++ = *p;
    }
    *o = 0;
    return out;
}

int main(int argc, char **argv)
{
    const char *loc = getenv("LC_ALL");
    if (!loc || !*loc) loc = getenv("LC_MESSAGES");
    if (!loc || !*loc) loc = getenv("LANG");
    g_es = loc && !strncmp(loc, "es", 2);

    tr_stage("start");
    files = calloc(argc, sizeof *files);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) { files[n_files++] = local_path(argv[i]); continue; }
        if (!strcmp(a, "--")) { while (++i < argc) files[n_files++] = local_path(argv[i]); break; }
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        if (!strcmp(a, "--version")) { printf("turron %s\n", TURRON_VERSION); return 0; }
        if (!strcmp(a, "--no-hw")) O.no_hw = true;
        else if (!strcmp(a, "--fast")) O.fast = true;
        else if (!strcmp(a, "--keep-open")) O.keep_open = true;
        else if (!strcmp(a, "--fs")) O.fullscreen = true;
        else if (!strcmp(a, "--no-resume")) O.no_resume = true;
        else if (!strcmp(a, "--stats")) show_stats = true;
        else if (!strcmp(a, "--no-subs")) O.no_subs = true;
        else if (!strncmp(a, "--start=", 8)) O.start = atof(a + 8);
        else if (!strncmp(a, "--volume=", 9)) O.volume = atoi(a + 9);
        else if (!strncmp(a, "--audio-target=", 15)) O.audio_target = a + 15;
        else if (!strncmp(a, "--quit-after=", 13)) O.quit_after = atof(a + 13);
        else if (!strncmp(a, "--shot=", 7)) O.shot_path = a + 7;
        else if (!strncmp(a, "--shot-at=", 10)) O.shot_at = atof(a + 10);
        else { fprintf(stderr, "turron: %s: %s\n", T("opción desconocida", "unknown option"), a); return 2; }
    }
    if (!n_files) { usage(); return 2; }
    if (O.volume >= 0) atomic_store(&g_volume, O.volume > 150 ? 150 : O.volume);
    if (O.shot_path && O.shot_at < 0) O.shot_at = 1;

    av_log_set_level(AV_LOG_ERROR);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    g_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    t_start = now_s();
    if (wl_connect() == 0) gl_prewarm(wl_display_ptr());
    tr_stage("wl_connect");
    audio_init();
    tr_stage("audio_init");
    osd_start();

    for (int i = 0; i < n_files && !P.opened; i++) open_file(i, O.start, -1);
    if (!P.opened) return 1;
    tr_stage("open_file");

    int w = 960, h = 540;
    if (P.vi >= 0 && P.vid_w > 0 && P.vid_h > 0) {
        double vw = P.vid_w, vh = P.vid_h;
        AVRational sar = P.vid_sar;
        if (sar.num > 0 && sar.den > 0) vw = vw * sar.num / sar.den;
        if (P.rotation == 90 || P.rotation == 270) { double tt = vw; vw = vh; vh = tt; }
        double k = fmin(1.0, fmin(1280 / vw, 720 / vh));
        w = (int)fmax(320, vw * k);
        h = (int)fmax(180, vh * k);
    }
    static const struct WlCallbacks cb = { on_key, on_button, on_motion, on_scroll, on_resize, on_close, on_frame };
    if (wl_init(&cb, "Turrón", w, h) < 0) {
        fprintf(stderr, "turron: %s\n", T("no se pudo conectar con Wayland", "cannot connect to Wayland"));
        player_close();
        return 1;
    }
    tr_stage("wl_init");
    wl_get_size(&win_w, &win_h, &win_scale);
    if (gl_init(wl_display_ptr(), wl_surface_ptr(), win_w, win_h) < 0) {
        fprintf(stderr, "turron: %s\n", T("no se pudo iniciar OpenGL ES 3", "cannot initialise OpenGL ES 3"));
        player_close();
        return 1;
    }
    gl_ready = true;
    tr_stage("gl_init");
    osd_resize(win_w, win_h, win_scale);
    update_title();
    static const struct MprisCallbacks mcb = { toggle_pause, m_play, m_pause, m_stop, m_next, m_prev, m_seek_rel, m_set_pos, m_pos };
    mpris_init(&mcb);
    tr_stage("mpris_init");
    sync_state();
    need_redraw = true;

    while (!quit) {
        double ik = inject_keys(now_s());
        double wait = tick();
        if (ik < wait) wait = ik;
        if (quit) break;
        if (wl_prepare() < 0) break;
        double rep = wl_next_repeat();
        if (rep > 0 && rep - now_s() < wait) wait = fmax(0, rep - now_s());
        struct pollfd fds[3] = { { wl_fd(), POLLIN, 0 }, { g_wake_fd, POLLIN, 0 }, { mpris_fd(), POLLIN, 0 } };
        struct timespec ts = { (time_t)fmin(wait, 3600), (long)(fmod(fmin(wait, 3600), 1.0) * 1e9) };
        int r = ppoll(fds, fds[2].fd >= 0 ? 3 : 2, &ts, NULL);
        if (wl_dispatch(r > 0 && (fds[0].revents & POLLIN)) < 0) break;
        if (r > 0 && (fds[0].revents & (POLLERR | POLLHUP))) break;
        if (r > 0 && (fds[1].revents & POLLIN)) { uint64_t v; if (read(g_wake_fd, &v, 8) < 0) { /* drained */ } }
        if (r > 0 && fds[2].fd >= 0 && fds[2].revents) mpris_dispatch();
        if (rep > 0 && wl_next_repeat() > 0 && now_s() >= wl_next_repeat()) wl_do_repeat();
    }
    save_resume();
    if (O.quit_after >= 0 || show_stats) {
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        fprintf(stderr, "turron: shown=%d dropped=%d cpu=%.2fs wall=%.2fs hw=%d %s\n", atomic_load(&P.shown), atomic_load(&P.dropped),
                ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6,
                now_s() - t_start, (int)P.hw, gl_frame_desc());
        fprintf(stderr, "turron: degraded=%d no_exact=%d\n", (int)P.degraded, (int)P.no_exact);
        extern double gl_t_upload, gl_t_draw, gl_t_swap;
        extern int gl_n_render;
        fprintf(stderr, "turron: renders=%d upload=%.2fs draw=%.2fs swap=%.2fs | threads:", gl_n_render, gl_t_upload, gl_t_draw, gl_t_swap);
        char cmd[128];
        snprintf(cmd, sizeof cmd, "cat /proc/%d/task/*/stat 2>/dev/null", (int)getpid());
        FILE *pf = popen(cmd, "r");
        if (pf) {
            char line[1024], last[32] = "";
            double acc = 0;
            while (fgets(line, sizeof line, pf)) {
                char *l = strchr(line, '('), *r = strrchr(line, ')');
                if (!l || !r) continue;
                *r = 0;
                unsigned long ut = 0, st = 0;
                sscanf(r + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st);
                if (strcmp(last, l + 1)) { if (*last) fprintf(stderr, " %s=%.2f", last, acc); snprintf(last, sizeof last, "%s", l + 1); acc = 0; }
                acc += (ut + st) / (double)sysconf(_SC_CLK_TCK);
            }
            if (*last) fprintf(stderr, " %s=%.2f", last, acc);
            pclose(pf);
        }
        fprintf(stderr, "\n");
    }
    player_close();
    return 0;
}
