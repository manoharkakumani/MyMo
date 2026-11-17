// modules/http.c — built-in `_http` module: the libcurl engine under the
// `http` standard library module (stdlib/http.my), which is what scripts
// import.
//
// Each request runs on its own worker thread so the interpreter never
// blocks on the network. When it finishes, the thread writes a byte to the
// request's pipe, so an event loop can wait for it like any socket (mono
// and ui park the calling fiber on that fd).
//
// Exports:
//   _http.start(method, url, body, headers, timeout_s) -> handle (int)
//       body: string or Nil; headers: dict of name -> value or Nil
//   _http.fd(handle)      -> the fd that becomes readable when it's done
//   _http.done(handle)    -> bool
//   _http.wait(handle)    -> Nil, after blocking until it's done
//   _http.result(handle)  -> {status, ok, body, headers, url, error};
//                            frees the handle
//   _http.hosting(on)     -> Nil: an event loop is (not) stepping fibers
//   _http.can_yield()     -> True inside a fiber run by such a loop
//
// Without pthreads (Windows builds) start() does the request inline.
//
// Build dependency: libcurl (linked via -lcurl in the main Makefile).

#include "../include/mymo_module.h"
#include "../datatypes/dict.h"
#include "../datatypes/fiber.h"
#include "../repr.h"
#include <curl/curl.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
  #include <pthread.h>
  #include <unistd.h>
  #include <fcntl.h>
  #define HTTP_THREADS 1
#endif

typedef struct {
    char *data;
    size_t len, cap;
} Buf;

static void buf_add(Buf *b, const char *src, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = realloc(b->data, cap);
        if (!p) return;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    b->data[b->len] = '\0';
}

typedef struct {
    // Request (owned copies; the thread must not touch VM objects).
    char *method, *url, *body;
    size_t blen;
    struct curl_slist *headers;
    long timeout_ms;
    // Response.
    Buf out, head;
    long status;
    char *effective_url;
    char error[CURL_ERROR_SIZE];
    volatile int done;
    int pipe[2];
#ifdef HTTP_THREADS
    pthread_t thread;
#endif
} Job;

// Jobs are separately allocated (a worker thread holds a pointer to its
// own), indexed by handle.
static Job **g_jobs = NULL;
static int g_jobs_cap = 0;
static bool g_hosting = false;

static size_t write_cb(char *src, size_t size, size_t n, void *userdata)
{
    buf_add((Buf *)userdata, src, size * n);
    return size * n;
}

// Keep only the last response's headers (redirects send several).
static size_t header_cb(char *src, size_t size, size_t n, void *userdata)
{
    Buf *b = (Buf *)userdata;
    if (size * n >= 5 && memcmp(src, "HTTP/", 5) == 0)
        b->len = 0;
    buf_add(b, src, size * n);
    return size * n;
}

static void perform(Job *j)
{
    CURL *h = curl_easy_init();
    if (!h) {
        snprintf(j->error, sizeof j->error, "curl_easy_init failed");
        return;
    }
    curl_easy_setopt(h, CURLOPT_URL, j->url);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, j->timeout_ms);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, j->timeout_ms < 10000 ? j->timeout_ms : 10000L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, ""); // gzip/deflate/br if built in
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &j->out);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &j->head);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "mymo-http/2.0");
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, j->error);
    if (strcmp(j->method, "GET") != 0)
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, j->method);
    if (strcmp(j->method, "HEAD") == 0)
        curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
    if (j->body) {
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, j->body);
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)j->blen);
    }
    if (j->headers)
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, j->headers);
    CURLcode rc = curl_easy_perform(h);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &j->status);
        char *eff = NULL;
        curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &eff);
        if (eff) j->effective_url = strdup(eff);
        j->error[0] = '\0';
    } else if (j->error[0] == '\0') {
        snprintf(j->error, sizeof j->error, "%s", curl_easy_strerror(rc));
    }
    curl_easy_cleanup(h);
}

#ifdef HTTP_THREADS
static void *run_job(void *arg)
{
    Job *j = (Job *)arg;
    perform(j);
    __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
    char c = 1;
    ssize_t w = write(j->pipe[1], &c, 1);
    (void)w;
    return NULL;
}
#endif

static Job *job_at(MVM *vm, const char *fn, long handle)
{
    if (handle < 0 || handle >= g_jobs_cap || !g_jobs[handle]) {
        runtimeError(vm, "ValueError: %s(): invalid request handle", fn);
        return NULL;
    }
    return g_jobs[handle];
}

