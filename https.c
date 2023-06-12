/*
    Simple https dynamic library based on https://github.com/erkkah/naett
    Jason A. Petrasko 2022, MIT License
*/
#define _DEFAULT_SOURCE 1

#include "https.h"
#include "src/xthread.h"
#include "src/memio.h"
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <memory.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>

#define BUFFER_USE_BIT  0x10000000
#define BUFFER_ID(x)    (x & 0x0FFFFFFF)

#define _ENTER_     pthread_mutex_lock(&con.mainLock);
#define __EXIT_     pthread_mutex_unlock(&con.mainLock);
#define _EXIT_RET(x)    { pthread_mutex_unlock(&con.mainLock); return x; }
#define _ENTER_REQ(x)   pthread_mutex_lock(&x->mutex);
#define _EXIT_REQ(x)    pthread_mutex_unlock(&x->mutex);

httpsMemoryInterface mem = { malloc, calloc, realloc, free };

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

static inline char *memStrdup(const char *str) {
    int i = strlen(str) + 1;
    char *ret = mem.malloc(i);
    strncpy(ret, str, i);
    ret[i] = 0;
    return ret;
}

httpsReq* _newHttpsReq(int flags) {
    httpsReq* req = NULL;
    int i;

    _ENTER_

    for (i = 0; i < MAX_REQUEST; i++)
        if (con.requestTable[i] == NULL) break;

    if (i == MAX_REQUEST) _EXIT_RET(NULL)

    // pull an allocated request and make it live in the system
    req = con.requestTable[i] = &con.requestBacker[i];
    // properly configure the request
    req->index = i;
    req->flags = flags;
    if (flags & HTTPS_FIXED_BUFFER) {
        // we want a fixed buffer for this request, so reflect that
        req->buffer.index = HTTPS_MEMBUFFER_UNINDEX;
        req->buffer.data = mem.malloc(HTTPS_BUFFER_KB(flags));
        if (req->buffer.data == NULL) _EXIT_RET(NULL)
        req->buffer.length = HTTPS_BUFFER_KB(flags);
    } else if (flags & HTTPS_FIXED_BUFFER) {
        // we are using a persistent buffer, so make that happen
        memcpy(&req->buffer, &con.persistentBuffer[HTTPS_PERSIST_ID(flags)], sizeof(memBuffer));
    } else {
        // allocate a buffer for this locally
        req->buffer.index = HTTPS_MEMBUFFER_UNINDEX;
        req->buffer.data = mem.malloc(con.bufferSize);
        if (req->buffer.data == NULL) _EXIT_RET(NULL)
        req->buffer.length = con.bufferSize;
    }
    con.bufferBytes += req->buffer.length;

    __EXIT_ // exit early because we are done with the global state

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

    return req;
}

