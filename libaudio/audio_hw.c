/*
 * Copyright (C) 2011 The Android Open Source Project
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

#define LOG_TAG "audio_hw_tegra"
//#define LOG_NDEBUG 0

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>

#include <cutils/log.h>
#include <cutils/properties.h>
#include <cutils/str_parms.h>

#include <hardware/audio.h>
#include <hardware/hardware.h>

#include <system/audio.h>

#include <tinyalsa/asoundlib.h>

#include <audio_utils/resampler.h>
#include <audio_route/audio_route.h>



#define PCM_CARD 0
#define PCM_DEVICE 0

#define MIXER_CARD 0

#define OUT_PERIOD_SIZE 1024
#define OUT_SHORT_PERIOD_COUNT 2
#define OUT_LONG_PERIOD_COUNT 4
#define OUT_SAMPLING_RATE 44100

#define IN_PERIOD_SIZE 1024
#define IN_PERIOD_SIZE_LOW_LATENCY 512
#define IN_PERIOD_COUNT 4
#define IN_SAMPLING_RATE 44100

/* minimum sleep time in out_write() when write threshold is not reached */
#define MIN_WRITE_SLEEP_US 2000
#define MAX_WRITE_SLEEP_US ((OUT_PERIOD_SIZE * OUT_SHORT_PERIOD_COUNT * 1000000) \
                                / OUT_SAMPLING_RATE)

enum {
    OUT_BUFFER_TYPE_UNKNOWN,
    OUT_BUFFER_TYPE_SHORT,
    OUT_BUFFER_TYPE_LONG,
};

struct pcm_config pcm_config_out = {
    .channels = 2,
    .rate = OUT_SAMPLING_RATE,
    .period_size = OUT_PERIOD_SIZE,
    .period_count = OUT_LONG_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = OUT_PERIOD_SIZE * OUT_SHORT_PERIOD_COUNT,
    // .start_threshold = 0,
    // .stop_threshold = 0,
    // .silence_threshold = 0,
    // .avail_min = 0,
};

struct pcm_config pcm_config_in = {
    .channels = 2,
    .rate = IN_SAMPLING_RATE,
    .period_size = IN_PERIOD_SIZE,
    .period_count = IN_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = 1,
    .stop_threshold = (IN_PERIOD_SIZE * IN_PERIOD_COUNT),
    // .start_threshold = 0,
    // .stop_threshold = 0,
    // .silence_threshold = 0,
    // .avail_min = 0,
};

struct pcm_config pcm_config_in_low_latency = {
    .channels = 2,
    .rate = IN_SAMPLING_RATE,
    .period_size = IN_PERIOD_SIZE_LOW_LATENCY,
    .period_count = IN_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = 1,
    .stop_threshold = (IN_PERIOD_SIZE_LOW_LATENCY * IN_PERIOD_COUNT),
};

struct audio_device {
    struct audio_hw_device hw_device;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    audio_mode_t mode;
    unsigned int out_device;
    unsigned int in_device;
    unsigned int in_source;
    bool standby;
    bool mic_mute;
    // struct audio_route *ar;
    // struct mixer* mixer;
    bool screen_off;

    int lock_cnt;

    struct stream_out *active_out;
    struct stream_in *active_in;
};

struct stream_out {
    struct audio_stream_out stream;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    struct pcm *pcm;
    struct pcm_config *pcm_config;
    bool standby;
    uint64_t written; /* total frames written, not cleared when entering standby */

    struct resampler_itfe *resampler;
    int16_t *buffer;
    size_t buffer_frames;

    int write_threshold;
    int cur_write_threshold;
    int buffer_type;

    bool sleep_req;
    int lock_cnt;

    struct audio_device *dev;
};

struct stream_in {
    struct audio_stream_in stream;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    struct pcm *pcm;
    struct pcm_config *pcm_config;          /* current configuration */
    bool standby;

    unsigned int requested_rate;
    struct resampler_itfe *resampler;
    struct resampler_buffer_provider buf_provider;
    int16_t *buffer;
    size_t buffer_size;
    size_t frames_in;
    int read_status;

    bool sleep_req;
    int lock_cnt;

    struct audio_device *dev;
};

static uint32_t out_get_sample_rate(const struct audio_stream *stream);
static size_t out_get_buffer_size(const struct audio_stream *stream);
static audio_format_t out_get_format(const struct audio_stream *stream);
static uint32_t in_get_sample_rate(const struct audio_stream *stream);
static size_t in_get_buffer_size(const struct audio_stream *stream);
static audio_format_t in_get_format(const struct audio_stream *stream);
static int get_next_buffer(struct resampler_buffer_provider *buffer_provider,
                                   struct resampler_buffer* buffer);
static void release_buffer(struct resampler_buffer_provider *buffer_provider,
                                  struct resampler_buffer* buffer);

/*
 * NOTE: when multiple mutexes have to be acquired, always take the
 * audio_device mutex first, followed by the stream_in and/or
 * stream_out mutexes.
 */

/* Helper functions */

static struct mixer* open_mixer()
{
    struct mixer* mixer;

    mixer = mixer_open(0);
    if (mixer == NULL) {
        ALOGE("open_mixer() cannot open mixer");
    }

    return mixer;
}

static void close_mixer(struct mixer* mixer)
{
    mixer_close(mixer);
}

