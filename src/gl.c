/* Turrón — EGL/GLES3 renderer.
 * VA-API frames are imported as dmabuf EGLImages (zero copy); software frames are
 * uploaded plane by plane and converted to RGB (and tone-mapped if HDR) in the shader. */
#include "turron.h"
#include <math.h>
#include <unistd.h>
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <wayland-egl.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#ifndef GL_R16_EXT
#define GL_R16_EXT 0x822A
#define GL_RG16_EXT 0x822C
#endif
#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL
#endif
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

static EGLDisplay dpy;
static EGLContext ctx;
static EGLSurface surf;
static struct wl_egl_window *ewin;
static int win_w, win_h;
static bool has_norm16, has_dmabuf, dmabuf_broken;
static void (*image_target)(GLenum target, void *image);

static GLuint prog_v, prog_o, vbo;
static GLint u_mode, u_swap, u_cm, u_coff, u_trc, u_peak;
static GLuint tex[3], osd_tex;
static struct { int w, h; GLenum ifmt; } tex_info[3];
static int osd_w, osd_h, osd_bbox[4];

static AVFrame *cur, *mapped;
static bool cur_dirty, have_video;
static int rotation;
static int vid_w, vid_h;                 /* display size of the video (SAR + rotation applied) */
static int rect[4];
static struct { int mode, swap, trc; float cm[9], coff[3], peak; } par;
static struct SwsContext *sws;
static AVFrame *conv;
static char desc[160] = "";
static const char *shot_path;
double gl_t_upload, gl_t_draw, gl_t_swap;
int gl_n_render;

static const char *VS =
    "#version 300 es\n"
    "in vec2 pos; in vec2 tc; out vec2 vt;\n"
    "void main(){ vt = tc; gl_Position = vec4(pos, 0.0, 1.0); }\n";

static const char *FS_VIDEO =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 vt; out vec4 o;\n"
    "uniform sampler2D t0, t1, t2;\n"
    "uniform int mode, swap, trc;\n"
    "uniform mat3 cm; uniform vec3 coff; uniform float peak;\n"
    "const mat3 to709 = mat3(1.6605, -0.1246, -0.0182, -0.5876, 1.1329, -0.1006, -0.0728, -0.0083, 1.1187);\n"
    "vec3 pq(vec3 e){ vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0/78.84375));\n"
    "  return pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), vec3(1.0/0.1593017578125)); }\n"
    "float hlg1(float e){ return e <= 0.5 ? e*e/3.0 : (exp((e - 0.55991073)/0.17883277) + 0.28466892)/12.0; }\n"
    "void main(){\n"
    "  if (mode == 2) { o = vec4(texture(t0, vt).rgb, 1.0); return; }\n"
    "  vec3 c = vec3(texture(t0, vt).r, 0.0, 0.0);\n"
    "  if (mode == 0) c.yz = vec2(texture(t1, vt).r, texture(t2, vt).r);\n"
    "  else if (mode == 1) c.yz = texture(t1, vt).rg;\n"
    "  if (swap == 1) c.yz = c.zy;\n"
    "  vec3 rgb = cm * c + coff;\n"
    "  if (trc != 0) {\n"
    "    vec3 l;\n"
    "    if (trc == 1) l = pq(rgb) * (10000.0/203.0);\n"
    "    else { vec3 s = vec3(hlg1(clamp(rgb.r,0.0,1.0)), hlg1(clamp(rgb.g,0.0,1.0)), hlg1(clamp(rgb.b,0.0,1.0)));\n"
    "      l = s * pow(max(dot(s, vec3(0.2627, 0.6780, 0.0593)), 1e-6), 0.2) * (1000.0/203.0); }\n"
    "    l = max(to709 * l, 0.0);\n"
    "    float m = max(max(l.r, l.g), l.b);\n"
    "    const float k = 0.5;\n"
    "    if (m > k) {\n"
    "      float t = k + (1.0 - k) * (1.0 - exp(-(m - k)/(1.0 - k))) / (1.0 - exp(-(peak - k)/(1.0 - k)));\n"
    "      l *= min(t, 1.0) / m;\n"
    "    }\n"
    "    rgb = pow(clamp(l, 0.0, 1.0), vec3(1.0/2.2));\n"
    "  }\n"
    "  o = vec4(rgb, 1.0);\n"
    "}\n";

