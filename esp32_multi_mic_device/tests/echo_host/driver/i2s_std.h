#pragma once
#include <cstddef>
#include <cstdint>
#include <array>
using i2s_chan_handle_t = void *;
using esp_err_t = int;
constexpr int ESP_OK = 0;
#define IRAM_ATTR
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define portENTER_CRITICAL_ISR(x) ((void)(x))
#define portEXIT_CRITICAL_ISR(x) ((void)(x))
struct i2s_event_data_t { void *data; void *dma_buf; size_t size; };
using Callback = bool (*)(i2s_chan_handle_t, i2s_event_data_t *, void *);
struct i2s_event_callbacks_t { Callback on_recv=nullptr, on_recv_q_ovf=nullptr, on_sent=nullptr, on_send_q_ovf=nullptr; };
inline std::array<i2s_event_callbacks_t, 4> callbacks;
inline int failInstall = 0;
inline int enables = 0, disables = 0;
inline int i2s_channel_disable(i2s_chan_handle_t) { ++disables; return ESP_OK; }
inline int i2s_channel_enable(i2s_chan_handle_t) { ++enables; return ESP_OK; }
inline int i2s_channel_register_event_callback(i2s_chan_handle_t ch, const i2s_event_callbacks_t *cb, void *) {
  const int index = reinterpret_cast<uintptr_t>(ch);
  if (failInstall == index) return -1;
  callbacks[index] = *cb; return ESP_OK;
}
