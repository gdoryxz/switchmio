// switchmio - Native Stremio catalog browser for Nintendo Switch
// Poster grid UI + lazy-loaded thumbnails.
// DEBUG BUILD: logs every init step to sdmc:/switch/switchmio/log.txt

#include <switch.h>
#include <curl/curl.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>

#define MOVIE_URL_FMT  "https://v3-cinemeta.strem.io/catalog/movie/top/skip=%d.json"
#define SERIES_URL_FMT "https://v3-cinemeta.strem.io/catalog/series/top/skip=%d.json"
#define PAGE_SIZE      100
#define MAX_ITEMS      600
#define FONT_SIZE      22
#define SCREEN_W       1280
#define SCREEN_H       720

// grid layout
#define GRID_COLS   7
#define TILE_W      140
#define TILE_H      210
#define LABEL_H     24
#define GAP_X       16
#define GAP_Y       16
#define GRID_TOP    100
#define FOOTER_H    50

typedef struct {
    char id[128];
    char name[256];
    char type[16];
    char poster_url[300];
} CatalogItem;

static CatalogItem g_items[MAX_ITEMS];
static int g_item_count = 0;

static SDL_Texture *g_poster_tex[MAX_ITEMS];
static int g_poster_state[MAX_ITEMS]; // 0 = not tried, 1 = failed, 2 = loaded

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

// Copies [start,end) into dest, unescaping \/ \" \\ (Cinemeta JSON escapes slashes).
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
        }
        dest[di++] = *p++;
    }
    dest[di] = 0;
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
        const char *window_end = id_end + 1000;
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

        // poster is usually within a couple KB after name
        const char *poster_key = "\"poster\":\"";
        const char *poster_window_end = id_end + 3000;
        const char *pp = strstr(name_end, poster_key);

        CatalogItem *item = &g_items[g_item_count];
        copy_json_string(item->id, sizeof(item->id), id_start, id_end);
        copy_json_string(item->name, sizeof(item->name), np, name_end);
        strncpy(item->type, type_label, sizeof(item->type) - 1);
        item->type[sizeof(item->type) - 1] = 0;
        item->poster_url[0] = 0;

        if (pp && pp < poster_window_end) {
            pp += strlen(poster_key);
            const char *purl_end = pp;
            while (*purl_end && *purl_end != '"') {
                if (*purl_end == '\\' && *(purl_end + 1)) purl_end++;
                purl_end++;
            }
            copy_json_string(item->poster_url, sizeof(item->poster_url), pp, purl_end);
            p = purl_end;
        } else {
            p = name_end;
        }

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

// Downloads + decodes one poster. Called at most once per item; safe to call
// repeatedly, it no-ops once a state has already been set.
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
    SDL_Surface *surf = rw ? IMG_Load_RW(rw, 1) : NULL; // frees rw itself
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

    g_poster_tex[idx] = tex;
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

