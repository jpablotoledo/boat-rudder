#include "ua_parser.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Returns the integer immediately following `marker` in `ua` (skipping any
// spaces in between), or 0 if `marker` isn't present or isn't followed by a
// digit. Only the leading run of digits is read - "Chrome/124.0.6367.82"
// yields 124 (major version only), which is all analytics.c buckets by.
static int version_after(const char *ua, const char *marker) {
    const char *p = strstr(ua, marker);
    if (!p) return 0;
    p += strlen(marker);
    while (*p == ' ') p++;
    int v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    return v;
}

// Last-resort key for a User-Agent this parser doesn't recognize at all:
// the first 64 bytes, with anything non-printable (or '.'/'$', which BSON
// dotted-path notation can't hold) replaced by '_'.
static void raw_ua_key(const char *ua, char *out, size_t size) {
    char buf[65] = {0};
    size_t n = strlen(ua);
    if (n > 64) n = 64;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)ua[i];
        buf[i] = (isprint(c) && c != '.' && c != '$') ? (char)c : '_';
    }
    snprintf(out, size, "%s", buf);
}

void ua_parse_browser(const char *ua, char *out, size_t size) {
    if (!ua || !ua[0]) { snprintf(out, size, "Unknown"); return; }

    int v;

    // Edge and newer Opera both inject "Chrome/" into their own UA, so both
    // must be checked before the plain Chrome branch below.
    if (strstr(ua, "Edg/") || strstr(ua, "Edge/")) {
        v = version_after(ua, "Edg/");
        if (!v) v = version_after(ua, "Edge/");
        snprintf(out, size, "Edge/%d", v);
        return;
    }
    if (strstr(ua, "OPR/")) {
        snprintf(out, size, "Opera/%d", version_after(ua, "OPR/"));
        return;
    }
    if (strstr(ua, "Chrome/")) {
        snprintf(out, size, "Chrome/%d", version_after(ua, "Chrome/"));
        return;
    }
    if (strstr(ua, "Firefox/")) {
        snprintf(out, size, "Firefox/%d", version_after(ua, "Firefox/"));
        return;
    }
    // Classic Opera (pre-Blink) also masquerades as MSIE and as old
    // Netscape, so it must be checked before both of those fallbacks.
    if (strstr(ua, "Opera/") || strstr(ua, "Opera ")) {
        v = version_after(ua, "Opera/");
        if (!v) v = version_after(ua, "Opera ");
        snprintf(out, size, "Opera/%d", v);
        return;
    }
    if (strstr(ua, "Trident/")) {
        snprintf(out, size, "MSIE/%d", version_after(ua, "rv:"));
        return;
    }
    if (strstr(ua, "MSIE ")) {
        snprintf(out, size, "MSIE/%d", version_after(ua, "MSIE "));
        return;
    }
    // Chrome/Edge/Opera all also inject "Safari/", so this must come after
    // all three.
    if (strstr(ua, "Safari/")) {
        v = version_after(ua, "Version/");
        if (v) snprintf(out, size, "Safari/%d", v);
        else   snprintf(out, size, "Safari");
        return;
    }
    if (strstr(ua, "Netscape6/")) {
        snprintf(out, size, "Netscape/%d", version_after(ua, "Netscape6/"));
        return;
    }
    if (strstr(ua, "Netscape/")) {
        snprintf(out, size, "Netscape/%d", version_after(ua, "Netscape/"));
        return;
    }
    if (strstr(ua, "Lynx/")) {
        snprintf(out, size, "Lynx/%d", version_after(ua, "Lynx/"));
        return;
    }
    if (strstr(ua, "Links (")) {
        snprintf(out, size, "Links/%d", version_after(ua, "Links ("));
        return;
    }
    if (strstr(ua, "w3m/")) {
        snprintf(out, size, "w3m/%d", version_after(ua, "w3m/"));
        return;
    }
    if (strstr(ua, "NCSA_Mosaic/") || strstr(ua, "NCSA Mosaic/")) {
        v = version_after(ua, "NCSA_Mosaic/");
        if (!v) v = version_after(ua, "NCSA Mosaic/");
        snprintf(out, size, "Mosaic/%d", v);
        return;
    }
    if (strstr(ua, "LII-Cello") || strstr(ua, "Cello/")) {
        snprintf(out, size, "Cello");
        return;
    }
    // WAP 1.x browsers - not in the epoch-3/2/1 chain above at all, these
    // only ever show up on EPOCH_WML requests.
    if (strstr(ua, "WinWAP")) {
        v = version_after(ua, "WinWAP/");
        if (!v) v = version_after(ua, "WinWAP-SPBE/");
        snprintf(out, size, "WinWAP/%d", v);
        return;
    }
    if (strstr(ua, "UP.Browser")) {
        snprintf(out, size, "UP_Browser/%d", version_after(ua, "UP.Browser/"));
        return;
    }
    if (strstr(ua, "OBIGO")) {
        snprintf(out, size, "Obigo");
        return;
    }
    // Every modern browser uses "Mozilla/5.0" - a Mozilla/1-4 prefix with
    // none of the above tokens is genuinely an old Netscape.
    if (strncmp(ua, "Mozilla/", 8) == 0) {
        v = version_after(ua, "Mozilla/");
        if (v > 0 && v < 5) {
            snprintf(out, size, "Netscape/%d", v);
            return;
        }
    }
    raw_ua_key(ua, out, size);
}