static long new_job(void)
{
    int slot = -1;
    for (int i = 0; i < g_jobs_cap && slot < 0; i++)
        if (!g_jobs[i])
            slot = i;
    if (slot < 0) {
        slot = g_jobs_cap;
        g_jobs_cap = g_jobs_cap ? g_jobs_cap * 2 : 16;
        g_jobs = realloc(g_jobs, sizeof(Job *) * (size_t)g_jobs_cap);
        memset(g_jobs + slot, 0, sizeof(Job *) * (size_t)(g_jobs_cap - slot));
    }
    g_jobs[slot] = calloc(1, sizeof(Job));
    return slot;
}

static void free_job(long h)
{
    Job *j = g_jobs[h];
    free(j->method);
    free(j->url);
    free(j->body);
    free(j->out.data);
    free(j->head.data);
    free(j->effective_url);
    if (j->headers) curl_slist_free_all(j->headers);
#ifdef HTTP_THREADS
    if (j->pipe[0] > 0) close(j->pipe[0]);
    if (j->pipe[1] > 0) close(j->pipe[1]);
#endif
    free(j);
    g_jobs[h] = NULL;
}

static Value http_start(MVM *vm, uint argc, Value argv[])
{
    // Checks the count and pops the arguments (argv is our own copy).
    if (!mymo_check_args(vm, "_http.start", argc, 5)) return MYMO_ERROR;
    if (!V_IS_OBJ_TYPE(argv[0], OBJ_STRING) || !V_IS_OBJ_TYPE(argv[1], OBJ_STRING)) {
        runtimeError(vm, "TypeError: _http.start(): method and url must be strings");
        return MYMO_ERROR;
    }
    long h = new_job();
    Job *j = g_jobs[h];
    j->method = strdup(AS_STRING(V_AS_OBJ(argv[0]))->value);
    j->url = strdup(AS_STRING(V_AS_OBJ(argv[1]))->value);
    if (V_IS_OBJ_TYPE(argv[2], OBJ_STRING)) {
        MyMoString *b = AS_STRING(V_AS_OBJ(argv[2]));
        j->body = malloc((size_t)b->length + 1);
        memcpy(j->body, b->value, (size_t)b->length + 1);
        j->blen = (size_t)b->length;
    }
    if (V_IS_OBJ_TYPE(argv[3], OBJ_DICT)) {
        Entry *e;
        DICT_FOREACH(AS_DICT(V_AS_OBJ(argv[3])), e) {
            if (e->key->type != OBJ_STRING) continue;
            Value text = valueToStr(vm, e->value);
            if (V_IS_EMPTY(text)) continue;
            const char *name = AS_STRING(e->key)->value;
            const char *val = AS_STRING(V_AS_OBJ(text))->value;
            if (strpbrk(name, "\r\n:") || strpbrk(val, "\r\n")) {
                runtimeError(vm, "ValueError: invalid HTTP header '%s'", name);
                free_job(h);
                return MYMO_ERROR;
            }
            size_t n = strlen(name) + strlen(val) + 3;
            char *line = malloc(n);
            snprintf(line, n, "%s: %s", name, val);
            j->headers = curl_slist_append(j->headers, line);
            free(line);
        }
    }
    double timeout = valueLooksLikeNumber(argv[4]) ? valueAsNumber(argv[4]) : 30.0;
    j->timeout_ms = timeout > 0 ? (long)(timeout * 1000) : 30000L;
#ifdef HTTP_THREADS
    if (pipe(j->pipe) != 0) {
        free_job(h);
        runtimeError(vm, "OSError: _http.start(): pipe failed");
        return MYMO_ERROR;
    }
    fcntl(j->pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(j->pipe[1], F_SETFD, FD_CLOEXEC);
    if (pthread_create(&j->thread, NULL, run_job, j) != 0) {
        free_job(h);
        runtimeError(vm, "OSError: _http.start(): can't start a thread");
        return MYMO_ERROR;
    }
    pthread_detach(j->thread);
#else
    perform(j);
    j->done = 1;
#endif
    return V_INT_VAL((int32_t)h);
}

static Value http_fd(MVM *vm, uint argc, Value argv[])
{
    long h;
    if (!mymo_parse(vm, "_http.fd", argc, argv, "i", &h)) return MYMO_ERROR;
    Job *j = job_at(vm, "_http.fd", h);
    if (!j) return MYMO_ERROR;
#ifdef HTTP_THREADS
    return V_INT_VAL(j->pipe[0]);
#else
    return V_INT_VAL(-1);
#endif
}

static Value http_done(MVM *vm, uint argc, Value argv[])
{
    long h;
    if (!mymo_parse(vm, "_http.done", argc, argv, "i", &h)) return MYMO_ERROR;
    Job *j = job_at(vm, "_http.done", h);
    if (!j) return MYMO_ERROR;
    return MYMO_BOOL(__atomic_load_n(&j->done, __ATOMIC_ACQUIRE) != 0);
}

static Value http_wait(MVM *vm, uint argc, Value argv[])
{
    long h;
    if (!mymo_parse(vm, "_http.wait", argc, argv, "i", &h)) return MYMO_ERROR;
    Job *j = job_at(vm, "_http.wait", h);
    if (!j) return MYMO_ERROR;
#ifdef HTTP_THREADS
    while (!__atomic_load_n(&j->done, __ATOMIC_ACQUIRE)) {
        char c;
        if (read(j->pipe[0], &c, 1) < 0)
            break;
    }
#endif
    return MYMO_NIL;
}

static void set_str(MVM *vm, MyMoDict *d, const char *key, const char *s, size_t n)
{
    setEntry(vm, d, AS_OBJECT(newString(vm, key, (int)strlen(key))), s ? mymo_strn(vm, s, (int)n) : mymo_str(vm, ""));
}

// "Name: value" lines -> {"name": "value"} (lower-case names).
static MyMoDict *parse_headers(MVM *vm, Buf *head)
{
    MyMoDict *d = newDict(vm);
    if (!head->data) return d;
    char *p = head->data, *end = head->data + head->len;
    while (p < end) {
        char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) eol = end;
        char *colon = memchr(p, ':', (size_t)(eol - p));
        if (colon && memcmp(p, "HTTP/", 5) != 0) {
            size_t nlen = (size_t)(colon - p);
            char *name = malloc(nlen + 1);
            for (size_t i = 0; i < nlen; i++) name[i] = (char)tolower((unsigned char)p[i]);
            name[nlen] = '\0';
            char *v = colon + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            char *ve = eol;
            while (ve > v && (ve[-1] == '\r' || ve[-1] == ' ')) ve--;
            setEntry(vm, d, AS_OBJECT(newString(vm, name, (int)nlen)), mymo_strn(vm, v, (int)(ve - v)));
            free(name);
        }
        p = eol + 1;
    }
    return d;
}

