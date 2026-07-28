//+------------------------------------------------------------------+
// PetFeeder IoT - Firmware Principal
//+------------------------------------------------------------------+

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <time.h>
#include <string.h>
#include <PubSubClient.h>
#include <Adafruit_VL53L0X.h>
#include <Preferences.h>
#include "global_vars.h"
#include "config.h"

//+------------------------------------------------------------------+
// Instanciamento de Objetos
//+------------------------------------------------------------------+
WiFiClient        espClient;
PubSubClient      mqttClient(espClient);
Adafruit_VL53L0X  sensorLaser = Adafruit_VL53L0X();
Preferences       preferenciasAgenda;

//+------------------------------------------------------------------+
// Definicao e Inicializacao das Variaveis Globais
// (declaradas extern em global_vars.h)
//+------------------------------------------------------------------+

const char ca_topic_feed_cmd[]      = "petfeeder/cmd/imediato";
const char ca_topic_feed_schedule[] = "petfeeder/cmd/agendar";
const char ca_topic_alert_empty[]   = "petfeeder/alerta/vazio";
const char ca_topic_agenda_status[] = "petfeeder/status/agenda";
const char ca_topic_device_status[] = "petfeeder/status/dispositivo";

uint32_t ul_prevMillis_Sensor      = 0;
uint32_t ul_prevMillis_Motor       = 0;
uint32_t ul_prevMillis_MQTT        = 0;
uint32_t ul_prevMillis_Debug       = 0;
uint32_t ul_prevMillis_Agenda      = 0;

uint16_t ui_distance_mm            = 0;
bool     b_sensor_ok               = false;
uint8_t  u8_motor_state            = 0;

// NTP e agenda diaria
// ast_agenda e inicializado com zeros pelo C++ (armazenamento estatico):
// b_ativo = false em todos os slots pois agenda vazia ao ligar.
bool         b_ntp_sincronizado                  = false;
SlotAgenda_t ast_agenda[DEF_MAX_SCHEDULE_SLOTS]  = {0};

volatile bool b_feed_requested      = false;
volatile bool b_schedule_active     = false;  // mantida por compatibilidade
volatile bool b_is_empty_alert_sent = false;

//+------------------------------------------------------------------+
// Prototipos de Funcoes
//+------------------------------------------------------------------+
void ConectarWiFi();
void SincronizarNtp();
bool ObterHoraAtual(uint8_t *p_u8_hora, uint8_t *p_u8_minuto);
void ConectarMQTT();
void CallbackMQTT(char *p_topic, uint8_t *p_payload, unsigned int ui_length);
void ProcessarComandoAgenda(const char *pca_payload);
bool ExtrairHoraMinuto(const char *pca_texto, uint8_t *p_u8_hora, uint8_t *p_u8_minuto);
bool ExtrairComandoSlot(const char *pca_texto, const char *pca_prefixo, uint8_t *p_u8_slot);
void PublicarStatusAgenda();
void SalvarAgendaFlash();
void CarregarAgendaFlash();
void ImprimirAgenda();
void TratarMotorRosca();
void LerSensorDistancia();
void VerificarAgendaDiaria();
void DebugSensorSerial();

//+------------------------------------------------------------------+
// SETUP
//+------------------------------------------------------------------+
void setup()
{
    Serial.begin(115200);

    // Restaura a agenda salva antes de qualquer outra coisa, para que
    // ela ja esteja disponivel assim que o NTP sincronizar
    CarregarAgendaFlash();

    // Motores forcado para LOW para evitar acionamento
    // involuntario durante o boot
    pinMode(DEF_PIN_MOTOR_AUGER, OUTPUT);
    digitalWrite(DEF_PIN_MOTOR_AUGER, LOW);

    pinMode(DEF_PIN_MOTOR_INVERSE, OUTPUT);
    digitalWrite(DEF_PIN_MOTOR_INVERSE, LOW);

    // Inicializa barramento I2C nos pinos padrao ESP32 (SDA=21, SCL=22)
    Wire.begin();

    Serial.println("Inicializando sensor VL53L0X...");

    if (sensorLaser.begin() == false)
    {
        Serial.println("ERRO: Sensor VL53L0X nao encontrado.");
        Serial.println("Verifique o cabo I2C e a alimentacao 3.3V.");
        b_sensor_ok = false;
    }
    else
    {
        Serial.println("Sensor VL53L0X inicializado com sucesso!");
        b_sensor_ok = true;
    }

    // Nesta ordem, pois NTP precisa de WiFi
    ConectarWiFi();
    SincronizarNtp();

    mqttClient.setServer(ca_mqtt_server, ui_mqtt_port);
    mqttClient.setCallback(CallbackMQTT);
}

