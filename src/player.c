/* Turrón — demux, decode threads, queues and the master clock. */
#include "turron.h"
#include <dirent.h>
#include <math.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>

struct Player P = { .vi = -1, .ai = -1 };
int g_wake_fd = -1;

void player_wake_main(void)
{
    uint64_t one = 1;
    if (g_wake_fd >= 0 && write(g_wake_fd, &one, 8) < 0) { /* full counter: already awake */ }
}

/* ------------------------------------------------------------------ clock */
void clock_set(double pts)
{
    pthread_mutex_lock(&P.clk.mu);
    P.clk.pts = pts;
    P.clk.at = now_s();
    pthread_mutex_unlock(&P.clk.mu);
}
void clock_run(bool run)
{
    pthread_mutex_lock(&P.clk.mu);
    if (run != P.clk.running) {
        double t = now_s();
        if (!run) P.clk.pts += t - P.clk.at;
        P.clk.at = t;
        P.clk.running = run;
    }
    pthread_mutex_unlock(&P.clk.mu);
}
double player_clock(void)
{
    pthread_mutex_lock(&P.clk.mu);
    double v = P.clk.running ? P.clk.pts + now_s() - P.clk.at : P.clk.pts;
    pthread_mutex_unlock(&P.clk.mu);
    return v;
}
double player_pos(void)
{
    double p = player_clock() - P.start_time;
    if (p < 0) p = 0;
    if (P.duration > 0 && p > P.duration) p = P.duration;
    return p;
}

/* ---------------------------------------------------------- packet queues */
typedef struct PNode { AVPacket *pkt; int serial; struct PNode *next; } PNode;
typedef struct PktQ {
    PNode *first, *last;
    int n;
    int64_t bytes;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} PktQ;
