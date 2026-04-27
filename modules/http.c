// modules/http.c — built-in `http` module: synchronous HTTP/HTTPS
// client backed by libcurl. (TLS handling, redirects, gzip — all
// inherited from libcurl.)
//
// Exports:
//   http.get(url)              -> dict {status: int, body: string}
//   http.post(url, body)       -> dict {status: int, body: string}
//   http.request(method, url, body) -> dict
//
// The dict shape lets callers do `r = http.get(url); r["status"];
// r["body"]`.
//
// Build dependency: libcurl (linked via -lcurl in the main Makefile).

#include "../include/mymo_module.h"
#include "../datatypes/dict.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    char  *data;
    size_t len;
    size_t cap;
} Buf;

static size_t write_cb(char *src, size_t size, size_t n, void *userdata)
{
    size_t add = size * n;
    Buf *b = (Buf *)userdata;
    if (b->len + add + 1 > b->cap) {
        size_t newcap = b->cap ? b->cap : 1024;
        while (newcap < b->len + add + 1) newcap *= 2;
        char *p = realloc(b->data, newcap);
        if (!p) return 0;
        b->data = p;
        b->cap  = newcap;
    }
    memcpy(b->data + b->len, src, add);
    b->len += add;
    b->data[b->len] = '\0';
    return add;
}

static MyMoObject *make_response(MVM *vm, long status, const char *body, size_t blen)
{
    MyMoDict *d = newDict(vm);
    setEntry(vm, d,
             AS_OBJECT(newString(vm, "status", 6)),
             mymo_int(vm, status));
    setEntry(vm, d,
             AS_OBJECT(newString(vm, "body", 4)),
             body ? mymo_strn(vm, body, (int)blen) : mymo_str(vm, ""));
    return AS_OBJECT(d);
}

static MyMoObject *do_request(MVM *vm, const char *funcname,
                              const char *method, const char *url,
                              const char *body, size_t blen)
{
    CURL *h = curl_easy_init();
    if (!h) {
        runtimeError(vm, "%s(): curl_easy_init failed", funcname);
        return MYMO_ERROR;
    }

    Buf out = {0};
    curl_easy_setopt(h, CURLOPT_URL,            url);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL,       1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA,      &out);
    curl_easy_setopt(h, CURLOPT_USERAGENT,      "mymo-http/1.0");

    struct curl_slist *headers = NULL;
    if (strcmp(method, "GET") != 0) {
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method);
    }
    if (body) {
        curl_easy_setopt(h, CURLOPT_POSTFIELDS,     body);
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE,  (long)blen);
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(h, CURLOPT_HTTPHEADER,     headers);
    }

    CURLcode rc = curl_easy_perform(h);
    long status = 0;
    if (rc == CURLE_OK)
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(h);

    if (rc != CURLE_OK) {
        free(out.data);
        runtimeError(vm, "%s(): %s", funcname, curl_easy_strerror(rc));
        return MYMO_ERROR;
    }

    MyMoObject *resp = make_response(vm, status, out.data, out.len);
    free(out.data);
    return resp;
}

static MyMoObject *http_get(MVM *vm, uint argc, MyMoObject *argv[])
{
    const char *url;
    if (!mymo_parse(vm, "http.get", argc, argv, "s", &url)) return MYMO_ERROR;
    return do_request(vm, "http.get", "GET", url, NULL, 0);
}

static MyMoObject *http_post(MVM *vm, uint argc, MyMoObject *argv[])
{
    const char *url, *body; int blen;
    if (!mymo_parse(vm, "http.post", argc, argv, "ssn", &url, &body, &blen))
        return MYMO_ERROR;
    return do_request(vm, "http.post", "POST", url, body, (size_t)blen);
}

static MyMoObject *http_request(MVM *vm, uint argc, MyMoObject *argv[])
{
    const char *method, *url, *body; int blen;
    if (!mymo_parse(vm, "http.request", argc, argv, "sssn",
                    &method, &url, &body, &blen))
        return MYMO_ERROR;
    return do_request(vm, "http.request", method, url,
                      blen > 0 ? body : NULL, (size_t)blen);
}

MyMoObject *httpModule(MVM *vm)
{
    static int curl_inited = 0;
    if (!curl_inited) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_inited = 1;
    }
    static MyMoModuleFunction fns[] = {
        {"get",     http_get},
        {"post",    http_post},
        {"request", http_request},
    };
    static MyMoModuleVariable vars[] = { {0, 0} };
    static MyMoModuleDef def = {
        "http", fns, vars,
        sizeof(fns) / sizeof(fns[0]), 0,
    };
    return defineBuiltInModule(vm, &def);
}
