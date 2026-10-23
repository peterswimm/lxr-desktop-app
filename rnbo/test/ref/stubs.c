/* Link-time stand-ins for the parts of the firmware outside the sound engine.
 * Only adc_getFaderVolume and seq_getBpm are actually reached
 * while rendering; the rest exist so MidiParser.c links. */
#include <stdint.h>
#include "stm32f7xx_hal.h"
#include "Parameters.h"

FPU_Type host_fpu;
GPIO_TypeDef host_gpio = { 0xFFFFFFFFu, 0, 0 }; /* all output jacks "plugged" */
RNG_HandleTypeDef hrng;
TIM_HandleTypeDef htim2;
volatile uint32_t systick_ticks;

uint8_t activeParameters[512];
uint8_t menu_activePage = 0xff; /* never the FX or sequencer page */
uint8_t frontParser_activeTrack;
uint8_t seq_patternSet[1 << 16];
uint16_t host_bpm = 120;

/* Noise sources call host_rng() (driver.c) instead; nothing else uses the RNG. */
uint32_t GetRngValue(void) { return host_rng(NULL); }
uint32_t HAL_RNG_GetRandomNumber(RNG_HandleTypeDef *h) { (void)h; return GetRngValue(); }

float adc_getFaderVolume(uint8_t idx) { (void)idx; return 1.f; }
uint16_t seq_getBpm(void) { return host_bpm; }

uint8_t menu_getActiveVoice(void) { return 0; }
uint8_t menu_getSubPage(void) { return 0; }
uint8_t menu_getViewedPattern(void) { return 0; }
void menu_repaint(void) {}
void menu_switchSubPage(uint8_t a, uint8_t b) { (void)a; (void)b; }
void seq_addNote(void) {}
uint8_t seq_getExtSync(void) { return 0; }
uint8_t seq_isRunning(void) { return 0; }
uint8_t seq_isTrackMuted(uint8_t t) { (void)t; return 0; }
void seq_midiNoteOff(void) {}
void seq_recordAutomation(void) {}
void seq_sendMidiNoteOn(void) {}
void seq_setMute(void) {}
void seq_setNextPattern(void) {}
void seq_setRunning(void) {}
void seq_sync(void) {}
void sync_midiResetPosition(void) {}
void sync_midiStartStop(void) {}
void uart_sendFrontpanelByte(void) {}
void uart_sendMidi(void) {}
void usb_sendMidi(void) {}
void voiceControl_noteOn(void) {}
