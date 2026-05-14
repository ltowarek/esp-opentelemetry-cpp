// esp_http_client-backed HTTP client for opentelemetry-cpp's OTLP exporter.
// Replaces the default libcurl transport in the firmware image.

#pragma once

#include "opentelemetry/ext/http/client/http_client.h"

#include <memory>

namespace esp_opentelemetry {

std::shared_ptr<opentelemetry::ext::http::client::HttpClient>
MakeEspHttpClient();

}  // namespace esp_opentelemetry