static void select_devices(struct audio_device *adev, struct mixer* mixer)
{
    int headphone_on;
    int headset_on;
    int speaker_on;
    int main_mic_on;
    int headset_mic_on;
    int bt_sco_on;

    headphone_on = adev->out_device & (AUDIO_DEVICE_OUT_WIRED_HEADSET |
                                    AUDIO_DEVICE_OUT_WIRED_HEADPHONE);
    headset_on = adev->out_device & AUDIO_DEVICE_OUT_WIRED_HEADSET;
    speaker_on = adev->out_device & AUDIO_DEVICE_OUT_SPEAKER;
    main_mic_on = adev->in_device & AUDIO_DEVICE_IN_BUILTIN_MIC;
    headset_mic_on = adev->in_device & AUDIO_DEVICE_IN_WIRED_HEADSET;
    bt_sco_on = adev->in_device & AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET;

    // audio_route_reset(adev->ar);

    // if (main_mic_on)
    //     audio_route_apply_path(adev->ar, "main-mic");
    // else if (headset_mic_on)
    //     audio_route_apply_path(adev->ar, "headset-mic");
    // else if (bt_sco_on)
    //     audio_route_apply_path(adev->ar, "bt-sco-mic");
    // else
    //     audio_route_apply_path(adev->ar, "mic-off");

    // if (speaker_on && headphone_on)
    //     audio_route_apply_path(adev->ar, "speaker-and-headphone");
    // else if (headset_on)
    //     audio_route_apply_path(adev->ar, "headset");
    // else if (headphone_on)
    //     audio_route_apply_path(adev->ar, "headphone");
    // else if (speaker_on)
    //     audio_route_apply_path(adev->ar, "speaker");
    // else
    //     audio_route_apply_path(adev->ar, "output-off");

    // audio_route_update_mixer(adev->ar);


    // struct mixer* mixer = adev->mixer;
    struct mixer_ctl *m_route_ctl = NULL;

    m_route_ctl = mixer_get_ctl_by_name(mixer, "Playback Path");
    if (speaker_on && headphone_on) {
        mixer_ctl_set_enum_by_string(m_route_ctl, "SPK_HP");
    } else if (speaker_on) {
        mixer_ctl_set_enum_by_string(m_route_ctl, "SPK");
    } else if (headset_on) {
        mixer_ctl_set_enum_by_string(m_route_ctl, "HP_NO_MIC");
    } else if (headphone_on) {
        mixer_ctl_set_enum_by_string(m_route_ctl, "HP");
    }

    m_route_ctl = mixer_get_ctl_by_name(mixer, "Capture MIC Path");
    if (main_mic_on) {
        mixer_ctl_set_enum_by_string(m_route_ctl, "Main Mic");
    } else if (headset_mic_on) {
        mixer_ctl_set_enum_by_string(m_route_ctl, "Hands Free Mic");
    } else if (bt_sco_on) {
        mixer_ctl_set_enum_by_string(m_route_ctl, "BT Sco Mic");
    } else {
        mixer_ctl_set_enum_by_string(m_route_ctl, "MIC OFF");
    }

    ALOGD("hp=%c speaker=%c main-mic=%c headset-mic=%c",
            headphone_on ? 'y' : 'n',
            speaker_on ? 'y' : 'n',
            main_mic_on ? 'y' : 'n',
            headset_mic_on ? 'y' : 'n');
}

static void select_input_source(struct audio_device *adev, struct mixer* mixer)
{
    // int input_source = adev->in_source;

    // switch (input_source) {
    //     case AUDIO_SOURCE_DEFAULT:
    //     case AUDIO_SOURCE_MIC:
    //     case AUDIO_SOURCE_VOICE_COMMUNICATION:
    //         audio_route_apply_path(adev->ar, "in-source-default");
    //         break;
    //     case AUDIO_SOURCE_CAMCORDER:
    //         audio_route_apply_path(adev->ar, "in-source-camcorder");
    //         break;
    //     case AUDIO_SOURCE_VOICE_RECOGNITION:
    //         audio_route_apply_path(adev->ar, "in-source-voice-recog");
    //         break;
    //     case AUDIO_SOURCE_VOICE_UPLINK:
    //     case AUDIO_SOURCE_VOICE_DOWNLINK:
    //     case AUDIO_SOURCE_VOICE_CALL:
    //     default:
    //         audio_route_apply_path(adev->ar, "in-source-default");
    //         break;
    //  }

    // audio_route_update_mixer(adev->ar);

    const char* source_name;
    int input_source = adev->in_source;
    struct mixer_ctl *ctl= mixer_get_ctl_by_name(mixer, "Input Source");

    if (ctl == NULL) {
        ALOGE("select_input_source: Error: Could not open mixer.");
        return;
    }

    switch (input_source) {
        case AUDIO_SOURCE_DEFAULT:
        case AUDIO_SOURCE_MIC:
        case AUDIO_SOURCE_VOICE_COMMUNICATION:
            source_name = "Default";
            break;
        case AUDIO_SOURCE_CAMCORDER:
            source_name = "Camcorder";
            break;
        case AUDIO_SOURCE_VOICE_RECOGNITION:
            source_name = "Voice Recognition";
            break;
        case AUDIO_SOURCE_VOICE_UPLINK:
        case AUDIO_SOURCE_VOICE_DOWNLINK:
        case AUDIO_SOURCE_VOICE_CALL:
        default:
            source_name = "Default";
            break;
     }

    mixer_ctl_set_enum_by_string(ctl, source_name);
    ALOGD("select_input_source %s", source_name);

    ALOGD("select_input_source: done.");
 }

/* must be called with hw device and output stream mutexes locked */
static void do_out_standby(struct stream_out *out)
{
    struct audio_device *adev = out->dev;

    if (!out->standby) {
        pcm_close(out->pcm);
        out->pcm = NULL;
        adev->active_out = NULL;
        if (out->resampler) {
            release_resampler(out->resampler);
            out->resampler = NULL;
        }
        if (out->buffer) {
            free(out->buffer);
            out->buffer = NULL;
        }
        out->standby = true;
    } else {
        ALOGD("do_out_standby() did nothing. Called with out->standby already true.");
    }
}

