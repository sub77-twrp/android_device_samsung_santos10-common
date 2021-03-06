/*
 * Copyright (c) 2014 Intel Corporation All Rights Reserved
 * Copyright (C) 2012 The Android Open Source Project
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

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

//#define LOG_NDEBUG 0
#define LOG_TAG "IntelPowerHAL"
#include <utils/Log.h>

#include <hardware/hardware.h>
#include <hardware/power.h>

#include "power.h"

#define BOOST_PULSE_SYSFS    "/sys/devices/system/cpu/cpufreq/interactive/boostpulse"
#define BOOST_FREQ_SYSFS     "/sys/devices/system/cpu/cpufreq/interactive/hispeed_freq"
#define BOOST_DURATION_SYSFS "/sys/devices/system/cpu/cpufreq/interactive/boostpulse_duration"

struct intel_power_module {
    struct power_module container;
    uint32_t pulse_duration;
    struct timespec last_boost_time; /* latest POWER_HINT_INTERACTION boost */
};

#define CPUFREQ_PATH "/sys/devices/system/cpu/cpu0/cpufreq/"
#define INTERACTIVE_PATH "/sys/devices/system/cpu/cpufreq/interactive/"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int current_power_profile = -1;

static ssize_t sysfs_write(char *path, char *s)
{
    char buf[80];
    ssize_t len;
    int fd = open(path, O_WRONLY);

    if (fd < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error opening %s: %s\n", path, buf);
        return -1;
    }

    if ((len = write(fd, s, strlen(s))) < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error writing to %s: %s\n", path, buf);
    }

    close(fd);
    ALOGV("wrote '%s' to %s", s, path);

    return len;
}

static int sysfs_write_int(char *path, int value)
{
    char buf[80];
    snprintf(buf, 80, "%d", value);
    return sysfs_write(path, buf);
}

static ssize_t sysfs_read(char *path, char *s, int num_bytes)
{
    char buf[80];
    ssize_t count;
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error reading from %s: %s\n", path, buf);
        return -1;
    }

    if ((count = read(fd, s, (num_bytes - 1))) < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error reading from  %s: %s\n", path, buf);
    } else {
        if ((count >= 1) && (s[count-1] == '\n')) {
            s[count-1] = '\0';
        } else {
            s[count] = '\0';
        }
    }

    close(fd);
    ALOGV("read '%s' from %s", s, path);

    return count;
}

static void fugu_power_init(struct power_module *module)
{
    struct intel_power_module *mod = (struct intel_power_module *) module;
    char boost_freq[32];
    char boostpulse_duration[32];

    /* Keep default boost_freq for fugu => max freq */

    if (sysfs_read(BOOST_FREQ_SYSFS, boost_freq, 32) < 0) {
        strcpy(boost_freq, "?");
    }
    if (sysfs_read(BOOST_DURATION_SYSFS, boostpulse_duration, 32) < 0) {
        /* above should not fail but just in case it does use an arbitrary 20ms value */
        snprintf(boostpulse_duration, 32, "%d", 20000);
    }
    mod->pulse_duration = atoi(boostpulse_duration);
    /* initialize last_boost_time */
    clock_gettime(CLOCK_MONOTONIC, &mod->last_boost_time);

    ALOGI("init done: will boost CPU to %skHz for %dus on input events",
            boost_freq, mod->pulse_duration);
}

static void fugu_power_set_interactive(struct power_module *module, int on)
{
    ALOGI("setInteractive: on=%d", on);
    (void) module; /* unused */
    (void) on; /* unused */
}

static inline void timespec_sub(struct timespec *res, struct timespec *a, struct timespec *b)
{
    res->tv_sec = a->tv_sec - b->tv_sec;
    if (a->tv_sec >= b->tv_sec) {
        res->tv_nsec = a->tv_nsec - b->tv_nsec;
    } else {
        res->tv_nsec = 1000000000 - b->tv_nsec + a->tv_nsec;
        res->tv_sec--;
    }
}