static PktQ vq = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };
static PktQ aq = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static void pktq_put(PktQ *q, AVPacket *src, int serial)   /* src NULL = drain marker */
{
    PNode *n = calloc(1, sizeof *n);
    if (src) {
        n->pkt = av_packet_alloc();
        av_packet_move_ref(n->pkt, src);
    }
    n->serial = serial;
    pthread_mutex_lock(&q->mu);
    if (q->last) q->last->next = n; else q->first = n;
    q->last = n;
    q->n++;
    if (n->pkt) q->bytes += n->pkt->size;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
}
static int pktq_get(PktQ *q, AVPacket **pkt, int *serial)
{
    pthread_mutex_lock(&q->mu);
    while (!q->first && !P.abort) pthread_cond_wait(&q->cv, &q->mu);
    if (P.abort) { pthread_mutex_unlock(&q->mu); return -1; }
    PNode *n = q->first;
    q->first = n->next;
    if (!q->first) q->last = NULL;
    q->n--;
    if (n->pkt) q->bytes -= n->pkt->size;
    pthread_mutex_unlock(&q->mu);
    *pkt = n->pkt;
    *serial = n->serial;
    free(n);
    return 0;
}
static void pktq_flush(PktQ *q)
{
    pthread_mutex_lock(&q->mu);
    for (PNode *n = q->first, *nx; n; n = nx) {
        nx = n->next;
        av_packet_free(&n->pkt);
        free(n);
    }
    q->first = q->last = NULL;
    q->n = 0;
    q->bytes = 0;
    pthread_mutex_unlock(&q->mu);
}
static void pktq_wake(PktQ *q)
{
    pthread_mutex_lock(&q->mu);
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

/* ------------------------------------------------------------ frame queue */
#define FQ_N 6
static struct {
    VFrame v[FQ_N];
    int r, n;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} fq = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static void fq_push(AVFrame *src, double pts, double dur, int serial)
{
    pthread_mutex_lock(&fq.mu);
    while (fq.n == FQ_N && !P.abort && serial == P.serial) pthread_cond_wait(&fq.cv, &fq.mu);
    if (P.abort || serial != P.serial) {
        pthread_mutex_unlock(&fq.mu);
        av_frame_unref(src);
        return;
    }
    VFrame *v = &fq.v[(fq.r + fq.n) % FQ_N];
    v->f = av_frame_alloc();
    av_frame_move_ref(v->f, src);
    v->pts = pts;
    v->dur = dur;
    v->serial = serial;
    fq.n++;
    pthread_mutex_unlock(&fq.mu);
    player_wake_main();
}
bool fq_peek(VFrame *out, double *next_pts)
{
    pthread_mutex_lock(&fq.mu);
    while (fq.n && fq.v[fq.r].serial != P.serial) {
        av_frame_free(&fq.v[fq.r].f);
        fq.r = (fq.r + 1) % FQ_N;
        fq.n--;
        pthread_cond_signal(&fq.cv);
    }
    bool ok = fq.n > 0;
    if (ok) {
        *out = fq.v[fq.r];
        *next_pts = fq.n > 1 ? fq.v[(fq.r + 1) % FQ_N].pts : NAN;
    }
    pthread_mutex_unlock(&fq.mu);
    return ok;
}
void fq_pop(void)   /* the caller already owns (or freed) the head frame */
{
    pthread_mutex_lock(&fq.mu);
    if (fq.n) {
        fq.v[fq.r].f = NULL;
        fq.r = (fq.r + 1) % FQ_N;
        fq.n--;
        pthread_cond_signal(&fq.cv);
    }
    pthread_mutex_unlock(&fq.mu);
}
static void fq_flush(void)
{
    pthread_mutex_lock(&fq.mu);
    while (fq.n) {
        av_frame_free(&fq.v[fq.r].f);
        fq.r = (fq.r + 1) % FQ_N;
        fq.n--;
    }
    pthread_cond_broadcast(&fq.cv);
    pthread_mutex_unlock(&fq.mu);
}
static bool fq_empty(void)
{
    pthread_mutex_lock(&fq.mu);
    bool e = fq.n == 0;
    pthread_mutex_unlock(&fq.mu);
    return e;
}

/* ------------------------------------------------------ subtitle messages */
static struct { SubMsg *first, *last; pthread_mutex_t mu; } sq = { .mu = PTHREAD_MUTEX_INITIALIZER };

static void sq_put(SubMsg *m)
{
    pthread_mutex_lock(&sq.mu);
    if (sq.last) sq.last->next = m; else sq.first = m;
    sq.last = m;
    pthread_mutex_unlock(&sq.mu);
    player_wake_main();
}
SubMsg *player_take_subs(void)
{
    pthread_mutex_lock(&sq.mu);
    SubMsg *m = sq.first;
    sq.first = sq.last = NULL;
    pthread_mutex_unlock(&sq.mu);
    return m;
}
void submsg_free(SubMsg *m)
{
    if (!m) return;
    if (m->type == SUB_BITMAP) avsubtitle_free(&m->sub);
    free(m->text);
    free(m);
}

/* ------------------------------------------------------------- threads */
static pthread_t th_demux, th_video, th_audio;
static bool has_demux, has_video, has_audio;
static pthread_mutex_t seek_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t demux_cv = PTHREAD_COND_INITIALIZER;
static bool seek_req, seek_exact;
static double seek_target, seek_origin;
static AVCodecContext *sctx;
static int cur_strack = -1;
static int out_rate;
static AVBufferRef *hwdev;
static bool hwdev_tried;

static void demux_wait_ms(int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += ms * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&seek_mu);
    if (!seek_req && !P.abort) pthread_cond_timedwait(&demux_cv, &seek_mu, &ts);
    pthread_mutex_unlock(&seek_mu);
}

static void sub_switch(void)
{
    int want = P.want_strack;
    if (want == cur_strack) return;
    avcodec_free_context(&sctx);
    cur_strack = want;
    SubMsg *m = calloc(1, sizeof *m);
    m->type = SUB_RESET;
    m->serial = want;                       /* track index for the main thread */
    if (want >= 0 && P.stracks[want].stream >= 0) {
        AVStream *st = P.ic->streams[P.stracks[want].stream];
        const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
        if (c && (sctx = avcodec_alloc_context3(c))) {
            avcodec_parameters_to_context(sctx, st->codecpar);
            sctx->pkt_timebase = st->time_base;
            if (avcodec_open2(sctx, c, NULL) < 0) avcodec_free_context(&sctx);
        }
        if (sctx) {
            if (sctx->subtitle_header && sctx->subtitle_header_size > 0) {
                m->text_len = sctx->subtitle_header_size;
                m->text = malloc(m->text_len + 1);
                memcpy(m->text, sctx->subtitle_header, m->text_len);
                m->text[m->text_len] = 0;
            }
            m->w = sctx->width;
            m->h = sctx->height;
        }
    }
    sq_put(m);
}

