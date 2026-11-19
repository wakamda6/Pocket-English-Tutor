#include "tutor_session.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bsp_audio.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"
#include "tutor_adpcm.h"
#include "tutor_cloud_message.h"
#include "tutor_pcm.h"
#include "tutor_playback.h"

#define SEEDUPLEX_URI "wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue"
#define PCM_BYTES (TUTOR_FRAME_SAMPLES * sizeof(int16_t))
#define BASE64_BYTES (4 * ((PCM_BYTES + 2) / 3) + 1)
#define PLAYBACK_WAIT_MS 75
#define WS_CONNECTED BIT0
#define SESSION_READY BIT1
#define SESSION_CLOSED BIT2
#define SESSION_FAILED BIT3
#define PLAYBACK_EXITED BIT4
#define WS_TEXT 1
#define WS_CONTINUATION 0
#define MIC_DRAIN_FRAMES 5
#define MIC_FRAME_MS 20
#define REPLY_TIMEOUT_MS 25000

static const char *TAG = "tutor_session";

#define SESSION_PREFIX "{\"type\":\"session.create\",\"session\":{\"model\":\"1.2.6.1\",\"instructions\":\""
#define SESSION_SUFFIX "\",\"audio\":{\"input\":{\"format\":{\"type\":\"pcm\",\"rate\":16000}}," \
                       "\"output\":{\"format\":{\"type\":\"pcm_s16le\",\"rate\":24000}," \
                       "\"voice\":\"zh_female_vv_jupiter_bigtts\",\"speed\":%d}}}}"
#define TUTOR_CONTEXT "Adapt English wording to the situation and audience. Carry context across turns. " \
                      "Base pronunciation feedback on what you actually hear. If unsure, ask the learner to repeat. "

static const char english_chat_session[] = SESSION_PREFIX
    "You are a friendly English conversation partner and tutor. Speak exclusively in English, "
    "including grammar, pronunciation, and spelling corrections, even if the learner speaks Mandarin. "
    "Engage with the meaning first. If the learner makes a clear grammar or pronunciation error, "
    "briefly model the corrected phrase in English and continue the conversation. When asked for spelling "
    "or correcting a spelling the learner actually gave, say the letters individually. If the learner "
    "asks how to say something, give a natural English example and keep any explanation short. "
    TUTOR_CONTEXT SESSION_SUFFIX;

static const char teaching_session[] = SESSION_PREFIX
    "You teach English to a Mandarin speaker. Explain grammar, pronunciation and word choice mainly "
    "in Mandarin Chinese, even when the learner asks in English. Say the original English, corrected "
    "examples and pronunciation demonstrations in English. When asked how to say something, first give "
    "a usable English sentence, then briefly explain in Mandarin. For grammar, explain one useful rule "
    "with a short example. Limit each answer to one main teaching point. Give pronunciation feedback "
    "only when requested or when you clearly hear an issue, and demonstrate it aloud. If asked "
    "for spelling, say the English letters individually. "
    TUTOR_CONTEXT SESSION_SUFFIX;

typedef struct {
    int16_t samples[TUTOR_FRAME_SAMPLES];
    size_t count;
} playback_block_t;

_Static_assert(TUTOR_PLAYBACK_BLOCKS * sizeof(tutor_adpcm_block_t) <= 44 * sizeof(playback_block_t),
               "Compressed playback must fit in the previous PCM queue budget");

typedef struct received_pcm {
    struct received_pcm *next;
    size_t count;
    int16_t samples[];
} received_pcm_t;

// Written only by the WebSocket task, including its PCM callbacks.
typedef struct {
    unsigned first_rx_ms;
    unsigned last_rx_ms;
    unsigned max_rx_gap_ms;
    unsigned feed_us;
    unsigned first_enqueue_ms;
    unsigned last_enqueue_ms;
    unsigned enqueue_wait_us;
    unsigned blocks;
    unsigned fragments;
    size_t pcm_bytes;
} audio_message_trace_t;