//+------------------------------------------------------------------+
// LOOP PRINCIPAL
//+------------------------------------------------------------------+
void loop()
{
    if (mqttClient.connected() == false)
    {
        ConectarMQTT();
    }

    mqttClient.loop();

    TratarMotorRosca();
    LerSensorDistancia();
    VerificarAgendaDiaria();
    DebugSensorSerial();
}

//+------------------------------------------------------------------+
// Conexao Wi-Fi com Timeout
// Unico ponto bloqueante legitimo, pois sem WiFi nao ha IoT possivel.
// Apos 30 tentativas (15s) reinicia o MCU automaticamente.
//+------------------------------------------------------------------+
void ConectarWiFi()
{
    const uint8_t u8_maxTentativas = 30;
    uint8_t       u8_tentativas    = 0;

    Serial.print("Conectando ao WiFi: ");
    Serial.println(ca_wifi_ssid);

    WiFi.begin(ca_wifi_ssid, ca_wifi_pass);

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
        u8_tentativas++;

        if (u8_tentativas >= u8_maxTentativas)
        {
            Serial.println("");
            Serial.println("ERRO: Timeout de conexao Wi-Fi. Reiniciando...");
            delay(1000);
            ESP.restart();
        }
    }

    Serial.println("");
    Serial.println("=================================");
    Serial.print("WiFi conectado! IP: ");
    Serial.println(WiFi.localIP());
    Serial.println("=================================");
}

//+------------------------------------------------------------------+
// Sincronizacao NTP (chamada uma vez no setup, apos WiFi conectado)
//+------------------------------------------------------------------+
void SincronizarNtp()
{
    struct tm st_tempo;
    uint8_t   u8_tentativas = 0;

    Serial.print("Sincronizando NTP...");
    configTime(DEF_NTP_GMT_OFFSET_SEC, DEF_NTP_DAYLIGHT_SEC, DEF_NTP_SERVER);

    // Aguarda ate 5 segundos pela 1a resposta NTP (10 tentativas x 500ms)
    // Este e o unico ponto onde aceitamos um pequeno bloqueio na inicializacao
    while ((u8_tentativas < 10) && (b_ntp_sincronizado == false))
    {
        if (getLocalTime(&st_tempo, 500U))
        {
            b_ntp_sincronizado = true;
        }
        else
        {
            Serial.print(".");
            u8_tentativas++;
        }
    }

    if (b_ntp_sincronizado == true)
    {
        Serial.println("");
        Serial.print("NTP OK! Hora atual: ");
        Serial.printf("%02d:%02d:%02d (UTC-3)\n",
                      st_tempo.tm_hour, st_tempo.tm_min, st_tempo.tm_sec);
    }
    else
    {
        Serial.println("");
        Serial.println("AVISO: NTP nao sincronizado. Agenda diaria desabilitada.");
        Serial.println("       Despejo imediato e sensor continuam funcionando.");
    }
}

//+------------------------------------------------------------------+
// Obtem Hora e Minuto Atuais via Relogio Interno do ESP32
// Retorna true se a leitura foi bem-sucedida, false caso contrario.
//+------------------------------------------------------------------+
bool ObterHoraAtual(uint8_t *p_u8_hora, uint8_t *p_u8_minuto)
{
    struct tm st_tempo;
    bool      b_resultado = false;

    if (getLocalTime(&st_tempo, 10U))
    {
        *p_u8_hora    = (uint8_t)st_tempo.tm_hour;
        *p_u8_minuto  = (uint8_t)st_tempo.tm_min;
        b_resultado   = true;
    }

    return b_resultado;
}

