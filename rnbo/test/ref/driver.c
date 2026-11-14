/* lxr_ref: renders the original LXR sound engine from a small command script.
 *
 * usage: lxr_ref <script.txt> <out.raw>
 *
 * Script commands, one per line ('#' starts a comment):
 *   bpm <n>                      tempo seen by synced LFOs
 *   param <PAR index> <value>    set a sound parameter the way the front panel does
 *   lfodest <lfo 0-5> <PAR>      LFO modulation destination (a PAR index, 0 = none)
 *   velodest <voice 0-5> <PAR>   velocity modulation destination
 *   trig <voice 0-6> <vel> <note>  0-2 drums, 3 snare, 4 cymbal, 5 closed hat, 6 open hat
 *   render <blocks>              run the engine for <blocks> * OUTPUT_DMA_SIZE samples
 *   dumpptr                      print the PAR indices that have a modulation pointer
 *   dumpall                      print one line of engine state (test/statediff.mjs)
 *
 * Output: little-endian int16, 4 interleaved channels (stereo out 1 L/R, stereo out 2 L/R).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Soundengine/Soundengine.h"
#include "Soundengine/MIDI/ParameterArray.h"
#include "Soundengine/MIDI/MidiMessages.h"
#include "Soundengine/FxBus.h"
#include "frontPanelParser_ui.h"

extern uint8_t activeParameters[];
extern uint16_t host_bpm;

/* The firmware's noise comes from the hardware RNG, shared by all voices. That makes
 * the noise of one voice depend on when every other noise source last wrapped, which
 * no port can reproduce sample for sample. For testing, each source gets its own LCG
 * (the RNBO engine does the same): oscillator slots 0-15 as numbered in the engine,
 * LFOs 16-21. */
static uint32_t rng_state[24];

static int rng_index(const void *p)
{
	const void *src[22] = {
		&voiceArray[0].osc, &voiceArray[0].modOsc, &voiceArray[1].osc, &voiceArray[1].modOsc,
		&voiceArray[2].osc, &voiceArray[2].modOsc, &snareVoice.osc, &snareVoice.noiseOsc,
		&cymbalVoice.osc, &cymbalVoice.modOsc, &cymbalVoice.modOsc2,
		&hatVoice.osc, &hatVoice.modOsc, &hatVoice.modOsc2, &fxBus.ringmod.osc, NULL,
		&voiceArray[0].lfo, &voiceArray[1].lfo, &voiceArray[2].lfo,
		&snareVoice.lfo, &cymbalVoice.lfo, &hatVoice.lfo,
	};
	for (int i = 0; i < 22; i++)
		if (src[i] == p)
			return i;
	return 23;
}

uint32_t host_rng(const void *source)
{
	uint32_t *s = &rng_state[rng_index(source)];
	*s = *s * 1664525u + 1013904223u;
	return (*s >> 16) | (*s << 16);
}

static void setParam(int idx, int val)
{
	MidiMsg msg = {0};
	activeParameters[idx] = (uint8_t)val;
	if (idx < 128) {
		msg.status = MIDI_CC;
		msg.data1 = (uint8_t)((idx + 1) & 0x7f);
	} else {
		msg.status = CC_2;
		msg.data1 = (uint8_t)((idx - 128) & 0x7f);
	}
	msg.data2 = (uint8_t)val;
	midiParser_ccHandler(msg, 1, 0, 0.f);
}