typedef struct {
    esp_websocket_client_handle_t socket;
    tutor_mode_t mode;
    int8_t speed;
    EventGroupHandle_t events;
    QueueHandle_t speaker;
    atomic_bool playback_stop;
    atomic_bool reply_finished;
    atomic_bool input_finalized;
    atomic_bool reply_submitted;
    atomic_uint playback_epoch;
    bool output_open;
    bool receiving_text;
    tutor_cloud_message_t message;
    audio_message_trace_t trace;
    unsigned audio_message_number;
    unsigned previous_audio_rx_ms;
    received_pcm_t *pending_pcm;
    received_pcm_t *pending_tail;
    tutor_pcm_stream_t converter;
    tutor_pcm_stream_t pending_converter;
    bool pending_conversion;
    playback_block_t partial;
    unsigned adpcm_step_index;
    char auth_header[sizeof("X-Api-Key: \r\n") + TUTOR_KEY_MAX];
} session_t;

static atomic_bool active;
static atomic_bool stop_requested;
static atomic_bool mic_requested;
static atomic_uint mic_stop_ms;
static atomic_bool mic_streaming;
static atomic_bool speaker_active;
static atomic_int status;
static atomic_uint volume = TUTOR_DEFAULT_VOLUME;

static unsigned trace_ms(void)
{
    return (unsigned)(esp_timer_get_time() / 1000);
}

static void session_fail(session_t *session, tutor_status_t failure)
{
    atomic_store(&status, failure);
    xEventGroupSetBits(session->events, SESSION_FAILED);
}

static void discard_playback(session_t *session)
{
    session->output_open = false;
    session->converter = (tutor_pcm_stream_t){0};
    session->partial.count = 0;
    session->adpcm_step_index = 0;
    xQueueReset(session->speaker);
    atomic_store(&session->reply_finished, true);
    atomic_fetch_add(&session->playback_epoch, 1);
}

static bool queue_playback(session_t *session)
{
    if (session->partial.count == 0) {
        return true;
    }
    atomic_store(&speaker_active, true);
    if (atomic_load(&stop_requested)) {
        return false;
    }
    tutor_adpcm_block_t block = {0};
    tutor_adpcm_encode(&block, session->partial.samples, session->partial.count,
                        &session->adpcm_step_index);
    int64_t send_start = esp_timer_get_time();
    // Apply receive-side backpressure when an early reply fills the queue.
    // The WebSocket client uses a separate TX lock for the concurrent microphone upload.
    BaseType_t sent;
    bool waiting_for_submit = false;
    for (;;) {
        bool submitted = atomic_load(&session->reply_submitted);
        sent = xQueueSend(session->speaker, &block, pdMS_TO_TICKS(submitted ? 100 : 20));
        if (atomic_load(&stop_requested) ||
            (xEventGroupGetBits(session->events) & SESSION_FAILED)) return false;
        if (sent == pdTRUE || submitted) break;
        if (!waiting_for_submit) {
            ESP_LOGI(TAG, "Audio trace waiting_for_send=%u", trace_ms());
            waiting_for_submit = true;
        }
    }
    int64_t send_end = esp_timer_get_time();
    session->trace.enqueue_wait_us += (unsigned)(send_end - send_start);
    if (sent != pdTRUE) {
        ESP_LOGW(TAG, "Playback queue full");
        session_fail(session, TUTOR_AUDIO_FAILED);
        return false;
    }
    unsigned enqueued_ms = (unsigned)(send_end / 1000);
    if (session->trace.blocks++ == 0) session->trace.first_enqueue_ms = enqueued_ms;
    session->trace.last_enqueue_ms = enqueued_ms;
    session->partial.count = 0;
    return true;
}

static bool append_samples(session_t *session, const int16_t *samples, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        session->partial.samples[session->partial.count++] = samples[i];
        if (session->partial.count == TUTOR_FRAME_SAMPLES && !queue_playback(session)) {
            return false;
        }
    }
    return true;
}

