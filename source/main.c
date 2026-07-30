// switchmio - Local HTTP server + system browser front-end
// Runs a tiny HTTP server on 127.0.0.1:8080 inside the .nro, serving a
// custom-built (simple, old-browser-friendly) poster grid page instead of
// Stremio's real web app. The Switch's built-in browser then just renders
// http://127.0.0.1:8080/ - no external hosting needed.
//
// DEBUG BUILD: logs every init step to sdmc:/switch/switchmio/log.txt

#include <switch.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define MOVIE_URL_FMT  "https://v3-cinemeta.strem.io/catalog/movie/top/skip=%d.json"
#define SERIES_URL_FMT "https://v3-cinemeta.strem.io/catalog/series/top/skip=%d.json"
#define PAGE_SIZE      100
#define MAX_ITEMS      600
#define SERVER_PORT    8080

typedef struct {
    char id[128];
    char name[256];
    char type[16];
    char poster_url[300];
    char description[600];
} CatalogItem;

static CatalogItem g_items[MAX_ITEMS];
static int g_item_count = 0;

// ---------- debug log ----------
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
// --------------------------------

// ---------- growable buffer, reused for both curl responses and HTTP responses we build ----------
typedef struct {
    char *data;
    size_t size;
    size_t cap;
} DynBuf;

static void dynbuf_init(DynBuf *b) {
    b->cap = 256;
    b->data = malloc(b->cap);
    b->size = 0;
    if (b->data) b->data[0] = 0;
}

static void dynbuf_append(DynBuf *b, const char *s, size_t len) {
    if (b->size + len + 1 > b->cap) {
        while (b->size + len + 1 > b->cap) b->cap *= 2;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->size, s, len);
    b->size += len;
    b->data[b->size] = 0;
}

static void dynbuf_append_str(DynBuf *b, const char *s) {
    dynbuf_append(b, s, strlen(s));
}

static void dynbuf_append_json_escaped(DynBuf *b, const char *s) {
    dynbuf_append(b, "\"", 1);
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            char esc[2] = { '\\', *p };
            dynbuf_append(b, esc, 2);
        } else if (*p == '\n') {
            dynbuf_append(b, "\\n", 2);
        } else if ((unsigned char)*p < 0x20) {
            // skip other control chars
        } else {
            dynbuf_append(b, p, 1);
        }
    }
    dynbuf_append(b, "\"", 1);
}
// ---------------------------------------------------------------------------------------------

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    DynBuf *mem = (DynBuf *)userp;
    dynbuf_append(mem, (const char *)contents, realsize);
    return realsize;
}

static CURLcode fetch_url(const char *url, DynBuf *out) {
    dynbuf_init(out);

    CURL *curl = curl_easy_init();
    if (!curl) return CURLE_FAILED_INIT;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
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

// ---------- Cinemeta catalog fetch + parse (same approach as the native-UI version) ----------
static void copy_json_string(char *dest, size_t dest_size, const char *start, const char *end) {
    size_t di = 0;
    const char *p = start;
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
    while (*vend && *vend != '"') {
        if (*vend == '\\' && *(vend + 1)) vend++;
        vend++;
    }
    *out_end = vend;
    return kp;
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

        const char *name_end;
        const char *np = find_json_string_field(id_end, "\"name\":\"", 1000, &name_end);
        if (!np) { p = id_end; continue; }

        CatalogItem *item = &g_items[g_item_count];
        copy_json_string(item->id, sizeof(item->id), id_start, id_end);
        copy_json_string(item->name, sizeof(item->name), np, name_end);
        strncpy(item->type, type_label, sizeof(item->type) - 1);
        item->type[sizeof(item->type) - 1] = 0;
        item->poster_url[0] = 0;
        item->description[0] = 0;

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

        DynBuf chunk;
        CURLcode res = fetch_url(url, &chunk);
        if (res != CURLE_OK) {
            logmsg("fetch_url failed for %s : curl code %d", url, res);
            free(chunk.data);
            break;
        }

        int added = extract_items(chunk.data, type_label);
        logmsg("fetched %s skip=%d -> +%d items (total=%d)", type_label, skip, added, g_item_count);
        free(chunk.data);

        if (added == 0 || added < PAGE_SIZE) break;
        skip += PAGE_SIZE;
    }
}
// -----------------------------------------------------------------------------------------------

