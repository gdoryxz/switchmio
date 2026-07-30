// switchmio - Native Stremio catalog browser for Nintendo Switch
// Poster grid UI, Movies/TV tabs, details screen, smooth animation,
// Switch neon red/blue theme, synthesized UI sound effects,
// Real-Debrid device-code login with SD-card token persistence.
// DEBUG BUILD: logs every init step to sdmc:/switch/switchmio/log.txt

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
#include <sys/stat.h>
#include <math.h>

#define MOVIE_URL_FMT  "https://v3-cinemeta.strem.io/catalog/movie/top/skip=%d.json"
#define SERIES_URL_FMT "https://v3-cinemeta.strem.io/catalog/series/top/skip=%d.json"
#define PAGE_SIZE      100
#define MAX_ITEMS      600
#define FONT_SIZE      22
#define FONT_SIZE_SMALL 18
#define SCREEN_W       1280
#define SCREEN_H       720

// grid layout
#define GRID_COLS   7
#define TILE_W      140
#define TILE_H      210
#define LABEL_H     24
#define GAP_X       16
#define GAP_Y       16
#define GRID_TOP    110
#define FOOTER_H    50

// tabs
#define TAB_ALL     0
#define TAB_MOVIE   1
#define TAB_SERIES  2
#define TAB_COUNT   3
static const char *TAB_NAMES[TAB_COUNT] = { "All", "Movies", "TV Series" };

// app states
#define STATE_GRID     0
#define STATE_DETAILS  1

// audio
#define AUDIO_FREQ      48000
#define SFX_MOVE   0
#define SFX_SELECT 1
#define SFX_BACK   2
#define SFX_TAB    3
#define SFX_COUNT  4

// ---------- Real-Debrid device-code login ----------
// X245A4XAIBGVM is Real-Debrid's own published "open source app" client id,
// meant for exactly this use case (same one other open-source debrid clients
// use) - it is not a secret and is safe to hardcode.
#define RD_CLIENT_ID_OPENSOURCE "X245A4XAIBGVM"
#define RD_TOKEN_PATH "sdmc:/switch/switchmio/rd_token.txt"

#define RD_LOGGED_OUT     0
#define RD_FETCHING_CODE  1
#define RD_AWAITING_AUTH  2
#define RD_LOGGED_IN      3
#define RD_ERROR          4

static int  g_rd_state = RD_LOGGED_OUT;
static char g_rd_device_code[128]      = {0};
static char g_rd_user_code[32]         = {0};
static char g_rd_verification_url[128] = {0};
static int  g_rd_interval    = 5;
static int  g_rd_expires_in  = 1800;
static int  g_rd_login_start_frame = 0;
static int  g_rd_last_poll_frame   = 0;

static char g_rd_client_id[64]      = {0}; // per-user creds from device/credentials
static char g_rd_client_secret[80]  = {0};
static char g_rd_access_token[160]  = {0};
static char g_rd_refresh_token[160] = {0};
static char g_rd_username[64]       = {0};
static char g_rd_error_msg[160]     = {0};
// ----------------------------------------------------

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
static int g_poster_state[MAX_ITEMS];      // 0 = not tried, 1 = failed, 2 = loaded
static int g_poster_load_frame[MAX_ITEMS]; // frame number when it finished loading, for fade-in

// cached title-label textures (gray, non-selected state) - rendered once per
// item instead of every frame; big cost saver since ~49 tiles are visible at once
static SDL_Texture *g_title_tex[MAX_ITEMS];

// indices into g_items matching the current tab filter, rebuilt when tab changes
static int g_filtered[MAX_ITEMS];
static int g_filtered_count = 0;

static int g_frame = 0; // global frame counter, used for poster fade-in + RD polling timing

// synthesized sound effects
typedef struct {
    Uint8 *buf;
    Mix_Chunk *chunk;
} SoundFX;
static SoundFX g_sfx[SFX_COUNT];
static int g_audio_ok = 0;

typedef struct {
    char *data;
    size_t size;
} MemBuf;

// ---------- debug log to SD card ----------
static FILE *g_log = NULL;

static void logmsg(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fprintf(g_log, "\n");
    fflush(g_log);
}
// -------------------------------------------

// ---------- synthesized sound effects (original tones, no external audio files) ----------
static void make_tone(SoundFX *out, double f0, double f1, double dur_sec, double volume) {
    int n = (int)(dur_sec * AUDIO_FREQ);
    if (n < 1) n = 1;
    size_t bytes = (size_t)n * 2 /*channels*/ * sizeof(Sint16);
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
    if (!g_audio_ok) return;
    if (idx < 0 || idx >= SFX_COUNT || !g_sfx[idx].chunk) return;
    Mix_PlayChannel(-1, g_sfx[idx].chunk, 0);
}
// -------------------------------------------------------------------------------------------

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

static CURLcode fetch_url(const char *url, MemBuf *out) {
    out->data = malloc(1);
    out->size = 0;

    CURL *curl = curl_easy_init();
    if (!curl) return CURLE_FAILED_INIT;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)out);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "switchmio/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return res;
}

// POST with a application/x-www-form-urlencoded body - used for RD token exchange.
static CURLcode fetch_url_post(const char *url, const char *postfields, MemBuf *out) {
    out->data = malloc(1);
    out->size = 0;

    CURL *curl = curl_easy_init();
    if (!curl) return CURLE_FAILED_INIT;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postfields);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)out);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "switchmio/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return res;
}

