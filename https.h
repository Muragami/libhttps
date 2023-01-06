/*
    Simple https dynamic library based on https://github.com/erkkah/naett
    Jason A. Petrasko 2022, MIT License
*/

#include "src/naett.h"
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

// void* is the httpsReq* that called for this listing, you can get and set *userData in there
typedef int (*httpsHeaderLister)(const char* name, const char* value, void* r);

// the low level interface if you want to be fancy
void httpsInit(httpsInitData init, unsigned int readBufferSize);
void httpsCleanup();
void httpsUpdate();
void* httpsGet(const char *URL, void *headers);
void* httpsPost(const char *URL, const char *body, unsigned int bodyBytes, void *headers);
// in the linked case, we don't copy body at all and expect you to only free it when we are done!
void* httpsPostLinked(const char *URL, const char *body, unsigned int bodyBytes, void *headers);
void* httpsHead(const char *URL, void *headers);
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
void easyOptionUI(unsigned int opt, unsigned int val);
void easyOptionD(unsigned int opt, double val);
int easyHasMetrics(int i);
double easyGetMetricD(int i, int w);
int easyGetMetricI(int i, int w);
const char *easyGetMetricS(int i, int w);
void easyUpdate();	// if you call this is counts as calling the low-level httpsUpdate() above, FYI
int easyGet(const char *URL, const char* *headers, int header_count, bool header_compact);
int easyPost(const char *URL, const char *body, unsigned int bodyBytes, const char* *headers, int header_count, bool header_compact);
int easyHead(const char *URL, const char* *headers, int header_count, bool header_compact);


#ifdef __cplusplus
}
#endif
