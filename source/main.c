// switchmio - Native Stremio-style app for Nintendo Switch
// Catalog grid, tabs, details, neon UI, sound effects (existing),
// PLUS: Real-Debrid device-code login, Torrentio+Real-Debrid stream
// resolution, and native ffmpeg+SDL2 video/audio playback.
//
// DEBUG BUILD: logs every stage to sdmc:/switch/switchmio/log.txt

#include <switch.h>
#include <curl/curl.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_mixer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <math.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#define MOVIE_URL_FMT  "https://v3-cinemeta.strem.io/catalog/movie/top/skip=%d.json"
#define SERIES_URL_FMT "https://v3-cinemeta.strem.io/catalog/series/top/skip=%d.json"
#define PAGE_SIZE      100
#define MAX_ITEMS      600
#define FONT_SIZE      22
#define FONT_SIZE_SMALL 18
#define SCREEN_W       1280
#define SCREEN_H       720

#define GRID_COLS   7
#define TILE_W      140
#define TILE_H      210
#define LABEL_H     24
#define GAP_X       16
#define GAP_Y       16
#define GRID_TOP    110
#define FOOTER_H    50

#define TAB_ALL     0
#define TAB_MOVIE   1
#define TAB_SERIES  2
#define TAB_COUNT   3
static const char *TAB_NAMES[TAB_COUNT] = { "All", "Movies", "TV Series" };

#define STATE_GRID     0
#define STATE_DETAILS  1

#define AUDIO_FREQ 48000
#define SFX_MOVE   0
#define SFX_SELECT 1
#define SFX_BACK   2
#define SFX_TAB    3
#define SFX_COUNT  4

// Real-Debrid: uses the widely-used open-source client id (same one many
// third-party Debrid-linked apps use for the device-code flow).
#define RD_CLIENT_ID "X245A4XAIBGVM"
#define RD_TOKEN_PATH "sdmc:/switch/switchmio/rd_token.txt"

typedef struct {
    char id[128];
    char name[256];
    char type[16];
    char poster_url[300];
    char description[600];
} CatalogItem;

static CatalogItem g_items[MAX_ITEMS];
static int g_item_count = 0;

static SDL_Texture *g_poster_tex[MAX_ITEMS];
static int g_poster_state[MAX_ITEMS];
static int g_poster_load_frame[MAX_ITEMS];

static int g_filtered[MAX_ITEMS];
static int g_filtered_count = 0;
static int g_frame = 0;

typedef struct { Uint8 *buf; Mix_Chunk *chunk; } SoundFX;
static SoundFX g_sfx[SFX_COUNT];
static int g_audio_ok = 0;

// Real-Debrid auth state
static char g_rd_access_token[512] = {0};
static int g_rd_authed = 0;
static char g_rd_user_code[32] = {0};
static char g_rd_verify_url[128] = {0};
static char g_rd_device_code[256] = {0};
static int g_rd_poll_interval = 5;

// status line shown on screen during long blocking network/decoding operations
static char g_status_line[256] = {0};

typedef struct { char *data; size_t size; } MemBuf;

static FILE *g_log = NULL;
static void logmsg(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fprintf(g_log, "\n");
    fflush(g_log);
}

// ---------- synthesized sound effects ----------
static void make_tone(SoundFX *out, double f0, double f1, double dur_sec, double volume) {
    int n = (int)(dur_sec * AUDIO_FREQ);
    if (n < 1) n = 1;
    size_t bytes = (size_t)n * 2 * sizeof(Sint16);
    Sint16 *buf = (Sint16 *)malloc(bytes);
    for (int i = 0; i < n; i++) {
        double t = (double)i / n;
        double freq = f0 + (f1 - f0) * t;
        double phase = 2.0 * M_PI * freq * ((double)i / AUDIO_FREQ);
        double env = 1.0;
        if (t > 0.75) env = (1.0 - t) / 0.25;
        Sint16 sample = (Sint16)(sin(phase) * volume * 30000.0 * env);
        buf[i * 2 + 0] = sample;
        buf[i * 2 + 1] = sample;
    }
    out->buf = (Uint8 *)buf;
    out->chunk = Mix_QuickLoad_RAW(out->buf, (Uint32)bytes);
}
static void init_sfx(void) {
    make_tone(&g_sfx[SFX_MOVE],   700,  850, 0.05, 0.35);
    make_tone(&g_sfx[SFX_SELECT], 550, 1100, 0.14, 0.45);
    make_tone(&g_sfx[SFX_BACK],   750,  400, 0.14, 0.40);
    make_tone(&g_sfx[SFX_TAB],    800,  950, 0.07, 0.35);
}
static void free_sfx(void) {
    for (int i = 0; i < SFX_COUNT; i++) {
        if (g_sfx[i].chunk) Mix_FreeChunk(g_sfx[i].chunk);
        if (g_sfx[i].buf) free(g_sfx[i].buf);
    }
}
static void play_sfx(int idx) {
    if (!g_audio_ok || idx < 0 || idx >= SFX_COUNT || !g_sfx[idx].chunk) return;
    Mix_PlayChannel(-1, g_sfx[idx].chunk, 0);
}

// ---------- HTTP helpers ----------
static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemBuf *mem = (MemBuf *)userp;
    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0;
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;
    return realsize;
}

// generic request: method NULL/"" = GET, or "POST"; postfields NULL for none;
// extra_header NULL for none (e.g. "Authorization: Bearer xyz")
static CURLcode http_request(const char *url, const char *method, const char *postfields, const char *extra_header, MemBuf *out) {
    out->data = malloc(1);
    out->size = 0;
    if (out->data) out->data[0] = 0;

    CURL *curl = curl_easy_init();
    if (!curl) return CURLE_FAILED_INIT;

    struct curl_slist *headers = NULL;
    if (extra_header) headers = curl_slist_append(headers, extra_header);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)out);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "switchmio/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (method && strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postfields ? postfields : "");
    }

    CURLcode res = curl_easy_perform(curl);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return res;
}

