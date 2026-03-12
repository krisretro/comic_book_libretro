#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <archive.h>
#include <archive_entry.h>
#include <webp/decode.h>
#include "animations.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"
#include "libretro.h"
#define MAX_PAGES 4096
#define MAX_W 1920
#define MAX_H 1440
/* --- New Includes for PDF --- */
#include <mupdf/fitz.h>
typedef struct { unsigned char *pixels; int w, h; } PageData;
typedef struct { char *filename; bool is_right_half; } VirtualPage;
typedef struct {
    unsigned page_index;
    int render_mode;
    int brightness;
    float contrast;
    float sharpness;
    float gamma;
    bool auto_contrast;
    bool manga_mode;
    bool double_mode;
} SaveState;
/* --- Global State --- */
static uint16_t *framebuffer = NULL;
static int cur_width = 1280, cur_height = 960;
static char archive_path[1024];
static VirtualPage v_pages[MAX_PAGES];
static unsigned total_v_pages = 0;
static unsigned current_view_index = 0;
static PageData img_l = {NULL, 0, 0}, img_r = {NULL, 0, 0};
static bool game_loaded = false;
static uint8_t dummy_state[4];
/* --- Visual & Page Params --- */
static int opt_start = 0, opt_ignore_last = 0, opt_res = 2;
// 0=None, 1=Curl, 2=Push, 3=Cover
static int opt_anim_mode = 1;
static bool opt_manga = false, opt_double = false, opt_p1_right = true;
static int brightness = 0;
static float contrast = 1.0f;
static float sharpness = 0.0f;
static float gamma_val = 1.0f;
static bool auto_contrast = false;
static float auto_min = 0.0f, auto_max = 255.0f;
static int render_mode = 0;
static bool last_btns[16] = {0};
static uint16_t *anim_source_fb = NULL;
static uint16_t *anim_target_fb = NULL;
static bool vars_initialized = false;
static retro_video_refresh_t video_cb;
static retro_environment_t environ_cb;
static retro_input_state_t input_state_cb;
static retro_input_poll_t input_poll_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static int16_t *flip_sound_data = NULL;
static size_t flip_sound_samples = 0;
static size_t flip_sound_pos = 0;
static bool play_flip = false;
/* --- Global State (Added for PDF) --- */
static bool is_pdf = false;
static fz_context *pdf_ctx = NULL;
static fz_document *pdf_doc = NULL;
/* --- PDF Helper Functions --- */
static void close_pdf() {
    if (pdf_doc) fz_drop_document(pdf_ctx, pdf_doc);
    if (pdf_ctx) fz_drop_context(pdf_ctx);
    pdf_doc = NULL;
    pdf_ctx = NULL;
}
static unsigned char* render_pdf_page(int page_num, int *w, int *h) {
    if (!pdf_ctx || !pdf_doc) return NULL;
   
    int max_pages = fz_count_pages(pdf_ctx, pdf_doc);
    if (page_num < 0 || page_num >= max_pages) return NULL;
    fz_pixmap *pix = NULL;
    unsigned char *output = NULL;
   
    fz_try(pdf_ctx) {
        fz_page *page = fz_load_page(pdf_ctx, pdf_doc, page_num);
        fz_rect rect = fz_bound_page(pdf_ctx, page);
       
        float scale = (float)cur_height / (rect.y1 - rect.y0);
        fz_matrix ctm = fz_scale(scale, scale);
       
        pix = fz_new_pixmap_from_page(pdf_ctx, page, ctm, fz_device_rgb(pdf_ctx), 0);
        *w = fz_pixmap_width(pdf_ctx, pix);
        *h = fz_pixmap_height(pdf_ctx, pix);
       
        output = malloc((*w) * (*h) * 4);
        unsigned char *s = fz_pixmap_samples(pdf_ctx, pix);
        int stride = fz_pixmap_stride(pdf_ctx, pix);
        for (int y = 0; y < *h; y++) {
            for (int x = 0; x < *w; x++) {
                unsigned char *src = s + (y * stride) + (x * 3);
                unsigned char *dst = output + (y * (*w) * 4) + (x * 4);
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = 255;
            }
        }
       
        fz_drop_page(pdf_ctx, page);
        fz_drop_pixmap(pdf_ctx, pix);
    }
    fz_catch(pdf_ctx) {
        if(output) free(output);
        return NULL;
    }
    return output;
}
static void load_flip_sound() {
    const char *system_dir = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) && system_dir) {
        char sound_path[1024];
        snprintf(sound_path, sizeof(sound_path), "%s/flip.wav", system_dir);
       
        FILE *f = fopen(sound_path, "rb");
        if (f) {
            fseek(f, 44, SEEK_SET);
            fseek(f, 0, SEEK_END);
            long size = ftell(f) - 44;
            fseek(f, 44, SEEK_SET);
           
            flip_sound_samples = size / sizeof(int16_t);
            flip_sound_data = malloc(size);
            fread(flip_sound_data, 1, size, f);
            fclose(f);
        }
    }
}
/* --- Image Processing Math --- */
static void analyze_page(PageData *img) {
    if (!img || !img->pixels) return;
    float min_l = 255, max_l = 0;
    for (int y = 0; y < img->h; y += 20) {
        for (int x = 0; x < img->w; x += 20) {
            unsigned char *p = &img->pixels[(y * img->w + x) * 4];
            float lum = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
            if (lum < min_l) min_l = lum;
            if (lum > max_l) max_l = lum;
        }
    }
    auto_min = min_l; auto_max = max_l;
}
static inline uint16_t process_pixel(int r, int g, int b) {
    if (auto_contrast) {
        float range = (auto_max - auto_min);
        if (range < 1.0f) range = 1.0f;
        r = (int)((r - auto_min) * 255.0f / range);
        g = (int)((g - auto_min) * 255.0f / range);
        b = (int)((b - auto_min) * 255.0f / range);
    }
    r = (int)((r - 128) * contrast + 128 + brightness);
    g = (int)((g - 128) * contrast + 128 + brightness);
    b = (int)((b - 128) * contrast + 128 + brightness);
    if (gamma_val != 1.0f) {
        r = (int)(255 * pow(r/255.0, 1.0/gamma_val));
        g = (int)(255 * pow(g/255.0, 1.0/gamma_val));
        b = (int)(255 * pow(b/255.0, 1.0/gamma_val));
    }
    if (r<0) r=0; if (r>255) r=255;
    if (g<0) g=0; if (g>255) g=255;
    if (b<0) b=0; if (b>255) b=255;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}
