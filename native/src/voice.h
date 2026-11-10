#pragma once
/* Japanese voices from SPJ ([general] voices=japanese), and the clips the voices play for the
   subtitles (subtitles: 1 = tell subtitles.c); see voice.c. Returns 1 if the voice hook is in. */
int voice_install(const char *game_dir, int japanese, int subtitles, int log);   /* log: [debug] voice_log */
long voice_skipped(void);   /* empty entries play_voice ignored so far */
