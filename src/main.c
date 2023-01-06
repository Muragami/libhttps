/*
	test program for libhttps dynamic library
	Jason A. Petrasko 2022, MIT License
*/

#include "https.h"
#include "stdlib.h"
#include "stdio.h"
#include "string.h"

bool done = false;
int handle = -1;

typedef const char* (*_easyGetHeader)(int i, const char *header);

void theCallback(int handle, const char* url, const char* msg, int code, unsigned int sz, void* data)
{
	_easyGetHeader ezHead = (_easyGetHeader)data;
	if (!strcmp(msg, "START")) {
		printf("Started download from URL: %s\n", url);
	}
	if (!strcmp(msg, "UPDATE")) {
		printf("Return code from response: %d\n", code);
	}
	if (!strcmp(msg, "HEADERS")) {
		printf("All headers read.\n");
		printf(" -- Content-Length: %s\n", ezHead(handle, "Content-Length"));
		printf(" -- Content-Encoding: %s\n", ezHead(handle, "Content-Encoding"));
	}
	if (!strcmp(msg, "READ")) {
		printf("Read %d bytes.\n", code);
	}
	if (!strcmp(msg, "LENGTH")) {
		printf("Content-Length: %d\n", code);
	}
	if (!strcmp(msg, "MIME")) {
		printf("Content-Mime-Type: %s\n", (char*)data);
	}
	if (!strcmp(msg, "COMPLETE")) {
		printf("Response complete!\n");
		done = true;
	}
}

int main(int argc, char *argv[])
{
	easySetup(theCallback);
	if (argc < 2)
	{
		printf("readurl.exe usage: readurl <url>\n");
		return 0;
	}
	handle = easyGet(argv[1], NULL, 0, false);
	while (!done) {
		easyUpdate();
	}
}

