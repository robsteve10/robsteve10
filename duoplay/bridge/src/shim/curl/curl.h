/* Minimal stand-in for <curl/curl.h>.
 *
 * chiaki-ng's common.c includes "aia.h" (certificate fetching for its internet
 * holepunch mode), which in turn includes curl. The LAN-only bridge never builds
 * that code, so it only needs the opaque CURL type for the prototypes to parse. */
#ifndef DUOPLAY_CURL_SHIM_H
#define DUOPLAY_CURL_SHIM_H
typedef void CURL;
#endif