//+------------------------------------------------------------------+
// Reconexao MQTT Nao-Bloqueante (tenta a cada 5 segundos)
//
// O connect() registra um Last Will: se este ESP32 cair sem desconectar
// direito (queda de energia, WiFi cortado), o proprio broker publica
// "offline" em ca_topic_device_status em nome dele. Por isso
// o app consegue detectar uma queda mesmo sem o firmware fazer nada.
//+------------------------------------------------------------------+
void ConectarMQTT()
{
    const uint8_t u8_willQos    = 0;
    const bool    b_willRetain  = true;

    if ((uint32_t)(millis() - ul_prevMillis_MQTT) >= 5000)
    {
        ul_prevMillis_MQTT = millis();
        Serial.println("Tentando conexao MQTT... ");

        if (mqttClient.connect(ca_mqtt_client_id, ca_topic_device_status,
                                u8_willQos, b_willRetain, "offline"))
        {
            Serial.println("Conectado ao Broker MQTT!");
            mqttClient.subscribe(ca_topic_feed_cmd);
            mqttClient.subscribe(ca_topic_feed_schedule);

            // Confirma que esta conexao especifica esta online
            mqttClient.publish(ca_topic_device_status, "online", true);

            // Publica o estado atual da agenda para quem estiver ouvindo
            // (ex: o app) receber a lista real assim que conectar
            PublicarStatusAgenda();
        }
        else
        {
            Serial.print("Falhou, rc=");
            Serial.println(mqttClient.state());
        }
    }
}

//+------------------------------------------------------------------+
// Callback MQTT: Despacha mensagens recebidas para os handlers
//+------------------------------------------------------------------+
void CallbackMQTT(char *p_topic, uint8_t *p_payload, unsigned int ui_length)
{
    char     ca_mensagem[16] = {0};
    uint32_t ui_i            = 0;

    if (ui_length > 15)
    {
        Serial.println("AVISO: Payload MQTT truncado para 15 bytes.");
    }

    for (ui_i = 0; ui_i < ui_length; ui_i++)
    {
        if (ui_i < 15)
        {
            ca_mensagem[ui_i] = (char)p_payload[ui_i];
        }
    }

    Serial.print("[MQTT RECEBIDO] Topico: ");
    Serial.print(p_topic);
    Serial.print(" | Payload: ");
    Serial.println(ca_mensagem);

    if (strcmp(p_topic, ca_topic_feed_cmd) == 0)
    {
        b_feed_requested = true;
    }
    else if (strcmp(p_topic, ca_topic_feed_schedule) == 0)
    {
        // Delega o processamento para a funcao especializada
        ProcessarComandoAgenda(ca_mensagem);
    }
}