/* must be called with hw device and input stream mutexes locked */
static void do_in_standby(struct stream_in *in)
{
    struct audio_device *adev = in->dev;

    if (!in->standby) {
        pcm_close(in->pcm);
        in->pcm = NULL;
        adev->active_in = NULL;
        if (in->resampler) {
            release_resampler(in->resampler);
            in->resampler = NULL;
        }
        if (in->buffer) {
            free(in->buffer);
            in->buffer = NULL;
        }
        in->standby = true;
    } else {
        ALOGD("do_in_standby() did nothing. Called with in->standby already true.");
    }
}

/* must be called with hw device and output stream mutexes locked */
static int start_output_stream(struct stream_out *out)
{
    struct audio_device *adev = out->dev;
    unsigned int device;
    int ret;

    ALOGD("start_output_stream()");

    device = PCM_DEVICE;
    out->pcm_config = &pcm_config_out;
    out->buffer_type = OUT_BUFFER_TYPE_UNKNOWN;

    out->pcm = pcm_open(PCM_CARD, device, PCM_OUT | PCM_NORESTART | PCM_MONOTONIC, out->pcm_config);

    if (out->pcm && !pcm_is_ready(out->pcm)) {
        ALOGE("pcm_open(out) failed: %s", pcm_get_error(out->pcm));
        pcm_close(out->pcm);
        return -ENOMEM;
    }
    ALOGE("pcm_open(out) opened");

    /*
     * If the stream rate differs from the PCM rate, we need to
     * create a resampler.
     */
    if (out_get_sample_rate(&out->stream.common) != out->pcm_config->rate) {
        ret = create_resampler(out_get_sample_rate(&out->stream.common),
                               out->pcm_config->rate,
                               out->pcm_config->channels,
                               RESAMPLER_QUALITY_DEFAULT,
                               NULL,
                               &out->resampler);
        out->buffer_frames = (pcm_config_out.period_size * out->pcm_config->rate) /
                out_get_sample_rate(&out->stream.common) + 1;

        out->buffer = malloc(pcm_frames_to_bytes(out->pcm, out->buffer_frames));

        ALOGE("pcm_open(out) created resampler. %d -> %d", out_get_sample_rate(&out->stream.common),
            out->pcm_config->rate);
    }

    adev->active_out = out;

    ALOGD("start_output_stream() done");

    return 0;
}

/* must be called with hw device and input stream mutexes locked */
static int start_input_stream(struct stream_in *in)
{
    struct audio_device *adev = in->dev;
    int ret;

    ALOGD("start_input_stream()");

    in->pcm = pcm_open(PCM_CARD, PCM_DEVICE, PCM_IN, in->pcm_config);

    if (in->pcm && !pcm_is_ready(in->pcm)) {
        ALOGE("pcm_open(in) failed: %s", pcm_get_error(in->pcm));
        pcm_close(in->pcm);
        return -ENOMEM;
    }
    ALOGD("start_input_stream() opened");

    /*
     * If the stream rate differs from the PCM rate, we need to
     * create a resampler.
     */
    if (in_get_sample_rate(&in->stream.common) != in->pcm_config->rate) {
        in->buf_provider.get_next_buffer = get_next_buffer;
        in->buf_provider.release_buffer = release_buffer;

        ret = create_resampler(in->pcm_config->rate,
                               in_get_sample_rate(&in->stream.common),
                               1,
                               RESAMPLER_QUALITY_DEFAULT,
                               &in->buf_provider,
                               &in->resampler);
        ALOGD("start_input_stream() created resampler %d -> %d", in->pcm_config->rate,
            in_get_sample_rate(&in->stream.common));
    }
    in->buffer_size = pcm_frames_to_bytes(in->pcm,
                                          in->pcm_config->period_size);
    in->buffer = malloc(in->buffer_size);
    in->frames_in = 0;

    adev->active_in = in;

    ALOGD("start_input_stream() done");
    return 0;
}

static int get_next_buffer(struct resampler_buffer_provider *buffer_provider,
                                   struct resampler_buffer* buffer)
{
    struct stream_in *in;

    if (buffer_provider == NULL || buffer == NULL)
        return -EINVAL;

    in = (struct stream_in *)((char *)buffer_provider -
                                   offsetof(struct stream_in, buf_provider));

    if (in->pcm == NULL) {
        buffer->raw = NULL;
        buffer->frame_count = 0;
        in->read_status = -ENODEV;
        return -ENODEV;
    }

    if (in->frames_in == 0) {
        in->read_status = pcm_read(in->pcm,
                                   (void*)in->buffer,
                                   in->buffer_size);
        if (in->read_status != 0) {
            ALOGE("get_next_buffer() pcm_read error %d", in->read_status);
            buffer->raw = NULL;
            buffer->frame_count = 0;
            return in->read_status;
        }
        in->frames_in = in->pcm_config->period_size;
        if (in->pcm_config->channels == 2) {
            unsigned int i;

            /* Discard right channel */
            for (i = 1; i < in->frames_in; i++)
                in->buffer[i] = in->buffer[i * 2];
        }
    }

    buffer->frame_count = (buffer->frame_count > in->frames_in) ?
                                in->frames_in : buffer->frame_count;
    buffer->i16 = in->buffer + (in->pcm_config->period_size - in->frames_in);

    return in->read_status;

}

static void release_buffer(struct resampler_buffer_provider *buffer_provider,
                                  struct resampler_buffer* buffer)
{
    struct stream_in *in;

    if (buffer_provider == NULL || buffer == NULL)
        return;

    in = (struct stream_in *)((char *)buffer_provider -
                                   offsetof(struct stream_in, buf_provider));

    in->frames_in -= buffer->frame_count;
}

