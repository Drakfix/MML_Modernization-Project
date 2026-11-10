#pragma once
/* English subtitles for the cutscenes, with either voices; see subtitles.c. Returns 1 if installed. */
int subtitles_install(const char *game_dir, int on);
/* a voice clip starts (bank file name such as "3700E.WBK", clip number): voice.c */
void subtitles_voice(const char *bank, unsigned clip);
