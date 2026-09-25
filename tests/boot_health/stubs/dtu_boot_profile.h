#define DTU_PROFILE_BAUD 460800U
#define DTU_PROFILE_TX_GPIO 17
#define DTU_PROFILE_RX_GPIO 18
static const struct { const char *query; const char *expected; } s_profile[] = {
    {"AT+UART1", "+UART1:460800,8,1,NONE,485"},
    {"AT+SOCK1A", "+SOCK1A:MQTT,mqtt.example.com,1883"},
    {"AT+MQPUB1", "+MQPUB1:1,dji-h1/test/up,1,0"}
};