//+------------------------------------------------------------------+
// Processamento do Comando de Agenda (MQTT)
//
// Payloads aceitos
//   "HH:MM": Adiciona horario recorrente (ex: "06:00", "12:00")
//              O minuto do payload e o minuto exato de disparo.
//   "CLEAR": Remove todos os horarios agendados.
//   "LIST":Imprime a agenda atual no Monitor Serial (sem alterar nada).
//
// Slots sao alocados sequencialmente. Se todos estiverem ocupados,
// o slot 0 e sobrescrito (comportamento documentado e previsivel).
// Para liberar um slot especifico, usar "CLEAR" e reconfigure os demais.
//+------------------------------------------------------------------+
void ProcessarComandoAgenda(const char *pca_payload)
{
    uint8_t u8_hora           = 0;
    uint8_t u8_minuto         = 0;
    uint8_t u8_slotEscolhido  = 0;
    uint8_t u8_slotAlvo       = 0;
    uint8_t u8_i              = 0;
    bool    b_slotEncontrado  = false;

    // Comando: limpar agenda
    if (strcmp(pca_payload, "CLEAR") == 0)
    {
        for (u8_i = 0; u8_i < DEF_MAX_SCHEDULE_SLOTS; u8_i++)
        {
            ast_agenda[u8_i].b_ativo         = false;
            ast_agenda[u8_i].b_disparadoHoje = false;
            ast_agenda[u8_i].u8_hora         = 0;
            ast_agenda[u8_i].u8_minuto       = 0;
        }
        Serial.println("Agenda: todos os horarios foram removidos.");
        SalvarAgendaFlash();
        PublicarStatusAgenda();
        return;
    }

    // Comando: listar agenda (debug)
    if (strcmp(pca_payload, "LIST") == 0)
    {
        ImprimirAgenda();
        return;
    }

    // Comando: reativar um slot existente sem apagar o horario
    if (ExtrairComandoSlot(pca_payload, "ON:", &u8_slotAlvo) == true)
    {
        ast_agenda[u8_slotAlvo].b_ativo = true;
        Serial.print("Agenda: slot ");
        Serial.print(u8_slotAlvo);
        Serial.println(" reativado.");
        ImprimirAgenda();
        SalvarAgendaFlash();
        PublicarStatusAgenda();
        return;
    }

    // Comando: desativar um slot sem apagar o horario (pausado)
    if (ExtrairComandoSlot(pca_payload, "OFF:", &u8_slotAlvo) == true)
    {
        ast_agenda[u8_slotAlvo].b_ativo = false;
        Serial.print("Agenda: slot ");
        Serial.print(u8_slotAlvo);
        Serial.println(" desativado.");
        ImprimirAgenda();
        SalvarAgendaFlash();
        PublicarStatusAgenda();
        return;
    }

    // Comando: remover um slot especifico (libera para reuso)
    if (ExtrairComandoSlot(pca_payload, "DEL:", &u8_slotAlvo) == true)
    {
        ast_agenda[u8_slotAlvo].b_ativo         = false;
        ast_agenda[u8_slotAlvo].b_disparadoHoje = false;
        ast_agenda[u8_slotAlvo].u8_hora         = 0;
        ast_agenda[u8_slotAlvo].u8_minuto       = 0;
        Serial.print("Agenda: slot ");
        Serial.print(u8_slotAlvo);
        Serial.println(" removido.");
        ImprimirAgenda();
        SalvarAgendaFlash();
        PublicarStatusAgenda();
        return;
    }

    // Comando: adicionar horario "HH:MM"
    if (ExtrairHoraMinuto(pca_payload, &u8_hora, &u8_minuto) == false)
    {
        Serial.println("AVISO: Formato invalido! Use HH:MM, CLEAR, LIST, ON:n, OFF:n ou DEL:n.");
        return;
    }

    // Procura primeiro slot livre
    for (u8_i = 0; u8_i < DEF_MAX_SCHEDULE_SLOTS; u8_i++)
    {
        if (ast_agenda[u8_i].b_ativo == false)
        {
            u8_slotEscolhido = u8_i;
            b_slotEncontrado = true;
            break;
        }
    }

    // Se a agenda estiver cheia, sobrescreve o slot 0 (comportamento documentado)
    if (b_slotEncontrado == false)
    {
        u8_slotEscolhido = 0;
        Serial.println("AVISO: Agenda cheia! Sobrescrevendo slot 0...");
    }

    ast_agenda[u8_slotEscolhido].u8_hora          = u8_hora;
    ast_agenda[u8_slotEscolhido].u8_minuto        = u8_minuto;
    ast_agenda[u8_slotEscolhido].b_ativo          = true;
    ast_agenda[u8_slotEscolhido].b_disparadoHoje  = false;

    Serial.print("Agenda: horario ");
    Serial.printf("%02d:%02d", u8_hora, u8_minuto);
    Serial.print(" adicionado no slot ");
    Serial.print(u8_slotEscolhido);
    Serial.println(". Repete todo dia.");

    ImprimirAgenda();
    SalvarAgendaFlash();
    PublicarStatusAgenda();
}

//+------------------------------------------------------------------+
// Parser do Formato "HH:MM"
//
// Regras de validacao:
//   - Comprimento exato de 5 caracteres
//   - Posicao 2 deve ser ':'
//   - Posicoes 0,1,3,4 devem ser digitos ASCII
//   - Hora resultante: 0-23
//   - Minuto resultante: 0-59
//+------------------------------------------------------------------+
bool ExtrairHoraMinuto(const char *pca_texto, uint8_t *p_u8_hora, uint8_t *p_u8_minuto)
{
    bool b_horarioValido = false;

    if (strlen(pca_texto) == 5U)
    {
        if ((pca_texto[2] == ':')                          &&
            (pca_texto[0] >= '0') && (pca_texto[0] <= '9') &&
            (pca_texto[1] >= '0') && (pca_texto[1] <= '9') &&
            (pca_texto[3] >= '0') && (pca_texto[3] <= '9') &&
            (pca_texto[4] >= '0') && (pca_texto[4] <= '9'))
        {
            *p_u8_hora   = (uint8_t)(((pca_texto[0] - '0') * 10) + (pca_texto[1] - '0'));
            *p_u8_minuto = (uint8_t)(((pca_texto[3] - '0') * 10) + (pca_texto[4] - '0'));

            if ((*p_u8_hora <= 23U) && (*p_u8_minuto <= 59U))
            {
                b_horarioValido = true;
            }
        }
    }

    return b_horarioValido;
}