static bool accept_pcm(void *context, const uint8_t *pcm, size_t length)
{
    session_t *session = context;
    if (!session->output_open || atomic_load(&stop_requested) ||
        (xEventGroupGetBits(session->events) & SESSION_FAILED)) return true;
    session->trace.pcm_bytes += length;
    int16_t samples[TUTOR_PCM_OUTPUT_CAP(TUTOR_CLOUD_PCM_CHUNK)];
    if (session->message.type[0]) {
        size_t count = tutor_pcm_convert_le(&session->converter, pcm, length, samples);
        return append_samples(session, samples, count);
    }
    // Delta can precede type. Resample speculatively and commit only for an audio event.
    if (!session->pending_conversion) {
        session->pending_converter = session->converter;
        session->pending_conversion = true;
    }
    size_t count = tutor_pcm_convert_le(&session->pending_converter, pcm, length, samples);
    if (!count) return true;
    received_pcm_t *block = malloc(sizeof(*block) + count * sizeof(*samples));
    if (!block) return false;
    block->next = NULL;
    block->count = count;
    memcpy(block->samples, samples, count * sizeof(*samples));
    if (session->pending_tail) {
        session->pending_tail->next = block;
    } else {
        session->pending_pcm = block;
    }
    session->pending_tail = block;
    return true;
}

static void release_pending_pcm(session_t *session, bool play)
{
    if (play && session->pending_conversion) session->converter = session->pending_converter;
    session->pending_conversion = false;
    while (session->pending_pcm) {
        received_pcm_t *block = session->pending_pcm;
        session->pending_pcm = block->next;
        if (play) play = append_samples(session, block->samples, block->count);
        free(block);
    }
    session->pending_tail = NULL;
}

static void handle_message(session_t *session, unsigned message_complete_ms)
{
    const char *type = session->message.type;
    if (strcmp(type, "session.created") == 0) {
        xEventGroupSetBits(session->events, SESSION_READY);
    } else if (strcmp(type, "session.closed") == 0) {
        xEventGroupSetBits(session->events, SESSION_CLOSED);
    } else if (strcmp(type, "error") == 0) {
        ESP_LOGW(TAG, "Seeduplex error event");
        session_fail(session, TUTOR_CLOUD_FAILED);
    } else if (atomic_load(&stop_requested) || !tutor_turn_accepts_reply(atomic_load(&status)) ||
               (xEventGroupGetBits(session->events) & SESSION_FAILED)) {
        // Conversation events are only accepted after a local recording is submitted.
    } else if (strcmp(type, "conversation.item.input_audio_transcription.started") == 0) {
        ESP_LOGI(TAG, "Audio trace transcription_start=%u", message_complete_ms);
    } else if (strcmp(type, "response.canceled") == 0) {
        discard_playback(session);
        ESP_LOGI(TAG, "Audio trace canceled=%u", message_complete_ms);
        atomic_store(&status, atomic_load(&mic_requested) ? TUTOR_LISTENING : TUTOR_READY);
    } else if (strcmp(type, "conversation.item.input_audio_transcription.completed") == 0) {
        atomic_store(&session->input_finalized, true);
        ESP_LOGI(TAG, "Audio trace transcription_done=%u", message_complete_ms);
    } else if (strcmp(type, "conversation.item.input_audio_transcription.failed") == 0) {
        ESP_LOGW(TAG, "Audio trace transcription_failed=%u", message_complete_ms);
        int expected = TUTOR_THINKING;
        atomic_compare_exchange_strong(&status, &expected, TUTOR_READY);
    } else if (strcmp(type, "response.output_audio.started") == 0 &&
               atomic_load(&status) != TUTOR_SPEAKING) {
        atomic_store(&session->input_finalized, true);
        session->audio_message_number = 0;
        session->previous_audio_rx_ms = 0;
        session->output_open = true;
        session->converter = (tutor_pcm_stream_t){0};
        session->partial.count = 0;
        session->adpcm_step_index = 0;
        atomic_store(&session->reply_finished, false);
        atomic_store(&status, TUTOR_SPEAKING);
        ESP_LOGI(TAG, "Audio trace output_start=%u speed=%d", message_complete_ms, session->speed);
    } else if (tutor_cloud_message_is_audio(&session->message) && session->output_open) {
        release_pending_pcm(session, true);
        const audio_message_trace_t *trace = &session->trace;
        unsigned gap = session->previous_audio_rx_ms ?
                       trace->first_rx_ms - session->previous_audio_rx_ms : 0;
        session->previous_audio_rx_ms = trace->last_rx_ms;
        ESP_LOGI(TAG, "Audio trace msg=%u bytes=%u pcm_bytes=%u fragments=%u "
                 "rx_first=%u rx_last=%u gap_ms=%u fragment_gap_max_ms=%u "
                 "feed_us=%u end=%u enqueue_first=%u enqueue_last=%u "
                 "queue_send_us=%u blocks=%u queued=%u",
                 ++session->audio_message_number, (unsigned)session->message.length,
                 (unsigned)trace->pcm_bytes, trace->fragments, trace->first_rx_ms,
                 trace->last_rx_ms, gap, trace->max_rx_gap_ms, trace->feed_us,
                 message_complete_ms, trace->first_enqueue_ms, trace->last_enqueue_ms,
                 trace->enqueue_wait_us, trace->blocks,
                 (unsigned)uxQueueMessagesWaiting(session->speaker));
    } else if (strcmp(type, "response.output_audio.done") == 0 && session->output_open) {
        int16_t tail[2];
        unsigned count = tutor_pcm_finish(&session->converter.resampler, tail);
        if (session->converter.have_low_byte) {
            session_fail(session, TUTOR_CLOUD_FAILED);
        } else if (append_samples(session, tail, count)) {
            queue_playback(session);
        }
        session->output_open = false;
        atomic_store(&session->reply_finished, true);
        ESP_LOGI(TAG, "Audio trace output_done=%u blocks=%u queued=%u", message_complete_ms,
                 session->trace.blocks, (unsigned)uxQueueMessagesWaiting(session->speaker));
    } else if (strcmp(type, "response.done") == 0 && !session->output_open) {
        atomic_store(&status, TUTOR_READY);
    }
}

