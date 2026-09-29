/* Music, sounds and ambiences (see audio.h): each voice decodes its .ogg
   as it plays (stb_vorbis, streamed from the file), resampled to the
   output rate with linear interpolation, mono files spread on both
   channels. The mixer thread keeps AUDIO_BUFFERS waveOut buffers queued
   and refills each one as soon as it has been played. */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "audio.h"
#define STB_VORBIS_HEADER_ONLY
#include "third_party/stb_vorbis.c"

#define OUT_RATE 44100
#define AUDIO_BUFFERS 4
#define AUDIO_FRAMES 1024      /* per buffer: ~23 ms, ~93 ms queued */
#define FB_CAP 2048            /* decoded frames kept per voice */
#define MAX_VOICES 32

static const float KIND_GAIN[4] = { 0.8f, 1.0f, 0.9f, 1.0f }; /* music, sound, ambience, preview */

typedef struct {
    int used, id, kind;
    char file[64];
    stb_vorbis *v;
    int ch;
    double step, rpos;         /* source frames per output frame, read position in fb */
    float fb[FB_CAP * 2];      /* decoded frames, stereo */
    int fb_n;
    int repeat_left, loop;
    float fade, fade_step;     /* volume factor; fading out: - fade_step per frame, freed at 0 */
} Voice;

static Voice g_voice[MAX_VOICES];
static CRITICAL_SECTION g_cs;
static char g_dir[MAX_PATH];
static HANDLE g_thread = NULL, g_event = NULL;
static volatile LONG g_quit = 0;
static int g_running = 0;
static int g_next_id = 1;
/* the music playlist */
static AudioTrack g_tracks[AUDIO_MAX_TRACKS];
static int g_ntracks = 0, g_track = -1;

static stb_vorbis *open_file(const char *file, int *channels, unsigned *rate) {
    char path[MAX_PATH * 2];
    snprintf(path, sizeof(path), "%s\\%s", g_dir, file);
    int err = 0;
    stb_vorbis *v = stb_vorbis_open_filename(path, &err, NULL);
    if (!v) return NULL;
    stb_vorbis_info in = stb_vorbis_get_info(v);
    if (in.channels < 1 || in.sample_rate == 0) { stb_vorbis_close(v); return NULL; }
    *channels = in.channels; *rate = in.sample_rate;
    return v;
}

/* Fills a voice slot with an opened file (no decoding yet). */
static void voice_setup(Voice *vc, stb_vorbis *v, int ch, unsigned rate, const char *file, int kind, int repeat, int loop) {
    vc->v = v; vc->ch = ch; vc->step = (double)rate / OUT_RATE; vc->rpos = 0; vc->fb_n = 0;
    vc->kind = kind; vc->repeat_left = repeat; vc->loop = loop;
    vc->fade = 1.0f; vc->fade_step = 0.0f;
    snprintf(vc->file, sizeof(vc->file), "%s", file);
    vc->id = g_next_id++;
    vc->used = 1;
}

static void voice_free(Voice *vc) {
    if (vc->v) stb_vorbis_close(vc->v);
    vc->v = NULL; vc->used = 0; vc->id = 0;
}

/* Decodes the next frames after the last one (kept for interpolation).
   At the end of the file: back to its start if it loops / repeats.
   Returns 0 when the voice has nothing more to play. */
static int voice_refill(Voice *vc) {
    static float tmp[FB_CAP * 8];
    int keep = 0;
    if (vc->fb_n > 0) {
        vc->fb[0] = vc->fb[(vc->fb_n - 1) * 2]; vc->fb[1] = vc->fb[(vc->fb_n - 1) * 2 + 1];
        vc->rpos -= vc->fb_n - 1;
        keep = 1;
    }
    int ch = vc->ch > 8 ? 8 : vc->ch, want = FB_CAP - keep;
    int got = stb_vorbis_get_samples_float_interleaved(vc->v, ch, tmp, want * ch);
    if (got <= 0 && (vc->loop || vc->repeat_left > 0)) {
        if (!vc->loop) vc->repeat_left--;
        stb_vorbis_seek_start(vc->v);
        got = stb_vorbis_get_samples_float_interleaved(vc->v, ch, tmp, want * ch);
    }
    if (got <= 0) { vc->fb_n = keep; return 0; }
    for (int i = 0; i < got; i++) {
        float l = tmp[i * ch], r = ch > 1 ? tmp[i * ch + 1] : l;
        vc->fb[(keep + i) * 2] = l; vc->fb[(keep + i) * 2 + 1] = r;
    }
    vc->fb_n = keep + got;
    return 1;
}