static const char *FS_OSD =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 vt; out vec4 o; uniform sampler2D t0;\n"
    "void main(){ o = texture(t0, vt); }\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "turron: shader: %s\n", log);
    }
    return s;
}
static GLuint link_prog(const char *fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "pos");
    glBindAttribLocation(p, 1, "tc");
    glLinkProgram(p);
    return p;
}
static void tex_defaults(void)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

int gl_init(void *wl_display, void *wl_surface, int w, int h)
{
    dpy = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, wl_display, NULL);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) return -1;
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint cattr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cattr, &cfg, 1, &n) || n < 1) return -1;
    static const EGLint xattr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xattr);
    if (ctx == EGL_NO_CONTEXT) return -1;
    win_w = w; win_h = h;
    ewin = wl_egl_window_create(wl_surface, w, h);
    surf = eglCreatePlatformWindowSurface(dpy, cfg, ewin, NULL);
    if (surf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, surf, surf, ctx)) return -1;
    eglSwapInterval(dpy, 0);            /* we pace ourselves with frame callbacks */

    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    const char *eext = eglQueryString(dpy, EGL_EXTENSIONS);
    has_norm16 = ext && strstr(ext, "GL_EXT_texture_norm16");
    has_dmabuf = eext && strstr(eext, "EGL_EXT_image_dma_buf_import") && ext && strstr(ext, "GL_OES_EGL_image");
    image_target = (void (*)(GLenum, void *))eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!image_target) has_dmabuf = false;

    prog_v = link_prog(FS_VIDEO);
    prog_o = link_prog(FS_OSD);
    glUseProgram(prog_v);
    glUniform1i(glGetUniformLocation(prog_v, "t0"), 0);
    glUniform1i(glGetUniformLocation(prog_v, "t1"), 1);
    glUniform1i(glGetUniformLocation(prog_v, "t2"), 2);
    u_mode = glGetUniformLocation(prog_v, "mode");
    u_swap = glGetUniformLocation(prog_v, "swap");
    u_trc = glGetUniformLocation(prog_v, "trc");
    u_cm = glGetUniformLocation(prog_v, "cm");
    u_coff = glGetUniformLocation(prog_v, "coff");
    u_peak = glGetUniformLocation(prog_v, "peak");
    glUseProgram(prog_o);
    glUniform1i(glGetUniformLocation(prog_o, "t0"), 0);

    glGenTextures(3, tex);
    glGenTextures(1, &osd_tex);
    for (int i = 0; i < 3; i++) { glBindTexture(GL_TEXTURE_2D, tex[i]); tex_defaults(); }
    glBindTexture(GL_TEXTURE_2D, osd_tex);
    tex_defaults();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glClearColor(0, 0, 0, 1);
    return 0;
}

static void layout(void)
{
    if (!have_video || vid_w <= 0 || vid_h <= 0) { rect[0] = rect[1] = 0; rect[2] = win_w; rect[3] = win_h; return; }
    double a = (double)vid_w / vid_h;
    int w = win_w, h = (int)lrint(win_w / a);
    if (h > win_h) { h = win_h; w = (int)lrint(win_h * a); }
    rect[0] = (win_w - w) / 2;
    rect[1] = (win_h - h) / 2;
    rect[2] = w;
    rect[3] = h;
}

void gl_resize(int w, int h)
{
    if (w == win_w && h == win_h) return;
    win_w = w; win_h = h;
    wl_egl_window_resize(ewin, w, h, 0, 0);
    layout();
}

