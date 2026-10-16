#pragma once

// FirmwareBoardTag.cpp reads only the FREEINK_DEVICE_* build flags from the SDK's BoardConfig.h; the host build sets
// them on the command line.
#ifndef FREEINK_DEVICE_X4PRO
#define FREEINK_DEVICE_X4PRO 0
#endif
#ifndef FREEINK_DEVICE_X4CLASSIC
#define FREEINK_DEVICE_X4CLASSIC 0
#endif