static void sub_decode(AVPacket *pkt)
{
    AVSubtitle sub;
    int got = 0;
    if (avcodec_decode_subtitle2(sctx, &sub, &got, pkt) < 0 || !got) return;
    double tb = av_q2d(sctx->pkt_timebase);
    double pts = sub.pts != AV_NOPTS_VALUE ? sub.pts / (double)AV_TIME_BASE
               : pkt->pts != AV_NOPTS_VALUE ? pkt->pts * tb : player_clock();
    pts += sub.start_display_time / 1000.0;
    double dur = -1;
    if (sub.end_display_time && sub.end_display_time != UINT32_MAX)
        dur = (sub.end_display_time - sub.start_display_time) / 1000.0;
    else if (pkt->duration > 0)
        dur = pkt->duration * tb;
    if (sub.format == 0) {
        SubMsg *m = calloc(1, sizeof *m);
        m->type = SUB_BITMAP;
        m->pts = pts;
        m->dur = dur;
        m->sub = sub;
        m->w = sctx->width;
        m->h = sctx->height;
        sq_put(m);
        return;
    }
    for (unsigned i = 0; i < sub.num_rects; i++) {
        if (sub.rects[i]->type != SUBTITLE_ASS || !sub.rects[i]->ass) continue;
        SubMsg *m = calloc(1, sizeof *m);
        m->type = SUB_TEXT;
        m->pts = pts;
        m->dur = dur < 0 ? 5 : dur;
        m->text = strdup(sub.rects[i]->ass);
        m->text_len = strlen(m->text);
        sq_put(m);
    }
    avsubtitle_free(&sub);
}

static void *demux_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "t-demux");
    AVPacket *pkt = av_packet_alloc();
    bool eof = false;
    while (!P.abort) {
        pthread_mutex_lock(&seek_mu);
        bool do_seek = seek_req, exact = seek_exact;
        double target = seek_target;
        seek_req = false;
        pthread_mutex_unlock(&seek_mu);
        if (do_seek) {
            int64_t ts = llrint(target * AV_TIME_BASE);
            int64_t cur = llrint(seek_origin * AV_TIME_BASE);
            int64_t mn = INT64_MIN, mx = INT64_MAX;
            if (exact) mx = ts;
            else if (ts > cur) mn = cur + 2;
            else mx = cur - 2;
            int ret = avformat_seek_file(P.ic, -1, mn, ts, mx, 0);
            if (ret < 0) ret = avformat_seek_file(P.ic, -1, INT64_MIN, ts, INT64_MAX, 0);
            if (ret < 0) ret = av_seek_frame(P.ic, -1, ts, AVSEEK_FLAG_BACKWARD);
            if (ret >= 0) {
                pktq_flush(&vq);
                pktq_flush(&aq);
                P.serial++;                 /* first: stale audio can no longer anchor the clock */
                atomic_store(&P.exact_target, exact ? target : -1.0);
                clock_run(false);
                clock_set(target);
                P.await = true;
                P.force_show = true;
                P.await_video_since = now_s();
                P.await_video = P.vi >= 0;
                fq_flush();
                audio_flush();
                if (sctx) avcodec_flush_buffers(sctx);
                eof = false;
                P.demux_eof = false;
                player_wake_main();
            }
        }
        sub_switch();
        if (eof) { demux_wait_ms(50); continue; }
        if (vq.bytes + aq.bytes > (64 << 20) ||
            ((P.vi < 0 || vq.n > 48) && (P.ai < 0 || aq.n > 48))) {
            demux_wait_ms(10);
            continue;
        }
        int ret = av_read_frame(P.ic, pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) { demux_wait_ms(10); continue; }
            if (P.vi >= 0) pktq_put(&vq, NULL, P.serial);
            if (P.ai >= 0) pktq_put(&aq, NULL, P.serial);
            eof = true;
            P.demux_eof = true;
            player_wake_main();
            continue;
        }
        if (pkt->stream_index == P.vi) pktq_put(&vq, pkt, P.serial);
        else if (pkt->stream_index == P.ai) pktq_put(&aq, pkt, P.serial);
        else if (sctx && cur_strack >= 0 && pkt->stream_index == P.stracks[cur_strack].stream)
            sub_decode(pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avcodec_free_context(&sctx);
    cur_strack = -1;
    return NULL;
}

static AVCodecContext *open_decoder(AVStream *st, bool video);

