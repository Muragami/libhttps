/*
    Simple https dynamic library based on https://github.com/erkkah/naett
    Jason A. Petrasko 2022, MIT License
*/
#define _DEFAULT_SOURCE 1

#include "https.h"
#include "src/xthread.h"
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <memory.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>

// maximum simultaneous requests allowed
#define MAX_REQUEST 128
// maximum headers allowed in a request
#define MAX_HEADERS 256
// maximum number of possible fixed buffers (and never ever more than 65536)
#define MAX_FIXED_BUFFERS 128

#define BUFFER_USE_BIT  0x10000000
#define BUFFER_ID(x)    (x & 0x0FFFFFFF)

httpsMemoryInterface mem = { malloc, calloc, realloc, free };

typedef struct _headers {
    char *str[MAX_HEADERS*2];
    int last;
} headers;

typedef struct _httpsReq {
    void *request;
    void *res;
    pthread_mutex_t mutex;
    int index;
    int flags;
    char *URL;
    memBuffer buffer;
    bool complete;
    bool finished;
    bool headerDone;
    int returnCode;
    unsigned int readTotalBytes;
    unsigned int bodyTotalBytes;
    unsigned int contentTotalBytes;
    char *contentMimeType;
    char *body;
    void *userData;
    httpsHeaderLister lister;
    httpsFlush flush;
    // metrics
    double startTime;
} httpsReq;

typedef struct _httpsContext {
    unsigned int bufferSize;
    unsigned long bufferBytes;
    int requestCount;
    int persistentBufferCount;
    pthread_mutex_t mainLock;
    httpsReq* requestTable[MAX_REQUEST];
    memBuffer* persistentBuffer;
    httpsReq requestBacker[MAX_REQUEST];
    httpsFlush flush;
} httpsContext;

httpsContext con = { 0, 0, 0, 0 };

static inline double _getSeconds() {
    struct timeval currentTime;
    gettimeofday(&currentTime, NULL);
    return (double)currentTime.tv_sec + (double)currentTime.tv_usec * 0.0000001;
}

httpsReq* _newHttpsReq(int flags) {
    httpsReq* req = NULL;
    int i;

    pthread_mutex_lock(&con.mainLock);

    for (i = 0; i < MAX_REQUEST; i++)
        if (con.requestTable[i] == NULL) break;

    if (i == MAX_REQUEST) {
        pthread_mutex_unlock(&con.mainLock);
        return NULL;
    }
    // pull an allocated request and make it live in the system
    req = con.requestTable[i] = &con.requestBacker[i];
    // properly configure the request
    req->index = i;
    req->flags = flags;
    if (flags & HTTPS_FIXED_BUFFER) {
        // we want a fixed buffer for this request, so reflect that
        req->buffer.data = mem.malloc(HTTPS_BUFFER_KB(flags));
        if (req->buffer.data == NULL) return NULL;
        req->buffer.length = HTTPS_BUFFER_KB(flags);
    } else {
        req->buffer.data = mem.malloc(con.bufferSize);
        if (req->buffer.data == NULL) return NULL;
        req->buffer.length = con.bufferSize;
    }
    con.bufferBytes += req->buffer.length;    
    pthread_mutex_init((pthread_mutex_t*)&req->mutex, NULL);
    req->headerDone = req->complete = req->finished = false;
    req->flush = con.flush;
    req->buffer.end = 0;
    req->readTotalBytes = 0;
    req->startTime = _getSeconds();
    req->returnCode = 0;
    req->bodyTotalBytes = 0;
    req->contentTotalBytes = 0;
    req->contentMimeType = NULL;
    req->userData = NULL;

    pthread_mutex_unlock(&con.mainLock);

    return req;
}

void _delHttpsReq(httpsReq *p) {
    con.requestTable[p->index] = NULL;
    // free read buffer
    free(p->buffer.data);
    con.bufferBytes -= p->buffer.length;
    // free the mutex
    pthread_mutex_destroy((pthread_mutex_t*)&p->mutex);
    // free the request itself
    naettClose((naettRes*)p->res);
    naettFree((naettReq*)p->request);
}

int _bodyWriter(const void* source, int bytes, void* userData) {
    httpsReq *r = (httpsReq*)userData;
    memBuffer *p = &r->buffer;
    const char* src = (const char*)source;
    r->headerDone = true;
    int toWrite = bytes;
    int nibble;
    while (toWrite > 0) {
        unsigned int b = p->length - p->end;
        if (toWrite > b) nibble = b;
            else nibble = toWrite;
        memcpy(p->data + p->end, src, nibble);
        toWrite -= nibble;
        src += nibble;
        p->end += nibble;
        r->readTotalBytes += nibble;
        if (p->end == p->length)
        {
            if (r->flags & HTTPS_REUSE_BUFFER) {
                // do a flush callback and then just reset this buffer
                r->flush(r->index, r->URL, r->userData, p);
                p->end = 0;
            }
            // if this is a fixed buffer we are done, so close out this writing call
            if (r->flags & HTTPS_FIXED_BUFFER) return 0;
            // just forever double?
            if HTTPS_DOUBLE_FOREVER(r->flags) {
                p->data = mem.realloc(p->data, p->length * 2);
                if (p->data == NULL) return 0;
                con.bufferBytes += p->length;
                p->length *= 2;
            } else {
                if (r->flags & HTTPS_DOUBLE_UNTIL) {
                    if (p->length < (HTTPS_BUFFER_KB(r->flags) * 1024)) {
                        p->data = mem.realloc(p->data, p->length * 2);
                        if (p->data == NULL) return 0;
                        p->length *= 2;
                        con.bufferBytes += (p->length >> 1);
                    } else {
                        p->data = mem.realloc(p->data, p->length + HTTPS_BUFFER_KB(r->flags) * 1024);
                        if (p->data == NULL) return 0;
                        p->length += HTTPS_BUFFER_KB(r->flags) * 1024;
                        con.bufferBytes += HTTPS_BUFFER_KB(r->flags) * 1024;
                    }
                }
            }
        }
    }
    return bytes;
}

