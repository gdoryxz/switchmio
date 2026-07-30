// switchmio - Native Stremio catalog browser for Nintendo Switch
// Fetches the "Top Movies" catalog from Stremio's official Cinemeta addon
// over plain HTTP(S) using libcurl, and lists titles on screen.
// This is a NATIVE app - no web browser involved.

#include <switch.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CATALOG_URL "https://v3-cinemeta.strem.io/catalog/movie/top.json"
#define MAX_TITLES 30

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

// Very small, targeted extractor: pulls out each "name":"..." value
// from the JSON response. Not a full JSON parser - just enough for
// this specific catalog format.
static int extract_titles(const char *json, char titles[][256], int max_titles) {
    int count = 0;
    const char *p = json;
    const char *key = "\"name\":\"";
    size_t keylen = strlen(key);

    while (count < max_titles) {
        p = strstr(p, key);
        if (!p) break;
        p += keylen;

        const char *end = p;
        while (*end && *end != '"') {
            if (*end == '\\' && *(end + 1)) end++; // skip escaped chars
            end++;
        }

        size_t len = (size_t)(end - p);
        if (len >= 256) len = 255;
        memcpy(titles[count], p, len);
        titles[count][len] = 0;
        count++;

        p = end;
    }
    return count;
}

int main(int argc, char *argv[]) {
    consoleInit(NULL);

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    printf("switchmio - Native Stremio Catalog Browser\n");
    printf("Fetching top movies from Cinemeta...\n\n");
    consoleUpdate(NULL);

    // Bring up networking (required before any socket/curl usage)
    Result rc = socketInitializeDefault();
    if (R_FAILED(rc)) {
        printf("Failed to init network: 0x%x\n", rc);
        consoleUpdate(NULL);
        goto wait_and_exit;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    MemBuf chunk = { .data = malloc(1), .size = 0 };

    CURL *curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, CATALOG_URL);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "switchmio/1.0");
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); // simplifies cert handling on-device
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        CURLcode res = curl_easy_perform(curl);

        if (res != CURLE_OK) {
            printf("Request failed: %s\n", curl_easy_strerror(res));
        } else {
            static char titles[MAX_TITLES][256];
            int count = extract_titles(chunk.data, titles, MAX_TITLES);

            if (count == 0) {
                printf("No titles found (unexpected response format).\n");
            } else {
                printf("Top Movies:\n\n");
                for (int i = 0; i < count; i++) {
                    printf("%2d. %s\n", i + 1, titles[i]);
                }
            }
        }
        curl_easy_cleanup(curl);
    } else {
        printf("Failed to init curl\n");
    }

    free(chunk.data);
    curl_global_cleanup();
    socketExit();

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
