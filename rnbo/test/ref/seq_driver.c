/* seq_ref: runs the firmware's sequencer (sequencer.c, EuklidGenerator.c,
 * automationNode.c) from a command script and prints what it does.
 *
 * usage: seq_ref <script.txt>
 *
 * Time is counted in systicks (the firmware's 0.25 ms timer). The firmware calls
 * seq_tick() from a ~36 kHz timer interrupt, i.e. about CALLS_PER_SYSTICK times per
 * systick; "ticks" does the same.
 *
 * Script commands (one per line, '#' starts a comment):
 *   bpm <n>                          seq_setBpm
 *   shuffle <0-127>                  as the front panel's shuffle knob
 *   patshuffle <pat> <0-127>         a pattern's stored shuffle
 *   step <pat> <trk> <step> <volume> <prob> <note> <flam> <shift> <p1nr> <p1val> <p2nr> <p2val>
 *   main <pat> <trk> <step> <0|1>    main step on/off
 *   len <pat> <trk> <length>
 *   run <0|1>                        start / stop (FRONT_SEQ_RUN_STOP)
 *   mute <trk> <0|1>
 *   next <pat>                       seq_setNextPattern
 *   song <p0> <p1> ...               song list (the rest is SONG_END)
 *   songmode <0|1>
 *   rollrate <0-5>  /  roll <voice> <0|1>
 *   rec <0|1>  /  erase <0|1>
 *   track <trk>  /  shown <pat>      front panel's active track and shown pattern
 *   note <trk> <vel> <note>          a played note (voice trigger + seq_addNote)
 *   autotrack <0|1>                  active automation lane
 *   arm <step> <trk> <0|1>           step held for parameter locks
 *   autorec <dest> <value>           a knob moved: seq_recordAutomation(active track, ...)
 *   euklid <trk> <length> <steps> <rotation> <pat>
 *   cleartrack <trk> <pat> / clearpattern <pat> / clearautom <trk> <pat> <lane>
 *   copytrack <src> <dst> <pat> / copypattern <src> <dst>
 *   tracklen <trk> <length>          seq_setTrackLength (shown pattern)
 *   midichan <trk 0-7> <0-15>
 *   ticks <n>                        advance n systicks
 *   dump <pat> <trk>                 print a track's data
 *   state                            print the playback state
 *
 * Events: N tick voice note vel | F tick voice | C tick status d1 d2 orig
 *         M tick status d1 d2 len
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Soundengine/Sequencer/sequencer.h"
#include "Soundengine/Sequencer/EuklidGenerator.h"

#define CALLS_PER_SYSTICK 9

extern volatile uint32_t systick_ticks;
extern uint8_t frontParser_activeTrack;
extern uint8_t frontParser_shownPattern;
extern uint8_t midi_MidiChannels[8];
extern uint8_t seq_masterStepCnt;

static void dump(int p, int t)
{
	printf("D %d %d len %u main", p, t, seq_patternSet.length[p][t]);
	for (int b = 0; b < NUM_BARS; b++)
		printf(" %u", seq_patternSet.seq_mainSteps[b][p][t]);
	printf(" steps");
	for (int s = 0; s < NUM_STEPS; s++) {
		Step *st = &seq_patternSet.seq_subStepPattern[p][t][s];
		StepTiming *tm = &seq_patternSet.seq_stepTiming[p][t][s];
		printf(" %u,%u,%u,%u,%u,%u,%u,%u,%u", st->volume, st->prob, st->note, tm->flam, tm->shift,
		       st->param1Nr, st->param1Val, st->param2Nr, st->param2Val);
	}
	printf("\n");
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: seq_ref <script>\n");
		return 2;
	}
	FILE *f = fopen(argv[1], "r");
	if (!f) {
		perror(argv[1]);
		return 1;
	}
	seq_init();
	euklid_init();
	seq_setBpm(120);
	for (int i = 0; i < 8; i++)
		midi_MidiChannels[i] = (uint8_t)i;

	char line[1024];
	while (fgets(line, sizeof line, f)) {
		char *hash = strchr(line, '#');
		if (hash) *hash = 0;
		char cmd[32];
		int a[16] = {0};
		int n = sscanf(line, "%31s %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d", cmd,
		               &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7], &a[8], &a[9], &a[10],
		               &a[11], &a[12], &a[13], &a[14], &a[15]);
		if (n < 1) continue;
		if (!strcmp(cmd, "bpm")) seq_setBpm((uint16_t)a[0]);
		else if (!strcmp(cmd, "shuffle")) seq_setShuffle(a[0] / 127.f);
		else if (!strcmp(cmd, "patshuffle")) seq_patternSet.shuffle[a[0]] = (uint8_t)a[1];
		else if (!strcmp(cmd, "step")) {
			Step *st = &seq_patternSet.seq_subStepPattern[a[0]][a[1]][a[2]];
			StepTiming *tm = &seq_patternSet.seq_stepTiming[a[0]][a[1]][a[2]];
			st->volume = (uint8_t)a[3]; st->prob = (uint8_t)a[4]; st->note = (uint8_t)a[5];
			tm->flam = (uint8_t)a[6]; tm->shift = (uint8_t)a[7];
			st->param1Nr = (uint8_t)a[8]; st->param1Val = (uint8_t)a[9];
			st->param2Nr = (uint8_t)a[10]; st->param2Val = (uint8_t)a[11];
		}
		else if (!strcmp(cmd, "main")) seq_setMainStep((uint8_t)a[0], (uint8_t)a[1], (uint8_t)a[2], (uint8_t)a[3]);
		else if (!strcmp(cmd, "len")) seq_patternSet.length[a[0]][a[1]] = (uint8_t)a[2];
		else if (!strcmp(cmd, "run")) {
			if (a[0] == 0) seq_resetPosition();
			seq_setRunning((uint8_t)a[0]);
		}
		else if (!strcmp(cmd, "mute")) seq_setMute((uint8_t)a[0], (uint8_t)a[1]);
		else if (!strcmp(cmd, "next")) seq_setNextPattern((uint8_t)a[0]);
		else if (!strcmp(cmd, "song")) {
			for (int i = 0; i < MAX_SONG_LENGTH; i++) seq_songData[0][i] = SONG_END;
			for (int i = 0; i < n - 1; i++) seq_songData[0][i] = (uint8_t)a[i];
		}
		else if (!strcmp(cmd, "songmode")) seq_setSongModeEnabled((uint8_t)a[0]);
		else if (!strcmp(cmd, "rollrate")) seq_setRollRate((uint8_t)a[0]);
		else if (!strcmp(cmd, "roll")) seq_setRoll((uint8_t)a[0], (uint8_t)a[1]);
		else if (!strcmp(cmd, "rec")) seq_setRecordingMode((uint8_t)a[0]);
		else if (!strcmp(cmd, "erase")) seq_setErasingMode((uint8_t)a[0]);
		else if (!strcmp(cmd, "track")) frontParser_activeTrack = (uint8_t)a[0];
		else if (!strcmp(cmd, "shown")) frontParser_shownPattern = (uint8_t)a[0];
		else if (!strcmp(cmd, "note")) {
			extern void voiceControl_noteOn(uint8_t voice, uint8_t note, uint8_t vel);
			voiceControl_noteOn((uint8_t)a[0], (uint8_t)a[2], (uint8_t)a[1]);
			seq_addNote((uint8_t)a[0], (uint8_t)a[1], (uint8_t)a[2]);
		}
		else if (!strcmp(cmd, "autotrack")) seq_setActiveAutomationTrack((uint8_t)a[0]);
		else if (!strcmp(cmd, "arm")) seq_armAutomationStep((uint8_t)a[0], (uint8_t)a[1], (uint8_t)a[2]);
		else if (!strcmp(cmd, "autorec")) seq_recordAutomation(frontParser_activeTrack, (uint8_t)a[0], (uint8_t)a[1]);
		else if (!strcmp(cmd, "euklid")) {
			euklid_setLength((uint8_t)a[0], (uint8_t)a[1]);
			euklid_setRotation((uint8_t)a[0], (uint8_t)a[3], (uint8_t)a[4]);
			euklid_setSteps((uint8_t)a[0], (uint8_t)a[2], (uint8_t)a[4]);
		}
		else if (!strcmp(cmd, "cleartrack")) seq_clearTrack((uint8_t)a[0], (uint8_t)a[1]);
		else if (!strcmp(cmd, "clearpattern")) seq_clearPattern((uint8_t)a[0]);
		else if (!strcmp(cmd, "clearautom")) seq_clearAutomation((uint8_t)a[0], (uint8_t)a[1], (uint8_t)a[2]);
		else if (!strcmp(cmd, "copytrack")) seq_copyTrack((uint8_t)a[0], (uint8_t)a[1], (uint8_t)a[2]);
		else if (!strcmp(cmd, "copypattern")) seq_copyPattern((uint8_t)a[0], (uint8_t)a[1]);
		else if (!strcmp(cmd, "tracklen")) seq_setTrackLength((uint8_t)a[0], (uint8_t)a[1]);
		else if (!strcmp(cmd, "midichan")) midi_MidiChannels[a[0]] = (uint8_t)a[1];
		else if (!strcmp(cmd, "ticks")) {
			for (int i = 0; i < a[0]; i++) {
				systick_ticks++;
				for (int k = 0; k < CALLS_PER_SYSTICK; k++) seq_tick();
			}
		}
		else if (!strcmp(cmd, "dump")) dump(a[0], a[1]);
		else if (!strcmp(cmd, "state")) {
			printf("S %u pat %u %u cnt %u master %u song %u steps", systick_ticks, seq_activePattern,
			       seq_pendingPattern, seq_prescaleCounter, seq_masterStepCnt, seq_songPositionPointer);
			for (int t = 0; t < NUM_TRACKS; t++) printf(" %d", (int8_t)seq_getStepIndex((uint8_t)t));
			printf("\n");
		}
		else {
			fprintf(stderr, "unknown command: %s\n", cmd);
			return 1;
		}
	}
	return 0;
}
