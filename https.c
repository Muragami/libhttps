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

typedef struct _headers {
    char *str[MAX_HEADERS*2];
    int last;
} headers;

typedef struct _readBuffer {
    unsigned char *data;
    unsigned int end;
    void* nextBuffer;
} readBuffer;

typedef struct _httpsReq {
    void *request;
    void *res;
    void *mutex;
    int index;
    char *URL;
    readBuffer *read;
    readBuffer *last;
    unsigned int readBufferSize;
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
    // metrics
    double startTime;
} httpsReq;

unsigned int _bufferSize = 0;
httpsReq* _requestTable[MAX_REQUEST];
pthread_mutex_t _mainLock;

static inline double _getSeconds()
{
    struct timeval currentTime;
    gettimeofday(&currentTime, NULL);
    return (double)currentTime.tv_sec + (double)currentTime.tv_usec * 0.0000001;
}

httpsReq* _newHttpsReq()
{
    httpsReq* req = NULL;
    int i;
    for (i = 0; i < MAX_REQUEST; i++)
        if (_requestTable[i] == NULL) break;

    if (i == MAX_REQUEST) return NULL;

    req = calloc(1, sizeof(httpsReq));
    req->index = i;
    req->mutex = malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init((pthread_mutex_t*)req->mutex, NULL);
    req->read = calloc(1, sizeof(readBuffer));
    req->read->data = calloc(1, _bufferSize);
    req->last = req->read;
    req->headerDone = req->complete = req->finished = false;
    req->startTime = _getSeconds();

    pthread_mutex_lock(&_mainLock);
    _requestTable[i] = req;
    pthread_mutex_unlock(&_mainLock);
    return req;
}

void _delHttpsReq(httpsReq *p)
{
    pthread_mutex_lock(&_mainLock);
    _requestTable[p->index] = NULL;
    // free read buffers
    readBuffer *b = p->read;
    readBuffer *o;
    while (b != NULL) {
        free(b->data);
        o = b;
        b = b->nextBuffer;
        free(o);
    }
    // free the request itself
    naettClose((naettRes*)p->res);
    naettFree((naettReq*)p->request);
    free(p);
    pthread_mutex_unlock(&_mainLock);
}

int _bodyWriter(const void* source, int bytes, void* userData)
{
    httpsReq *r = (httpsReq*)userData;
    readBuffer *p = r->last;
    r->headerDone = true;
    int toWrite = bytes;
    int nibble;
    while (toWrite > 0) {
        unsigned int b = _bufferSize - p->end;
        if (toWrite > b) nibble = b;
            else nibble = toWrite;
        memcpy(p->data + p->end, source, nibble);
        toWrite -= nibble;
        p->end += nibble;
        r->readTotalBytes += nibble;
        if (p->end == _bufferSize)
        {
            p->nextBuffer = calloc(1, sizeof(readBuffer));
            if (p->nextBuffer == NULL) return 0;
            p = p->nextBuffer;
            p->data = calloc(1, _bufferSize);
            if (p->data == NULL) return 0;
            r->last = p;
        }
    }
    return bytes;
}

void httpsInit(httpsInitData init, unsigned int readBufferSize)
{
    naettInit((naettInitData)init);
    _bufferSize = readBufferSize;
    if (_bufferSize == 0) _bufferSize = 16384;
    for (int i = 0; i < MAX_REQUEST; i++)
        _requestTable[i] = NULL;
    pthread_mutex_init(&_mainLock, NULL);
}

void httpsCleanup()
{
    pthread_mutex_lock(&_mainLock);
    for (int i = 0; i < MAX_REQUEST; i++)
    {
        httpsReq* r = _requestTable[i];
        if (r != NULL) {
            if (r->complete && r->finished) {
                // delete it
                _delHttpsReq(r);
            } else {
                // force it to end
                naettClose((naettRes*)r->res);
                naettFree((naettReq*)r->request);
                _requestTable[i] = NULL;
            }    
        }
    }
    pthread_mutex_unlock(&_mainLock);
}