/* read_frames() reads frames from kernel driver, down samples to capture rate
 * if necessary and output the number of frames requested to the buffer specified */
static ssize_t read_frames(struct stream_in *in, void *buffer, ssize_t frames)
{
    ssize_t frames_wr = 0;

    while (frames_wr < frames) {
        size_t frames_rd = frames - frames_wr;
        if (in->resampler != NULL) {
            in->resampler->resample_from_provider(in->resampler,
                    (int16_t *)((char *)buffer +
                            frames_wr * audio_stream_in_frame_size(&in->stream)),
                    &frames_rd);
        } else {
            struct resampler_buffer buf = {
                    { raw : NULL, },
                    frame_count : frames_rd,
            };
            get_next_buffer(&in->buf_provider, &buf);
            if (buf.raw != NULL) {
                memcpy((char *)buffer +
                           frames_wr * audio_stream_in_frame_size(&in->stream),
                        buf.raw,
                        buf.frame_count * audio_stream_in_frame_size(&in->stream));
                frames_rd = buf.frame_count;
            }
            release_buffer(&in->buf_provider, &buf);
        }
        /* in->read_status is updated by getNextBuffer() also called by
         * in->resampler->resample_from_provider() */
        if (in->read_status != 0)
            return in->read_status;

        frames_wr += frames_rd;
    }
    return frames_wr;
}

static void out_lock(struct stream_out *out) {
    pthread_mutex_lock(&out->lock);
    out->lock_cnt++;
    ALOGV("out_lock() %d", out->lock_cnt);
}

static void out_unlock(struct stream_out *out) {
    out->lock_cnt--;
    ALOGV("out_unlock() %d", out->lock_cnt);
    pthread_mutex_unlock(&out->lock);
}

static void in_lock(struct stream_in *in) {
    pthread_mutex_lock(&in->lock);
    in->lock_cnt++;
    ALOGV("in_lock() %d", in->lock_cnt);
}

static void in_unlock(struct stream_in *in) {
    in->lock_cnt--;
    ALOGV("in_unlock() %d", in->lock_cnt);
    pthread_mutex_unlock(&in->lock);
}

static void adev_lock(struct audio_device *adev) {
    pthread_mutex_lock(&adev->lock);
    adev->lock_cnt++;
    ALOGV("adev_lock() %d", adev->lock_cnt);
}

static void adev_unlock(struct audio_device *adev) {
    adev->lock_cnt--;
    ALOGV("adev_unlock() %d", adev->lock_cnt);
    pthread_mutex_unlock(&adev->lock);
}


/* API functions */

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    return pcm_config_out.rate;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return 0;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    return pcm_config_out.period_size *
               audio_stream_out_frame_size((const struct audio_stream_out *)stream);
}

static audio_channel_mask_t out_get_channels(const struct audio_stream *stream)
{
    return AUDIO_CHANNEL_OUT_STEREO;
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int out_set_format(struct audio_stream *stream, audio_format_t format)
{
    return -ENOSYS;
}

static int out_standby(struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    ALOGD("out_standby()");

    out->sleep_req = true;
    out_lock(out);
    out->sleep_req = false;
    adev_lock(out->dev);
    do_out_standby(out);
    adev_unlock(out->dev);
    out_unlock(out);

    return 0;
}

static int out_dump(const struct audio_stream *stream, int fd)
{
    ALOGD("out_dump()");
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    struct str_parms *parms;
    char value[32];
    int ret;
    unsigned int val;

    ALOGD("out_set_parameters()");

    parms = str_parms_create_str(kvpairs);

    out->sleep_req = true;
    out_lock(out);
    out->sleep_req = false;
    adev_lock(adev);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING,
                            value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value);
        if ((adev->out_device != val) && (val != 0)) {
            if (adev->out_device != val) {
                if (adev->mode != AUDIO_MODE_IN_CALL) {

                    if (!out->standby)
                       do_out_standby(out);
                }
            }

            adev->out_device = val;
            // select_devices(adev);
        }
    }

    adev_unlock(adev);
    out_unlock(out);

    str_parms_destroy(parms);
    return ret;
}

static char * out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    return strdup("");
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    size_t period_count;

    period_count = OUT_LONG_PERIOD_COUNT;

    return (pcm_config_out.period_size * period_count * 1000) / pcm_config_out.rate;
}