void httpsSetFlushRoutine(httpsFlush f) {
    con.flush = f;
}

void httpsEnsurePersistentBuffers(int x) {
    x = HTTPS_PERSIST_ID(x);
    if (x >= con.persistentBufferCount) {
        mem.realloc(con.persistentBuffer, sizeof(memBuffer) * x);
        for (int i = con.persistentBufferCount; i < x; i++) {
            con.persistentBuffer[i].data = NULL;
            con.persistentBuffer[i].end = HTTPS_OPEN_BUFFER;
        }
        con.persistentBufferCount = x;
    }
}

int httpsAddPersistentBuffer(char *bmem, unsigned int bytes);
void httpsRemovePersistentBuffer(int id);

void httpsInit(httpsInitData init, unsigned int readBufferSize)
{
    // for some console debugging REMOVE
    // setvbuf (stdout, (char*)NULL, _IONBF, BUFSIZ);
    // set all memory in our context to zero
    memset(&con, 0, sizeof(httpsContext));
    // initialize
    naettInit((naettInitData)init);
    con.bufferSize = readBufferSize;
    if (con.bufferSize == 0) con.bufferSize = 16384;
    pthread_mutex_init(&con.mainLock, NULL);
}

void httpsCleanup()
{
    pthread_mutex_lock(&con.mainLock);
    for (int i = 0; i < MAX_REQUEST; i++)
    {
        httpsReq* r = con.requestTable[i];
        if (r != NULL) {
            if (r->complete && r->finished) {
                // delete it
                _delHttpsReq(r);
            } else {
                // force it to end
                naettClose((naettRes*)r->res);
                naettFree((naettReq*)r->request);
                con.requestTable[i] = NULL;
            }    
        }
    }
    pthread_mutex_unlock(&con.mainLock);
}

void httpsUpdate()
{
    pthread_mutex_lock(&con.mainLock);
    if (con.bufferSize == 0) httpsInit(NULL, 0);
    for (int i = 0; i < MAX_REQUEST; i++)
    {
        httpsReq* r = con.requestTable[i];
        if (r != NULL) {
            // if we are done totally, free the request so it can be deleted,
            // opening the slot it's taking
            if (r->complete && r->finished) {
                // delete it
                _delHttpsReq(r);
            } else {
                // update status on the response
                int rc, comp;
                rc = naettGetStatus(r->res);
                if (rc != r->returnCode) {
                    r->returnCode = rc;
                }
                if (r->headerDone) {
                    // might be valid, probe headers for content type and length
                    char *hval = (char*)naettGetHeader((naettRes*)r->res, "Content-Length");
                    if (hval != NULL) r->contentTotalBytes = atoi(hval);
                    r->contentMimeType = (char*)naettGetHeader((naettRes*)r->res, "Content-Type");
                }
                comp = naettComplete((naettRes*)r->res);
                if (comp != r->complete) {
                    r->complete = comp;    
                }    
            }
            
        }
    }
    pthread_mutex_unlock(&con.mainLock);
}

unsigned int httpsRequestCount()
{
    unsigned int ret = 0;
    pthread_mutex_lock(&con.mainLock);
    for (int i = 0; i < MAX_REQUEST; i++)
        if (con.requestTable[i] != NULL) ret++;
    pthread_mutex_unlock(&con.mainLock);
    return ret;
}

void* _makeRequest(httpsReq* r, const char *method, void* _headers, unsigned int bodyBytes, const char* body)
{
    if (_headers == NULL) {
        return (void*)naettRequest(r->URL, naettMethod(method), naettHeader("accept", "*/*"), naettBodyWriter(_bodyWriter, r));
    } else {
        headers *h = (headers*)_headers;
        naettOption* bopt = NULL;
        int l = h->last + 2;
        int x = 0;
        if (body != NULL)
        {
            l++;
            if (bodyBytes == 0) bodyBytes = strlen(body);
            bopt = naettBody(body, bodyBytes);
        }
        naettOption** opts = calloc(1,sizeof(naettOption*)*l);
        opts[x++] = naettMethod(method);
        opts[x++] = naettHeader("accept", "*/*");
        if (bopt != NULL) opts[x++] = bopt;
        for (int i = 0; i < h->last; i++)
            opts[x++] = naettHeader(h->str[i*2], h->str[i*2+1]);
        return (void*)naettRequestWithOptions(r->URL, l, (const naettOption**)opts);
    }
}

void* httpsGet(const char *URL, int flags,  void *headers)
{
    httpsReq* r;
    if ((con.bufferSize == 0) || (URL == NULL) || (strlen(URL) == 0)) return NULL;
    r = _newHttpsReq(flags);
    if (r == NULL) return NULL;
    r->URL = strdup(URL);
    r->request = _makeRequest(r, "GET", headers, 0, NULL);
    r->res = (void*)naettMake((naettReq*)r->request);
    r->complete = r->finished = false;
    return (void*)r;
}