static void draw_to_fb(PageData *img, int x_off, int dst_w, bool is_right) {
    if (!img || !img->pixels) return;
    int render_h = cur_height;
    bool wide = (img->w > img->h);
    int sx_base = (wide && is_right) ? (img->w / 2) : 0;
    int sw = wide ? (img->w / 2) : img->w;
    for (int y = 0; y < render_h; y++) {
        float fy = (float)y * img->h / render_h;
        int sy = (int)fy;
        for (int x = 0; x < dst_w; x++) {
            float fx = (float)sx_base + (x * sw) / dst_w;
            int sx = (int)fx;
            int r, g, b;
            if (render_mode == 0) { // Nearest
                unsigned char *p = &img->pixels[(sy * img->w + sx) * 4];
                r = p[0]; g = p[1]; b = p[2];
            } else if (render_mode == 1) { // Bilinear
                int x1 = (sx < img->w - 1) ? sx + 1 : sx;
                int y1 = (sy < img->h - 1) ? sy + 1 : sy;
                float dx = fx - sx, dy = fy - sy;
                unsigned char *p00 = &img->pixels[(sy * img->w + sx) * 4];
                unsigned char *p10 = &img->pixels[(sy * img->w + x1) * 4];
                unsigned char *p01 = &img->pixels[(y1 * img->w + sx) * 4];
                unsigned char *p11 = &img->pixels[(y1 * img->w + x1) * 4];
                r = (int)(p00[0]*(1-dx)*(1-dy) + p10[0]*dx*(1-dy) + p01[0]*(1-dx)*dy + p11[0]*dx*dy);
                g = (int)(p00[1]*(1-dx)*(1-dy) + p10[1]*dx*(1-dy) + p01[1]*(1-dx)*dy + p11[1]*dx*dy);
                b = (int)(p00[2]*(1-dx)*(1-dy) + p10[2]*dx*(1-dy) + p01[2]*(1-dx)*dy + p11[2]*dx*dy);
            } else { // Area Average
                unsigned char *p = &img->pixels[(sy * img->w + sx) * 4];
                unsigned char *p2 = &img->pixels[(sy * img->w + (sx+1 >= img->w ? sx : sx+1)) * 4];
                r = (p[0] + p2[0]) / 2; g = (p[1] + p2[1]) / 2; b = (p[2] + p2[2]) / 2;
            }
            if (sharpness > 0.05f && sx > 0 && sy > 0 && sx < img->w-1 && sy < img->h-1) {
                unsigned char *c = &img->pixels[(sy*img->w+sx)*4];
                unsigned char *u = &img->pixels[((sy-1)*img->w+sx)*4];
                unsigned char *d = &img->pixels[((sy+1)*img->w+sx)*4];
                r += (int)(sharpness * (c[0]*4 - u[0] - d[0] - c[-4] - c[4]));
                g += (int)(sharpness * (c[1]*4 - u[1] - d[1] - c[-3] - c[5]));
                b += (int)(sharpness * (c[2]*4 - u[2] - d[2] - c[-2] - c[6]));
            }
            framebuffer[y * cur_width + (x + x_off)] = process_pixel(r, g, b);
        }
    }
}
/* --- Archive, Indexing & Sorting --- */
static int qsort_filenames(const void *a, const void *b) {
    return strcmp(((VirtualPage*)a)->filename, ((VirtualPage*)b)->filename);
}
static void* extract_mem(const char* arc, const char* file, size_t* sz) {
    struct archive *a = archive_read_new();
    archive_read_support_format_all(a); archive_read_support_filter_all(a);
    if (archive_read_open_filename(a, arc, 10240) != ARCHIVE_OK) { archive_read_free(a); return NULL; }
    struct archive_entry *e; void* b = NULL;
    while (archive_read_next_header(a, &e) == ARCHIVE_OK) {
        if (strcmp(archive_entry_pathname(e), file) == 0) {
            *sz = archive_entry_size(e); b = malloc(*sz); archive_read_data(a, b, *sz); break;
        }
    }
    archive_read_free(a); return b;
}
static void rebuild_index() {
    if (!game_loaded) return;
    const char* ext = strrchr(archive_path, '.');
    is_pdf = (ext && strcasecmp(ext, ".pdf") == 0);
    for (unsigned i = 0; i < total_v_pages; i++) if (v_pages[i].filename) free(v_pages[i].filename);
    total_v_pages = 0;
    if (is_pdf) {
        if (!pdf_ctx) { is_pdf = false; return; }
        fz_try(pdf_ctx) {
            if (pdf_doc) fz_drop_document(pdf_ctx, pdf_doc);
            pdf_doc = fz_open_document(pdf_ctx, archive_path);
            int count = fz_count_pages(pdf_ctx, pdf_doc);
            int start = (opt_start > 0) ? opt_start - 1 : 0;
            int end = count - opt_ignore_last;
            if (start < 0) start = 0;
            if (end > count) end = count;
            for (int i = start; i < end && total_v_pages < MAX_PAGES; i++) {
                v_pages[total_v_pages++] = (VirtualPage){NULL, false};
            }
        }
        fz_catch(pdf_ctx) {
            is_pdf = false;
            if (pdf_doc) fz_drop_document(pdf_ctx, pdf_doc);
            pdf_doc = NULL;
        }
    } else {
        struct archive *a = archive_read_new();
        archive_read_support_format_all(a); archive_read_support_filter_all(a);
        if (archive_read_open_filename(a, archive_path, 10240) != ARCHIVE_OK) {
            archive_read_free(a);
            return;
        }
        struct archive_entry *e;
        VirtualPage temp[MAX_PAGES];
        int count = 0;
        while (archive_read_next_header(a, &e) == ARCHIVE_OK) {
            if (archive_entry_filetype(e) == AE_IFDIR) continue;
            const char* name = archive_entry_pathname(e);
            const char* ext = strrchr(name, '.');
            if (ext && (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".png") || !strcasecmp(ext, ".jpeg") || !strcasecmp(ext, ".webp"))) {
                temp[count++].filename = strdup(name);
                if (count >= MAX_PAGES) break;
            }
        }
        archive_read_free(a);
        qsort(temp, count, sizeof(VirtualPage), qsort_filenames);
        int start_point = (opt_start > 0) ? opt_start - 1 : 0;
        int end_point = count - opt_ignore_last;
        if (start_point < 0) start_point = 0;
        if (start_point > count) start_point = count;
        if (end_point > count) end_point = count;
        if (end_point < start_point) end_point = start_point;
        for (int i = start_point; i < end_point; i++) {
            v_pages[total_v_pages++] = (VirtualPage){temp[i].filename, false};
        }
        for (int i = 0; i < start_point; i++) if (i < count) free(temp[i].filename);
        for (int i = end_point; i < count; i++) if (i < count) free(temp[i].filename);
    }
}
static unsigned char* load_pixels_helper(void* d, size_t sz, int* w, int* h) {
    if (sz > 12 && memcmp(d, "RIFF", 4) == 0 && memcmp((uint8_t*)d + 8, "WEBP", 4) == 0) {
        return WebPDecodeRGBA((const uint8_t*)d, sz, w, h);
    }
    return stbi_load_from_memory((const stbi_uc*)d, (int)sz, w, h, NULL, 4);
}
static void refresh() {
    if (img_l.pixels) free(img_l.pixels); if (img_r.pixels) free(img_r.pixels);
    img_l.pixels = img_r.pixels = NULL;
    if (is_pdf) {
        if (!opt_double) {
            if (current_view_index < total_v_pages) {
                img_l.pixels = render_pdf_page(current_view_index + (opt_start > 0 ? opt_start - 1 : 0), &img_l.w, &img_l.h);
                analyze_page(&img_l);
            }
        } else {
            int l_idx = opt_p1_right ? ((int)current_view_index * 2) - 1 : ((int)current_view_index * 2);
            int r_idx = l_idx + 1;
            int offset = (opt_start > 0 ? opt_start - 1 : 0);
            if (l_idx >= 0 && l_idx < (int)total_v_pages) {
                img_l.pixels = render_pdf_page(l_idx + offset, &img_l.w, &img_l.h);
                analyze_page(&img_l);
            }
            if (r_idx >= 0 && r_idx < (int)total_v_pages) {
                img_r.pixels = render_pdf_page(r_idx + offset, &img_r.w, &img_r.h);
            }
        }
    } else {
        if (!opt_double) {
            if (current_view_index < total_v_pages) {
                size_t sz; void* d = extract_mem(archive_path, v_pages[current_view_index].filename, &sz);
                if(d){ img_l.pixels = load_pixels_helper(d, sz, &img_l.w, &img_l.h); free(d); analyze_page(&img_l); }
            }
        } else {
            int l_idx = opt_p1_right ? ((int)current_view_index * 2) - 1 : ((int)current_view_index * 2);
            int r_idx = l_idx + 1;
            size_t sl, sr;
            if (l_idx >= 0 && l_idx < (int)total_v_pages) {
                void* d = extract_mem(archive_path, v_pages[l_idx].filename, &sl);
                if(d){ img_l.pixels = load_pixels_helper(d, sl, &img_l.w, &img_l.h); free(d); analyze_page(&img_l); }
            }
            if (r_idx >= 0 && r_idx < (int)total_v_pages) {
                void* d = extract_mem(archive_path, v_pages[r_idx].filename, &sr);
                if(d){ img_r.pixels = load_pixels_helper(d, sr, &img_r.w, &img_r.h); free(d); }
            }
        }
    }
}
/* --- Libretro API Implementation --- */
void update_vars() {
    struct retro_variable var = {0};
    bool reindex = false;
    bool resize = false;
    bool old_double = opt_double;
   
    var.key = "ComicBook_start";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        int v = atoi(var.value);
        if(v != opt_start){ opt_start = v; reindex = true; }
    }
   
    var.key = "ComicBook_ignore_last";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        int v = atoi(var.value);
        if(v != opt_ignore_last){ opt_ignore_last = v; reindex = true; }
    }
    var.key = "ComicBook_res";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        int v = 2; /* default 1280x960 */
        if (strstr(var.value, "1920")) v = 4;
        else if (strstr(var.value, "640")) v = 1;
        /* else 1280 */
        if(v != opt_res){ opt_res = v; resize = true; }
    }
    var.key = "ComicBook_anim_mode";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (strcmp(var.value, "none") == 0) opt_anim_mode = 0;
        else if (strcmp(var.value, "push") == 0) opt_anim_mode = 2;
        else if (strcmp(var.value, "cover") == 0) opt_anim_mode = 3;
        else opt_anim_mode = 1; /* curl */
    }
    var.key = "ComicBook_gamma";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        gamma_val = atof(var.value);
    }
    var.key = "ComicBook_manga";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) opt_manga = (strcmp(var.value, "true") == 0);
    var.key = "ComicBook_double";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) opt_double = (strcmp(var.value, "true") == 0);
    var.key = "ComicBook_p1_right";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) opt_p1_right = (strcmp(var.value, "true") == 0);
    if (opt_double != old_double) {
        if (opt_double) {
            if (opt_p1_right) current_view_index = (current_view_index + 1) / 2;
            else current_view_index = current_view_index / 2;
        } else {
            if (opt_p1_right) current_view_index = (current_view_index == 0) ? 0 : (current_view_index * 2) - 1;
            else current_view_index = current_view_index * 2;
        }
        refresh();
    }
    if (resize) {
        if(opt_res == 4){ cur_width=1920; cur_height=1080; }
        else if(opt_res == 1){ cur_width=640; cur_height=480; }
        else { cur_width=1280; cur_height=960; }
        free(framebuffer);
        framebuffer = calloc(cur_width * cur_height, 2);
        free(anim_source_fb);
        free(anim_target_fb);
        anim_source_fb = malloc(cur_width * cur_height * 2);
        anim_target_fb = malloc(cur_width * cur_height * 2);
        struct retro_game_geometry geom = {
            .base_width = (unsigned)cur_width,
            .base_height = (unsigned)cur_height,
            .max_width = MAX_W,
            .max_height = MAX_H,
            .aspect_ratio = (float)cur_width / (float)cur_height
        };
        environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);
    }
    if (reindex) {
        rebuild_index();
        if (total_v_pages > 0 && current_view_index >= total_v_pages) {
            current_view_index = total_v_pages - 1;
        } else if (total_v_pages == 0) {
            current_view_index = 0;
        }
        refresh();
    }
}
/* Helper to render the current state to the global framebuffer immediately */
static void render_scene() {
    memset(framebuffer, 0, cur_width * cur_height * 2);
    if (!opt_double) {
        draw_to_fb(&img_l, 0, cur_width, false);
    } else {
        draw_to_fb(&img_l, 0, cur_width / 2, false);
        draw_to_fb(&img_r, cur_width / 2, cur_width / 2, false);
    }
}
void retro_run(void)
{
  
if (!vars_initialized) {
        update_vars();
        vars_initialized = true;
    }
    bool u;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &u) && u)
        update_vars();
    input_poll_cb();
    int next = opt_manga ? RETRO_DEVICE_ID_JOYPAD_LEFT
                         : RETRO_DEVICE_ID_JOYPAD_RIGHT;
    int prev = opt_manga ? RETRO_DEVICE_ID_JOYPAD_RIGHT
                         : RETRO_DEVICE_ID_JOYPAD_LEFT;
    /* --------------------------------------------------
       INPUT (only if not already animating)
    -------------------------------------------------- */
    if (!is_animating) {
        /* NEXT PAGE */
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, next) && !last_btns[0]) {
            if (current_view_index < total_v_pages - 1) {
                if (opt_anim_mode > 0) {
                    unsigned old_index = current_view_index;
                    unsigned new_index = old_index + 1;
                    /* 1. Capture OLD page (A) */
                    // Framebuffer currently holds the old page, so just copy it
                    memcpy(anim_source_fb, framebuffer, cur_width * cur_height * 2);
                    /* 2. Switch to NEW page (B) */
                    current_view_index = new_index;
                    refresh(); // Loads pixels from zip
                    render_scene(); // FIX: Actually draw pixels to framebuffer!
                   
                    /* 3. Capture NEW page (B) */
                    memcpy(anim_target_fb, framebuffer, cur_width * cur_height * 2);
                    /* 4. Start Animation */
                    start_page_turn(
                        anim_source_fb,
                        anim_target_fb,
                        cur_width,
                        cur_height,
                        true, // Next direction
                        opt_anim_mode
                    );
if (flip_sound_data) {
    flip_sound_pos = 0; // Reset to start
    play_flip = true; // Start playback
}
                } else {
                    current_view_index++;
                    refresh();
                }
            }
        }
        /* PREV PAGE */
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, prev) && !last_btns[1]) {
            if (current_view_index > 0) {
                if (opt_anim_mode > 0) {
                    unsigned old_index = current_view_index;
                    unsigned new_index = old_index - 1;
                    /* 1. Capture OLD page (A) */
                    memcpy(anim_source_fb, framebuffer, cur_width * cur_height * 2);
                    /* 2. Switch to NEW page (B) */
                    current_view_index = new_index;
                    refresh();
                    render_scene(); // FIX: Draw new page to FB
                   
                    /* 3. Capture NEW page (B) */
                    memcpy(anim_target_fb, framebuffer, cur_width * cur_height * 2);
                    /* 4. Start Animation */
                    start_page_turn(
                        anim_source_fb,
                        anim_target_fb,
                        cur_width,
                        cur_height,
                        false, // FIX: Set to false for Reverse direction
                        opt_anim_mode
                    );
if (flip_sound_data) {
    flip_sound_pos = 0; // Reset to start
    play_flip = true; // Start playback
}
                } else {
                    current_view_index--;
                    refresh();
                }
            }
        }
    }
    /* --------------------------------------------------
       OTHER CONTROLS
    -------------------------------------------------- */
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP) && !last_btns[4]) {
        if (total_v_pages > 0) {
            current_view_index = (current_view_index + 10 >= total_v_pages)
                               ? total_v_pages - 1
                               : current_view_index + 10;
            refresh();
        }
    }
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN) && !last_btns[5]) {
        if (total_v_pages > 0) {
            current_view_index = (current_view_index < 10)
                               ? 0
                               : current_view_index - 10;
            refresh();
        }
    }
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START) && !last_btns[6]) {
        current_view_index = 0;
        refresh();
    }
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y) && !last_btns[2])
        render_mode = (render_mode + 1) % 3;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X) && !last_btns[3])
        auto_contrast = !auto_contrast;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))
        sharpness += 0.1f;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B)) {
        sharpness -= 0.1f;
        if (sharpness < 0) sharpness = 0;
    }
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R))
        brightness += 2;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L))
        brightness -= 2;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2))
        contrast += 0.05f;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2))
        contrast -= 0.05f;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT)) {
        brightness = 0;
        contrast = 1.0f;
        sharpness = 0;
        auto_contrast = false;
    }
    last_btns[0] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, next);
    last_btns[1] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, prev);
    last_btns[2] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y);
    last_btns[3] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X);
    last_btns[4] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
    last_btns[5] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
    last_btns[6] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
    /* --------------------------------------------------
       RENDER FINAL OUTPUT
    -------------------------------------------------- */
    if (!is_animating) {
        // Use the new helper here too for consistency
        render_scene();
    }
    else {
        // If animating, draw the animation frame ON TOP of the framebuffer
        // (Note: render_animation writes directly to framebuffer)
        render_animation(framebuffer, cur_width, cur_height);
    }
    video_cb(framebuffer, cur_width, cur_height, cur_width * 2);
    if (play_flip && flip_sound_data) {
    size_t samples_to_send = 735 * 2; // Stereo
    if (flip_sound_pos + samples_to_send > flip_sound_samples) {
        samples_to_send = flip_sound_samples - flip_sound_pos;
        play_flip = false; // End of file reached
    }
   
    audio_batch_cb(&flip_sound_data[flip_sound_pos], samples_to_send / 2);
    flip_sound_pos += samples_to_send;
} else {
    // Send silence so RetroArch doesn't crackle
    int16_t silence[1470] = {0};
    audio_batch_cb(silence, 735);
}
}
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;

    static const struct retro_variable vars[] = {
        { "ComicBook_start", "Start at Page (0=Default); 0|1|2|3|4|5|6|7|8|9|10|15|20|25|30|40|50" },
        { "ComicBook_ignore_last", "Ignore Last X Pages; 0|1|2|3|4|5|6|7|8|9|10|15|20|25|30" },
        { "ComicBook_anim_mode", "Animation Mode; curl|push|cover|none" },
        { "ComicBook_res", "Resolution; 1280x960|1920x1080|640x480" },
        { "ComicBook_gamma", "Gamma; 1.0|0.5|0.6|0.7|0.8|0.9|1.1|1.2|1.3|1.4|1.5|1.6|1.7|1.8|1.9|2.0" },
        { "ComicBook_manga", "Manga Mode; false|true" },
        { "ComicBook_double", "Double Page Mode; false|true" },
        { "ComicBook_p1_right", "Page 1 Orientation Right; true|false" },
        { NULL, NULL },
    };
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
    cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}