static void reset_message(session_t *session)
{
    session->receiving_text = false;
    session->message = (tutor_cloud_message_t){0};
    session->trace = (audio_message_trace_t){0};
    release_pending_pcm(session, false);
}

static void receive_data(session_t *session, const esp_websocket_event_data_t *event)
{
    if (event->op_code != WS_TEXT && event->op_code != WS_CONTINUATION) {
        return;
    }
    int64_t rx_time = esp_timer_get_time();
    if (event->op_code == WS_TEXT && event->payload_offset == 0) {
        reset_message(session);
        session->receiving_text = true;
    }
    if (!session->receiving_text) {
        return;
    }
    if (event->payload_offset < 0 || event->payload_len < event->payload_offset ||
        event->data_len < 0 || event->data_len > event->payload_len - event->payload_offset ||
        (size_t)(event->payload_len - event->payload_offset) >
            TUTOR_CLOUD_MESSAGE_MAX - session->message.length) {
        ESP_LOGW(TAG, "Invalid or oversized Seeduplex frame (%d bytes)", event->payload_len);
        reset_message(session);
        session_fail(session, TUTOR_CLOUD_FAILED);
        return;
    }
    audio_message_trace_t *trace = &session->trace;
    unsigned rx_ms = (unsigned)(rx_time / 1000);
    if (trace->fragments++ == 0) {
        trace->first_rx_ms = rx_ms;
    } else {
        unsigned gap = rx_ms - trace->last_rx_ms;
        if (gap > trace->max_rx_gap_ms) trace->max_rx_gap_ms = gap;
    }
    trace->last_rx_ms = rx_ms;
    int64_t feed_start = esp_timer_get_time();
    bool fed = tutor_cloud_message_feed(&session->message, event->data_ptr, event->data_len,
                                        accept_pcm, session);
    trace->feed_us += (unsigned)(esp_timer_get_time() - feed_start);
    if (!fed) {
        ESP_LOGW(TAG, "Seeduplex message receive failed (%d-byte frame, largest block %u)",
                 event->payload_len, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        reset_message(session);
        if (!atomic_load(&stop_requested) &&
            !(xEventGroupGetBits(session->events) & SESSION_FAILED)) {
            session_fail(session, TUTOR_CLOUD_FAILED);
        }
        return;
    }
    if (event->fin && event->payload_offset + event->data_len == event->payload_len) {
        if (tutor_cloud_message_end(&session->message)) {
            handle_message(session, trace_ms());
        } else {
            ESP_LOGW(TAG, "Incomplete or invalid Seeduplex message (%u bytes)",
                     (unsigned)session->message.length);
            session_fail(session, TUTOR_CLOUD_FAILED);
        }
        reset_message(session);
    }
}

static void on_websocket(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    session_t *session = arg;
    const esp_websocket_event_data_t *event = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WSS connected, free heap %u, largest block %u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        xEventGroupSetBits(session->events, WS_CONNECTED);
        break;
    case WEBSOCKET_EVENT_DATA:
        receive_data(session, event);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WSS error: HTTP %d, TLS flags 0x%x",
                 event->error_handle.esp_ws_handshake_status_code,
                 (unsigned)event->error_handle.esp_tls_cert_verify_flags);
        if (!atomic_load(&stop_requested)) {
            session_fail(session, TUTOR_CLOUD_FAILED);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (!atomic_load(&stop_requested)) {
            session_fail(session, TUTOR_CLOUD_FAILED);
        }
        break;
    default:
        break;
    }
}