void* httpsPost(const char *URL, int flags,  const char *body, unsigned int bodyBytes, void *headers)
{
    httpsReq* r;
    if ((con.bufferSize == 0) || (URL == NULL) || (strlen(URL) == 0)) return NULL;
    r = _newHttpsReq(flags);
    if (r == NULL) return NULL;
    r->URL = strdup(URL);
    if (bodyBytes == 0) {
        if ((body == NULL) || (strlen(body) == 0)) return NULL;
        r->body = strdup(body);
        r->bodyTotalBytes = strlen(body);
    } else
    {
        r->bodyTotalBytes = bodyBytes;
        r->body = malloc(bodyBytes);
        memcpy(r->body, body, bodyBytes);
    }
    r->request = _makeRequest(r, "POST", headers, bodyBytes, body);
    r->res = (void*)naettMake((naettReq*)r->request);
    r->complete = r->finished = false;
    return (void*)r;
}

void* httpsPostLinked(const char *URL, int flags,  const char *body, unsigned int bodyBytes, void *headers)
{
    httpsReq* r;
    if ((con.bufferSize == 0) || (URL == NULL) || (strlen(URL) == 0)) return NULL;
    r = _newHttpsReq(flags);
    if (r == NULL) return NULL;
    r->URL = strdup(URL);
    if (bodyBytes == 0) {
        if ((body == NULL) || (strlen(body) == 0)) return NULL;
        r->body = (char*)body;
        r->bodyTotalBytes = strlen(body);
    } else
    {
        r->bodyTotalBytes = bodyBytes;
        r->body = (char*)body;
    }
    r->request = _makeRequest(r, "POST", headers, bodyBytes, body);
    r->res = (void*)naettMake((naettReq*)r->request);
    r->complete = r->finished = false;
    return (void*)r;
}

void* httpsHead(const char *URL, int flags,  void *headers)
{
    httpsReq* r;
    if ((con.bufferSize == 0) || (URL == NULL) || (strlen(URL) == 0)) return NULL;
    r = _newHttpsReq(flags);
    if (r == NULL) return NULL;
    r->URL = strdup(URL);
    r->request = _makeRequest(r, "HEAD", headers, 0, NULL);
    r->res = (void*)naettMake((naettReq*)r->request);
    r->complete = r->finished = false;
    return (void*)r;    
}

int httpsGetCode(void *p)
{
    httpsReq *r = (httpsReq*)p;
    int ret;
    pthread_mutex_lock((pthread_mutex_t*)&r->mutex);
    ret = naettGetStatus((naettRes*)r);
    pthread_mutex_unlock((pthread_mutex_t*)&r->mutex);
    return ret;
}

int httpsGetCodeI(int i)
{
    httpsReq *r = con.requestTable[i];
    int ret;
    pthread_mutex_lock((pthread_mutex_t*)&r->mutex);
    ret = naettGetStatus((naettRes*)r);
    pthread_mutex_unlock((pthread_mutex_t*)&r->mutex);
    return ret;
}

const char* httpsGetHeader(void *p, const char *w)
{
    httpsReq *r = (httpsReq*)p;
    const char *ret;
    pthread_mutex_lock((pthread_mutex_t*)&r->mutex);
    ret = naettGetHeader((naettRes*)r->res, w);
    pthread_mutex_unlock((pthread_mutex_t*)&r->mutex);
    return ret;
}

int _HeaderLister(const char* name, const char* value, void* userData)
{
    httpsReq* r = (httpsReq*)userData;
    return r->lister(name, value, userData);
}

void httpsListHeaders(void *p, httpsHeaderLister lister)
{
    httpsReq *r = (httpsReq*)p;
    pthread_mutex_lock((pthread_mutex_t*)&r->mutex);
    r->lister = lister;
    naettListHeaders((naettRes*)r->res, _HeaderLister, (void*)r);
    pthread_mutex_unlock((pthread_mutex_t*)&r->mutex);
}

bool httpsIsComplete(void *p)
{
    httpsReq *r = (httpsReq*)p;
    bool ret = false;
    pthread_mutex_lock((pthread_mutex_t*)&r->mutex);
    ret = r->complete;
    pthread_mutex_unlock((pthread_mutex_t*)&r->mutex);
    return ret;
}

void httpsFinished(void *p)
{
    httpsReq *r = (httpsReq*)p;
    pthread_mutex_lock((pthread_mutex_t*)&r->mutex);
    r->finished = true;
    pthread_mutex_unlock((pthread_mutex_t*)&r->mutex);
}

void* httpsNewHeaders()
{
    headers *h = calloc(1,sizeof(headers));
    return (void*)h;
}

void httpsSetHeader(void *p, const char *name, const char *val)
{
    headers *h = (headers*)p;
    int i;
    for (i = 0; i < MAX_HEADERS; i++) {
        if (!strcmp(name,h->str[i*2])) {
            free(h->str[i*2+1]);
            h->str[i*2+1] = strdup(val);
        }
    }
    if ((i == MAX_HEADERS) && (h->last != MAX_HEADERS)) {
        i = h->last++;
        h->str[i*2] = strdup(name);
        h->str[i*2+1] = strdup(val);
    }
}

void httpsDelHeaders(void *p)
{
    headers *h = (headers*)p;
    for (int i = 0; i < h->last*2; i++)
        free(h->str[i]);
    free((headers*)h);
}

unsigned int httpsGetBodyLength(void *p)
{
    httpsReq *r = (httpsReq*)p;
    unsigned int ret;
    pthread_mutex_lock((pthread_mutex_t*)&r->mutex);
    ret = r->readTotalBytes;
    pthread_mutex_unlock((pthread_mutex_t*)&r->mutex);
    return ret;
}

