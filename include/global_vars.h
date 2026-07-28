#ifndef GLOBAL_VARS_H
#define GLOBAL_VARS_H

//+------------------------------------------------------------------+
// PetFeeder IoT
// global_vars.h 
// v3.0: Agendamento diario recorrente por horario via NTP
//+------------------------------------------------------------------+

#include <stdint.h>
#include <stdbool.h>

//+------------------------------------------------------------------+
// Pinos de Hardware
//+------------------------------------------------------------------+
#define DEF_PIN_MOTOR_AUGER     18  // GPIO 18 -> Rele/MOSFET do motor (giro normal)
#define DEF_PIN_MOTOR_INVERSE    5  // GPIO  5 -> Rele/MOSFET do motor (inversao)

//+------------------------------------------------------------------+
// Sensor VL53L0X - Parametros de Calibracao
//
// Logica: reservatorio CHEIO = distancia PEQUENA (racao perto do sensor)
//         reservatorio VAZIO = distancia GRANDE  (sensor ve o fundo)
// Dispara alerta quando: ui_distance_mm > DEF_EMPTY_DISTANCE_MM
//+------------------------------------------------------------------+
#define DEF_EMPTY_DISTANCE_MM    80  // Distancia do sensor para reservatorio vazio (mm) 
#define DEF_RANGE_INVALID      8190  // Valor de saturacao do VL53L0X (sem alvo)
#define DEF_SENSOR_READ_MS    15000  // Intervalo de leitura do sensor (ms)
#define DEF_DEBUG_INTERVAL_MS 20000  // Intervalo do print de debug serial (ms)

//+------------------------------------------------------------------+
// Motor - Temporização
//+------------------------------------------------------------------+
#define DEF_AUGER_RUN_TIME_MS   3000  // Tempo de giro da rosca por dose (ms)

//+------------------------------------------------------------------+
// NTP - Sincronizacao de Horario pela Internet
//
// Necessario para o agendamento diario recorrente
// O ESP32 mantem o relogio internamente apos a primeira sincronizacao
// Fuso: UTC-3 (Brasilia)
//+------------------------------------------------------------------+
#define DEF_NTP_SERVER          "pool.ntp.org"
#define DEF_NTP_GMT_OFFSET_SEC  (-10800L)  // UTC-3 em segundos
#define DEF_NTP_DAYLIGHT_SEC    (0L)       // Sem horario de verao

//+------------------------------------------------------------------+
// Agenda Diaria Recorrente
//
// O firmware suporta ate DEF_MAX_SCHEDULE_SLOTS horarios simultaneos.
// Cada slot armazena hora/minuto e e disparado uma vez por dia.
// A meia-noite (00:00), todos os slots sao rearmados automaticamente.
//
// A agenda e persistida na memoria flash (NVS via Preferences) toda vez
// que muda, e restaurada automaticamente no boot (SalvarAgendaFlash /
// CarregarAgendaFlash em main.cpp) — sobrevive a queda de energia.
//
// Interface MQTT (topico ca_topic_feed_schedule):
//   Payload "HH:MM"   -> adiciona horario recorrente (ex: "06:00")
//   Payload "CLEAR"   -> apaga todos os horarios agendados
//   Payload "LIST"    -> imprime agenda atual no Monitor Serial
//   Payload "ON:n"    -> reativa o slot n sem apagar o horario
//   Payload "OFF:n"   -> desativa o slot n sem apagar o horario
//   Payload "DEL:n"   -> remove o slot n (libera para reuso)
//
// Toda mudanca na agenda (add/CLEAR/ON/OFF/DEL) e tambem a conexao com
// o broker disparam a publicacao do estado atual em ca_topic_agenda_status
// (retained), no formato "slot,HH:MM,ativo;slot,HH:MM,ativo;...".
//
// Intervalo de verificacao: a cada DEF_SCHEDULE_CHECK_MS o firmware
// consulta a hora atual e dispara os slots correspondentes.
// 10 segundos garante que nenhum slot seja perdido dentro do minuto.
//+------------------------------------------------------------------+
#define DEF_MAX_SCHEDULE_SLOTS  5      // Maximo de horarios simultaneos
#define DEF_SCHEDULE_CHECK_MS   10000  // Intervalo de verificacao da agenda (ms)

// Struct de um slot de horario agendado
typedef struct
{
    uint8_t u8_hora;           // Hora do disparo (0-23)
    uint8_t u8_minuto;         // Minuto do disparo (0-59)
    bool    b_ativo;           // true = slot ocupado com horario valido
    bool    b_disparadoHoje;   // true = ja disparou hoje (reset a meia-noite)
} SlotAgenda_t;

//+------------------------------------------------------------------+
// Topicos MQTT
//
// ca_topic_device_status usa o recurso de "Last Will and Testament" do
// MQTT: e o proprio broker (nao o firmware) quem publica "offline" nele
// se o ESP32 cair sem desconectar direito (queda de energia, WiFi cortado).
// O firmware so publica "online" apos conectar com sucesso.
//+------------------------------------------------------------------+
extern const char ca_topic_feed_cmd[];       // Despejo imediato
extern const char ca_topic_feed_schedule[];  // Gerenciar agenda (HH:MM / CLEAR / LIST / ON:n / OFF:n / DEL:n)
extern const char ca_topic_alert_empty[];    // Publicar alerta de vazio
extern const char ca_topic_agenda_status[];  // Publicar estado da agenda (retained)
extern const char ca_topic_device_status[];  // "online"/"offline" (retained, via LWT)

// Temporizadores nao-bloqueantes
extern uint32_t ul_prevMillis_Sensor;
extern uint32_t ul_prevMillis_Motor;
extern uint32_t ul_prevMillis_MQTT;
extern uint32_t ul_prevMillis_Debug;
extern uint32_t ul_prevMillis_Agenda;

// Sensor VL53L0X
extern uint16_t ui_distance_mm;  // Ultima distancia valida (mm)
extern bool     b_sensor_ok;     // true = sensor inicializou corretamente

// Motor
extern uint8_t u8_motor_state;   // Estado da MEF do motor (0=IDLE,1,2,3)

// NTP e Agenda
extern bool         b_ntp_sincronizado;                   // true apos 1a sincronizacao
extern SlotAgenda_t ast_agenda[DEF_MAX_SCHEDULE_SLOTS];   // Tabela de horarios

// Flags de eventos (volatile: escritas no callback MQTT, lidas no loop)
extern volatile bool b_feed_requested;
extern volatile bool b_schedule_active;   // Mantida para compatibilidade (nao usada na v3)
extern volatile bool b_is_empty_alert_sent;

#endif // GLOBAL_VARS_H