//+------------------------------------------------------------------+
// Parser dos Comandos "ON:n", "OFF:n" e "DEL:n"
//+------------------------------------------------------------------+
bool ExtrairComandoSlot(const char *pca_texto, const char *pca_prefixo, uint8_t *p_u8_slot)
{
    uint8_t u8_tamPrefixo = (uint8_t)strlen(pca_prefixo);
    bool    b_valido      = false;

    if (strlen(pca_texto) == (size_t)(u8_tamPrefixo + 1U))
    {
        if ((strncmp(pca_texto, pca_prefixo, u8_tamPrefixo) == 0)      &&
            (pca_texto[u8_tamPrefixo] >= '0') && (pca_texto[u8_tamPrefixo] <= '9'))
        {
            *p_u8_slot = (uint8_t)(pca_texto[u8_tamPrefixo] - '0');

            if (*p_u8_slot < DEF_MAX_SCHEDULE_SLOTS)
            {
                b_valido = true;
            }
        }
    }

    return b_valido;
}

//+------------------------------------------------------------------+
// Publica o Estado Atual da Agenda (MQTT, retained)
//
// Formato: "slot,HH:MM,ativo;slot,HH:MM,ativo;..." para os
// DEF_MAX_SCHEDULE_SLOTS slots, nesta ordem.
//+------------------------------------------------------------------+
void PublicarStatusAgenda()
{
    char    ca_status[128] = {0};
    char    ca_slot[24]    = {0};
    uint8_t u8_i           = 0;
    uint8_t u8_ativo       = 0;

    for (u8_i = 0; u8_i < DEF_MAX_SCHEDULE_SLOTS; u8_i++)
    {
        u8_ativo = (ast_agenda[u8_i].b_ativo == true) ? 1U : 0U;

        if ((ast_agenda[u8_i].u8_hora == 0U)   &&
            (ast_agenda[u8_i].u8_minuto == 0U) &&
            (ast_agenda[u8_i].b_ativo == false))
        {
            snprintf(ca_slot, sizeof(ca_slot), "%d,,%d;", u8_i, u8_ativo);
        }
        else
        {
            snprintf(ca_slot, sizeof(ca_slot), "%d,%02d:%02d,%d;",
                      u8_i, ast_agenda[u8_i].u8_hora, ast_agenda[u8_i].u8_minuto, u8_ativo);
        }

        strncat(ca_status, ca_slot, sizeof(ca_status) - strlen(ca_status) - 1U);
    }

    mqttClient.publish(ca_topic_agenda_status, ca_status, true);

    Serial.print("[MQTT ENVIADO] ");
    Serial.print(ca_topic_agenda_status);
    Serial.print(" | ");
    Serial.println(ca_status);
}

//+------------------------------------------------------------------+
// Grava a Agenda Inteira na Memoria Flash (NVS via Preferences)
//+------------------------------------------------------------------+
void SalvarAgendaFlash()
{
    size_t ui_gravado = 0;

    preferenciasAgenda.begin("agenda", false);
    ui_gravado = preferenciasAgenda.putBytes("slots", ast_agenda, sizeof(ast_agenda));
    preferenciasAgenda.end();

    if (ui_gravado == sizeof(ast_agenda))
    {
        Serial.println("[Flash] Agenda gravada com sucesso.");
    }
    else
    {
        Serial.print("[Flash] ERRO ao gravar agenda! putBytes retornou ");
        Serial.print(ui_gravado);
        Serial.print(" de ");
        Serial.print(sizeof(ast_agenda));
        Serial.println(" bytes esperados.");
    }
}

