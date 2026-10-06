# Work Report

## 2026-07-14 - Investigation: "Malformed Packet: packet length" in USB.pcapng

What was done:
- Analyzed USB.pcapng (USBPcap capture of the rusEFI ECU USB link, device address 7)
  using tshark to explain the "malformed packet" warnings.

Findings:
- Protocol hierarchy: 84926 frames, mostly USB mass storage (usbms/scsi) plus 2 CDC
  (usbcom) control frames. 23 SCSI frames + 1 URB frame flagged _ws.malformed.
- The 23 usbms malformed frames are ALL the same case: the device reply to
  SCSI Mode Sense(6) (opcode 0x1a). Wireshark message: "SCSI: length of contained
  item exceeds length of containing item".
- Root cause is a Wireshark dissector strictness issue, NOT bad wire data. The 16-byte
  reply is self-consistent:
      0f 00 00 00  08 0a 00 00 00 00 00 00 00 00 00 00
  Mode Data Length=15 (=total-1), Caching mode page (0x08) with PageLength=0x0a=10.
  SBC-2 mandates the Caching page be 0x12=18 long (20-byte page); rusEFI emits a valid
  but non-standard SHORT caching page. Wireshark decodes the full 20-byte layout, reads
  past the 16-byte buffer, and raises the exception. Windows accepts it -> device works.
- Response is hardcoded in ChibiOS-Contrib USB-MSD SCSI target (os/hal/src/hal_usb_msd.c,
  submodule not checked out locally), driven by
  firmware/hw_layer/mass_storage/mass_storage_device.cpp.
- Frames 27759-27772 "USBPcap did not recognize URB Function code" are a USBPcap capture-
  driver limitation, unrelated to rusEFI traffic.

Decisions:
- Classified as cosmetic; no code change made. Not a functional defect.

Validation:
- tshark -z io,phs, per-frame -V dissection, and raw -x hex confirmed the byte layout
  and that all 23 malformed frames share the Mode Sense(6) cause.

Open follow-ups:
- Optional cleanup if the warning ever matters: set the Caching mode page length to 0x12
  and pad the page to the full 20 bytes, or drop the caching page from the Mode Sense
  reply. Lives upstream in ChibiOS-Contrib hal_usb_msd.c.

## 2026-07-14 - SD ECU<->PC switch soak sandbox + USB CDC link-drop investigation

What was done:
- Created a headless soak sandbox SdEcuPcCycleSandbox in the :ui test subproject
  (java_console/ui/src/test/java/com/rusefi/SdEcuPcCycleSandbox.java), modeled on the
  purple-gateway SdPcToEcuSwitchSandbox. It cycles the SD card ECU/logging <-> PC/MSD 10
  times, 20s dwell per mode, confirming each switch via the sd_present / sd_logging_internal
  / sd_msd output channels, and reports a pass/fail tally. Added Gradle task :ui:runSdCycle.
- Initialized two uninitialized git submodules required by the Java build:
  java_console/peak-can-basic (missing peak.can.basic.* -> :ecu_io compile fail) and
  java_console/luaformatter (missing neoe.formatter.lua -> :ui compile fail).

Result of the run (COM149, purple-gateway fw, USB-powered only / no +12V):
- Cycle 1 fully succeeded BOTH directions. PC/MSD->ECU no longer hits FR_DISK_ERR: firmware
  logged "SD: switched from PC/MSD to ECU/logging" and opened log file re_10.mlg; status bits
  confirmed sd_logging_internal=1.
- ~1.5s after the ECU switch the host CDC serial link dropped:
  "output channels: executeCommand failed: java.io.IOException: write failed: wrote 0 but
  expected 11", COM149 closed. Never recovered, so cycle 2's first command got no response and
  the soak aborted at 1/20. Sandbox behaved correctly - it detected and reported the drop.

Root cause (investigated, code-evidenced):
- CDC console and USB mass storage are interfaces on ONE composite USB device (USBD1). The
  config descriptor is fixed at 3 interfaces - MSD IF0 + CDC-control IF1 + CDC-data IF2, 98
  bytes (usbcfg.cpp DESCRIPTOR_SIZE/NUM_INTERFACES). MSD is always present in the enumerated
  descriptor whenever HAL_USE_USB_MSD is built in.
- The SD mode switch does NOT re-enumerate or reconfigure USB. attachMsdSdCard /
  deattachMsdSdCard (mass_storage_init.cpp) merely hot-swap LUN1's backing block device
  between the real SD card and the null device ND1 on the already-running MSD controller.
- Causal chain: PC/MSD->ECU calls deattachMsdSdCard() which swaps LUN1 (SD card -> ND1) while
  Windows still has that mass-storage volume mounted -> the medium vanishes under the mounted
  volume -> the Windows usbstor stack resets/re-enumerates the whole composite device to
  recover -> firmware gets USB_EVENT_RESET/SUSPEND, whose handler calls sduSuspendHookI(&SDU1)
  (usbcfg.cpp:446), tearing down the CDC channel -> host CDC write returns 0, COM149 drops.
- It is host-side (write wrote 0 = port handle invalidated), not a firmware stall: the switch
  completed cleanly, logging started, and the device kept emitting messages up to the drop.

Remediation directions (not implemented - investigation only):
- Don't swap the MSD LUN to a dead null device under a mounted volume. Instead present a stable
  medium or return SCSI "not ready / medium not present" (unit attention) so Windows performs an
  orderly media-eject rather than treating it as a device fault and resetting the port.
- Or signal proper SCSI medium-removal / unit-attention before switching so the host dismounts
  cleanly.
- Host-side, for a true 10x soak: reconnect LinkManager after each switch (treat the CDC drop
  as expected re-enumeration). The current sandbox intentionally reports it instead.
- The existing USB.pcapng capture can confirm the host-issued bus reset around a mode switch.

Validation:
- ./gradlew :ui:compileTestJava BUILD SUCCESSFUL after submodule init.
- ./gradlew :ui:runSdCycle exercised against real hardware; full log captured.

Open follow-ups:
- Decide remediation approach (firmware SCSI media-eject vs host-side reconnect).
- Consider gating: the soak cannot complete 10 cycles over one connection until the CDC drop is
  addressed.

## 2026-07-14 - SD indicator/output-channel name reuse between SdEcuPcCycleSandbox and .ini

What was done:
- Removed the duplicated magic strings sd_present / sd_logging_internal / sd_msd that existed
  independently in output_channels.txt (bit field names), tunerstudio.template.ini (6 indicator
  expressions) and SdEcuPcCycleSandbox.java (SensorCentral lookups). They now flow from a single
  source of truth via the existing code generation.

Change inventory:
| File                                                        | Change                                                          |
|-------------------------------------------------------------|-----------------------------------------------------------------|
| firmware/integration/rusefi_config_shared.txt               | new OUTPUT_CHANNEL_SD_PRESENT / _SD_LOGGING_INTERNAL / _SD_MSD quoted defines |
| firmware/console/binary/output_channels.txt                 | the 3 SD bit fields renamed to @#OUTPUT_CHANNEL_SD_...#@ references |
| firmware/tunerstudio/tunerstudio.template.ini               | 6 indicator lines reference @#OUTPUT_CHANNEL_SD_...#@           |
| java_tools/.../ReaderStateImpl.java (config_definition_base)| handleBitLine now applies variable substitution to the bit name (comment stays templated, matching plain-field parsing) |
| java_tools/.../ConfigFieldParserTest.java                   | new testBitNameViaVariableReference                             |
| java_console/.../SdEcuPcCycleSandbox.java                   | uses VariableRegistryValues.OUTPUT_CHANNEL_SD_* constants       |
| java_tools/version/.../UiVersion.java                       | CONSOLE_VERSION -> 20260714                                     |

Key decisions and why:
- Constants live in rusefi_config_shared.txt because it is prepended by BOTH pipelines that need
  them: gen_config_common.sh (template .ini + VariableRegistryValues.java) and the LiveData.yaml
  output_channels entry (LiveDataProcessor parsing output_channels.txt).
- Used the existing @#NAME#@ quote-stripping substitution (same as TS_HELLO_COMMAND usages) so the
  quoted define yields a bare identifier in struct field names and { } indicator expressions while
  generating a proper Java String constant.
- handleBitLine substitution was narrowed to the name part only: applying it to the whole line
  expanded @@...@@ comment templates at parse time, which changed engine_state_generated.h
  (clutchDownState comment). The narrowed version keeps every generated artifact byte-identical.
