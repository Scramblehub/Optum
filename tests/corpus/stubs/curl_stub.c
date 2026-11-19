/* Built as a shared library with soname libcurl.so.4, so corpus binaries import from it the way
   they would from the real libcurl. The functions do nothing: corpus binaries are never run. */
#include "curl/curl.h"

CURL *curl_easy_init(void) { return 0; }
CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...) { return (void)curl, (void)option, 0; }
CURLcode curl_easy_perform(CURL *curl) { return (void)curl, 0; }
void curl_easy_cleanup(CURL *curl) { (void)curl; }