static CURLcode fetch_url(const char *url, MemBuf *out) {
    return http_request(url, NULL, NULL, NULL, out);
}

// ---------- tiny JSON helpers (same targeted-extraction approach used throughout) ----------
static void copy_json_string(char *dest, size_t dest_size, const char *start, const char *end) {
    size_t di = 0; const char *p = start;
    while (p < end && di + 1 < dest_size) {
        if (*p == '\\' && p + 1 < end) {
            char n = *(p + 1);
            if (n == '/' || n == '"' || n == '\\') { dest[di++] = n; p += 2; continue; }
            if (n == 'n') { dest[di++] = ' '; p += 2; continue; }
        }
        dest[di++] = *p++;
    }
    dest[di] = 0;
}
static const char *find_json_string_field(const char *from, const char *key, size_t max_window, const char **out_end) {
    const char *window_end = from + max_window;
    const char *kp = strstr(from, key);
    if (!kp || kp > window_end) return NULL;
    kp += strlen(key);
    const char *vend = kp;
    while (*vend && *vend != '"') { if (*vend == '\\' && *(vend + 1)) vend++; vend++; }
    *out_end = vend;
    return kp;
}
// finds "key":NUMBER (no quotes) and returns it as long, or default_val if not found
static long find_json_number_field(const char *from, const char *key, long default_val) {
    const char *kp = strstr(from, key);
    if (!kp) return default_val;
    kp += strlen(key);
    while (*kp == ' ' || *kp == ':') kp++;
    return strtol(kp, NULL, 10);
}

// ---------- Cinemeta catalog (unchanged from previous version) ----------
static int extract_items(const char *json, const char *type_label) {
    int added = 0;
    const char *p = json;
    const char *id_key = "\"id\":\"";
    size_t id_keylen = strlen(id_key);
    while (g_item_count < MAX_ITEMS) {
        p = strstr(p, id_key);
        if (!p) break;
        p += id_keylen;
        const char *id_start = p, *id_end = p;
        while (*id_end && *id_end != '"') { if (*id_end == '\\' && *(id_end + 1)) id_end++; id_end++; }
        const char *name_end;
        const char *np = find_json_string_field(id_end, "\"name\":\"", 1000, &name_end);
        if (!np) { p = id_end; continue; }
        CatalogItem *item = &g_items[g_item_count];
        copy_json_string(item->id, sizeof(item->id), id_start, id_end);
        copy_json_string(item->name, sizeof(item->name), np, name_end);
        strncpy(item->type, type_label, sizeof(item->type) - 1);
        item->type[sizeof(item->type) - 1] = 0;
        item->poster_url[0] = 0; item->description[0] = 0;
        const char *poster_end;
        const char *pp = find_json_string_field(name_end, "\"poster\":\"", 3000, &poster_end);
        if (pp) copy_json_string(item->poster_url, sizeof(item->poster_url), pp, poster_end);
        const char *desc_end;
        const char *dp = find_json_string_field(name_end, "\"description\":\"", 4000, &desc_end);
        if (dp) copy_json_string(item->description, sizeof(item->description), dp, desc_end);
        const char *advance_to = name_end;
        if (pp && poster_end > advance_to) advance_to = poster_end;
        if (dp && desc_end > advance_to) advance_to = desc_end;
        p = advance_to;
        g_item_count++; added++;
    }
    return added;
}
static void fetch_full_catalog(const char *url_fmt, const char *type_label) {
    int skip = 0;
    while (g_item_count < MAX_ITEMS) {
        char url[256];
        snprintf(url, sizeof(url), url_fmt, skip);
        MemBuf chunk;
        CURLcode res = fetch_url(url, &chunk);
        if (res != CURLE_OK) { logmsg("fetch_url failed for %s : curl code %d", url, res); free(chunk.data); break; }
        int added = extract_items(chunk.data, type_label);
        logmsg("fetched %s skip=%d -> +%d items (total=%d)", type_label, skip, added, g_item_count);
        free(chunk.data);
        if (added == 0 || added < PAGE_SIZE) break;
        skip += PAGE_SIZE;
    }
}
static void rebuild_filtered(int tab) {
    g_filtered_count = 0;
    for (int i = 0; i < g_item_count; i++) {
        if (tab == TAB_ALL ||
            (tab == TAB_MOVIE && strcmp(g_items[i].type, "movie") == 0) ||
            (tab == TAB_SERIES && strcmp(g_items[i].type, "series") == 0)) {
            g_filtered[g_filtered_count++] = i;
        }
    }
}
static void load_poster(SDL_Renderer *ren, int idx) {
    if (idx < 0 || idx >= g_item_count) return;
    if (g_poster_state[idx] != 0) return;
    if (g_items[idx].poster_url[0] == 0) { g_poster_state[idx] = 1; return; }
    MemBuf chunk;
    CURLcode res = fetch_url(g_items[idx].poster_url, &chunk);
    if (res != CURLE_OK || chunk.size == 0) { free(chunk.data); g_poster_state[idx] = 1; return; }
    SDL_RWops *rw = SDL_RWFromMem(chunk.data, (int)chunk.size);
    SDL_Surface *surf = rw ? IMG_Load_RW(rw, 1) : NULL;
    free(chunk.data);
    if (!surf) { g_poster_state[idx] = 1; return; }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_FreeSurface(surf);
    if (!tex) { g_poster_state[idx] = 1; return; }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    g_poster_tex[idx] = tex;
    g_poster_load_frame[idx] = g_frame;
    g_poster_state[idx] = 2;
}

