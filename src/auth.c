#include "spotc.h"
#include <curl/curl.h>
#include <openssl/evp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#define TOKEN_URL "https://accounts.spotify.com/api/token"
#define SCOPES "user-read-playback-state user-modify-playback-state " \
               "user-read-currently-playing playlist-read-private " \
               "playlist-read-collaborative user-library-read"
#define SCOPES_CSV "user-read-playback-state,user-modify-playback-state," \
                   "user-read-currently-playing,playlist-read-private," \
                   "playlist-read-collaborative,user-library-read"

struct buf { char *p; size_t n; };

static size_t wr(void *data, size_t sz, size_t nm, void *ud) {
    struct buf *b = ud;
    size_t len = sz * nm;
    char *grown = realloc(b->p, b->n + len + 1);
    if (!grown) return 0;   /* aborts the transfer */
    b->p = grown;
    memcpy(b->p + b->n, data, len);
    b->n += len;
    b->p[b->n] = 0;
    return len;
}

static void b64url(const unsigned char *in, size_t n, char *out) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = in[i] << 16;
        if (i + 1 < n) v |= in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        if (i + 1 < n) out[o++] = t[(v >> 6) & 63];
        if (i + 2 < n) out[o++] = t[v & 63];
    }
    out[o] = 0;
}

static int rand_bytes(unsigned char *buf, size_t n) {
    FILE *f = fopen("/dev/urandom", "r");
    if (!f) return -1;
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    return got == n ? 0 : -1;
}

static char *tokens_path(void) {
    static char p[600];
    snprintf(p, sizeof p, "%s/tokens.json", spotc_dir());
    return p;
}

static const char *effective_cid(const App *a) {
    return a->cfg.client_id[0] ? a->cfg.client_id : DEFAULT_CLIENT_ID;
}

int tokens_load(Tokens *t) {
    memset(t, 0, sizeof *t);
    json_object *root = json_object_from_file(tokens_path());
    if (!root) return -1;
    json_object *v;
    if (json_object_object_get_ex(root, "access_token", &v))
        snprintf(t->access, sizeof t->access, "%s", json_object_get_string(v));
    if (json_object_object_get_ex(root, "refresh_token", &v))
        snprintf(t->refresh, sizeof t->refresh, "%s", json_object_get_string(v));
    if (json_object_object_get_ex(root, "client_id", &v))
        snprintf(t->cid, sizeof t->cid, "%s", json_object_get_string(v));
    if (json_object_object_get_ex(root, "expires_at", &v))
        t->expires_at = json_object_get_int64(v);
    json_object_put(root);
    return t->refresh[0] ? 0 : -1;
}

void tokens_save(const Tokens *t) {
    json_object *root = json_object_new_object();
    json_object_object_add(root, "access_token", json_object_new_string(t->access));
    json_object_object_add(root, "refresh_token", json_object_new_string(t->refresh));
    json_object_object_add(root, "client_id", json_object_new_string(t->cid));
    json_object_object_add(root, "expires_at", json_object_new_int64(t->expires_at));
    const char *path = tokens_path();
    json_object_to_file_ext(path, root, JSON_C_TO_STRING_PRETTY);
    chmod(path, 0600);
    json_object_put(root);
}

/* POST form to accounts.spotify.com, parse token response into t */
static int token_request(Tokens *t, const char *form) {
    CURL *c = curl_easy_init();
    if (!c) return -1;
    struct buf b = {0};
    curl_easy_setopt(c, CURLOPT_URL, TOKEN_URL);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, form);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    CURLcode rc = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK || http != 200 || !b.p) { free(b.p); return -1; }

    json_object *root = json_tokener_parse(b.p);
    free(b.p);
    if (!root) return -1;
    json_object *v;
    int ok = -1;
    if (json_object_object_get_ex(root, "access_token", &v)) {
        snprintf(t->access, sizeof t->access, "%s", json_object_get_string(v));
        ok = 0;
    }
    if (json_object_object_get_ex(root, "refresh_token", &v))
        snprintf(t->refresh, sizeof t->refresh, "%s", json_object_get_string(v));
    if (json_object_object_get_ex(root, "expires_in", &v))
        t->expires_at = time(NULL) + json_object_get_int(v);
    json_object_put(root);
    return ok;
}