// ---------- the page itself: plain HTML/CSS/JS, deliberately simple/old-browser-friendly ----------
static const char *INDEX_HTML =
"<!doctype html><html><head><meta charset='utf-8'><title>switchmio</title>\n"
"<style>\n"
"body{background:#121218;color:#eee;font-family:sans-serif;margin:0;padding:20px}\n"
"h1{color:#0ab9e6;margin:0 0 16px 0}\n"
".grid{display:flex;flex-wrap:wrap;gap:14px}\n"
".card{width:140px;cursor:pointer}\n"
".card img{width:140px;height:210px;object-fit:cover;background:#333;border-radius:6px;border:2px solid transparent}\n"
".card:hover img{border-color:#0ab9e6}\n"
".title{font-size:13px;margin-top:6px;color:#ccc;text-align:center}\n"
"#detail{display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.9);padding:40px;box-sizing:border-box;overflow:auto}\n"
"#detail.show{display:block}\n"
"#detail img{width:220px;float:left;margin-right:24px;border-radius:6px}\n"
"#detail h2{color:#fff;margin-top:0}\n"
"#detail p{color:#bbb;max-width:600px}\n"
"#closeBtn{background:#ff3c2d;color:#fff;border:none;padding:10px 20px;border-radius:4px;font-size:16px;cursor:pointer;clear:both}\n"
"</style></head><body>\n"
"<h1>switchmio</h1>\n"
"<div class='grid' id='grid'>Loading...</div>\n"
"<div id='detail'>\n"
"<img id='detailImg'>\n"
"<h2 id='detailTitle'></h2>\n"
"<p id='detailDesc'></p>\n"
"<button id='closeBtn' onclick='closeDetail()'>Close</button>\n"
"</div>\n"
"<script>\n"
"fetch('/api/catalog').then(function(r){return r.json();}).then(function(items){\n"
"  var grid=document.getElementById('grid');\n"
"  grid.innerHTML='';\n"
"  for (var i=0;i<items.length;i++){\n"
"    (function(it){\n"
"      var card=document.createElement('div');\n"
"      card.className='card';\n"
"      card.onclick=function(){ showDetail(it); };\n"
"      var img=document.createElement('img');\n"
"      img.src='/poster?id='+encodeURIComponent(it.id);\n"
"      var title=document.createElement('div');\n"
"      title.className='title';\n"
"      title.textContent=it.name;\n"
"      card.appendChild(img);\n"
"      card.appendChild(title);\n"
"      grid.appendChild(card);\n"
"    })(items[i]);\n"
"  }\n"
"}).catch(function(e){\n"
"  document.getElementById('grid').textContent='Failed to load catalog.';\n"
"});\n"
"function showDetail(it){\n"
"  document.getElementById('detailImg').src='/poster?id='+encodeURIComponent(it.id);\n"
"  document.getElementById('detailTitle').textContent=it.name;\n"
"  document.getElementById('detailDesc').textContent=it.description||'';\n"
"  document.getElementById('detail').className='show';\n"
"}\n"
"function closeDetail(){\n"
"  document.getElementById('detail').className='';\n"
"}\n"
"</script></body></html>\n";
// -----------------------------------------------------------------------------------------------

static void send_response(int client, int status, const char *status_text, const char *content_type, const char *body, size_t body_len) {
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        status, status_text, content_type, body_len);
    send(client, header, hlen, 0);
    if (body_len > 0) send(client, body, body_len, 0);
}

static void handle_catalog_request(int client) {
    DynBuf out;
    dynbuf_init(&out);
    dynbuf_append_str(&out, "[");
    for (int i = 0; i < g_item_count; i++) {
        if (i > 0) dynbuf_append_str(&out, ",");
        dynbuf_append_str(&out, "{\"id\":");
        dynbuf_append_json_escaped(&out, g_items[i].id);
        dynbuf_append_str(&out, ",\"name\":");
        dynbuf_append_json_escaped(&out, g_items[i].name);
        dynbuf_append_str(&out, ",\"type\":");
        dynbuf_append_json_escaped(&out, g_items[i].type);
        dynbuf_append_str(&out, ",\"description\":");
        dynbuf_append_json_escaped(&out, g_items[i].description);
        dynbuf_append_str(&out, "}");
    }
    dynbuf_append_str(&out, "]");

    send_response(client, 200, "OK", "application/json", out.data, out.size);
    free(out.data);
}

static void handle_poster_request(int client, const char *query) {
    // query looks like "id=tt1234567" (possibly URL-encoded, we do a minimal decode)
    char id[128] = {0};
    const char *idp = strstr(query, "id=");
    if (idp) {
        idp += 3;
        int di = 0;
        while (*idp && *idp != '&' && di + 1 < (int)sizeof(id)) {
            if (*idp == '%' && idp[1] && idp[2]) {
                char hex[3] = { idp[1], idp[2], 0 };
                id[di++] = (char)strtol(hex, NULL, 16);
                idp += 3;
            } else if (*idp == '+') {
                id[di++] = ' ';
                idp++;
            } else {
                id[di++] = *idp++;
            }
        }
        id[di] = 0;
    }

    for (int i = 0; i < g_item_count; i++) {
        if (strcmp(g_items[i].id, id) == 0 && g_items[i].poster_url[0]) {
            DynBuf img;
            CURLcode res = fetch_url(g_items[i].poster_url, &img);
            if (res == CURLE_OK && img.size > 0) {
                send_response(client, 200, "OK", "image/jpeg", img.data, img.size);
            } else {
                send_response(client, 502, "Bad Gateway", "text/plain", "poster fetch failed", 20);
            }
            free(img.data);
            return;
        }
    }
    send_response(client, 404, "Not Found", "text/plain", "not found", 9);
}