void gl_video_rect(int *x, int *y, int *w, int *h, int *vw, int *vh)
{
    *x = rect[0]; *y = rect[1]; *w = rect[2]; *h = rect[3];
    *vw = cur ? cur->width : 0;
    *vh = cur ? cur->height : 0;
}

/* YCbCr -> RGB matrix for normalised texture samples. `scale` maps the sample to
 * a 0..1 code value of the real bit depth (10-bit data in 16-bit textures). */
static void set_matrix(const AVFrame *f, int depth, double scale, bool gray)
{
    double kr, kb;
    int cs = f->colorspace;
    if (cs == AVCOL_SPC_UNSPECIFIED) cs = (f->width >= 1280 || f->height > 576) ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
    switch (cs) {
    case AVCOL_SPC_BT709: kr = 0.2126; kb = 0.0722; break;
    case AVCOL_SPC_BT2020_NCL: case AVCOL_SPC_BT2020_CL: kr = 0.2627; kb = 0.0593; break;
    case AVCOL_SPC_SMPTE240M: kr = 0.212; kb = 0.087; break;
    case AVCOL_SPC_FCC: kr = 0.30; kb = 0.11; break;
    default: kr = 0.299; kb = 0.114; break;
    }
    double kg = 1 - kr - kb;
    double maxv = (double)((1 << depth) - 1), sh = (double)(1 << (depth - 8));
    bool full = f->color_range == AVCOL_RANGE_JPEG;
    double yo = full ? 0 : 16 * sh / maxv, ys = full ? 1 : maxv / (219 * sh);
    double co = (double)(1 << (depth - 1)) / maxv, csn = full ? 1 : maxv / (224 * sh);
    /* rgb = M * [ys*(y*scale - yo), csn*(cb*scale - co), csn*(cr*scale - co)] */
    double m[3][3] = {
        { 1, 0, 2 * (1 - kr) },
        { 1, -2 * kb * (1 - kb) / kg, -2 * kr * (1 - kr) / kg },
        { 1, 2 * (1 - kb), 0 },
    };
    double off[3];
    for (int r = 0; r < 3; r++) {
        double a = m[r][0] * ys, b = gray ? 0 : m[r][1] * csn, c = gray ? 0 : m[r][2] * csn;
        off[r] = -(a * yo + b * co + c * co);
        /* column-major for GL */
        par.cm[0 * 3 + r] = (float)(a * scale);
        par.cm[1 * 3 + r] = (float)(b * scale);
        par.cm[2 * 3 + r] = (float)(c * scale);
        par.coff[r] = (float)off[r];
    }
    par.trc = f->color_trc == AVCOL_TRC_SMPTE2084 ? 1 : f->color_trc == AVCOL_TRC_ARIB_STD_B67 ? 2 : 0;
    double peak = 1000;
    const AVFrameSideData *sd = av_frame_get_side_data(f, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (sd && ((const AVContentLightMetadata *)sd->data)->MaxCLL > 0)
        peak = ((const AVContentLightMetadata *)sd->data)->MaxCLL;
    else if ((sd = av_frame_get_side_data(f, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA))) {
        const AVMasteringDisplayMetadata *md = (const AVMasteringDisplayMetadata *)sd->data;
        if (md->has_luminance && md->max_luminance.den) peak = av_q2d(md->max_luminance);
    }
    if (peak < 400) peak = 400;
    if (peak > 4000) peak = 4000;
    par.peak = (float)(peak / 203.0);
}

static void set_size(const AVFrame *f)
{
    int w = f->width, h = f->height;
    AVRational sar = f->sample_aspect_ratio;
    if (sar.num > 0 && sar.den > 0 && sar.num != sar.den) {
        if (sar.num > sar.den) w = (int)lrint((double)w * sar.num / sar.den);
        else h = (int)lrint((double)h * sar.den / sar.num);
    }
    if (rotation == 90 || rotation == 270) { int t = w; w = h; h = t; }
    if (w != vid_w || h != vid_h || !have_video) {
        vid_w = w; vid_h = h;
        have_video = true;
        layout();
    }
}

static GLuint pbo[3];

static void upload_plane(int i, int w, int h, GLenum ifmt, GLenum fmt, GLenum type, const uint8_t *data, int row_px)
{
    int comps = fmt == GL_RGBA ? 4 : fmt == GL_RG ? 2 : 1;
    int bpp = comps * (type == GL_UNSIGNED_SHORT ? 2 : 1);
    bool use_pbo = type == GL_UNSIGNED_SHORT;
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, tex[i]);
    if (tex_info[i].w != w || tex_info[i].h != h || tex_info[i].ifmt != ifmt) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, NULL);
        tex_info[i].w = w; tex_info[i].h = h; tex_info[i].ifmt = ifmt;
    }
    if (use_pbo) {
        /* The driver's direct path for 16-bit textures is slow; a pixel buffer
         * turns the upload into one memcpy plus a GPU-side copy. */
        size_t size = (size_t)w * h * bpp, row = (size_t)w * bpp, stride = (size_t)row_px * bpp;
        if (!pbo[i]) glGenBuffers(1, &pbo[i]);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[i]);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, size, NULL, GL_STREAM_DRAW);
        uint8_t *dst = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, size, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
        if (dst) {
            if (stride == row) memcpy(dst, data, size);
            else for (int y = 0; y < h; y++) memcpy(dst + y * row, data + y * stride, row);
            glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, type, NULL);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            return;
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, row_px);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, type, data);
}