static int out_set_volume(struct audio_stream_out *stream, float left,
                          float right)
{
    return -ENOSYS;
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer,
                         size_t bytes)
{
    int ret = 0;
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    size_t frame_size = audio_stream_out_frame_size(stream);
    int16_t *in_buffer = (int16_t *)buffer;
    size_t in_frames = bytes / frame_size;
    size_t out_frames;
    int buffer_type;
    int kernel_frames;

    bool in_locked = false;
    bool restart_input = false;

     ALOGV("-----out_write(%p, %d) START", buffer, (int)bytes);

    if (out->sleep_req) {
        // 10ms are always shorter than the time to reconfigure the audio path
        // which is the only condition when sleep_req would be true.
        ALOGD("out_write(): out->sleep_req: sleeping");
        usleep(10000);

        if (adev->active_out != out) {
            ALOGE("out_write() active out changed. abandoning this session.");
            // out_standby(out);
            // return 0;
            // out = adev->active_out;
            // return -EPIPE;
        }
    }

    /*
     * acquiring hw device mutex systematically is useful if a low
     * priority thread is waiting on the output stream mutex - e.g.
     * executing out_set_parameters() while holding the hw device
     * mutex
     */
    out_lock(out);
    if (out->standby) {
        ALOGD("out_write(): pcm playback is exiting standby %x.", out);
        adev_lock(adev);

        struct stream_in* in = adev->active_in;
        while (in != NULL && !in->standby) {
            ALOGD("out_write(): Warning: active_in is present.");

            // undo locks so that input can be locked in proper order
            adev_unlock(adev);

            ALOGV("out_write(): take input locks.");
            in->sleep_req = true;
            in_lock(in);
            // in->sleep_req = false;
            adev_lock(adev);

            // if (in == adev->active_in && in->standby == false) {
            if (in == adev->active_in) {
                // Here the input is locked and a sleep has been requested
                if (!in->standby) {
                    ALOGD("out_write(): Warning: active_in is present and NOT in standby.");
                    restart_input = true;
                    ALOGD("out_write(): forcing input standby");
                    do_in_standby(in);
                }

                ALOGD("out_write(): input wait done.");
                in_locked = true;
                break;
            }

            // restore the locks
            ALOGD("out_write(): release in lock.");
            in_unlock(in);
            in = adev->active_in;
        }

        ALOGD("out_write(): starting output stream.");
        ret = start_output_stream(out);
        if (ret != 0) {
            // adev_unlock(adev);
            ALOGE("out_write() Error starting output stream.");
            goto exit;
        }
        ALOGD("out_write(): starting output stream done.");

        if (in) {
            if (restart_input && in->standby) {
                ALOGD("out_write(): start input stream.");
                ret = start_input_stream(in);
                if (ret == 0) {
                    in->standby = false;

                    // ALOGD("out_write(): input standby.");
                    // do_in_standby(in);
                }
            }
            if (in_locked) {
                ALOGD("out_write(): release input lock.");
                in_unlock(in);
            }
            in->sleep_req = false;
        }

        /*
         * mixer must be set when coming out of standby
         */
        ALOGD("out_write(): selecting devices.");
        // audio_route_reset(adev->ar);
        struct mixer* mixer;
        mixer = open_mixer();
        select_devices(adev, mixer);
        close_mixer(mixer);
        // audio_route_update_mixer(adev->ar);

        out->standby = false;
        adev_unlock(adev);
        ALOGD("pcm playback is exiting standby. done.");
    }
    buffer_type = (adev->screen_off && !adev->active_in) ?
            OUT_BUFFER_TYPE_LONG : OUT_BUFFER_TYPE_SHORT;

    /* detect changes in screen ON/OFF state and adapt buffer size
     * if needed. Do not change buffer size when routed to SCO device. */
    if (buffer_type != out->buffer_type) {
        size_t period_count;

        if (buffer_type == OUT_BUFFER_TYPE_LONG)
            period_count = OUT_LONG_PERIOD_COUNT;
        else
            period_count = OUT_SHORT_PERIOD_COUNT;

        out->write_threshold = out->pcm_config->period_size * period_count;
        /* reset current threshold if exiting standby */
        if (out->buffer_type == OUT_BUFFER_TYPE_UNKNOWN)
            out->cur_write_threshold = out->write_threshold;
        out->buffer_type = buffer_type;
    }

    /* Reduce number of channels, if necessary */
    if (audio_channel_count_from_out_mask(out_get_channels(&stream->common)) >
                 (int)out->pcm_config->channels) {
        unsigned int i;

        /* Discard right channel */
        for (i = 1; i < in_frames; i++)
            in_buffer[i] = in_buffer[i * 2];

        /* The frame size is now half */
        frame_size /= 2;
    }

    /* Change sample rate, if necessary */
    if (out_get_sample_rate(&stream->common) != out->pcm_config->rate) {
        out_frames = out->buffer_frames;
        out->resampler->resample_from_input(out->resampler,
                                            in_buffer, &in_frames,
                                            out->buffer, &out_frames);
        in_buffer = out->buffer;
    } else {
        out_frames = in_frames;
    }

    {
        int total_sleep_time_us = 0;
        size_t period_size = out->pcm_config->period_size;

        /* do not allow more than out->cur_write_threshold frames in kernel
         * pcm driver buffer */
        do {
            struct timespec time_stamp;
            if (pcm_get_htimestamp(out->pcm,
                                   (unsigned int *)&kernel_frames,
                                   &time_stamp) < 0)
                break;
            kernel_frames = pcm_get_buffer_size(out->pcm) - kernel_frames;

            if (kernel_frames > out->cur_write_threshold) {
                int sleep_time_us =
                    (int)(((int64_t)(kernel_frames - out->cur_write_threshold)
                                    * 1000000) / out->pcm_config->rate);
                if (sleep_time_us < MIN_WRITE_SLEEP_US)
                    break;
                total_sleep_time_us += sleep_time_us;
                if (total_sleep_time_us > MAX_WRITE_SLEEP_US) {
                    ALOGW("out_write() limiting sleep time %d to %d",
                          total_sleep_time_us, MAX_WRITE_SLEEP_US);
                    sleep_time_us = MAX_WRITE_SLEEP_US -
                                        (total_sleep_time_us - sleep_time_us);
                }
                usleep(sleep_time_us);
            }

        } while ((kernel_frames > out->cur_write_threshold) &&
                (total_sleep_time_us <= MAX_WRITE_SLEEP_US));

        /* do not allow abrupt changes on buffer size. Increasing/decreasing
         * the threshold by steps of 1/4th of the buffer size keeps the write
         * time within a reasonable range during transitions.
         * Also reset current threshold just above current filling status when
         * kernel buffer is really depleted to allow for smooth catching up with
         * target threshold.
         */
        if (out->cur_write_threshold > out->write_threshold) {
            out->cur_write_threshold -= period_size / 4;
            if (out->cur_write_threshold < out->write_threshold) {
                out->cur_write_threshold = out->write_threshold;
            }
        } else if (out->cur_write_threshold < out->write_threshold) {
            out->cur_write_threshold += period_size / 4;
            if (out->cur_write_threshold > out->write_threshold) {
                out->cur_write_threshold = out->write_threshold;
            }
        } else if ((kernel_frames < out->write_threshold) &&
            ((out->write_threshold - kernel_frames) >
                (int)(period_size * OUT_SHORT_PERIOD_COUNT))) {
            out->cur_write_threshold = (kernel_frames / period_size + 1) * period_size;
            out->cur_write_threshold += period_size / 4;
        }
    }

    ret = pcm_write(out->pcm, in_buffer, out_frames * frame_size);
    if (ret == -EPIPE) {
        /* In case of underrun, don't sleep since we want to catch up asap */
        // adev_unlock(adev);
        out_unlock(out);

        ALOGV("-----out_write(%p, %d) END WITH ERROR -EPIPE", buffer, (int)bytes);

        return ret;
    }
    if (ret == 0) {
        out->written += out_frames;
    }

exit:
    // adev_unlock(adev);
    out_unlock(out);

    if (ret != 0) {
        usleep(bytes * 1000000 / audio_stream_out_frame_size(&stream->common) /
               out_get_sample_rate(&stream->common));
    }

    ALOGV("-----out_write(%p, %d) END", buffer, (int)bytes);

    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames)
{
    return -EINVAL;
}

