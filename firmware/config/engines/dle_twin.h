/**
 * @file dle_twin.h
 *
 * DLE two-stroke boxer twins (RC/UAV gasoline engines) converted to fuel injection.
 */

#pragma once

// set engine_type 106
void setDle60Twin();

// set engine_type 107
void setDle120Twin();

#if defined(HW_HELLEN_UAEFI) || EFI_UNIT_TEST
// uaEFI's connector pins: the presets apply them on uaEFI builds, the unit tests call them directly
void setDleTwinUaefiPins();
void setDle60TwinUaefiPins();
#endif