static bool direct_ok(const AVPixFmtDescriptor *d, const AVFrame *f)
{
    if (d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM |
                    AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_FLOAT | AV_PIX_FMT_FLAG_BAYER)) return false;
    if (f->linesize[0] <= 0) return false;
    int depth = d->comp[0].depth;
    if (depth > 8 && !has_norm16) return false;
    if (depth > 16) return false;
    int colour = d->nb_components - ((d->flags & AV_PIX_FMT_FLAG_ALPHA) ? 1 : 0);
    if (colour == 1) return d->comp[0].step == (depth > 8 ? 2 : 1);
    if (colour != 3) return false;
    int planes = av_pix_fmt_count_planes(f->format) - ((d->flags & AV_PIX_FMT_FLAG_ALPHA) ? 1 : 0);
    if (planes == 3) return d->flags & AV_PIX_FMT_FLAG_PLANAR ? d->comp[1].step == (depth > 8 ? 2 : 1) : false;
    if (planes == 2) return d->comp[1].plane == 1 && d->comp[2].plane == 1;   /* NV12, NV21, P010… */
    return false;
}

static void upload_sw(AVFrame *f)
{
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(f->format);
    if (!d) return;
    if (!direct_ok(d, f)) {
        enum AVPixelFormat dst = (d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BAYER))
                               ? AV_PIX_FMT_RGBA : AV_PIX_FMT_YUV420P;
        sws = sws_getCachedContext(sws, f->width, f->height, f->format, f->width, f->height, dst,
                                   SWS_BILINEAR, NULL, NULL, NULL);
        if (!sws) return;
        if (!conv) conv = av_frame_alloc();
        if (conv->width != f->width || conv->height != f->height || conv->format != dst) {
            av_frame_unref(conv);
            conv->width = f->width; conv->height = f->height; conv->format = dst;
            if (av_frame_get_buffer(conv, 32) < 0) return;
        }
        sws_scale(sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, conv->data, conv->linesize);
        av_frame_copy_props(conv, f);
        if (dst == AV_PIX_FMT_RGBA) {
            upload_plane(0, f->width, f->height, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, conv->data[0], conv->linesize[0] / 4);
            par.mode = 2;
            par.trc = 0;
            snprintf(desc, sizeof desc, "%dx%d %s → RGBA (swscale)", f->width, f->height, d->name);
            return;
        }
        const char *src = d->name;
        f = conv;
        d = av_pix_fmt_desc_get(dst);
        snprintf(desc, sizeof desc, "%dx%d %s → yuv420p (swscale)", f->width, f->height, src);
    } else {
        snprintf(desc, sizeof desc, "%dx%d %s", f->width, f->height, d->name);
    }
    int depth = d->comp[0].depth, bytes = depth > 8 ? 2 : 1;
    GLenum type = bytes == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
    int colour = d->nb_components - ((d->flags & AV_PIX_FMT_FLAG_ALPHA) ? 1 : 0);
    int cw = AV_CEIL_RSHIFT(f->width, d->log2_chroma_w), ch = AV_CEIL_RSHIFT(f->height, d->log2_chroma_h);
    upload_plane(0, f->width, f->height, bytes == 2 ? GL_R16_EXT : GL_R8, GL_RED, type, f->data[0], f->linesize[0] / bytes);
    par.swap = 0;
    if (colour == 1) par.mode = 3;
    else if (d->comp[1].plane == d->comp[2].plane) {
        upload_plane(1, cw, ch, bytes == 2 ? GL_RG16_EXT : GL_RG8, GL_RG, type, f->data[1], f->linesize[1] / (2 * bytes));
        par.mode = 1;
        par.swap = d->comp[1].offset > d->comp[2].offset;
    } else {
        upload_plane(1, cw, ch, bytes == 2 ? GL_R16_EXT : GL_R8, GL_RED, type, f->data[1], f->linesize[1] / bytes);
        upload_plane(2, cw, ch, bytes == 2 ? GL_R16_EXT : GL_R8, GL_RED, type, f->data[2], f->linesize[2] / bytes);
        par.mode = 0;
    }
    double scale = bytes == 2 ? 65535.0 / (double)(((1 << depth) - 1) << d->comp[0].shift) : 1.0;
    set_matrix(f, depth, scale, colour == 1);
}

