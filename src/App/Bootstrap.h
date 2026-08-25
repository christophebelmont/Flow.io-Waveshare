#pragma once

#include "App/AppContext.h"
#include "App/FirmwareProfile.h"

namespace Bootstrap {

void run();
void loop();
AppContext& context();
const FirmwareProfile& activeProfile();

}  // namespace Bootstrap