// Windows NT kernel version -> marketing name (the only version numbers a
// User-Agent ever actually carries).
static void windows_name(double nt, char *out, size_t size) {
    if (nt >= 10.0)      snprintf(out, size, "Windows/10");
    else if (nt >= 6.3)  snprintf(out, size, "Windows/8_1");
    else if (nt >= 6.2)  snprintf(out, size, "Windows/8");
    else if (nt >= 6.1)  snprintf(out, size, "Windows/7");
    else if (nt >= 6.0)  snprintf(out, size, "Windows/Vista");
    else if (nt >= 5.2)  snprintf(out, size, "Windows/Server2003");
    else if (nt >= 5.1)  snprintf(out, size, "Windows/XP");
    else if (nt >= 5.0)  snprintf(out, size, "Windows/2000");
    else if (nt >= 4.0)  snprintf(out, size, "Windows/NT4");
    else {
        int major = (int)nt;
        int minor = (int)((nt - major) * 10 + 0.5);
        snprintf(out, size, "Windows/NT%d_%d", major, minor);
    }
}

void ua_parse_os(const char *ua, char *out, size_t size) {
    if (!ua || !ua[0]) { snprintf(out, size, "Unknown"); return; }

    // Pre-standard browsers predate any OS disclosure in the UA string at
    // all - classify before anything else so a stray substring match below
    // doesn't misfire on one of these.
    if (strstr(ua, "LII-Cello") || strstr(ua, "Cello/") || strstr(ua, "Lynx/") ||
        strstr(ua, "ViolaWWW") || strstr(ua, "WorldWideWeb") ||
        strstr(ua, "Line Mode") || strstr(ua, "CERN-LineMode")) {
        snprintf(out, size, "Unknown");
        return;
    }

    int v;
    // Checked before the generic Linux branch below, since every Android UA
    // also contains "Linux".
    if (strstr(ua, "Android")) {
        v = version_after(ua, "Android ");
        if (v) snprintf(out, size, "Android/%d", v);
        else   snprintf(out, size, "Android");
        return;
    }
    if (strstr(ua, "iPhone") || strstr(ua, "iPad")) {
        v = version_after(ua, "OS ");
        if (v) snprintf(out, size, "iOS/%d", v);
        else   snprintf(out, size, "iOS");
        return;
    }
    if (strstr(ua, "Windows NT ")) {
        double nt = 0;
        sscanf(strstr(ua, "Windows NT ") + strlen("Windows NT "), "%lf", &nt);
        windows_name(nt, out, size);
        return;
    }
    if (strstr(ua, "Win32")) { snprintf(out, size, "Windows/x86"); return; }
    if (strstr(ua, "Windows 95") || strstr(ua, "Win95")) { snprintf(out, size, "Windows/95"); return; }
    if (strstr(ua, "Windows 98") || strstr(ua, "Win98")) { snprintf(out, size, "Windows/98"); return; }
    if (strstr(ua, "Windows ME") || strstr(ua, "WinME")) { snprintf(out, size, "Windows/ME"); return; }
    if (strstr(ua, "Win16") || strstr(ua, "Windows 3"))  { snprintf(out, size, "Windows/3x"); return; }
    if (strstr(ua, "Windows x86")) { snprintf(out, size, "Windows/x86"); return; }
    if (strstr(ua, "Mac OS X ")) {
        v = version_after(ua, "Mac OS X ");
        if (v) snprintf(out, size, "Mac/%d", v);
        else   snprintf(out, size, "Mac");
        return;
    }
    if (strstr(ua, "Linux")) {
        static const char *archs[] = {
            "x86_64", "i686", "i386", "aarch64", "armv7l", "armv6l", "mips", "ppc64", NULL
        };
        for (int i = 0; archs[i]; i++) {
            if (strstr(ua, archs[i])) { snprintf(out, size, "Linux/%s", archs[i]); return; }
        }
        snprintf(out, size, "Linux");
        return;
    }
    if (strstr(ua, "FreeBSD")) {
        v = version_after(ua, "FreeBSD ");
        if (v) snprintf(out, size, "FreeBSD/%d", v);
        else   snprintf(out, size, "FreeBSD");
        return;
    }
    if (strstr(ua, "OpenBSD")) { snprintf(out, size, "OpenBSD"); return; }
    if (strstr(ua, "NetBSD"))  { snprintf(out, size, "NetBSD");  return; }
    if (strstr(ua, "BeOS")) {
        v = version_after(ua, "BeOS/");
        if (v) snprintf(out, size, "BeOS/%d", v);
        else   snprintf(out, size, "BeOS");
        return;
    }
    if (strstr(ua, "OS/2")) { snprintf(out, size, "OS2"); return; }
    if (strstr(ua, "SunOS") || strstr(ua, "Solaris")) { snprintf(out, size, "Solaris"); return; }

    raw_ua_key(ua, out, size);
}