void httpsGetBody(void *p, unsigned int maxBytes)
{
    unsigned int len = maxBytes;
    httpsReq *r = (httpsReq*)p;
    pthread_mutex_lock((pthread_mutex_t*)&r->mutex);
    if (len > r->buffer.end) len = r->buffer.end;
    memcpy(p, r->buffer.data, len);
    pthread_mutex_unlock((pthread_mutex_t*)&r->mutex);
}

void httpsRelease(void *p)
{
    httpsReq *r = (httpsReq*)p;
    pthread_mutex_lock((pthread_mutex_t*)&r->mutex);
    r->finished = true;
    pthread_mutex_unlock((pthread_mutex_t*)&r->mutex);
}

#ifdef _WIN32

void _stringSep(char* str, char** k, char** v)
{
    char delim[] = ":";
    char *state, *vv;
    *k = strtok_s(str, delim, &state);
    vv = strtok_s(NULL, delim, &state);
    while (isspace(*vv)) vv++;
    *v = vv;
}

#else

void _stringSep(char* str, char** k, char** v)
{
    char delim[] = ":";
    char *state, *vv;
    *k = strtok_r(str, delim, &state);
    vv = strtok_r(NULL, delim, &state);
    while (isspace(*vv)) vv++;
    *v = vv;
}

#endif

headers *_easyCreateHeaders(const char* *_headers, int header_count, bool compact)
{
    headers *h = calloc(1,sizeof(_headers));
    if (compact) {
        for (int i = 0; i < header_count; i++)
        {
            char *k, *v;
            char *tst = strdup(_headers[i]);
            _stringSep(tst, &k, &v);
            httpsSetHeader(h, k, v);
        }
    } else {
        for (int i = 0; i < header_count; i++)
        {
            httpsSetHeader(h, _headers[i*2], _headers[i*2+1]);
        }
    }
    return h;
}

const char* _easyGetHeader(int i, const char *header)
{
    httpsReq* r = con.requestTable[i];
    const char *ret;
    pthread_mutex_lock((pthread_mutex_t*)&r->mutex);
    ret = naettGetHeader((naettRes*)r->res, header);
    pthread_mutex_unlock((pthread_mutex_t*)&r->mutex);
    return ret;
}

easyCallback _theEasyCallback = NULL;
typedef struct _easyData {
    bool complete;
    bool headerDone;
    int returnCode;
    unsigned int readTotalBytes;
    unsigned int contentTotalBytes;
    char *contentMimeType;
} easyData;

typedef struct _easyMetric {
    int handle;
    const char *url;
    const char *mime;
    double startTime;
    double bytesPerSecond;
    double currentBytes;
    double totalBytes;
    double estimatedRemainingTime;
} easyMetric;

easyMetric _metricTable[MAX_REQUEST];
unsigned int _easyOptions = 0;
double _easyDelay = 0.0;

#define EASY_OPT_FLAGS      1
#define EASY_OPT_DELAY      2

#define EASY_FLAG_ALL       0xFFFFFFFF
#define EASY_METRICS        (_easyOptions & 0x0001)

#define EASY_METRIC_HANDLE      0
#define EASY_METRIC_URL         1
#define EASY_METRIC_MIME        2
#define EASY_METRIC_START       3
#define EASY_METRIC_RATE        4
#define EASY_METRIC_BYTES       5
#define EASY_METRIC_TOTALBYTES  6
#define EASY_METRIC_REMAINING   7
#define EASY_METRIC_RUNTIME     8

void easySetup(easyCallback cb, unsigned int bsize)
{
    httpsInit(NULL, bsize);
    _theEasyCallback = cb;
}

void easyListHeaders(int h, httpsHeaderLister lister)
{
    httpsListHeaders(con.requestTable[h], lister);
}

void easyOptionUI(unsigned int opt, unsigned int val) {
    switch (opt) {
        case EASY_OPT_FLAGS:
            _easyOptions = val;
            break;
        case EASY_OPT_DELAY:
            _easyDelay = (double)val * 0.0000001;
            break;
        default:
            break;
    }
}

void easyOptionD(unsigned int opt, double val) {
    switch (opt) {
        case EASY_OPT_FLAGS:
            _easyOptions = (unsigned int)val;
            break;
        case EASY_OPT_DELAY:
            _easyDelay = val;
            break;
        default:
            break;
    }
}

int easyHasMetrics(int i) {
    if ((i < 0) || (i > MAX_REQUEST)) return 0;
    return _metricTable[i].handle + 1;
}

int easyGetMetricI(int i, int w) {
    int secs;
    if ((i < 0) || (i > MAX_REQUEST)) return 0;
    switch (w) {
        case EASY_METRIC_HANDLE: return _metricTable[i].handle; break;
        case EASY_METRIC_BYTES: return (int)_metricTable[i].currentBytes; break;
        case EASY_METRIC_TOTALBYTES: return (int)_metricTable[i].totalBytes; break;
        case EASY_METRIC_RATE: return (int)_metricTable[i].bytesPerSecond; break;
        case EASY_METRIC_START: return (int)_metricTable[i].startTime; break;
        case EASY_METRIC_REMAINING: return (int)_metricTable[i].estimatedRemainingTime; break;
        case EASY_METRIC_RUNTIME: 
            secs = (int)_getSeconds();
            return secs - (int)_metricTable[i].startTime; 
            break;
    }
    return 0;
}

