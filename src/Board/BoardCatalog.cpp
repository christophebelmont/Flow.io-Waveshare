#include "Board/BoardCatalog.h"

#include "Board/WaveshareBoard.h"

namespace BoardCatalog {

const BoardSpec& activeBoard()
{
    return BoardProfiles::kWaveshareESP32S3;
}

}  // namespace BoardCatalog
