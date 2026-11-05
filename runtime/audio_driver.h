#pragma once
#include "lifted.h"

/* Return 0 for a different module. A recognized module's unknown code target
 * fails explicitly; it is never interpreted or treated as a successful call.
 */
int lhx_audio_dispatch(CPU *cpu, uint16_t segment, uint16_t offset);