static void *video_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "t-video");
    AVFrame *fr = av_frame_alloc();
    AVCodecContext *c = P.vctx;
    double tb = av_q2d(P.vst->time_base);
    double fdur = P.fps > 0 ? 1.0 / P.fps : 0.04;
    double last = NAN, serial_t0 = now_s();
    int dec_serial = -1, late_run = 0, jumps = 0, slow = 0;
    bool catchup = false, reopen = false;
    AVPacket *pkt;
    int serial;
    while (pktq_get(&vq, &pkt, &serial) == 0) {
        if (serial != P.serial) { av_packet_free(&pkt); continue; }
        if (serial != dec_serial) {
            avcodec_flush_buffers(c);
            dec_serial = serial;
            last = NAN;
            late_run = 0;
            slow = 0;
            catchup = false;
            serial_t0 = now_s();
        }
        if (catchup && pkt) {
            /* Too far behind to ever catch up by decoding: skip straight to the
             * next keyframe that is still ahead of the clock. */
            double ppts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts * tb : pkt->dts != AV_NOPTS_VALUE ? pkt->dts * tb : NAN;
            if (!(pkt->flags & AV_PKT_FLAG_KEY) || isnan(ppts) || ppts < player_clock() + 0.25) {
                P.dropped++;
                av_packet_free(&pkt);
                continue;
            }
            catchup = false;
            late_run = 0;
            if (!reopen) avcodec_flush_buffers(c);
        }
        if (reopen && pkt && (pkt->flags & AV_PKT_FLAG_KEY)) {
            reopen = false;
            avcodec_free_context(&c);
            P.vctx = c = open_decoder(P.vst, true);
            if (!c) { av_packet_free(&pkt); break; }
        }
        if (!P.hw) c->skip_frame = (late_run >= 3 || P.degraded) ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
        int ret = avcodec_send_packet(c, pkt);
        av_packet_free(&pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) continue;
        while ((ret = avcodec_receive_frame(c, fr)) >= 0) {
            double dur = fr->duration > 0 ? fr->duration * tb : fdur;
            double pts = fr->best_effort_timestamp != AV_NOPTS_VALUE
                       ? fr->best_effort_timestamp * tb
                       : isnan(last) ? player_clock() : last + dur;
            last = pts;
            P.decoded++;
            double ex = atomic_load(&P.exact_target);
            if (ex >= 0 && pts + dur <= ex) {
                av_frame_unref(fr);
                if (now_s() - serial_t0 > 0.7) {
                    /* decoding up to the exact spot takes too long here: settle for keyframes */
                    P.no_exact = true;
                    pthread_mutex_lock(&seek_mu);
                    if (!seek_req) {
                        seek_target = ex;
                        seek_exact = false;
                        seek_req = true;
                        pthread_cond_broadcast(&demux_cv);
                    }
                    pthread_mutex_unlock(&seek_mu);
                }
                continue;
            }
            double late = (!P.await && !P.paused) ? player_clock() - pts : 0;
            if (late > 0.15) slow++; else if (late < 0.05 && slow > 0) slow--;
            if (slow > 45 && P.degraded == 0) {
                /* steadily behind: from now on decode reference frames only */
                P.degraded = 1;
                reopen = !strcmp(c->codec->name, "libdav1d");   /* it reads skip_frame only when opened */
                slow = 0;
            }
            if (late > 1.0 && !catchup) {
                catchup = true;
                P.jumps++;
                if (P.degraded == 0) {
                    P.degraded = 1;
                    reopen = !strcmp(c->codec->name, "libdav1d");
                } else if (++jumps >= 3 && P.degraded == 1 && !P.hw) {
                    P.degraded = 2;
                    c->skip_loop_filter = AVDISCARD_ALL;
                }
            }
            if (late > 0.08 && late_run < 6) {
                late_run++;
                P.dropped++;
                av_frame_unref(fr);
                continue;
            }
            if (late < 0.02) late_run = 0;
            fq_push(fr, pts, dur, dec_serial);
        }
        if (ret == AVERROR_EOF) {
            P.v_eof_serial = dec_serial;
            player_wake_main();
        }
    }
    av_frame_free(&fr);
    return NULL;
}