// ---------- text helpers ----------
static void draw_text(SDL_Renderer *ren, TTF_Font *font, const char *text, int x, int y, SDL_Color color) {
    if (!text || !*text || !font) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, color);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_Rect dst = { x, y, surf->w, surf->h };
    SDL_RenderCopy(ren, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
}
static void draw_text_clipped(SDL_Renderer *ren, TTF_Font *font, const char *text, int x, int y, int max_w, SDL_Color color) {
    if (!text || !*text || !font) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, color);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_Rect clip = { x, y, max_w, surf->h };
    SDL_RenderSetClipRect(ren, &clip);
    SDL_Rect dst = { x, y, surf->w, surf->h };
    SDL_RenderCopy(ren, tex, NULL, &dst);
    SDL_RenderSetClipRect(ren, NULL);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
}
static int draw_text_wrapped(SDL_Renderer *ren, TTF_Font *font, const char *text, int x, int y, int max_w, int line_h, int max_lines, SDL_Color color) {
    if (!text || !*text || !font) return 0;
    char line[512]; line[0] = 0;
    int lines_drawn = 0;
    const char *word_start = text;
    while (*word_start && lines_drawn < max_lines) {
        const char *word_end = word_start;
        while (*word_end && *word_end != ' ') word_end++;
        size_t wlen = (size_t)(word_end - word_start);
        char trial[512];
        size_t cur_len = strlen(line);
        snprintf(trial, sizeof(trial), "%s%s%.*s", line, cur_len ? " " : "", (int)wlen, word_start);
        int tw = 0, th = 0;
        TTF_SizeUTF8(font, trial, &tw, &th);
        if (tw > max_w && cur_len > 0) {
            draw_text(ren, font, line, x, y + lines_drawn * line_h, color);
            lines_drawn++; line[0] = 0; continue;
        }
        strncpy(line, trial, sizeof(line) - 1); line[sizeof(line) - 1] = 0;
        word_start = *word_end ? word_end + 1 : word_end;
    }
    if (line[0] && lines_drawn < max_lines) {
        draw_text(ren, font, line, x, y + lines_drawn * line_h, color);
        lines_drawn++;
    }
    return lines_drawn;
}
static void draw_poster_tile(SDL_Renderer *ren, int abs_i, SDL_Rect *rect) {
    if (g_poster_state[abs_i] == 2 && g_poster_tex[abs_i]) {
        int age = g_frame - g_poster_load_frame[abs_i];
        int alpha = age * 22; if (alpha > 255) alpha = 255; if (alpha < 0) alpha = 0;
        SDL_SetTextureAlphaMod(g_poster_tex[abs_i], (Uint8)alpha);
        SDL_RenderCopy(ren, g_poster_tex[abs_i], NULL, rect);
    }
}

// present a single status frame (used during blocking auth/resolve steps so
// the person sees progress instead of a frozen screen)
static void render_status_screen(SDL_Renderer *ren, TTF_Font *font, SDL_Color bg, SDL_Color fg, const char *line1, const char *line2) {
    SDL_SetRenderDrawColor(ren, bg.r, bg.g, bg.b, 255);
    SDL_RenderClear(ren);
    if (font) {
        draw_text(ren, font, "switchmio", 60, 60, fg);
        if (line1) draw_text(ren, font, line1, 60, 140, fg);
        if (line2) draw_text(ren, font, line2, 60, 180, fg);
    }
    SDL_RenderPresent(ren);
}

// ---------- Real-Debrid device-code auth ----------
static void rd_save_token(void) {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/switchmio", 0777);
    FILE *f = fopen(RD_TOKEN_PATH, "w");
    if (f) { fputs(g_rd_access_token, f); fclose(f); }
}
static void rd_load_token(void) {
    FILE *f = fopen(RD_TOKEN_PATH, "r");
    if (!f) return;
    if (fgets(g_rd_access_token, sizeof(g_rd_access_token), f)) {
        // strip trailing newline
        size_t l = strlen(g_rd_access_token);
        while (l > 0 && (g_rd_access_token[l-1] == '\n' || g_rd_access_token[l-1] == '\r')) g_rd_access_token[--l] = 0;
        if (g_rd_access_token[0]) g_rd_authed = 1;
    }
    fclose(f);
}

// Starts the device-code flow: gets a user_code + verification_url to show
// on screen. Returns 1 on success.
static int rd_start_device_code(void) {
    char url[256];
    snprintf(url, sizeof(url), "https://api.real-debrid.com/oauth/v2/device/code?client_id=%s&new_credentials=yes", RD_CLIENT_ID);
    MemBuf resp;
    CURLcode res = fetch_url(url, &resp);
    if (res != CURLE_OK) { logmsg("rd device/code failed: %d", res); free(resp.data); return 0; }

    const char *e;
    const char *dc = find_json_string_field(resp.data, "\"device_code\":\"", strlen(resp.data), &e);
    if (dc) copy_json_string(g_rd_device_code, sizeof(g_rd_device_code), dc, e);
    const char *uc = find_json_string_field(resp.data, "\"user_code\":\"", strlen(resp.data), &e);
    if (uc) copy_json_string(g_rd_user_code, sizeof(g_rd_user_code), uc, e);
    const char *vu = find_json_string_field(resp.data, "\"verification_url\":\"", strlen(resp.data), &e);
    if (vu) copy_json_string(g_rd_verify_url, sizeof(g_rd_verify_url), vu, e);
    g_rd_poll_interval = (int)find_json_number_field(resp.data, "\"interval\":", 5);

    logmsg("rd device/code: user_code=%s verify_url=%s interval=%d", g_rd_user_code, g_rd_verify_url, g_rd_poll_interval);
    free(resp.data);
    return g_rd_device_code[0] && g_rd_user_code[0];
}