// GET with an Authorization: Bearer header - used for /rest/1.0/user.
static CURLcode fetch_url_auth(const char *url, const char *bearer_token, MemBuf *out) {
    out->data = malloc(1);
    out->size = 0;

    CURL *curl = curl_easy_init();
    if (!curl) return CURLE_FAILED_INIT;

    struct curl_slist *headers = NULL;
    char authheader[300];
    snprintf(authheader, sizeof(authheader), "Authorization: Bearer %s", bearer_token);
    headers = curl_slist_append(headers, authheader);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)out);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "switchmio/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return res;
}

// Copies [start,end) into dest, unescaping \/ \" \\ \n (Cinemeta/RD JSON escapes these).
static void copy_json_string(char *dest, size_t dest_size, const char *start, const char *end) {
    size_t di = 0;
    const char *p = start;
    while (p < end && di + 1 < dest_size) {
        if (*p == '\\' && p + 1 < end) {
            char n = *(p + 1);
            if (n == '/' || n == '"' || n == '\\') {
                dest[di++] = n;
                p += 2;
                continue;
            }
            if (n == 'n') {
                dest[di++] = ' ';
                p += 2;
                continue;
            }
        }
        dest[di++] = *p++;
    }
    dest[di] = 0;
}

// Finds "key":" ... " starting the search at 'from', bounded to 'from'+max_window.
static const char *find_json_string_field(const char *from, const char *key, size_t max_window, const char **out_end) {
    const char *window_end = from + max_window;
    const char *kp = strstr(from, key);
    if (!kp || kp > window_end) return NULL;
    kp += strlen(key);
    const char *vend = kp;
    while (*vend && *vend != '"') {
        if (*vend == '\\' && *(vend + 1)) vend++;
        vend++;
    }
    *out_end = vend;
    return kp;
}

// Generic "key":"value" extractor over any small JSON blob (used for RD responses).
static int json_get_string(const char *json, const char *key, char *out, size_t out_size) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    const char *e = p;
    while (*e && *e != '"') {
        if (*e == '\\' && *(e + 1)) e++;
        e++;
    }
    copy_json_string(out, out_size, p, e);
    return out[0] != 0;
}

// Generic "key":<number> extractor (no quotes) - used for interval/expires_in.
static int json_get_int(const char *json, const char *key, int *out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ') p++;
    *out = atoi(p);
    return 1;
}

