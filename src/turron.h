/* Turrón — reproductor de vídeo ligero. GPL-3.0-or-later. */
#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

#define TURRON_VERSION "0.1.0"
#define MAX_TRACKS 64

static inline double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ---------- i18n: Spanish when the locale is es*, English otherwise ---------- */
extern bool g_es;
#define T(es, en) (g_es ? (es) : (en))

/* ---------- options ---------- */
struct Opts {
    bool no_hw, fast, keep_open, fullscreen, no_resume, stats, no_subs;
    double start, quit_after, shot_at;
    const char *shot_path, *audio_target;
    int volume;
};
extern struct Opts O;

/* ---------- player core (player.c) ---------- */
typedef struct VFrame {
    AVFrame *f;
    double pts, dur;
    int serial;
} VFrame;

enum { SUB_RESET, SUB_TEXT, SUB_BITMAP };
typedef struct SubMsg {
    int type, serial;
    double pts, dur;           /* seconds; dur < 0 = until the next one */
    AVSubtitle sub;            /* SUB_BITMAP (owned) */
    char *text;                /* SUB_TEXT: ASS chunk; SUB_RESET: ASS header */
    int text_len;
    int w, h;                  /* SUB_RESET: canvas of bitmap subs */
    struct SubMsg *next;
} SubMsg;

typedef struct Track {
    int stream;                /* AVStream index, or -1 for an external file */
    char label[160];
    char lang[16];
    char *ext_path;
} Track;

typedef struct Clock {
    pthread_mutex_t mu;
    double pts, at;
    bool running;
} Clock;

struct Player {
    char *path;
    AVFormatContext *ic;
    int vi, ai;                         /* stream indexes, -1 if none */
    AVStream *vst, *ast;
    AVCodecContext *vctx, *actx;
    atomic_bool hw;                     /* video decoded by VA-API */
    double start_time, duration;        /* seconds */
    int rotation;                       /* 0/90/180/270 */
    double fps;

    Track atracks[MAX_TRACKS], stracks[MAX_TRACKS];
    int n_atracks, n_stracks;
    int cur_atrack;                     /* index in atracks, -1 none */
    atomic_int want_strack;             /* index in stracks, -1 = off */

    atomic_int serial;
    atomic_bool abort, paused, await, force_show;
    atomic_bool await_video;            /* audio is held until the first picture after a seek is ready */
    double await_video_since;
    atomic_bool demux_eof;
    atomic_int v_eof_serial, a_eof_serial;
    atomic_int dropped, shown, decoded, jumps;
    atomic_int degraded;                /* 0 normal, 1 skipping non-reference frames, 2 also no loop filter */
    atomic_bool no_exact;               /* this file decodes too slowly for exact seeks */
    int vid_w, vid_h;                   /* coded size, for the first window */
    AVRational vid_sar;
    char vcodec[32], acodec[32];
    int a_rate, a_ch;
    Clock clk;
    _Atomic double exact_target;        /* frames before this are discarded; <0 = none */
    bool opened;
};
extern struct Player P;

int  player_open(const char *path, double start, int audio_track);
void player_close(void);
void player_seek(double pos, bool exact);   /* pos = seconds from start of file */
void player_set_paused(bool p);
double player_clock(void);              /* raw pts seconds */
double player_pos(void);                /* seconds from start of file */
void clock_set(double pts);
void clock_run(bool run);
bool player_at_end(void);
bool player_audio_alive(void);
/* video frame queue, consumed by the main thread */
bool fq_peek(VFrame *out, double *next_pts);   /* next_pts = NAN when there is no next frame */
void fq_pop(void);
/* subtitle messages, consumed by the main thread */
SubMsg *player_take_subs(void);
void submsg_free(SubMsg *m);
void player_wake_main(void);
extern int g_wake_fd;

/* ---------- audio out (audio.c) ---------- */
int  audio_init(void);
int  audio_open(int rate);
void audio_close(void);
bool audio_write(const float *st, int frames, double pts, int serial); /* false if aborted */
void audio_flush(void);
void audio_set_active(bool on);
bool audio_drained(void);
extern atomic_int g_volume;            /* 0..150 */
extern atomic_bool g_mute;

/* ---------- OSD + subtitles (osd.c) ---------- */
struct OsdState {
    bool bar, hover_bar, paused;
    double pos, dur;
    const char *title;
    const char *toast;
    const char *right;
    const char *stats;
};
void osd_start(void);                               /* background font init */
void osd_resize(int w, int h, double scale);
void osd_set_video_rect(int x, int y, int w, int h, int vw, int vh);
void osd_sub_msg(SubMsg *m);                        /* takes ownership */
void osd_load_external(const char *path);           /* NULL = unload */
void osd_reset(void);                               /* new file */
void osd_invalidate(void);
/* Rebuilds the overlay if something changed. Returns true if pixels changed. */
bool osd_update(const struct OsdState *st, double sub_time,
                const uint8_t **pix, int *stride, int dirty[4], int bbox[4]);
void osd_bar_geometry(int *x, int *y, int *w, int *h); /* seek bar hit box, px */

/* ---------- GL renderer (gl.c) ---------- */
int  gl_init(void *wl_display, void *wl_surface, int w, int h);
void gl_resize(int w, int h);
void gl_set_frame(AVFrame *f, int rotation);        /* takes ownership of the reference */
void gl_drop_frame(void);
void gl_video_rect(int *x, int *y, int *w, int *h, int *vw, int *vh);
void gl_osd_upload(const uint8_t *pix, int stride, const int dirty[4], const int bbox[4]);
void gl_render(void);                               /* draws + swaps */
void gl_shot(const char *path);
const char *gl_frame_desc(void);

/* ---------- window (wl.c) ---------- */
struct WlCallbacks {
    void (*key)(uint32_t sym, uint32_t mods);       /* mods: 1 shift, 2 ctrl, 4 alt */
    void (*button)(int btn, bool pressed, double x, double y);  /* px */
    void (*motion)(double x, double y);
    void (*scroll)(double dy);
    void (*resize)(int pxw, int pxh, double scale);
    void (*close)(void);
    void (*frame)(void);
};
int  wl_init(const struct WlCallbacks *cb, const char *title, int w, int h);
int  wl_fd(void);
void *wl_display_ptr(void);
void *wl_surface_ptr(void);
int  wl_prepare(void);                              /* dispatch pending + flush; <0 on error */
int  wl_dispatch(bool readable);
double wl_next_repeat(void);                        /* absolute time, or 0 */
void wl_do_repeat(void);
void wl_request_frame(void);
void wl_set_title(const char *t);
void wl_toggle_fullscreen(void);
void wl_inhibit_idle(bool on);
void wl_hide_cursor(bool hide);
void wl_commit_size(int pxw, int pxh);

/* ---------- MPRIS (mpris.c) ---------- */
struct MprisCallbacks {
    void (*play_pause)(void);
    void (*play)(void);
    void (*pause)(void);
    void (*stop)(void);
    void (*next)(void);
    void (*prev)(void);
    void (*seek_rel)(double s);
    void (*set_pos)(double s);
    double (*pos)(void);
};
int  mpris_init(const struct MprisCallbacks *cb);
int  mpris_fd(void);
void mpris_dispatch(void);
void mpris_update(const char *title, const char *path, double dur, bool playing);
void mpris_seeked(double pos);
