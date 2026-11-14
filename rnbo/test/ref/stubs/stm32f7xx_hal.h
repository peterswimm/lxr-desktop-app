/* Host stand-in for the ST HAL + CMSIS intrinsics used by the LXR DSP code.
 * Only what the sound engine needs to compile and run on a PC. */
#ifndef HOST_STM32F7XX_HAL_H
#define HOST_STM32F7XX_HAL_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

#define __IO volatile
#define __fp16 _Float16 /* firmware builds with -mfp16-format=ieee */
#define __STATIC_INLINE static inline
#define __INLINE inline
#define UNUSED(x) ((void)(x))
#define PROGMEM
#define pgm_read_word(a) (*(a))
#define pgm_read_byte(a) (*(a))
#define I2S_AUDIOFREQ_44K 44100U

typedef struct { volatile uint32_t IDR, ODR, BSRR; } GPIO_TypeDef;
typedef struct { volatile uint32_t FPDSCR; } FPU_Type;
typedef struct { int dummy; } RNG_HandleTypeDef;
typedef struct { int dummy; } TIM_HandleTypeDef;
typedef struct { int dummy; } UART_HandleTypeDef;
typedef struct { int dummy; } SPI_HandleTypeDef;
typedef enum { HAL_OK = 0, HAL_ERROR } HAL_StatusTypeDef;
typedef union { struct { uint32_t _r:28; uint32_t V:1; uint32_t C:1; uint32_t Z:1; uint32_t N:1; } b; uint32_t w; } APSR_Type;

static inline int16_t host_sat16(int32_t x) { return x > 32767 ? 32767 : (x < -32768 ? -32768 : (int16_t)x); }
static inline int32_t __SSAT(int32_t x, uint32_t bits) {
    const int32_t mx = (1 << (bits - 1)) - 1, mn = -(1 << (bits - 1));
    return x > mx ? mx : (x < mn ? mn : x);
}
static inline uint32_t __QADD16(uint32_t a, uint32_t b) {
    int32_t lo = host_sat16((int16_t)(a & 0xffff) + (int16_t)(b & 0xffff));
    int32_t hi = host_sat16((int16_t)(a >> 16) + (int16_t)(b >> 16));
    return ((uint32_t)(uint16_t)lo) | ((uint32_t)(uint16_t)hi << 16);
}
static inline uint32_t __QSUB16(uint32_t a, uint32_t b) {
    int32_t lo = host_sat16((int16_t)(a & 0xffff) - (int16_t)(b & 0xffff));
    int32_t hi = host_sat16((int16_t)(a >> 16) - (int16_t)(b >> 16));
    return ((uint32_t)(uint16_t)lo) | ((uint32_t)(uint16_t)hi << 16);
}
#define __PKHBT(a, b, s) ((((uint32_t)(a)) & 0x0000FFFFUL) | ((((uint32_t)(b)) << (s)) & 0xFFFF0000UL))
static inline uint8_t __CLZ(uint32_t v) { return v ? (uint8_t)__builtin_clz(v) : 32; }
static inline uint32_t __get_APSR(void) { return 0; }

/* Cortex-M7 VCVT.U32.F32/F64: round toward zero, saturating (negative -> 0).
 * x86 wraps instead, so the reference build routes float->uint32 casts through this. */
static inline uint32_t ARM_F2U(double x) {
    if (!(x > 0.0)) return 0;
    if (x >= 4294967295.0) return 0xFFFFFFFFu;
    return (uint32_t)x;
}

/* VCVT.S32.F32: truncate, saturate, NaN -> 0 */
static inline int32_t ARM_F2I(double x) {
    if (x != x) return 0;
    if (x >= 2147483647.0) return 2147483647;
    if (x <= -2147483648.0) return -2147483647 - 1;
    return (int32_t)x;
}

/* per noise source RNG stream, defined in driver.c */
uint32_t host_rng(const void *source);

extern FPU_Type host_fpu;
#define FPU (&host_fpu)
#define FPU_FPDSCR_FZ_Msk 0
extern RNG_HandleTypeDef hrng;
uint32_t HAL_RNG_GetRandomNumber(RNG_HandleTypeDef *h);
#endif