static void *audio_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "t-audio");
    AVFrame *fr = av_frame_alloc();
    AVCodecContext *c = P.actx;
    double tb = av_q2d(P.ast->time_base);
    SwrContext *swr = NULL;
    int in_fmt = -1, in_rate = 0;
    AVChannelLayout in_lay = { 0 };
    float *buf = NULL;
    int cap = 0, dec_serial = -1;
    AVPacket *pkt;
    int serial;
    while (pktq_get(&aq, &pkt, &serial) == 0) {
        if (serial != P.serial) { av_packet_free(&pkt); continue; }
        if (serial != dec_serial) {
            avcodec_flush_buffers(c);
            dec_serial = serial;
        }
        int ret = avcodec_send_packet(c, pkt);
        av_packet_free(&pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) continue;
        while ((ret = avcodec_receive_frame(c, fr)) >= 0) {
            if (fr->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
                av_channel_layout_default(&fr->ch_layout, fr->ch_layout.nb_channels);
            if (!swr || fr->format != in_fmt || fr->sample_rate != in_rate ||
                av_channel_layout_compare(&fr->ch_layout, &in_lay)) {
                swr_free(&swr);
                AVChannelLayout st = AV_CHANNEL_LAYOUT_STEREO;
                if (swr_alloc_set_opts2(&swr, &st, AV_SAMPLE_FMT_FLT, out_rate, &fr->ch_layout,
                                        fr->format, fr->sample_rate, 0, NULL) < 0 || swr_init(swr) < 0) {
                    swr_free(&swr);
                    av_frame_unref(fr);
                    continue;
                }
                in_fmt = fr->format;
                in_rate = fr->sample_rate;
                av_channel_layout_uninit(&in_lay);
                av_channel_layout_copy(&in_lay, &fr->ch_layout);
            }
            int want = swr_get_out_samples(swr, fr->nb_samples);
            if (want > cap) {
                cap = want + 1024;
                buf = realloc(buf, (size_t)cap * 2 * sizeof(float));
            }
            uint8_t *out = (uint8_t *)buf;
            int n = swr_convert(swr, &out, cap, (const uint8_t **)fr->extended_data, fr->nb_samples);
            double pts = fr->pts != AV_NOPTS_VALUE ? fr->pts * tb : NAN;
            av_frame_unref(fr);
            if (n <= 0) continue;
            float *p = buf;
            double ex = atomic_load(&P.exact_target);
            if (ex >= 0 && !isnan(pts) && pts < ex) {
                int skip = (int)((ex - pts) * out_rate);
                if (skip >= n) continue;
                p += skip * 2;
                n -= skip;
                pts = ex;
            }
            if (!audio_write(p, n, pts, dec_serial)) break;
        }
        if (ret == AVERROR_EOF) {
            P.a_eof_serial = dec_serial;
            player_wake_main();
        }
    }
    swr_free(&swr);
    av_channel_layout_uninit(&in_lay);
    free(buf);
    av_frame_free(&fr);
    return NULL;
}

/* --------------------------------------------------------------- opening */
static enum AVPixelFormat get_format(AVCodecContext *c, const enum AVPixelFormat *fmts)
{
    if (c->hw_device_ctx)
        for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
            if (*p == AV_PIX_FMT_VAAPI) { P.hw = true; return *p; }
    P.hw = false;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return fmts[0];
}

static int interrupt_cb(void *opaque)
{
    (void)opaque;
    return P.abort;
}

static const char *lang_name(const char *code)
{
    static const struct { const char *c, *es, *en; } L[] = {
        { "spa", "Español", "Spanish" }, { "es", "Español", "Spanish" }, { "esp", "Español", "Spanish" },
        { "eng", "Inglés", "English" }, { "en", "Inglés", "English" },
        { "jpn", "Japonés", "Japanese" }, { "ja", "Japonés", "Japanese" },
        { "fre", "Francés", "French" }, { "fra", "Francés", "French" }, { "fr", "Francés", "French" },
        { "ger", "Alemán", "German" }, { "deu", "Alemán", "German" }, { "de", "Alemán", "German" },
        { "ita", "Italiano", "Italian" }, { "it", "Italiano", "Italian" },
        { "por", "Portugués", "Portuguese" }, { "pt", "Portugués", "Portuguese" },
        { "cat", "Catalán", "Catalan" }, { "ca", "Catalán", "Catalan" },
        { "glg", "Gallego", "Galician" }, { "baq", "Euskera", "Basque" }, { "eus", "Euskera", "Basque" },
        { "kor", "Coreano", "Korean" }, { "chi", "Chino", "Chinese" }, { "zho", "Chino", "Chinese" },
        { "rus", "Ruso", "Russian" }, { "lat", "Latín", "Latin" },
    };
    for (size_t i = 0; i < sizeof L / sizeof *L; i++)
        if (!strcasecmp(code, L[i].c)) return g_es ? L[i].es : L[i].en;
    return code;
}

static bool lang_is_user(const char *l)
{
    if (!*l) return false;
    if (g_es) return !strncasecmp(l, "spa", 3) || !strcasecmp(l, "es") || !strcasecmp(l, "esp");
    return !strncasecmp(l, "eng", 3) || !strcasecmp(l, "en");
}