// Polls until the user authorizes on their phone/PC, or times out.
// Returns 1 on success (g_rd_access_token filled + g_rd_authed=1).
static int rd_poll_for_auth(SDL_Renderer *ren, TTF_Font *font, PadState *pad, SDL_Color bg, SDL_Color fg, SDL_Color accent) {
    char cred_client_id[128] = {0}, cred_client_secret[256] = {0};
    int max_tries = 60; // ~ up to 5 min at 5s interval

    for (int tries = 0; tries < max_tries; tries++) {
        // render the code + instructions each iteration so the screen is alive
        char line1[128], line2[128];
        snprintf(line1, sizeof(line1), "Go to %s", g_rd_verify_url[0] ? g_rd_verify_url : "real-debrid.com/device");
        snprintf(line2, sizeof(line2), "Enter code: %s   (B to cancel)", g_rd_user_code);
        render_status_screen(ren, font, bg, fg, line1, line2);

        // allow cancel
        padUpdate(pad);
        u64 kDown = padGetButtonsDown(pad);
        if (kDown & HidNpadButton_B) { logmsg("rd auth cancelled by user"); return 0; }

        if (!cred_client_id[0]) {
            char url[300];
            snprintf(url, sizeof(url), "https://api.real-debrid.com/oauth/v2/device/credentials?client_id=%s&code=%s", RD_CLIENT_ID, g_rd_device_code);
            MemBuf resp;
            CURLcode res = fetch_url(url, &resp);
            if (res == CURLE_OK && resp.data) {
                const char *e;
                const char *cid = find_json_string_field(resp.data, "\"client_id\":\"", strlen(resp.data), &e);
                if (cid) copy_json_string(cred_client_id, sizeof(cred_client_id), cid, e);
                const char *cs = find_json_string_field(resp.data, "\"client_secret\":\"", strlen(resp.data), &e);
                if (cs) copy_json_string(cred_client_secret, sizeof(cred_client_secret), cs, e);
                logmsg("rd credentials poll try=%d got_client_id=%d", tries, cred_client_id[0] != 0);
            }
            free(resp.data);
        } else {
            // we have device-specific client_id/secret, exchange for access token
            char post[600];
            snprintf(post, sizeof(post),
                "client_id=%s&client_secret=%s&code=%s&grant_type=http://oauth.net/grant_type/device/1.0",
                cred_client_id, cred_client_secret, g_rd_device_code);
            MemBuf resp;
            CURLcode res = http_request("https://api.real-debrid.com/oauth/v2/token", "POST", post, NULL, &resp);
            if (res == CURLE_OK && resp.data) {
                const char *e;
                const char *at = find_json_string_field(resp.data, "\"access_token\":\"", strlen(resp.data), &e);
                if (at) {
                    copy_json_string(g_rd_access_token, sizeof(g_rd_access_token), at, e);
                    g_rd_authed = 1;
                    rd_save_token();
                    logmsg("rd auth SUCCESS");
                    free(resp.data);
                    return 1;
                }
                logmsg("rd token exchange try=%d: no access_token yet", tries);
            }
            free(resp.data);
        }

        svcSleepThread((u64)g_rd_poll_interval * 1000000000ULL);
    }
    logmsg("rd auth timed out after %d tries", max_tries);
    return 0;
}

// ---------- Torrentio + Real-Debrid stream resolution ----------
// Finds the first infoHash + fileIdx from a Torrentio streams.json response.
static int parse_first_stream(const char *json, char *info_hash_out, size_t hash_sz, int *file_idx_out) {
    const char *e;
    const char *hp = find_json_string_field(json, "\"infoHash\":\"", strlen(json), &e);
    if (!hp) return 0;
    copy_json_string(info_hash_out, hash_sz, hp, e);
    *file_idx_out = (int)find_json_number_field(hp, "\"fileIdx\":", -1);
    return 1;
}

// Full resolve pipeline for one catalog item. Returns 1 and fills out_url on
// success. Renders status updates on screen throughout since this can take
// a while (torrent caching, RD processing).
static int resolve_stream_url(SDL_Renderer *ren, TTF_Font *font, SDL_Color bg, SDL_Color fg,
                               const CatalogItem *item, char *out_url, size_t out_url_sz) {
    render_status_screen(ren, font, bg, fg, "Searching sources...", item->name);

    char torrentio_url[256];
    snprintf(torrentio_url, sizeof(torrentio_url), "https://torrentio.strem.fun/stream/%s/%s.json",
             strcmp(item->type, "series") == 0 ? "series" : "movie", item->id);

    MemBuf resp;
    CURLcode res = fetch_url(torrentio_url, &resp);
    if (res != CURLE_OK) { logmsg("torrentio fetch failed: %d", res); free(resp.data); return 0; }

    char info_hash[64] = {0};
    int file_idx = -1;
    int found = parse_first_stream(resp.data, info_hash, sizeof(info_hash), &file_idx);
    free(resp.data);
    if (!found) { logmsg("no torrentio streams found for %s", item->id); return 0; }
    logmsg("torrentio: infoHash=%s fileIdx=%d", info_hash, file_idx);

    char auth_header[560];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", g_rd_access_token);

    render_status_screen(ren, font, bg, fg, "Adding to Real-Debrid...", item->name);
    char magnet_post[128];
    snprintf(magnet_post, sizeof(magnet_post), "magnet=magnet:?xt=urn:btih:%s", info_hash);
    MemBuf add_resp;
    res = http_request("https://api.real-debrid.com/rest/1.0/torrents/addMagnet", "POST", magnet_post, auth_header, &add_resp);
    if (res != CURLE_OK) { logmsg("rd addMagnet failed: %d", res); free(add_resp.data); return 0; }

    char torrent_id[128] = {0};
    { const char *e; const char *idp = find_json_string_field(add_resp.data, "\"id\":\"", strlen(add_resp.data), &e);
      if (idp) copy_json_string(torrent_id, sizeof(torrent_id), idp, e); }
    free(add_resp.data);
    if (!torrent_id[0]) { logmsg("rd addMagnet: no torrent id in response"); return 0; }
    logmsg("rd torrent id=%s", torrent_id);

    render_status_screen(ren, font, bg, fg, "Selecting files...", item->name);
    char select_url[200];
    snprintf(select_url, sizeof(select_url), "https://api.real-debrid.com/rest/1.0/torrents/selectFiles/%s", torrent_id);
    MemBuf sel_resp;
    http_request(select_url, "POST", "files=all", auth_header, &sel_resp);
    free(sel_resp.data);

    render_status_screen(ren, font, bg, fg, "Waiting for Real-Debrid cache...", item->name);
    char info_url[200];
    snprintf(info_url, sizeof(info_url), "https://api.real-debrid.com/rest/1.0/torrents/info/%s", torrent_id);

    char link[400] = {0};
    for (int tries = 0; tries < 20; tries++) {
        MemBuf info_resp;
        res = http_request(info_url, NULL, NULL, auth_header, &info_resp);
        if (res == CURLE_OK && info_resp.data) {
            char status[32] = {0};
            const char *e;
            const char *sp = find_json_string_field(info_resp.data, "\"status\":\"", strlen(info_resp.data), &e);
            if (sp) copy_json_string(status, sizeof(status), sp, e);
            logmsg("rd torrent status try=%d: %s", tries, status);

            if (strcmp(status, "downloaded") == 0) {
                const char *lp = find_json_string_field(info_resp.data, "\"links\":[\"", strlen(info_resp.data), &e);
                if (lp) copy_json_string(link, sizeof(link), lp, e);
                free(info_resp.data);
                break;
            }
            free(info_resp.data);
        } else {
            free(info_resp.data);
        }
        svcSleepThread(1500000000ULL); // 1.5s
    }

    if (!link[0]) { logmsg("rd: no link after waiting (not cached, or took too long)"); return 0; }
    logmsg("rd hoster link=%s", link);

    render_status_screen(ren, font, bg, fg, "Unlocking stream link...", item->name);
    char unrestrict_post[500];
    snprintf(unrestrict_post, sizeof(unrestrict_post), "link=%s", link);
    MemBuf unres_resp;
    res = http_request("https://api.real-debrid.com/rest/1.0/unrestrict/link", "POST", unrestrict_post, auth_header, &unres_resp);
    if (res != CURLE_OK) { logmsg("rd unrestrict failed: %d", res); free(unres_resp.data); return 0; }

    { const char *e; const char *dp = find_json_string_field(unres_resp.data, "\"download\":\"", strlen(unres_resp.data), &e);
      if (dp) copy_json_string(out_url, out_url_sz, dp, e); }
    free(unres_resp.data);

    if (!out_url[0]) { logmsg("rd unrestrict: no download url in response"); return 0; }
    logmsg("resolved final stream url: %s", out_url);
    return 1;
}

