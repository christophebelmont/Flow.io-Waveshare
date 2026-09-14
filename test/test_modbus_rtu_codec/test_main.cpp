#include <unity.h>

#include "Modules/IOModule/IOProtocols/Modbus/ModbusRtuCodec.h"

void setUp() {}
void tearDown() {}

namespace {

void appendCrc(uint8_t* frame, size_t payloadLength)
{
    const uint16_t crc = ModbusRtuCodec::crc16(frame, payloadLength);
    frame[payloadLength] = (uint8_t)(crc & 0xFFU);
    frame[payloadLength + 1U] = (uint8_t)(crc >> 8U);
}

ModbusRequest holdingReadRequest()
{
    ModbusRequest request{};
    request.slaveAddress = 0x11U;
    request.function = MODBUS_FC_READ_HOLDING_REGISTERS;
    request.registerAddress = 0x03E8U;
    request.registerCount = 2U;
    return request;
}

} // namespace

void test_encodes_read_holding_registers_request()
{
    const ModbusRequest request = holdingReadRequest();
    uint8_t frame[8]{};
    size_t length = 0U;

    TEST_ASSERT_TRUE(ModbusRtuCodec::encodeRequest(request, frame, sizeof(frame), length));
    const uint8_t expected[] = {0x11U, 0x03U, 0x03U, 0xE8U,
                                0x00U, 0x02U, 0x46U, 0xEBU};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, sizeof(expected));
}

void test_rejects_output_buffer_that_is_too_small_without_writing()
{
    const ModbusRequest request = holdingReadRequest();
    uint8_t frame[4] = {0xA5U, 0xA5U, 0xA5U, 0xA5U};
    size_t length = 99U;

    TEST_ASSERT_FALSE(ModbusRtuCodec::encodeRequest(request, frame, sizeof(frame), length));
    const uint8_t expected[] = {0xA5U, 0xA5U, 0xA5U, 0xA5U};
    TEST_ASSERT_EQUAL_UINT32(0U, length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, sizeof(expected));
}

void test_decodes_read_register_values()
{
    const ModbusRequest request = holdingReadRequest();
    uint8_t frame[] = {0x11U, 0x03U, 0x04U, 0x12U, 0x34U, 0xABU, 0xCDU, 0U, 0U};
    appendCrc(frame, sizeof(frame) - 2U);
    ModbusResponse response{};

    TEST_ASSERT_EQUAL_UINT8(
        MODBUS_RESULT_OK,
        ModbusRtuCodec::decodeResponse(request, frame, sizeof(frame), response));
    TEST_ASSERT_EQUAL_UINT16(2U, response.registerCount);
    TEST_ASSERT_EQUAL_HEX16(0x1234U, response.values[0]);
    TEST_ASSERT_EQUAL_HEX16(0xABCDU, response.values[1]);
}

void test_decodes_modbus_exception()
{
    const ModbusRequest request = holdingReadRequest();
    uint8_t frame[] = {0x11U, 0x83U, 0x02U, 0U, 0U};
    appendCrc(frame, sizeof(frame) - 2U);
    ModbusResponse response{};

    TEST_ASSERT_EQUAL_UINT8(
        MODBUS_RESULT_EXCEPTION,
        ModbusRtuCodec::decodeResponse(request, frame, sizeof(frame), response));
    TEST_ASSERT_EQUAL_UINT8(0x02U, response.exceptionCode);
}

void test_rejects_invalid_crc()
{
    const ModbusRequest request = holdingReadRequest();
    const uint8_t frame[] = {0x11U, 0x03U, 0x04U, 0x12U, 0x34U,
                             0xABU, 0xCDU, 0x00U, 0x00U};
    ModbusResponse response{};

    TEST_ASSERT_EQUAL_UINT8(
        MODBUS_RESULT_CRC_ERROR,
        ModbusRtuCodec::decodeResponse(request, frame, sizeof(frame), response));
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_encodes_read_holding_registers_request);
    RUN_TEST(test_rejects_output_buffer_that_is_too_small_without_writing);
    RUN_TEST(test_decodes_read_register_values);
    RUN_TEST(test_decodes_modbus_exception);
    RUN_TEST(test_rejects_invalid_crc);
    return UNITY_END();
}
