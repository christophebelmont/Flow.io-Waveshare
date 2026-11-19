#include <assert.h>
#include <stdint.h>
#include "Modules/Network/ModbusTcpServerModule/ModbusTcpAdu.h"

int main() {
    using namespace ModbusTcpAdu;
    const uint8_t read24[] = {0x12,0x34,0,0,0,6,17,3,0x01,0xF4,0,24};
    Request request{};
    assert(expectedSize(read24, sizeof(read24)) == sizeof(read24));
    assert(decode(read24, sizeof(read24), request));
    assert(request.transactionId == 0x1234 && request.unitId == 17);
    assert(request.address == 500 && request.count == 24);
    const uint8_t read2[] = {0,1,0,0,0,6,17,3,3,0xE8,0,2};
    assert(decode(read2, sizeof(read2), request));
    assert(request.address == 1000 && request.count == 2);
    const uint8_t wrongProtocol[] = {0,1,0,1,0,6,17,3,0,0,0,1};
    assert(!decode(wrongProtocol, sizeof(wrongProtocol), request));
    const uint8_t wrongLength[] = {0,1,0,0,0,7,17,3,0,0,0,1};
    assert(!decode(wrongLength, sizeof(wrongLength), request));
    // FC03 and FC04 accept 125 registers, but not 126.
    const uint8_t readMax[] = {0,1,0,0,0,6,17,3,0,52,0,125};
    assert(decode(readMax, sizeof(readMax), request));
    assert(request.address == 52 && request.count == 125);
    const uint8_t readInputMax[] = {0,1,0,0,0,6,17,4,0,52,0,125};
    assert(decode(readInputMax, sizeof(readInputMax), request));
    const uint8_t invalidCount[] = {0,1,0,0,0,6,17,3,0,0,0,126};
    assert(!decode(invalidCount, sizeof(invalidCount), request));
    const uint8_t writeMultiple[] = {0,1,0,0,0,11,17,0x10,0,5,0,2,4,0,10,0,11};
    assert(decode(writeMultiple, sizeof(writeMultiple), request));
    assert(request.count == 2 && request.pduLength == 10);
    // FC16: 123 words => 246 data bytes and a 259-byte Modbus TCP ADU.
    uint8_t writeMax[259]{};
    writeMax[4] = 0;
    writeMax[5] = 253;
    writeMax[6] = 17;
    writeMax[7] = 0x10;
    writeMax[8] = 0;
    writeMax[9] = 52;
    writeMax[10] = 0;
    writeMax[11] = 123;
    writeMax[12] = 246;
    for (uint16_t i = 0; i < 123; ++i) {
        writeMax[13 + 2 * i] = uint8_t(i >> 8);
        writeMax[14 + 2 * i] = uint8_t(i);
    }
    assert(expectedSize(writeMax, sizeof(writeMax)) == sizeof(writeMax));
    assert(decode(writeMax, sizeof(writeMax), request));
    assert(request.address == 52 && request.count == 123 && request.pduLength == 252);
    // FC16 with 124 registers would exceed both protocol and ADU bounds.
    uint8_t oversizedWriteHeader[] = {0,1,0,0,0,255,17,0x10,0,52,0,124,248};
    assert(expectedSize(oversizedWriteHeader, sizeof(oversizedWriteHeader)) == 0);
    uint8_t output[16]{};
    assert(exception(0x1234, 17, 3, 0x0B, output, sizeof(output)) == 9);
    assert(output[0] == 0x12 && output[1] == 0x34 && output[4] == 0 && output[5] == 3);
    assert(output[6] == 17 && output[7] == 0x83 && output[8] == 0x0B);
    assert(exception(1, 17, 3, 0x0B, output, 8) == 0);
}
