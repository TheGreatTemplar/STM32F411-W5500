#ifndef CONFIG_H
#define CONFIG_H

#define APP_HTTP_PORT 5025U
#define APP_SAMPLE_PERIOD_MS 20U
#define APP_REQUEST_TIMEOUT_MS 2000U
#define APP_ADC_VREF_MV 3300U
#define APP_REQUEST_CAPACITY 2048U
#define APP_HTTP_SOCKETS 4U
#define APP_SOCKET_BUFFER_SIZE 2048U

/* Locally administered unicast MAC. Use a different MAC for each board. */
#define APP_MAC_ADDRESS                                                                            \
    {                                                                                              \
        0x02U, 0x41U, 0x11U, 0xECU, 0x00U, 0xDCU                                                   \
    }
#define APP_IP_ADDRESS                                                                             \
    {                                                                                              \
        172U, 16U, 236U, 220U                                                                      \
    }
#define APP_NETMASK                                                                                \
    {                                                                                              \
        255U, 255U, 255U, 0U                                                                       \
    }
#define APP_GATEWAY                                                                                \
    {                                                                                              \
        172U, 16U, 236U, 1U                                                                        \
    }

#endif
