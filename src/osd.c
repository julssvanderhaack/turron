/* Turrón — on-screen display and subtitles, composed on the CPU into one premultiplied
 * RGBA overlay that is only rebuilt (and re-uploaded) when something changes.
 * Text for both the OSD and the subtitles is rendered by libass. */
#include "turron.h"
#include <limits.h>
#include <math.h>
#include <ass/ass.h>

static ASS_Library *lib;
static ASS_Renderer *r_sub, *r_osd;
static ASS_Track *t_sub, *t_osd;
static atomic_bool ready;
static pthread_t th_init;

static uint8_t *pix;
static int W, H;
static double S = 1;
static int vrect[4], vid_w, vid_h;
static int prev_bb[4];
static uint64_t prev_sig;
static int stats_top;
static bool force = true, fonts_added, geometry_dirty = true;
static SubMsg *pending, *pending_last;

typedef struct BRect { int x, y, w, h; uint8_t *rgba; } BRect;
typedef struct BEvent { double start, end; int cw, ch, n; BRect *r; struct BEvent *next; } BEvent;
static BEvent *bevents;
static const BEvent *b_drawn;

/* ------------------------------------------------------------------ init */
static void ass_msg(int level, const char *fmt, va_list va, void *data) { (void)level; (void)fmt; (void)va; (void)data; }

