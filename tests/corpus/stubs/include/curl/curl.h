/* Stand-in for libcurl's <curl/curl.h>, for building the binary corpus without libcurl's
   development files. Only what the samples use, with the real ABI values (curl 7/8), so the
   machine code is what the real header would produce. Never used by Optum itself. */
#ifndef OPTUM_STUB_CURL_H
#define OPTUM_STUB_CURL_H

typedef void CURL;
typedef int CURLcode;

typedef enum {
  CURLOPT_NOBODY = 44,
  CURLOPT_UPLOAD = 46,
  CURLOPT_POST = 47,
  CURLOPT_URL = 10002,
  CURLOPT_POSTFIELDS = 10015,
  CURLOPT_CUSTOMREQUEST = 10036,
  CURLOPT_COPYPOSTFIELDS = 10165
} CURLoption;

CURL *curl_easy_init(void);
CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...);
CURLcode curl_easy_perform(CURL *curl);
void curl_easy_cleanup(CURL *curl);

#endif
