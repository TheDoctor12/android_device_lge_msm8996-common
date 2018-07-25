/*
 * Copyright (C) 2015, The CyanogenMod Project
 * Copyright (C) 2018, The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <android-base/logging.h>

#include "edify/expr.h"
#include "updater/install.h"

#define MAX(a, b) (((a) > (b)) ? (a) : (b))

#define ALPHABET_LEN 256

#ifdef USES_BOOTDEVICE_PATH
#define TZ_PART_PATH "/dev/block/bootdevice/by-name/tz"
#else
#define TZ_PART_PATH "/dev/block/platform/msm_sdcc.1/by-name/tz"
#endif
#define TZ_VER_STR "QC_IMAGE_VERSION_STRING="
#define TZ_VER_STR_LEN 24
#define TZ_VER_BUF_LEN 255

/* Boyer-Moore string search implementation from Wikipedia */

/* Return longest suffix length of suffix ending at str[p] */
static int max_suffix_len(const char *str, size_t str_len, size_t p) {
    uint32_t i;

    for (i = 0; (str[p - i] == str[str_len - 1 - i]) && (i < p); ) {
        i++;
    }

    return i;
}

/* Generate table of distance between last character of pat and rightmost
 * occurrence of character c in pat
 */
static void bm_make_delta1(int *delta1, const char *pat, size_t pat_len) {
    uint32_t i;
    for (i = 0; i < ALPHABET_LEN; i++) {
        delta1[i] = pat_len;
    }
    for (i = 0; i < pat_len - 1; i++) {
        uint8_t idx = (uint8_t) pat[i];
        delta1[idx] = pat_len - 1 - i;
    }
}

/* Generate table of next possible full match from mismatch at pat[p] */
static void bm_make_delta2(int *delta2, const char *pat, size_t pat_len) {
    int p;
    uint32_t last_prefix = pat_len - 1;

    for (p = pat_len - 1; p >= 0; p--) {
        /* Compare whether pat[p-pat_len] is suffix of pat */
        if (strncmp(pat + p, pat, pat_len - p) == 0) {
            last_prefix = p + 1;
        }
        delta2[p] = last_prefix + (pat_len - 1 - p);
    }

    for (p = 0; p < (int) pat_len - 1; p++) {
        /* Get longest suffix of pattern ending on character pat[p] */
        int suf_len = max_suffix_len(pat, pat_len, p);
        if (pat[p - suf_len] != pat[pat_len - 1 - suf_len]) {
            delta2[pat_len - 1 - suf_len] = pat_len - 1 - p + suf_len;
        }
    }
}

static char * bm_search(const char *str, size_t str_len, const char *pat,
        size_t pat_len) {
    int delta1[ALPHABET_LEN];
    int delta2[pat_len];
    int i;

    bm_make_delta1(delta1, pat, pat_len);
    bm_make_delta2(delta2, pat, pat_len);

    if (pat_len == 0) {
        return (char *) str;
    }

    i = pat_len - 1;
    while (i < (int) str_len) {
        int j = pat_len - 1;
        while (j >= 0 && (str[i] == pat[j])) {
            i--;
            j--;
        }
        if (j < 0) {
            return (char *) (str + i + 1);
        }
        i += MAX(delta1[(uint8_t) str[i]], delta2[j]);
    }

    return NULL;
}