static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream,
                                        int64_t *timestamp)
{
    return -EINVAL;
}

static int out_get_presentation_position(const struct audio_stream_out *stream,
                                   uint64_t *frames, struct timespec *timestamp)
{
    struct stream_out *out = (struct stream_out *)stream;
    int ret = -1;

    out->sleep_req = true;
    out_lock(out);
    out->sleep_req = false;

    if (out->standby) {
        ALOGE("out_get_presentation_position() out stream is in standby.");
        out_unlock(out);
        return -ENOSYS;
    }

    if (out->pcm == NULL) {
        ALOGE("out_get_presentation_position() out->pcm is NULL");
        out_unlock(out);
        return -ENOSYS;
    }

    size_t avail;
    if (pcm_get_htimestamp(out->pcm, &avail, timestamp) == 0) {
        size_t kernel_buffer_size = out->pcm_config->period_size * out->pcm_config->period_count;
        // FIXME This calculation is incorrect if there is buffering after app processor
        int64_t signed_frames = out->written - kernel_buffer_size + avail;
        // It would be unusual for this value to be negative, but check just in case ...
        if (signed_frames >= 0) {
            *frames = signed_frames;
            ret = 0;
        }
    }

    out_unlock(out);

    return ret;
}

/** audio_stream_in implementation **/
static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    return in->requested_rate;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    ALOGD("in_set_sample_rate()");
    return 0;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;
    size_t size;

    /*
     * take resampling into account and return the closest majoring
     * multiple of 16 frames, as audioflinger expects audio buffers to
     * be a multiple of 16 frames
     */
    size = (in->pcm_config->period_size * in_get_sample_rate(stream)) /
            in->pcm_config->rate;
    size = ((size + 15) / 16) * 16;

    return size * audio_stream_in_frame_size(&in->stream);
}

static audio_channel_mask_t in_get_channels(const struct audio_stream *stream)
{
    return AUDIO_CHANNEL_IN_MONO;
}

static audio_format_t in_get_format(const struct audio_stream *stream)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int in_set_format(struct audio_stream *stream, audio_format_t format)
{
    return -ENOSYS;
}

static int in_standby(struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    ALOGD("in_standby()");

    in->sleep_req = true;
    in_lock(in);
    in->sleep_req = false;
    adev_lock(in->dev);
    do_in_standby(in);
    adev_unlock(in->dev);
    in_unlock(in);

    ALOGD("in_standby() done");

    return 0;
}

static int in_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    struct str_parms *parms;
    char value[32];
    int ret;
    unsigned int val;

    ALOGD("in_set_parameters()");

    parms = str_parms_create_str(kvpairs);

    in->sleep_req = true;
    in_lock(in);
    in->sleep_req = false;
    adev_lock(adev);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_INPUT_SOURCE,
                            value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value);
        if (adev->in_source != val) {
            adev->in_source = val;

            struct mixer* mixer;
            mixer = open_mixer();
            select_input_source(adev, mixer);
            close_mixer(mixer);
        }
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING,
                            value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value) & ~AUDIO_DEVICE_BIT_IN;
        if ((adev->in_device != val) && (val != 0)) {

            if (adev->mode != AUDIO_MODE_IN_CALL) {
                if (!in->standby)
                    do_in_standby(in);
            }

            adev->in_device = val;
            // select_devices(adev);
        }
    }
    adev_unlock(adev);
    in_unlock(in);

    ALOGD("in_set_parameters() done");

    str_parms_destroy(parms);
    return ret;
}

static char * in_get_parameters(const struct audio_stream *stream,
                                const char *keys)
{
    return strdup("");
}

static int in_set_gain(struct audio_stream_in *stream, float gain)
{
    return 0;
}