//+------------------------------------------------------------------+
// Restaura a Agenda da Memoria Flash (chamada uma vez no setup)
//+------------------------------------------------------------------+
void CarregarAgendaFlash()
{
    uint8_t u8_i         = 0;
    size_t  ui_tamanho   = 0;

    preferenciasAgenda.begin("agenda", true);
    ui_tamanho = preferenciasAgenda.getBytesLength("slots");

    Serial.print("[Flash] Chave 'slots' tem ");
    Serial.print(ui_tamanho);
    Serial.print(" bytes salvos (esperado: ");
    Serial.print(sizeof(ast_agenda));
    Serial.println(" bytes).");

    if (ui_tamanho == sizeof(ast_agenda))
    {
        preferenciasAgenda.getBytes("slots", ast_agenda, sizeof(ast_agenda));

        for (u8_i = 0; u8_i < DEF_MAX_SCHEDULE_SLOTS; u8_i++)
        {
            ast_agenda[u8_i].b_disparadoHoje = false;
        }

        Serial.println("[Flash] Agenda restaurada com sucesso.");
        ImprimirAgenda();
    }
    else
    {
        Serial.println("[Flash] Nenhum horario salvo encontrado (ou 1a inicializacao).");
    }

    preferenciasAgenda.end();
}

//+------------------------------------------------------------------+
// Imprime a Agenda Atual no Monitor Serial
// Util para debug e confirmacao visual dos horarios configurados.
//+------------------------------------------------------------------+
void ImprimirAgenda()
{
    uint8_t u8_i        = 0;
    uint8_t u8_ativos   = 0;

    Serial.println("=== Agenda atual ===");

    for (u8_i = 0; u8_i < DEF_MAX_SCHEDULE_SLOTS; u8_i++)
    {
        if (ast_agenda[u8_i].b_ativo == true)
        {
            Serial.printf("  Slot %d: %02d:%02d",
                          u8_i,
                          ast_agenda[u8_i].u8_hora,
                          ast_agenda[u8_i].u8_minuto);

            if (ast_agenda[u8_i].b_disparadoHoje == true)
            {
                Serial.print(" [Despejo ja realizado hoje!]");
            }
            Serial.println("");
            u8_ativos++;
        }
    }

    if (u8_ativos == 0)
    {
        Serial.println("  (vazia)");
    }

    Serial.println("====================");
}

//+------------------------------------------------------------------+
// Maquina de Estados do Motor ligado a Rosca Sem-Fim
//
// Estado 0 (IDLE)     : Parado, aguardando b_feed_requested
// Estado 1 (LIGAR)    : Aciona pino, registra tempo de inicio
// Estado 2 (AGUARDAR) : Aguarda DEF_AUGER_RUN_TIME_MS sem bloquear
// Estado 3 (DESLIGAR) : Desaciona pino, volta ao estado IDLE
//+------------------------------------------------------------------+
void TratarMotorRosca()
{
    if ((b_feed_requested == true) && (u8_motor_state == 0))
    {
        u8_motor_state   = 1;
        b_feed_requested = false;
        Serial.println("[Motor] Iniciando despejo...");
    }

    if (u8_motor_state == 1)
    {
        digitalWrite(DEF_PIN_MOTOR_AUGER, HIGH);
        ul_prevMillis_Motor = millis();
        u8_motor_state      = 2;
    }
    else if (u8_motor_state == 2)
    {
        if ((uint32_t)(millis() - ul_prevMillis_Motor) >= DEF_AUGER_RUN_TIME_MS)
        {
            u8_motor_state = 3;
        }
    }
    else if (u8_motor_state == 3)
    {
        digitalWrite(DEF_PIN_MOTOR_AUGER, LOW);
        u8_motor_state = 0;
        Serial.println("[Motor] Despejo concluido :D");
    }
}

//+------------------------------------------------------------------+
// Leitura do Sensor VL53L0X e Alerta de Nivel Baixo
// Executa a cada DEF_SENSOR_READ_MS (nao-bloqueante).
// Leituras invalidas sao ignoradas.
//+------------------------------------------------------------------+
void LerSensorDistancia()
{
    VL53L0X_RangingMeasurementData_t st_medicao;

    if (b_sensor_ok == false)
    {
        return;
    }

    if ((uint32_t)(millis() - ul_prevMillis_Sensor) >= DEF_SENSOR_READ_MS)
    {
        ul_prevMillis_Sensor = millis();

        sensorLaser.rangingTest(&st_medicao, false);

        if ((st_medicao.RangeStatus == 0) &&
            (st_medicao.RangeMilliMeter < DEF_RANGE_INVALID))
        {
            ui_distance_mm = (uint16_t)st_medicao.RangeMilliMeter;

            if (ui_distance_mm > DEF_EMPTY_DISTANCE_MM)
            {
                if (b_is_empty_alert_sent == false)
                {
                    mqttClient.publish(ca_topic_alert_empty, "ALERTA: Recipiente vazio");
                    b_is_empty_alert_sent = true;

                    Serial.print("[MQTT ENVIADO] ");
                    Serial.print(ca_topic_alert_empty);
                    Serial.println(" | ALERTA: Recipiente vazio");
                }
            }
            else
            {
                b_is_empty_alert_sent = false;
            }
        }
    }
}

