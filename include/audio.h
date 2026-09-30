/* Music, sounds and ambiences: the .ogg files of assets/sound, decoded
   with stb_vorbis and mixed on a thread of their own (waveOut, 44.1 kHz
   stereo). Three independent parts:
     - the MUSIC: one playlist at a time (each track looped or not);
       a new playlist replaces the old one right away;
     - SOUNDS: any number at once, each played 1 + `repeat` times;
     - AMBIENCES: any number at once, looped or not.
   Every sound / ambience gets a voice id to stop it or ask if it's still
   playing. File names are relative to the folder given to audio_init
   (assets/sound), or, with a '/', to its parent (e.g. a room's own
   sounds: "levels/verdante/dockside/albfine.ogg"). */
#ifndef SILVER_AUDIO_H
#define SILVER_AUDIO_H

#define AUDIO_MAX_TRACKS 8
enum { AUDIO_MUSIC = 0, AUDIO_SOUND = 1, AUDIO_AMBIENCE = 2, AUDIO_PREVIEW = 3 };

typedef struct { char file[64]; int loop; } AudioTrack;
typedef struct { int id, kind; char file[64]; } AudioVoiceInfo;

int audio_init(const char *sound_dir); /* 1 = sound output running */
void audio_shutdown(void);

/* the music: n = 0 stops it. If the track playing is the new playlist's
   first one, it goes on (no restart); else the new playlist starts. */
void audio_music_play(const AudioTrack *tracks, int n);
void audio_music_stop(void);
/* same, the music playing fading out over `fade` seconds first (the new
   playlist starts once it's silent; 0 = at once) */
void audio_music_play_fade(const AudioTrack *tracks, int n, float fade);
/* file of the track playing ("" if none), its index in the playlist */
void audio_music_now(char *file, int n, int *track);

/* a sound (kind AUDIO_SOUND, played 1 + repeat times) or an ambience
   (AUDIO_AMBIENCE, loop = forever): returns its voice id, 0 if the file
   can't be played */
int audio_play(const char *file, int kind, int repeat, int loop);
/* same, from `offset` seconds into the file (its repeats start from the beginning) */
int audio_play_at(const char *file, int kind, int repeat, int loop, float offset);
void audio_stop(int id);
void audio_fade(int id, float seconds);  /* that voice fades out, then stops (0 = at once) */
void audio_stop_kind(int kind);      /* every voice of that kind */
void audio_fade_kind(int kind, float seconds); /* every voice of that kind fades out, then stops */
int audio_playing(int id);           /* 1 while the voice is still playing */
int audio_voices(AudioVoiceInfo *out, int max); /* sounds + ambiences playing */

/* editor "Listen": one preview voice, a new preview replaces it;
   audio_preview(NULL) stops it */
void audio_preview(const char *file);
int audio_preview_playing(void);

/* length of a file in seconds (-1 unreadable) */
float audio_file_seconds(const char *file);

#endif