static void *init_thread(void *arg)
{
    (void)arg;
    lib = ass_library_init();
    ass_set_message_cb(lib, ass_msg, NULL);
    ass_set_extract_fonts(lib, 1);
    r_sub = ass_renderer_init(lib);
    r_osd = ass_renderer_init(lib);
    ass_set_fonts(r_sub, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    ass_set_fonts(r_osd, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    ready = true;
    player_wake_main();
    return NULL;
}
void osd_start(void) { pthread_create(&th_init, NULL, init_thread, NULL); }

void osd_resize(int w, int h, double scale)
{
    if (w == W && h == H && scale == S) return;
    W = w; H = h; S = scale;
    free(pix);
    pix = calloc((size_t)W * H, 4);
    memset(prev_bb, 0, sizeof prev_bb);
    geometry_dirty = true;
    force = true;
}

void osd_set_video_rect(int x, int y, int w, int h, int vw, int vh)
{
    if (x == vrect[0] && y == vrect[1] && w == vrect[2] && h == vrect[3] && vw == vid_w && vh == vid_h) return;
    vrect[0] = x; vrect[1] = y; vrect[2] = w; vrect[3] = h;
    vid_w = vw; vid_h = vh;
    geometry_dirty = true;
    force = true;
}

void osd_invalidate(void) { force = true; }

/* ------------------------------------------------------------ subtitles */
static void bevents_clear(void)
{
    for (BEvent *e = bevents, *nx; e; e = nx) {
        nx = e->next;
        for (int i = 0; i < e->n; i++) free(e->r[i].rgba);
        free(e->r);
        free(e);
    }
    bevents = NULL;
    b_drawn = NULL;
    force = true;
}

static void add_fonts(void)
{
    if (fonts_added || !P.ic) return;
    fonts_added = true;
    bool any = false;
    for (unsigned i = 0; i < P.ic->nb_streams; i++) {
        AVStream *st = P.ic->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_ATTACHMENT || !st->codecpar->extradata_size) continue;
        AVDictionaryEntry *name = av_dict_get(st->metadata, "filename", NULL, 0);
        AVDictionaryEntry *mime = av_dict_get(st->metadata, "mimetype", NULL, 0);
        bool font = st->codecpar->codec_id == AV_CODEC_ID_TTF || st->codecpar->codec_id == AV_CODEC_ID_OTF ||
                    (mime && (strstr(mime->value, "font") || strstr(mime->value, "ttf") || strstr(mime->value, "otf")));
        if (!font) continue;
        ass_add_font(lib, name ? name->value : "font", (const char *)st->codecpar->extradata, st->codecpar->extradata_size);
        any = true;
    }
    if (any) ass_set_fonts(r_sub, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
}

static bool is_utf8(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return true;
    static uint8_t buf[1 << 18];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    for (size_t i = 0; i < n;) {
        uint8_t c = buf[i];
        int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!len) return false;
        if (i + len > n) break;
        for (int k = 1; k < len; k++) if ((buf[i + k] & 0xC0) != 0x80) return false;
        i += len;
    }
    return true;
}

void osd_load_external(const char *path)
{
    if (t_sub) { ass_free_track(t_sub); t_sub = NULL; }
    force = true;
    if (!path) return;
    AVFormatContext *ic = NULL;
    if (avformat_open_input(&ic, path, NULL, NULL) < 0) return;
    avformat_find_stream_info(ic, NULL);
    int si = av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0);
    const AVCodec *codec = si >= 0 ? avcodec_find_decoder(ic->streams[si]->codecpar->codec_id) : NULL;
    AVCodecContext *c = codec ? avcodec_alloc_context3(codec) : NULL;
    if (c) {
        avcodec_parameters_to_context(c, ic->streams[si]->codecpar);
        c->pkt_timebase = ic->streams[si]->time_base;
        AVDictionary *o = NULL;
        if (!is_utf8(path)) av_dict_set(&o, "sub_charenc", "CP1252", 0);
        if (avcodec_open2(c, codec, &o) >= 0) {
            t_sub = ass_new_track(lib);
            if (c->subtitle_header)
                ass_process_codec_private(t_sub, (char *)c->subtitle_header, c->subtitle_header_size);
            AVPacket *pkt = av_packet_alloc();
            double tb = av_q2d(c->pkt_timebase);
            while (av_read_frame(ic, pkt) >= 0) {
                AVSubtitle sub;
                int got = 0;
                if (pkt->stream_index == si && avcodec_decode_subtitle2(c, &sub, &got, pkt) >= 0 && got) {
                    long long start = llrint(pkt->pts * tb * 1000) + sub.start_display_time;
                    long long dur = sub.end_display_time && sub.end_display_time != UINT32_MAX
                                  ? sub.end_display_time - sub.start_display_time : llrint(pkt->duration * tb * 1000);
                    for (unsigned i = 0; i < sub.num_rects; i++)
                        if (sub.rects[i]->ass)
                            ass_process_chunk(t_sub, sub.rects[i]->ass, strlen(sub.rects[i]->ass), start, dur);
                    avsubtitle_free(&sub);
                }
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        }
        av_dict_free(&o);
        avcodec_free_context(&c);
    }
    avformat_close_input(&ic);
}

void osd_sub_msg(SubMsg *m)
{
    m->next = NULL;
    if (pending_last) pending_last->next = m; else pending = m;
    pending_last = m;
}

static void process_msg(SubMsg *m)
{
    switch (m->type) {
    case SUB_RESET:
        if (t_sub) { ass_free_track(t_sub); t_sub = NULL; }
        bevents_clear();
        if (m->serial >= 0 && m->serial < P.n_stracks && P.stracks[m->serial].ext_path) {
            osd_load_external(P.stracks[m->serial].ext_path);
        } else if (m->text) {
            add_fonts();
            t_sub = ass_new_track(lib);
            ass_process_codec_private(t_sub, m->text, m->text_len);
        }
        break;
    case SUB_TEXT:
        if (t_sub) ass_process_chunk(t_sub, m->text, m->text_len, llrint(m->pts * 1000), llrint(m->dur * 1000));
        break;
    case SUB_BITMAP: {
        for (BEvent *e = bevents; e; e = e->next) if (e->end > m->pts) e->end = m->pts;
        if (!m->sub.num_rects) break;
        BEvent *e = calloc(1, sizeof *e);
        e->start = m->pts;
        e->end = m->dur < 0 ? INFINITY : m->pts + m->dur;
        e->cw = m->w; e->ch = m->h;
        e->r = calloc(m->sub.num_rects, sizeof *e->r);
        for (unsigned i = 0; i < m->sub.num_rects; i++) {
            const AVSubtitleRect *r = m->sub.rects[i];
            if (r->type != SUBTITLE_BITMAP || !r->data[0] || !r->data[1] || r->w <= 0 || r->h <= 0) continue;
            BRect *b = &e->r[e->n++];
            b->x = r->x; b->y = r->y; b->w = r->w; b->h = r->h;
            b->rgba = malloc((size_t)r->w * r->h * 4);
            const uint32_t *pal = (const uint32_t *)r->data[1];
            for (int y = 0; y < r->h; y++) {
                const uint8_t *src = r->data[0] + y * r->linesize[0];
                uint8_t *dst = b->rgba + (size_t)y * r->w * 4;
                for (int x = 0; x < r->w; x++) {
                    uint32_t c = pal[src[x]], a = c >> 24;
                    dst[x * 4 + 0] = ((c >> 16) & 255) * a / 255;
                    dst[x * 4 + 1] = ((c >> 8) & 255) * a / 255;
                    dst[x * 4 + 2] = (c & 255) * a / 255;
                    dst[x * 4 + 3] = a;
                }
            }
        }
        e->next = bevents;
        bevents = e;
        break;
    }
    }
    submsg_free(m);
}

void osd_seek(void) { bevents_clear(); }

void osd_reset(void)
{
    for (SubMsg *m = pending, *nx; m; m = nx) { nx = m->next; submsg_free(m); }
    pending = pending_last = NULL;
    if (ready) {
        if (t_sub) { ass_free_track(t_sub); t_sub = NULL; }
        ass_clear_fonts(lib);
        if (fonts_added) ass_set_fonts(r_sub, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    }
    fonts_added = false;
    bevents_clear();
    force = true;
}

/* -------------------------------------------------------------- drawing */
static int bb[4];
static void bb_add(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    if (bb[2] <= 0) { bb[0] = x; bb[1] = y; bb[2] = w; bb[3] = h; return; }
    int x1 = bb[0] + bb[2] > x + w ? bb[0] + bb[2] : x + w;
    int y1 = bb[1] + bb[3] > y + h ? bb[1] + bb[3] : y + h;
    if (x < bb[0]) bb[0] = x;
    if (y < bb[1]) bb[1] = y;
    bb[2] = x1 - bb[0];
    bb[3] = y1 - bb[1];
}

/* straight colour (r,g,b) with alpha a over the premultiplied buffer */
static void fill(int x, int y, int w, int h, int r, int g, int b, int a)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0 || a <= 0) return;
    int pr = r * a / 255, pg = g * a / 255, pb = b * a / 255, ia = 255 - a;
    for (int j = y; j < y + h; j++) {
        uint8_t *p = pix + ((size_t)j * W + x) * 4;
        for (int i = 0; i < w; i++, p += 4) {
            p[0] = pr + p[0] * ia / 255;
            p[1] = pg + p[1] * ia / 255;
            p[2] = pb + p[2] * ia / 255;
            p[3] = a + p[3] * ia / 255;
        }
    }
    bb_add(x, y, w, h);
}