void httpsUpdate()
{
    pthread_mutex_lock(&_mainLock);
    for (int i = 0; i < MAX_REQUEST; i++)
    {
        httpsReq* r = _requestTable[i];
        if (r != NULL) {
            // if we are done totally, free the request so it can be deleted,
            // opening the slot it's taking
            if (r->complete && r->finished) {
                // delete it
                _delHttpsReq(r);
            }
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
    pthread_mutex_unlock(&_mainLock);
}

unsigned int httpsRequestCount()
{
    unsigned int ret = 0;
    pthread_mutex_lock(&_mainLock);
    for (int i = 0; i < MAX_REQUEST; i++)
        if (_requestTable[i] != NULL) ret++;
    pthread_mutex_unlock(&_mainLock);
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

void* httpsGet(const char *URL, void *headers)
{
    httpsReq* r;
    if ((_bufferSize == 0) || (URL == NULL) || (strlen(URL) == 0)) return NULL;
    r = _newHttpsReq();
    if (r == NULL) return NULL;
    r->URL = strdup(URL);
    r->request = _makeRequest(r, "GET", headers, 0, NULL);
    r->res = (void*)naettMake((naettReq*)r->request);
    r->complete = r->finished = false;
    return (void*)r;
}

void* httpsPost(const char *URL, const char *body, unsigned int bodyBytes, void *headers)
{
    httpsReq* r;
    if ((_bufferSize == 0) || (URL == NULL) || (strlen(URL) == 0)) return NULL;
    r = _newHttpsReq();
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

void* httpsPostLinked(const char *URL, const char *body, unsigned int bodyBytes, void *headers)
{
    httpsReq* r;
    if ((_bufferSize == 0) || (URL == NULL) || (strlen(URL) == 0)) return NULL;
    r = _newHttpsReq();
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

void* httpsHead(const char *URL, void *headers)
{
    httpsReq* r;
    if ((_bufferSize == 0) || (URL == NULL) || (strlen(URL) == 0)) return NULL;
    r = _newHttpsReq();
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
    pthread_mutex_lock((pthread_mutex_t*)r->mutex);
    ret = naettGetStatus((naettRes*)r);
    pthread_mutex_unlock((pthread_mutex_t*)r->mutex);
    return ret;
}

int httpsGetCodeI(int i)
{
    httpsReq *r = _requestTable[i];
    int ret;
    pthread_mutex_lock((pthread_mutex_t*)r->mutex);
    ret = naettGetStatus((naettRes*)r);
    pthread_mutex_unlock((pthread_mutex_t*)r->mutex);
    return ret;
}

const char* httpsGetHeader(void *p, const char *w)
{
    httpsReq *r = (httpsReq*)p;
    const char *ret;
    pthread_mutex_lock((pthread_mutex_t*)r->mutex);
    ret = naettGetHeader((naettRes*)r->res, w);
    pthread_mutex_unlock((pthread_mutex_t*)r->mutex);
    return ret;
}

int _HeaderLister(const char* name, const char* value, void* userData)
{
    httpsReq* r = (httpsReq*)userData;
    r->lister(name, value, userData);
    return 0;
}

void httpsListHeaders(void *p, httpsHeaderLister lister)
{
    httpsReq *r = (httpsReq*)p;
    pthread_mutex_lock((pthread_mutex_t*)r->mutex);
    r->lister = lister;
    naettListHeaders((naettRes*)r->res, _HeaderLister, (void*)r);
    pthread_mutex_unlock((pthread_mutex_t*)r->mutex);
}

bool httpsIsComplete(void *p)
{
    httpsReq *r = (httpsReq*)p;
    bool ret = false;
    pthread_mutex_lock((pthread_mutex_t*)r->mutex);
    ret = r->complete;
    pthread_mutex_unlock((pthread_mutex_t*)r->mutex);
    return ret;
}

void httpsFinished(void *p)
{
    httpsReq *r = (httpsReq*)p;
    pthread_mutex_lock((pthread_mutex_t*)r->mutex);
    if (r->complete) r->finished = true;
    pthread_mutex_unlock((pthread_mutex_t*)r->mutex);
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
    pthread_mutex_lock((pthread_mutex_t*)r->mutex);
    ret = r->readTotalBytes;
    pthread_mutex_unlock((pthread_mutex_t*)r->mutex);
    return ret;
}

void httpsGetBody(void *p, unsigned int maxBytes)
{
    httpsReq *r = (httpsReq*)p;
    pthread_mutex_lock((pthread_mutex_t*)r->mutex);
    unsigned int toWrite = r->readTotalBytes;
    if (toWrite > maxBytes) toWrite = maxBytes;
    unsigned int bCount = toWrite / r->readBufferSize;
    unsigned int last = toWrite - (bCount * r->readBufferSize);
    unsigned int pos = 0;
    readBuffer *b = r->read;
    while (bCount-- > 0) {
        memcpy(p + pos, b->data, r->readBufferSize);
        pos += r->readBufferSize;
        b = b->nextBuffer;
    }
    if (last > 0) memcpy(p + pos, b->data, last);
    pthread_mutex_unlock((pthread_mutex_t*)r->mutex);
}

void httpsRelease(void *p)
{
    httpsReq *r = (httpsReq*)p;
    pthread_mutex_lock((pthread_mutex_t*)r->mutex);
    r->finished = true;
    pthread_mutex_unlock((pthread_mutex_t*)r->mutex);
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
    httpsReq* r = _requestTable[i];
    const char *ret;
    pthread_mutex_lock((pthread_mutex_t*)r->mutex);
    ret = naettGetHeader((naettRes*)r->res, header);
    pthread_mutex_unlock((pthread_mutex_t*)r->mutex);
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
    readBuffer *read;
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

void _easyReadResponse(int i, char* data, int *bytes, bool *complete)
{
    httpsReq* r = _requestTable[i];
    easyData *d = (easyData*)r->userData;
    pthread_mutex_lock((pthread_mutex_t*)r->mutex);
    if (data == NULL)
    {
        *bytes = r->readTotalBytes;
        return;
    }
    if (d->read == NULL) d->read = r->read;
    *bytes = d->read->end;
    memcpy(data,d->read->data,d->read->end);
    d->read = d->read->nextBuffer;
    if (d->read == NULL) *complete = true; else *complete = false;
    pthread_mutex_unlock((pthread_mutex_t*)r->mutex);    
}

void easySetup(easyCallback cb, unsigned int bsize)
{
    httpsInit(NULL, bsize);
    _theEasyCallback = cb;
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
    if (_bufferSize == 0) httpsInit(NULL, 0);
    httpsUpdate();
    pthread_mutex_lock(&_mainLock);
    for (int i = 0; i < MAX_REQUEST; i++)
    {
        httpsReq* r = _requestTable[i];
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
                _theEasyCallback(i, r->URL, "COMPLETE", r->returnCode, r->readBufferSize, (void*)_easyReadResponse);
                d->returnCode = r->returnCode;
                httpsRelease(r);
            }
        }
    }
    pthread_mutex_unlock(&_mainLock);
    
    // are we doing metrics? if so update them
    if EASY_METRICS {
        secs = _getSeconds();
        pthread_mutex_lock(&_mainLock);
        // empty the metric table (just mark every entry invalid)
        for (int i = 0; i < MAX_REQUEST; i++)
            _metricTable[i].handle = -1;
        // see what metrics we have to collect!
        for (int i = 0; i < MAX_REQUEST; i++)
        {
            httpsReq* r = _requestTable[i];
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
        pthread_mutex_unlock(&_mainLock);
    }

    // sleep for the request delay amount if we are being nice
    if (_easyDelay > 0.0) usleep((useconds_t)(_easyDelay * 1000000));
}

int easyGet(const char *URL, const char* *_headers, int header_count, bool header_compact)
{
    headers *h;
    httpsReq *r;
    if ((header_count > 0) && (_headers != NULL))
    {
        h = _easyCreateHeaders(_headers, header_count, header_compact);
        r = httpsGet(URL, h);
        httpsDelHeaders(h);
    } else
        r = httpsGet(URL, NULL);
    r->userData = calloc(1, sizeof(easyData));
    _theEasyCallback(r->index, r->URL, "START", r->returnCode, 0, NULL);
    return r->index;
}

int easyPost(const char *URL, const char *body, unsigned int bodyBytes, const char* *_headers, int header_count, bool header_compact)
{
    headers *h;
    httpsReq *r;
    if ((header_count > 0) && (_headers != NULL))
    {
        h = _easyCreateHeaders(_headers, header_count, header_compact);
        r = httpsPost(URL, body, bodyBytes, h);
        httpsDelHeaders(h);
    } else
        r = httpsPost(URL, body, bodyBytes, NULL);
    r->userData = calloc(1, sizeof(easyData));
    _theEasyCallback(r->index, r->URL, "START", r->returnCode, 0, NULL);
    return r->index;
}

int easyHead(const char *URL, const char* *_headers, int header_count, bool header_compact)
{
    headers *h;
    httpsReq *r;
    if ((header_count > 0) && (_headers != NULL))
    {
        h = _easyCreateHeaders(_headers, header_count, header_compact);
        r = httpsHead(URL, h);
        httpsDelHeaders(h);
    } else
        r = httpsHead(URL, NULL);
    r->userData = calloc(1, sizeof(easyData));
    _theEasyCallback(r->index, r->URL, "START", r->returnCode, 0, NULL);
    return r->index;
}

// a lua module wrapper for the easy calls above
#ifndef NOT_LUA_DLL

#include "src/lauxlib.h"

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

int lua_ReadResponse(lua_State* L)
{
    void (*ReadResponse)(int i, char* data, int *bytes, bool *complete);
    ReadResponse = lua_touserdata(L, lua_upvalueindex(2));
    if (lua_isnoneornil(L, 1)) {
        // just return the total bytes of this response
        int bytes;
        ReadResponse(lua_tointeger(L, lua_upvalueindex(1)), NULL, &bytes, NULL);
        lua_pushinteger(L, bytes);
        return 1;
    } else if (lua_istable(L, 1)) {
        // return a string for this buffer chunk of the response
        char *buffer = calloc(1,lua_tointeger(L, lua_upvalueindex(3)));
        int bytes;
        bool done;
        ReadResponse(lua_tointeger(L, lua_upvalueindex(1)), buffer, &bytes, &done);
        lua_pushinteger(L, lua_objlen(L, 1) + 1);
        lua_pushlstring(L, buffer, bytes);
        lua_rawset(L, -3);
        return 0;
    } else luaL_error(L, "https callback ReadResponse() function must be called with a table to populate!");
    return 0;
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
                    lua_pushinteger(lState, handle);
                    lua_pushlightuserdata(lState, data);
                    lua_pushinteger(lState, sz);
                    lua_pushcclosure(lState, lua_ReadResponse, 3);
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
        r = easyGet(url, head, i, false);
    } else {
        r = easyGet(url, NULL, 0, false);
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
    return 0;
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
        r = easyPost(url, body, bbytes, head, i, false);
    } else {
        r = easyPost(url, body, bbytes, NULL, 0, false);
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
    return 0;
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
        r = easyHead(url, head, i, false);
    } else {
        r = easyHead(url, NULL, 0, false);
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
    return 0;
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
    lua_rawseti(L, -1, 1421421);
    lua_pop(L, 1);
    return 0;
}

/* 
    https.response(handle)

    handle integer of the request

    returns the status code of the request
*/
int lua_Response(lua_State *L) {
    int i = luaL_checkinteger(L, 1);
    if ((i < 0) || (i > MAX_REQUEST)) luaL_error(L, "https.response() attempt to index a request %d, outside range 0 - %d", i, MAX_REQUEST);
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

luaL_Reg lfunc[] = {
    { "init", lua_Init },
    { "options", lua_Options },
    { "metrics", lua_Metrics },
    { "response", lua_Response },
    { "update", lua_Update },
    { "get", lua_Get  },
    { "post", lua_Post  },
    { "head", lua_Head  },
    { NULL, NULL },
};

int luaopen_libhttps (lua_State* L) {
    lState = L;
    lua_pushlightuserdata(L, &_luaIdLocation);
    lua_newtable (L);
    lua_rawset (L, LUA_REGISTRYINDEX);
    luaL_register(L, "https", lfunc);
    return 1;
}

#endif
