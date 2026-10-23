/* Link-time stand-ins for everything sequencer.c, EuklidGenerator.c and
 * automationNode.c use from the rest of the firmware. The calls that make sound or
 * leave the box (voice triggers, parameter changes from automation, MIDI out) are
 * printed as events, with the systick time, for test/seqref.mjs to compare. */
#include <stdio.h>
#include <stdint.h>
#include "stm32f7xx_hal.h"
#include "Parameters.h"
#include "Soundengine/MIDI/MidiMessages.h"
#include "Soundengine/FxBus.h"
#include "Preset/PresetManager.h"
#include "menu/menu.h"

volatile uint32_t systick_ticks;

uint8_t activeParameters[NUM_PARAMS];
uint8_t frontParser_activeTrack;
uint8_t frontParser_shownPattern;
FxBus fxBus;
uint8_t menu_activeBar;
uint8_t menu_activePage = 0xff;
uint8_t menu_loadedPresetNr[NUM_CURRENT];
uint8_t menu_selectedPresetNr[NUM_PRESET_LOCATIONS];
uint8_t menu_playedPattern;
uint8_t menu_shownPattern;
uint8_t menu_songMenuPosition;
uint8_t midiParser_originalCcValues[0xff];
uint8_t midiParser_txRxFilter = 0xff; /* send everything */
uint8_t midi_MidiChannels[8];
uint8_t midi_process_block;
uint32_t sync_delayedStepTimer;
uint8_t trigger_clockReceived;

/* the hardware RNG, replaced by the LCG the port uses */
static uint32_t rng_state = 0x2545F491u;
uint32_t GetRngValue(void)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return (rng_state >> 16) | (rng_state << 16);
}

/* MidiVoiceControl.c, with the voice trigger printed instead of played */
static uint8_t active_voices;
void voiceControl_noteOn(uint8_t voice, uint8_t note, uint8_t vel)
{
	extern void seq_sendMidiNoteOn(const uint8_t channel, const uint8_t note, const uint8_t veloc);
	active_voices |= (uint8_t)(1 << voice);
	printf("N %u %u %u %u\n", systick_ticks, voice, note, vel);
	seq_sendMidiNoteOn(midi_MidiChannels[voice], note, vel);
}

void voiceControl_noteOff(uint8_t voice)
{
	extern void seq_midiNoteOff(uint8_t chan);
	printf("F %u %u\n", systick_ticks, voice);
	if (voice == 0xff) {
		active_voices = 0;
		seq_midiNoteOff(0xff);
		return;
	}
	if (active_voices & (1 << voice)) {
		active_voices &= (uint8_t)~(1 << voice);
		seq_midiNoteOff(midi_MidiChannels[voice]);
	}
}

void midiParser_ccHandler(MidiMsg msg, uint8_t updateOriginalValue, uint8_t isFloatModulator, float v)
{
	(void)isFloatModulator; (void)v;
	printf("C %u %u %u %u %u\n", systick_ticks, msg.status, msg.data1, msg.data2, updateOriginalValue);
}

void usb_sendMidi(MidiMsg msg)
{
	printf("M %u %u %u %u %u\n", systick_ticks, msg.status, msg.data1, msg.data2, msg.bits.length);
}
void uart_sendMidi(MidiMsg msg) { (void)msg; }

uint8_t buttonHandler_getMode(void) { return 0; }
uint8_t led_idx2StepLed(uint8_t idx) { return idx; }
void led_blinkHandler(void) {}
void led_clearActive_step(void) {}
void led_clearAllBlinkLeds(void) {}
void led_clearSequencerLeds(void) {}
void led_initPatternSelectLeds(void) {}
void led_resetBlinkTimer(void) {}
void led_setActive_step(uint8_t s) { (void)s; }
void led_setBlinkLed(const uint8_t l, const uint8_t o) { (void)l; (void)o; }
void led_setValue(uint8_t v, uint8_t l) { (void)v; (void)l; }
void led_updateSeqLeds(void) {}
void lfo_recalcSync(void) {}
uint8_t menu_getActiveVoice(void) { return frontParser_activeTrack; }
uint8_t menu_getViewedPattern(void) { return frontParser_shownPattern; }
void menu_repaint(void) {}
void menu_repaintAll(void) {}
void menu_setShownPattern(uint8_t p) { (void)p; }
void midiParser_checkMtc(void) {}
void preset_loadDrumsetRam(uint8_t a, uint8_t b, uint8_t c) { (void)a; (void)b; (void)c; }
void preset_touchTimer(PresetTimer t) { (void)t; }
void sync_tick(void) {}
void trigger_clockTick(uint8_t a, uint8_t b) { (void)a; (void)b; }
void trigger_reset(uint8_t v) { (void)v; }
void uart_sendFrontpanelSysExByte(uint8_t d) { (void)d; }