static void blend_ass(const ASS_Image *img)
{
    for (; img; img = img->next) {
        int x0 = img->dst_x, y0 = img->dst_y, w = img->w, h = img->h;
        if (w <= 0 || h <= 0) continue;
        unsigned r = img->color >> 24, g = (img->color >> 16) & 255, b = (img->color >> 8) & 255;
        unsigned op = 255 - (img->color & 255);
        int sx = 0, sy = 0;
        if (x0 < 0) { sx = -x0; w += x0; x0 = 0; }
        if (y0 < 0) { sy = -y0; h += y0; y0 = 0; }
        if (x0 + w > W) w = W - x0;
        if (y0 + h > H) h = H - y0;
        if (w <= 0 || h <= 0) continue;
        for (int j = 0; j < h; j++) {
            const uint8_t *s = img->bitmap + (size_t)(j + sy) * img->stride + sx;
            uint8_t *p = pix + ((size_t)(y0 + j) * W + x0) * 4;
            for (int i = 0; i < w; i++, p += 4) {
                unsigned a = s[i] * op;                 /* 0..65025 */
                if (!a) continue;
                unsigned ia = 65025 - a;
                p[0] = (r * a + p[0] * ia) / 65025;
                p[1] = (g * a + p[1] * ia) / 65025;
                p[2] = (b * a + p[2] * ia) / 65025;
                p[3] = (255 * a + p[3] * ia) / 65025;
            }
        }
        bb_add(x0, y0, w, h);
    }
}

