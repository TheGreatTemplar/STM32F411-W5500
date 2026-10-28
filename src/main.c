#include "board.h"
#include "command_store.h"
#include "config.h"
#include "http.h"
#include "w5500.h"

int main(void)
{
    adc_snapshot_t adc = {{0U, 0U}, 0U, 0U, false};
    uint32_t last_sample;
    uint32_t last_retry;
    bool network_ready;
    board_init();
    network_ready = w5500_init();
    command_store_init();
    last_retry = board_millis();
    last_sample = board_millis() - APP_SAMPLE_PERIOD_MS;
    for (;;)
    {
        const uint32_t now = board_millis();
        if ((uint32_t)(now - last_sample) >= APP_SAMPLE_PERIOD_MS)
        {
            uint16_t values[2];
            last_sample = now;
            adc.valid = board_adc_read(values);
            if (adc.valid)
            {
                adc.raw[0] = values[0];
                adc.raw[1] = values[1];
                adc.sample_ms = board_millis();
                ++adc.sample_count;
            }
        }
        if (network_ready && w5500_healthy())
        {
            if (w5500_link())
            {
                http_poll(&adc);
            }
            else
            {
                uint8_t socket;
                for (socket = 0U; socket < APP_HTTP_SOCKETS; ++socket)
                {
                    w5500_close(socket);
                }
                http_reset();
            }
        }
        else if ((uint32_t)(now - last_retry) >= 2000U)
        {
            last_retry = now;
            http_reset();
            network_ready = w5500_init();
        }
        command_store_poll();
        __WFI();
    }
}