static int extract_items(const char *json, const char *type_label) {
    int added = 0;
    const char *p = json;
    const char *id_key = "\"id\":\"";
    size_t id_keylen = strlen(id_key);

    while (g_item_count < MAX_ITEMS) {
        p = strstr(p, id_key);
        if (!p) break;
        p += id_keylen;

        const char *id_start = p;
        const char *id_end = p;
        while (*id_end && *id_end != '"') {
            if (*id_end == '\\' && *(id_end + 1)) id_end++;
            id_end++;
        }

        const char *name_key = "\"name\":\"";
        const char *name_end;
        const char *np = find_json_string_field(id_end, name_key, 1000, &name_end);
        if (!np) {
            p = id_end;
            continue;
        }

        CatalogItem *item = &g_items[g_item_count];
        copy_json_string(item->id, sizeof(item->id), id_start, id_end);
        copy_json_string(item->name, sizeof(item->name), np, name_end);
        strncpy(item->type, type_label, sizeof(item->type) - 1);
        item->type[sizeof(item->type) - 1] = 0;
        item->poster_url[0] = 0;
        item->description[0] = 0;

        const char *scan_from = name_end;

        const char *poster_end;
        const char *pp = find_json_string_field(scan_from, "\"poster\":\"", 3000, &poster_end);
        if (pp) {
            copy_json_string(item->poster_url, sizeof(item->poster_url), pp, poster_end);
        }

        const char *desc_end;
        const char *dp = find_json_string_field(scan_from, "\"description\":\"", 4000, &desc_end);
        if (dp) {
            copy_json_string(item->description, sizeof(item->description), dp, desc_end);
        }

        const char *advance_to = name_end;
        if (pp && poster_end > advance_to) advance_to = poster_end;
        if (dp && desc_end > advance_to) advance_to = desc_end;
        p = advance_to;

        g_item_count++;
        added++;
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
        if (res != CURLE_OK) {
            logmsg("fetch_url failed for %s : curl code %d", url, res);
            free(chunk.data);
            break;
        }

        int added = extract_items(chunk.data, type_label);
        logmsg("fetched %s skip=%d -> +%d items (total=%d)", type_label, skip, added, g_item_count);
        free(chunk.data);

        if (added == 0) break;
        if (added < PAGE_SIZE) break;
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

// Downloads + decodes one poster by absolute item index. Safe to call repeatedly.
static void load_poster(SDL_Renderer *ren, int idx) {
    if (idx < 0 || idx >= g_item_count) return;
    if (g_poster_state[idx] != 0) return;

    if (g_items[idx].poster_url[0] == 0) {
        g_poster_state[idx] = 1;
        return;
    }

    MemBuf chunk;
    CURLcode res = fetch_url(g_items[idx].poster_url, &chunk);
    if (res != CURLE_OK || chunk.size == 0) {
        logmsg("poster fetch failed idx=%d url=%s code=%d", idx, g_items[idx].poster_url, res);
        free(chunk.data);
        g_poster_state[idx] = 1;
        return;
    }

    SDL_RWops *rw = SDL_RWFromMem(chunk.data, (int)chunk.size);
    SDL_Surface *surf = rw ? IMG_Load_RW(rw, 1) : NULL;
    free(chunk.data);

    if (!surf) {
        logmsg("IMG_Load_RW failed idx=%d: %s", idx, IMG_GetError());
        g_poster_state[idx] = 1;
        return;
    }

    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_FreeSurface(surf);

    if (!tex) {
        logmsg("CreateTextureFromSurface failed idx=%d: %s", idx, SDL_GetError());
        g_poster_state[idx] = 1;
        return;
    }

    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    g_poster_tex[idx] = tex;
    g_poster_load_frame[idx] = g_frame;
    g_poster_state[idx] = 2;
}

static void draw_text(SDL_Renderer *ren, TTF_Font *font, const char *text, int x, int y, SDL_Color color) {
    if (!text || !*text) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, color);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_Rect dst = { x, y, surf->w, surf->h };
    SDL_RenderCopy(ren, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
}

static void draw_text_clipped(SDL_Renderer *ren, TTF_Font *font, const char *text, int x, int y, int max_w, SDL_Color color) {
    if (!text || !*text) return;
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

    char line[512];
    line[0] = 0;
    int lines_drawn = 0;

    const char *word_start = text;
    while (*word_start && lines_drawn < max_lines) {
        const char *word_end = word_start;
        while (*word_end && *word_end != ' ') word_end++;
        size_t wlen = (size_t)(word_end - word_start);

        char trial[512];
        size_t cur_len = strlen(line);
        size_t need = cur_len + (cur_len ? 1 : 0) + wlen;
        if (need >= sizeof(trial)) need = sizeof(trial) - 1;

        snprintf(trial, sizeof(trial), "%s%s%.*s", line, cur_len ? " " : "", (int)wlen, word_start);

        int tw = 0, th = 0;
        TTF_SizeUTF8(font, trial, &tw, &th);

        if (tw > max_w && cur_len > 0) {
            draw_text(ren, font, line, x, y + lines_drawn * line_h, color);
            lines_drawn++;
            line[0] = 0;
            continue;
        }

        strncpy(line, trial, sizeof(line) - 1);
        line[sizeof(line) - 1] = 0;

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
        int alpha = age * 22;
        if (alpha > 255) alpha = 255;
        if (alpha < 0) alpha = 0;
        SDL_SetTextureAlphaMod(g_poster_tex[abs_i], (Uint8)alpha);
        SDL_RenderCopy(ren, g_poster_tex[abs_i], NULL, rect);
    }
}

// Lazily renders+caches a gray title-label texture for an item (non-selected state).
// Cuts per-frame text rendering from ~49 draws to effectively 0 after warm-up.
static SDL_Texture *get_title_texture(SDL_Renderer *ren, TTF_Font *font, int abs_i, SDL_Color color) {
    if (g_title_tex[abs_i]) return g_title_tex[abs_i];
    if (!font || !g_items[abs_i].name[0]) return NULL;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, g_items[abs_i].name, color);
    if (!surf) return NULL;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_FreeSurface(surf);
    g_title_tex[abs_i] = tex;
    return tex;
}

static void render_texture_clipped(SDL_Renderer *ren, SDL_Texture *tex, int x, int y, int max_w) {
    if (!tex) return;
    int tw = 0, th = 0;
    SDL_QueryTexture(tex, NULL, NULL, &tw, &th);
    SDL_Rect clip = { x, y, max_w, th };
    SDL_RenderSetClipRect(ren, &clip);
    SDL_Rect dst = { x, y, tw, th };
    SDL_RenderCopy(ren, tex, NULL, &dst);
    SDL_RenderSetClipRect(ren, NULL);
}

// ---------- Real-Debrid device-code login ----------

static void rd_save_credentials(void) {
    FILE *f = fopen(RD_TOKEN_PATH, "w");
    if (!f) { logmsg("rd_save_credentials: fopen failed"); return; }
    fprintf(f, "%s\n%s\n%s\n%s\n", g_rd_client_id, g_rd_client_secret, g_rd_access_token, g_rd_refresh_token);
    fclose(f);
    logmsg("Real-Debrid credentials saved to SD card");
}

static int rd_load_credentials(void) {
    FILE *f = fopen(RD_TOKEN_PATH, "r");
    if (!f) return 0;
    char line[4][200] = {{0}};
    for (int i = 0; i < 4; i++) {
        if (!fgets(line[i], sizeof(line[i]), f)) { fclose(f); return 0; }
        size_t l = strlen(line[i]);
        while (l > 0 && (line[i][l-1] == '\n' || line[i][l-1] == '\r')) line[i][--l] = 0;
    }
    fclose(f);
    strncpy(g_rd_client_id, line[0], sizeof(g_rd_client_id)-1);
    strncpy(g_rd_client_secret, line[1], sizeof(g_rd_client_secret)-1);
    strncpy(g_rd_access_token, line[2], sizeof(g_rd_access_token)-1);
    strncpy(g_rd_refresh_token, line[3], sizeof(g_rd_refresh_token)-1);
    return g_rd_access_token[0] != 0;
}

static int rd_fetch_username(void) {
    MemBuf resp;
    CURLcode res = fetch_url_auth("https://api.real-debrid.com/rest/1.0/user", g_rd_access_token, &resp);
    if (res != CURLE_OK) { free(resp.data); logmsg("rd_fetch_username: curl failed %d", res); return 0; }
    int ok = json_get_string(resp.data, "username", g_rd_username, sizeof(g_rd_username));
    logmsg("rd_fetch_username: %s", ok ? g_rd_username : "failed to parse username");
    free(resp.data);
    return ok;
}

static int rd_refresh_access_token(void) {
    if (!g_rd_client_id[0] || !g_rd_refresh_token[0]) return 0;
    char post[512];
    snprintf(post, sizeof(post),
             "client_id=%s&client_secret=%s&code=%s&grant_type=http://oauth.net/grant_type/device/1.0",
             g_rd_client_id, g_rd_client_secret, g_rd_refresh_token);
    MemBuf resp;
    CURLcode res = fetch_url_post("https://api.real-debrid.com/oauth/v2/token", post, &resp);
    if (res != CURLE_OK) { free(resp.data); return 0; }
    char new_access[160] = {0}, new_refresh[160] = {0};
    int ok = json_get_string(resp.data, "access_token", new_access, sizeof(new_access));
    json_get_string(resp.data, "refresh_token", new_refresh, sizeof(new_refresh));
    free(resp.data);
    if (!ok) return 0;
    strncpy(g_rd_access_token, new_access, sizeof(g_rd_access_token)-1);
    if (new_refresh[0]) strncpy(g_rd_refresh_token, new_refresh, sizeof(g_rd_refresh_token)-1);
    rd_save_credentials();
    logmsg("Real-Debrid access token refreshed");
    return 1;
}

static void rd_logout(void) {
    remove(RD_TOKEN_PATH);
    memset(g_rd_client_id, 0, sizeof(g_rd_client_id));
    memset(g_rd_client_secret, 0, sizeof(g_rd_client_secret));
    memset(g_rd_access_token, 0, sizeof(g_rd_access_token));
    memset(g_rd_refresh_token, 0, sizeof(g_rd_refresh_token));
    memset(g_rd_username, 0, sizeof(g_rd_username));
    g_rd_state = RD_LOGGED_OUT;
    logmsg("Real-Debrid: logged out");
}

static void rd_start_device_login(void) {
    MemBuf resp;
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.real-debrid.com/oauth/v2/device/code?client_id=%s&new_credentials=yes",
             RD_CLIENT_ID_OPENSOURCE);
    CURLcode res = fetch_url(url, &resp);
    if (res != CURLE_OK) {
        logmsg("rd_start_device_login: curl failed %d", res);
        free(resp.data);
        g_rd_state = RD_ERROR;
        snprintf(g_rd_error_msg, sizeof(g_rd_error_msg), "Network error starting login.");
        return;
    }

    int ok = json_get_string(resp.data, "device_code", g_rd_device_code, sizeof(g_rd_device_code))
           && json_get_string(resp.data, "user_code", g_rd_user_code, sizeof(g_rd_user_code))
           && json_get_string(resp.data, "verification_url", g_rd_verification_url, sizeof(g_rd_verification_url));
    json_get_int(resp.data, "interval", &g_rd_interval);
    json_get_int(resp.data, "expires_in", &g_rd_expires_in);
    free(resp.data);

    if (!ok) {
        logmsg("rd_start_device_login: failed to parse response");
        g_rd_state = RD_ERROR;
        snprintf(g_rd_error_msg, sizeof(g_rd_error_msg), "Couldn't read login code from Real-Debrid.");
        return;
    }
    if (g_rd_interval < 3) g_rd_interval = 3; // be polite to the API

    g_rd_login_start_frame = g_frame;
    g_rd_last_poll_frame = g_frame;
    g_rd_state = RD_AWAITING_AUTH;
    logmsg("Real-Debrid device login started: user_code=%s url=%s", g_rd_user_code, g_rd_verification_url);
}

