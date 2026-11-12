/* Turrón — PipeWire output. Stereo float; the audio callback anchors the master clock. */
#include "turron.h"
#include <math.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

atomic_int g_volume = 100;
atomic_bool g_mute;

static struct pw_thread_loop *loop;
static struct pw_stream *stream;
static struct spa_hook hook;
static int rate;

/* ring of interleaved stereo frames */
static struct {
    float *buf;
    int cap, r, fill;          /* frames */
    double end_pts;            /* pts just after the last written frame */
    int serial;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} ring = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static void on_process(void *data)
{
    (void)data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(stream);
    if (!b) return;
    struct spa_data *d = &b->buffer->datas[0];
    float *dst = d->data;
    if (!dst) { pw_stream_queue_buffer(stream, b); return; }
    int want = d->maxsize / (2 * sizeof(float));
    if (b->requested && (int)b->requested < want) want = b->requested;
    int got = 0;
    double head = NAN;
    bool anchor = false;

    if (!P.paused && !P.await_video && pthread_mutex_trylock(&ring.mu) == 0) {
        got = ring.fill < want ? ring.fill : want;
        if (got > 0) {
            head = ring.end_pts - (double)ring.fill / rate;
            anchor = ring.serial == P.serial;
            int first = ring.cap - ring.r < got ? ring.cap - ring.r : got;
            memcpy(dst, ring.buf + ring.r * 2, first * 2 * sizeof(float));
            memcpy(dst + first * 2, ring.buf, (got - first) * 2 * sizeof(float));
            ring.r = (ring.r + got) % ring.cap;
            ring.fill -= got;
            pthread_cond_signal(&ring.cv);
        }
        pthread_mutex_unlock(&ring.mu);
    }
    if (got > 0) {
        int v = g_mute ? 0 : atomic_load(&g_volume);
        if (v != 100) {
            float g = v / 100.0f;
            g = g * g * g;                       /* perceptual curve */
            for (int i = 0; i < got * 2; i++) dst[i] *= g;
        }
        if (anchor && !isnan(head)) {
            struct pw_time t;
            double delay = 0;
            if (pw_stream_get_time_n(stream, &t, sizeof t) == 0 && t.rate.denom)
                delay = (double)t.delay * t.rate.num / t.rate.denom + (double)(t.queued + t.buffered) / rate;
            if (P.await) tr_stage("audio anchored (clock runs)");
            clock_set(head - delay);
            if (P.await) P.await = false;
            clock_run(!P.paused);
        }
    }
    if (got < want) memset(dst + got * 2, 0, (want - got) * 2 * sizeof(float));
    d->chunk->offset = 0;
    d->chunk->stride = 2 * sizeof(float);
    d->chunk->size = want * 2 * sizeof(float);
    pw_stream_queue_buffer(stream, b);
}

static const struct pw_stream_events events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
};

int audio_init(void)
{
    pw_init(NULL, NULL);
    loop = pw_thread_loop_new("turron-audio", NULL);
    if (!loop || pw_thread_loop_start(loop) < 0) { loop = NULL; return -1; }
    return 0;
}

int audio_open(int r)
{
    if (!loop) return -1;
    rate = r;
    pthread_mutex_lock(&ring.mu);
    ring.cap = rate / 4;                         /* 250 ms */
    free(ring.buf);
    ring.buf = calloc(ring.cap * 2, sizeof(float));
    ring.r = ring.fill = 0;
    ring.end_pts = NAN;
    ring.serial = -1;
    pthread_mutex_unlock(&ring.mu);

    pw_thread_loop_lock(loop);
    char lat[32], nrate[32];
    snprintf(lat, sizeof lat, "%d/%d", 1024, rate);
    snprintf(nrate, sizeof nrate, "1/%d", rate);
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Movie",
        PW_KEY_APP_NAME, "Turrón", PW_KEY_APP_ID, "turron", PW_KEY_APP_ICON_NAME, "turron",
        PW_KEY_NODE_NAME, "turron", PW_KEY_NODE_LATENCY, lat, PW_KEY_NODE_RATE, nrate, NULL);
    if (O.audio_target) pw_properties_set(props, PW_KEY_TARGET_OBJECT, O.audio_target);
    stream = pw_stream_new_simple(pw_thread_loop_get_loop(loop), "Turrón", props, &events, NULL);
    int ret = -1;
    if (stream) {
        uint8_t pod[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pod, sizeof pod);
        struct spa_audio_info_raw info = {
            .format = SPA_AUDIO_FORMAT_F32, .rate = rate, .channels = 2,
            .position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR },
        };
        const struct spa_pod *params[1] = { spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info) };
        ret = pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS, params, 1);
        if (ret < 0) { pw_stream_destroy(stream); stream = NULL; }
    }
    pw_thread_loop_unlock(loop);
    (void)hook;
    return ret < 0 ? -1 : 0;
}

void audio_close(void)
{
    if (!loop || !stream) return;
    pw_thread_loop_lock(loop);
    pw_stream_destroy(stream);
    stream = NULL;
    pw_thread_loop_unlock(loop);
}

bool audio_write(const float *st, int frames, double pts, int serial)
{
    pthread_mutex_lock(&ring.mu);
    bool first = true;
    while (frames > 0) {
        if (P.abort || serial != P.serial) { pthread_mutex_unlock(&ring.mu); return !P.abort; }
        int space = ring.cap - ring.fill;
        if (space == 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 20000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&ring.cv, &ring.mu, &ts);
            continue;
        }
        int n = frames < space ? frames : space;
        int w = (ring.r + ring.fill) % ring.cap;
        int part = ring.cap - w < n ? ring.cap - w : n;
        memcpy(ring.buf + w * 2, st, part * 2 * sizeof(float));
        memcpy(ring.buf, st + part * 2, (n - part) * 2 * sizeof(float));
        if (first) {
            if (!isnan(pts)) ring.end_pts = pts;
            else if (isnan(ring.end_pts) || ring.serial != serial) ring.end_pts = player_clock();
            first = false;
        }
        ring.end_pts += (double)n / rate;
        ring.serial = serial;
        ring.fill += n;
        st += n * 2;
        frames -= n;
    }
    pthread_mutex_unlock(&ring.mu);
    return true;
}

void audio_flush(void)
{
    pthread_mutex_lock(&ring.mu);
    ring.r = ring.fill = 0;
    ring.end_pts = NAN;
    ring.serial = -1;
    pthread_cond_broadcast(&ring.cv);
    pthread_mutex_unlock(&ring.mu);
}

void audio_set_active(bool on)
{
    if (!loop || !stream) return;
    pw_thread_loop_lock(loop);
    pw_stream_set_active(stream, on);
    pw_thread_loop_unlock(loop);
}

bool audio_drained(void)
{
    pthread_mutex_lock(&ring.mu);
    bool e = ring.fill == 0;
    pthread_mutex_unlock(&ring.mu);
    return e;
}
