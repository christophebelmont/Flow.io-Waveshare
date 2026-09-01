#include "Profiles/Waveshare/WaveshareProfile.h"

#include "Board/BoardCatalog.h"
#include "Board/BoardSpec.h"

namespace Profiles {
namespace Waveshare {


namespace {

int oneWirePinForSignal(const BoardSpec& board, BoardSignal signal, int fallbackPin)
{
    const OneWireBusSpec* spec = boardFindOneWire(board, signal);
    return spec ? spec->pin : fallbackPin;
}

}  // namespace

ModuleInstances::ModuleInstances(const BoardSpec& board)
    : ethernetModule(board),
      wifiModule(board),
      webInterfaceModule(board),
      firmwareUpdateModule(board),
#if defined(FLOW_ENABLE_TFT_S3) && (FLOW_ENABLE_TFT_S3 != 0)
      tftModuleS3(board),
#endif
      systemModule(board),
      hmiModule(board),
      hmiBuzzerModule(board),
      ioModule(board),
      oneWireWater(oneWirePinForSignal(board, BoardSignal::TempProbe1, 3)),
      oneWireAir(oneWirePinForSignal(board, BoardSignal::TempProbe2, 2))
{
}

ModuleInstances& moduleInstances()
{
    static ModuleInstances instances{BoardCatalog::activeBoard()};
    return instances;
}

}  // namespace Waveshare
}  // namespace Profiles