- Did not resurrect the deleted generated TsOutputs.java (removed in #6711); defines + existing
  VariableRegistryValues generation is the sanctioned mechanism.

Validation:
- ./gradlew :config_definition:test :config_definition_base:test green including the new test.
- gen_live_documentation.sh + gen_config_board.sh f407-discovery: all generated outputs
  (output_channels_generated.h, live_data_fragments.ini, data_logs.ini, board .ini indicator
  lines) byte-identical to committed state; only VariableRegistryValues.java gains the 3 new
  String constants. Board-generated .h/.ini signature/date churn reverted (CI regenerates).
- ./gradlew :ui:compileTestJava BUILD SUCCESSFUL with the sandbox on the generated constants.

Open follow-ups:
- Other magic output-channel names shared between java_console and .ini (e.g. sd_error,
  sd_formating, sd_active_wr/rd) could adopt the same pattern when java code starts using them.

---

## 2026-07-14 - N52 preset: bake in TPS/PPS calibration from reference tune

What: Ported TPS + PPS calibration out of the "super N52" TunerStudio tune (CurrentTune.msq)
into the `bmwN52()` engine preset in `firmware/config/engines/bmw.cpp`, so a fresh N52 selection
ships with the real throttle-body/pedal calibration instead of raw defaults.

| Field(s)                                                   | Source (msq, volts) | Stored as |
|------------------------------------------------------------|---------------------|-----------|
| tpsMin / tpsMax                                            | 4.545 / 0.58        | 10-bit ADC via convertVoltageTo10bitADC |
| tps1SecondaryMin / tps1SecondaryMax                       | 0.75 / 4.72         | 10-bit ADC via convertVoltageTo10bitADC |
| throttlePedalUpVoltage / throttlePedalWOTVoltage          | 0.625 / 2.230       | float volts (verbatim) |
| throttlePedalSecondaryUpVoltage / ...WOTVoltage           | 0.947 / 4.197       | float volts (verbatim) |

Key decisions:
- Used shared helpers setTPS1Calibration()/setPPSCalibration() (defaults.h), matching subaru.cpp.
- TPS stored as 10-bit ADC counts (not volts): wrapped msq volts in convertVoltageTo10bitADC
  (= volts*200). PPS stored as float volts, copied through directly. This units split is the main
  porting hazard and is now documented.
- Skipped tps2* fields: msq had them at defaults (0/5, 5/0); N52 runs a single dual-sensor throttle.
- Added #include "defaults.h"; convertVoltageTo10bitADC comes transitively via pch.h.

Docs: new docs/AI/engine_presets.md documents the canned-tune/preset process end to end
(enum -> engine_type_impl.cpp dispatch -> config/engines setup fn), the TPS-vs-PPS units gotcha,
and a step-by-step msq->preset porting recipe.

Validation: static review only - mirrors the established subaru.cpp calibration pattern; all
referenced helpers are declared in the included headers. No generated files touched (presets are
plain code, no gen_config step).

Open follow-ups:
- Confirm on hardware that a defaults-reset N52 reads plausible TPS
---

## 2026-07-14 - N52 preset: bake in TPS/PPS calibration from reference tune

What: Ported TPS + PPS calibration out of the "super N52" TunerStudio tune (CurrentTune.msq)
into the bmwN52() engine preset in firmware/config/engines/bmw.cpp, so a fresh N52 selection
ships with the real throttle-body/pedal calibration instead of raw defaults.

| Field(s)                                           | Source (msq, volts) | Stored as |
|----------------------------------------------------|---------------------|-----------|
| tpsMin / tpsMax                                     | 4.545 / 0.58        | 10-bit ADC via convertVoltageTo10bitADC |
| tps1SecondaryMin / tps1SecondaryMax                | 0.75 / 4.72         | 10-bit ADC via convertVoltageTo10bitADC |
| throttlePedalUpVoltage / throttlePedalWOTVoltage   | 0.625 / 2.230       | float volts (verbatim) |
| throttlePedalSecondaryUpVoltage / ...WOTVoltage    | 0.947 / 4.197       | float volts (verbatim) |

Key decisions:
- Used shared helpers setTPS1Calibration()/setPPSCalibration() (defaults.h), matching subaru.cpp.
- TPS stored as 10-bit ADC counts (not volts): wrapped msq volts in convertVoltageTo10bitADC
  (-> volts*200). PPS stored as float volts, copied through directly. This units split is the
  main porting hazard and is now documented.
- Skipped tps2* fields: msq had them at defaults (0/5, 5/0); N52 runs a single dual-sensor throttle.
- Added #include "defaults.h"; convertVoltageTo10bitADC comes transitively via pch.h.

Docs: new docs/AI/engine_presets.md documents the canned-tune/preset process end to end
(enum -> engine_type_impl.cpp dispatch -> config/engines setup fn), the TPS-vs-PPS units gotcha,
and a step-by-step msq->preset porting recipe.

Validation: static review only - mirrors the established subaru.cpp calibration pattern; all
referenced helpers are declared in the included headers. No generated files touched (presets are
plain code, no gen_config step).

Open follow-ups:
- Confirm on hardware that a defaults-reset N52 reads plausible TPS%/pedal% before user tuning.
---

## 2026-07-15 - SD card logging: overview doc + code comments (f_expand logic)

What: Documented the SD card logging subsystem and annotated its three core source files.

| File | Change |
|-------------------------------------------------------|--------------------------------------------|
| docs/AI/sd_card_logging.md (new)                       | End-to-end overview: SD thread mode state machine (IDLE/ECU/PC/UNMOUNT/FORMAT, all transitions via IDLE), .mlg vs .teeth loggers, FileBufferedWriter path, f_expand pre-allocation, file naming, status channels, console commands |
| firmware/hw_layer/mmc_card.cpp                         | Expanded LOGGER_MAX_FILE_SIZE, f_expand and f_truncate comments; doc pointer in file header |
| firmware/console/binary_mlg_log/binary_mlg_logging.cpp | File-header overview (MLG v2 layout, who owns file lifecycle); comments on writeFileHeader/writeSdBlock/writeSdLogLine/resetFileLogging |
| firmware/console/binary/tooth_logger.cpp               | Comment on freeBuffers/filledBuffers multi-buffering (BigBuffer, interrupt producers -> TS/SD consumers); ToothLoggerWriter() contract incl. 3s idle timeout -> new file |
| CLAUDE.md                                              | Added sd_card_logging.md to Deep Dive AI Guidance list |

Key facts captured (the f_expand logic in particular):
- sdLoggerCreateFile() pre-allocates each log file to 32Mb with f_expand(fd, size, opt=1)
  (allocate-now, contiguous; FF_USE_EXPAND=1 in firmware/ext/FatFS/ffconf.h). All FAT
  updates happen up-front, so writes inside the pre-allocated area never touch FAT
  structures -> sudden power loss loses buffered data but not the filesystem.
- f_expand failure (fragmented card) is deliberately non-fatal: FatFS falls back to
  cluster-by-cluster growth, logging works without the corruption protection.
- sdLoggerCloseFile() f_truncate()s back to actual size; a power-lossed file stays 32Mb
  with trailing garbage.
- Both loggers share sdLoggerCreateFile() (so .teeth files are pre-allocated too) but
  only sdLoggerMlg() enforces the 32Mb rollover cap.

Validation: comment/doc-only changes, no code touched; facts verified against source
(mmc_card.cpp, file_writer.h, ffconf.h FF_USE_EXPAND=1, tooth_logger.cpp, sd_log_trigger.h).

Open follow-ups: none.

## 2026-07-15 - Lua scripting API: categorized hook inventory doc

What: Reviewed every custom Lua method registered around lua_hooks.cpp and documented
them in a new docs/AI/lua_scripting.md, grouped into 11 categories.

| File | Change |
|-------------------------------|--------------------------------------------------------|
| docs/AI/lua_scripting.md (new) | Full inventory of Lua hooks by category: input reads, virtual sensors, virtual switches, closed-loop trims, cut/disable controls, PWM/DAC outputs, CAN, config/calibration access, state queries, luaaa helper classes, framework/test hooks; plus registration-site map, indexing conventions, build-flag gating, and an "adding a new hook" recipe |
| CLAUDE.md | Added lua_scripting.md to the Deep Dive AI Guidance list |

Key decisions / findings:
- Registration is spread over four files: lua_hooks.cpp (bulk + luaaa classes),
  lua_hooks_util.cpp (print/interpolate/find*/mcu_standby), lua.cpp (setTickRate,
  onTick dispatch), lua_can_rx.cpp (onCanRx dispatch, global_can_data workaround).
  lua_hooks_ext.cpp is an empty extension point; boardConfigureLuaHooks() is a weak
  board hook with no in-tree overrides.
- Documented the mixed indexing convention explicitly: 1-based (HUMAN_OFFSET) for
  TS-facing entities (CAN bus, curves, tables, TS buttons, gauges), 0-based for
  sensor indices, PWM channels, aux digital inputs, vin().
- Documented flash-saving exclusions (#if !defined(STM32F4) group) and the
  DISABLE_LUA_* / WITH_LUA_* opt-out macros.
- setTickRate code clamps 1..2000 Hz while its comment says 1..200 - doc records
  the code behavior (comment discrepancy left in source, not a functional issue).

Validation: doc-only change; every listed hook, guard macro and constant
(LUA_PWM_COUNT=8, LUA_GAUGE_COUNT=8, LUA_DIGITAL_INPUT_COUNT=8, LUA_BUTTON_COUNT=10,
CMD_BURNCONFIG="burnconfig") verified against source via grep/read of the four
registration files, lua_pid.h and rusefi_config.txt.

Open follow-ups:
- lua.cpp setTickRate comment ("Limit to 1..200 hz") disagrees with clampF(1, x, 2000).
- lua_hooks.cpp has a commented-out hasCriticalReportFile hook referencing issue #7291.

## 2026-07-17 - loss-of-cdc.pcapng analysis: one-shot composite reset from pre-capture MSD wedge

What: Analyzed loss-of-cdc.pcapng (repo root, USBPcap, 24.6 s, captured 2026-07-17
12:25 - i.e. the day AFTER the #9860 fix series landed) against the recent
mass_storage changes. Goal: confirm/refute whether the CDC drop mechanism from
issue #9860 is still present.

Devices in capture: address 21 = the ECU (VID 0483:5740, composite MSD+CDC),
address 22 = PEAK PCAN-USB adapter (19.9k of the 22k packets - unrelated noise).

Timeline (t = seconds from capture start):
- t=0..10.8: ZERO MSD traffic from the ECU. A healthy medium-less device gets
  ~1 Hz Test Unit Ready polls (visible later in this same capture), so at capture
  start usbstor already had one command in flight that never completed - the MSD
  side was already wedged/stuck before the capture began.
- t=7.79: host opens the COM port (GET/SET LINE CODING burst); CDC request/reply
  traffic (TS-style 7/11-byte commands, 1024-byte replies) runs cleanly for 3 s.
- t=10.847: usbstor ~20 s give-up timer fires -> all-endpoint cancel storm on the
  ECU: 10 URBs with USBD_STATUS_CANCELED (0xc0010000) - MSD bulk-IN 0x81 (the
  stuck data/CSW read, pending since before capture start), CDC data 0x82/0x02,
  CDC interrupt 0x83, plus control. This is the loss-of-CDC moment.
- t=10.883: host immediately retries line coding - those control URBs are
  canceled too (device still resetting).
- t=11.03..11.05: MSD recovers: Test Unit Ready on LUN0 and LUN1 -> Check
  Condition -> Request Sense (Good) -> Mode Sense(6) (the known-cosmetic
  "malformed" short caching page). Both LUNs report medium-not-present.
- t=11.28: CDC port re-opens at USB level (line coding OK) but NO data traffic
  follows - the app-level session was dead, host serial layer sat in its ~10 s
  timeout.
- t=12..24.5: clean steady state: 1 Hz TUR polls per LUN, no stalls, no babble,
  no further cancels or resets.
- t=20.79: app fully reconnects (line coding + control line state), TS-style
  traffic resumes. Total user-visible CDC outage: ~10 s (10.85 -> 20.79).

Reading vs the 2026-07-16 fix series (298162eb075..68e7d77c042, all in
firmware/hw_layer/mass_storage/):
- 298162eb0/8a515546c (MSD diag #9838): sdinfo diagnostics incl. per-opcode
  in-flight timer.
- e1feee380 (isCommandAbandoned #9861): 10 s data-phase timeouts on all SCSI
  transfers + CSW via msdUsb*Timeout helpers -> wedged thread self-recovers,
  re-arms bulk-OUT.
- 12b613c59 (#9864): LUN detach now synchronizes with in-flight command
  (m_lunMutex held around scsiExecCmd+CSW) -> kills the SPI double-waiter
  deadlock from the SD mode switch.
- 04331c28f (#9866) + 68e7d77c0 (uaefi): medium-less data-IN commands answered
  with ZLP instead of STALL -> no EP0 clear-halt round-trip near CDC traffic.
The capture is consistent with the fixes WORKING as designed for the recurring
part: exactly ONE reset (the tail of a wedge that began ~9 s before capture,
matching usbstor's ~20 s timer), then 13.5 s of clean behavior with no repeat
reset - the old signature was a reset every ~20 s.

Remaining gap (why one reset still happens): the firmware 10 s data-phase
timeout releases the MSD *thread*, but leaves the *host's* pending IN URB
hanging - firmware just returns to CBW wait and never completes/STALLs the
IN transfer the host is still waiting on. usbstor therefore still escalates to
a full composite reset once, taking CDC down with it. A full fix would complete
the host's data phase on timeout (e.g. STALL the IN endpoint so the host gets
an immediate error -> clear-halt -> CSW path) instead of leaving the URB
pending. Caveat: cannot verify from the capture which firmware build was
flashed or which opcode wedged (the CBW predates the capture); console sdinfo
counters (data-phase timeouts / no-data ZLPs) on the connected unit would
distinguish "fixed firmware, host-side URB gap" from "stale firmware".

Validation: tshark 3.6.2 field-level analysis (usb.usbd_status, endpoints,
SCSI dissection); code cross-checked at HEAD (mass_storage_device.cpp timeout/
ZLP/mutex mechanisms present).

Open follow-ups:
- On data-phase timeout, also complete the host-visible transfer (STALL data-IN
  or arm+flush) so usbstor never needs its 20 s reset - would remove the single
  remaining CDC drop.
- Confirm via sdinfo on hardware whether the flashed build has the 07-16 fixes
  and whether data-phase timeout counters tick.

## 2026-07-17 - MSD data-phase timeout: close the command host-side (stall + phase-error CSW)

What: Implemented the follow-up from the loss-of-cdc.pcapng analysis (previous
entry). Before this change, a data-phase timeout only freed the MSD *thread*
(e1feee380 #9861); the *host's* pending URB was left hanging and the CSW was
skipped, so usbstor still escalated to one full composite-device reset per
wedge - taking the CDC console down for ~10 s each time.

| File | Change |
|----------------------------------------------------|----------------------------------------|
| firmware/hw_layer/mass_storage/mass_storage_device.cpp | ThreadTask: split the abandoned-command check. BOT reset still skips the CSW (host is not expecting one). Data-phase timeout now STALLs the data endpoint in the CBW's direction (usbStallTransmitI/usbStallReceiveI) and then sends a CSW with CSW_STATUS_PHASE_ERROR and honest residue. sendCsw() now returns whether the host read the CSW; sdinfo prints "N data-phase timeouts (M closed by CSW)" |
| firmware/hw_layer/mass_storage/mass_storage_device.h | sendCsw() -> bool; new m_timeoutCswDeliveredCount counter |

Key decisions and why:
- STALL is the BOT-sanctioned "cannot complete this data phase" signal: a host
  still waiting on its data URB completes it with an error immediately (well
  before usbstor's ~20 s give-up), does a clear-halt on this one endpoint, and
  collects the CSW - recovery stays class-level on the MSD interface, the CDC
  endpoints never notice. A host that already canceled its URBs ignores the
  stall and resets anyway - no worse than before.
- Arming the CSW while the endpoint is still stalled is the exact sequence the
  pre-ZLP medium-less path used (04331c28f), already validated on Windows
  hardware (STALL -> clear-halt -> CSW observed on the wire).
- CSW_STATUS_PHASE_ERROR rather than FAILED: after a broken data phase the
  transport has genuinely lost sync; phase error makes the host run Bulk-Only
  Reset Recovery (class request + clear both halts), fully resynchronizing
  data toggles without any port-level reset. The existing onBulkOnlyResetIsr
  path handles that request.
- Safe to stall: all three msdUsb*Timeout helpers clear the endpoint's
  active flag on timeout, so usbStall*I (which refuses while a transfer is
  active) always takes effect by the time ThreadTask runs the recovery.
- The no-data-ZLP timeout path intentionally keeps its plain 'continue': a
  host that will not even take a zero-length packet is gone from the data
  phase entirely; its next action is a new CBW (accepted normally) or a reset.

Validation: uaefi firmware build (see below). No unit-test coverage exists for
this path (EFI_PROD_CODE + HAL_USE_USB_MSD only). Hardware validation plan:
reproduce the wedge (host abandons a command mid-data-phase), then check
1. sdinfo shows "closed by CSW" ticking together with data-phase timeouts,
2. a capture shows STALL -> clear-halt -> CSW(phase error) -> BOT reset
   instead of the all-endpoint cancel storm,
3. the CDC console stays connected across the event.

Open follow-ups:
- Wedges *below* the USB layer (e.g. blkRead stuck on a dying SD card) are
  still uncovered: no timeout wraps lib_scsi's block-device calls, so such a
  wedge never reaches the new recovery path (lib_scsi is in ChibiOS-Contrib).
- The loss-of-cdc.pcapng pre-capture wedge could not be attributed (stale
  firmware vs blkRead wedge); confirm the flashed build via sdinfo counters.

## 2026-08-01 - Decouple VE Analyze from the STFT display scale

What: Restored zero-based STFT presentation without changing the 100-based
correction contract required by TunerStudio VE Analyze. PR #9657 changed the
STFT translation from -1.0 to -100; under TunerStudio's `(raw + translate) *
scale` conversion, a neutral raw multiplier of 1.0 became -9900 percent. The
derived `100 + stftCorrection1` channel then supplied -9800 instead of 100 to
VE Analyze, causing it to remove fuel.

| File | Change |
|----------------------------------------------------|----------------------------------------|
| firmware/controllers/algo/engine_state.txt | Restore STFT display metadata to scale 100, translation -1.0, so raw 0.9/1.0/1.1 displays as -10/0/+10 percent |
| firmware/tunerstudio/tunerstudio.template.ini | Feed `egoCorrectionForVeAnalyze` directly from `Gego`, the existing 100-neutral STFT output channel |
| java_tools/configuration_definition/src/test/java/com/rusefi/test/VeAnalyzeCorrectionTest.java | Regression coverage for zero-neutral display and independence of the VE Analyze channel |
| java_tools/version/src/main/java/com/rusefi/UiVersion.java | Bump console version to 20260801 as required for Java changes |

Key decisions and why:
- Reused `Gego` instead of adding another live-data field. `status_loop.cpp`
  already publishes it as `100 * stftCorrection[0]`, so this avoids output
  layout churn and keeps the machine-facing 100-neutral contract explicit.
- Kept the user-facing `stftCorrection` channels zero-neutral and independent
  from AutoTune. Gauge scale or translation changes can no longer alter the
  correction consumed by VE Analyze.
- No persistent calibration field or generated file is part of the change, so
  existing tunes require no migration.
- LTFT behavior is intentionally unchanged in this unit of work. Stored LTFT
  correction still affects delivered fuel without being represented in the VE
  Analyze correction channel; that requires a separate policy change and test.

Validation:
- Regression test first failed on the old code: raw 0.9 displayed as -9910
  instead of -10, and VE Analyze still referenced the visual STFT channel.
- The same test passes after the fix.
- `gradlew.bat :config_definition:test` passes.
- Clean uaefi `make -B -j12 ini` generation passes. The generated INI contains
  `stftCorrection1/2` with `100.0, -1.0`, keeps `Gego` at scale 0.01, and emits
  `egoCorrectionForVeAnalyze = { Gego }`; both VE Analyze and WUE Analyze use
  that alias.

Open follow-ups:
- Define and test the LTFT policy during AutoTune (disable application, require
  applying/resetting learned trims, or introduce an explicit tuning session).
- Decide how a future bank-2-aware VE Analyze correction should select/combine
  STFT banks; this change preserves the existing bank-1 behavior.

## 2026-08-13 - Console logs the real build date instead of the 1969 epoch (#6836)

What: The console and the updater logged
"Compiled Wed Dec 31 19:00:00 EST 1969" instead of a build timestamp.

Root cause: `rusEFIVersion#classBuildTimeMillis` handled the `jar:` protocol by
chopping the "file:" prefix off the URL path with `path.substring(5, ...)`.
That path is percent-encoded, so any installation directory containing a space
produced a file name with a literal `%20`, a file which does not exist, and
therefore `lastModified() == 0`. `new Date(0)` then rendered the epoch.

Reproduced exactly, with the jar URL shape of a bundle installed under
"Program Files":

    current  -> C:\Program%20Files\Purple%20Updater\console\rusefi_console.jar
    exists   -> false, lastModified=0
    printed  -> Wed Dec 31 17:00:00 MST 1969
    fixed    -> C:\Program Files\Purple Updater\console\rusefi_console.jar

The same encoding bug also affected the "Source ..." line logged by
`Autoupdate#main`, which is where it first showed up in the #10000 log.

| File | Change |
|-------------------------------------------------------|--------------------------------------------------|
| java_console/shared_io/.../rusEFIVersion.java | New `jarFileOf` parses the jar URL as a URI; new `classBuildTimeString` renders "unknown" rather than the epoch |
| java_console/ui/.../Launcher.java | Use `classBuildTimeString()` |
| java_console/autoupdate/.../Autoupdate.java | Use `classBuildTimeString(Class)`; `toURI()` for the "Source" log line; bump AUTOUPDATE_VERSION |
| java_tools/proxy_server/.../Monitoring.java | Use `classBuildTimeString()` |
| java_console/shared_io/src/test/.../RusEfiVersionTest.java | 7 cases: encoded path, plain path, encoded file name, missing separator, malformed URL, relative URL, no-epoch contract |

Key decisions and why:
- Two separate defects, both fixed. Decoding the path makes the timestamp
  correct for the overwhelming majority of installs; rendering "unknown"
  covers the cases where the timestamp genuinely cannot be determined, so the
  log never again claims a 1969 build.
- `jarFileOf` is a package-visible pure function taking the URL path as a
  string, so the tests cover both the encoded and the malformed cases without
  building a jar or touching the class loader. No reflection.
- `jarFileOf` returns null instead of throwing. `new File(URI)` rejects
  relative and opaque URIs with `IllegalArgumentException`, and a logging
  helper must never be the reason startup fails.
- Removed the now-unused `java.util.Date` imports from the two call sites that
  no longer construct a Date.

Validation:
- Old and new path resolution compared side by side on the "Program Files"
  URL shape; the old one reproduces the issue's literal 1969 string.
- `gradlew :shared_io:test :autoupdate:test :ui:shadowJar :proxy_server:compileTestJava`
  green, 7 new tests among them.
- Not exercised by launching an installed bundle from a spaced path - verified
  at the unit level and by the side-by-side reproduction only.

## 2026-08-24 - Units-expression migration gap: minimal GREEN coverage

What was done:
- Added java_console/io/src/test/java/com/rusefi/maintenance/migration/
  UnitsExpressionMigrationTest.java - 4 JUnit5 tests, all GREEN against
  current behavior, documenting the bug that lost a Harley hd81 customer's
  VE/ignition load axes during the Kansas -> Lima firmware update: the
  customer's 20..180 bins were silently replaced by the new defaults
  (10..160 / 21..120).
- Root cause under test: IniFieldMigrationUtils.checkIfUnitsCanBeMigrated
  compares RAW unevaluated TS units strings. Lima changed units from
  Kansas's `{bitStringValue(fuelUnits, fuelAlgorithm) }` (veLoadBins) /
  literal `Load` (ignitionLoadBins) / literal `kPa` (boostCutPressure) to
  new `{bitStringValue(...)}` expressions for kPa/psi display support; the
  strings differ textually while the physical unit (kPa) is unchanged, so
  DefaultTuneMigrator refuses with "WARNING! Field `...` cannot be updated
  because its units are updated" and the tuned value is dropped.

Key decisions and why:
- Tests parse the VERBATIM hd81 Kansas/Lima ini lines through the
  production tokenizer (RawIniFile.Line -> ArrayIniField/ScalarIniField
  .parse) rather than passing hand-written unit strings - this pins the
  actual contract: splitTokens strips quotes (`"Load"` -> `Load`) but keeps
  `{...}` expressions raw and whole (spaces, trailing ` }` included), which
  is exactly what reaches the comparison in the updater flow (both tunes
  come from CalibrationsInfo.generateMsq; TS-saved .msq files carry
  EVALUATED units and do NOT reproduce the bug).
- assertFalse() calls are marked as bug-documenting: flip to assertTrue()
  when checkIfUnitsCanBeMigrated learns to evaluate or tolerate expression
  units. A control test shows identical expressions still migrate.
- Placed in the io module (":ecu_io" in gradle) next to the code under
  test; the end-to-end board-level RED repro already lives in fw-iws
  (java-tests/board-specific-tests KansasLimaMigrationTest, see that
  repo's docs/report.md 2026-08-24 fourth entry).

Validation:
- ./gradlew :ecu_io:test --tests '*UnitsExpressionMigrationTest*' - 4/4
  pass (JUnit XML confirms all 4 testcases executed, 0 failures).

Open follow-ups:
- Implement the fix in checkIfUnitsCanBeMigrated (evaluate/ignore `{...}`
  expression units, ideally with a same-evaluated-unit check), then flip
  the three assertFalse() to assertTrue() and un-RED the fw-iws
  KansasLimaMigrationTest.

## 2026-08-24 - Fix: TS `{...}` expression units no longer block tune migration

What was done:
- Fixed checkIfUnitsCanBeMigrated (java_console/io/.../migration/
  IniFieldMigrationUtils.java): if either side's units string is a TS
  `{...}` expression (trimmed string starts with `{`), the units check
  passes. Expressions reach the migrator unevaluated, so the same
  physical unit can be spelled as a literal in one ini and as an
  expression in the other (or as two different expressions) - a raw
  string mismatch involving an expression says nothing about the
  physical unit, while refusing silently replaces the user's tuned
  value with the new firmware default (the Kansas -> Lima load-axis
  loss from the previous entry).
- Updated UnitsExpressionMigrationTest to assert the FIXED behavior:
  the three former bug-documenting assertFalse() flipped to
  assertTrue(); added differentLiteralUnitsAreStillRefused (afr vs
  lambda) proving the literal-vs-literal guard is untouched.

Key decisions and why:
- Tolerate (skip) expression units rather than evaluate them: proper
  evaluation of bitStringValue(...) needs the ini's string lists plus
  the live selector field values - far beyond this comparison's reach.
  The check keeps guarding real literal unit changes; the remaining
  type/row/col checks in DefaultTuneMigrator and
  DefaultIniFieldMigrationStrategy still apply to expression-unit
  fields.
- Both call sites (DefaultTuneMigrator, DefaultIniFieldMigrationStrategy)
  share the helper, so scalars (boostCutPressure & friends) are covered
  by the same one-line policy.

Validation:
- ./gradlew :ecu_io:test - all 25 suites green, including the 5-test
  UnitsExpressionMigrationTest.
- ./gradlew :ui:test --tests '*Migrat*' --tests '*migration*' - all
  migration suites green, notably DefaultTuneMigratorTest (26 tests,
  includes the afr-vs-lambda refusal) and CalibrationsHelperTest (19).

Open follow-ups:
- fw-iws's end-to-end KansasLimaMigrationTest (RED repro against the
  submodule copy of this code) flips green once ext/fw-private/ext/rusefi
  picks up this change.
- Optional future hardening: same-evaluated-unit check for expressions
  once an expression evaluator with ini context is available.

## 2026-08-27 - Fix: "Grab baro value from MAP" latched 101.325 kPa (#9744)

What was done:
- Root-caused rusefi#9744: with `useFixedBaroCorrFromMap` enabled, barometric
  pressure stayed at 101.325 kPa even though MAP reported ~95 kPa at key-on.
- `initMapDecoder()` (controllers/sensors/impl/map.cpp) read
  `Sensor::get(SensorType::MapSlow).value_or(STD_ATMOSPHERE)`. In
  commonInitEngineController() `initNewSensors()` (engine_controller.cpp:440)
  only *subscribes* slowMapSensor to the ADC; `initSensors()` ->
  `initMapDecoder()` runs three lines later on the same thread, so no slow-ADC
  callback has fired and MapSlow is always invalid. The `.value_or()` therefore
  returned STD_ATMOSPHERE = 101.325, `validateBaroMap()` accepted it (plausible
  range is 60..110 kPa), and `Sensor::setMockValue(BarometricPressure, ...)`
  latched it permanently (`m_useMock` is sticky, sensor.cpp:19).
- Deferred the grab to the slow callback:

  | File | Change |
  |---|---|
  | controllers/sensors/impl/map.cpp | `initMapDecoder()` now only arms `baroFromMapPending` + resets a Timer; new `updateFixedBaroFromMap()` performs the grab |
  | controllers/sensors/impl/map.h | declares `updateFixedBaroFromMap()` (reaches all TUs via pch -> allsensors.h) |
  | controllers/algo/engine.cpp | calls it from `periodicSlowCallback()`, right after `updateSlowSensors()` |
  | unit_tests/tests/sensor/test_baro_from_map.cpp | new, 5 tests |
  | unit_tests/tests/tests.mk | registers the new test file |

Key decisions and why:
- One shot per valid sample: as soon as MapSlow becomes valid we validate and
  either latch or disable, and never retry. Retrying would re-run
  `validateBaroMap()` every slow callback and spam `warning()`.
- Engine-turning guard (`Rpm > 0` -> give up): MAP only reads atmosphere with
  the engine stopped, so a late first sample must not be trusted.
- 3 s timeout -> one `OBD_Barometric_Press_Circ` warning, then give up. Covers
  MAP not configured / sensor faulted, without waiting forever.
- On any failure path we leave BarometricPressure *unregistered* rather than
  mocking STD_ATMOSPHERE. That matches the pre-existing "the fixed baro
  correction will be disabled" branch: `getBaroCorrection()` returns 1 when
  `!hasSensor(BarometricPressure)` (fuel_math.cpp:457). It also makes the up-to
  50 ms delay before the first grab harmless - correction is neutral meanwhile.
- Not made an EngineModule: no TS page, and a two-line hook keeps the change
  small. The pre-existing "TODO: do literally anything other than this" on the
  setMockValue hack is left in place - out of scope here.

Validation:
- RED first (per .junie/guidelines.md "Bug Fix Process"), retrofitted: I wrote
  fix and test together, which violates the mandated order, so I proved the
  test's RED afterwards by temporarily restoring the pre-fix behaviour in
  map.cpp. 4 of 5 tests failed, with the diagnostic literally reading
  `Which is: 101.325` - the issue's symptom. Restored, all 5 pass.
- Full suite after restore: 1198 tests / 236 suites, all pass.

Environment notes (not repo changes):
- This machine had no `make`/gcc on PATH, so `unit_tests/test.sh` fails with
  `make: command not found`. Built with the MSYS2 UCRT64 toolchain by exporting
  PATH=/c/msys64/ucrt64/bin:/c/msys64/usr/bin.
- That GCC is 16.1.0 (much newer than CI) and rejects pre-existing
  `unit_tests/mocks.cpp:38` (`MockAirmass::MockAirmass() :
  AirmassVeModelBase(veTable)`) with `-Werror=maybe-uninitialized`. Worked
  around on the command line only, via `make UDEFS='-Wno-error=maybe-uninitialized'`.
  `UDEFS` is the free additive slot (rules.mk:53 `DEFS = $(DDEFS) $(UDEFS)`,
  `UDEFS =` empty at unit_test_rules.mk:235). Do NOT use `DDEFS` for this - it
  carries the real project `-D`s including `META_GENERATED_H_OVERRIDE`, and
  overriding it from the command line breaks the whole build.

Open follow-ups:
- clang build of unit tests not verified: no clang in this environment (MSYS2
  install has no clang64/ucrt64 clang). CLAUDE.md wants both compilers; left to CI.
- `unit_tests/mocks.cpp:38` will need a real fix (or a targeted suppression)
  whenever CI moves to GCC 16.
- `useFixedBaroCorrFromMap` remains boot-only (unchanged by this fix):
  `initMapDecoder()` is not re-run on Burn, so toggling it in TS still needs a
  reboot. Not listed in the ini `requiresPowerCycle` set - candidate for
  docs/hardware-reinit-and-power-cycle.md if it ever confuses someone.

## 2026-08-27 - Review round on PR #10153 (baro from MAP)

What was done:
- Addressed both inline comments from dron0gus's CHANGES_REQUESTED review on
  PR #10153 (the #9744 fix from the previous entry). No objection was raised to
  the mechanism itself - deferring the grab to the slow callback, the
  engine-turning guard and the timeout all stood.

  | Comment | Change |
  |---|---|
  | "Making validateBaroMap() return SensorResult will simplify further code." | `validateBaroMap()` returns `SensorResult` instead of float-with-NaN-sentinel |
  | "Print actual value instead of \"this\"?" | Confirmation message now carries the kPa value; the two prints collapsed to one per outcome |

Key decisions and why:
- Kept the parameter as `float` and changed only the return type. The caller
  already validated the `SensorResult` from `Sensor::get(MapSlow)` before
  calling, so taking a `SensorResult` in would just move the same check around.
  The `std::isnan()` guard inside stays - it is now the only NaN handling left.
- Dropped the pre-validation `"Get initial baro MAP pressure = %.2fkPa"` line.
  With the value printed in both outcome messages (and `validateBaroMap()`
  already warning with the value on rejection) it carried no information that
  is not printed elsewhere, and it read as a success line even when the value
  was about to be rejected.
- Both findings were in code carried over unchanged from the original
  implementation rather than written fresh for the fix - worth noting as a
  pattern: moving code into a new function is a good moment to clean up its
  idioms, not just relocate them.

Validation:
- Build on master base: 0 errors. Full suite 1201 tests / 237 suites, all pass,
  BaroFromMap 5/5.
- CI on the previous commit (4173a63ed6) was fully green: 64 checks, 0 failures,
  including `build (macos-latest)` - which closes the "clang not verified"
  follow-up from the previous entry - plus `build (ubuntu-latest)`,
  `clang-format`, and `hardware-ci` on f407-discovery and nucleo_f767.

Open follow-ups:
- `unit_tests/mocks.cpp:38` still trips GCC 16's `-Wmaybe-uninitialized`; only
  a local concern until CI moves to that compiler (see previous entry).

## 2026-10-02 - PWM input TPS (RC servo pulse as throttle position)

What was done:
- New sensor `PwmInputTps` (`firmware/controllers/sensors/pwm_input_tps.*`): reads an RC-servo
  pulse (500-2500 us) on a digital input with both-edge EXTI capture and registers it directly as
  `SensorType::Tps1`, replacing the analog TPS1 when `pwmInputTpsPin` is set. Target use: a
  helicopter where the autopilot drives the throttle servo and rusEFI taps the same signal (uaEFI
  FLEX input, connector D5 / PE11) for alpha-N fueling, so the throttle body needs no TPS.

  | File | Change |
  |---|---|
  | `controllers/sensors/pwm_input_tps.h/.cpp` | New sensor: edge handling, conversion, 100 ms timeout, EXTI init/deinit |
  | `init/sensor/init_tps.cpp` | PWM input takes precedence over SENT and analog TPS1; deinit added |
  | `integration/rusefi_config.txt` | `pwmInputTpsPin`, `pwmInputTpsMinUs`, `pwmInputTpsMaxUs` |
  | `controllers/algo/defaults/default_base_engine.cpp` | 1000 / 2000 us defaults via `applyDefaultsOrFixAfterBurn()` |
  | `tunerstudio/tunerstudio.template.ini`, `secondary_panels.ini` | "PWM input TPS" dialog in the TPS page; analog TPS1 panel hidden while it is active |
  | `unit_tests/tests/sensor/test_pwm_input_tps.cpp` | 11 tests |

Key decisions and why:
- Registered as `Tps1` directly (like `SentTps`), not as `Tps1Primary`: there is no second
  channel to make a redundant pair from.
- Min > max reverses direction instead of adding an invert flag. Equal min/max -> invalid with
  `UnexpectedCode::Configuration`.
- Pulses outside 500-2500 us invalidate the sensor (Low/High); plausible pulses outside the
  calibrated span clamp to 0/100%.
- A falling edge with no preceding rising edge is ignored (signal already high at start-up, or a
  missed edge); the pin level is read in the EXTI callback, so a lost edge cannot invert the
  measurement.
- The pulse width is NOT exposed via `getRaw()`/`hasRaw()`: those are consumed as volts by the
  check engine light range checks and by the TPS "grab closed/open" buttons. Exposed as
  `getPulseWidthUs()` and printed by `sensorinfo`.
- No new output channel: TPS1 % is already logged; calibration uses `sensorinfo`.

Validation:
- Unit tests: 1358 tests / 263 suites pass (GCC 13), including the 11 new `PwmInputTps` tests
  (conversion, reversed range, implausible pulses, zero span, defaults, Tps1 replacement,
  50 and 333 Hz frames, stray falling edge, recovery, timeout).
- Not built for any firmware target (no ARM toolchain in the environment); the EFI_PROD_CODE part
  is the EXTI enable/disable, modelled on `init_flex.cpp`. Not hardware tested.

Open follow-ups:
- Bench test on uaEFI: confirm the FLEX input front end (10 k pull-up) switches cleanly on a
  3.3 V autopilot PWM signal, and check pulse-width accuracy with a scope.
- A TunerStudio gauge for the measured pulse width would make calibration easier than `sensorinfo`.
- clang build not run locally; macOS CI covers it.

## 2026-10-02 - Servo throttle output, passthrough mode

What was done:
- rusEFI now drives an RC-style throttle servo (`ServoThrottle` engine module,
  `firmware/controllers/actuators/servo_throttle.*`). Architecture change for the hybrid
  helicopter project: the autopilot no longer governs the engine. It sends a throttle request
  pulse (ArduPilot `H_RSC_MODE=3`, throttle curve), rusEFI owns the servo and will own the rotor
  speed governor. This first step is passthrough: command = request.

  | File | Change |
  |---|---|
  | `controllers/actuators/servo_throttle.h/.cpp` | New module: request -> command (passthrough, hold on loss, closed before first request) -> pulse width -> hardware PWM; TPS1 = command; config-error producer |
  | `controllers/algo/engine.h` | `ServoThrottle` in the module list, before `TpsAccelEnrichment` |
  | `controllers/engine_controller.cpp` | `initServoThrottleOutput()` once at boot |
  | `controllers/core/error_handling.cpp` | `checkServoThrottleConfigError()` in `refreshConfigErrorState()` |
  | `controllers/sensors/pwm_input_tps.*` | `initPwmInputTps(registerAsTps1)`: in servo mode the pulse is the request and is not TPS1; `getPwmInputTps()` accessor replaces the unit-test-only one |
  | `init/sensor/init_tps.cpp` | Servo mode registers the command as TPS1 |
  | `integration/rusefi_config.txt` | `servoThrottlePin`, `servoThrottleFrequency`, `servoThrottleClosedUs`, `servoThrottleOpenUs` |
  | `controllers/algo/defaults/default_base_engine.cpp` | 50 Hz, 1000 / 2000 us defaults |
  | `tunerstudio/*.ini` | "Servo throttle" dialog in the TPS page; pin and frame rate require power cycle |
  | `unit_tests/tests/actuators/test_servo_throttle.cpp` | 9 tests |

Key decisions and why:
- TPS1 reports rusEFI's command, not the request: once the governor exists the two differ, and
  fueling must follow what the throttle does.
- Request lost (timeout or implausible pulse) -> hold the last command. On a helicopter, closing
  the throttle on a lost signal is the unsafe choice; the coming governor will hold rotor speed.
  Before the first valid request the command is closed (0%).
- Output started once at boot: `hardware_pwm::tryInitPin()` takes a new timer channel on every
  call and never releases one, so restarting on each burn would leak channels. Pin and frame rate
  are `requiresPowerCycle`; pulse-width calibration applies live.
- Missing request input is a level-triggered config error (refreshConfigErrorState producer), not
  a critical error: the output just holds closed.
- Output calibration (closed/open us) is separate from the input calibration, so the servo travel
  can be matched to the throttle linkage independently of the autopilot's range.

Validation:
- Unit tests: 1367 tests / 264 suites pass (GCC 13), including 9 ServoThrottle and 11 PwmInputTps
  tests. New sources also compiled with clang (`-Wall -Wextra`, syntax only): no diagnostics in them.
- First full-suite run crashed (ASan SEGV): `ConfigErrorRefresh` tests call
  `refreshConfigErrorState()` without an engine configuration; the producer now returns false
  when `engineConfiguration` is null.
- Not built for firmware (no ARM toolchain), not hardware tested.

Open follow-ups:
- Bench: confirm hardware PWM on the chosen uaEFI pin (Coil 6 = B10/PB8 TIM4, or Coil 2 =
  B14/PE5 TIM9) and the pulse width on a scope.
- Next step: rotor speed governor mode in `ServoThrottle` (request = feed-forward).
- Live data channels for request / command / pulse width would help tuning.

## 2026-10-05 - Servo throttle governor (rotor speed)

What was done:
- Closed-loop engine speed governor in `ServoThrottle` (`controllers/actuators/servo_throttle.*`):
  the throttle request from the autopilot (ArduPilot `H_RSC_MODE=3`, throttle curve) is the
  feed-forward, a PID on engine RPM adds a trim. Overspeed protection closes the throttle in both
  passthrough and governor modes.

  | File | Change |
  |---|---|
  | `controllers/actuators/servo_throttle.h/.cpp` | `ServoGovernorState` (Passthrough / Following / Governing / Overspeed), `update(request, rpm, dt)`, governor step, overspeed latch, live data |
  | `integration/rusefi_config.txt` | `servoGovernorEnabled`, `servoGovernorTargetRpm`, `servoGovernorEngageWindow`, `servoGovernorMinRequest`, `servoThrottleOverspeedRpm`, `pid_s servoGovernorPid` |
  | `console/binary/output_channels.txt` | `servoThrottleRequest`, `servoThrottlePulseUs`, `servoGovernorState`, `pid_status_s servoGovernorStatus` |
  | `controllers/algo/defaults/default_base_engine.cpp` | Migration: engage window 300 rpm, minimum request 10%; fresh tunes: PID 0.01 / 0.02 / 0, trim +-20% |
  | `tunerstudio/tunerstudio.template.ini` | Governor and overspeed fields in the "Servo throttle" dialog |
  | `unit_tests/tests/actuators/test_servo_throttle.cpp` | 15 ServoGovernor tests |

Key decisions and why:
- Feed-forward + trim, not a full PID on throttle: the autopilot's throttle curve already carries
  collective anticipation; the PID only corrects what the curve gets wrong (density altitude,
  engine condition). Trim authority (`pid_s` min/max) bounds what a bad tune or bad RPM can do.
- Governor off below `servoGovernorMinRequest`: that is how idle, motor interlock off and
  autorotation (all sent as a low request by ArduPilot) reach the throttle. A zero minimum would
  let the governor fight an idle request, so 0 is migrated to 10%.
- Engages only once RPM is inside the window below target (spool-up follows the request, like
  ArduPilot's own governor), bumpless (trim starts at 0). Leaving `Governing` resets the PID.
- Underspeed never disengages the governor: dropping the trim when the engine cannot keep up would
  remove power exactly when it is needed. The integrator is clamped to the trim authority
  (`iTermMin/Max`), so it recovers immediately when the load goes away.
- A lost request keeps governing on the last feed-forward (holds rotor speed).
- Target 0 never governs (it would trim the throttle towards 0 RPM).
- Overspeed latches until RPM is back at the governor target (or one engage window below the limit
  without the governor) to avoid chatter; 0 disables it.
- Target is engine RPM (rotor speed x gear ratio): the governor only sees engine speed, and there is
  no ratio parameter to get wrong.

Validation:
- Unit tests: 1382 tests / 265 suites pass (GCC 13), including 15 new ServoGovernor tests (spool-up,
  engage window, bumpless engage, integration to the authority limit, overspeed trim, clamping, low
  request, no RPM, zero target, lost request, overspeed latch in both modes, fast-callback path
  with mocked RPM and live data).
- Mutation check: removing the integrator clamp makes the anti-windup test fail.
- Changed sources compiled with clang (`-Wall -Wextra`, syntax only): no diagnostics in them.
- Not built for firmware (no ARM toolchain), not hardware tested.

Open follow-ups:
- Ground-run tuning on the DLE-60: start in passthrough, log `servoThrottleRequest` / TPS1 /
  RPM, then enable the governor with low gains.
- Optional fast trim through ignition timing (servo slew limits how fast throttle can correct).
- RPM source: the governor uses crank RPM, so a clutch slip shows up as rotor underspeed only via
  the autopilot's own RPM input.

## 2026-10-05 - Servo position model for TPS1 (KST SV12-12 as reference servo)

What was done:
- The project picked the KST SV12-12 (12 V rated, 8.4-13.0 V) as reference throttle servo, so no
  buck converter is needed. Its datasheet figures (vendor listings; the KST datasheet host is not
  reachable from this environment): 0.11 s/60 deg at 12 V, 8.0 kgf.cm at 12 V, 333 Hz frame rate,
  1000/1500/2000 us = -50/0/+50 deg, 800-2200 us = +-70 deg full travel.
- New optional servo position model: with `servoThrottleFullTravelMs` set, TPS1 follows the command
  at the servo's speed instead of jumping to it. At 0.11 s/60 deg a 100 deg stroke takes ~183 ms, so
  without the model alpha-N acceleration enrichment fires before the throttle has moved.

  | File | Change |
  |---|---|
  | `controllers/actuators/servo_throttle.h/.cpp` | `m_positionPercent` rate-limited towards the command; TPS1 reports it; output pulse stays the command |
  | `integration/rusefi_config.txt` | `servoThrottleFullTravelMs` (0 = off, TPS1 = command) |
  | `tunerstudio/tunerstudio.template.ini` | Field in the "Servo throttle" dialog |
  | `unit_tests/tests/actuators/test_servo_throttle.cpp` | 3 ServoPositionModel tests |

Key decisions and why:
- Model, not measurement: the servo has an internal potentiometer but no feedback wire.
- Default 0 keeps the previous behaviour; the tooltip gives the formula and the SV12-12 number.
- Frame rate default stays 50 Hz (safe for any servo); 333 Hz is recommended for the SV12-12.

Validation:
- Unit tests: 1385 tests / 266 suites pass (GCC 13), including the 3 new model tests; changed sources
  compile cleanly with clang (`-Wall -Wextra`, syntax only).

Open follow-ups:
- Reported KST factory failsafe: after ~1-1.5 s without pulses the servo moves to 1500 us (mid
  stroke). Verify on the actual servo and reprogram (KST programming card) to hold, since rusEFI
  holds the last command itself and a wiring fault should not open the throttle to mid travel.
- Measure the real stroke time on the bench (step command, video or scope on the servo pot) and
  set `servoThrottleFullTravelMs` from it.

## 2026-10-05 - DLE-60 / DLE-120 twin engine presets, Alpha-N barometric correction

What was done:
- Engine presets for the DLE-60 Twin (engine type 106) and DLE-120 Twin (107), two-stroke boxer
  twins converted to fuel injection for the hybrid helicopter project (`config/engines/dle_twin.*`).
- Opt-in barometric correction for Alpha-N (`alphaNUseBaro`): Alpha-N airmass used a hard-coded
  standard atmosphere (there was a TODO for it), so at the project's 1500-2000 m operating altitude
  every Alpha-N engine ran ~18-21% rich.

  | File | Change |
  |---|---|
  | `config/engines/dle_twin.h/.cpp` | `setDle60Twin()`, `setDle120Twin()` + shared base |
  | `controllers/algo/engine_type_e.h`, `engine_type_impl.cpp`, `config/engines/engines.mk` | `DLE_60_TWIN = 106`, `DLE_120_TWIN = 107`, board-independent |
  | `controllers/algo/airmass/alphan_airmass.cpp` | Pressure = `BarometricPressure` sensor when `alphaNUseBaro`, standard atmosphere otherwise or on sensor failure |
  | `integration/rusefi_config.txt`, `tunerstudio/tunerstudio.template.ini` | `alphaNUseBaro` bit, field next to "Alpha-N uses IAT" |
  | `unit_tests/tests/test_dle_twin.cpp` | 6 tests: configuration, one spark + one injection per revolution while running up, WOT injector duty |
  | `unit_tests/tests/ignition_injection/test_fuel_math.cpp` | `AirmassModes.AlphaNUseBaro` |

Key decisions and why:
- Both engines are twins (earlier project notes called the DLE-60 a single - wrong). The boxer's
  pistons reach TDC together and the stock DLE "TWIN" ignition fires both plugs at once, so the
  preset models one two-stroke cylinder of the full displacement: `cylindersCount = 1`,
  `IM_ONE_COIL` into a two-tower coil firing both plugs. Simultaneous firing still to be confirmed
  with a timing light on the real engines.
- Trigger: the stock hub magnet on a Hall sensor = `TT_NARROW_SINGLE_TOOTH`, `twoStroke`.
  `globalTriggerAngleOffset` = 28 is an assumption (typical magnet-to-TDC for these CDIs); an error
  goes straight into spark timing, and a larger offset errs towards retard.
- Alpha-N on TPS = servo command, with IAT and barometric correction. VE starting map rises to ~90%
  at WOT: at rated power these engines move about their swept air mass per revolution (DLE-60:
  7 hp x ~540 g/kWh at AFR ~12.5 = ~95%). A 70% ceiling (first draft) would have been ~25% lean at
  WOT - seizure risk on an air-cooled two-stroke. Lambda target 1.0 below 40% TPS, 0.85 at 80%+.
- RPM-only timing (stock-CDI-like): 10 deg up to 1500 rpm, 22 deg at 3000, 26 deg from 4000.
- Injectors sized by the WOT duty test: DLE-60 100 cc/min -> 75% at 8500 rpm; DLE-120 150 cc/min
  gave 87%, raised to 180 cc/min -> 76% at 8000 rpm.
- Servo throttle defaults for the KST SV12-12 reference servo (333 Hz, 180 ms stroke); overspeed
  closes the throttle 300 rpm below the hard rev limit (9000 / 8500).

Validation:
- Unit tests: 1392 tests / 267 suites pass (GCC 13); changed sources compile cleanly with clang
  (`-Wall -Wextra`, syntax only).
- Running test: starter 1500 rpm -> 3000 -> 6000 rpm on a 0.3 ms magnet pulse: RPM correct, no
  trigger errors, no out-of-order sparks, one spark and one injection per revolution. A jump from
  standstill straight to 6000 rpm makes the spark logic skip one dwell on purpose (out-of-order
  guard) - not a fault.
- Not run on an engine.

Open follow-ups:
- Measure the magnet-to-TDC angle (timing light at fixed cranking timing) and set
  `globalTriggerAngleOffset`.
- Choose real injectors and coil; set flow, dead times and dwell.
- Live barometric source on uaEFI for `alphaNUseBaro` (analog baro sensor or LPS25 on I2C; the
  start-up MAP reading does not follow altitude changes in flight).
- Tune VE with a wideband; check two-stroke premix stoichiometry against the wideband reading.

## 2026-10-05 - DLE crank trigger wheel: 36-1 vs 60-2

What was done:
- The project will fit a toothed crank wheel to both DLE engines instead of using the hub magnet.
  Both candidate wheels were run through the DLE presets in two-stroke mode
  (`unit_tests/tests/test_dle_twin.cpp`, 4 new tests: each wheel on each engine, 1000 rpm cranking
  then ramps to 3000 / 6000 / rated rpm).

Findings:
- Both decode cleanly: RPM within 0.2%, no trigger errors, no out-of-order sparks, one spark and one
  injection per revolution at every step.
- Acceleration tolerance is the same for both (probe, not kept): 1000 -> 6000 rpm in 5 revolutions
  (~90 ms) is clean; in 3 revolutions (~50 ms, ~100 000 rpm/s) both skip one dwell through the
  out-of-order guard. Instant steps of 3x speed also trip it on 36-1 - a test artifact.
- So the choice is mechanical (tooth size for the Hall sensor on a small wheel), not firmware.

Test harness notes:
- A wheel spinner must accumulate edge times exactly: rounding each half-tooth step to whole
  microseconds made 60-2 at 8500 rpm read 8618 rpm (59 -> 58 us).
- The rev limiter cuts fuel above `rpmHardLimit`, so "rated speed" checks must stay below it.

Validation:
- Unit tests: 1396 tests / 267 suites pass (GCC 13).

Open follow-ups:
- Switch the DLE presets from `TT_NARROW_SINGLE_TOOTH` to the chosen wheel once decided, and
  measure the missing-tooth-to-TDC angle on each engine.

## 2026-10-05 - DLE presets switched to a 36-1 crank wheel

What was done:
- `setDle60Twin()` / `setDle120Twin()`: trigger `TT_NARROW_SINGLE_TOOTH` -> `TT_TOOTHED_WHEEL_36_1`
  (two-stroke mode kept). `globalTriggerAngleOffset` placeholder changed 28 -> 90: the wheel is to be
  fitted so that the first tooth after the gap passes the sensor ~90 deg BTDC; must be measured with
  a timing light before the first start.
- `test_dle_twin.cpp`: single-pulse helper removed; the injector-duty tests now spin the preset's
  36-1 wheel; the wheel tests use the preset as-is for 36-1 and switch the trigger only for the 60-2
  alternative. The two single-pulse per-revolution tests were dropped (the wheel tests assert the
  same one spark and one injection per revolution at every speed step).

Sensor review (ZF GS100701, planned by the user):
- Single back-biased Hall, 5-24 V, open-collector sink output (needs a pull-up), ~1.5 mm typical
  air gap, 15/32-32 aluminium housing. Its recommended target is 2.5 mm teeth with 10 mm spaces and
  6.35 mm thickness, i.e. ~12.5 mm pitch -> a 36-1 wheel of ~143 mm diameter. A 50-70 mm wheel has a
  4.4-6.1 mm pitch (2.2-3.1 mm teeth and slots) - far below that guidance.
- Recommended instead: a differential (dual-element) gear-tooth sensor rated for small features,
  e.g. Honeywell SNDH-T (min 2.0 mm tooth and 2.0 mm slot, 0-2 mm air gap, 4.5-18 V, gear width
  > 5 mm recommended); SNG-Q has the same target figures. Differential sensors must be aligned with
  the direction of rotation.
- uaEFI wiring: open-collector Hall to C5 (HALL1, 4.7K pull-up, `MM100_IN_D1`) as
  `triggerInputPins[0]`; the board default is the MAX9924 VR2 input (C17), so the pin is set per
  installation, not by the engine preset.

Validation:
- Unit tests: 1394 tests / 267 suites pass (GCC 13; 1396 before minus the two dropped tests).
- Clang `-fsyntax-only` on the changed files: no diagnostics in them.

Open follow-ups:
- Wheel outside diameter (>= 60 mm keeps 36-1 teeth above the 2.0 mm minimum), steel rim >= 5 mm
  wide, sensor bracket; measure the trigger angle on each engine.

## 2026-10-05 - DLE trigger wheel size: 80 mm 36-1 for both engines

Decision: one 80 mm 36-1 wheel for both DLE-60 and DLE-120 (user). Only the adapter hub differs per
engine; the ring, sensor, air gap and rusEFI trigger settings are common. Spec recorded in the
`dle_twin.cpp` header comment and in hybrid_ctrl `docs/heli-hybrid-architecture.md`.

Check of the 80 mm wheel (36-1, 4 mm tooth height, 6 mm wide, 64 mm ring ID, steel):

| Item | Value | Limit / note |
|------|-------|--------------|
| Tooth pitch | 6.98 mm | |
| Tooth / gap width at OD | 3.49 mm | SNDH-T min 2.0 mm -> 1.75x margin |
| Missing-tooth gap | 10.5 mm | |
| Signal frequency | 180 Hz @ 300 rpm, 5.4 kHz @ 9000 rpm | SNDH-T 20 kHz max |
| Rim speed / hoop stress @ 9000 rpm | 37.7 m/s / 11 MPa | negligible for steel |
| Ring mass | ~62 g (+ aluminium adapter) | |
| Inertia | ~8e-5 kg m^2 | negligible vs. rotor |
| Missing-tooth imbalance | ~24 g mm -> ~21 N @ 9000 rpm | balance it |
| Balance hole (start value) | dia ~4.7 mm through, r = 29 mm, opposite the gap | trim by spin balancing |

- 60-2 at 80 mm would give 2.1 mm teeth - right at the sensor minimum, so 36-1 stays.
- The ZF GS100701 is still outside its recommended target at 80 mm (3.5 mm gaps vs 10 mm).
- Material: magnetic steel (1018 / C15 / S235, zinc or black-oxide finish); not 304 stainless.

Validation: comment-only firmware change; no rebuild needed beyond the earlier run (1394 pass).

Open follow-ups:
- Measure each engine's front stack (crankcase nose to drive hub, hub bolt pattern) for the two
  adapters, and the sensor bracket location at r = 41 mm from the shaft axis.
- Wheel runout <= 0.1 mm TIR so the ~1 mm air gap holds.

## 2026-10-05 - DLE trigger wheel tooth shape and sensor

Decisions (user): crank sensor Honeywell SNDH-T (dual differential Hall, min 2.0 mm tooth and slot);
narrow-tooth 36-1 wheel - 2.5 mm teeth, 4.5 mm gaps at the 80 mm OD, 5 mm tooth height - which also
gives the cheaper single-element candidates (ZF GS101205, Honeywell SNG-SPRC-001, OEM VW Hall crank
sensors) a better chance on the bench. Spec text updated in the `dle_twin.cpp` header comment;
the full wheel spec and sensor comparison live in hybrid_ctrl `docs/heli-hybrid-architecture.md`.

Check: the 36-1 decoder syncs on one edge, so tooth duty should not matter. Verified by temporarily
running the DLE wheel tests (`test_dle_twin.cpp`, WheelSpinner) with 36% tooth duty instead of 50%:
all 8 pass, same RPM accuracy and no trigger errors. The probe was not kept (the test file is
unchanged).

Validation: comment-only firmware change.

Open follow-ups:
- Bench the SNDH-T and GS101205 on the real wheel (scope at 0.5 / 1.0 / 1.5 mm air gap, cranking to
  9000 rpm).

## 2026-10-06 - DLE presets: rusEFI verbose CAN broadcast as the hybrid controller's engine status

Request (user): wire rusEFI's CAN status to the hybrid controller (HCU, r8d8/hybrid_ctrl), which
forwards it to ArduPilot as DroneCAN ICE status.

Decision: no new CAN frame. rusEFI's standard verbose broadcast (`can_verbose.cpp`,
`rusEFI_CAN_verbose.dbc`, 12 frames at base 0x200) already carries everything the ICE status can
use: RPM, timing, TPS1 (= the servo command in servo mode), MAP, CLT, IAT, EGT 1/2, lambda, injector
pulse, fuel used / flow, battery, warning counter, fuel/spark cut. Reusing it keeps the fork's CAN
format identical to upstream and the DBC valid. Not in the broadcast: baro, oil and fuel pressure,
spark dwell - the HCU sends those as unknown.

| File | Change |
|------|--------|
| `firmware/config/engines/dle_twin.cpp` | `setDleTwinCommon`: `canWriteEnabled`, `enableVerboseCanTx`, base `CAN_DEFAULT_BASE` (0x200), 11-bit, `canSleepPeriodMs` 50; header comment |
| `unit_tests/tests/test_dle_twin.cpp` | Preset expectations for the above; new `DleTwin.verboseCanLayoutForHybridController` |

The new test mocks sensors, sends one broadcast through `canTransmitMock` and pins the exact bytes
the HCU decodes (offsets, scales, signedness, +40 temperature offset, 11-bit IDs). The HCU's tests
(`firmware/tests/platform`, `fake_rusefi_broadcast()`) use the same values, so a layout change on
either side fails a test. uaEFI already enabled the broadcast in its board defaults; the preset
makes it board-independent.

Notes:
- Bus load on the power CAN (500 kbit/s): the broadcast adds 240 frames/s (~30 kbit/s) to the HCU's
  1 kHz VESC current commands and VESC status - ~40-55% total. rusEFI receives all of it; its CAN RX
  load on the F4 is a bench item.
- CLT on the air-cooled DLE is the cylinder head temperature sensor; the HCU reports it as such.
- `Sensor::getOrZero()` for CLT/IAT means a failed sensor reads 0 C on CAN; rusEFI's warning counter
  (reported by the HCU as a general engine error) is the signal for that.

Validation: unit tests 1395/1395 pass (GCC, clean build). Clang (`make CC=clang`): everything compiles, no
warnings in the changed files; the link fails in this container only because clang's ASan runtime
(`libclang_rt.asan*.a`) is not installed. HCU side: hybrid_ctrl `tools/check.sh` all green (16de0a9).

Open follow-ups:
- Bench: compare the HCU's `hcu ecu` with rusEFI's console; F4 CAN RX load with the VESC traffic.
- Baro in the broadcast once uaEFI has a live barometric sensor (would change the CAN format).

## 2026-10-06 - Hybrid controller HITL bench: nucleo_f429 HITL variant; servo throttle firmware build fix

Request (user): set up hardware-in-the-loop testing of the hybrid controller (HCU) with the parts on
hand - NUCLEO-H755 (HCU), NUCLEO-G474RE (plant emulator), NUCLEO-F439ZI, ArduPilot autopilot, Flipsky
75200 VESC. The F439 stands in for uaEFI (same STM32F4 family), so the real rusEFI firmware with the
DLE-60 preset is in the loop: crank decoding, servo throttle, tach, start/stop and the CAN broadcast.
Bench design and wiring: r8d8/hybrid_ctrl `docs/hitl.md`.

| File | Change |
|------|--------|
| `config/boards/nucleo_f429/meta-info-nucleo_f429_hitl.env` | New build variant `stm32f429_nucleo_hitl` (picked up by CI's matrix scan) |
| `config/boards/nucleo_f429/compile_stm32f429_nucleo_hitl.sh` | Build script for it |
| `config/boards/nucleo_f429/board.mk` | Variant: `HW_NUCLEO_F429_HITL`, default engine `DLE_60_TWIN`, own `FIRMWARE_ID`; plain build unchanged (MINIMAL_PINS) |
| `config/boards/nucleo_f429/board_configuration.cpp` | `custom_board_DefaultConfiguration` for the variant: HITL pins on the Zio header |
| `controllers/actuators/servo_throttle.cpp` | `engineConfiguration == nullptr` check only under `EFI_UNIT_TEST` |

HITL pins (Zio header, same positions as the HCU's Nucleo-144 signals): trigger PF13 (D7), PWM input
TPS PE11 (D5), servo throttle PE9 (D6, TIM1_CH1 hardware PWM), tach PE13 (D3), coil 1 PD15 (D9),
injector 1 PD14 (D10, unwired), start/stop button PF14 (D4, pull-down, the HCU drives it active high),
CAN1 TX PB9 / RX PB8 (D14/D15). None collide with the board's Ethernet, LED or VCP pins. Board
defaults are applied before the engine preset and the DLE preset sets no pins, so they survive.

Servo throttle build fix: since 7ac9aae3b12 `checkServoThrottleConfigError()` compared
`engineConfiguration` with nullptr. In firmware that name is the address of a global, so GCC 12+
rejects the comparison (`-Werror=address`) and every firmware build failed; unit tests, where it is a
pointer, compiled fine. Same idiom as `trigger_structure.cpp`: the check only exists under
`EFI_UNIT_TEST`.

Validation:
- No arm-none-eabi toolchain here (developer.arm.com blocked, rusefi/build_support not reachable).
  Compiled the HITL variant with the Zephyr SDK's GCC 14.3 behind an `arm-none-eabi-` shim: all 633
  objects compile except ChibiOS's newlib `syscalls.c` (that toolchain has picolibc, not newlib), so
  no link and no size figure. The servo throttle error showed up in that build, then passed after the
  fix. CI builds the variant with the real toolchain.
- `make -n` of the plain nucleo_f429 build: MINIMAL_PINS, FIRMWARE_ID nucleo_f429, no HITL flag.
- Unit tests 1395/1395 pass (GCC).

Open follow-ups:
- Confirm the variant (and every board) builds and links in CI with the official toolchain.
- On the bench: rusEFI syncs on the plant's 36-1 signal, servo pulse and coil output reach the plant,
  verbose CAN reaches the HCU at 500 kbit/s.

## 2026-10-06 - Servo throttle as a build option (EFI_SERVO_THROTTLE); F4 Hellen boards back under 1 MB

Problem: the first firmware CI run after the servo throttle build fix (401de58afa8) linked every
board except four 1 MB F4 Hellen boards with OpenBLT (736 KB application region):

| Board | Over by | Free at 241ea57 (last good) |
|-------|---------|-----------------------------|
| uaefi | 500 B | - |
| hellen121nissan | 1460 B | - |
| hellen-honda-k | 1720 B | - |
| uaefi121 | 4056 B | 628 B |

They were already over before; the servo throttle compile error since 7ac9aae3b12 hid it. The fork's
additions since upstream dcb730b10cc (PWM input TPS, servo throttle + governor + position model, DLE
presets, alpha-N baro, ~21 config fields) cost ~4.7 KB on uaefi121.

Decision (user, option 1): make the helicopter features a build option, on only where used, and trim
features a two-stroke helicopter does not use on uaEFI F4.

| File | Change |
|------|--------|
| `config/stm32f4ems/efifeatures.h` | `EFI_SERVO_THROTTLE` default FALSE (F7/H7 inherit); simulator and unit tests TRUE; kinetis/cypress FALSE |
| `controllers/actuators/servo_throttle.cpp`, `controllers/sensors/pwm_input_tps.cpp`, `config/engines/dle_twin.cpp` | bodies under `EFI_SERVO_THROTTLE` |
| `controllers/algo/engine.h` | `ServoThrottle` module entry gated |
| `controllers/engine_controller.cpp`, `controllers/core/error_handling.cpp`, `init/sensor/init_tps.cpp` | call sites gated |
| `controllers/algo/engine_type_impl.cpp` | DLE presets only with the feature; otherwise a clear firmwareError instead of "unexpected engine type" |
| `config/boards/hellen/uaefi/board.mk` | `EFI_SERVO_THROTTLE=TRUE` (all uaEFI variants); F4 only: `EFI_ALTERNATOR_CONTROL=FALSE`, `EFI_VVT_PID=FALSE` |
| `config/boards/nucleo_f429/board.mk` | HITL variant: `EFI_SERVO_THROTTLE=TRUE` |
| `config/boards/hellen/uaefi121/board.mk` | `EFI_ALTERNATOR_CONTROL=FALSE` (its margin was 628 B before the fork) |

The settings (pins, frequency, governor) stay in the configuration on every board, so the layout and
tunes are unchanged; without the feature they do nothing. Not a TS page owner, so no prepend.txt flag.

Sizes, non-LTO objects compiled with GCC 14.3 (no linker here): servo throttle 1457 B, PWM input TPS
831 B, DLE presets 1124 B, alternator control 1227 B, VVT control part of vvt.cpp ~500 B. uaEFI F4
text -2971 B and data -460 B against its 500 B overflow; uaefi121 -3412 B plus alternator against
4056 B (tight - CI decides); nissan and honda-k -3.4 KB against 1.5/1.7 KB.

Also checked: uaEFI F4 disables TIM1 PWM and TIM9 PWM is off on F4 by default, so the servo throttle
on uaEFI Coil 2 (PE5, TIM9 only) would fall back to software PWM. Use Coil 6 (PB8, TIM4).

Validation: unit tests 1395/1395 (feature on); uaefi (on, trimmed) and uaefi121 (off) compiled with
the GCC 14.3 shim: only the same newlib/picolibc-related objects fail as before, no new errors.
Linking and the final sizes come from CI.

Open follow-ups:
- Confirm all four boards link in CI; if uaefi121 is still over, the next candidate trim is chosen there.