static void play_audio(void *arg)
{
    session_t *session = arg;
    tutor_adpcm_block_t block;
    int16_t samples[TUTOR_FRAME_SAMPLES];
    unsigned current_volume = atomic_load(&volume);
    unsigned epoch = atomic_load(&session->playback_epoch);
    unsigned underruns = 0;
    unsigned reply_underruns = 0;
    unsigned last_write_done_ms = 0;
    unsigned max_write_us = 0;
    bool played_reply = false;
    bool playing = false;
    while (!atomic_load(&session->playback_stop)) {
        unsigned current_epoch = atomic_load(&session->playback_epoch);
        if (epoch != current_epoch) {
            playing = false;
            played_reply = false;
            reply_underruns = 0;
            max_write_us = 0;
            epoch = current_epoch;
        }
        bool finished = atomic_load(&session->reply_finished);
        bool submitted = atomic_load(&session->reply_submitted);
        size_t queued_blocks = uxQueueMessagesWaiting(session->speaker);
        if (!playing && !tutor_playback_can_start(submitted, queued_blocks, finished)) {
            if (finished && !queued_blocks) {
                atomic_store(&speaker_active, false);
                if (played_reply) {
                    ESP_LOGI(TAG, "Audio trace playback_done last_write=%u underruns=%u max_write_us=%u",
                             last_write_done_ms, reply_underruns, max_write_us);
                    reply_underruns = 0;
                    max_write_us = 0;
                    played_reply = false;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        unsigned wait_start_ms = trace_ms();
        if (xQueueReceive(session->speaker, &block, pdMS_TO_TICKS(PLAYBACK_WAIT_MS)) != pdTRUE) {
            if (atomic_load(&session->reply_finished)) {
                atomic_store(&speaker_active, false);
            } else if (playing) {
                underruns++;
                reply_underruns++;
                ESP_LOGW(TAG, "Audio trace underrun=%u wait_start=%u timeout=%u last_write=%u",
                         reply_underruns, wait_start_ms, trace_ms(), last_write_done_ms);
            }
            playing = false;
            continue;
        }
        if (atomic_load(&session->playback_stop)) {
            break;
        }
        unsigned next_volume = atomic_load(&volume);
        if (next_volume != current_volume) {
            bsp_audio_set_volume(next_volume);
            current_volume = next_volume;
        }
        atomic_store(&speaker_active, true);
        bool starting = !playing;
        unsigned queued = starting ? (unsigned)uxQueueMessagesWaiting(session->speaker) + 1 : 0;
        size_t count = tutor_adpcm_decode(&block, samples);
        int64_t write_start = esp_timer_get_time();
        if (!count || bsp_audio_write(samples, count * sizeof(samples[0])) != ESP_OK) {
            session_fail(session, TUTOR_AUDIO_FAILED);
            break;
        }
        int64_t write_end = esp_timer_get_time();
        unsigned write_us = (unsigned)(write_end - write_start);
        if (write_us > max_write_us) max_write_us = write_us;
        unsigned prior_write_done_ms = last_write_done_ms;
        last_write_done_ms = (unsigned)(write_end / 1000);
        if (starting) {
            unsigned first_write_ms = (unsigned)(write_start / 1000);
            ESP_LOGI(TAG, "Audio trace %s=%u queued=%u finished=%d write_gap_ms=%u",
                     played_reply ? "playback_resume" : "playback_start", first_write_ms,
                     queued, finished, played_reply ? first_write_ms - prior_write_done_ms : 0);
        }
        playing = true;
        played_reply = true;
    }
    atomic_store(&speaker_active, false);
    ESP_LOGI(TAG, "Playback queue underruns: %u", underruns);
    xEventGroupSetBits(session->events, PLAYBACK_EXITED);
    vTaskDelete(NULL);
}

static bool send_message(session_t *session, const char *message)
{
    int length = strlen(message);
    return esp_websocket_client_send_text(session->socket, message, length,
                                          pdMS_TO_TICKS(750)) == length;
}

static bool send_session_create(session_t *session)
{
    const char *format = session->mode == TUTOR_MODE_TEACHING ?
                         teaching_session : english_chat_session;
    // The template has one integer substitution in the range -30 to 30.
    size_t capacity = strlen(format) + 2;
    char *message = malloc(capacity);
    if (!message) return false;
    int length = snprintf(message, capacity, format, (int)session->speed);
    bool sent = length > 0 && (size_t)length < capacity && send_message(session, message);
    free(message);
    return sent;
}

static bool wait_until(session_t *session, EventBits_t bit, TickType_t timeout)
{
    TickType_t start = xTaskGetTickCount();
    while (xTaskGetTickCount() - start < timeout && !atomic_load(&stop_requested)) {
        EventBits_t bits = xEventGroupWaitBits(session->events, bit | SESSION_FAILED,
                                              pdFALSE, pdFALSE, pdMS_TO_TICKS(100));
        if (bits & (bit | SESSION_FAILED)) {
            return (bits & (bit | SESSION_FAILED)) == bit;
        }
    }
    return false;
}

static bool session_running(session_t *session)
{
    return !atomic_load(&stop_requested) &&
           !(xEventGroupGetBits(session->events) & SESSION_FAILED);
}

static void stream_mic(session_t *session)
{
    int16_t input[TUTOR_FRAME_SAMPLES];
    char message[BASE64_BYTES + 64] = "{\"type\":\"input_audio_buffer.append\",\"audio\":\"";
    size_t prefix = strlen(message);
    TickType_t frame_ticks = pdMS_TO_TICKS(MIC_FRAME_MS);
    TickType_t last_frame = xTaskGetTickCount() - frame_ticks;
    TickType_t stopped_at = last_frame;
    bool unmuted = false;
    bool sent_audio = false;
    while (session_running(session)) {
        if (!atomic_load(&mic_requested)) {
            if (unmuted) {
                unsigned button_ms = atomic_load(&mic_stop_ms);
                int expected = TUTOR_LISTENING;
                atomic_compare_exchange_strong(&status, &expected,
                                               sent_audio ? TUTOR_THINKING : TUTOR_READY);
                bool commit = sent_audio && atomic_load(&status) == TUTOR_THINKING &&
                              !atomic_load(&session->input_finalized);
                unsigned send_ms = trace_ms();
                // The last microphone read and upload have finished. Release any early reply.
                atomic_store(&session->reply_submitted, true);
                if ((commit && !send_message(session, "{\"type\":\"input_audio_buffer.commit\"}")) ||
                    !send_message(session, "{\"type\":\"input_audio_mute.commit\"}")) {
                    session_fail(session, TUTOR_CLOUD_FAILED);
                    break;
                }
                unmuted = false;
                sent_audio = false;
                stopped_at = xTaskGetTickCount();
                ESP_LOGI(TAG, "Audio trace mic_stop button=%u send_start=%u send_done=%u commit=%d",
                         button_ms, send_ms, trace_ms(), commit);
                atomic_store(&mic_streaming, false);
            } else if (xTaskGetTickCount() - stopped_at >= pdMS_TO_TICKS(REPLY_TIMEOUT_MS)) {
                int expected = TUTOR_THINKING;
                atomic_compare_exchange_strong(&status, &expected, TUTOR_READY);
            }
            vTaskDelay(frame_ticks);
            continue;
        }
        if (!unmuted) {
            atomic_store(&session->reply_submitted, false);
            atomic_store(&session->input_finalized, false);
            // Discard samples already buffered by I2S while the microphone was idle.
            for (unsigned i = 0; i < MIC_DRAIN_FRAMES && atomic_load(&mic_requested) &&
                                 session_running(session); i++) {
                if (bsp_audio_read(input, sizeof(input)) != ESP_OK) {
                    session_fail(session, TUTOR_AUDIO_FAILED);
                    return;
                }
            }
            if (!atomic_load(&mic_requested) || !session_running(session)) continue;
            atomic_store(&status, TUTOR_LISTENING);
            if (!send_message(session, "{\"type\":\"input_audio_unmute.commit\"}")) {
                session_fail(session, TUTOR_CLOUD_FAILED);
                break;
            }
            unmuted = true;
            atomic_store(&mic_streaming, true);
            ESP_LOGI(TAG, "Audio trace mic_start=%u", trace_ms());
        }
        if (!atomic_load(&mic_requested) || !session_running(session)) continue;
        if (bsp_audio_read(input, sizeof(input)) != ESP_OK) {
            session_fail(session, TUTOR_AUDIO_FAILED);
            break;
        }
        size_t encoded;
        if (mbedtls_base64_encode((uint8_t *)message + prefix, BASE64_BYTES, &encoded,
                                  (const uint8_t *)input, sizeof(input)) != 0) {
            session_fail(session, TUTOR_AUDIO_FAILED);
            break;
        }
        memcpy(message + prefix + encoded, "\"}", sizeof("\"}"));
        TickType_t elapsed = xTaskGetTickCount() - last_frame;
        if (elapsed < frame_ticks) vTaskDelay(frame_ticks - elapsed);
        if (!atomic_load(&mic_requested) || !session_running(session)) continue;
        last_frame = xTaskGetTickCount();
        if (!send_message(session, message)) {
            session_fail(session, TUTOR_CLOUD_FAILED);
            break;
        }
        sent_audio = true;
    }
}
static void run_session(void *arg)
{
    session_t *session = arg;
    bool playback_started = false;
    tutor_status_t result = TUTOR_CLOUD_FAILED;
    esp_websocket_client_config_t config = {
        .uri = SEEDUPLEX_URI,
        .headers = session->auth_header,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = true,
        .network_timeout_ms = 4500,
        .buffer_size = 2048,
        .task_stack = 6144,
        .task_prio = 5,
    };
    ESP_LOGI(TAG, "Starting WSS, free heap %u, largest block %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    session->socket = esp_websocket_client_init(&config);
    if (!session->socket ||
        esp_websocket_register_events(session->socket, WEBSOCKET_EVENT_ANY, on_websocket,
                                      session) != ESP_OK ||
        esp_websocket_client_start(session->socket) != ESP_OK ||
        !wait_until(session, WS_CONNECTED, pdMS_TO_TICKS(9000))) {
        goto done;
    }
    if (bsp_audio_init() != ESP_OK || bsp_audio_wake() != ESP_OK ||
        bsp_audio_set_format(TUTOR_PCM_RATE, 16, 1) != ESP_OK) {
        result = TUTOR_AUDIO_FAILED;
        goto done;
    }
    bsp_audio_set_volume(atomic_load(&volume));
    playback_started = xTaskCreate(play_audio, "tutor_speaker", 3072, session, 5, NULL) == pdPASS;
    if (!playback_started) {
        result = TUTOR_AUDIO_FAILED;
        goto done;
    }
    if (!send_session_create(session) ||
        !wait_until(session, SESSION_READY, pdMS_TO_TICKS(7000)) ||
        !send_message(session, "{\"type\":\"input_audio_mute.commit\"}")) {
        goto done;
    }
    atomic_store(&status, TUTOR_READY);
    stream_mic(session);
    result = atomic_load(&status);

done:
    if (atomic_load(&stop_requested)) {
        result = TUTOR_IDLE;
        atomic_store(&status, TUTOR_CLOSING);
    }
    atomic_store(&session->playback_stop, true);
    if (session->socket) {
        if (esp_websocket_client_is_connected(session->socket)) {
            if ((xEventGroupGetBits(session->events) & SESSION_READY) &&
                send_message(session, "{\"type\":\"session.close\"}")) {
                xEventGroupWaitBits(session->events, SESSION_CLOSED,
                                    pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
            }
            esp_websocket_client_close(session->socket, pdMS_TO_TICKS(500));
        }
        esp_websocket_client_destroy(session->socket);
    }
    if (playback_started) {
        xEventGroupWaitBits(session->events, PLAYBACK_EXITED, pdFALSE, pdFALSE, portMAX_DELAY);
    }
    if (bsp_audio_sleep() != ESP_OK) {
        ESP_LOGW(TAG, "Could not suspend the audio codec");
    }
    vQueueDelete(session->speaker);
    vEventGroupDelete(session->events);
    mbedtls_platform_zeroize(session->auth_header, sizeof(session->auth_header));
    release_pending_pcm(session, false);
    free(session);
    atomic_store(&status, result);
    atomic_store(&active, false);
    vTaskDelete(NULL);
}

esp_err_t tutor_session_start(const char *key, tutor_mode_t mode, int8_t speed)
{
    if (!key || !*key || strlen(key) > TUTOR_KEY_MAX || !tutor_speed_valid(speed)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_load(&active)) {
        return ESP_ERR_INVALID_STATE;
    }
    session_t *session = calloc(1, sizeof(*session));
    if (!session) {
        atomic_store(&status, TUTOR_CLOUD_FAILED);
        return ESP_ERR_NO_MEM;
    }
    session->mode = mode;
    session->speed = speed;
    atomic_init(&session->playback_stop, false);
    atomic_init(&session->reply_finished, true);
    atomic_init(&session->input_finalized, false);
    atomic_init(&session->reply_submitted, false);
    atomic_init(&session->playback_epoch, 0);
    session->events = xEventGroupCreate();
    session->speaker = xQueueCreate(TUTOR_PLAYBACK_BLOCKS, sizeof(tutor_adpcm_block_t));
    snprintf(session->auth_header, sizeof(session->auth_header), "X-Api-Key: %s\r\n", key);
    atomic_store(&stop_requested, false);
    atomic_store(&mic_stop_ms, 0);
    atomic_store(&mic_requested, true);
    atomic_store(&mic_streaming, false);
    atomic_store(&speaker_active, false);
    atomic_store(&status, TUTOR_CONNECTING);
    atomic_store(&active, true);
    if (session->events && session->speaker &&
        xTaskCreate(run_session, "tutor_cloud", 5120, session, 5, NULL) == pdPASS) {
        return ESP_OK;
    }
    if (session->speaker) {
        vQueueDelete(session->speaker);
    }
    if (session->events) {
        vEventGroupDelete(session->events);
    }
    mbedtls_platform_zeroize(session->auth_header, sizeof(session->auth_header));
    free(session);
    atomic_store(&status, TUTOR_CLOUD_FAILED);
    atomic_store(&active, false);
    return ESP_ERR_NO_MEM;
}

void tutor_session_stop(void)
{
    atomic_store(&stop_requested, true);
}

void tutor_session_toggle_mic(void)
{
    if (atomic_load(&stop_requested)) return;
    tutor_status_t current = atomic_load(&status);
    if (atomic_load(&mic_requested)) {
        atomic_store(&mic_stop_ms, trace_ms());
        atomic_store(&mic_requested, false);
    } else if (tutor_turn_can_start(current, atomic_load(&mic_streaming),
                                    atomic_load(&speaker_active))) {
        atomic_store(&mic_requested, true);
    }
}

bool tutor_session_active(void)
{
    return atomic_load(&active);
}

tutor_status_t tutor_session_status(void)
{
    return tutor_turn_view(atomic_load(&status), atomic_load(&mic_requested),
                           atomic_load(&mic_streaming), atomic_load(&speaker_active));
}

void tutor_session_set_volume(uint8_t percent)
{
    atomic_store(&volume, percent);
}
