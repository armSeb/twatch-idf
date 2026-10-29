#pragma once

#include <stdint.h>

#define NUM_ELEM(x) (sizeof(x) / sizeof(*(x)))

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IrCode {
    uint32_t carrier_freq;
    uint8_t num_pairs;
    const uint32_t *pairs;
    const uint8_t *sequence;
} IrCode;

extern const struct IrCode * const NApowerCodes[];
extern const struct IrCode * const EUpowerCodes[];
extern const uint8_t num_NAcodes;
extern const uint8_t num_EUcodes;

#ifdef __cplusplus
}
#endif
