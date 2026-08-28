#pragma once
/**
 * @file ConfigDefaults_Generated.h
 * @brief Compile-time defaults for the Waveshare firmware.
 */

#ifndef FLOW_WIRDEF_WIFI_SSID
#define FLOW_WIRDEF_WIFI_SSID ""
#endif

#ifndef FLOW_WIRDEF_WIFI_PASS
#define FLOW_WIRDEF_WIFI_PASS ""
#endif

#ifndef FLOW_WIRDEF_MQ_EN
#define FLOW_WIRDEF_MQ_EN false
#endif

#ifndef FLOW_WIRDEF_MQ_HOST
#define FLOW_WIRDEF_MQ_HOST ""
#endif

#ifndef FLOW_WIRDEF_MQ_PORT
#define FLOW_WIRDEF_MQ_PORT Limits::Mqtt::Defaults::Port
#endif

#ifndef FLOW_WIRDEF_MQ_USER
#define FLOW_WIRDEF_MQ_USER ""
#endif

#ifndef FLOW_WIRDEF_MQ_PASS
#define FLOW_WIRDEF_MQ_PASS ""
#endif

#ifndef FLOW_WIRDEF_MQ_BASE
#define FLOW_WIRDEF_MQ_BASE "flowio"
#endif

#ifndef FLOW_WIRDEF_MQ_TID
#define FLOW_WIRDEF_MQ_TID ""
#endif

#ifndef FLOW_WIRDEF_HA_ENTITY_PREFIX
#define FLOW_WIRDEF_HA_ENTITY_PREFIX "fio"
#endif

#ifndef FLOW_WIRDEF_LOG_MQTT_LVL
#define FLOW_WIRDEF_LOG_MQTT_LVL 1
#endif

#ifndef FLOW_MODDEF_IO_ADS
#define FLOW_MODDEF_IO_ADS 125
#endif

#ifndef FLOW_MODDEF_IO_DS
#define FLOW_MODDEF_IO_DS 2000
#endif

#ifndef FLOW_MODDEF_IO_DIN
#define FLOW_MODDEF_IO_DIN 100
#endif

#ifndef FLOW_MODDEF_IO_AGAI
#define FLOW_MODDEF_IO_AGAI ADS1X15_GAIN_6144MV
#endif

#ifndef FLOW_MODDEF_IO_ARAT
#define FLOW_MODDEF_IO_ARAT 1
#endif

#ifndef FLOW_MODDEF_IO_TREN
#define FLOW_MODDEF_IO_TREN true
#endif

#ifndef FLOW_MODDEF_IO_TRMS
#define FLOW_MODDEF_IO_TRMS ((int32_t)Limits::IoTracePeriodMs)
#endif

#ifndef FLOW_WIRDEF_IO_EN
#define FLOW_WIRDEF_IO_EN true
#endif

#ifndef FLOW_WIRDEF_IO_SDA
#define FLOW_WIRDEF_IO_SDA 21
#endif

#ifndef FLOW_WIRDEF_IO_SCL
#define FLOW_WIRDEF_IO_SCL 22
#endif

#ifndef FLOW_WIRDEF_IO_AIAD
#define FLOW_WIRDEF_IO_AIAD 0x48u
#endif

#ifndef FLOW_WIRDEF_IO_AEAD
#define FLOW_WIRDEF_IO_AEAD 0x49u
#endif

#ifndef FLOW_WIRDEF_IO_A0S
#define FLOW_WIRDEF_IO_A0S IO_SRC_ADS_INTERNAL_SINGLE
#endif

#ifndef FLOW_WIRDEF_IO_A0C
#define FLOW_WIRDEF_IO_A0C 0u
#endif

#ifndef FLOW_WIRDEF_IO_A00
#define FLOW_WIRDEF_IO_A00 ((FLOW_WIRDEF_IO_A0S == IO_SRC_ADS_EXTERNAL_DIFF) ? Calib::Orp::ExternalC0 : Calib::Orp::InternalC0)
#endif

#ifndef FLOW_WIRDEF_IO_A01
#define FLOW_WIRDEF_IO_A01 ((FLOW_WIRDEF_IO_A0S == IO_SRC_ADS_EXTERNAL_DIFF) ? Calib::Orp::ExternalC1 : Calib::Orp::InternalC1)
#endif

#ifndef FLOW_WIRDEF_IO_A0P
#define FLOW_WIRDEF_IO_A0P 0
#endif

#ifndef FLOW_WIRDEF_IO_A0N
#define FLOW_WIRDEF_IO_A0N -32768.0f
#endif

#ifndef FLOW_WIRDEF_IO_A0X
#define FLOW_WIRDEF_IO_A0X 32767.0f
#endif

#ifndef FLOW_WIRDEF_IO_A1S
#define FLOW_WIRDEF_IO_A1S IO_SRC_ADS_INTERNAL_SINGLE
#endif

#ifndef FLOW_WIRDEF_IO_A1C
#define FLOW_WIRDEF_IO_A1C 1u
#endif