static void trigger(int voice, int vel, int note)
{
	if (voice < 3)
		Drum_trigger(voice, vel, note);
	else if (voice == 3)
		Snare_trigger(vel, note);
	else if (voice == 4)
		Cymbal_trigger(vel, note);
	else
		HiHat_trigger(vel, voice - 5, note);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s script.txt out.raw\n", argv[0]);
		return 2;
	}
	FILE *in = fopen(argv[1], "r");
	FILE *out = fopen(argv[2], "wb");
	if (!in || !out) {
		perror("open");
		return 1;
	}

	for (int i = 0; i < 24; i++)
		rng_state[i] = 0x2545F491u + (uint32_t)i * 0x9E3779B9u;

	/* the engine part of soundengine_init() */
	mixer_init();
	parameterArray_init();
	for (int i = 0; i < 6; i++)
		modNode_init(&velocityModulators[i]);
	initDrumVoice();
	Snare_init();
	HiHat_init();
	Cymbal_init();

	char line[256];
	while (fgets(line, sizeof line, in)) {
		char *hash = strchr(line, '#');
		if (hash)
			*hash = 0;
		char cmd[32];
		int a = 0, b = 0, c = 0;
		int n = sscanf(line, "%31s %d %d %d", cmd, &a, &b, &c);
		if (n < 1)
			continue;
		if (!strcmp(cmd, "bpm")) {
			host_bpm = (uint16_t)a;
			lfo_recalcSync();
		} else if (!strcmp(cmd, "param")) {
			setParam(a, b);
		} else if (!strcmp(cmd, "lfodest")) {
			Lfo *lfos[6] = { &voiceArray[0].lfo, &voiceArray[1].lfo, &voiceArray[2].lfo,
			                 &snareVoice.lfo, &cymbalVoice.lfo, &hatVoice.lfo };
			modNode_setDestination(&lfos[a]->modTarget, (uint16_t)b);
		} else if (!strcmp(cmd, "velodest")) {
			modNode_setDestination(&velocityModulators[a], (uint16_t)b);
		} else if (!strcmp(cmd, "trig")) {
			trigger(a, b, c);
		} else if (!strcmp(cmd, "render")) {
			int16_t o1[OUTPUT_DMA_SIZE * 2], o2[OUTPUT_DMA_SIZE * 2], frame[4];
			for (int blk = 0; blk < a; blk++) {
				mixer_calcNextSampleBlock(o1, o2);
				for (int i = 0; i < OUTPUT_DMA_SIZE; i++) {
					frame[0] = o1[2 * i];
					frame[1] = o1[2 * i + 1];
					frame[2] = o2[2 * i];
					frame[3] = o2[2 * i + 1];
					fwrite(frame, sizeof frame, 1, out);
				}
			}
		} else if (!strcmp(cmd, "dumpall")) {
			/* one line of numbers, matched field by field by the JS side (test/statediff.mjs) */
			OscInfo *osc[15] = { &voiceArray[0].osc, &voiceArray[0].modOsc, &voiceArray[1].osc, &voiceArray[1].modOsc,
				&voiceArray[2].osc, &voiceArray[2].modOsc, &snareVoice.osc, &snareVoice.noiseOsc,
				&cymbalVoice.osc, &cymbalVoice.modOsc, &cymbalVoice.modOsc2,
				&hatVoice.osc, &hatVoice.modOsc, &hatVoice.modOsc2, &fxBus.ringmod.osc };
			for (int i = 0; i < 15; i++)
				printf("o%d %u %u %u %u %.9g %.9g %.9g %d ", i, osc[i]->phase, osc[i]->phaseInc, osc[i]->waveform,
					osc[i]->tableOffset, osc[i]->freq, osc[i]->pitchMod, osc[i]->fmMod, osc[i]->output);
			SlopeEg2 *ae[6] = { &voiceArray[0].oscVolEg, &voiceArray[1].oscVolEg, &voiceArray[2].oscVolEg,
				&snareVoice.oscVolEg, &cymbalVoice.oscVolEg, &hatVoice.oscVolEg };
			ResonantFilter *fl[6] = { &voiceArray[0].filter, &voiceArray[1].filter, &voiceArray[2].filter,
				&snareVoice.filter, &cymbalVoice.filter, &hatVoice.filter };
			TransientGenerator *tg[6] = { &voiceArray[0].transGen, &voiceArray[1].transGen, &voiceArray[2].transGen,
				&snareVoice.transGen, &cymbalVoice.transGen, &hatVoice.transGen };
			Lfo *lf[6] = { &voiceArray[0].lfo, &voiceArray[1].lfo, &voiceArray[2].lfo, &snareVoice.lfo, &cymbalVoice.lfo, &hatVoice.lfo };
			uint8_t ft[6] = { voiceArray[0].filterType, voiceArray[1].filterType, voiceArray[2].filterType,
				snareVoice.filterType, cymbalVoice.filterType, hatVoice.filterType };
			for (int v = 0; v < 6; v++)
				printf("v%d ae %.17g %u %u %.17g %.17g f %.9g %.9g %.9g %.9g %.9g %u t %u %u %.9g %.9g l %u %u %u %d m %u %.9g %.9g ", v,
					ae[v]->value, ae[v]->state, ae[v]->repeatCnt, ae[v]->attack, ae[v]->decay,
					fl[v]->f, fl[v]->q, fl[v]->drive, fl[v]->s1, fl[v]->s2, ft[v],
					tg[v]->phase, tg[v]->waveform, tg[v]->volume, tg[v]->pitch,
					lf[v]->phase, lf[v]->phaseInc, lf[v]->waveform, lf[v]->retrigger,
					lf[v]->modTarget.destination, lf[v]->modTarget.amount, velocityModulators[v].amount);
			printf("fx %u %u %.9g %.9g %.9g %.9g %.9g\n", fxBus.fxType, fxBus.audioRouting, fxBus.dryWet,
				fxBus.parameter[0], fxBus.parameter[1], fxBus.parameter[2], fxBus.parameter[3]);
		} else if (!strcmp(cmd, "dumpptr")) {
			for (int i = 0; i < END_OF_SOUND_PARAMETERS; i++)
				if (parameterArray[i].ptr)
					printf("%d\n", i);
		} else {
			fprintf(stderr, "unknown command: %s\n", cmd);
			return 1;
		}
	}
	fclose(out);
	return 0;
}