static ssize_t in_read(struct audio_stream_in *stream, void* buffer,
                       size_t bytes)
{
    int ret = 0;
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    size_t frames_rq = bytes / audio_stream_in_frame_size(stream);

    bool out_locked = false;

    if (in->sleep_req) {
        // 10ms are always shorter than the time to reconfigure the audio path
        // which is the only condition when sleep_req would be true.
        ALOGD("in->sleep_req: sleeping");
        usleep(10000);

        if (adev->active_in != in) {
            ALOGE("in_read() active in changed. abandoning this session.");
        //     // out = adev->active_out;
        //     return -EPIPE;
        }
    }

    /*
     * acquiring hw device mutex systematically is useful if a low
     * priority thread is waiting on the input stream mutex - e.g.
     * executing in_set_parameters() while holding the hw device
     * mutex
     */
    in_lock(in);
    if (in->standby) {
        ALOGD("in_read() pcm capture is exiting standby.");
        adev_lock(adev);

        struct stream_out* out = adev->active_out;
        while (out && !out->standby) {
            ALOGD("in_read() Warning: active_out is present.");

            // undo locks so we can lock the output in proper order
            adev_unlock(adev);
            in_unlock(in);

            ALOGD("in_read(): initial release locks.");
            // lock output for standby
            out->sleep_req = true;
            out_lock(out);
            // out->sleep_req = false;
            in_lock(in);
            adev_lock(adev);
            ALOGD("in_read(): locks taken.");

            if (out == adev->active_out) {
                out_locked = true;
                break;
            }

            ALOGD("in_read(): release out lock again.");
            out_unlock(out);
            out = adev->active_out;
            ALOGD("in_read(): release out locks again done.");
        }

        if (out && !out->standby) {
            ALOGD("in_read(): output go into standby.");
            do_out_standby(out);

            ALOGD("in_read(): output starting stream.");
            ret = start_output_stream(out);
            if (ret != 0)
                ALOGE("in_read(): Error restarting output stream.");
            out->standby = false;

            // ALOGD("in_read(): output go into standby again.");
            // do_out_standby(out);

            if (out_locked) {
                out_unlock(out);
                out->sleep_req = false;
                ALOGD("in_read(): release output lock.");
            }
            ALOGD("in_read(): restart output done. standby %d.", out->standby);
        }

        ALOGD("in_read(): starting input stream.");
        ret = start_input_stream(in);
        if (ret == 0)
            in->standby = false;

        /*
         * mixer must be set when coming out of standby
         */
        // audio_route_reset(adev->ar);
        struct mixer* mixer;
        mixer = open_mixer();
        select_devices(adev, mixer);
        select_input_source(adev, mixer);
        close_mixer(mixer);
        // audio_route_update_mixer(adev->ar);

        adev_unlock(adev);
        ALOGD("in_read() pcm capture is exiting standby. done.");
    }

    if (ret < 0)
        goto exit;

    /*if (in->num_preprocessors != 0) {
        ret = process_frames(in, buffer, frames_rq);
    } else */if (in->resampler != NULL) {
        ret = read_frames(in, buffer, frames_rq);
    } else if (in->pcm_config->channels == 2) {
        /*
         * If the PCM is stereo, capture twice as many frames and
         * discard the right channel.
         */
        unsigned int i;
        int16_t *in_buffer = (int16_t *)buffer;

        ret = pcm_read(in->pcm, in->buffer, bytes * 2);

        /* Discard right channel */
        for (i = 0; i < frames_rq; i++)
            in_buffer[i] = in->buffer[i * 2];
    } else {
        ret = pcm_read(in->pcm, buffer, bytes);
    }

    if (ret > 0)
        ret = 0;

    /*
     * Instead of writing zeroes here, we could trust the hardware
     * to always provide zeroes when muted.
     */
    if (ret == 0 && adev->mic_mute)
        memset(buffer, 0, bytes);

exit:
    if (ret < 0)
        usleep(bytes * 1000000 / audio_stream_in_frame_size(stream) /
               in_get_sample_rate(&stream->common));

    // adev_unlock(adev);
    in_unlock(in);

    ALOGV("in_read() done");

    return bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream)
{
    return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int adev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out,
                                   const char *address __unused)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_out *out;
    int ret;

    ALOGD("adev_open_output_stream()");

    out = (struct stream_out *)calloc(1, sizeof(struct stream_out));
    if (!out)
        return -ENOMEM;

    if (config->channel_mask != AUDIO_CHANNEL_OUT_STEREO || config->sample_rate != 44100) {
        config->channel_mask = AUDIO_CHANNEL_OUT_STEREO;
        config->sample_rate = 44100;
        ALOGE("adev_open_output_stream(): Error invalid channel mask. Requesting stereo output.");
        return -EINVAL;
    }

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;
    out->stream.get_presentation_position = out_get_presentation_position;

    out->dev = adev;

    config->format = out_get_format(&out->stream.common);
    config->channel_mask = out_get_channels(&out->stream.common);
    config->sample_rate = out_get_sample_rate(&out->stream.common);

    out->standby = true;
    /* out->written = 0; by calloc() */

    *stream_out = &out->stream;

    // adev_lock(adev);
    // select_devices(adev);
    // // start_output_stream(*out);
    // adev_unlock(adev);

    ALOGD("adev_open_output_stream: done");

    return 0;

err_open:
    free(out);
    *stream_out = NULL;
    return ret;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    ALOGD("adev_close_output_stream()");

    out_standby(&stream->common);
    free(stream);
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct str_parms *parms;
    char *str;
    char value[32];
    int ret;

    ALOGD("adev_set_parameters()");

    parms = str_parms_create_str(kvpairs);

    ret = str_parms_get_str(parms, "screen_state", value, sizeof(value));
    if (ret >= 0) {
        if (strcmp(value, AUDIO_PARAMETER_VALUE_ON) == 0)
            adev->screen_off = false;
        else
            adev->screen_off = true;
    }

    str_parms_destroy(parms);
    return ret;
}

static char * adev_get_parameters(const struct audio_hw_device *dev,
                                  const char *keys)
{
    return NULL;
}