static int get_tz_version(char *ver_str, size_t len) {
    int ret = 0;
    int fd;
    int tz_size;
    char *tz_data = NULL;
    char *offset = NULL;

    fd = open(TZ_PART_PATH, O_RDONLY);
    if (fd < 0) {
        ret = errno;
        goto err_ret;
    }

    tz_size = lseek64(fd, 0, SEEK_END);
    if (tz_size == -1) {
        ret = errno;
        goto err_fd_close;
    }

    tz_data = (char *) mmap(NULL, tz_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (tz_data == (char *)-1) {
        ret = errno;
        goto err_fd_close;
    }

    /* Do Boyer-Moore search across TZ data */
    offset = bm_search(tz_data, tz_size, TZ_VER_STR, TZ_VER_STR_LEN);
    if (offset != NULL) {
        strncpy(ver_str, offset + TZ_VER_STR_LEN, len);
    } else {
        ret = -ENOENT;
    }

    munmap(tz_data, tz_size);
err_fd_close:
    close(fd);
err_ret:
    return ret;
}

/* verify_trustzone("TZ_VERSION", "TZ_VERSION", ...) */
Value * VerifyTrustZoneFn(const char *name, State *state, const std::vector<std::unique_ptr<Expr>>& argv) {
    char current_tz_version[TZ_VER_BUF_LEN];
    size_t i;
    int ret;

    ret = get_tz_version(current_tz_version, TZ_VER_BUF_LEN);
    if (ret) {
        return ErrorAbort(state, kFreadFailure, "%s() failed to read current TZ version: %d",
                name, ret);
    }

    std::vector<std::string> tz_version;
    if (!ReadArgs(state, argv, &tz_version)) {
        return ErrorAbort(state, kArgsParsingFailure, "%s() error parsing arguments", name);
    }

    ret = 0;
    for (i = 0; i < argv.size(); i++) {
       LOG(INFO) << "\nComparing TZ version " << tz_version[i].c_str() << " == " << current_tz_version;
       uiPrintf(state,"Comparing TZ versions:\n");
       uiPrintf(state,"  Must be TZ version: %s\n", tz_version[i].c_str());
       uiPrintf(state,"  Current TZ version: %s\n", current_tz_version);
        if (strncmp(tz_version[i].c_str(), current_tz_version, tz_version[i].length()) == 0) {
            ret = 1;
            break;
        }
    }

    return StringValue(strdup(ret ? "1" : "0"));
}

/* compares two versions */
int versionCompare(std::string v1, std::string v2) {
    // vnum stores each numeric part of version
    unsigned long vnum1 = 0, vnum2 = 0;

    // lop untill both string are processed
    for (unsigned long i=0,j=0; (i<v1.length() || j<v2.length()); ) {
        // storing numeric part of version 1 in vnum1
        while (i < v1.length() && v1[i] != '.' && v1[i] != '-') {
            vnum1 = vnum1 * 10 + (v1[i] - '0');
            i++;
        }

        //  storing numeric part of version 2 in vnum2
        while (j < v2.length() && v2[j] != '.' && v2[j] != '-' ) {
            vnum2 = vnum2 * 10 + (v2[j] - '0');
            j++;
        }

        if (vnum1 > vnum2)
            return 1;
        if (vnum2 > vnum1)
            return -1;

        // if equal, reset variables and go for next numeric
        // part
        vnum1 = vnum2 = 0;
        i++;
        j++;
    }
    return 0;
}

/* verify_min_trustzone("TZ_VERSION", "TZ_VERSION", ...) */
Value * VerifyMinTrustZoneFn(const char *name, State *state, const std::vector<std::unique_ptr<Expr>>& argv) {
    char current_tz_version[TZ_VER_BUF_LEN];
    size_t i;
    int ret;

    ret = get_tz_version(current_tz_version, TZ_VER_BUF_LEN);
    if (ret) {
        return ErrorAbort(state, kFreadFailure, "%s() failed to read current TZ version: %d",
                name, ret);
    }

    std::vector<std::string> tz_version;
    if (!ReadArgs(state, argv, &tz_version)) {
        return ErrorAbort(state, kArgsParsingFailure, "%s() error parsing arguments", name);
    }

    ret = 0;
    for (i = 0; i < argv.size(); i++) {
       LOG(INFO) << "\nComparing TZ version " << tz_version[i].c_str() << " <= " << current_tz_version;
       uiPrintf(state,"Comparing TZ versions:\n");
       uiPrintf(state,"      Min TZ version: %s\n", tz_version[i].c_str());
       uiPrintf(state,"  Current TZ version: %s\n", current_tz_version);
        if ( versionCompare(tz_version[i].c_str(), current_tz_version) <= 0 ) {
            ret = 1;
            break;
        }
    }

    return StringValue(strdup(ret ? "1" : "0"));
}

void Register_librecovery_updater_msm8996() {
    RegisterFunction("msm8996.verify_trustzone", VerifyTrustZoneFn);
    RegisterFunction("msm8996.verify_min_trustzone", VerifyMinTrustZoneFn);
}