// Draws text clipped to a max width (used for titles under posters).
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

    int img_flags = IMG_INIT_JPG | IMG_INIT_PNG;
    int img_ok = IMG_Init(img_flags);
    if ((img_ok & img_flags) != img_flags) {
        logmsg("IMG_Init warning: got 0x%x wanted 0x%x : %s", img_ok, img_flags, IMG_GetError());
    } else {
        logmsg("IMG_Init ok");
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

    SDL_Color white     = {255, 255, 255, 255};
    SDL_Color gray      = {160, 160, 160, 255};
    SDL_Color dim_gray  = {70, 70, 78, 255};
    SDL_Color highlight = {40, 120, 220, 255};

    if (!win || !ren) {
        logmsg("Aborting: window/renderer not available.");
        if (font) TTF_CloseFont(font);
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

    // NOTE: curl_global_cleanup() / socketExit() moved to the very end now -
    // poster downloads during the main loop still need both.

    int cursor = 0;
    int scroll_row = 0;
    int running = 1;
    int show_account_popup = 0;
    int frame = 0;

    int margin_x = (SCREEN_W - (GRID_COLS * TILE_W + (GRID_COLS - 1) * GAP_X)) / 2;
    int row_stride = TILE_H + LABEL_H + GAP_Y;
    int rows_visible = (SCREEN_H - GRID_TOP - FOOTER_H) / row_stride;
    if (rows_visible < 1) rows_visible = 1;

    while (running && appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_Plus) running = 0;

        if (show_account_popup) {
            if (kDown & (HidNpadButton_B | HidNpadButton_Y)) show_account_popup = 0;
        } else {
            if (kDown & HidNpadButton_Y) show_account_popup = 1;

            if (g_item_count > 0) {
                int total_rows = (g_item_count + GRID_COLS - 1) / GRID_COLS;

                if (kDown & HidNpadButton_Down) {
                    if (cursor + GRID_COLS < g_item_count) cursor += GRID_COLS;
                }
                if (kDown & HidNpadButton_Up) {
                    if (cursor - GRID_COLS >= 0) cursor -= GRID_COLS;
                }
                if (kDown & HidNpadButton_Right) {
                    if ((cursor % GRID_COLS) != GRID_COLS - 1 && cursor + 1 < g_item_count) cursor++;
                }
                if (kDown & HidNpadButton_Left) {
                    if ((cursor % GRID_COLS) != 0 && cursor > 0) cursor--;
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

        // Lazy-load: at most one new poster per frame, from what's currently visible.
        if (g_item_count > 0) {
            int start_idx = scroll_row * GRID_COLS;
            int end_idx = start_idx + rows_visible * GRID_COLS;
            if (end_idx > g_item_count) end_idx = g_item_count;
            for (int i = start_idx; i < end_idx; i++) {
                if (g_poster_state[i] == 0) {
                    load_poster(ren, i);
                    break;
                }
            }
        }

        SDL_SetRenderDrawColor(ren, 15, 15, 20, 255);
        SDL_RenderClear(ren);

        // top bar
        if (font) {
            char header[128];
            snprintf(header, sizeof(header), "switchmio  -  %d titles", g_item_count);
            draw_text(ren, font, header, margin_x, 24, white);
            draw_text(ren, font, "Account: Not signed in", SCREEN_W - margin_x - 260, 24, gray);
        }

        if (g_item_count == 0) {
            if (font) draw_text(ren, font, "No titles loaded - check your network connection.", margin_x, 120, white);
        } else {
            int start_idx = scroll_row * GRID_COLS;
            int end_idx = start_idx + rows_visible * GRID_COLS;
            if (end_idx > g_item_count) end_idx = g_item_count;

            for (int i = start_idx; i < end_idx; i++) {
                int local_i = i - start_idx;
                int col = local_i % GRID_COLS;
                int row = local_i / GRID_COLS;

                int x = margin_x + col * (TILE_W + GAP_X);
                int y = GRID_TOP + row * row_stride;

                SDL_Rect poster_rect = { x, y, TILE_W, TILE_H };

                if (g_poster_state[i] == 2 && g_poster_tex[i]) {
                    SDL_RenderCopy(ren, g_poster_tex[i], NULL, &poster_rect);
                } else {
                    SDL_SetRenderDrawColor(ren, dim_gray.r, dim_gray.g, dim_gray.b, 255);
                    SDL_RenderFillRect(ren, &poster_rect);
                    if (g_poster_state[i] == 1 && font) {
                        draw_text_clipped(ren, font, "no image", x + 8, y + TILE_H / 2 - 10, TILE_W - 16, gray);
                    }
                }

                if (i == cursor) {
                    SDL_SetRenderDrawColor(ren, highlight.r, highlight.g, highlight.b, 255);
                    SDL_Rect border = { x - 4, y - 4, TILE_W + 8, TILE_H + 8 };
                    for (int b = 0; b < 3; b++) {
                        SDL_Rect ring = { border.x - b, border.y - b, border.w + b * 2, border.h + b * 2 };
                        SDL_RenderDrawRect(ren, &ring);
                    }
                }

                if (font) {
                    SDL_Color label_color = (i == cursor) ? white : gray;
                    draw_text_clipped(ren, font, g_items[i].name, x, y + TILE_H + 4, TILE_W, label_color);
                }
            }
        }

        // footer hint bar
        if (font) {
            SDL_SetRenderDrawColor(ren, 25, 25, 32, 255);
            SDL_Rect footer = { 0, SCREEN_H - FOOTER_H, SCREEN_W, FOOTER_H };
            SDL_RenderFillRect(ren, &footer);
            draw_text(ren, font, "D-Pad: move   A: select   Y: account   +: exit", margin_x, SCREEN_H - FOOTER_H + 12, gray);
        }

        if (show_account_popup && font) {
            int pw = 500, ph = 180;
            int px = (SCREEN_W - pw) / 2, py = (SCREEN_H - ph) / 2;
            SDL_SetRenderDrawColor(ren, 30, 30, 40, 255);
            SDL_Rect box = { px, py, pw, ph };
            SDL_RenderFillRect(ren, &box);
            SDL_SetRenderDrawColor(ren, highlight.r, highlight.g, highlight.b, 255);
            SDL_RenderDrawRect(ren, &box);
            draw_text(ren, font, "Account login", px + 24, py + 24, white);
            draw_text(ren, font, "Coming in the next update.", px + 24, py + 64, gray);
            draw_text(ren, font, "B: close", px + 24, py + ph - 40, gray);
        }

        SDL_RenderPresent(ren);

        frame++;
        if (frame == 1 || frame == 60 || frame == 300) {
            logmsg("frame %d presented", frame);
        }
    }

    logmsg("Exiting main loop, total frames=%d", frame);

    for (int i = 0; i < g_item_count; i++) {
        if (g_poster_tex[i]) SDL_DestroyTexture(g_poster_tex[i]);
    }

    if (font) TTF_CloseFont(font);
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