// Called once per frame. Only actually hits the network every g_rd_interval
// seconds (assuming ~60fps), so it doesn't spam the API or the render loop.
static void rd_poll_step(void) {
    if (g_rd_state != RD_AWAITING_AUTH) return;

    int elapsed_frames = g_frame - g_rd_login_start_frame;
    if (elapsed_frames > g_rd_expires_in * 60) {
        logmsg("Real-Debrid device login expired");
        g_rd_state = RD_ERROR;
        snprintf(g_rd_error_msg, sizeof(g_rd_error_msg), "Login code expired. Try again.");
        return;
    }

    int frames_since_poll = g_frame - g_rd_last_poll_frame;
    if (frames_since_poll < g_rd_interval * 60) return;
    g_rd_last_poll_frame = g_frame;

    char url[256];
    snprintf(url, sizeof(url), "https://api.real-debrid.com/oauth/v2/device/credentials?client_id=%s&code=%s",
             RD_CLIENT_ID_OPENSOURCE, g_rd_device_code);
    MemBuf resp;
    CURLcode res = fetch_url(url, &resp);
    if (res != CURLE_OK) {
        logmsg("rd_poll_step: credentials curl failed %d", res);
        free(resp.data);
        return; // transient, try again next interval
    }

    char new_client_id[64] = {0}, new_client_secret[80] = {0};
    int got_creds = json_get_string(resp.data, "client_id", new_client_id, sizeof(new_client_id))
                  && json_get_string(resp.data, "client_secret", new_client_secret, sizeof(new_client_secret));
    logmsg("rd_poll_step: credentials response: %.200s", resp.data);
    free(resp.data);

    if (!got_creds) return; // still "authorization_pending" - normal, keep polling

    strncpy(g_rd_client_id, new_client_id, sizeof(g_rd_client_id)-1);
    strncpy(g_rd_client_secret, new_client_secret, sizeof(g_rd_client_secret)-1);

    char post[512];
    snprintf(post, sizeof(post),
             "client_id=%s&client_secret=%s&code=%s&grant_type=http://oauth.net/grant_type/device/1.0",
             g_rd_client_id, g_rd_client_secret, g_rd_device_code);
    MemBuf tok_resp;
    CURLcode tok_res = fetch_url_post("https://api.real-debrid.com/oauth/v2/token", post, &tok_resp);
    if (tok_res != CURLE_OK) {
        logmsg("rd_poll_step: token exchange curl failed %d", tok_res);
        free(tok_resp.data);
        g_rd_state = RD_ERROR;
        snprintf(g_rd_error_msg, sizeof(g_rd_error_msg), "Network error finishing login.");
        return;
    }

    int got_token = json_get_string(tok_resp.data, "access_token", g_rd_access_token, sizeof(g_rd_access_token));
    json_get_string(tok_resp.data, "refresh_token", g_rd_refresh_token, sizeof(g_rd_refresh_token));
    logmsg("rd_poll_step: token response: %.200s", tok_resp.data);
    free(tok_resp.data);

    if (!got_token) {
        g_rd_state = RD_ERROR;
        snprintf(g_rd_error_msg, sizeof(g_rd_error_msg), "Couldn't get access token.");
        return;
    }

    rd_save_credentials();
    rd_fetch_username();
    g_rd_state = RD_LOGGED_IN;
    logmsg("Real-Debrid login complete for user: %s", g_rd_username);
}
// -----------------------------------------------------

