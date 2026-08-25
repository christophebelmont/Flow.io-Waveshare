#include <Arduino.h>

#include "App/Bootstrap.h"

void setup()
{
    Serial.begin(115200);
    Bootstrap::run();
}

void loop()
{
    Bootstrap::loop();
}
