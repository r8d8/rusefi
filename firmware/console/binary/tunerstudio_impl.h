/**
 * @file tunerstudio_impl.h
 */

#pragma once

#include <cstdint>

class TsChannelBase;

typedef enum {
	TS_PLAIN = 0,
	TS_CRC = 1
} ts_response_format_e;

class TunerStudioBase {
public:

protected:
	virtual void cmdOutputChannels(TsChannelBase* tsChannel, uint16_t offset, uint16_t count) = 0;
};

class TunerStudio : public TunerStudioBase {
public:
	int handleCrcCommand(TsChannelBase* tsChannel, char *data, int incomingPacketSize);
	bool handlePlainCommand(TsChannelBase* tsChannel, uint8_t command);

	void cmdOutputChannels(TsChannelBase* tsChannel, uint16_t offset, uint16_t count) override;
	/**
	 * this command is part of protocol initialization
	 */
	void handleQueryCommand(TsChannelBase* tsChannel, ts_response_format_e mode);
	void handleExecuteCommand(TsChannelBase* tsChannel, char *data, int incomingPacketSize);
	// contentSize: the payload bytes the packet actually carries after its page / offset / count header
	void handleWriteChunkCommand(TsChannelBase* tsChannel, uint16_t page, uint16_t offset, uint16_t count,
			void *content, size_t contentSize);
	void handleCrc32Check(TsChannelBase *tsChannel,  uint32_t page, uint32_t offset, uint32_t count);
	void handleBurnCommand(TsChannelBase* tsChannel, uint16_t page);
	void handlePendingBurnTimeout(TsChannelBase* tsChannel);
	void handlePageReadCommand(TsChannelBase* tsChannel, uint32_t page, uint32_t offset, uint32_t count);
	void handleScatteredReadCommand(TsChannelBase* tsChannel);
	void handlePerfTraceCommand(TsChannelBase* tsChannel, char command);
	// a command this build was compiled without (perf trace, scatter)
	void refuseUnsupportedCommand(TsChannelBase* tsChannel, const char *what);

private:
	void finishPendingBurn(TsChannelBase* tsChannel);
	void sendErrorCode(TsChannelBase* tsChannel, uint8_t code, /*empty line by default, use nullptr not to log*/const char *msg="");
};

// true when [offset, offset + count) does not lie inside the page; 32-bit arithmetic, as on the MCU
bool validateOffsetCount(uint32_t page, uint32_t offset, uint32_t count);