/* The music voice ended: the next track of the playlist, if any. */
static void music_next(Voice *vc) {
    voice_free(vc);
    while (++g_track < g_ntracks) {
        int ch; unsigned rate;
        stb_vorbis *v = open_file(g_tracks[g_track].file, &ch, &rate);
        if (!v) continue; /* unreadable track: skipped */
        voice_setup(vc, v, ch, rate, g_tracks[g_track].file, AUDIO_MUSIC, 0, g_tracks[g_track].loop);
        return;
    }
    g_track = -1;
}

static void mix(short *out, int frames) {
    static float acc[AUDIO_FRAMES * 2];
    memset(acc, 0, sizeof(float) * 2 * frames);
    EnterCriticalSection(&g_cs);
    for (int k = 0; k < MAX_VOICES; k++) {
        Voice *vc = &g_voice[k];
        if (!vc->used) continue;
        float g = KIND_GAIN[vc->kind];
        for (int i = 0; i < frames; i++) {
            while (vc->used && vc->rpos + 1 >= vc->fb_n) {
                if (voice_refill(vc)) continue;
                if (vc->kind == AUDIO_MUSIC) music_next(vc); else voice_free(vc);
            }
            if (!vc->used) break;
            int i0 = (int)vc->rpos;
            float f = (float)(vc->rpos - i0);
            const float *a = &vc->fb[i0 * 2];
            float gf = g * vc->fade;
            acc[i * 2] += (a[0] + (a[2] - a[0]) * f) * gf;
            acc[i * 2 + 1] += (a[1] + (a[3] - a[1]) * f) * gf;
            vc->rpos += vc->step;
            if (vc->fade_step > 0.0f && (vc->fade -= vc->fade_step) <= 0.0f) { voice_free(vc); break; }
        }
    }
    LeaveCriticalSection(&g_cs);
    for (int i = 0; i < frames * 2; i++) {
        float x = acc[i];
        if (x > 1.0f) x = 1.0f;
        if (x < -1.0f) x = -1.0f;
        out[i] = (short)(x * 32767.0f);
    }
}

static DWORD WINAPI mixer_thread(LPVOID param) {
    HWAVEOUT wo = (HWAVEOUT)param;
    static short buf[AUDIO_BUFFERS][AUDIO_FRAMES * 2];
    WAVEHDR hdr[AUDIO_BUFFERS];
    memset(hdr, 0, sizeof(hdr));
    for (int b = 0; b < AUDIO_BUFFERS; b++) {
        hdr[b].lpData = (LPSTR)buf[b]; hdr[b].dwBufferLength = sizeof(buf[b]);
        waveOutPrepareHeader(wo, &hdr[b], sizeof(WAVEHDR));
        mix(buf[b], AUDIO_FRAMES);
        waveOutWrite(wo, &hdr[b], sizeof(WAVEHDR));
    }
    while (!g_quit) {
        WaitForSingleObject(g_event, 50);
        for (int b = 0; b < AUDIO_BUFFERS && !g_quit; b++) {
            if (!(hdr[b].dwFlags & WHDR_DONE)) continue;
            mix(buf[b], AUDIO_FRAMES);
            waveOutWrite(wo, &hdr[b], sizeof(WAVEHDR));
        }
    }
    waveOutReset(wo);
    for (int b = 0; b < AUDIO_BUFFERS; b++) waveOutUnprepareHeader(wo, &hdr[b], sizeof(WAVEHDR));
    waveOutClose(wo);
    return 0;
}