static void blit_scaled(const BRect *b, int cw, int ch)
{
    if (cw <= 0 || ch <= 0) { cw = vid_w; ch = vid_h; }
    if (cw <= 0 || ch <= 0) return;
    double fx = (double)vrect[2] / cw, fy = (double)vrect[3] / ch;
    int dx = vrect[0] + (int)(b->x * fx), dy = vrect[1] + (int)(b->y * fy);
    int dw = (int)ceil(b->w * fx), dh = (int)ceil(b->h * fy);
    for (int j = 0; j < dh; j++) {
        int Y = dy + j;
        if (Y < 0 || Y >= H) continue;
        double v = (j + 0.5) / fy - 0.5;
        int y0 = (int)floor(v);
        int wy = (int)((v - y0) * 256);
        int ya = y0 < 0 ? 0 : y0, yb = y0 + 1 >= b->h ? b->h - 1 : y0 + 1;
        if (ya >= b->h) ya = b->h - 1;
        for (int i = 0; i < dw; i++) {
            int X = dx + i;
            if (X < 0 || X >= W) continue;
            double u = (i + 0.5) / fx - 0.5;
            int x0 = (int)floor(u);
            int wx = (int)((u - x0) * 256);
            int xa = x0 < 0 ? 0 : x0, xb = x0 + 1 >= b->w ? b->w - 1 : x0 + 1;
            if (xa >= b->w) xa = b->w - 1;
            const uint8_t *p00 = b->rgba + ((size_t)ya * b->w + xa) * 4, *p01 = b->rgba + ((size_t)ya * b->w + xb) * 4;
            const uint8_t *p10 = b->rgba + ((size_t)yb * b->w + xa) * 4, *p11 = b->rgba + ((size_t)yb * b->w + xb) * 4;
            uint8_t *d = pix + ((size_t)Y * W + X) * 4;
            int c[4];
            for (int k = 0; k < 4; k++) {
                int top = p00[k] * (256 - wx) + p01[k] * wx, bot = p10[k] * (256 - wx) + p11[k] * wx;
                c[k] = (top * (256 - wy) + bot * wy) >> 16;
            }
            if (!c[3]) continue;
            int ia = 255 - c[3];
            for (int k = 0; k < 4; k++) d[k] = c[k] + d[k] * ia / 255;
        }
    }
    bb_add(dx, dy, dw, dh);
}

