/**
 * file can_bench_test.h
 */

#pragma once
#include "can.h"

void processCanQcBenchTest(const CANRxFrame& frame);
// whether a CAN QC bench command (bench_test_io_control_e) may run now
bool canQcCommandAllowed(uint8_t command);
void sendQcBenchEventCounters(size_t bus = 0);
void sendQcBenchButtonCounters();
void sendQcBenchAuxDigitalCounters();
void sendQcBenchRawAnalogValues(size_t bus = 0);
void sendQcBenchBoardStatus(size_t bus = 0);
void initQcBenchControls();
