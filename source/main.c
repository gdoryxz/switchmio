// switchmio - Native Stremio catalog browser for Nintendo Switch
// Fetches the FULL "Top Movies" and "Top Series" catalogs from Stremio's
// official Cinemeta addon (paginated, not just the first page), and lets
// you scroll the combined list with the D-pad.
// This is a NATIVE app - no web browser involved.

#include <switch.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOVIE_URL_FMT  "https://v3-cinemeta.strem.io/catalog/movie/top/skip=%d.json"
#define SERIES_URL_FMT "https://v3-cinemeta.strem.io/catalog/series/top/skip=%d.json"
#define PAGE_SIZE      100
#define MAX_ITEMS      600   // hard cap so we don't grow unbounded
#define VISIBLE_ROWS   28    // how many lines fit on the Switch console at once

typedef struct {
    char id[128];
    char name[256];
    char type[16]; // "movie" or "series"
} CatalogItem;

static CatalogItem g_items[MAX_ITEMS];
static int g_item_count = 0;

// Growable buffer to receive the HTTP response body
typedef struct {
    char *data;
    size_t size;
} MemBuf;

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

// Fetches one URL into `out`. Returns 0 on success, non-zero curl error otherwise.
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
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); // simplifies cert handling on-device
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // accept any encoding, auto-decode

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return res;
}

// Small, targeted extractor: for each item object in the "metas" array,
// pulls "id" and "name". Not a full JSON parser - just enough for this
// specific catalog response shape. Returns how many items it appended
// to g_items (starting at g_item_count), capped by MAX_ITEMS.
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

        // Look for the matching "name" within the next 1000 chars
        // (i.e. still inside the same JSON object)
        const char *window_end = id_end + 1000;
        const char *name_key = "\"name\":\"";
        const char *np = strstr(id_end, name_key);
        if (!np || np > window_end) {
            p = id_end;
            continue; // no nearby name field, skip this "id" match
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

// Fetches every page of a catalog (movie or series) until a short page
// (< PAGE_SIZE items) signals the end, or we hit MAX_ITEMS.
static void fetch_full_catalog(const char *url_fmt, const char *type_label) {
    int skip = 0;
    char last_raw[301] = {0};
    size_t last_size = 0;

    while (g_item_count < MAX_ITEMS) {
        char url[256];
        snprintf(url, sizeof(url), url_fmt, skip);

        MemBuf chunk;
        CURLcode res = fetch_url(url, &chunk);

        if (res != CURLE_OK) {
            printf("  [%s skip=%d] request failed: %s\n", type_label, skip, curl_easy_strerror(res));
            free(chunk.data);
            break;
        }

        last_size = chunk.size;
        size_t copy_len = chunk.size < 300 ? chunk.size : 300;
        memcpy(last_raw, chunk.data, copy_len);
        last_raw[copy_len] = 0;

        int added = extract_items(chunk.data, type_label);
        free(chunk.data);

        if (added == 0) {
            printf("  [%s skip=%d] no items parsed (raw bytes: %zu)\n", type_label, skip, last_size);
            printf("  first 300 chars: %.300s\n", last_raw);
            break;
        }

        printf("  [%s] fetched skip=%d, +%d items (total %d)\n", type_label, skip, added, g_item_count);
        consoleUpdate(NULL);

        if (added < PAGE_SIZE) break; // last page
        skip += PAGE_SIZE;
    }
}

static void draw_list(int scroll_offset, int cursor) {
    consoleClear();
    printf("switchmio - %d titles loaded (movies + series)\n", g_item_count);
    printf("D-Pad: scroll   +: exit\n\n");

    int end = scroll_offset + VISIBLE_ROWS;
    if (end > g_item_count) end = g_item_count;

    for (int i = scroll_offset; i < end; i++) {
        const char *marker = (i == cursor) ? ">" : " ";
        printf("%s %3d. [%s] %s\n", marker, i + 1, g_items[i].type, g_items[i].name);
    }
    consoleUpdate(NULL);
}

int main(int argc, char *argv[]) {
    consoleInit(NULL);

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    printf("switchmio - Native Stremio Catalog Browser\n");
    printf("Fetching full movie + series catalogs from Cinemeta...\n\n");
    consoleUpdate(NULL);

    Result rc = socketInitializeDefault();
    if (R_FAILED(rc)) {
        printf("Failed to init network: 0x%x\n", rc);
        consoleUpdate(NULL);
        goto wait_and_exit;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    printf("Movies:\n");
    consoleUpdate(NULL);
    fetch_full_catalog(MOVIE_URL_FMT, "movie");

    printf("\nSeries:\n");
    consoleUpdate(NULL);
    fetch_full_catalog(SERIES_URL_FMT, "series");

    curl_global_cleanup();
    socketExit();

    if (g_item_count == 0) {
        printf("\nNo titles loaded at all. Press + to exit.\n");
        consoleUpdate(NULL);
        goto wait_and_exit;
    }

    printf("\nLoaded %d titles total. Press A to browse.\n", g_item_count);
    consoleUpdate(NULL);

    // Wait for A before switching into browse mode, so the fetch log stays
    // readable for a moment.
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        if (kDown & HidNpadButton_A) break;
        if (kDown & HidNpadButton_Plus) goto wait_and_exit;
        consoleUpdate(NULL);
    }

    // Browse mode: D-pad up/down moves cursor, list scrolls to follow it.
    {
        int cursor = 0;
        int scroll_offset = 0;
        draw_list(scroll_offset, cursor);

        while (appletMainLoop()) {
            padUpdate(&pad);
            u64 kDown = padGetButtonsDown(&pad);

            int moved = 0;
            if (kDown & HidNpadButton_AnyDown) {
                if (cursor < g_item_count - 1) { cursor++; moved = 1; }
            }
            if (kDown & HidNpadButton_AnyUp) {
                if (cursor > 0) { cursor--; moved = 1; }
            }
            if (kDown & HidNpadButton_Plus) break;

            if (moved) {
                if (cursor < scroll_offset) scroll_offset = cursor;
                if (cursor >= scroll_offset + VISIBLE_ROWS) scroll_offset = cursor - VISIBLE_ROWS + 1;
                draw_list(scroll_offset, cursor);
            }

            consoleUpdate(NULL);
        }
    }

    consoleExit(NULL);
    return 0;

wait_and_exit:
    printf("\nPress + to exit.\n");
    consoleUpdate(NULL);

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        if (kDown & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }

    consoleExit(NULL);
    return 0;
}