double easyGetMetricD(int i, int w) {
    double secs;
    if ((i < 0) || (i > MAX_REQUEST)) return 0;
    switch (w) {
        case EASY_METRIC_HANDLE: return (double)_metricTable[i].handle; break;
        case EASY_METRIC_BYTES: return _metricTable[i].currentBytes; break;
        case EASY_METRIC_TOTALBYTES: return _metricTable[i].totalBytes; break;
        case EASY_METRIC_RATE: return _metricTable[i].bytesPerSecond; break;
        case EASY_METRIC_START: return _metricTable[i].startTime; break;
        case EASY_METRIC_REMAINING: return _metricTable[i].estimatedRemainingTime; break;
        case EASY_METRIC_RUNTIME: 
            secs = _getSeconds();
            return secs - _metricTable[i].startTime; 
            break;
    }
    return 0;
}

const char *easyGetMetricS(int i, int w) {
    if ((i < 0) || (i > MAX_REQUEST)) return 0;
    switch (w) {
        case EASY_METRIC_URL: return _metricTable[i].url; break;
        case EASY_METRIC_MIME: return _metricTable[i].mime; break;
    }
    return NULL;
}

// void (*easyCallback)(int handle, const char* url, const char* msg, int code, unsigned int sz, void* data);
void easyUpdate()
{
    int mcnt = 0;
    double secs;
    httpsUpdate();
    pthread_mutex_lock(&con.mainLock);
    for (int i = 0; i < MAX_REQUEST; i++)
    {
        httpsReq* r = con.requestTable[i];
        if (r != NULL) {
            easyData *d = (easyData*)r->userData;
            // here we go, check for needed callbacks, etc.
            if (r->returnCode != d->returnCode)
            {
                // a change of state, so mark that and do the callback!
                _theEasyCallback(i, r->URL, "UPDATE", r->returnCode, 0, NULL);
                d->returnCode = r->returnCode;
            }
            if (r->headerDone != d->headerDone) {
                // we have all the headers!
                _theEasyCallback(i, r->URL, "HEADERS", r->returnCode, 0, (void*)_easyGetHeader);
                d->headerDone = r->headerDone;
            }
            if (r->contentTotalBytes != d->contentTotalBytes) {
                // we have size of the download, so let caller know
                _theEasyCallback(i, r->URL, "LENGTH", r->contentTotalBytes, 0, NULL);
                d->contentTotalBytes = r->contentTotalBytes;
            }
            if (r->contentMimeType != d->contentMimeType) {
                // we have mime type of the download, so let caller know
                _theEasyCallback(i, r->URL, "MIME", r->contentTotalBytes, strlen(r->contentMimeType), (void*)r->contentMimeType);
                d->contentMimeType = r->contentMimeType;
            }
            if (r->readTotalBytes != d->readTotalBytes) {
                // we read more bytes!
                _theEasyCallback(i, r->URL, "READ", r->readTotalBytes, 0, NULL);
                d->readTotalBytes = r->readTotalBytes;
            }
            if (r->complete != d->complete) {
                // response is complete, so let the caller know
                _theEasyCallback(i, r->URL, "COMPLETE", r->returnCode, r->buffer.end, (void*)&r->buffer);
                d->returnCode = r->returnCode;
                httpsRelease(r);
            }
        }
    }
    pthread_mutex_unlock(&con.mainLock);
    
    // are we doing metrics? if so update them
    if EASY_METRICS {
        secs = _getSeconds();
        pthread_mutex_lock(&con.mainLock);
        // empty the metric table (just mark every entry invalid)
        for (int i = 0; i < MAX_REQUEST; i++)
            _metricTable[i].handle = -1;
        // see what metrics we have to collect!
        for (int i = 0; i < MAX_REQUEST; i++)
        {
            httpsReq* r = con.requestTable[i];
            if (r != NULL) {
                _metricTable[mcnt].handle = i;
                _metricTable[mcnt].url = r->URL;
                _metricTable[mcnt].mime = r->contentMimeType;
                _metricTable[mcnt].startTime = r->startTime;
                _metricTable[mcnt].currentBytes = r->readTotalBytes;
                _metricTable[mcnt].totalBytes = r->contentTotalBytes;
                if (_metricTable[mcnt].currentBytes > 0.0) {
                    _metricTable[mcnt].bytesPerSecond = _metricTable[mcnt].currentBytes / (secs - _metricTable[mcnt].startTime);
                } else _metricTable[mcnt].bytesPerSecond = 0.0;
                if ((_metricTable[mcnt].totalBytes > 0.0) && (_metricTable[mcnt].bytesPerSecond > 0.0)) {
                    _metricTable[mcnt].estimatedRemainingTime = (_metricTable[mcnt].totalBytes - _metricTable[mcnt].currentBytes) / _metricTable[mcnt].bytesPerSecond;
                } else _metricTable[mcnt].estimatedRemainingTime = 0.0f;
            }
        }
        pthread_mutex_unlock(&con.mainLock);
    }

    // sleep for the request delay amount if we are being nice
    if (_easyDelay > 0.0) usleep((useconds_t)(_easyDelay * 1000000));
}

int easyGet(const char *URL, int flags, const char* *_headers, int header_count, bool header_compact)
{
    headers *h;
    httpsReq *r;
    if ((header_count > 0) && (_headers != NULL))
    {
        h = _easyCreateHeaders(_headers, header_count, header_compact);
        r = httpsGet(URL, flags, h);
        httpsDelHeaders(h);
    } else
        r = httpsGet(URL, flags, NULL);
    r->userData = calloc(1, sizeof(easyData));
    _theEasyCallback(r->index, r->URL, "START", r->returnCode, 0, NULL);
    return r->index;
}