/* --- Standard Libretro Boilerplate --- */
bool retro_load_game(const struct retro_game_info *info) {
    if(!info) return false;
    strncpy(archive_path, info->path, 1023);
   
    game_loaded = true;
  
    rebuild_index();
    refresh();
    return true;
}
void retro_init(void) {
    framebuffer = calloc(MAX_W * MAX_H, 2);
    init_animations(MAX_W, MAX_H);
    anim_target_fb = calloc(MAX_W * MAX_H, 2);
    anim_source_fb = malloc(MAX_W * MAX_H * 2); // Use MAX_W here for safety
    vars_initialized = false;
    load_flip_sound();
   
    // Initialize MuPDF context FIRST
    if (!pdf_ctx) {
        pdf_ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
        fz_register_document_handlers(pdf_ctx); // IMPORTANT: Register PDF/CBZ handlers
    }
 update_vars();
   
}
void retro_deinit(void) {
    if(img_l.pixels) free(img_l.pixels);
    if(img_r.pixels) free(img_r.pixels);
    if(framebuffer) free(framebuffer);
    if(anim_source_fb) free(anim_source_fb);
    if(anim_target_fb) free(anim_target_fb);
    if(flip_sound_data) free(flip_sound_data); // Free the sound too!
    img_l.pixels = img_r.pixels = NULL;
    framebuffer = anim_source_fb = anim_target_fb = flip_sound_data = NULL;
}
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
void retro_get_system_info(struct retro_system_info *i) { i->library_name="ComicBook"; i->library_version="6.5.0"; i->valid_extensions="cbz|zip|cbr|rar|pdf"; i->need_fullpath=true; }
void retro_get_system_av_info(struct retro_system_av_info *i) { 
    i->timing.fps = 60.0; 
    i->timing.sample_rate = 44100.0;
    i->geometry.base_width = cur_width;
    i->geometry.base_height = cur_height;
    i->geometry.max_width = MAX_W;
    i->geometry.max_height = MAX_H;
    i->geometry.aspect_ratio = (float)cur_width / (float)cur_height;
}
unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_reset(void) { current_view_index = 0; refresh(); }
size_t retro_serialize_size(void)
{
    return sizeof(SaveState);
}
bool retro_serialize(void *data, size_t size)
{
    if (size < sizeof(SaveState))
        return false;
    SaveState state = {
        .page_index = current_view_index,
        .render_mode = render_mode,
        .brightness = brightness,
        .contrast = contrast,
        .sharpness = sharpness,
        .gamma = gamma_val,
        .auto_contrast = auto_contrast,
        .manga_mode = opt_manga,
        .double_mode = opt_double
    };
    memcpy(data, &state, sizeof(SaveState));
    return true;
}
bool retro_unserialize(const void *data, size_t size)
{
    if (size < sizeof(SaveState))
        return false;
    SaveState state;
    memcpy(&state, data, sizeof(SaveState));
    // Restore the variables
    current_view_index = state.page_index;
    render_mode = state.render_mode;
    brightness = state.brightness;
    contrast = state.contrast;
    sharpness = state.sharpness;
    gamma_val = state.gamma;
    auto_contrast = state.auto_contrast;
    opt_manga = state.manga_mode;
    opt_double = state.double_mode;
    // Trigger a reload of the images and a redraw of the screen
    refresh();
    render_scene();
    return true;
}
void retro_unload_game(void) {
    game_loaded = false;
    vars_initialized = false; // CRITICAL: Reset this so the next book forces a fresh env-call
   
    if (is_pdf) close_pdf();
   
    for (unsigned i = 0; i < total_v_pages; i++) {
        if (v_pages[i].filename) {
            free(v_pages[i].filename);
            v_pages[i].filename = NULL;
        }
    }
    total_v_pages = 0;
    current_view_index = 0;
    if (img_l.pixels) { free(img_l.pixels); img_l.pixels = NULL; }
    if (img_r.pixels) { free(img_r.pixels); img_r.pixels = NULL; }
   
    play_flip = false;
    is_pdf = false;
}
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
void *retro_get_memory_data(unsigned id) { return NULL; }
size_t retro_get_memory_size(unsigned id) { return 0; }
void retro_set_controller_port_device(unsigned p, unsigned d) {}
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned i, bool e, const char *c) {}
bool retro_load_game_special(unsigned t, const struct retro_game_info *i, size_t n) { return false; }
void retro_set_audio_sample(retro_audio_sample_t cb) {}