int audio_init(const char *sound_dir) {
    snprintf(g_dir, sizeof(g_dir), "%s", sound_dir);
    InitializeCriticalSection(&g_cs);
    g_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    WAVEFORMATEX fmt; memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM; fmt.nChannels = 2; fmt.nSamplesPerSec = OUT_RATE;
    fmt.wBitsPerSample = 16; fmt.nBlockAlign = 4; fmt.nAvgBytesPerSec = OUT_RATE * 4;
    HWAVEOUT wo;
    if (waveOutOpen(&wo, WAVE_MAPPER, &fmt, (DWORD_PTR)g_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) return 0;
    g_thread = CreateThread(NULL, 0, mixer_thread, (LPVOID)wo, 0, NULL);
    if (!g_thread) { waveOutClose(wo); return 0; }
    SetThreadPriority(g_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    g_running = 1;
    return 1;
}

void audio_shutdown(void) {
    if (!g_running) return;
    g_quit = 1;
    SetEvent(g_event);
    WaitForSingleObject(g_thread, 1000);
    for (int k = 0; k < MAX_VOICES; k++) if (g_voice[k].used) voice_free(&g_voice[k]);
    g_running = 0;
}

static Voice *free_slot(void) {
    for (int k = 0; k < MAX_VOICES; k++) if (!g_voice[k].used) return &g_voice[k];
    return NULL;
}

void audio_music_play(const AudioTrack *tracks, int n) {
    if (!g_running) return;
    if (n > AUDIO_MAX_TRACKS) n = AUDIO_MAX_TRACKS;
    EnterCriticalSection(&g_cs);
    for (int k = 0; k < MAX_VOICES; k++) if (g_voice[k].used && g_voice[k].kind == AUDIO_MUSIC) voice_free(&g_voice[k]);
    memcpy(g_tracks, tracks, sizeof(AudioTrack) * (n > 0 ? n : 0));
    g_ntracks = n > 0 ? n : 0;
    g_track = -1;
    Voice *vc = free_slot();
    if (vc && g_ntracks > 0) music_next(vc);
    LeaveCriticalSection(&g_cs);
}
void audio_music_stop(void) { audio_music_play(NULL, 0); }

void audio_music_now(char *file, int n, int *track) {
    if (file && n > 0) file[0] = 0;
    if (track) *track = -1;
    if (!g_running) return;
    EnterCriticalSection(&g_cs);
    if (file && n > 0) snprintf(file, n, "%s", g_track >= 0 ? g_tracks[g_track].file : "");
    if (track) *track = g_track;
    LeaveCriticalSection(&g_cs);
}

int audio_play(const char *file, int kind, int repeat, int loop) {
    if (!g_running || !file || !file[0]) return 0;
    int ch; unsigned rate;
    stb_vorbis *v = open_file(file, &ch, &rate);
    if (!v) return 0;
    EnterCriticalSection(&g_cs);
    Voice *vc = free_slot();
    int id = 0;
    if (vc) { voice_setup(vc, v, ch, rate, file, kind, repeat < 0 ? 0 : repeat, loop); id = vc->id; }
    LeaveCriticalSection(&g_cs);
    if (!vc) stb_vorbis_close(v);
    return id;
}

void audio_stop(int id) {
    if (!g_running || id <= 0) return;
    EnterCriticalSection(&g_cs);
    for (int k = 0; k < MAX_VOICES; k++) if (g_voice[k].used && g_voice[k].id == id) voice_free(&g_voice[k]);
    LeaveCriticalSection(&g_cs);
}

void audio_stop_kind(int kind) {
    if (!g_running) return;
    EnterCriticalSection(&g_cs);
    for (int k = 0; k < MAX_VOICES; k++) if (g_voice[k].used && g_voice[k].kind == kind) voice_free(&g_voice[k]);
    if (kind == AUDIO_MUSIC) { g_ntracks = 0; g_track = -1; }
    LeaveCriticalSection(&g_cs);
}

void audio_fade_kind(int kind, float seconds) {
    if (!g_running) return;
    if (seconds <= 0.0f) { audio_stop_kind(kind); return; }
    EnterCriticalSection(&g_cs);
    for (int k = 0; k < MAX_VOICES; k++)
        if (g_voice[k].used && g_voice[k].kind == kind) g_voice[k].fade_step = g_voice[k].fade / (seconds * OUT_RATE);
    LeaveCriticalSection(&g_cs);
}

int audio_playing(int id) {
    if (!g_running || id <= 0) return 0;
    int on = 0;
    EnterCriticalSection(&g_cs);
    for (int k = 0; k < MAX_VOICES; k++) if (g_voice[k].used && g_voice[k].id == id) on = 1;
    LeaveCriticalSection(&g_cs);
    return on;
}

int audio_voices(AudioVoiceInfo *out, int max) {
    int n = 0;
    if (!g_running) return 0;
    EnterCriticalSection(&g_cs);
    for (int k = 0; k < MAX_VOICES && n < max; k++) {
        Voice *vc = &g_voice[k];
        if (!vc->used || (vc->kind != AUDIO_SOUND && vc->kind != AUDIO_AMBIENCE)) continue;
        out[n].id = vc->id; out[n].kind = vc->kind;
        snprintf(out[n].file, sizeof(out[n].file), "%s", vc->file);
        n++;
    }
    LeaveCriticalSection(&g_cs);
    return n;
}

void audio_preview(const char *file) {
    audio_stop_kind(AUDIO_PREVIEW);
    if (file && file[0]) audio_play(file, AUDIO_PREVIEW, 0, 0);
}

int audio_preview_playing(void) {
    int on = 0;
    if (!g_running) return 0;
    EnterCriticalSection(&g_cs);
    for (int k = 0; k < MAX_VOICES; k++) if (g_voice[k].used && g_voice[k].kind == AUDIO_PREVIEW) on = 1;
    LeaveCriticalSection(&g_cs);
    return on;
}

float audio_file_seconds(const char *file) {
    int ch; unsigned rate;
    stb_vorbis *v = open_file(file, &ch, &rate);
    if (!v) return -1.0f;
    float s = stb_vorbis_stream_length_in_seconds(v);
    stb_vorbis_close(v);
    return s;
}
