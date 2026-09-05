#include "esp_opentelemetry.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "profiles-example";

extern "C" void app_main()
{
    // Profiles have no SDK exporter interface, so the component supplies one;
    // the application picks an implementation the same way.
    esp_opentelemetry_profiling_setup(esp_opentelemetry::MakeConsoleProfilesExporter());

    for (int iteration = 0; iteration < 5; ++iteration) {
        // Burn a little CPU so the sampler has stacks to aggregate.
        volatile uint32_t sink = 0;
        for (uint32_t i = 0; i < 200000; ++i) {
            sink += i;
        }
        (void)sink;

        ESP_LOGI(TAG, "iteration %d", iteration);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "done");
}