//+------------------------------------------------------------------+
// Verificacao da Agenda Diaria Recorrente (Nao-Bloqueante)
//
// Executa a cada DEF_SCHEDULE_CHECK_MS (10 s), garantindo que
// nenhum slot seja perdido dentro de uma janela de 1 minuto.
//
// Logica de reset de meia-noite:
//   Ao detectar hora=0 e minuto=0 pela primeira vez, rearm todos os
//   b_disparadoHoje para false. A flag estatica s_b_resetFeito
//   garante que o reset ocorra uma unica vez por meia-noite.
//
// Requer b_ntp_sincronizado == true para operar.
//+------------------------------------------------------------------+
void VerificarAgendaDiaria()
{
    static bool s_b_resetFeito = false;

    uint8_t u8_horaAtual   = 0;
    uint8_t u8_minutoAtual = 0;
    uint8_t u8_i           = 0;

    if (b_ntp_sincronizado == false)
    {
        return; // Sem hora valida a agenda nao pode operar
    }

    if ((uint32_t)(millis() - ul_prevMillis_Agenda) < DEF_SCHEDULE_CHECK_MS)
    {
        return; // Ainda nao e hora de verificar
    }
    ul_prevMillis_Agenda = millis();

    if (ObterHoraAtual(&u8_horaAtual, &u8_minutoAtual) == false)
    {
        return; // Falha ao ler relogio interno
    }

    //+--- Reset de meia-noite ---+
    // Rearma todos os slots para disparar novamente no novo dia
    if ((u8_horaAtual == 0U) && (u8_minutoAtual == 0U))
    {
        if (s_b_resetFeito == false)
        {
            for (u8_i = 0; u8_i < DEF_MAX_SCHEDULE_SLOTS; u8_i++)
            {
                ast_agenda[u8_i].b_disparadoHoje = false;
            }
            s_b_resetFeito = true;
            Serial.println("[Agenda] Slots rearmados para o novo dia!");
        }
    }
    else
    {
        s_b_resetFeito = false;
    }

    //+--- Verifica cada slot ---+
    for (u8_i = 0; u8_i < DEF_MAX_SCHEDULE_SLOTS; u8_i++)
    {
        if ((ast_agenda[u8_i].b_ativo == true)           &&
            (ast_agenda[u8_i].b_disparadoHoje == false)  &&
            (ast_agenda[u8_i].u8_hora == u8_horaAtual)   &&
            (ast_agenda[u8_i].u8_minuto == u8_minutoAtual))
        {
            b_feed_requested                   = true;  // Aciona o motor
            ast_agenda[u8_i].b_disparadoHoje   = true;  // Nao dispara de novo hoje

            Serial.println("[Agenda] Despejo automatico disparado em: ");
            Serial.printf("%02d:%02d (slot %d)\n",
                          u8_horaAtual, u8_minutoAtual, u8_i);
        }
    }
}

//+------------------------------------------------------------------+
// Debug Serial Periodico
// Imprime distancia a cada DEF_DEBUG_INTERVAL_MS
//+------------------------------------------------------------------+
void DebugSensorSerial()
{
    if ((uint32_t)(millis() - ul_prevMillis_Debug) >= DEF_DEBUG_INTERVAL_MS)
    {
        ul_prevMillis_Debug = millis();

        if (b_sensor_ok == true)
        {
            Serial.print("[Debug VL53L0X] Distancia medida: ");
            Serial.print(ui_distance_mm);
            Serial.println(" mm");
        }
        else
        {
            Serial.println("[Debug VL53L0X] INDISPONIVEL");
        }
    }
}
