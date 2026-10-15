/*
 * @file test_flash.cpp
 *
 * @date: jul 15, 2025
 * @author FDSoftware
 */


#include "pch.h"
#include "storage.h"
#include "board_overrides.h"
#include "engine_test_helper.h"

bool canFlashWhileRunning = true;

bool mcuCanFlashWhileRunning() { return canFlashWhileRunning; }

TEST(Storage, AllowWriteID) {
    EngineTestHelper eth(engine_type_e::TEST_ENGINE);
    engine->rpmCalculator.setStopSpinning();

    // Settings record with MCU that can flash while running
    engine->triggerCentral.directSelfStimulation = false;
    // Mock that MCU can flash while running
    canFlashWhileRunning = true;
    EXPECT_TRUE(storageAllowWriteID(EFI_SETTINGS_RECORD_ID));

    // Settings record with MCU that cannot flash while running,
    // but engine is in self-stimulation mode
    canFlashWhileRunning = false;
    engine->triggerCentral.directSelfStimulation = true;
    EXPECT_TRUE(storageAllowWriteID(EFI_SETTINGS_RECORD_ID));

    // Settings record with MCU that cannot flash while running,
    // engine not in self-stimulation but stopped
    engine->triggerCentral.directSelfStimulation = false;
    engine->rpmCalculator.setStopSpinning();
    EXPECT_TRUE(storageAllowWriteID(EFI_SETTINGS_RECORD_ID));

    // Settings record with MCU that cannot flash while running,
    // engine running (should not allow write)
    engine->rpmCalculator.setRpmValue(1000);
    EXPECT_FALSE(storageAllowWriteID(EFI_SETTINGS_RECORD_ID));

    // Non-settings record (should always allow write)
    EXPECT_TRUE(storageAllowWriteID(EFI_LTFT_RECORD_ID));
    EXPECT_TRUE(storageAllowWriteID((StorageItemId)123)); // Some random ID
}

class StorageBoardGate : public testing::Test {
protected:
	std::optional<setup_custom_bool_type> savedGate;
	bool savedCanFlash;

	void SetUp() override {
		savedGate = custom_board_allowFlashNow;
		savedCanFlash = canFlashWhileRunning;
	}

	void TearDown() override {
		custom_board_allowFlashNow = savedGate;
		canFlashWhileRunning = savedCanFlash;
	}
};

TEST_F(StorageBoardGate, DefersAllRecordsWhileRunning) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	canFlashWhileRunning = false;
	custom_board_allowFlashNow = []() {
		return engine->triggerCentral.directSelfStimulation || engine->rpmCalculator.isStopped();
	};
	engine->triggerCentral.directSelfStimulation = false;
	engine->rpmCalculator.setRpmValue(1000);
	EXPECT_FALSE(storageAllowWriteID(EFI_SETTINGS_RECORD_ID));
	EXPECT_FALSE(storageAllowWriteID(EFI_SETTINGS_BACKUP_RECORD_ID));
	// Every MFS record must honor the board's flash gate.
	EXPECT_FALSE(storageAllowWriteID(EFI_LTFT_RECORD_ID));
	EXPECT_FALSE(storageAllowWriteID(static_cast<StorageItemId>(123)));

	engine->rpmCalculator.setStopSpinning();
	EXPECT_TRUE(storageAllowWriteID(EFI_LTFT_RECORD_ID));
	EXPECT_TRUE(storageAllowWriteID(EFI_SETTINGS_RECORD_ID));

	engine->rpmCalculator.setRpmValue(1000);
	engine->triggerCentral.directSelfStimulation = true;
	EXPECT_TRUE(storageAllowWriteID(EFI_LTFT_RECORD_ID));
	EXPECT_TRUE(storageAllowWriteID(EFI_SETTINGS_RECORD_ID));
}

// A forced write - writeToFlashNow(), which a TS burn of the Lua page or the second tables uses on
// INT_FLASH boards - waits for the engine to stop on an MCU that stalls during the erase (F4), like a
// normal write: it used to go ahead and stop engine control for the erase (HZ-29 audit).
TEST_F(StorageBoardGate, ForcedWriteWaitsForTheEngineToStop) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	custom_board_allowFlashNow.reset();
	canFlashWhileRunning = false;
	engine->triggerCentral.directSelfStimulation = false;
	engine->rpmCalculator.setRpmValue(6000);

	EXPECT_FALSE(storageAllowWriteID(EFI_SETTINGS_RECORD_ID));
	EXPECT_FALSE(storageAllowForcedWriteID(EFI_SETTINGS_RECORD_ID));
	EXPECT_FALSE(storageAllowForcedWriteID(EFI_SETTINGS_BACKUP_RECORD_ID));
	// records that never erase the internal settings sector: as before
	EXPECT_TRUE(storageAllowForcedWriteID(EFI_LTFT_RECORD_ID));

	// self-stimulation (no real engine, HW CI) and an MCU that keeps running while erasing: as before
	engine->triggerCentral.directSelfStimulation = true;
	EXPECT_TRUE(storageAllowForcedWriteID(EFI_SETTINGS_RECORD_ID));
	engine->triggerCentral.directSelfStimulation = false;
	canFlashWhileRunning = true;
	EXPECT_TRUE(storageAllowForcedWriteID(EFI_SETTINGS_RECORD_ID));
	canFlashWhileRunning = false;

	// stopped: it goes ahead, and still skips the board gate
	custom_board_allowFlashNow = []() { return false; };
	engine->rpmCalculator.setStopSpinning();
	EXPECT_FALSE(storageAllowWriteID(EFI_SETTINGS_RECORD_ID));
	EXPECT_TRUE(storageAllowForcedWriteID(EFI_SETTINGS_RECORD_ID));
}

TEST_F(StorageBoardGate, NoHookPreservesExternalStorageWrites) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	custom_board_allowFlashNow.reset();
	canFlashWhileRunning = false;
	engine->triggerCentral.directSelfStimulation = false;
	engine->rpmCalculator.setRpmValue(1000);
	EXPECT_FALSE(storageAllowWriteID(EFI_SETTINGS_RECORD_ID));
	EXPECT_TRUE(storageAllowWriteID(EFI_LTFT_RECORD_ID));

	canFlashWhileRunning = true;
	EXPECT_TRUE(storageAllowWriteID(EFI_SETTINGS_RECORD_ID));
}