#ifndef FLOW_WIRDEF_IO_A10
#define FLOW_WIRDEF_IO_A10 ((FLOW_WIRDEF_IO_A1S == IO_SRC_ADS_EXTERNAL_DIFF) ? Calib::Ph::ExternalC0 : Calib::Ph::InternalC0)
#endif

#ifndef FLOW_WIRDEF_IO_A11
#define FLOW_WIRDEF_IO_A11 ((FLOW_WIRDEF_IO_A1S == IO_SRC_ADS_EXTERNAL_DIFF) ? Calib::Ph::ExternalC1 : Calib::Ph::InternalC1)
#endif

#ifndef FLOW_WIRDEF_IO_A1P
#define FLOW_WIRDEF_IO_A1P 1
#endif

#ifndef FLOW_WIRDEF_IO_A1N
#define FLOW_WIRDEF_IO_A1N -32768.0f
#endif

#ifndef FLOW_WIRDEF_IO_A1X
#define FLOW_WIRDEF_IO_A1X 32767.0f
#endif

#ifndef FLOW_WIRDEF_IO_A2S
#define FLOW_WIRDEF_IO_A2S IO_SRC_ADS_INTERNAL_SINGLE
#endif

#ifndef FLOW_WIRDEF_IO_A2C
#define FLOW_WIRDEF_IO_A2C 2u
#endif

#ifndef FLOW_WIRDEF_IO_A20
#define FLOW_WIRDEF_IO_A20 Calib::Psi::DefaultC0
#endif

#ifndef FLOW_WIRDEF_IO_A21
#define FLOW_WIRDEF_IO_A21 Calib::Psi::DefaultC1
#endif

#ifndef FLOW_WIRDEF_IO_A2P
#define FLOW_WIRDEF_IO_A2P 1
#endif

#ifndef FLOW_WIRDEF_IO_A2N
#define FLOW_WIRDEF_IO_A2N -32768.0f
#endif

#ifndef FLOW_WIRDEF_IO_A2X
#define FLOW_WIRDEF_IO_A2X 32767.0f
#endif

#ifndef FLOW_WIRDEF_IO_A3S
#define FLOW_WIRDEF_IO_A3S IO_SRC_ADS_INTERNAL_SINGLE
#endif

#ifndef FLOW_WIRDEF_IO_A3C
#define FLOW_WIRDEF_IO_A3C 3u
#endif

#ifndef FLOW_WIRDEF_IO_A30
#define FLOW_WIRDEF_IO_A30 1.0f
#endif

#ifndef FLOW_WIRDEF_IO_A31
#define FLOW_WIRDEF_IO_A31 0.0f
#endif

#ifndef FLOW_WIRDEF_IO_A3P
#define FLOW_WIRDEF_IO_A3P 3
#endif

#ifndef FLOW_WIRDEF_IO_A3N
#define FLOW_WIRDEF_IO_A3N -32768.0f
#endif

#ifndef FLOW_WIRDEF_IO_A3X
#define FLOW_WIRDEF_IO_A3X 32767.0f
#endif

#ifndef FLOW_WIRDEF_IO_A4S
#define FLOW_WIRDEF_IO_A4S IO_SRC_DS18_WATER
#endif

#ifndef FLOW_WIRDEF_IO_A4C
#define FLOW_WIRDEF_IO_A4C 0u
#endif

#ifndef FLOW_WIRDEF_IO_A40
#define FLOW_WIRDEF_IO_A40 1.0f
#endif

#ifndef FLOW_WIRDEF_IO_A41
#define FLOW_WIRDEF_IO_A41 0.0f
#endif

#ifndef FLOW_WIRDEF_IO_A4P
#define FLOW_WIRDEF_IO_A4P 1
#endif

#ifndef FLOW_WIRDEF_IO_A4N
#define FLOW_WIRDEF_IO_A4N Calib::Temperature::Ds18MinValidC
#endif

#ifndef FLOW_WIRDEF_IO_A4X
#define FLOW_WIRDEF_IO_A4X Calib::Temperature::Ds18MaxValidC
#endif

#ifndef FLOW_WIRDEF_IO_A5S
#define FLOW_WIRDEF_IO_A5S IO_SRC_DS18_AIR
#endif

#ifndef FLOW_WIRDEF_IO_A5C
#define FLOW_WIRDEF_IO_A5C 0u
#endif

#ifndef FLOW_WIRDEF_IO_A50
#define FLOW_WIRDEF_IO_A50 1.0f
#endif

#ifndef FLOW_WIRDEF_IO_A51
#define FLOW_WIRDEF_IO_A51 0.0f
#endif

#ifndef FLOW_WIRDEF_IO_A5P
#define FLOW_WIRDEF_IO_A5P 1
#endif

#ifndef FLOW_WIRDEF_IO_A5N
#define FLOW_WIRDEF_IO_A5N Calib::Temperature::Ds18MinValidC
#endif

#ifndef FLOW_WIRDEF_IO_A5X
#define FLOW_WIRDEF_IO_A5X Calib::Temperature::Ds18MaxValidC
#endif
