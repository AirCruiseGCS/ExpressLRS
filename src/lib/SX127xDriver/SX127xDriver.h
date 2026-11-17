#pragma once
#include "targets.h"
#if defined(RADIO_SX1262)
// An SX1262 standing in for the SX127x (twatch-elrs): same 900 MHz LoRa tables, FHSS grid and API, other chip.
#include "SX1262Driver.h"
using SX127xDriver = SX1262Driver;
#else
#include "SX127xHal.h"
#include "SX127x.h"
#endif
