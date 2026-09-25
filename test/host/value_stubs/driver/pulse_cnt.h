#pragma once
#include <stdint.h>
#include "esp_err.h"
using pcnt_unit_handle_t = void*;
using pcnt_channel_handle_t = void*;
using pcnt_channel_edge_action_t = int;
constexpr int PCNT_CHANNEL_EDGE_ACTION_HOLD=0, PCNT_CHANNEL_EDGE_ACTION_INCREASE=1;
struct pcnt_unit_config_t { int low_limit=0, high_limit=0; };
struct pcnt_glitch_filter_config_t { uint32_t max_glitch_ns=0; };
struct pcnt_chan_config_t { int edge_gpio_num=0, level_gpio_num=0; };
inline int fakePcnt = 0;
inline int fakeStops = 0;
inline esp_err_t pcnt_new_unit(const pcnt_unit_config_t*, pcnt_unit_handle_t* u) { *u=&fakePcnt; return 0; }
inline esp_err_t pcnt_new_channel(pcnt_unit_handle_t, const pcnt_chan_config_t*, pcnt_channel_handle_t* c) { *c=&fakePcnt; return 0; }
inline esp_err_t pcnt_unit_set_glitch_filter(pcnt_unit_handle_t, const pcnt_glitch_filter_config_t*) { return 0; }
inline esp_err_t pcnt_channel_set_edge_action(pcnt_channel_handle_t, int, int) { return 0; }
inline esp_err_t pcnt_unit_enable(pcnt_unit_handle_t) { return 0; }
inline esp_err_t pcnt_unit_disable(pcnt_unit_handle_t) { return 0; }
inline esp_err_t pcnt_unit_start(pcnt_unit_handle_t) { return 0; }
inline esp_err_t pcnt_unit_stop(pcnt_unit_handle_t) { ++fakeStops; return 0; }
inline esp_err_t pcnt_unit_clear_count(pcnt_unit_handle_t) { fakePcnt=0; return 0; }
inline esp_err_t pcnt_unit_get_count(pcnt_unit_handle_t, int* c) { *c=fakePcnt; return 0; }
inline esp_err_t pcnt_del_channel(pcnt_channel_handle_t) { return 0; }
inline esp_err_t pcnt_del_unit(pcnt_unit_handle_t) { return 0; }