int main(int argc, char *argv[]) {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/switchmio", 0777);
    g_log = fopen("sdmc:/switch/switchmio/log.txt", "w");
    logmsg("=== switchmio starting ===");

    Result rc_romfs = romfsInit();
    logmsg("romfsInit: %s (0x%x)", R_FAILED(rc_romfs) ? "FAILED" : "ok", rc_romfs);

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    Result rc = socketInitializeDefault();
    logmsg("socketInitializeDefault: %s (0x%x)", R_FAILED(rc) ? "FAILED" : "ok", rc);
    if (R_FAILED(rc)) {
        logmsg("Aborting: no network.");
        if (!R_FAILED(rc_romfs)) romfsExit();
        if (g_log) fclose(g_log);
        return 1;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Try to resume a saved Real-Debrid session before anything else.
    if (rd_load_credentials()) {
        if (rd_fetch_username() || (rd_refresh_access_token() && rd_fetch_username())) {
            g_rd_state = RD_LOGGED_IN;
            logmsg("Resumed Real-Debrid session for %s", g_rd_username);
        } else {
            logmsg("Saved Real-Debrid token invalid/expired, clearing.");
            rd_logout();
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_AUDIO) != 0) {
        logmsg("SDL_Init FAILED: %s", SDL_GetError());
        curl_global_cleanup();
        socketExit();
        romfsExit();
        if (g_log) fclose(g_log);
        return 1;
    }
    logmsg("SDL_Init ok");

    if (TTF_Init() != 0) {
        logmsg("TTF_Init FAILED: %s", TTF_GetError());
        SDL_Quit();
        curl_global_cleanup();
        socketExit();
        romfsExit();
        if (g_log) fclose(g_log);
        return 1;
    }
    logmsg("TTF_Init ok");

    int img_flags = IMG_INIT_JPG | IMG_INIT_PNG;
    int img_ok = IMG_Init(img_flags);
    if ((img_ok & img_flags) != img_flags) {
        logmsg("IMG_Init warning: got 0x%x wanted 0x%x : %s", img_ok, img_flags, IMG_GetError());
    } else {
        logmsg("IMG_Init ok");
    }

    if (Mix_OpenAudio(AUDIO_FREQ, AUDIO_S16SYS, 2, 1024) != 0) {
        logmsg("Mix_OpenAudio FAILED: %s (sound effects disabled, app continues)", Mix_GetError());
        g_audio_ok = 0;
    } else {
        Mix_AllocateChannels(8);
        init_sfx();
        g_audio_ok = 1;
        logmsg("Mix_OpenAudio ok, sound effects ready");
    }

    SDL_Window *win = SDL_CreateWindow("switchmio", 0, 0, SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN);
    if (!win) logmsg("SDL_CreateWindow FAILED: %s", SDL_GetError());
    else logmsg("SDL_CreateWindow ok");

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) logmsg("SDL_CreateRenderer FAILED: %s", SDL_GetError());
    else logmsg("SDL_CreateRenderer ok");

    TTF_Font *font = TTF_OpenFont("romfs:/font.ttf", FONT_SIZE);
    if (!font) {
        logmsg("TTF_OpenFont(romfs:/font.ttf) FAILED: %s", TTF_GetError());
        font = TTF_OpenFont("sdmc:/switch/switchmio/font.ttf", FONT_SIZE);
        if (!font) {
            logmsg("TTF_OpenFont(sdmc fallback) FAILED: %s", TTF_GetError());
        } else {
            logmsg("Loaded font from sdmc fallback path instead of romfs.");
        }
    } else {
        logmsg("TTF_OpenFont(romfs:/font.ttf) ok");
    }

    TTF_Font *font_small = TTF_OpenFont("romfs:/font.ttf", FONT_SIZE_SMALL);
    if (!font_small) {
        font_small = TTF_OpenFont("sdmc:/switch/switchmio/font.ttf", FONT_SIZE_SMALL);
    }

    SDL_Color white     = {255, 255, 255, 255};
    SDL_Color gray      = {160, 160, 160, 255};
    SDL_Color dim_gray  = {45, 45, 52, 255};
    SDL_Color neon_blue = {10, 185, 230, 255};
    SDL_Color neon_red  = {255, 60, 45, 255};
    SDL_Color tab_off   = {90, 90, 100, 255};
    SDL_Color bg_color  = {18, 18, 24, 255};

    if (!win || !ren) {
        logmsg("Aborting: window/renderer not available.");
        if (g_audio_ok) { free_sfx(); Mix_CloseAudio(); }
        if (font) TTF_CloseFont(font);
        if (font_small) TTF_CloseFont(font_small);
        TTF_Quit();
        IMG_Quit();
        if (ren) SDL_DestroyRenderer(ren);
        if (win) SDL_DestroyWindow(win);
        SDL_Quit();
        curl_global_cleanup();
        socketExit();
        romfsExit();
        if (g_log) fclose(g_log);
        return 1;
    }

    SDL_SetRenderDrawColor(ren, bg_color.r, bg_color.g, bg_color.b, 255);
    SDL_RenderClear(ren);
    if (font) {
        draw_text(ren, font, "switchmio - loading movies + series from Cinemeta...", 40, 40, white);
    } else {
        SDL_SetRenderDrawColor(ren, neon_red.r, neon_red.g, neon_red.b, 255);
        SDL_Rect bar = {40, 40, 600, 40};
        SDL_RenderFillRect(ren, &bar);
    }
    SDL_RenderPresent(ren);
    logmsg("Loading screen presented (font=%s)", font ? "yes" : "no");

    fetch_full_catalog(MOVIE_URL_FMT, "movie");
    fetch_full_catalog(SERIES_URL_FMT, "series");
    logmsg("Catalog fetch done, total items=%d", g_item_count);

    int tab = TAB_ALL;
    rebuild_filtered(tab);

    int cursor = 0;
    int scroll_row = 0;
    int running = 1;
    int show_account_popup = 0;
    int app_state = STATE_GRID;
    int details_idx = -1;

    int margin_x = (SCREEN_W - (GRID_COLS * TILE_W + (GRID_COLS - 1) * GAP_X)) / 2;
    int row_stride = TILE_H + LABEL_H + GAP_Y;
    int rows_visible = (SCREEN_H - GRID_TOP - FOOTER_H) / row_stride;
    if (rows_visible < 1) rows_visible = 1;

    float anim_x = (float)margin_x;
    float anim_y = (float)GRID_TOP;
    int anim_inited = 0;

    while (running && appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_Plus) running = 0;

        if (show_account_popup) {
            if (g_rd_state == RD_LOGGED_IN && (kDown & HidNpadButton_X)) {
                rd_logout();
                play_sfx(SFX_BACK);
            } else if (kDown & HidNpadButton_Y && g_rd_state != RD_AWAITING_AUTH && g_rd_state != RD_LOGGED_IN) {
                // retry from an error state
                g_rd_state = RD_FETCHING_CODE;
                rd_start_device_login();
                play_sfx(SFX_TAB);
            } else if (kDown & HidNpadButton_B) {
                show_account_popup = 0;
                play_sfx(SFX_BACK);
            }
        } else if (app_state == STATE_DETAILS) {
            if (kDown & HidNpadButton_B) {
                app_state = STATE_GRID;
                play_sfx(SFX_BACK);
            }
        } else { // STATE_GRID
            if (kDown & HidNpadButton_Y) {
                show_account_popup = 1;
                play_sfx(SFX_TAB);
                if (g_rd_state == RD_LOGGED_OUT) {
                    g_rd_state = RD_FETCHING_CODE;
                    rd_start_device_login();
                }
            }

            if (kDown & HidNpadButton_R) {
                tab = (tab + 1) % TAB_COUNT;
                rebuild_filtered(tab);
                cursor = 0;
                scroll_row = 0;
                play_sfx(SFX_TAB);
            }
            if (kDown & HidNpadButton_L) {
                tab = (tab - 1 + TAB_COUNT) % TAB_COUNT;
                rebuild_filtered(tab);
                cursor = 0;
                scroll_row = 0;
                play_sfx(SFX_TAB);
            }

            if (g_filtered_count > 0) {
                int total_rows = (g_filtered_count + GRID_COLS - 1) / GRID_COLS;
                int old_cursor = cursor;

                if (kDown & HidNpadButton_Down) {
                    if (cursor + GRID_COLS < g_filtered_count) cursor += GRID_COLS;
                }
                if (kDown & HidNpadButton_Up) {
                    if (cursor - GRID_COLS >= 0) cursor -= GRID_COLS;
                }
                if (kDown & HidNpadButton_Right) {
                    if ((cursor % GRID_COLS) != GRID_COLS - 1 && cursor + 1 < g_filtered_count) cursor++;
                }
                if (kDown & HidNpadButton_Left) {
                    if ((cursor % GRID_COLS) != 0 && cursor > 0) cursor--;
                }
                if (cursor != old_cursor) play_sfx(SFX_MOVE);

                if (kDown & HidNpadButton_A) {
                    details_idx = g_filtered[cursor];
                    app_state = STATE_DETAILS;
                    play_sfx(SFX_SELECT);
                }

                int cursor_row = cursor / GRID_COLS;
                if (cursor_row < scroll_row) scroll_row = cursor_row;
                if (cursor_row >= scroll_row + rows_visible) scroll_row = cursor_row - rows_visible + 1;
                if (scroll_row < 0) scroll_row = 0;
                if (scroll_row > total_rows - rows_visible) {
                    scroll_row = total_rows - rows_visible;
                    if (scroll_row < 0) scroll_row = 0;
                }
            }
        }

        // Advance Real-Debrid login polling regardless of whether the popup
        // is open, so it finishes quietly in the background even if you back out.
        rd_poll_step();

        // Lazy-load: at most one new poster per frame, from what's currently visible.
        if (app_state == STATE_DETAILS) {
            if (details_idx >= 0) load_poster(ren, details_idx);
        } else if (g_filtered_count > 0) {
            int start_local = scroll_row * GRID_COLS;
            int end_local = start_local + rows_visible * GRID_COLS;
            if (end_local > g_filtered_count) end_local = g_filtered_count;
            for (int li = start_local; li < end_local; li++) {
                int abs_i = g_filtered[li];
                if (g_poster_state[abs_i] == 0) {
                    load_poster(ren, abs_i);
                    break;
                }
            }
        }

        SDL_SetRenderDrawColor(ren, bg_color.r, bg_color.g, bg_color.b, 255);
        SDL_RenderClear(ren);

        if (app_state == STATE_DETAILS && details_idx >= 0) {
            CatalogItem *it = &g_items[details_idx];

            int pw = 300, ph = 450;
            int px = margin_x, py = 90;
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

            int text_x = px + pw + 50;
            int text_w = SCREEN_W - text_x - margin_x;

            if (font) {
                draw_text(ren, font, it->name, text_x, py, white);
                char sub[64];
                snprintf(sub, sizeof(sub), "%s", strcmp(it->type, "movie") == 0 ? "Movie" : "TV Series");
                draw_text(ren, font, sub, text_x, py + 40, neon_blue);
            }
            if (font_small) {
                const char *desc = it->description[0] ? it->description : "No description available.";
                draw_text_wrapped(ren, font_small, desc, text_x, py + 90, text_w, 26, 12, gray);
            }

            int bw = 260, bh = 56;
            int bx = text_x, by = py + ph - bh;
            SDL_SetRenderDrawColor(ren, neon_red.r, neon_red.g, neon_red.b, 255);
            SDL_Rect btn = { bx, by, bw, bh };
            SDL_RenderFillRect(ren, &btn);
            if (font) {
                const char *play_label = (g_rd_state == RD_LOGGED_IN)
                    ? "A: Play (coming soon)"
                    : "Y on grid: sign in to Real-Debrid first";
                draw_text(ren, font, play_label, bx + 14, by + 14, white);
            }

            if (font) {
                SDL_SetRenderDrawColor(ren, 25, 25, 32, 255);
                SDL_Rect footer = { 0, SCREEN_H - FOOTER_H, SCREEN_W, FOOTER_H };
                SDL_RenderFillRect(ren, &footer);
                draw_text(ren, font, "B: back", margin_x, SCREEN_H - FOOTER_H + 12, gray);
            }
        } else {
            if (font) {
                draw_text(ren, font, "switchmio", margin_x, 20, white);

                int tab_x = margin_x + 180;
                for (int t = 0; t < TAB_COUNT; t++) {
                    SDL_Color c = (t == tab) ? neon_blue : tab_off;
                    draw_text(ren, font, TAB_NAMES[t], tab_x, 20, c);
                    int tw = 0, th = 0;
                    TTF_SizeUTF8(font, TAB_NAMES[t], &tw, &th);
                    tab_x += tw + 30;
                }

                char acct_label[80];
                SDL_Color acct_color = neon_red;
                if (g_rd_state == RD_LOGGED_IN) {
                    snprintf(acct_label, sizeof(acct_label), "Account: %s", g_rd_username);
                    acct_color = neon_blue;
                } else if (g_rd_state == RD_AWAITING_AUTH) {
                    snprintf(acct_label, sizeof(acct_label), "Account: signing in...");
                } else {
                    snprintf(acct_label, sizeof(acct_label), "Account: Not signed in");
                }
                draw_text(ren, font, acct_label, SCREEN_W - margin_x - 260, 20, acct_color);
            }
            if (font_small) {
                char count_line[64];
                snprintf(count_line, sizeof(count_line), "%d titles", g_filtered_count);
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
                int cur_col = cur_local % GRID_COLS;
                int cur_row = cur_local / GRID_COLS;
                float target_x = (float)(margin_x + cur_col * (TILE_W + GAP_X));
                float target_y = (float)(GRID_TOP + cur_row * row_stride);

                if (!anim_inited) {
                    anim_x = target_x;
                    anim_y = target_y;
                    anim_inited = 1;
                } else {
                    anim_x += (target_x - anim_x) * 0.35f;
                    anim_y += (target_y - anim_y) * 0.35f;
                }

                for (int li = start_local; li < end_local; li++) {
                    int abs_i = g_filtered[li];
                    int local_i = li - start_local;
                    int col = local_i % GRID_COLS;
                    int row = local_i / GRID_COLS;

                    int x = margin_x + col * (TILE_W + GAP_X);
                    int y = GRID_TOP + row * row_stride;

                    SDL_Rect poster_rect = { x, y, TILE_W, TILE_H };

                    if (g_poster_state[abs_i] == 2 && g_poster_tex[abs_i]) {
                        draw_poster_tile(ren, abs_i, &poster_rect);
                    } else {
                        SDL_SetRenderDrawColor(ren, dim_gray.r, dim_gray.g, dim_gray.b, 255);
                        SDL_RenderFillRect(ren, &poster_rect);
                        if (g_poster_state[abs_i] == 1 && font) {
                            draw_text_clipped(ren, font, "no image", x + 8, y + TILE_H / 2 - 10, TILE_W - 16, gray);
                        }
                    }

                    if (font) {
                        if (li == cursor) {
                            draw_text_clipped(ren, font, g_items[abs_i].name, x, y + TILE_H + 4, TILE_W, white);
                        } else {
                            SDL_Texture *ttex = get_title_texture(ren, font, abs_i, gray);
                            render_texture_clipped(ren, ttex, x, y + TILE_H + 4, TILE_W);
                        }
                    }
                }

                int hx = (int)(anim_x + 0.5f);
                int hy = (int)(anim_y + 0.5f);
                SDL_Color ring_colors[3] = { neon_blue, neon_red, neon_blue };
                for (int b = 0; b < 3; b++) {
                    SDL_Rect ring = { hx - 4 - b, hy - 4 - b, TILE_W + 8 + b * 2, TILE_H + 8 + b * 2 };
                    SDL_SetRenderDrawColor(ren, ring_colors[b].r, ring_colors[b].g, ring_colors[b].b, 255);
                    SDL_RenderDrawRect(ren, &ring);
                }
            }

            if (font) {
                SDL_SetRenderDrawColor(ren, 25, 25, 32, 255);
                SDL_Rect footer = { 0, SCREEN_H - FOOTER_H, SCREEN_W, FOOTER_H };
                SDL_RenderFillRect(ren, &footer);
                draw_text(ren, font, "D-Pad: move   A: details   L/R: tabs   Y: account   +: exit", margin_x, SCREEN_H - FOOTER_H + 12, gray);
            }
        }

        if (show_account_popup && font) {
            int pw = 620, ph = 220;
            int px = (SCREEN_W - pw) / 2, py = (SCREEN_H - ph) / 2;
            SDL_SetRenderDrawColor(ren, 30, 30, 40, 255);
            SDL_Rect box = { px, py, pw, ph };
            SDL_RenderFillRect(ren, &box);
            SDL_SetRenderDrawColor(ren, neon_blue.r, neon_blue.g, neon_blue.b, 255);
            SDL_RenderDrawRect(ren, &box);

            if (g_rd_state == RD_AWAITING_AUTH) {
                draw_text(ren, font, "Real-Debrid login", px + 24, py + 20, white);
                char line1[160];
                snprintf(line1, sizeof(line1), "Go to: %s", g_rd_verification_url);
                draw_text(ren, font_small ? font_small : font, line1, px + 24, py + 64, gray);
                char line2[64];
                snprintf(line2, sizeof(line2), "Enter code: %s", g_rd_user_code);
                draw_text(ren, font, line2, px + 24, py + 96, neon_blue);
                draw_text(ren, font_small ? font_small : font, "Waiting for you to authorize on another device...", px + 24, py + 140, gray);
                draw_text(ren, font, "B: close (keeps waiting in background)", px + 24, py + ph - 40, gray);
            } else if (g_rd_state == RD_LOGGED_IN) {
                draw_text(ren, font, "Real-Debrid", px + 24, py + 24, white);
                char line[80];
                snprintf(line, sizeof(line), "Signed in as: %s", g_rd_username);
                draw_text(ren, font, line, px + 24, py + 70, neon_blue);
                draw_text(ren, font, "X: log out    B: close", px + 24, py + ph - 40, gray);
            } else if (g_rd_state == RD_ERROR) {
                draw_text(ren, font, "Real-Debrid login", px + 24, py + 24, white);
                draw_text(ren, font_small ? font_small : font, g_rd_error_msg, px + 24, py + 70, neon_red);
                draw_text(ren, font, "Y: try again    B: close", px + 24, py + ph - 40, gray);
            } else {
                draw_text(ren, font, "Real-Debrid login", px + 24, py + 24, white);
                draw_text(ren, font, "Starting...", px + 24, py + 70, gray);
                draw_text(ren, font, "B: close", px + 24, py + ph - 40, gray);
            }
        }

        SDL_RenderPresent(ren);

        g_frame++;
        if (g_frame == 1 || g_frame == 60 || g_frame == 300) {
            logmsg("frame %d presented", g_frame);
        }
    }

    logmsg("Exiting main loop, total frames=%d", g_frame);

    for (int i = 0; i < g_item_count; i++) {
        if (g_poster_tex[i]) SDL_DestroyTexture(g_poster_tex[i]);
        if (g_title_tex[i]) SDL_DestroyTexture(g_title_tex[i]);
    }

    if (g_audio_ok) { free_sfx(); Mix_CloseAudio(); }
    if (font) TTF_CloseFont(font);
    if (font_small) TTF_CloseFont(font_small);
    TTF_Quit();
    IMG_Quit();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    curl_global_cleanup();
    socketExit();
    romfsExit();
    if (g_log) fclose(g_log);
    return 0;
}
