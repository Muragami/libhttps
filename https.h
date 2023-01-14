/*
    Simple https dynamic library based on https://github.com/erkkah/naett
    Jason A. Petrasko 2022, MIT License
*/

#include "src/naett.h"
#include "src/lauxlib.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if __ANDROID__
#include <jni.h>
typedef JavaVM* httpsInitData;
#else
typedef void* httpsInitData;
#endif

typedef struct _httpsMemoryInterface {
    void* (*malloc)(size_t bytes);
    void* (*calloc)(size_t sz, size_t count);
    void* (*realloc)(void *p, size_t bytes);
    void  (*free)(void *p);
} httpsMemoryInterface;

typedef struct _memBuffer {
    unsigned int index;
    unsigned int end;
    unsigned long length;
    unsigned char *data;
} memBuffer;

#define HTTPS_MEMBUFFER_FOREIGN     0x10000000      // foreign buffer, don't automatically free()
#define HTTPS_MEMBUFFER_UNINDEX     0x20000000      // a not indexed buffer

// void* is the httpsReq* that called for this listing, you can get and set *userData in there
typedef int (*httpsHeaderLister)(const char* name, const char* value, void* r);

// a flush routine to call when a read buffer is full
typedef void (*httpsFlush)(int index, const char* URL, void *user, memBuffer *p);

#define HTTPS_FIXED_BUFFER          0x10000000      // a fixed buffer for this request
#define HTTPS_PERSISTENT_BUFFER     0x30000000      // use an established already existing buffer (always also fixed size)
#define HTTPS_REUSE_BUFFER          0x40000000      // reuse a buffer, using a flush callback each time it's full
#define HTTPS_DOUBLE_UNTIL          0x80000000      // double realloc() until we hit a set size and them just allocated that size over and over (2x, 3x, etc.)
#define HTTPS_DOUBLE_FOREVER(x)     ((x & 0xF0000000) == 0)      
                                                    // just double each time we realloc()
#define HTTPS_BUFFER_KB(x)          (x & 0xFFFFFFF) // ~ 255GB is the largest fixed buffer we can support, allocated as 1 kb units
#define HTTPS_PERSIST_ID(x)         (x & 0xFFFF)    // 65536 possible persistant buffers
#define HTTPS_OPEN_BUFFER           0xFFFFFFFF      // a buffer end point that is invalid

// the low level interface if you want to be fancy
void httpsInit(httpsInitData init, unsigned int readBufferSize);
void httpsCleanup();
void httpsUseMemoryInterface(httpsMemoryInterface *p);
void httpsSetFlushRoutine(httpsFlush f);
void httpsEnsurePersistentBuffers(int i);
int httpsAddPersistentBuffer(char *bmem, unsigned int bytes);
void httpsRemovePersistentBuffer(int id);
void httpsUpdate();
void* httpsGet(const char *URL, int flags, void *headers);
void* httpsPost(const char *URL, int flags, const char *body, unsigned int bodyBytes, void *headers);
// in the linked case, we don't copy body at all and expect you to only free it when we are done!
void* httpsPostLinked(const char *URL, int flags, const char *body, unsigned int bodyBytes, void *headers);
void* httpsHead(const char *URL, int flags, void *headers);
int httpsGetCode(void *p);
int httpsGetCodeI(int i);
const char* httpsGetHeader(void *p, const char *w);
unsigned int httpsGetBodyLength(void *p);
void httpsGetBody(void *p, unsigned int maxBytes);
void httpsListHeaders(void *p, httpsHeaderLister lister);
bool httpsIsComplete(void* p);
void httpsFinished(void* p);
unsigned int httpsRequestCount();
void* httpsNewHeaders();
void httpsSetHeader(void *p, const char *name, const char *val);
void httpsDelHeaders(void *p);
void httpsRelease(void *p);

// high-level callbacks
typedef void (*easyCallback)(int handle, const char* url, const char* msg, int code, unsigned int sz, void* data);

// the high level interface if you hail from Letterkenney
void easySetup(easyCallback cb, unsigned int bsize);
void easyListHeaders(int h, httpsHeaderLister lister);
void easyOptionUI(unsigned int opt, unsigned int val);
void easyOptionD(unsigned int opt, double val);
int easyHasMetrics(int i);
double easyGetMetricD(int i, int w);
int easyGetMetricI(int i, int w);
const char *easyGetMetricS(int i, int w);
void easyUpdate();	// if you call this is counts as calling the low-level httpsUpdate() above, FYI
int easyGet(const char *URL, int flags, const char* *headers, int header_count, bool header_compact);
int easyPost(const char *URL, int flags, const char *body, unsigned int bodyBytes, const char* *headers, int header_count, bool header_compact);
int easyHead(const char *URL, int flags, const char* *headers, int header_count, bool header_compact);

int luaopen_libhttps(lua_State* L);

#ifdef __cplusplus
}
#endif