// ---------- ffmpeg + SDL2 native video/audio playback ----------
// Minimal blocking player: decodes and renders until EOF or the user
// presses B/+. Uses SDL_QueueAudio for audio, SDL YUV texture for video.
static void play_video(SDL_Renderer *ren, PadState *pad, const char *url) {
    logmsg("play_video: opening %s", url);

    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, url, NULL, NULL) != 0) {
        logmsg("play_video: avformat_open_input failed");
        return;
    }
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        logmsg("play_video: avformat_find_stream_info failed");
        avformat_close_input(&fmt_ctx);
        return;
    }

    int video_stream = -1, audio_stream = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream < 0) video_stream = i;
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream < 0) audio_stream = i;
    }
    logmsg("play_video: video_stream=%d audio_stream=%d", video_stream, audio_stream);
    if (video_stream < 0) {
        logmsg("play_video: no video stream found, aborting");
        avformat_close_input(&fmt_ctx);
        return;
    }

    AVCodecParameters *vpar = fmt_ctx->streams[video_stream]->codecpar;
    const AVCodec *vcodec = avcodec_find_decoder(vpar->codec_id);
    AVCodecContext *vctx = avcodec_alloc_context3(vcodec);
    avcodec_parameters_to_context(vctx, vpar);
    if (!vcodec || avcodec_open2(vctx, vcodec, NULL) < 0) {
        logmsg("play_video: failed to open video codec");
        avformat_close_input(&fmt_ctx);
        return;
    }

    AVCodecContext *actx = NULL;
    SwrContext *swr = NULL;
    SDL_AudioDeviceID audio_dev = 0;
    if (audio_stream >= 0) {
        AVCodecParameters *apar = fmt_ctx->streams[audio_stream]->codecpar;
        const AVCodec *acodec = avcodec_find_decoder(apar->codec_id);
        if (acodec) {
            actx = avcodec_alloc_context3(acodec);
            avcodec_parameters_to_context(actx, apar);
            if (avcodec_open2(actx, acodec, NULL) == 0) {
                swr = swr_alloc();
                av_opt_set_chlayout(swr, "in_chlayout", &actx->ch_layout, 0);
                av_opt_set_int(swr, "in_sample_rate", actx->sample_rate, 0);
                av_opt_set_sample_fmt(swr, "in_sample_fmt", actx->sample_fmt, 0);
                AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
                av_opt_set_chlayout(swr, "out_chlayout", &out_layout, 0);
                av_opt_set_int(swr, "out_sample_rate", AUDIO_FREQ, 0);
                av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
                swr_init(swr);

                SDL_AudioSpec want = {0};
                want.freq = AUDIO_FREQ;
                want.format = AUDIO_S16SYS;
                want.channels = 2;
                want.samples = 1024;
                audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
                if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);
                logmsg("play_video: audio decoder+device ready (dev=%u)", (unsigned)audio_dev);
            } else {
                logmsg("play_video: failed to open audio codec, continuing video-only");
            }
        }
    }

    struct SwsContext *sws = sws_getContext(
        vctx->width, vctx->height, vctx->pix_fmt,
        vctx->width, vctx->height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, NULL, NULL, NULL);

    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, vctx->width, vctx->height);

    AVFrame *frame = av_frame_alloc();
    AVFrame *yuv_frame = av_frame_alloc();
    yuv_frame->format = AV_PIX_FMT_YUV420P;
    yuv_frame->width = vctx->width;
    yuv_frame->height = vctx->height;
    av_frame_get_buffer(yuv_frame, 32);

    AVPacket *pkt = av_packet_alloc();

    int running = 1;
    logmsg("play_video: entering decode loop (%dx%d)", vctx->width, vctx->height);

    while (running && appletMainLoop()) {
        padUpdate(pad);
        u64 kDown = padGetButtonsDown(pad);
        if (kDown & (HidNpadButton_B | HidNpadButton_Plus)) { logmsg("play_video: stopped by user"); break; }

        int read_ret = av_read_frame(fmt_ctx, pkt);
        if (read_ret < 0) { logmsg("play_video: end of stream"); break; }

        if (pkt->stream_index == video_stream) {
            if (avcodec_send_packet(vctx, pkt) == 0) {
                while (avcodec_receive_frame(vctx, frame) == 0) {
                    sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, vctx->height, yuv_frame->data, yuv_frame->linesize);
                    SDL_UpdateYUVTexture(tex, NULL,
                        yuv_frame->data[0], yuv_frame->linesize[0],
                        yuv_frame->data[1], yuv_frame->linesize[1],
                        yuv_frame->data[2], yuv_frame->linesize[2]);
                    SDL_RenderClear(ren);
                    SDL_RenderCopy(ren, tex, NULL, NULL);
                    SDL_RenderPresent(ren);
                }
            }
        } else if (audio_stream >= 0 && pkt->stream_index == audio_stream && actx && swr && audio_dev) {
            if (avcodec_send_packet(actx, pkt) == 0) {
                while (avcodec_receive_frame(actx, frame) == 0) {
                    uint8_t *out_buf = NULL;
                    int out_samples = av_rescale_rnd(swr_get_delay(swr, actx->sample_rate) + frame->nb_samples, AUDIO_FREQ, actx->sample_rate, AV_ROUND_UP);
                    av_samples_alloc(&out_buf, NULL, 2, out_samples, AV_SAMPLE_FMT_S16, 0);
                    int converted = swr_convert(swr, &out_buf, out_samples, (const uint8_t **)frame->data, frame->nb_samples);
                    if (converted > 0) {
                        int bytes = converted * 2 * sizeof(Sint16);
                        SDL_QueueAudio(audio_dev, out_buf, bytes);
                    }
                    av_freep(&out_buf);
                }
            }
        }

        av_packet_unref(pkt);
    }

    logmsg("play_video: cleaning up");
    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_frame_free(&yuv_frame);
    sws_freeContext(sws);
    SDL_DestroyTexture(tex);
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    if (swr) swr_free(&swr);
    if (actx) avcodec_free_context(&actx);
    avcodec_free_context(&vctx);
    avformat_close_input(&fmt_ctx);
}