static void handle_client(int client) {
    char req[2048];
    int n = recv(client, req, sizeof(req) - 1, 0);
    if (n <= 0) return;
    req[n] = 0;

    // parse "METHOD /path?query HTTP/1.1"
    char method[8] = {0}, path[512] = {0};
    sscanf(req, "%7s %511s", method, path);

    char *qmark = strchr(path, '?');
    char query[512] = {0};
    if (qmark) {
        strncpy(query, qmark + 1, sizeof(query) - 1);
        *qmark = 0;
    }

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        send_response(client, 200, "OK", "text/html", INDEX_HTML, strlen(INDEX_HTML));
    } else if (strcmp(path, "/api/catalog") == 0) {
        handle_catalog_request(client);
    } else if (strcmp(path, "/poster") == 0) {
        handle_poster_request(client, query);
    } else {
        send_response(client, 404, "Not Found", "text/plain", "not found", 9);
    }
}

static volatile bool g_server_running = true;

static void server_thread_func(void *arg) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        logmsg("server: socket() failed");
        return;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(SERVER_PORT);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        logmsg("server: bind() failed on port %d", SERVER_PORT);
        close(listen_fd);
        return;
    }
    if (listen(listen_fd, 4) < 0) {
        logmsg("server: listen() failed");
        close(listen_fd);
        return;
    }

    logmsg("server: listening on 127.0.0.1:%d", SERVER_PORT);

    while (g_server_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listen_fd, &fds);
        struct timeval tv = { 0, 200000 }; // 200ms poll, so we can exit cleanly

        int sel = select(listen_fd + 1, &fds, NULL, NULL, &tv);
        if (sel > 0 && FD_ISSET(listen_fd, &fds)) {
            int client = accept(listen_fd, NULL, NULL);
            if (client >= 0) {
                handle_client(client);
                close(client);
            }
        }
    }

    close(listen_fd);
    logmsg("server: stopped");
}

int main(int argc, char *argv[]) {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/switchmio", 0777);
    g_log = fopen("sdmc:/switch/switchmio/log.txt", "w");
    logmsg("=== switchmio (local server + browser) starting ===");

    consoleInit(NULL);
    printf("switchmio\nLoading catalog...\n");
    consoleUpdate(NULL);

    Result rc = socketInitializeDefault();
    logmsg("socketInitializeDefault: %s (0x%x)", R_FAILED(rc) ? "FAILED" : "ok", rc);
    if (R_FAILED(rc)) {
        printf("Network init failed.\n");
        consoleUpdate(NULL);
        svcSleepThread(3e9);
        consoleExit(NULL);
        if (g_log) fclose(g_log);
        return 1;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);

    fetch_full_catalog(MOVIE_URL_FMT, "movie");
    fetch_full_catalog(SERIES_URL_FMT, "series");
    logmsg("Catalog fetch done, total items=%d", g_item_count);

    printf("Loaded %d titles. Starting local server...\n", g_item_count);
    consoleUpdate(NULL);

    Thread server_thread;
    Result trc = threadCreate(&server_thread, server_thread_func, NULL, NULL, 0x8000, 0x2C, -2);
    if (R_FAILED(trc)) {
        logmsg("threadCreate FAILED: 0x%x", trc);
        printf("Failed to start local server.\n");
        consoleUpdate(NULL);
        svcSleepThread(3e9);
    } else {
        threadStart(&server_thread);
        svcSleepThread(500e6); // give the server a moment to bind before opening the browser

        char url[64];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/", SERVER_PORT);
        logmsg("Opening web applet at %s", url);

        printf("\nLocal server running at:\n");
        printf("  %s\n", url);
        printf("  port: %d\n\n", SERVER_PORT);
        printf("Opening browser...\n");
        consoleUpdate(NULL);
        svcSleepThread(1500e6); // brief pause so the address is actually readable before the browser takes over

        WebCommonConfig config;
        Result wrc = webPageCreate(&config, url);
        if (R_SUCCEEDED(wrc)) {
            webConfigSetWhitelist(&config, "^http*");
            webConfigSetFooter(&config, true);
            WebCommonReply reply;
            wrc = webConfigShow(&config, &reply);
            if (R_FAILED(wrc)) logmsg("webConfigShow FAILED: 0x%x", wrc);
        } else {
            logmsg("webPageCreate FAILED: 0x%x", wrc);
        }

        g_server_running = false;
        threadWaitForExit(&server_thread);
        threadClose(&server_thread);
    }

    curl_global_cleanup();
    socketExit();
    consoleExit(NULL);
    if (g_log) fclose(g_log);
    return 0;
}