static inline uint64_t timespec_to_us(struct timespec *t)
{
    return t->tv_sec * 1000000 + t->tv_nsec / 1000;
}

static bool check_governor(void)
{
    struct stat s;
    int err = stat(INTERACTIVE_PATH, &s);
    if (err != 0) return false;
    if (S_ISDIR(s.st_mode)) return true;
    return false;
}

static int is_profile_valid(int profile)
{
    return profile >= 0 && profile < PROFILE_MAX;
}

static void set_power_profile(int profile) {
    if (!is_profile_valid(profile)) {
        ALOGE("%s: unknown profile: %d", __func__, profile);
        return;
    }

    if (profile == current_power_profile)
        return;

    // break out early if governor is not interactive
    if (!check_governor()) return;

    sysfs_write_int(INTERACTIVE_PATH "boost",
                    profiles[profile].boost);
    sysfs_write_int(INTERACTIVE_PATH "boostpulse_duration",
                    profiles[profile].boostpulse_duration);
    sysfs_write_int(INTERACTIVE_PATH "go_hispeed_load",
                    profiles[profile].go_hispeed_load);
    sysfs_write_int(INTERACTIVE_PATH "hispeed_freq",
                    profiles[profile].hispeed_freq);
    sysfs_write_int(INTERACTIVE_PATH "io_is_busy",
                    profiles[profile].io_is_busy);
    sysfs_write(INTERACTIVE_PATH "target_loads",
                    profiles[profile].target_loads);
    sysfs_write_int(CPUFREQ_PATH "scaling_min_freq",
                    profiles[profile].scaling_min_freq);
    sysfs_write_int(CPUFREQ_PATH "scaling_max_freq",
                    profiles[profile].scaling_max_freq);

    current_power_profile = profile;
}

static void fugu_power_hint(struct power_module *module, power_hint_t hint, void *data)
{
    struct intel_power_module *mod = (struct intel_power_module *) module;
    struct timespec curr_time;
    struct timespec diff_time;
    uint64_t diff;

    switch (hint) {
        case POWER_HINT_INTERACTION:
        case POWER_HINT_CPU_BOOST:
        case POWER_HINT_LAUNCH_BOOST:
            if (!is_profile_valid(current_power_profile)) {
                ALOGD("%s: no power profile selected yet", __func__);
                return;
            }

            if (!profiles[current_power_profile].boostpulse_duration)
                return;

            // break out early if governor is not interactive
            if (!check_governor()) return;

            clock_gettime(CLOCK_MONOTONIC, &curr_time);
            timespec_sub(&diff_time, &curr_time, &mod->last_boost_time);
            diff = timespec_to_us(&diff_time);

            ALOGV("POWER_HINT_INTERACTION: diff=%llu", diff);

            if (diff > mod->pulse_duration) {
                sysfs_write(BOOST_PULSE_SYSFS, "1");
                mod->last_boost_time = curr_time;
            }
            break;
        case POWER_HINT_SET_PROFILE:
            pthread_mutex_lock(&lock);
            set_power_profile(*(int32_t *)data);
            pthread_mutex_unlock(&lock);
            break;
        case POWER_HINT_VSYNC:
            break;
        default:
            break;
    }
}

int get_feature(struct power_module *module __unused, feature_t feature)
{
    if (feature == POWER_FEATURE_SUPPORTED_PROFILES) {
        return PROFILE_MAX;
    }
    return -1;
}

static struct hw_module_methods_t power_module_methods = {
    .open = NULL,
};

struct intel_power_module HAL_MODULE_INFO_SYM = {
    container:{
        common: {
            tag: HARDWARE_MODULE_TAG,
            module_api_version: POWER_MODULE_API_VERSION_0_2,
            hal_api_version: HARDWARE_HAL_API_VERSION,
            id: POWER_HARDWARE_MODULE_ID,
            name: "Fugu Power HAL",
            author: "Intel",
            methods: &power_module_methods,
        },
        init: fugu_power_init,
        setInteractive: fugu_power_set_interactive,
        powerHint: fugu_power_hint,
        getFeature: get_feature
    },
};