static void fmt_time(char *out, size_t n, double t)
{
    int s = (int)t;
    if (s < 0) s = 0;
    if (s >= 3600) snprintf(out, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

static void add_text(int an, double x, double y, double size, const char *extra, const char *text)
{
    if (!text || !*text) return;
    size_t len = strlen(text);
    char *buf = malloc(len * 2 + 160), *o = buf;
    o += sprintf(o, "{\\an%d\\pos(%.1f,%.1f)\\fs%.1f%s}", an, x, y, size, extra ? extra : "");
    for (const char *p = text; *p; p++) {
        if (*p == '{') *o++ = '(';
        else if (*p == '}') *o++ = ')';
        else if (*p == '\\') *o++ = '/';
        else if (*p == '\n') { *o++ = '\\'; *o++ = 'N'; }
        else *o++ = *p;
    }
    *o = 0;
    int id = ass_alloc_event(t_osd);
    ASS_Event *ev = &t_osd->events[id];
    ev->Start = 0;
    ev->Duration = INT_MAX / 2;
    ev->Style = t_osd->default_style;
    ev->ReadOrder = id;
    ev->Text = buf;
}

static void bar_geom(int *x, int *y, int *w, int *h)
{
    *x = (int)(18 * S);
    *w = W - 2 * *x;
    *h = (int)(4 * S);
    *y = H - (int)(44 * S);
}
void osd_bar_geometry(int *x, int *y, int *w, int *h)
{
    bar_geom(x, y, w, h);
    *y -= (int)(12 * S);
    *h += (int)(24 * S);
}

static uint64_t hash_str(uint64_t h, const char *s)
{
    if (!s) return h * 1099511628211ULL + 1;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ULL;
    return h * 1099511628211ULL + 7;
}

static void make_osd_track(void)
{
    if (t_osd) ass_free_track(t_osd);
    char hdr[1024];
    int n = snprintf(hdr, sizeof hdr,
        "[Script Info]\nScriptType: v4.00+\nPlayResX: %d\nPlayResY: %d\nScaledBorderAndShadow: yes\nWrapStyle: 2\n\n"
        "[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, "
        "MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,sans-serif,20,&H00FFFFFF,&H00FFFFFF,&H50000000,&H80000000,0,0,0,0,100,100,0,0,1,%.2f,0,7,0,0,0,1\n\n"
        "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n",
        W, H, 1.2 * S);
    t_osd = ass_read_memory(lib, hdr, n, NULL);
    ass_set_frame_size(r_osd, W, H);
    ass_set_storage_size(r_osd, W, H);
}

bool osd_update(const struct OsdState *st, double sub_time,
                const uint8_t **out, int *stride, int dirty[4], int out_bb[4])
{
    if (!ready || !pix) return false;
    for (SubMsg *m = pending, *nx; m; m = nx) { nx = m->next; process_msg(m); force = true; }
    pending = pending_last = NULL;

    if (geometry_dirty) {
        geometry_dirty = false;
        make_osd_track();
        ass_set_frame_size(r_sub, W, H);
        if (vid_w > 0 && vid_h > 0) ass_set_storage_size(r_sub, vid_w, vid_h);
        ass_set_margins(r_sub, vrect[1], H - vrect[1] - vrect[3], vrect[0], W - vrect[0] - vrect[2]);
        ass_set_use_margins(r_sub, 0);
        prev_sig = 0;
    }

    /* subtitles */
    ASS_Image *sub_img = NULL;
    int changed = 0;
    if (t_sub && t_sub->n_events > 0) sub_img = ass_render_frame(r_sub, t_sub, llrint(sub_time * 1000), &changed);
    const BEvent *bact = NULL;
    for (const BEvent *e = bevents; e; e = e->next)
        if (sub_time >= e->start && sub_time < e->end && (!bact || e->start > bact->start)) bact = e;
    if (bact != b_drawn) changed = 1;

    /* OSD signature */
    int bx, by, bw, bh;
    bar_geom(&bx, &by, &bw, &bh);
    int fillw = st->dur > 0 ? (int)(bw * fmin(1.0, fmax(0.0, st->pos / st->dur))) : 0;
    uint64_t sig = 1469598103934665603ULL;
    sig = (sig ^ (st->bar ? 2 : 1)) * 1099511628211ULL;
    if (st->bar) {
        sig = (sig ^ (uint64_t)(int)st->pos) * 1099511628211ULL;
        sig = (sig ^ (uint64_t)fillw) * 1099511628211ULL;
        sig = (sig ^ (uint64_t)(int)st->dur) * 1099511628211ULL;
        sig = (sig ^ (st->hover_bar ? 5 : 3) ^ (st->paused ? 16 : 0)) * 1099511628211ULL;
        sig = hash_str(sig, st->title);
        sig = hash_str(sig, st->right);
    }
    sig = hash_str(sig, st->toast);
    sig = hash_str(sig, st->stats);
    bool osd_changed = sig != prev_sig;
    if (!changed && !osd_changed && !force) return false;
    force = false;

    if (osd_changed) {
        prev_sig = sig;
        ass_flush_events(t_osd);
        double top = 14 * S;
        if (st->bar) {
            char a[32], b[32], line[80];
            fmt_time(a, sizeof a, st->pos);
            if (st->dur > 0) { fmt_time(b, sizeof b, st->dur); snprintf(line, sizeof line, "%s%s / %s", st->paused ? "⏸  " : "", a, b); }
            else snprintf(line, sizeof line, "%s%s", st->paused ? "⏸  " : "", a);
            add_text(1, bx, H - 12 * S, 15 * S, NULL, line);
            add_text(3, bx + bw, H - 12 * S, 15 * S, NULL, st->right);
            if (st->title) { add_text(7, bx, top, 17 * S, "\\q2", st->title); top += 28 * S; }
        }
        if (st->toast) { add_text(7, 18 * S, top, 21 * S, "\\b1", st->toast); top += 32 * S; }
        stats_top = (int)top;
        if (st->stats) add_text(7, 18 * S, top, 13 * S, "\\fnmonospace", st->stats);
    }
    int oc = 0;
    ASS_Image *osd_img = t_osd->n_events ? ass_render_frame(r_osd, t_osd, 0, &oc) : NULL;

    /* compose */
    if (prev_bb[2] > 0)
        for (int j = prev_bb[1]; j < prev_bb[1] + prev_bb[3]; j++)
            memset(pix + ((size_t)j * W + prev_bb[0]) * 4, 0, (size_t)prev_bb[2] * 4);
    memset(bb, 0, sizeof bb);
    if (bact) for (int i = 0; i < bact->n; i++) blit_scaled(&bact->r[i], bact->cw, bact->ch);
    b_drawn = bact;
    blend_ass(sub_img);
    if (st->bar) {
        int band = (int)(84 * S);
        for (int j = 0; j < band; j++)                    /* soft gradient behind the controls */
            fill(0, H - band + j, W, 1, 0, 0, 0, 190 * (j + 1) / band);
        if (st->title) {
            int tb = (int)(56 * S);
            for (int j = 0; j < tb; j++) fill(0, j, W, 1, 0, 0, 0, 150 * (tb - j) / tb);
        }
        int th = st->hover_bar ? bh + (int)(3 * S) : bh, ty = by - (th - bh) / 2;
        fill(bx, ty, bw, th, 255, 255, 255, 70);
        fill(bx, ty, fillw, th, 242, 140, 40, 255);
        if (st->hover_bar) {
            int k = (int)(13 * S);
            fill(bx + fillw - k / 2, by + bh / 2 - k / 2, k, k, 255, 255, 255, 255);
        }
    }
    if (st->stats && osd_img) {
        int x0 = W, y0 = H, x1 = 0, y1 = 0;
        for (const ASS_Image *i = osd_img; i; i = i->next) {
            if (i->dst_y < stats_top - (int)(3 * S) || i->dst_y + i->h > H - (int)(50 * S)) continue;
            if (i->dst_x < x0) x0 = i->dst_x;
            if (i->dst_y < y0) y0 = i->dst_y;
            if (i->dst_x + i->w > x1) x1 = i->dst_x + i->w;
            if (i->dst_y + i->h > y1) y1 = i->dst_y + i->h;
        }
        int pad = (int)(8 * S);
        if (x1 > x0) fill(x0 - pad, y0 - pad, x1 - x0 + 2 * pad, y1 - y0 + 2 * pad, 0, 0, 0, 150);
    }
    blend_ass(osd_img);

    /* dirty = union(previous bbox, new bbox) */
    int nb[4];
    memcpy(nb, bb, sizeof nb);
    if (prev_bb[2] > 0) bb_add(prev_bb[0], prev_bb[1], prev_bb[2], prev_bb[3]);
    memcpy(dirty, bb, sizeof bb);
    memcpy(prev_bb, nb, sizeof nb);
    memcpy(out_bb, nb, sizeof nb);
    *out = pix;
    *stride = W * 4;
    return true;
}