int easyPost(const char *URL, int flags, const char *body, unsigned int bodyBytes, const char* *_headers, int header_count, bool header_compact)
{
    headers *h;
    httpsReq *r;
    if ((header_count > 0) && (_headers != NULL))
    {
        h = _easyCreateHeaders(_headers, header_count, header_compact);
        r = httpsPost(URL, flags, body, bodyBytes, h);
        httpsDelHeaders(h);
    } else
        r = httpsPost(URL, flags, body, bodyBytes, NULL);
    r->userData = calloc(1, sizeof(easyData));
    _theEasyCallback(r->index, r->URL, "START", r->returnCode, 0, NULL);
    return r->index;
}

int easyHead(const char *URL, int flags, const char* *_headers, int header_count, bool header_compact)
{
    headers *h;
    httpsReq *r;
    if ((header_count > 0) && (_headers != NULL))
    {
        h = _easyCreateHeaders(_headers, header_count, header_compact);
        r = httpsHead(URL, flags, h);
        httpsDelHeaders(h);
    } else
        r = httpsHead(URL, flags, NULL);
    r->userData = calloc(1, sizeof(easyData));
    _theEasyCallback(r->index, r->URL, "START", r->returnCode, 0, NULL);
    return r->index;
}

void easyShutdown()
{
    httpsCleanup();
}

lua_State* lState = NULL;
int _luaIdLocation = 51;

#define EASY_CB_START       1
#define EASY_CB_UPDATE      2
#define EASY_CB_HEADERS     3
#define EASY_CB_LENGTH      4
#define EASY_CB_MIME        5
#define EASY_CB_READ        6
#define EASY_CB_COMPLETE    7

void lua_getregtable(lua_State *L) {
    lua_pushlightuserdata(L, &_luaIdLocation);
    lua_rawget (L, LUA_REGISTRYINDEX);
}

void lua_assert_init(lua_State *L) {
    lua_rawgeti(L, -1, 1421421);
    if (lua_tointeger(L, -1) != 2422422) 
        luaL_error (L, "https.init() must be called before any other https function.");
    lua_pop(L, 1);
}

int lua_ReadHeader(lua_State* L)
{
    const char* (*GetHeader)(int i, const char *header);
    GetHeader = lua_touserdata(L, lua_upvalueindex(2));
    const char* ret = GetHeader(lua_tointeger(L, lua_upvalueindex(1)), lua_tostring(L, 1));
    if (ret != NULL) lua_pushstring(L, ret);
        else lua_pushnil(L);
    return 1;
}

void lua_Callback(int handle, const char* url, const char* msg, int code, unsigned int sz, void* data)
{
    int t = lua_gettop(lState);
    int cbm = 0;
    lua_getregtable(lState);
    lua_rawgeti (lState, -1, handle);
    if (lua_istable(lState,-1)) {
        // find the callback in the 
        if (!strcmp(msg,"START")) { lua_pushliteral(lState,"start"); cbm = EASY_CB_START; }
         else if (!strcmp(msg,"UPDATE")) { lua_pushliteral(lState,"update"); cbm = EASY_CB_UPDATE; }
         else if (!strcmp(msg,"HEADERS")) { lua_pushliteral(lState,"headers"); cbm = EASY_CB_HEADERS; }
         else if (!strcmp(msg,"LENGTH")) { lua_pushliteral(lState,"length"); cbm = EASY_CB_LENGTH; }
         else if (!strcmp(msg,"MIME")) { lua_pushliteral(lState,"mime"); cbm = EASY_CB_MIME; }
         else if (!strcmp(msg,"READ")) { lua_pushliteral(lState,"read"); cbm = EASY_CB_READ; }
         else if (!strcmp(msg,"COMPLETE")) { lua_pushliteral(lState,"complete"); cbm = EASY_CB_COMPLETE; }
         else lua_pushliteral(lState,"nope");
         lua_gettable(lState, -2);
         if (lua_isfunction(lState, -1)) {
            lua_pushvalue(lState, -2);  // push self for the :() calling convention
            lua_pushinteger(lState, handle);
            lua_pushstring(lState, url);
            lua_pushstring(lState, msg);
            lua_pushinteger(lState, code);
            lua_pushinteger(lState, sz);
            if (data) {
                if (cbm == EASY_CB_MIME) lua_pushstring(lState, (char*)data);
                if (cbm == EASY_CB_HEADERS) {
                    lua_pushinteger(lState, handle);
                    lua_pushlightuserdata(lState, data);
                    lua_pushcclosure(lState, lua_ReadHeader, 2);
                }
                if (cbm == EASY_CB_COMPLETE) {
                    lua_pushlightuserdata(lState, data);
                }
            } else lua_pushnil(lState);
            lua_call(lState, 7, 0);
         }
    }
    lua_settop(lState, t);
}

/* 
    https.update()

    called repeatedly to update the system and issue callbacks on requests,
    if https.options("EASY_OPT_DELAY", seconds) has been called this
    will delay that many seconds before returning
*/
int lua_Update(lua_State* L) {
    lua_getregtable(L);
    lua_assert_init(L);
    easyUpdate();
    return 0;
}