static void fill_track(Track *t, AVStream *st, int n)
{
    AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
    AVDictionaryEntry *title = av_dict_get(st->metadata, "title", NULL, 0);
    t->stream = st->index;
    t->ext_path = NULL;
    snprintf(t->lang, sizeof t->lang, "%s", lang && strcmp(lang->value, "und") ? lang->value : "");
    char base[96];
    if (*t->lang) snprintf(base, sizeof base, "%s", lang_name(t->lang));
    else snprintf(base, sizeof base, "%s %d", T("Pista", "Track"), n + 1);
    const char *codec = avcodec_get_name(st->codecpar->codec_id);
    if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        int ch = st->codecpar->ch_layout.nb_channels;
        char chs[16];
        if (ch == 6) strcpy(chs, "5.1"); else if (ch == 8) strcpy(chs, "7.1");
        else if (ch == 2) strcpy(chs, T("estéreo", "stereo")); else if (ch == 1) strcpy(chs, "mono");
        else snprintf(chs, sizeof chs, "%d ch", ch);
        if (title) snprintf(t->label, sizeof t->label, "%.40s — %.60s (%.16s %s)", base, title->value, codec, chs);
        else snprintf(t->label, sizeof t->label, "%.60s (%.20s %s)", base, codec, chs);
    } else {
        if (title) snprintf(t->label, sizeof t->label, "%.60s — %.80s", base, title->value);
        else snprintf(t->label, sizeof t->label, "%s", base);
    }
}

static void find_external_subs(const char *path)
{
    if (strstr(path, "://")) return;
    char *dir = strdup(path), *slash = strrchr(dir, '/');
    const char *base = path;
    if (slash) { *slash = 0; base = path + (slash - dir) + 1; }
    const char *dot = strrchr(base, '.');
    size_t stem = dot ? (size_t)(dot - base) : strlen(base);
    DIR *d = opendir(slash ? (*dir ? dir : "/") : ".");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && P.n_stracks < MAX_TRACKS) {
            const char *ext = strrchr(e->d_name, '.');
            if (!ext || strncmp(e->d_name, base, stem) || e->d_name[stem] != '.') continue;
            if (strcasecmp(ext, ".srt") && strcasecmp(ext, ".ass") && strcasecmp(ext, ".ssa") &&
                strcasecmp(ext, ".vtt") && strcasecmp(ext, ".sub")) continue;
            Track *t = &P.stracks[P.n_stracks++];
            memset(t, 0, sizeof *t);
            t->stream = -1;
            if (asprintf(&t->ext_path, "%s/%s", slash ? dir : ".", e->d_name) < 0) t->ext_path = NULL;
            snprintf(t->label, sizeof t->label, "%.150s", e->d_name + stem + 1);
        }
        closedir(d);
    }
    free(dir);
}

static void init_hwdev(void)
{
    if (hwdev_tried) return;
    hwdev_tried = true;
    if (O.no_hw) return;
    static const char *nodes[] = { "/dev/dri/renderD128", "/dev/dri/renderD129", "/dev/dri/renderD130" };
    for (int i = 0; i < 3 && !hwdev; i++)
        if (access(nodes[i], R_OK | W_OK) == 0)
            av_hwdevice_ctx_create(&hwdev, AV_HWDEVICE_TYPE_VAAPI, nodes[i], NULL, 0);
}

static AVCodecContext *open_decoder(AVStream *st, bool video)
{
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return NULL;
    AVCodecContext *c = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(c, st->codecpar);
    c->pkt_timebase = st->time_base;
    if (video) {
        c->thread_count = 0;
        c->get_format = get_format;
        c->extra_hw_frames = FQ_N + 6;
        if (hwdev && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC))
            c->hw_device_ctx = av_buffer_ref(hwdev);
        if (O.fast) {
            c->flags2 |= AV_CODEC_FLAG2_FAST;
            c->skip_loop_filter = AVDISCARD_NONREF;
        }
        if (P.degraded) c->skip_frame = AVDISCARD_NONREF;
        if (P.degraded >= 2) c->skip_loop_filter = AVDISCARD_ALL;
    }
    if (avcodec_open2(c, codec, NULL) < 0) avcodec_free_context(&c);
    return c;
}