static Value http_result(MVM *vm, uint argc, Value argv[])
{
    long h;
    if (!mymo_parse(vm, "_http.result", argc, argv, "i", &h)) return MYMO_ERROR;
    Job *j = job_at(vm, "_http.result", h);
    if (!j) return MYMO_ERROR;
    if (!__atomic_load_n(&j->done, __ATOMIC_ACQUIRE)) {
        runtimeError(vm, "RuntimeError: _http.result(): the request is still running");
        return MYMO_ERROR;
    }
    MyMoDict *d = newDict(vm);
    setEntry(vm, d, AS_OBJECT(newString(vm, "status", 6)), mymo_int(vm, j->status));
    setEntryV(vm, d, AS_OBJECT(newString(vm, "ok", 2)), V_BOOL_VAL(j->status >= 200 && j->status < 300));
    set_str(vm, d, "body", j->out.data, j->out.len);
    setEntry(vm, d, AS_OBJECT(newString(vm, "headers", 7)), AS_OBJECT(parse_headers(vm, &j->head)));
    const char *url = j->effective_url ? j->effective_url : j->url;
    set_str(vm, d, "url", url, strlen(url));
    if (j->error[0])
        set_str(vm, d, "error", j->error, strlen(j->error));
    else
        setEntryV(vm, d, AS_OBJECT(newString(vm, "error", 5)), V_NIL_VAL);
    free_job(h);
    return objectToValue(AS_OBJECT(d));
}

static Value http_hosting(MVM *vm, uint argc, Value argv[])
{
    int on;
    if (!mymo_parse(vm, "_http.hosting", argc, argv, "b", &on)) return MYMO_ERROR;
    g_hosting = on != 0;
    return MYMO_NIL;
}

static Value http_can_yield(MVM *vm, uint argc, Value argv[])
{
    if (!mymo_check_args(vm, "_http.can_yield", argc, 0)) return MYMO_ERROR;
#ifdef HTTP_THREADS
    return MYMO_BOOL(g_hosting && vm->fiber->parent != NULL);
#else
    return MYMO_BOOL(false);
#endif
}

MyMoObject *httpModule(MVM *vm)
{
    static int curl_inited = 0;
    if (!curl_inited) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_inited = 1;
    }
    static MyMoModuleFunction fns[] = {
        {"start",     http_start},
        {"fd",        http_fd},
        {"done",      http_done},
        {"wait",      http_wait},
        {"result",    http_result},
        {"hosting",   http_hosting},
        {"can_yield", http_can_yield},
    };
    static MyMoModuleVariable vars[] = { {0, 0} };
    static MyMoModuleDef def = {
        "_http", fns, vars,
        sizeof(fns) / sizeof(fns[0]), 0,
    };
    return defineBuiltInModule(vm, &def);
}