/* 
    https.get(url, callback, headers)

    url is a string with the url to be requested using http get.

    callback is a table object which gets callbacks from this request, as so:
        callback:name(vars)

    headers is an optional table of headers to pass to this request
        the string:string keys/values of the table only are sent as http headers
*/
int lua_Get(lua_State* L) {
    const char *head[MAX_HEADERS*2];
    int i = 0;
    int r = 0;
    const char *url = luaL_checklstring(L, 1, NULL);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (lua_istable(L, 3)) {
        // scan the table for string pairs, ignoring everything else
        lua_pushnil(L);
        while ((lua_next(L, 3) != 0) && (i < MAX_HEADERS)) {
            if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
                head[i*2] = lua_tolstring(L, -2, NULL);
                head[i*2+1] = lua_tolstring(L, -1, NULL);
                i++;
            }
            lua_pop(L, 1);
        }
        r = easyGet(url, 0, head, i, false);
    } else {
        r = easyGet(url, 0, NULL, 0, false);
    }
    lua_getregtable(L);
    lua_assert_init(L);
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, r);
    // see if the callback has a table called handle and if it does, add this handle to it
    lua_getfield(L, 2, "handle");
    if (lua_istable(L, -1)) {
        lua_pushinteger(L, r);
        lua_pushvalue(L, 1);
        lua_settable(L, -3);
    }
    lua_settop(L, 3);
    lua_pushinteger(L, r);
    return 1;
}

/* 
    https.get(url, body, callback, headers)

    url is a string with the url to be requested using http get.

    body is a string containing the body of the post.

    callback is a table object which gets callbacks from this request, as so:
        callback:name(vars)

    headers is an optional table of headers to pass to this request
        the string:string keys/values of the table only are sent as http headers
*/
int lua_Post(lua_State* L) {
    const char *head[MAX_HEADERS*2];
    int i = 0;
    int r = 0;
    const char *url = luaL_checklstring (L, 1, NULL);
    size_t bbytes;
    const char *body = luaL_checklstring (L, 2, &bbytes);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (lua_istable(L, 3)) {
        // scan the table for string pairs, ignoring everything else
        lua_pushnil(L);
        while ((lua_next(L, 3) != 0) && (i < MAX_HEADERS)) {
            if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
                head[i*2] = lua_tolstring(L, -2, NULL);
                head[i*2+1] = lua_tolstring(L, -1, NULL);
                i++;
            }
            lua_pop(L, 1);
        }
        r = easyPost(url, 0, body, bbytes, head, i, false);
    } else {
        r = easyPost(url, 0, body, bbytes, NULL, 0, false);
    }
    lua_getregtable(L);
    lua_assert_init(L);
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, r);
    // see if the callback has a table called handle and if it does, add this handle to it
    lua_getfield(L, 2, "handle");
    if (lua_istable(L, -1)) {
        lua_pushinteger(L, r);
        lua_pushvalue(L, 1);
        lua_settable(L, -3);
    }
    lua_settop(L, 3);
    lua_pushinteger(L, r);
    return 1;
}

/* 
    https.head(url, callback, headers)

    url is a string with the url to be requested using http head.

    callback is a table object which gets callbacks from this request, as so:
        callback:name(vars)

    headers is an optional table of headers to pass to this request
        the string:string keys/values of the table only are sent as http headers
*/
int lua_Head(lua_State* L) {
    const char *head[MAX_HEADERS*2];
    int i = 0;
    int r = 0;
    const char *url = luaL_checklstring (L, 1, NULL);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (lua_istable(L, 3)) {
        // scan the table for string pairs, ignoring everything else
        lua_pushnil(L);
        while ((lua_next(L, 3) != 0) && (i < MAX_HEADERS)) {
            if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
                head[i*2] = lua_tolstring(L, -2, NULL);
                head[i*2+1] = lua_tolstring(L, -1, NULL);
                i++;
            }
            lua_pop(L, 1);
        }
        r = easyHead(url, 0, head, i, false);
    } else {
        r = easyHead(url, 0, NULL, 0, false);
    }
    lua_getregtable(L);
    lua_assert_init(L);
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, r);
    // see if the callback has a table called handle and if it does, add this handle to it
    lua_getfield(L, 2, "handle");
    if (lua_istable(L, -1)) {
        lua_pushinteger(L, r);
        lua_pushvalue(L, 1);
        lua_settable(L, -3);
    }
    lua_settop(L, 3);
    lua_pushinteger(L, r);
    return 1;
}

/* 
    https.init(nil or bytes)

    called to initialize the system, must be done for any other calls into https

    if passed a number, that is the amount of buffer bytes used by requests,
    defaults to 16384 for 16kb
*/
int lua_Init(lua_State* L) {
    if (lua_isnumber (L, 1)) {
        easySetup(lua_Callback, lua_tointeger(L, 1));
    } else easySetup(lua_Callback, 0);
    lua_getregtable(L);
    lua_pushinteger(L, 2422422);
    lua_rawseti(L, -2, 1421421);
    lua_pop(L, 1);
    return 0;
}

/* 
    https.shutdown()

    close and clean up the system
*/
int lua_Shutdown(lua_State *L) {
    easyShutdown();
    return 0;
}

/* 
    https.response(handle)

    handle integer of the request

    returns the status code of the request
*/
int lua_Response(lua_State *L) {
    int i = luaL_checkinteger(L, 1);
    if ((i < 0) || (i >= MAX_REQUEST)) luaL_error(L, "https.response() attempt to index a request %d, outside range 0 - %d", i, MAX_REQUEST);
    lua_pushinteger(L, httpsGetCodeI(i));
    return 1;
}