int player_open(const char *path, double start, int audio_track)
{
    static bool once;
    if (!once) {
        once = true;
        pthread_mutex_init(&P.clk.mu, NULL);
        P.serial = 1;
        avformat_network_init();
    }
    P.vi = P.ai = -1;
    P.vst = P.ast = NULL;
    P.n_atracks = P.n_stracks = 0;
    P.cur_atrack = -1;
    P.hw = false;
    P.rotation = 0;
    P.demux_eof = false;
    P.dropped = P.shown = 0;
    P.v_eof_serial = P.a_eof_serial = 0;

    AVFormatContext *ic = avformat_alloc_context();
    ic->interrupt_callback.callback = interrupt_cb;
    int ret = avformat_open_input(&ic, path, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "turron: %s: %s\n", path, av_err2str(ret));
        return -1;
    }
    avformat_find_stream_info(ic, NULL);
    P.ic = ic;
    free(P.path);
    P.path = strdup(path);
    P.start_time = ic->start_time != AV_NOPTS_VALUE ? ic->start_time / (double)AV_TIME_BASE : 0;
    P.duration = ic->duration != AV_NOPTS_VALUE ? ic->duration / (double)AV_TIME_BASE : 0;

    for (unsigned i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        st->discard = AVDISCARD_ALL;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && P.n_atracks < MAX_TRACKS) {
            fill_track(&P.atracks[P.n_atracks], st, P.n_atracks);
            P.n_atracks++;
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE && P.n_stracks < MAX_TRACKS) {
            fill_track(&P.stracks[P.n_stracks], st, P.n_stracks);
            P.n_stracks++;
        }
    }
    int n_embedded = P.n_stracks;
    find_external_subs(path);

    /* video */
    init_hwdev();
    int vi = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    P.degraded = O.fast ? 1 : 0;
    P.no_exact = false;
    P.vctx = NULL;
    if (vi >= 0) P.vctx = open_decoder(ic->streams[vi], true);
    if (vi >= 0 && P.vctx) {
        P.vid_w = P.vctx->width;
        P.vid_h = P.vctx->height;
        P.vid_sar = ic->streams[vi]->sample_aspect_ratio;
        snprintf(P.vcodec, sizeof P.vcodec, "%s", avcodec_get_name(P.vctx->codec_id));
        P.vi = vi;
        P.vst = ic->streams[vi];
        P.vst->discard = AVDISCARD_DEFAULT;
        P.fps = av_q2d(av_guess_frame_rate(ic, P.vst, NULL));
        const AVPacketSideData *sd = av_packet_side_data_get(P.vst->codecpar->coded_side_data,
            P.vst->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
        if (sd && sd->size >= 9 * 4) {
            double th = -av_display_rotation_get((const int32_t *)sd->data);
            if (!isnan(th)) P.rotation = (((int)lrint(th / 90.0) % 4) + 4) % 4 * 90;
        }
    }

    /* audio: explicit choice, else the user's language, else FFmpeg's best guess */
    int at = -1;
    if (audio_track >= 0 && audio_track < P.n_atracks) at = audio_track;
    for (int pass = 0; pass < 2 && at < 0; pass++)
        for (int i = 0; i < P.n_atracks && at < 0; i++) {
            AVStream *st = ic->streams[P.atracks[i].stream];
            if (lang_is_user(P.atracks[i].lang) && (pass || (st->disposition & AV_DISPOSITION_DEFAULT))) at = i;
        }
    if (at < 0) {
        int ai = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, P.vi, NULL, 0);
        for (int i = 0; i < P.n_atracks; i++) if (P.atracks[i].stream == ai) at = i;
    }
    if (at >= 0) {
        AVStream *st = ic->streams[P.atracks[at].stream];
        if ((P.actx = open_decoder(st, false))) {
            out_rate = P.actx->sample_rate > 0 ? P.actx->sample_rate : 48000;
            snprintf(P.acodec, sizeof P.acodec, "%s", avcodec_get_name(P.actx->codec_id));
            P.a_rate = P.actx->sample_rate;
            P.a_ch = P.actx->ch_layout.nb_channels;
            if (audio_open(out_rate) == 0) {
                P.ai = st->index;
                P.ast = st;
                st->discard = AVDISCARD_DEFAULT;
                P.cur_atrack = at;
            } else avcodec_free_context(&P.actx);
        }
    }
    if (P.vi < 0 && P.ai < 0) {
        fprintf(stderr, "turron: %s: %s\n", path, T("no hay nada que reproducir", "nothing to play"));
        avformat_close_input(&P.ic);
        return -1;
    }

    /* subtitles: sidecar file first; else forced/own-language track when the audio is foreign */
    int stk = -1;
    if (P.n_stracks > n_embedded) stk = n_embedded;
    bool audio_user = at >= 0 && lang_is_user(P.atracks[at].lang);
    if (stk < 0 && !audio_user) {
        for (int pass = 0; pass < 2 && stk < 0; pass++)
            for (int i = 0; i < n_embedded && stk < 0; i++) {
                AVStream *st = ic->streams[P.stracks[i].stream];
                bool forced = st->disposition & AV_DISPOSITION_FORCED;
                if (pass == 0 ? (lang_is_user(P.stracks[i].lang) && !forced)
                              : (st->disposition & AV_DISPOSITION_DEFAULT) != 0) stk = i;
            }
    }
    if (stk < 0 && audio_user)
        for (int i = 0; i < n_embedded && stk < 0; i++) {
            AVStream *st = ic->streams[P.stracks[i].stream];
            if ((st->disposition & AV_DISPOSITION_FORCED) && lang_is_user(P.stracks[i].lang)) stk = i;
        }
    for (int i = 0; i < n_embedded; i++) ic->streams[P.stracks[i].stream]->discard = AVDISCARD_DEFAULT;
    P.want_strack = O.no_subs ? -1 : stk;
    cur_strack = -2;                        /* force a SUB_RESET message */

    P.abort = false;
    P.paused = false;
    P.await = true;
    P.force_show = true;
    P.await_video_since = now_s();
    P.await_video = P.vi >= 0;
    atomic_store(&P.exact_target, -1.0);
    clock_run(false);
    clock_set(P.start_time);
    seek_req = false;
    if (start > 0.5 && (P.duration <= 0 || start < P.duration - 1)) {
        seek_target = P.start_time + start;
        seek_origin = seek_target;      /* never land after the requested spot */
        seek_exact = false;     /* start on the keyframe before: instant, whatever the codec */
        seek_req = true;
    }
    P.serial++;
    has_demux = pthread_create(&th_demux, NULL, demux_thread, NULL) == 0;
    if (P.vi >= 0) has_video = pthread_create(&th_video, NULL, video_thread, NULL) == 0;
    if (P.ai >= 0) has_audio = pthread_create(&th_audio, NULL, audio_thread, NULL) == 0;
    audio_set_active(true);
    P.opened = true;
    return 0;
}

