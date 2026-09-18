#include <cassert>
#include <cstdio>
#include "Modules/PoolDeviceModule/Drivers/PoolDriverConfig.h"
int main() {
    PoolDriverConfig c; char error[100];
    assert(parsePoolDriverConfig("{\"kind\":0,\"outputs\":[0]}",c,error,sizeof(error)));
    assert(parsePoolDriverConfig("{\"kind\":1,\"outputs\":[8,9,10],\"steps\":[30,60,100],\"startup\":60}",c,error,sizeof(error)));
    assert(!parsePoolDriverConfig("{\"kind\":1,\"outputs\":[8,9],\"steps\":[60,30]}",c,error,sizeof(error)));
    assert(!parsePoolDriverConfig("{\"kind\":1,\"outputs\":[8,8],\"steps\":[60,100]}",c,error,sizeof(error)));
    assert(!parsePoolDriverConfig("{\"kind\":0,\"outputs\":[-1]}",c,error,sizeof(error)));
    assert(!parsePoolDriverConfig("{\"kind\":3,\"serial\":{\"baud\":-1}}",c,error,sizeof(error)));
    assert(!parsePoolDriverConfig("{\"kind\":3,\"serial\":{\"run\":{\"function\":208}}}",c,error,sizeof(error)));
    assert(parsePoolDriverConfig("{\"kind\":3,\"serial\":{\"protocol\":1,\"baud\":19200,\"run\":{\"function\":208},\"setpoint\":{\"function\":208},\"status\":{\"function\":195},\"feedback\":{\"function\":195},\"has_feedback\":true}}",c,error,sizeof(error)));
    assert(parsePoolDriverConfig("{\"kind\":2,\"outputs\":[256],\"flow_curve\":[[0,0],[50,800],[100,2400]]}",c,error,sizeof(error)));
    assert(c.flowPointCount==3);
    puts("pool config tests passed");
}