static int adev_init_check(const struct audio_hw_device *dev)
{
    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

#ifndef ICS_AUDIO_BLOB
static int adev_get_master_volume(struct audio_hw_device *dev, float *volume)
{
    return -ENOSYS;
}

static int adev_set_master_mute(struct audio_hw_device *dev, bool muted)
{
    return -ENOSYS;
}

static int adev_get_master_mute(struct audio_hw_device *dev, bool *muted)
{
    return -ENOSYS;
}

#endif

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    ALOGD("adev_set_mode()");

    struct audio_device *adev = (struct audio_device *)dev;
    // struct stream_out *out = adev->active_out;
    // struct stream_in *in = adev->active_in;


    // out->sleep_req = true;
    // out_lock(out);
    // out->sleep_req = false;
    // in->sleep_req = true;
    // in_lock(in);
    // in->sleep_req = false;
    adev_lock(adev);

    audio_mode_t prev_mode = adev->mode;
    adev->mode = mode;
    ALOGD("adev_set_mode() : new %d, old %d", mode, prev_mode);

    adev_unlock(adev);
    // in_unlock(in);
    // out_unlock(out);

    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_in *in = adev->active_in;

    ALOGV("adev_set_mic_mute(%d) adev->mic_mute %d", state, adev->mic_mute);

    if (in != NULL) {
        in->sleep_req = true;
        in_lock(in);
        in->sleep_req = false;
        adev_lock(adev);

        // in call mute is handled by RIL
        if (adev->mode != AUDIO_MODE_IN_CALL) {
            do_in_standby(in);
        }

        adev_unlock(adev);
        in_unlock(in);
    }

    adev->mic_mute = state;

    return 0;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    struct audio_device *adev = (struct audio_device *)dev;

    *state = adev->mic_mute;

    return 0;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
                                         const struct audio_config *config)
{
    size_t size;

    /*
     * take resampling into account and return the closest majoring
     * multiple of 16 frames, as audioflinger expects audio buffers to
     * be a multiple of 16 frames
     */
    size = (pcm_config_in.period_size * config->sample_rate) / pcm_config_in.rate;
    size = ((size + 15) / 16) * 16;

    return (size * audio_channel_count_from_in_mask(config->channel_mask) *
                audio_bytes_per_sample(config->format));
}

static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in,
                                  audio_input_flags_t flags __unused,
                                  const char *address __unused,
                                  audio_source_t source __unused)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_in *in;
    int ret;

    ALOGD("adev_open_input_stream()");

    *stream_in = NULL;

    /* Respond with a request for mono if a different format is given. */
    if (config->channel_mask != AUDIO_CHANNEL_IN_MONO) {
        config->channel_mask = AUDIO_CHANNEL_IN_MONO;
        ALOGE("adev_open_input_stream(): Error invalid channel mask. Requesting mono input.");
        return -EINVAL;
    }

    in = (struct stream_in *)calloc(1, sizeof(struct stream_in));
    if (!in) {
        ALOGE("adev_open_input_stream(): Error creating ENOMEM");
        return -ENOMEM;
    }

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;

    in->dev = adev;
    in->standby = true;
    in->requested_rate = config->sample_rate;
    /* default PCM config */
    in->pcm_config = (config->sample_rate == IN_SAMPLING_RATE) && (flags & AUDIO_INPUT_FLAG_FAST) ?
            &pcm_config_in_low_latency : &pcm_config_in;

    ALOGD("adev_open_input_stream() done");

    *stream_in = &in->stream;
    return 0;
}

static void adev_close_input_stream(struct audio_hw_device *dev,
                                   struct audio_stream_in *stream)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_in *in = (struct stream_in *)stream;

    ALOGD("adev_close_input_stream()");

    in_standby(&stream->common);

    adev_lock(adev);
    free(stream);
    ALOGD("adev_close_input_stream() done %x", adev->active_in);
    adev->active_in = NULL;
    adev_unlock(adev);
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    ALOGD("adev_dump()");
    return 0;
}

static int adev_close(hw_device_t *device)
{
    struct audio_device *adev = (struct audio_device *)device;

    ALOGD("adev_close()");

    // audio_route_free(adev->ar);
    // close_mixer(adev->mixer);

    free(device);
    return 0;
}

static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    struct audio_device *adev;
    int ret;

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return -EINVAL;

    adev = calloc(1, sizeof(struct audio_device));
    if (!adev)
        return -ENOMEM;

    adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->hw_device.common.module = (struct hw_module_t *) module;
    adev->hw_device.common.close = adev_close;

    adev->hw_device.init_check = adev_init_check;
    adev->hw_device.set_voice_volume = adev_set_voice_volume;
    adev->hw_device.set_master_volume = adev_set_master_volume;
    adev->hw_device.set_mode = adev_set_mode;
    adev->hw_device.set_mic_mute = adev_set_mic_mute;
    adev->hw_device.get_mic_mute = adev_get_mic_mute;
    adev->hw_device.set_parameters = adev_set_parameters;
    adev->hw_device.get_parameters = adev_get_parameters;
    adev->hw_device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->hw_device.open_output_stream = adev_open_output_stream;
    adev->hw_device.close_output_stream = adev_close_output_stream;
    adev->hw_device.open_input_stream = adev_open_input_stream;
    adev->hw_device.close_input_stream = adev_close_input_stream;
    adev->hw_device.dump = adev_dump;

    // adev->ar = audio_route_init(MIXER_CARD, NULL);
    // adev->mixer = open_mixer();
    adev->out_device = AUDIO_DEVICE_NONE;
    // adev->in_device = AUDIO_DEVICE_IN_BUILTIN_MIC & ~AUDIO_DEVICE_BIT_IN;
    adev->in_device = AUDIO_DEVICE_NONE;
    adev->in_source = AUDIO_DEVICE_NONE;
    adev->mode = AUDIO_MODE_NORMAL;

    *device = &adev->hw_device.common;

    ALOGD("adev_open: done");

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "GT-P75xx audio HW HAL",
        .author = "The Android Open Source Project",
        .methods = &hal_module_methods,
    },
};