void player_close(void)
{
    if (!P.opened) return;
    P.abort = true;
    pthread_mutex_lock(&seek_mu);
    pthread_cond_broadcast(&demux_cv);
    pthread_mutex_unlock(&seek_mu);
    pktq_wake(&vq);
    pktq_wake(&aq);
    fq_flush();
    if (has_demux) pthread_join(th_demux, NULL);
    if (has_video) pthread_join(th_video, NULL);
    if (has_audio) pthread_join(th_audio, NULL);
    has_demux = has_video = has_audio = false;
    pktq_flush(&vq);
    pktq_flush(&aq);
    fq_flush();
    for (SubMsg *m = player_take_subs(), *nx; m; m = nx) { nx = m->next; submsg_free(m); }
    audio_close();
    avcodec_free_context(&P.vctx);
    avcodec_free_context(&P.actx);
    avformat_close_input(&P.ic);
    for (int i = 0; i < P.n_stracks; i++) { free(P.stracks[i].ext_path); P.stracks[i].ext_path = NULL; }
    P.n_stracks = P.n_atracks = 0;
    P.vi = P.ai = -1;
    P.abort = false;
    P.opened = false;
}

void player_seek(double pos, bool exact)
{
    if (!P.opened) return;
    if (pos < 0) pos = 0;
    if (P.duration > 0 && pos > P.duration - 0.5) pos = P.duration > 0.5 ? P.duration - 0.5 : 0;
    pthread_mutex_lock(&seek_mu);
    if (!seek_req) seek_origin = player_clock();
    seek_target = P.start_time + pos;
    seek_exact = exact && !P.no_exact;
    seek_req = true;
    pthread_cond_broadcast(&demux_cv);
    pthread_mutex_unlock(&seek_mu);
}

void player_set_paused(bool p)
{
    P.paused = p;
    audio_set_active(!p);
    if (p) clock_run(false);
    else if (!P.await) clock_run(true);
}

void player_debug(int *v, int *a, int *f) { *v = vq.n; *a = aq.n; *f = fq.n; }

bool player_audio_alive(void)
{
    return P.ai >= 0 && !(P.a_eof_serial == P.serial && audio_drained());
}

bool player_at_end(void)
{
    if (!P.demux_eof) return false;
    if (P.vi >= 0 && !(P.v_eof_serial == P.serial && fq_empty())) return false;
    if (P.ai >= 0 && !(P.a_eof_serial == P.serial && audio_drained())) return false;
    return true;
}
