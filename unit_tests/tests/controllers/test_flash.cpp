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

// Coverage before a fix (TDB, HZ-29 audit): a forced write - writeToFlashNow(), which a TS burn of the
// Lua page or the second tables uses on INT_FLASH boards - goes ahead with the engine running. On an MCU
// that stalls during the erase (F4) engine control stops for the erase.
TEST_F(StorageBoardGate, ForcedWriteIgnoresRunningEngine) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	custom_board_allowFlashNow.reset();
	canFlashWhileRunning = false;
	engine->triggerCentral.directSelfStimulation = false;
	engine->rpmCalculator.setRpmValue(6000);

	// a normal write waits for the engine to stop
	EXPECT_FALSE(storageAllowWriteID(EFI_SETTINGS_RECORD_ID));
	// a forced one does not
	EXPECT_TRUE(storageAllowForcedWriteID(EFI_SETTINGS_RECORD_ID));
	EXPECT_TRUE(storageAllowForcedWriteID(EFI_SETTINGS_BACKUP_RECORD_ID));
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