/* tiny one-shot HTTP server: waits for GET /callback?code=... , max wait_s */
static int catch_code(char *code, size_t code_n, int wait_s) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return -1;
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(REDIRECT_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(srv, 1) < 0) {
        close(srv);
        return -1;
    }

    int got = -1;
    time_t deadline = time(NULL) + wait_s;
    while (time(NULL) < deadline && got != 0) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(srv, &fds);
        struct timeval tv = { 1, 0 };
        if (select(srv + 1, &fds, NULL, NULL, &tv) <= 0) continue;
        int cl = accept(srv, NULL, NULL);
        if (cl < 0) continue;
        char req[4096] = {0};
        ssize_t n = read(cl, req, sizeof req - 1);
        (void)n;
        char *q = strstr(req, "GET /login?");
        const char *body;
        if (q && (q = strstr(q, "code="))) {
            q += 5;
            size_t i = 0;
            while (q[i] && q[i] != '&' && q[i] != ' ' && i < code_n - 1) {
                code[i] = q[i];
                i++;
            }
            code[i] = 0;
            got = 0;
            body = "<html><body style=\"background:#111;color:#ddd;font-family:sans-serif;"
                   "display:flex;align-items:center;justify-content:center;height:100vh\">"
                   "<div style=\"text-align:center\"><h2 style=\"color:#b48ead\">spotc</h2>"
                   "<p>Logged in. You can close this tab.</p></div></body></html>";
        } else {
            body = "<html><body>spotc: login failed or was denied.</body></html>";
        }
        char resp[1024];
        int len = snprintf(resp, sizeof resp,
            "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %zu\r\n"
            "Connection: close\r\n\r\n%s", strlen(body), body);
        ssize_t w = write(cl, resp, len);
        (void)w;
        close(cl);
    }
    close(srv);
    return got;
}

int auth_login(App *a) {
    const char *cid = effective_cid(a);
    unsigned char rnd[32], digest[32];
    char verifier[64], challenge[64];
    if (rand_bytes(rnd, sizeof rnd)) return -1;
    b64url(rnd, sizeof rnd, verifier);
    unsigned int dn = 0;
    EVP_Digest(verifier, strlen(verifier), digest, &dn, EVP_sha256(), NULL);
    b64url(digest, dn, challenge);

    CURL *c = curl_easy_init();
    char *scopes = curl_easy_escape(c, SCOPES, 0);
    char *redir = curl_easy_escape(c, REDIRECT_URI, 0);
    char url[1024];
    snprintf(url, sizeof url,
        "https://accounts.spotify.com/authorize?client_id=%s&response_type=code"
        "&redirect_uri=%s&scope=%s&code_challenge_method=S256&code_challenge=%s",
        cid, redir, scopes, challenge);
    curl_free(scopes);
    curl_free(redir);
    curl_easy_cleanup(c);

    char cmd[1200];
    snprintf(cmd, sizeof cmd, "xdg-open '%s' >/dev/null 2>&1 &", url);
    int rc = system(cmd);
    (void)rc;

    char code[512];
    if (catch_code(code, sizeof code, 180) != 0) return -1;

    char form[1400];
    snprintf(form, sizeof form,
        "grant_type=authorization_code&code=%s&redirect_uri=%s"
        "&client_id=%s&code_verifier=%s",
        code, "http%3A%2F%2F127.0.0.1%3A5588%2Flogin", cid, verifier);
    if (token_request(&a->tok, form) != 0) return -1;
    snprintf(a->tok.cid, sizeof a->tok.cid, "%s", cid);
    tokens_save(&a->tok);
    return 0;
}

int auth_have_creds(App *a) {
    (void)a;
    char p[700];
    snprintf(p, sizeof p, "%s/librespot/credentials.json", cache_dir());
    return access(p, R_OK) == 0;
}

void auth_logout(App *a) {
    char creds[700];
    snprintf(creds, sizeof creds, "%s/librespot/credentials.json", cache_dir());
    unlink(tokens_path());
    unlink(creds);
    memset(&a->tok, 0, sizeof a->tok);
}

/* mint a Web API token through the librespot session (spotc-token helper);
 * this is the path official clients use, so it avoids shared-app rate limits */
static int session_token(App *a) {
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
        "spotc-token '%s/librespot' '%s' 2>'%s/token.log'",
        cache_dir(), SCOPES_CSV, cache_dir());
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    char line[600] = {0};
    if (!fgets(line, sizeof line, p)) { pclose(p); return -1; }
    if (pclose(p) != 0) return -1;
    char access[512];
    long expires = 0;
    if (sscanf(line, "%511s %ld", access, &expires) != 2 || expires <= 0) return -1;
    snprintf(a->tok.access, sizeof a->tok.access, "%s", access);
    a->tok.expires_at = time(NULL) + expires;
    return 0;
}

int auth_ensure(App *a) {
    if (a->tok.access[0] && time(NULL) < a->tok.expires_at - 60) return 0;
    if (!a->cfg.client_id[0])
        return auth_have_creds(a) ? session_token(a) : -1;
    if (!a->tok.refresh[0]) return -1;
    char form[1024];
    snprintf(form, sizeof form,
        "grant_type=refresh_token&refresh_token=%s&client_id=%s",
        a->tok.refresh, a->tok.cid[0] ? a->tok.cid : effective_cid(a));
    if (token_request(&a->tok, form) != 0) return -1;
    tokens_save(&a->tok);
    return 0;
}
