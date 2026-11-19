#include <Arduino.h>

#include "App/Bootstrap.h"
#include "Core/SystemLimits.h"

SET_LOOP_TASK_STACK_SIZE(Limits::Core::Task::ArduinoLoopStackSize)

void setup()
{
    Serial.begin(115200);
    Bootstrap::run();
}

void loop()
{
    Bootstrap::loop();
}