static bool upload_hw(AVFrame *f)
{
    if (!has_dmabuf || dmabuf_broken) return false;
    AVFrame *m = av_frame_alloc();
    m->format = AV_PIX_FMT_DRM_PRIME;
    if (av_hwframe_map(m, f, AV_HWFRAME_MAP_READ) < 0) { av_frame_free(&m); return false; }
    const AVDRMFrameDescriptor *dd = (const AVDRMFrameDescriptor *)m->data[0];
    enum AVPixelFormat swf = AV_PIX_FMT_NV12;
    if (f->hw_frames_ctx) swf = ((AVHWFramesContext *)f->hw_frames_ctx->data)->sw_format;
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(swf);
    int depth = d ? d->comp[0].depth : 8;
    struct { int fd; uint32_t off, pitch, fourcc; uint64_t mod; } pl[3];
    int np = 0;
    for (int l = 0; l < dd->nb_layers && np < 3; l++)
        for (int p = 0; p < dd->layers[l].nb_planes && np < 3; p++) {
            const AVDRMPlaneDescriptor *pd = &dd->layers[l].planes[p];
            pl[np].fd = dd->objects[pd->object_index].fd;
            pl[np].mod = dd->objects[pd->object_index].format_modifier;
            pl[np].off = pd->offset;
            pl[np].pitch = pd->pitch;
            pl[np].fourcc = dd->layers[l].nb_planes == 1 ? dd->layers[l].format : 0;
            np++;
        }
    bool ok = d && (np == 2 || np == 3);
    for (int i = 0; ok && i < np; i++) {
        uint32_t fourcc = pl[i].fourcc;
        if (!fourcc) {
            if (np == 2) fourcc = i == 0 ? (depth > 8 ? FOURCC('R', '1', '6', ' ') : FOURCC('R', '8', ' ', ' '))
                                         : (depth > 8 ? FOURCC('G', 'R', '3', '2') : FOURCC('G', 'R', '8', '8'));
            else fourcc = depth > 8 ? FOURCC('R', '1', '6', ' ') : FOURCC('R', '8', ' ', ' ');
        }
        int w = i ? AV_CEIL_RSHIFT(f->width, d->log2_chroma_w) : f->width;
        int h = i ? AV_CEIL_RSHIFT(f->height, d->log2_chroma_h) : f->height;
        EGLAttrib at[32];
        int n = 0;
        at[n++] = EGL_WIDTH; at[n++] = w;
        at[n++] = EGL_HEIGHT; at[n++] = h;
        at[n++] = EGL_LINUX_DRM_FOURCC_EXT; at[n++] = fourcc;
        at[n++] = EGL_DMA_BUF_PLANE0_FD_EXT; at[n++] = pl[i].fd;
        at[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; at[n++] = pl[i].off;
        at[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT; at[n++] = pl[i].pitch;
        if (pl[i].mod != DRM_FORMAT_MOD_INVALID) {
            at[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT; at[n++] = pl[i].mod & 0xffffffff;
            at[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT; at[n++] = pl[i].mod >> 32;
        }
        at[n] = EGL_NONE;
        EGLImage img = eglCreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, at);
        if (img == EGL_NO_IMAGE) { ok = false; break; }
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        image_target(GL_TEXTURE_2D, img);
        eglDestroyImage(dpy, img);
        tex_info[i].w = tex_info[i].h = 0;      /* storage now belongs to the image */
    }
    if (!ok) {
        av_frame_free(&m);
        dmabuf_broken = true;
        for (int i = 0; i < 3; i++) {           /* textures bound to images cannot be re-specified */
            glDeleteTextures(1, &tex[i]);
            glGenTextures(1, &tex[i]);
            glBindTexture(GL_TEXTURE_2D, tex[i]);
            tex_defaults();
            tex_info[i].w = 0;
        }
        return false;
    }
    av_frame_free(&mapped);
    mapped = m;
    par.mode = np == 2 ? 1 : 0;
    par.swap = 0;
    set_matrix(f, depth, 1.0, false);
    snprintf(desc, sizeof desc, "%dx%d %s VA-API (%s)", f->width, f->height, d->name, T("copia cero", "zero copy"));
    return true;
}

static void upload(void)
{
    cur_dirty = false;
    if (!cur) return;
    set_size(cur);
    if (cur->format == AV_PIX_FMT_VAAPI || cur->hw_frames_ctx) {
        if (upload_hw(cur)) return;
        AVFrame *swf = av_frame_alloc();
        if (av_hwframe_transfer_data(swf, cur, 0) >= 0) {
            av_frame_copy_props(swf, cur);
            av_frame_free(&mapped);
            for (int i = 0; i < 3; i++) if (!tex_info[i].w) {   /* drop image-backed textures */
                glDeleteTextures(1, &tex[i]);
                glGenTextures(1, &tex[i]);
                glBindTexture(GL_TEXTURE_2D, tex[i]);
                tex_defaults();
            }
            upload_sw(swf);
        }
        av_frame_free(&swf);
        return;
    }
    if (mapped) {
        av_frame_free(&mapped);
        for (int i = 0; i < 3; i++) {
            glDeleteTextures(1, &tex[i]);
            glGenTextures(1, &tex[i]);
            glBindTexture(GL_TEXTURE_2D, tex[i]);
            tex_defaults();
            tex_info[i].w = 0;
        }
    }
    upload_sw(cur);
}

void gl_set_frame(AVFrame *f, int rot)
{
    av_frame_free(&cur);
    cur = f;
    rotation = rot;
    cur_dirty = true;
}

void gl_drop_frame(void)
{
    av_frame_free(&cur);
    av_frame_free(&mapped);
    have_video = false;
    cur_dirty = false;
    desc[0] = 0;
    layout();
}

const char *gl_frame_desc(void) { return desc; }

void gl_osd_upload(const uint8_t *pix, int stride, const int dirty[4], const int bbox[4])
{
    memcpy(osd_bbox, bbox, sizeof osd_bbox);
    if (!pix) return;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, osd_tex);
    if (osd_w != win_w || osd_h != win_h) {
        osd_w = win_w; osd_h = win_h;
        glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, osd_w, osd_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
        return;
    }
    if (dirty[2] <= 0 || dirty[3] <= 0) return;
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, dirty[0], dirty[1], dirty[2], dirty[3], GL_RGBA, GL_UNSIGNED_BYTE,
                    pix + (size_t)dirty[1] * stride + (size_t)dirty[0] * 4);
}

static void quad(const int r[4], const float t[8])
{
    float x0 = -1 + 2.0f * r[0] / win_w, x1 = -1 + 2.0f * (r[0] + r[2]) / win_w;
    float y0 = 1 - 2.0f * r[1] / win_h, y1 = 1 - 2.0f * (r[1] + r[3]) / win_h;
    float v[16] = { x0, y0, t[0], t[1], x1, y0, t[2], t[3], x0, y1, t[4], t[5], x1, y1, t[6], t[7] };
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void gl_shot(const char *path) { shot_path = path; }

void gl_render(void)
{
    double t0 = now_s();
    if (cur_dirty) upload();
    double t1 = now_s();
    glViewport(0, 0, win_w, win_h);
    bool full = have_video && cur && rect[2] == win_w && rect[3] == win_h;
    if (!full) glClear(GL_COLOR_BUFFER_BIT);
    if (have_video && cur) {
        static const float TC[4][8] = {
            { 0, 0, 1, 0, 0, 1, 1, 1 },
            { 0, 1, 0, 0, 1, 1, 1, 0 },
            { 1, 1, 0, 1, 1, 0, 0, 0 },
            { 1, 0, 1, 1, 0, 0, 0, 1 },
        };
        glDisable(GL_BLEND);
        glUseProgram(prog_v);
        glUniform1i(u_mode, par.mode);
        glUniform1i(u_swap, par.swap);
        glUniform1i(u_trc, par.trc);
        glUniform1f(u_peak, par.peak);
        glUniformMatrix3fv(u_cm, 1, GL_FALSE, par.cm);
        glUniform3fv(u_coff, 1, par.coff);
        for (int i = 0; i < 3; i++) { glActiveTexture(GL_TEXTURE0 + i); glBindTexture(GL_TEXTURE_2D, tex[i]); }
        quad(rect, TC[(rotation / 90) & 3]);
    }
    if (osd_bbox[2] > 0 && osd_bbox[3] > 0 && osd_w == win_w && osd_h == win_h) {
        float u0 = (float)osd_bbox[0] / osd_w, u1 = (float)(osd_bbox[0] + osd_bbox[2]) / osd_w;
        float v0 = (float)osd_bbox[1] / osd_h, v1 = (float)(osd_bbox[1] + osd_bbox[3]) / osd_h;
        float t[8] = { u0, v0, u1, v0, u0, v1, u1, v1 };
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(prog_o);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, osd_tex);
        quad(osd_bbox, t);
    }
    if (shot_path) {
        uint8_t *px = malloc((size_t)win_w * win_h * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, win_w, win_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        FILE *fp = fopen(shot_path, "wb");
        if (fp) {
            fprintf(fp, "P6\n%d %d\n255\n", win_w, win_h);
            for (int y = win_h - 1; y >= 0; y--)
                for (int x = 0; x < win_w; x++) fwrite(px + ((size_t)y * win_w + x) * 4, 1, 3, fp);
            fclose(fp);
        }
        free(px);
        shot_path = NULL;
    }
    double t2 = now_s();
    eglSwapBuffers(dpy, surf);
    gl_t_upload += t1 - t0;
    gl_t_draw += t2 - t1;
    gl_t_swap += now_s() - t2;
    gl_n_render++;
}