void _delHttpsReq(httpsReq *p) {
    con.requestTable[p->index] = NULL;
    // free read buffer
    if (p->flags & HTTPS_PERSISTENT_BUFFER) {
        // users manage this, so we do nothing
    } else {
        // just free the allocated buffer for this request
        mem.free(p->buffer.data);    
    }
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

/*
    Set the default flush routine for all requests.
*/
void httpsSetFlushRoutine(httpsFlush f) {
    _ENTER_
    con.flush = f;
    __EXIT_
}

/*
    Ensure we have (int x) buffers available, expand our persistent buffer table as needed.

    By default we allocate 0, so if you want to use them, call this first.
*/
void httpsEnsurePersistentBuffers(int x) {
    _ENTER_
    x = HTTPS_PERSIST_ID(x);
    if (x >= con.persistentBufferCount) {
        mem.realloc(con.persistentBuffer, sizeof(memBuffer) * x);
        for (int i = con.persistentBufferCount; i < x; i++) {
            con.persistentBuffer[i].index = i;
            con.persistentBuffer[i].data = NULL;
            con.persistentBuffer[i].end = HTTPS_OPEN_BUFFER;
        }
        con.persistentBufferCount = x;
    }
     __EXIT_
}

/*
    Add a new persistent buffer to use later, a couple options to call:

        int ret = httpsAddPersistentBuffer(NULL, size);

            Allocate a new buffer of size bytes and return it's identifier

        int ret = httpsAddPersistentBuffer(pBuffer, size);

            Add a buffer of size bytes that already exists to the table.

    returns the handle id of the persistent buffer to pass to a request later
*/
int httpsAddPersistentBuffer(char *bmem, unsigned int bytes) {
    int i;
    _ENTER_
    for (i = 0; i < con.persistentBufferCount; i++) {
        if (con.persistentBuffer[i].end == HTTPS_OPEN_BUFFER) break;
    }
    // grow the buffers as needed if we didn't alread allocate enough
    if (i == con.persistentBufferCount) {
        int npbc = con.persistentBufferCount * 2;
        if (npbc == 0) npbc = 128;
        // if we will exceed the allowed 65536 buffers, just fail instead
        if (npbc > 0xFFFF) return -1;
        __EXIT_
        httpsEnsurePersistentBuffers(npbc);
        _ENTER_
    }
    con.persistentBuffer[i].length = bytes;
    con.persistentBuffer[i].end = 0;
    if (bmem == NULL) {
        con.persistentBuffer[i].data = mem.malloc(bytes);
        if (con.persistentBuffer[i].data == NULL) _EXIT_RET(-1)
        con.persistentBuffer[i].index = i;
        con.bufferBytes += bytes;
    } else {
        con.persistentBuffer[i].data = (unsigned char*)bmem;
        con.persistentBuffer[i].index = i | HTTPS_MEMBUFFER_FOREIGN;
    }
    __EXIT_
    return i;
}

/*
    Remove (free for use later) persistent buffer handle id
*/
void httpsRemovePersistentBuffer(int id) {
    if ((id < 0) || (id >= con.persistentBufferCount)) return;
    _ENTER_
    if (con.persistentBuffer[id].index & HTTPS_MEMBUFFER_FOREIGN)
    {
        con.persistentBuffer[id].end = HTTPS_OPEN_BUFFER;
        con.persistentBuffer[id].length = 0;
        con.persistentBuffer[id].index -= HTTPS_MEMBUFFER_FOREIGN;
        con.persistentBuffer[id].data = NULL;
        __EXIT_
        return;
    }
    mem.free(con.persistentBuffer[id].data);
    con.bufferBytes += con.persistentBuffer[id].end;
    con.persistentBuffer[id].end = HTTPS_OPEN_BUFFER;
    con.persistentBuffer[id].length = 0;
    con.persistentBuffer[id].data = NULL;
    __EXIT_
}

void httpsInit(httpsInitData init, unsigned int readBufferSize) {
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

void httpsCleanup() {
    _ENTER_
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
    __EXIT_
}

void httpsUpdate() {
    if (con.bufferSize == 0) return;
    _ENTER_
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
                    // might be valid, probe httpsHeaders for content type and length
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
    __EXIT_
}

unsigned int httpsRequestCount() {
    unsigned int ret = 0;
    _ENTER_
    for (int i = 0; i < MAX_REQUEST; i++)
        if (con.requestTable[i] != NULL) ret++;
    __EXIT_
    return ret;
}

void* _makeRequest(httpsReq* r, const char *method, void* _httpsHeaders, unsigned int bodyBytes, const char* body) {
    if (_httpsHeaders == NULL) {
        return (void*)naettRequest(r->URL, naettMethod(method), naettHeader("accept", "*/*"), naettBodyWriter(_bodyWriter, r));
    } else {
        httpsHeaders *h = (httpsHeaders*)_httpsHeaders;
        naettOption* bopt = NULL;
        int l = h->count + 2;
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
        for (int i = 0; i < h->count; i++)
            opts[x++] = naettHeader(h->str[i*2], h->str[i*2+1]);
        return (void*)naettRequestWithOptions(r->URL, l, (const naettOption**)opts);
    }
}

void* httpsGet(const char *URL, int flags,  void *httpsHeaders) {
    httpsReq* r;
    if ((con.bufferSize == 0) || (URL == NULL) || (strlen(URL) == 0)) return NULL;
    r = _newHttpsReq(flags);
    if (r == NULL) return NULL;
    r->URL = memStrdup(URL);
    r->request = _makeRequest(r, "GET", httpsHeaders, 0, NULL);
    r->res = (void*)naettMake((naettReq*)r->request);
    r->complete = r->finished = false;
    return (void*)r;
}

void* httpsPost(const char *URL, int flags,  const char *body, unsigned int bodyBytes, void *httpsHeaders) {
    httpsReq* r;
    if ((con.bufferSize == 0) || (URL == NULL) || (strlen(URL) == 0)) return NULL;
    r = _newHttpsReq(flags);
    if (r == NULL) return NULL;
    r->URL = memStrdup(URL);
    if (bodyBytes == 0) {
        if ((body == NULL) || (strlen(body) == 0)) return NULL;
        r->body = memStrdup(body);
        r->bodyTotalBytes = strlen(body);
    } else
    {
        r->bodyTotalBytes = bodyBytes;
        r->body = malloc(bodyBytes);
        memcpy(r->body, body, bodyBytes);
    }
    r->request = _makeRequest(r, "POST", httpsHeaders, bodyBytes, body);
    r->res = (void*)naettMake((naettReq*)r->request);
    r->complete = r->finished = false;
    return (void*)r;
}

void* httpsPostLinked(const char *URL, int flags,  const char *body, unsigned int bodyBytes, void *httpsHeaders) {
    httpsReq* r;
    if ((con.bufferSize == 0) || (URL == NULL) || (strlen(URL) == 0)) return NULL;
    r = _newHttpsReq(flags);
    if (r == NULL) return NULL;
    r->URL = memStrdup(URL);
    if (bodyBytes == 0) {
        if ((body == NULL) || (strlen(body) == 0)) return NULL;
        r->body = (char*)body;
        r->bodyTotalBytes = strlen(body);
    } else
    {
        r->bodyTotalBytes = bodyBytes;
        r->body = (char*)body;
    }
    r->request = _makeRequest(r, "POST", httpsHeaders, bodyBytes, body);
    r->res = (void*)naettMake((naettReq*)r->request);
    r->complete = r->finished = false;
    return (void*)r;
}

void* httpsHead(const char *URL, int flags,  void *httpsHeaders) {
    httpsReq* r;
    if ((con.bufferSize == 0) || (URL == NULL) || (strlen(URL) == 0)) return NULL;
    r = _newHttpsReq(flags);
    if (r == NULL) return NULL;
    r->URL = memStrdup(URL);
    r->request = _makeRequest(r, "HEAD", httpsHeaders, 0, NULL);
    r->res = (void*)naettMake((naettReq*)r->request);
    r->complete = r->finished = false;
    return (void*)r;    
}

int httpsGetCode(void *p) {
    httpsReq *r = (httpsReq*)p;
    int ret;
    _ENTER_REQ(r)
    ret = naettGetStatus((naettRes*)r);
    _EXIT_REQ(r)
    return ret;
}

int httpsGetCodeI(int i) {
    int ret;
    _ENTER_
    httpsReq *r = con.requestTable[i];
    __EXIT_
    _ENTER_REQ(r)
    ret = naettGetStatus((naettRes*)r);
    _EXIT_REQ(r)
    return ret;
}

const char* httpsGetHeader(void *p, const char *w) {
    httpsReq *r = (httpsReq*)p;
    const char *ret;
    _ENTER_REQ(r)
    ret = naettGetHeader((naettRes*)r->res, w);
    _EXIT_REQ(r)
    return ret;
}

int _HeaderLister(const char* name, const char* value, void* userData) {
    httpsReq* r = (httpsReq*)userData;
    return r->lister(name, value, userData);
}

void httpsListhttpsHeaders(void *p, httpsHeaderLister lister) {
    httpsReq *r = (httpsReq*)p;
    _ENTER_REQ(r)
    r->lister = lister;
    naettListHeaders((naettRes*)r->res, _HeaderLister, (void*)r);
    _EXIT_REQ(r)
}

bool httpsIsComplete(void *p) {
    httpsReq *r = (httpsReq*)p;
    bool ret = false;
    _ENTER_REQ(r)
    ret = r->complete;
    _EXIT_REQ(r)
    return ret;
}

void httpsFinished(void *p) {
    httpsReq *r = (httpsReq*)p;
    _ENTER_REQ(r)
    r->finished = true;
    _EXIT_REQ(r)
}

void* httpsNewhttpsHeaders() {
    httpsHeaders *h = calloc(1,sizeof(httpsHeaders));
    return (void*)h;
}

void httpsSetHeader(httpsHeaders *h, const char *name, const char *val) {
    int i;
    for (i = 0; i < MAX_HEADERS; i++) {
        if (!strcmp(name,h->str[i*2])) {
            mem.free(h->str[i*2+1]);
            h->str[i*2+1] = memStrdup(val);
        }
    }
    if ((i == MAX_HEADERS) && (h->count != MAX_HEADERS)) {
        i = h->count++;
        h->str[i*2] = memStrdup(name);
        h->str[i*2+1] = memStrdup(val);
    }
}

void httpsDelhttpsHeaders(httpsHeaders *h) {
    for (int i = 0; i < h->count*2; i++)
        mem.free(h->str[i]);
    mem.free((httpsHeaders*)h);
}

unsigned int httpsGetBodyLength(void *p) {
    httpsReq *r = (httpsReq*)p;
    unsigned int ret;
    _ENTER_REQ(r)
    ret = r->readTotalBytes;
    _EXIT_REQ(r)
    return ret;
}

void httpsGetBody(void *p, unsigned int maxBytes) {
    unsigned int len = maxBytes;
    httpsReq *r = (httpsReq*)p;
    _ENTER_REQ(r)
    if (len > r->buffer.end) len = r->buffer.end;
    memcpy(p, r->buffer.data, len);
    _EXIT_REQ(r)
}

void httpsGetBodyBuffer(void *p, memBuffer *b) {
    httpsReq *r = (httpsReq*)p;
    _ENTER_REQ(r)
    memcpy(b, &r->buffer, sizeof(memBuffer));
    _EXIT_REQ(r)
}

void httpsRelease(void *p) {
    httpsReq *r = (httpsReq*)p;
    _ENTER_REQ(r)
    r->finished = true;
    _EXIT_REQ(r)
}

void httpsGetInfo(httpsSystemInfo *info) {
    memset(info, 0, sizeof(httpsSystemInfo));
    _ENTER_
    for (int i = 0; i < MAX_REQUEST; i++) {
        httpsReq *r = con.requestTable[i];
        if (r != NULL) {
            info->numRequests++;
            if (!r->complete) info->activeRequests++;
        }
    }
    info->maxRequests = MAX_REQUEST;
    info->bufferBytes = con.bufferBytes;
    __EXIT_
}

bool libhttpsLove = false;

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

httpsHeaders *_easyCreateHeaders(const char* *_httpsHeaders, int header_count, bool compact)
{
    httpsHeaders *h = calloc(1,sizeof(_httpsHeaders));
    if (compact) {
        for (int i = 0; i < header_count; i++)
        {
            char *k, *v;
            char *tst = memStrdup(_httpsHeaders[i]);
            _stringSep(tst, &k, &v);
            httpsSetHeader(h, k, v);
        }
    } else {
        for (int i = 0; i < header_count; i++)
        {
            httpsSetHeader(h, _httpsHeaders[i*2], _httpsHeaders[i*2+1]);
        }
    }
    return h;
}

const char* _easyGetHeader(int i, const char *header)
{
    httpsReq* r = con.requestTable[i];
    const char *ret;
    _ENTER_REQ(r)
    ret = naettGetHeader((naettRes*)r->res, header);
    _EXIT_REQ(r)
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
    void *user;
    int flushMode;
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

typedef struct _easyThreadStack {
    int version;
    int msgLimit;
    int slotLimit;
    easyMessage *msg;
    easyMessage *slot;
    pthread_mutex_t msgLock;
    pthread_mutex_t slotLock;
} easyThreadStack;

typedef struct _easyDataBlock {
    httpsHeaders *headers;
    int bodyBytes;
    char *body;
    void *user;
} easyDataBlock;

pthread_t _thread;
easyThreadStack *_threadStack = NULL;
easyMetric _metricTable[MAX_REQUEST];
unsigned int _easyOptions = 0;
double _easyDelay = 0.0;

#define EASY_THREADED       ((_threadStack != NULL) && (_threadStack->version == HTTPS_VERSION_NUM))
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


void easyFlush(int index, const char* URL, void *user, memBuffer *p) {
    easyData *d = (easyData*)user;
    if (d->flushMode == 0) {
        FILE *fp = (FILE*)d->user;
        fwrite(p->data, 1, p->end, fp);
    }
}

xthread_ret easyWorkerThread(void *p) {
    bool done = false;
    while (!done) {
        usleep(5000);
    }
    return (xthread_ret)0;
}

void easySetup(easyCallback cb, unsigned int bsize)
{
    httpsInit(NULL, bsize);
    httpsSetFlushRoutine(easyFlush);
    _theEasyCallback = cb;
}

void easySetupThreaded(easyCallback cb, unsigned int msgQueDepth, unsigned int slotCount)
{
    if (msgQueDepth == 0) msgQueDepth = 200;
    if (slotCount == 0) slotCount = 50;

    httpsInit(NULL, 0);
    httpsSetFlushRoutine(easyFlush);
    _theEasyCallback = cb;

    _threadStack = mem.calloc(1, sizeof(easyThreadStack));
    easyThreadStack *ps = _threadStack;
    ps->msgLimit = msgQueDepth;
    ps->slotLimit = slotCount;
    ps->msg = mem.calloc(1, sizeof(easyMessage) * ps->msgLimit);
    if (ps->msg == NULL) return;
    ps->slot = mem.calloc(1, sizeof(easyMessage) * ps->slotLimit);
    if (ps->slot == NULL) return;
    pthread_mutex_init(&ps->msgLock, NULL);
    pthread_mutex_init(&ps->slotLock, NULL);
    // make all the messages and slots invalid
    pthread_mutex_lock(&ps->msgLock);
    for (int i = 0; i < ps->msgLimit; i++)
        ps->msg[i].handle = -1;
    pthread_mutex_unlock(&ps->msgLock);
    pthread_mutex_lock(&ps->slotLock);
    for (int i = 0; i < ps->slotLimit; i++)
        ps->slot[i].handle = -1;
    pthread_mutex_unlock(&ps->slotLock);
    ps->version = HTTPS_VERSION_NUM;
    xthread_create(&_thread, easyWorkerThread, NULL);
}

void easyListhttpsHeaders(int h, httpsHeaderLister lister)
{
    httpsListhttpsHeaders(con.requestTable[h], lister);
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
    // are we threaded? if so, why are we calling this? bug out
    if EASY_THREADED return;
    // or... proceed and handle the update
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
                // we have all the httpsHeaders!
                _theEasyCallback(i, r->URL, "httpsHeaders", r->returnCode, 0, (void*)_easyGetHeader);
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

static inline int easyFreeSlot() {
    int ret = -1, i;
    pthread_mutex_lock(&_threadStack->slotLock);
    for (i = 0; i < _threadStack->slotLimit; i++)
        if (_threadStack->slot[i].handle == -1) {
            _threadStack->slot[i].handle = HTTPS_OPEN_HANDLE;
            break;
        }
    pthread_mutex_unlock(&_threadStack->slotLock);
    if (i < _threadStack->slotLimit) ret = i;
    return ret;
}

static inline easyDataBlock* easyMakeDataBlock(const char *body, unsigned int bodyBytes, const char* *_httpsHeaders, int header_count, bool header_compact) {
    easyDataBlock *d = mem.calloc(1, sizeof(easyDataBlock));
    if (d == NULL) return NULL;
    // copy the body data if needed
    if (body != NULL && bodyBytes > 0) {
        d->bodyBytes = bodyBytes;
        d->body = mem.malloc(bodyBytes);
        if (d->body == NULL) {
            mem.free(d);
            return NULL;
        }
        memcpy(d->body, body, bodyBytes);
    }
    // copy the header data if needed
    if (_httpsHeaders != NULL && header_count > 0) {
        d->headers = mem.malloc(sizeof(httpsHeaders));
        d->headers->count = header_count;
        if (header_compact) {
            for (int i = 0; i < header_count; i++) {
                char *k, *v;
                char *tst = memStrdup(_httpsHeaders[i]);
                _stringSep(tst, &k, &v);
                d->headers->str[i*2] = memStrdup(k);
                d->headers->str[i*2+1] = memStrdup(v);
                mem.free(tst);
            }
        } else {
            for (int i = 0; i < header_count; i++) {
                d->headers->str[i*2] = memStrdup(_httpsHeaders[i*2]);
                d->headers->str[i*2+1] = memStrdup(_httpsHeaders[i*2+1]);
            }
        }
    }
    return d;
}

static inline easyDataBlock* easyMakeDataBlockPass(const char *body, unsigned int bodyBytes, httpsHeaders *h) {
    easyDataBlock *d = mem.calloc(1, sizeof(easyDataBlock));
    if (d == NULL) return NULL;
    // copy the body data if needed
    if (body != NULL && bodyBytes > 0) {
        d->bodyBytes = bodyBytes;
        d->body = mem.malloc(bodyBytes);
        if (d->body == NULL) {
            mem.free(d);
            return NULL;
        }
        memcpy(d->body, body, bodyBytes);
    }
    // pass through the header data if needed
    if (h != NULL) d->headers = h;
    return d;
}

static inline int easyThreadedSlot(const char *mode, const char *URL, int flags, const char *body, unsigned int bodyBytes, 
                                        const char* *_httpsHeaders, int header_count, bool header_compact) {
    int slot = easyFreeSlot();
    if (slot < 0) return slot;
    easyMessage *m = &_threadStack->slot[slot];
    m->version = 0;
    m->slot = slot;
    m->url = memStrdup(URL);
    strcpy(m->message, mode);
    m->code = 0;
    m->sz = 0;
    if (((header_count > 0) && (_httpsHeaders != NULL)) || ((body != NULL) && (bodyBytes > 0))) 
        m->data = easyMakeDataBlock(body, bodyBytes, _httpsHeaders, header_count, header_compact);
    else
        m->data = NULL;
    m->version = HTTPS_VERSION_NUM;
    return slot;
}

static inline int easyThreadedSlotPass(const char *mode, const char *URL, int flags, const char *body, unsigned int bodyBytes, 
                                        httpsHeaders *h) {
    int slot = easyFreeSlot();
    if (slot < 0) return slot;
    easyMessage *m = &_threadStack->slot[slot];
    m->version = 0;
    m->slot = slot;
    m->url = memStrdup(URL);
    strcpy(m->message, mode);
    m->code = 0;
    m->sz = 0;
    if ((h != NULL) || ((body != NULL) && (bodyBytes > 0))) 
        m->data = easyMakeDataBlockPass(body, bodyBytes, h);
    else
        m->data = NULL;
    m->version = HTTPS_VERSION_NUM;
    return slot;
}

int easyGet(const char *URL, int flags, const char* *_httpsHeaders, int header_count, bool header_compact) {
    httpsHeaders *h;
    httpsReq *r;

    // are we threaded? if so, we just populate a message slot and leave
    if EASY_THREADED {
        return easyThreadedSlot("GET", URL, flags, NULL, 0, _httpsHeaders, header_count, header_compact);
    }

    if ((header_count > 0) && (_httpsHeaders != NULL))
    {
        h = _easyCreateHeaders(_httpsHeaders, header_count, header_compact);
        r = httpsGet(URL, flags, h);
        httpsDelhttpsHeaders(h);
    } else
        r = httpsGet(URL, flags, NULL);
    r->userData = mem.calloc(1, sizeof(easyData));
    _theEasyCallback(r->index, r->URL, "START", r->returnCode, 0, NULL);
    return r->index;
}

int easyGetFile(const char *URL, const char *ofname, const char* *_httpsHeaders, int header_count, bool header_compact) {
    httpsHeaders *h;
    httpsReq *r;

    // are we threaded? if so, we just populate a message slot and leave
    if EASY_THREADED {
        int slot = easyThreadedSlot("GET", URL, HTTPS_REUSE_BUFFER, NULL, 0, _httpsHeaders, header_count, header_compact);
        _threadStack->slot[slot].flush = (void*)easyFlush;
        _threadStack->slot[slot].user = (void*)fopen(ofname, "wb");
        return slot;
    }

    if ((header_count > 0) && (_httpsHeaders != NULL))
    {
        h = _easyCreateHeaders(_httpsHeaders, header_count, header_compact);
        r = httpsGet(URL, HTTPS_REUSE_BUFFER, h);
        httpsDelhttpsHeaders(h);
    } else
        r = httpsGet(URL, HTTPS_REUSE_BUFFER, NULL);
    easyData *d = mem.calloc(1, sizeof(easyData));
    d->user = (void*)fopen(ofname, "wb");
    r->userData = d;
    _theEasyCallback(r->index, r->URL, "START", r->returnCode, 0, NULL);
    return r->index;
}

int easyPost(const char *URL, int flags, const char *body, unsigned int bodyBytes, const char* *_httpsHeaders, int header_count, bool header_compact) {
    httpsHeaders *h;
    httpsReq *r;

    // are we threaded? if so, we just populate a message slot and leave
    if EASY_THREADED {
        return easyThreadedSlot("POST", URL, flags, body, bodyBytes, _httpsHeaders, header_count, header_compact);
    }

    if ((header_count > 0) && (_httpsHeaders != NULL))
    {
        h = _easyCreateHeaders(_httpsHeaders, header_count, header_compact);
        r = httpsPost(URL, flags, body, bodyBytes, h);
        httpsDelhttpsHeaders(h);
    } else
        r = httpsPost(URL, flags, body, bodyBytes, NULL);
    r->userData = mem.calloc(1, sizeof(easyData));
    _theEasyCallback(r->index, r->URL, "START", r->returnCode, 0, NULL);
    return r->index;
}

int easyHead(const char *URL, int flags, const char* *_httpsHeaders, int header_count, bool header_compact) {
    httpsHeaders *h;
    httpsReq *r;

    if EASY_THREADED {
        return easyThreadedSlot("HEAD", URL, flags, NULL, 0, _httpsHeaders, header_count, header_compact);
    }

    if ((header_count > 0) && (_httpsHeaders != NULL))
    {
        h = _easyCreateHeaders(_httpsHeaders, header_count, header_compact);
        r = httpsHead(URL, flags, h);
        httpsDelhttpsHeaders(h);
    } else
        r = httpsHead(URL, flags, NULL);
    r->userData = mem.calloc(1, sizeof(easyData));
    _theEasyCallback(r->index, r->URL, "START", r->returnCode, 0, NULL);
    return r->index;
}

int easyGetPass(const char *URL, int flags, httpsHeaders *h) {
    httpsReq *r;

    // are we threaded? if so, we just populate a message slot and leave
    if EASY_THREADED {
        return easyThreadedSlotPass("GET", URL, flags, NULL, 0, h);
    }

    r = httpsGet(URL, flags, h);
    r->userData = mem.calloc(1, sizeof(easyData));
    _theEasyCallback(r->index, r->URL, "START", r->returnCode, 0, NULL);
    return r->index;
}

int easyPostPass(const char *URL, int flags, const char *body, unsigned int bodyBytes, httpsHeaders *h) {
    httpsReq *r;

    // are we threaded? if so, we just populate a message slot and leave
    if EASY_THREADED {
        return easyThreadedSlotPass("POST", URL, flags, body, bodyBytes, h);
    }

    r = httpsPost(URL, flags, body, bodyBytes, h);
    r->userData = mem.calloc(1, sizeof(easyData));
    _theEasyCallback(r->index, r->URL, "START", r->returnCode, 0, NULL);
    return r->index;
}

int easyHeadPass(const char *URL, int flags, httpsHeaders *h) {
    httpsReq *r;

    if EASY_THREADED {
        return easyThreadedSlotPass("HEAD", URL, flags, NULL, 0, h);
    }

    r = httpsHead(URL, flags, h);    
    r->userData = mem.calloc(1, sizeof(easyData));
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
#define EASY_CB_httpsHeaders     3
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
         else if (!strcmp(msg,"httpsHeaders")) { lua_pushliteral(lState,"httpsHeaders"); cbm = EASY_CB_httpsHeaders; }
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
                if (cbm == EASY_CB_httpsHeaders) {
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
    function love.handlers.https(what, handle, url, msg, code, sz data)
    end

    what: the event that occured on this handle ->
        'start' - the request was started
        'update' - status change, likely a return code was received
        'headers' - all the request headers have been received
        'length' - content length (in bytes) was determined
        'mime' - content mime type was determined
        'read' - a single read event finished (might need multiple to complete)
        'complete' - the request has been completed (ok or error)
*/
void luaLove_Callback(int handle, const char* url, const char* msg, int code, unsigned int sz, void* data)
{
    int t = lua_gettop(lState);
    int cbm = 0;
    lua_getfield(lState, LUA_GLOBALSINDEX, "love");
    lua_getfield(lState, -1, "handlers");
    lua_getfield(lState, -1, "https");
    if (lua_isfunction(lState,-1)) {
        // find the callback in the 
        if (!strcmp(msg,"START")) { lua_pushliteral(lState,"start"); cbm = EASY_CB_START; }
         else if (!strcmp(msg,"UPDATE")) { lua_pushliteral(lState,"update"); cbm = EASY_CB_UPDATE; }
         else if (!strcmp(msg,"HEADERS")) { lua_pushliteral(lState,"headers"); cbm = EASY_CB_httpsHeaders; }
         else if (!strcmp(msg,"LENGTH")) { lua_pushliteral(lState,"length"); cbm = EASY_CB_LENGTH; }
         else if (!strcmp(msg,"MIME")) { lua_pushliteral(lState,"mime"); cbm = EASY_CB_MIME; }
         else if (!strcmp(msg,"READ")) { lua_pushliteral(lState,"read"); cbm = EASY_CB_READ; }
         else if (!strcmp(msg,"COMPLETE")) { lua_pushliteral(lState,"complete"); cbm = EASY_CB_COMPLETE; }
         else lua_pushliteral(lState,"nope");
        lua_pushinteger(lState, handle);
        lua_pushstring(lState, url);
        lua_pushstring(lState, msg);
        lua_pushinteger(lState, code);
        lua_pushinteger(lState, sz);
        if (data) {
            if (cbm == EASY_CB_MIME) lua_pushstring(lState, (char*)data);
            if (cbm == EASY_CB_httpsHeaders) {
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
    lua_settop(lState, t);
}

/* 
    https.update()

    called repeatedly to update the system and issue callbacks on requests,
    if https.options("EASY_OPT_DELAY", seconds) has been called this
    will delay that many seconds before returning
*/
int lua_Update(lua_State* L) {
    // are we threaded? poll the messages
    if EASY_THREADED {
        // TODO
        return 0;
    }
    // pass the call down
    lua_getregtable(L);
    lua_assert_init(L);
    easyUpdate();
    return 0;
}

/* 
    https.get(url, callback, httpsHeaders)

        url is a string with the url to be requested using http get.

        callback is a table object which gets callbacks from this request, as so:
            callback:name(vars)

        httpsHeaders is an optional table of httpsHeaders to pass to this request
            the string:string keys/values of the table only are sent as http httpsHeaders
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
    https.getFile(url, outfilename, callback, httpsHeaders)

        url is a string with the url to be requested using http get.

        outfilename is where the body of the request will be written.

        callback is a table object which gets callbacks from this request, as so:
            callback:name(vars)

        httpsHeaders is an optional table of httpsHeaders to pass to this request
            the string:string keys/values of the table only are sent as http httpsHeaders
*/
int lua_GetFile(lua_State* L) {
    const char *head[MAX_HEADERS*2];
    int i = 0;
    int r = 0;
    const char *url = luaL_checklstring(L, 1, NULL);
    const char *ofname = luaL_checklstring(L, 2, NULL);
    luaL_checktype(L, 3, LUA_TTABLE);
    if (lua_istable(L, 4)) {
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
        r = easyGetFile(url, ofname, head, i, false);
    } else {
        r = easyGetFile(url, ofname, NULL, 0, false);
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
    https.post(url, body, callback, httpsHeaders)

    url is a string with the url to be requested using http get.

    body is a string containing the body of the post.

    callback is a table object which gets callbacks from this request, as so:
        callback:name(vars)

    httpsHeaders is an optional table of httpsHeaders to pass to this request
        the string:string keys/values of the table only are sent as http httpsHeaders
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
    https.head(url, callback, httpsHeaders)

    url is a string with the url to be requested using http head.

    callback is a table object which gets callbacks from this request, as so:
        callback:name(vars)

    httpsHeaders is an optional table of httpsHeaders to pass to this request
        the string:string keys/values of the table only are sent as http httpsHeaders
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
    called to initialize the system, must be done for any other calls into https

    https.init(false, nil or bytes)
  
        false makes an unthreaded system.

        if passed a number for argument two, that is the amount of buffer bytes used by requests,
        defaults to 16384 for 16kb

    https.init(true, msg, slot)

        true creates a threaded system.

        msg is the msq que depth

        slot is the slot que depth
*/
int lua_Init(lua_State* L) {
    int arg2 = 0;
    int arg3 = 0;
    if (lua_isnumber (L, 2)) arg2 = lua_tointeger(L, 2);
    if (lua_isnumber (L, 3)) arg3 = lua_tointeger(L, 3);
    if (lua_toboolean(L, 1) > 0) {
        easySetupThreaded(lua_Callback, arg2, arg3);
    } else {
        easySetup(lua_Callback, arg2);
    }
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

    returns a table of read httpsHeaders for the request
*/
int lua_List(lua_State* L) {
    int h = luaL_checkinteger(L, 1);
    if ((h < 0) || (h >= MAX_REQUEST)) luaL_error(L, "https.list() called with out of range value %d", h);
    lua_newtable(L);
    httpsListhttpsHeaders(con.requestTable[h], lua_HeaderLister);
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

/* 
    https.memio(handle)

    handle integer of the request

    returns a memio interface into the body of the request
*/
int lua_Memio(lua_State* L) {
    int h = luaL_checkinteger(L, 1);
    if ((h < 0) || (h >= MAX_REQUEST)) luaL_error(L, "https.memio() called with out of range value %d", h);
    httpsReq *r = con.requestTable[h];
    if (!r->complete)  luaL_error(L, "https.memio() called on an incomplete request");
    lua_pushIO(L, r->body, r->bodyTotalBytes, 0);
    return 1;
}

luaL_Reg lfunc[] = {
    { "init", lua_Init },
    { "shutdown", lua_Shutdown },
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
    { "memio", lua_Memio  },
    { NULL, NULL },
};

int luaopen_libhttps(lua_State* L) {
    lState = L;
    lua_pushlightuserdata(L, &_luaIdLocation);
    lua_newtable (L);
    lua_rawset (L, LUA_REGISTRYINDEX);
    luaL_register(L, "https", lfunc);
    // detect love and set the flag for integration
    lua_getfield(L, LUA_GLOBALSINDEX, "love");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "getVersion");
        if (lua_isfunction(L, -1)) {
            libhttpsLove = true;
            easySetupThreaded(luaLove_Callback, 0, 0);
            lua_getregtable(L);
            lua_pushinteger(L, 2422422);
            lua_rawseti(L, -2, 1421421);
            lua_pop(L, 1);
        }
         else libhttpsLove = false;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    return 1;
}

