#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

//+------------------------------------------------------------------+
// Configuracoes de Conectividade
//+------------------------------------------------------------------+
extern const char     ca_wifi_ssid[];
extern const char     ca_wifi_pass[];
extern const char     ca_mqtt_server[];
extern const uint16_t ui_mqtt_port;
extern const char     ca_mqtt_client_id[];

#endif