/* 
    https.options(name, value)

    name is the name of the option to set.

    value can be an integer, double, or string (as needed by the option)
*/
int lua_Options(lua_State* L) {
    const char *n = luaL_checklstring (L, 1, NULL);
    if (!strcmp(n, "EASY_OPT_FLAGS")) {
        easyOptionUI(EASY_OPT_FLAGS, luaL_checkinteger(L, 2));
    } else if (!strcmp(n, "EASY_OPT_DELAY")) {
        easyOptionD(EASY_OPT_DELAY, luaL_checknumber(L, 2));
    } else {
        luaL_error(L, "Unsupported option name: %s", n);    
    }
    return 0;
}

/* 
    https.metrics(handle, table)

    handle integer of the request, populates table with the metrics of
    the request:

        url = url of the request
        mime = mime content type of the request
        start = time the request began
        rate = bytes per second
        bytes = bytes read so far
        totalbytes = total bytes to read
        remaining = estimated time remaining
        runtime = time running in seconds

    FYI: there is no guarantee that all of these will be available, since
    not all http servers will supply the proper data

    returns true if any metrics were found or false otherwise
*/
int lua_Metrics(lua_State* L) {
    // find if we have any metrics
    int h = -1;
    int s = luaL_checkinteger(L, 1);
    for (int i = 0; i < MAX_REQUEST; i++) {
        h = easyGetMetricI(i, EASY_METRIC_HANDLE);
        if (h == s) break;
    }
    if (h == -1) {
        // not found, return false
        lua_pushboolean(L, 0);
        return 1;
    }
    // found it, so let's populate the data
    luaL_checktype(L, 2, LUA_TTABLE);
    // we have a table, do the work
    lua_pushstring(L, easyGetMetricS(h, EASY_METRIC_URL)); lua_setfield(L, 2, "url");
    lua_pushstring(L, easyGetMetricS(h, EASY_METRIC_MIME)); lua_setfield(L, 2, "mime");
    lua_pushnumber(L, easyGetMetricD(h, EASY_METRIC_START)); lua_setfield(L, 2, "start");
    lua_pushnumber(L, easyGetMetricD(h, EASY_METRIC_RUNTIME)); lua_setfield(L, 2, "runtime");
    lua_pushnumber(L, easyGetMetricD(h, EASY_METRIC_REMAINING)); lua_setfield(L, 2, "remaining");
    lua_pushnumber(L, easyGetMetricD(h, EASY_METRIC_BYTES)); lua_setfield(L, 2, "bytes");
    lua_pushnumber(L, easyGetMetricD(h, EASY_METRIC_TOTALBYTES)); lua_setfield(L, 2, "totalbytes");
    lua_pushnumber(L, easyGetMetricD(h, EASY_METRIC_RATE)); lua_setfield(L, 2, "rate");
    lua_pushboolean(L, 1);
    return 1;
}

int lua_HeaderLister(const char* name, const char* value, void* r)
{
    lua_State* L = lState;
    lua_pushstring(L, name);
    lua_pushstring(L, value);
    lua_settable(L, -3);
    return 1;
}

/* 
    https.list(handle)

    handle integer of the request

    returns a table of read headers for the request
*/
int lua_List(lua_State* L) {
    int h = luaL_checkinteger(L, 1);
    if ((h < 0) || (h >= MAX_REQUEST)) luaL_error(L, "https.list() called with out of range value %d", h);
    lua_newtable(L);
    httpsListHeaders(con.requestTable[h], lua_HeaderLister);
    return 1;
}

/* 
    https.body(handle)
    https.body(handle, start)
    https.body(handle, start, end)

    handle integer of the request (if no start or end all the body)
    start integer of the first byte (if no end, start until end of body)
        - lua style, start byte is 1
    end integer of the first byte

    returns a string with the body contents (or a slice of it) for the response
*/
int lua_Body(lua_State* L) {
    int h = luaL_checkinteger(L, 1);
    unsigned int start, end;
    if ((h < 0) || (h >= MAX_REQUEST)) luaL_error(L, "https.body() called with out of range value %d", h);
    httpsReq *r = con.requestTable[h];
    if (lua_isnumber(L, 2)) start = lua_tointeger(L, 2);
        else start = 0;
    if (lua_isnumber(L, 3)) end = lua_tointeger(L, 3);
        else end = r->buffer.end;
    if (start < 1) start = 1;
    if (end < start) lua_pushstring(L, "");
        else lua_pushlstring(L, (const char*)r->buffer.data + start - 1, end - start + 1);
    return 1;
}

/* 
    https.release(handle)

    handle integer of the request

    mark the request as finished so it can be returned to the available pool
*/
int lua_Release(lua_State* L) {
    int h = luaL_checkinteger(L, 1);
    if ((h < 0) || (h >= MAX_REQUEST)) luaL_error(L, "https.release() called with out of range value %d", h);
    httpsFinished(con.requestTable[h]);
    return 0;
}

luaL_Reg lfunc[] = {
    { "init", lua_Init },
    { "shutdown", lua_Init },
    { "options", lua_Options },
    { "metrics", lua_Metrics },
    { "release", lua_Release },
    { "list", lua_List },
    { "response", lua_Response },
    { "update", lua_Update },
    { "get", lua_Get  },
    { "post", lua_Post  },
    { "head", lua_Head  },
    { "body", lua_Body  },
    { NULL, NULL },
};

int luaopen_libhttps(lua_State* L) {
    lState = L;
    lua_pushlightuserdata(L, &_luaIdLocation);
    lua_newtable (L);
    lua_rawset (L, LUA_REGISTRYINDEX);
    luaL_register(L, "https", lfunc);
    return 1;
}