int main(int argc, char *argv[]) {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/switchmio", 0777);
    g_log = fopen("sdmc:/switch/switchmio/log.txt", "w");
    logmsg("=== switchmio (full build: catalog+RD+playback) starting ===");

    Result rc_romfs = romfsInit();
    logmsg("romfsInit: %s (0x%x)", R_FAILED(rc_romfs) ? "FAILED" : "ok", rc_romfs);

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    Result rc = socketInitializeDefault();
    logmsg("socketInitializeDefault: %s (0x%x)", R_FAILED(rc) ? "FAILED" : "ok", rc);
    if (R_FAILED(rc)) { if (!R_FAILED(rc_romfs)) romfsExit(); if (g_log) fclose(g_log); return 1; }
    curl_global_init(CURL_GLOBAL_DEFAULT);

    av_log_set_level(AV_LOG_WARNING);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_AUDIO) != 0) {
        logmsg("SDL_Init FAILED: %s", SDL_GetError());
        curl_global_cleanup(); socketExit(); romfsExit(); if (g_log) fclose(g_log); return 1;
    }
    if (TTF_Init() != 0) { logmsg("TTF_Init FAILED"); SDL_Quit(); curl_global_cleanup(); socketExit(); romfsExit(); if (g_log) fclose(g_log); return 1; }

    int img_flags = IMG_INIT_JPG | IMG_INIT_PNG;
    IMG_Init(img_flags);

    if (Mix_OpenAudio(AUDIO_FREQ, AUDIO_S16SYS, 2, 1024) == 0) {
        Mix_AllocateChannels(8);
        init_sfx();
        g_audio_ok = 1;
    } else {
        logmsg("Mix_OpenAudio failed, sfx disabled: %s", Mix_GetError());
    }

    SDL_Window *win = SDL_CreateWindow("switchmio", 0, 0, SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    TTF_Font *font = TTF_OpenFont("romfs:/font.ttf", FONT_SIZE);
    if (!font) font = TTF_OpenFont("sdmc:/switch/switchmio/font.ttf", FONT_SIZE);
    TTF_Font *font_small = TTF_OpenFont("romfs:/font.ttf", FONT_SIZE_SMALL);
    if (!font_small) font_small = TTF_OpenFont("sdmc:/switch/switchmio/font.ttf", FONT_SIZE_SMALL);

    SDL_Color white = {255,255,255,255}, gray = {160,160,160,255}, dim_gray = {45,45,52,255};
    SDL_Color neon_blue = {10,185,230,255}, neon_red = {255,60,45,255}, tab_off = {90,90,100,255}, bg_color = {18,18,24,255};

    if (!win || !ren) {
        logmsg("Aborting: no window/renderer");
        if (g_audio_ok) { free_sfx(); Mix_CloseAudio(); }
        if (font) TTF_CloseFont(font); if (font_small) TTF_CloseFont(font_small);
        TTF_Quit(); IMG_Quit();
        if (ren) SDL_DestroyRenderer(ren); if (win) SDL_DestroyWindow(win);
        SDL_Quit(); curl_global_cleanup(); socketExit(); romfsExit(); if (g_log) fclose(g_log);
        return 1;
    }

    rd_load_token();
    logmsg("rd_load_token: authed=%d", g_rd_authed);

    render_status_screen(ren, font, bg_color, white, "Loading movies + series from Cinemeta...", NULL);
    fetch_full_catalog(MOVIE_URL_FMT, "movie");
    fetch_full_catalog(SERIES_URL_FMT, "series");
    logmsg("Catalog fetch done, total items=%d", g_item_count);

    int tab = TAB_ALL;
    rebuild_filtered(tab);

    int cursor = 0, scroll_row = 0, running = 1;
    int show_account_popup = 0;
    int app_state = STATE_GRID;
    int details_idx = -1;

    int margin_x = (SCREEN_W - (GRID_COLS * TILE_W + (GRID_COLS - 1) * GAP_X)) / 2;
    int row_stride = TILE_H + LABEL_H + GAP_Y;
    int rows_visible = (SCREEN_H - GRID_TOP - FOOTER_H) / row_stride;
    if (rows_visible < 1) rows_visible = 1;

    float anim_x = (float)margin_x, anim_y = (float)GRID_TOP;
    int anim_inited = 0;

    while (running && appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_Plus) running = 0;

        if (show_account_popup) {
            if (kDown & HidNpadButton_A && !g_rd_authed) {
                // start login flow (blocking sequence with its own screen updates)
                if (rd_start_device_code()) {
                    rd_poll_for_auth(ren, font, &pad, bg_color, white, neon_blue);
                }
                show_account_popup = 0;
            } else if (kDown & (HidNpadButton_B | HidNpadButton_Y)) {
                show_account_popup = 0;
                play_sfx(SFX_BACK);
            }
        } else if (app_state == STATE_DETAILS) {
            if (kDown & HidNpadButton_B) { app_state = STATE_GRID; play_sfx(SFX_BACK); }
            if (kDown & HidNpadButton_A && details_idx >= 0) {
                if (!g_rd_authed) {
                    render_status_screen(ren, font, bg_color, neon_red, "Sign in with Real-Debrid first (press Y).", NULL);
                    svcSleepThread(2000000000ULL);
                } else {
                    char stream_url[600] = {0};
                    if (resolve_stream_url(ren, font, bg_color, white, &g_items[details_idx], stream_url, sizeof(stream_url))) {
                        play_video(ren, &pad, stream_url);
                    } else {
                        render_status_screen(ren, font, bg_color, neon_red, "Could not find a playable source.", "Try another title.");
                        svcSleepThread(2000000000ULL);
                    }
                }
            }
        } else {
            if (kDown & HidNpadButton_Y) { show_account_popup = 1; play_sfx(SFX_TAB); }
            if (kDown & HidNpadButton_R) { tab = (tab + 1) % TAB_COUNT; rebuild_filtered(tab); cursor = 0; scroll_row = 0; play_sfx(SFX_TAB); }
            if (kDown & HidNpadButton_L) { tab = (tab - 1 + TAB_COUNT) % TAB_COUNT; rebuild_filtered(tab); cursor = 0; scroll_row = 0; play_sfx(SFX_TAB); }

            if (g_filtered_count > 0) {
                int total_rows = (g_filtered_count + GRID_COLS - 1) / GRID_COLS;
                int old_cursor = cursor;
                if ((kDown & HidNpadButton_Down) && cursor + GRID_COLS < g_filtered_count) cursor += GRID_COLS;
                if ((kDown & HidNpadButton_Up) && cursor - GRID_COLS >= 0) cursor -= GRID_COLS;
                if ((kDown & HidNpadButton_Right) && (cursor % GRID_COLS) != GRID_COLS - 1 && cursor + 1 < g_filtered_count) cursor++;
                if ((kDown & HidNpadButton_Left) && (cursor % GRID_COLS) != 0 && cursor > 0) cursor--;
                if (cursor != old_cursor) play_sfx(SFX_MOVE);
                if (kDown & HidNpadButton_A) { details_idx = g_filtered[cursor]; app_state = STATE_DETAILS; play_sfx(SFX_SELECT); }

                int cursor_row = cursor / GRID_COLS;
                if (cursor_row < scroll_row) scroll_row = cursor_row;
                if (cursor_row >= scroll_row + rows_visible) scroll_row = cursor_row - rows_visible + 1;
                if (scroll_row < 0) scroll_row = 0;
                if (scroll_row > total_rows - rows_visible) scroll_row = total_rows - rows_visible < 0 ? 0 : total_rows - rows_visible;
            }
        }

        if (app_state == STATE_DETAILS) {
            if (details_idx >= 0) load_poster(ren, details_idx);
        } else if (g_filtered_count > 0) {
            int start_local = scroll_row * GRID_COLS;
            int end_local = start_local + rows_visible * GRID_COLS;
            if (end_local > g_filtered_count) end_local = g_filtered_count;
            for (int li = start_local; li < end_local; li++) {
                int abs_i = g_filtered[li];
                if (g_poster_state[abs_i] == 0) { load_poster(ren, abs_i); break; }
            }
        }

        SDL_SetRenderDrawColor(ren, bg_color.r, bg_color.g, bg_color.b, 255);
        SDL_RenderClear(ren);

        if (app_state == STATE_DETAILS && details_idx >= 0) {
            CatalogItem *it = &g_items[details_idx];
            int pw = 300, ph = 450, px = margin_x, py = 90;
            SDL_Rect prect = { px, py, pw, ph };
            if (g_poster_state[details_idx] == 2 && g_poster_tex[details_idx]) {
                SDL_SetTextureAlphaMod(g_poster_tex[details_idx], 255);
                SDL_RenderCopy(ren, g_poster_tex[details_idx], NULL, &prect);
            } else {
                SDL_SetRenderDrawColor(ren, dim_gray.r, dim_gray.g, dim_gray.b, 255);
                SDL_RenderFillRect(ren, &prect);
            }
            SDL_SetRenderDrawColor(ren, neon_blue.r, neon_blue.g, neon_blue.b, 255);
            SDL_RenderDrawRect(ren, &prect);

            int text_x = px + pw + 50, text_w = SCREEN_W - text_x - margin_x;
            if (font) {
                draw_text(ren, font, it->name, text_x, py, white);
                draw_text(ren, font, strcmp(it->type, "movie") == 0 ? "Movie" : "TV Series", text_x, py + 40, neon_blue);
            }
            if (font_small) draw_text_wrapped(ren, font_small, it->description[0] ? it->description : "No description available.", text_x, py + 90, text_w, 26, 10, gray);

            int bw = 260, bh = 56, bx = text_x, by = py + ph - bh;
            SDL_SetRenderDrawColor(ren, neon_red.r, neon_red.g, neon_red.b, 255);
            SDL_Rect btn = { bx, by, bw, bh };
            SDL_RenderFillRect(ren, &btn);
            if (font) draw_text(ren, font, g_rd_authed ? "A: Play" : "A: Play (sign in first, Y)", bx + 14, by + 14, white);

            if (font) {
                SDL_SetRenderDrawColor(ren, 25,25,32,255);
                SDL_Rect footer = { 0, SCREEN_H - FOOTER_H, SCREEN_W, FOOTER_H };
                SDL_RenderFillRect(ren, &footer);
                draw_text(ren, font, "B: back   A: play", margin_x, SCREEN_H - FOOTER_H + 12, gray);
            }
        } else {
            if (font) {
                draw_text(ren, font, "switchmio", margin_x, 20, white);
                int tab_x = margin_x + 180;
                for (int t = 0; t < TAB_COUNT; t++) {
                    SDL_Color c = (t == tab) ? neon_blue : tab_off;
                    draw_text(ren, font, TAB_NAMES[t], tab_x, 20, c);
                    int tw=0, th=0; TTF_SizeUTF8(font, TAB_NAMES[t], &tw, &th);
                    tab_x += tw + 30;
                }
                draw_text(ren, font, g_rd_authed ? "Account: Real-Debrid linked" : "Account: Not signed in", SCREEN_W - margin_x - 300, 20, g_rd_authed ? neon_blue : neon_red);
            }
            if (font_small) {
                char count_line[64]; snprintf(count_line, sizeof(count_line), "%d titles", g_filtered_count);
                draw_text(ren, font_small, count_line, margin_x, 58, gray);
            }

            if (g_filtered_count == 0) {
                if (font) draw_text(ren, font, "No titles in this tab.", margin_x, 130, white);
                anim_inited = 0;
            } else {
                int start_local = scroll_row * GRID_COLS;
                int end_local = start_local + rows_visible * GRID_COLS;
                if (end_local > g_filtered_count) end_local = g_filtered_count;

                int cur_local = cursor - start_local;
                float target_x = (float)(margin_x + (cur_local % GRID_COLS) * (TILE_W + GAP_X));
                float target_y = (float)(GRID_TOP + (cur_local / GRID_COLS) * row_stride);
                if (!anim_inited) { anim_x = target_x; anim_y = target_y; anim_inited = 1; }
                else { anim_x += (target_x - anim_x) * 0.35f; anim_y += (target_y - anim_y) * 0.35f; }

                for (int li = start_local; li < end_local; li++) {
                    int abs_i = g_filtered[li];
                    int col = (li - start_local) % GRID_COLS, row = (li - start_local) / GRID_COLS;
                    int x = margin_x + col * (TILE_W + GAP_X), y = GRID_TOP + row * row_stride;
                    SDL_Rect poster_rect = { x, y, TILE_W, TILE_H };
                    if (g_poster_state[abs_i] == 2 && g_poster_tex[abs_i]) draw_poster_tile(ren, abs_i, &poster_rect);
                    else {
                        SDL_SetRenderDrawColor(ren, dim_gray.r, dim_gray.g, dim_gray.b, 255);
                        SDL_RenderFillRect(ren, &poster_rect);
                        if (g_poster_state[abs_i] == 1 && font) draw_text_clipped(ren, font, "no image", x+8, y+TILE_H/2-10, TILE_W-16, gray);
                    }
                    if (font) draw_text_clipped(ren, font, g_items[abs_i].name, x, y + TILE_H + 4, TILE_W, li == cursor ? white : gray);
                }

                int hx = (int)(anim_x + 0.5f), hy = (int)(anim_y + 0.5f);
                SDL_Color ring_colors[3] = { neon_blue, neon_red, neon_blue };
                for (int b = 0; b < 3; b++) {
                    SDL_Rect ring = { hx-4-b, hy-4-b, TILE_W+8+b*2, TILE_H+8+b*2 };
                    SDL_SetRenderDrawColor(ren, ring_colors[b].r, ring_colors[b].g, ring_colors[b].b, 255);
                    SDL_RenderDrawRect(ren, &ring);
                }
            }

            if (font) {
                SDL_SetRenderDrawColor(ren, 25,25,32,255);
                SDL_Rect footer = { 0, SCREEN_H - FOOTER_H, SCREEN_W, FOOTER_H };
                SDL_RenderFillRect(ren, &footer);
                draw_text(ren, font, "D-Pad: move   A: details   L/R: tabs   Y: account   +: exit", margin_x, SCREEN_H - FOOTER_H + 12, gray);
            }
        }

        if (show_account_popup && font) {
            int pw = 560, ph = 220, px = (SCREEN_W-pw)/2, py = (SCREEN_H-ph)/2;
            SDL_SetRenderDrawColor(ren, 30,30,40,255);
            SDL_Rect box = { px, py, pw, ph };
            SDL_RenderFillRect(ren, &box);
            SDL_SetRenderDrawColor(ren, neon_blue.r, neon_blue.g, neon_blue.b, 255);
            SDL_RenderDrawRect(ren, &box);
            draw_text(ren, font, "Real-Debrid account", px+24, py+24, white);
            if (g_rd_authed) {
                draw_text(ren, font, "Already signed in.", px+24, py+64, gray);
            } else {
                draw_text(ren, font, "A: start sign-in (device code)", px+24, py+64, gray);
            }
            draw_text(ren, font, "B: close", px+24, py+ph-40, gray);
        }

        SDL_RenderPresent(ren);
        g_frame++;
    }

    logmsg("Exiting, total frames=%d", g_frame);
    for (int i = 0; i < g_item_count; i++) if (g_poster_tex[i]) SDL_DestroyTexture(g_poster_tex[i]);
    if (g_audio_ok) { free_sfx(); Mix_CloseAudio(); }
    if (font) TTF_CloseFont(font); if (font_small) TTF_CloseFont(font_small);
    TTF_Quit(); IMG_Quit();
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win);
    SDL_Quit(); curl_global_cleanup(); socketExit(); romfsExit();
    if (g_log) fclose(g_log);
    return 0;
}
