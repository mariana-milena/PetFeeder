//+------------------------------------------------------------------+
// config.cpp — Definicoes das credenciais e parametros de rede
//+------------------------------------------------------------------+

#include "config.h"

// Substituir os valores abaixo pelas credenciais
// reais da rede Wi-Fi e do broker MQTT antes de compilar.
const char     ca_wifi_ssid[]      = "IIOT";
const char     ca_wifi_pass[]      = "industria50";

const char     ca_mqtt_server[]    = "broker.hivemq.com";
const uint16_t ui_mqtt_port        = 1883;
const char     ca_mqtt_client_id[] = "ESP32_PetFeeder_Mecatronica";
