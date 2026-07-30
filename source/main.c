// switchmio - Native Stremio catalog browser for Nintendo Switch
// DEBUG BUILD: logs every init step to sdmc:/switch/switchmio/log.txt
// so we can see exactly where SDL/TTF/font setup is failing (SDL owns
// the whole screen once it starts, so printf output is no longer visible).

#include <switch.h>
#include <curl/curl.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define MOVIE_URL_FMT  "https://v3-cinemeta.strem.io/catalog/movie/top/skip=%d.json"
#define SERIES_URL_FMT "https://v3-cinemeta.strem.io/catalog/series/top/skip=%d.json"
#define PAGE_SIZE      100
#define MAX_ITEMS      600
#define ROW_HEIGHT     34
#define FONT_SIZE      24
#define SCREEN_W       1280
#define SCREEN_H       720

typedef struct {
    char id[128];
    char name[256];
    char type[16];
} CatalogItem;

static CatalogItem g_items[MAX_ITEMS];
static int g_item_count = 0;

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
        size_t id_len = (size_t)(id_end - id_start);

        const char *window_end = id_end + 1000;
        const char *name_key = "\"name\":\"";
        const char *np = strstr(id_end, name_key);
        if (!np || np > window_end) {
            p = id_end;
            continue;
        }
        np += strlen(name_key);
        const char *name_end = np;
        while (*name_end && *name_end != '"') {
            if (*name_end == '\\' && *(name_end + 1)) name_end++;
            name_end++;
        }
        size_t name_len = (size_t)(name_end - np);

        CatalogItem *item = &g_items[g_item_count];
        size_t clip_id = id_len >= sizeof(item->id) ? sizeof(item->id) - 1 : id_len;
        size_t clip_name = name_len >= sizeof(item->name) ? sizeof(item->name) - 1 : name_len;
        memcpy(item->id, id_start, clip_id);
        item->id[clip_id] = 0;
        memcpy(item->name, np, clip_name);
        item->name[clip_name] = 0;
        strncpy(item->type, type_label, sizeof(item->type) - 1);
        item->type[sizeof(item->type) - 1] = 0;

        g_item_count++;
        added++;
        p = name_end;
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

int main(int argc, char *argv[]) {
    // Open the debug log FIRST, before anything else, so we capture every step.
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

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
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

    SDL_Window *win = SDL_CreateWindow("switchmio", 0, 0, SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN);
    if (!win) logmsg("SDL_CreateWindow FAILED: %s", SDL_GetError());
    else logmsg("SDL_CreateWindow ok");

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) logmsg("SDL_CreateRenderer FAILED: %s", SDL_GetError());
    else logmsg("SDL_CreateRenderer ok");

    // Try the normal romfs path first; if that fails, fall back to a
    // plain sdmc path in case romfs never mounted correctly.
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

    SDL_Color white     = {255, 255, 255, 255};
    SDL_Color gray      = {160, 160, 160, 255};
    SDL_Color highlight = {40, 90, 200, 255};

    if (!win || !ren) {
        logmsg("Aborting: window/renderer not available.");
        if (font) TTF_CloseFont(font);
        TTF_Quit();
        if (ren) SDL_DestroyRenderer(ren);
        if (win) SDL_DestroyWindow(win);
        SDL_Quit();
        curl_global_cleanup();
        socketExit();
        romfsExit();
        if (g_log) fclose(g_log);
        return 1;
    }

    SDL_SetRenderDrawColor(ren, 15, 15, 20, 255);
    SDL_RenderClear(ren);
    if (font) {
        draw_text(ren, font, "switchmio - loading movies + series from Cinemeta...", 40, 40, white);
    } else {
        SDL_SetRenderDrawColor(ren, 220, 120, 20, 255);
        SDL_Rect bar = {40, 40, 600, 40};
        SDL_RenderFillRect(ren, &bar);
    }
    SDL_RenderPresent(ren);
    logmsg("Loading screen presented (font=%s)", font ? "yes" : "no");

    fetch_full_catalog(MOVIE_URL_FMT, "movie");
    fetch_full_catalog(SERIES_URL_FMT, "series");
    logmsg("Catalog fetch done, total items=%d", g_item_count);

    curl_global_cleanup();
    socketExit();

    int cursor = 0;
    int scroll_offset = 0;
    int visible_rows = (SCREEN_H - 100) / ROW_HEIGHT;
    int running = 1;
    int frame = 0;

    while (running && appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_Plus) running = 0;

        if (g_item_count > 0) {
            if (kDown & HidNpadButton_AnyDown) {
                if (cursor < g_item_count - 1) cursor++;
            }
            if (kDown & HidNpadButton_AnyUp) {
                if (cursor > 0) cursor--;
            }
            if (cursor < scroll_offset) scroll_offset = cursor;
            if (cursor >= scroll_offset + visible_rows) scroll_offset = cursor - visible_rows + 1;
        }

        SDL_SetRenderDrawColor(ren, 15, 15, 20, 255);
        SDL_RenderClear(ren);

        if (font) {
            char header[128];
            snprintf(header, sizeof(header), "switchmio - %d titles   (D-Pad: scroll, +: exit)", g_item_count);
            draw_text(ren, font, header, 30, 20, gray);

            if (g_item_count == 0) {
                draw_text(ren, font, "No titles loaded - check your network connection.", 30, 80, white);
            } else {
                int end = scroll_offset + visible_rows;
                if (end > g_item_count) end = g_item_count;

                int y = 70;
                for (int i = scroll_offset; i < end; i++) {
                    SDL_Rect row = { 20, y, SCREEN_W - 40, ROW_HEIGHT - 2 };
                    if (i == cursor) {
                        SDL_SetRenderDrawColor(ren, highlight.r, highlight.g, highlight.b, 255);
                        SDL_RenderFillRect(ren, &row);
                    }
                    char line[300];
                    snprintf(line, sizeof(line), "[%s] %s", g_items[i].type, g_items[i].name);
                    draw_text(ren, font, line, 30, y + 4, white);
                    y += ROW_HEIGHT;
                }
            }
        } else {
            SDL_SetRenderDrawColor(ren, 220, 40, 40, 255);
            SDL_Rect bar = {30, 30, 400, 40};
            SDL_RenderFillRect(ren, &bar);
        }

        SDL_RenderPresent(ren);

        frame++;
        if (frame == 1 || frame == 60 || frame == 300) {
            logmsg("frame %d presented", frame);
        }
    }

    logmsg("Exiting main loop, total frames=%d", frame);

    if (font) TTF_CloseFont(font);
    TTF_Quit();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    romfsExit();
    if (g_log) fclose(g_log);
    return 0;
}
