#pragma GCC optimize ("Ofast","unroll-loops")
#include "apu2A03.h"
#include "bus.h"
#include "cpu6502.h"

#ifndef COMPOSITE_VIDEO
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"
#endif

#ifdef COMPOSITE_VIDEO
void cv_audio_write_16(const uint16_t* s, int len, int channels);
bool cv_audio_buffer_full(int buffer_size);
#endif

DMA_ATTR uint16_t Apu2A03::audio_buffer[AUDIO_BUFFER_SIZE * 2];

// Diagnóstico COCODRILO NES audio.
static volatile uint32_t coco_apu_writes = 0;
static volatile uint8_t coco_apu_4015 = 0;

#ifndef COMPOSITE_VIDEO
// COCODRILO: desacoplar emulación APU de la salida física I2S.
// El emulador produce ~6 bloques de 128 frames por frame de vídeo.
// La cola absorbe esa ráfaga y una tarea separada hace el i2s_write()
// bloqueante, evitando frenar Cpu6502::clock().
static constexpr uint8_t COCO_AUDIO_QUEUE_BLOCKS = 12;

struct CocoAudioBlock
{
    int16_t pcm[AUDIO_BUFFER_SIZE * 2];
};

static QueueHandle_t coco_audio_queue = nullptr;
static TaskHandle_t coco_audio_writer_task = nullptr;
static volatile bool coco_audio_writer_running = false;
static volatile uint32_t coco_audio_queued = 0;
static volatile uint32_t coco_audio_written = 0;
static volatile uint32_t coco_audio_dropped = 0;
static volatile uint32_t coco_audio_i2s_errors = 0;
static volatile uint32_t coco_audio_max_depth = 0;

static void cocoAudioWriterTask(void*)
{
    CocoAudioBlock block;

    // COCODRILO FIX V1:
    // El APU ya genera muestras a 44.1 kHz según el reloj emulado.
    // No estiramos/comprimimos cada bloque según el tiempo real de llegada:
    // ese resampling variable cambia continuamente el tono y puede sonar
    // como distorsión cuando el emulador se atrasa.
    //
    // La cola sigue desacoplando el APU del i2s_write() bloqueante.
    while (coco_audio_writer_running)
    {
        if (xQueueReceive(coco_audio_queue, &block, pdMS_TO_TICKS(20)) != pdTRUE)
            continue;

        if (!coco_audio_writer_running) break;

        size_t escritos = 0;
        const size_t bytes = AUDIO_BUFFER_SIZE * 2 * sizeof(int16_t);
        const esp_err_t err = i2s_write(
            I2S_NUM_1,
            block.pcm,
            bytes,
            &escritos,
            pdMS_TO_TICKS(30));

        if (err == ESP_OK && escritos == bytes)
            coco_audio_written++;
        else
            coco_audio_i2s_errors++;
    }

    coco_audio_writer_task = nullptr;
    vTaskDelete(nullptr);
}

static bool cocoStartAudioWriter()
{
    if (coco_audio_writer_task != nullptr && coco_audio_queue != nullptr)
        return true;

    if (coco_audio_queue == nullptr)
    {
        coco_audio_queue = xQueueCreate(COCO_AUDIO_QUEUE_BLOCKS, sizeof(CocoAudioBlock));
        if (coco_audio_queue == nullptr)
        {
            Serial.println("[NES][APU] ERROR: sin memoria para cola PCM");
            return false;
        }
    }

    xQueueReset(coco_audio_queue);
    coco_audio_queued = 0;
    coco_audio_written = 0;
    coco_audio_dropped = 0;
    coco_audio_i2s_errors = 0;
    coco_audio_max_depth = 0;
    coco_audio_writer_running = true;

    const BaseType_t ok = xTaskCreatePinnedToCore(
        cocoAudioWriterTask,
        "NES_I2S",
        4096,
        nullptr,
        3,
        &coco_audio_writer_task,
        0);

    if (ok != pdPASS)
    {
        coco_audio_writer_running = false;
        vQueueDelete(coco_audio_queue);
        coco_audio_queue = nullptr;
        coco_audio_writer_task = nullptr;
        Serial.println("[NES][APU] ERROR: no se pudo crear tarea NES_I2S");
        return false;
    }

    Serial.printf("[NES][APU] salida I2S async: cola=%u bloques, task core 0\n",
                  (unsigned)COCO_AUDIO_QUEUE_BLOCKS);
    return true;
}

static void cocoStopAudioWriter()
{
    coco_audio_writer_running = false;

    // Despertar a la tarea si estaba esperando una muestra. No importa
    // el contenido porque writer_running ya está en false.
    if (coco_audio_queue != nullptr)
    {
        CocoAudioBlock silence = {};
        xQueueSend(coco_audio_queue, &silence, 0);
    }

    // Un i2s_write en curso tiene timeout de 20 ms. Esperamos algo más
    // antes de devolver el periférico I2S a COCODRILO OS.
    for (int n = 0; n < 60 && coco_audio_writer_task != nullptr; ++n)
        vTaskDelay(pdMS_TO_TICKS(1));

    if (coco_audio_writer_task != nullptr)
    {
        vTaskDelete(coco_audio_writer_task);
        coco_audio_writer_task = nullptr;
    }

    if (coco_audio_queue != nullptr)
    {
        vQueueDelete(coco_audio_queue);
        coco_audio_queue = nullptr;
    }

    Serial.printf("[NES][APU] I2S async detenido: queued=%u out=%u drop=%u err=%u maxQ=%u/%u\n",
                  (unsigned)coco_audio_queued,
                  (unsigned)coco_audio_written,
                  (unsigned)coco_audio_dropped,
                  (unsigned)coco_audio_i2s_errors,
                  (unsigned)coco_audio_max_depth,
                  (unsigned)COCO_AUDIO_QUEUE_BLOCKS);
}
#endif

Apu2A03::Apu2A03()
{
    memset(audio_buffer, 0, sizeof(audio_buffer));
}

Apu2A03::~Apu2A03()
{
}

void Apu2A03::reset()
{
    pulse1_enable = false;
    pulse2_enable = false;
    triangle_enable = false;
    noise_enable = false;
    DMC_enable = false;
    IRQ = false;

    pulse1.len_counter.timer = 0;
    pulse2.len_counter.timer = 0;
    triangle.len_counter.timer = 0;
    noise.len_counter.timer = 0;

    DMC.output_unit.output_level = 0;
    DMC.output_unit.remaining_bits = 0;
    DMC.output_unit.shift_register = 0;
    DMC.memory_reader.address = 0;
    DMC.memory_reader.remaining_bytes = 0;
    DMC.timer = 0;
    DMC.sample_address = 0;
    DMC.sample_buffer = 0;
    DMC.sample_length = 0;
    DMC.output_unit.silence_flag = true;
    // COCODRILO: reinicio completo del estado temporal del audio.
    // Upstream no reiniciaba estos acumuladores/buffer.
    clock_counter = 0;
    pulse_hz = 0;
    prev_sample = 0;
    buffer_index = 0;
    memset(audio_buffer, 0, sizeof(audio_buffer));
}


void Apu2A03::cpuWrite(uint16_t addr, uint8_t data)
{
    coco_apu_writes++;
    if (addr == 0x4015) coco_apu_4015 = data;
    switch (addr)
    {
    case 0x4000:
        pulse1.seq.duty_cycle = ((data & 0xC0) >> 6);

        pulse1.env.loop = ((data >> 5) & 0x01);
        pulse1.len_counter.halt = ((data >> 5) & 0x01);
        pulse1.env.constant_volume = ((data >> 4) & 0x01);
        pulse1.env.volume = (data & 0x0F);
        break;

    case 0x4001:
        pulse1.sweep.enable = (data >> 7);
        pulse1.sweep.reload = ((data >> 4) & 0x07) + 1;
        pulse1.sweep.negate = ((data >> 3) & 0x01);
        pulse1.sweep.shift_count = data & 0x07;
        pulse1.sweep.reload_flag = true;
        break;

    case 0x4002: pulse1.seq.reload = (pulse1.seq.reload & 0xFF00) | data; break;

    case 0x4003:
        pulse1.seq.cycle_position = 0;
        pulse1.seq.reload = (pulse1.seq.reload & 0x00FF) | (uint16_t)((data & 0x07) << 8);
        pulse1.seq.timer = pulse1.seq.reload;
        pulse1.env.start_flag = true;

        if (pulse1_enable) pulse1.len_counter.timer = length_counter_lookup[data >> 3] + 1;

        // Restart envelope
        pulse1.env.timer = pulse1.env.volume;
        pulse1.env.decay_level_counter = 15;
        break;

    case 0x4004:
        pulse2.seq.duty_cycle = ((data & 0xC0) >> 6);

        pulse2.env.loop = ((data >> 5) & 0x01);
        pulse2.len_counter.halt = pulse2.env.loop;
        pulse2.env.constant_volume = ((data >> 4) & 0x01);
        pulse2.env.volume = (data & 0x0F);
        break;

    case 0x4005:
        pulse2.sweep.enable = (data >> 7);
        pulse2.sweep.reload = ((data >> 4) & 0x07) + 1;
        pulse2.sweep.negate = ((data >> 3) & 0x01);
        pulse2.sweep.shift_count = data & 0x07;
        pulse2.sweep.reload_flag = true;
        break;

    case 0x4006: pulse2.seq.reload = (pulse2.seq.reload & 0xFF00) | data; break;

    case 0x4007:
        pulse2.seq.cycle_position = 0;
        pulse2.seq.reload = (pulse2.seq.reload & 0x00FF) | (uint16_t)((data & 0x07) << 8);
        pulse2.seq.timer = pulse2.seq.reload;
        pulse2.env.start_flag = true;

        if (pulse2_enable) pulse2.len_counter.timer = length_counter_lookup[data >> 3] + 1;

        // Restart envelope
        pulse2.env.timer = pulse2.env.volume;
        pulse2.env.decay_level_counter = 15;
        break;

    case 0x4008:
        triangle.lin_counter.reload = data & 0x7F;
        triangle.len_counter.halt = data >> 7;
        triangle.lin_counter.control = data >> 7;
        break;

    case 0x400A: triangle.seq.reload = (triangle.seq.reload & 0xFF00) | data; break;

    case 0x400B:
        triangle.seq.reload = ((triangle.seq.reload & 0x00FF) | (uint16_t)((data & 0x07)) << 8) + 1;
        triangle.seq.timer = triangle.seq.reload;

        if (triangle_enable) triangle.len_counter.timer = length_counter_lookup[data >> 3] + 1;
        triangle.lin_counter.reload_flag = true;
        break;

    case 0x400C:
        noise.len_counter.halt = (data >> 5) & 0x01;
        noise.env.constant_volume = (data >> 4) & 0x01;
        noise.env.volume = data & 0x0F;
        break;

    case 0x400E:
        noise.mode = data >> 7;
        noise.reload = noise_period_lookup[data & 0x0F] / 2;
        break;

    case 0x400F:
        noise.env.start_flag = true;

        if (noise_enable) noise.len_counter.timer = length_counter_lookup[data >> 3] + 1;
        break;

    case 0x4010:
        DMC.IRQ_flag = data >> 7;
        DMC.loop_flag = (data & 0x40) == 0x40;
        DMC.reload = (DMC_rate_lookup[data & 0x0F] / 2) - 1;
        DMC.timer = DMC.reload;
        break;

    case 0x4011: DMC.output_unit.output_level = data & 0x7F; break;

    case 0x4012:
        DMC.sample_address = 0xC000 | ((uint32_t)data << 6);
        DMC.memory_reader.address = DMC.sample_address;
        break;

    case 0x4013:
        DMC.sample_length = (data << 4) | 0x0001;
        DMC.memory_reader.remaining_bytes = DMC.sample_length;
        break;

    case 0x4015:
        IRQ = false;
        // Pulse 1 enable
        if (data & 0x01) { pulse1_enable = true; }
        else
        {
            pulse1_enable = false;
            pulse1.len_counter.timer = 0;
        }

        // Pulse 2 enable
        if ((data >> 1) & 0x01) { pulse2_enable = true; }
        else
        {
            pulse2_enable = false;
            pulse2.len_counter.timer = 0;
        }

        // Triangle enable
        if ((data >> 2) & 0x01) { triangle_enable = true; }
        else
        {
            triangle_enable = false;
            triangle.len_counter.timer = 0;
        }

        // Noise enable
        if ((data >> 3) & 0x01) { noise_enable = true; }
        else
        {
            noise_enable = false;
            noise.len_counter.timer = 0;
        }

        // DMC enable
        if ((data >> 4) & 0x01)
        {
            DMC_enable = true;
            if (DMC.sample_buffer_empty)
            {
                setDMCBuffer();
                cpu->cycles += 3;
            }
        }
        else { DMC_enable = false; }
        break;

    case 0x4017:
        four_step_sequence_mode = ((data >> 7) == 0) ? true : false;

        if (((data >> 6) & 0x01) == 1)
        {
            IRQ = false;
            interrupt_inhibit = true;
        }
        else interrupt_inhibit = false;
        break;

    default: return;
    }
}

uint8_t Apu2A03::cpuRead(uint16_t addr)
{
    if (addr != 0x4015) return 0x00;

    uint8_t data = 0x00;
    if (pulse1.len_counter.timer > 0) data |= 0x01;
    if (pulse2.len_counter.timer > 0) data |= 0x02;
    if (triangle.len_counter.timer > 0) data |= 0x04;
    if (noise.len_counter.timer > 0) data |= 0x08;
    if (DMC.memory_reader.remaining_bytes > 0) data |= 0x10;
    if (IRQ) data |= 0x40;
    if (DMC.IRQ_flag) data |= 0x80;

    IRQ = false;
    return data;
}

void Apu2A03::setVolume(uint8_t vol)
{
    volume = vol;
}

void Apu2A03::setOutputEnabled(bool enabled)
{
#ifndef COMPOSITE_VIDEO
    if (enabled)
    {
        // I2S ya fue cedido por AudioMAX98357::entrarModoNES().
        output_enabled = cocoStartAudioWriter();
    }
    else
    {
        // Cortar primero nuevas entregas y luego esperar a que la tarea
        // I2S termine antes de devolver el periférico al sistema.
        output_enabled = false;
        cocoStopAudioWriter();
    }
#else
    output_enabled = enabled;
#endif
}

IRAM_ATTR void Apu2A03::clock()
{
    clockMany(1);
}

IRAM_ATTR void Apu2A03::clockMany(int clocks)
{
    // COCODRILO V7 - APU por eventos:
    // En vez de ejecutar 5 relojes de canal + switch + acumulador de sample
    // ~895k veces/s, saltamos entre los únicos instantes que importan:
    // - siguiente muestra de 44.1 kHz
    // - siguiente evento del frame sequencer
    //
    // Los timers de pulse/triangle/noise/DMC se avanzan matemáticamente
    // por bloques y solo iteran cuando realmente vence un timer.
    while (clocks > 0)
    {
        // ¿En cuántos ticks ocurrirá la próxima muestra?
        const uint32_t ticksSample =
            ((894886U - pulse_hz) / SAMPLE_RATE) + 1U;

        // ¿En cuántos ticks se ejecutará el próximo evento del frame sequencer?
        uint32_t ticksFrame = 0xFFFFFFFFU;
        const uint32_t cc = clock_counter;

        if (cc <= 3728U)       ticksFrame = 3728U - cc + 1U;
        else if (cc <= 7456U)  ticksFrame = 7456U - cc + 1U;
        else if (cc <= 11185U) ticksFrame = 11185U - cc + 1U;
        else if (four_step_sequence_mode) {
            if (cc <= 14914U) ticksFrame = 14914U - cc + 1U;
        } else {
            if (cc <= 18640U) ticksFrame = 18640U - cc + 1U;
        }

        uint32_t hastaEvento = ticksSample < ticksFrame ? ticksSample : ticksFrame;
        if (hastaEvento > (uint32_t)clocks) hastaEvento = (uint32_t)clocks;

        // Avanzar de golpe todos los ticks anteriores al evento.
        if (hastaEvento > 1U)
        {
            const int boring = (int)(hastaEvento - 1U);
            clockChannelsMany(boring);
            pulse_hz += (uint32_t)boring * SAMPLE_RATE;
            clock_counter += (uint32_t)boring;
            clocks -= boring;
        }

        if (clocks <= 0) break;

        // Procesar exactamente el tick de frontera con el mismo orden
        // del código original: canales -> frame sequencer -> sample -> counter++.
        clockChannelsMany(1);

        switch (clock_counter)
        {
        case 3728:
            soundChannelEnvelopeClock(pulse1.env);
            soundChannelEnvelopeClock(pulse2.env);
            soundChannelEnvelopeClock(noise.env);
            linearCounterClock(triangle.lin_counter);
            break;

        case 7456:
            soundChannelEnvelopeClock(pulse1.env);
            soundChannelEnvelopeClock(pulse2.env);
            soundChannelEnvelopeClock(noise.env);
            linearCounterClock(triangle.lin_counter);

            soundChannelSweeperClock(pulse1);
            soundChannelLengthCounterClock(pulse1.len_counter);
            soundChannelSweeperClock(pulse2);
            soundChannelLengthCounterClock(pulse2.len_counter);
            soundChannelLengthCounterClock(triangle.len_counter);
            soundChannelLengthCounterClock(noise.len_counter);
            break;

        case 11185:
            soundChannelEnvelopeClock(pulse1.env);
            soundChannelEnvelopeClock(pulse2.env);
            soundChannelEnvelopeClock(noise.env);
            linearCounterClock(triangle.lin_counter);
            break;

        case 14914:
            if (four_step_sequence_mode)
            {
                if (!interrupt_inhibit) IRQ = true;
                soundChannelEnvelopeClock(pulse1.env);
                soundChannelEnvelopeClock(pulse2.env);
                soundChannelEnvelopeClock(noise.env);
                linearCounterClock(triangle.lin_counter);

                soundChannelSweeperClock(pulse1);
                soundChannelLengthCounterClock(pulse1.len_counter);
                soundChannelSweeperClock(pulse2);
                soundChannelLengthCounterClock(pulse2.len_counter);
                soundChannelLengthCounterClock(triangle.len_counter);
                soundChannelLengthCounterClock(noise.len_counter);
                clock_counter = 0;
            }
            break;

        case 18640:
            if (!four_step_sequence_mode)
            {
                soundChannelEnvelopeClock(pulse1.env);
                soundChannelEnvelopeClock(pulse2.env);
                soundChannelEnvelopeClock(noise.env);
                linearCounterClock(triangle.lin_counter);

                soundChannelSweeperClock(pulse1);
                soundChannelLengthCounterClock(pulse1.len_counter);
                soundChannelSweeperClock(pulse2);
                soundChannelLengthCounterClock(pulse2.len_counter);
                soundChannelLengthCounterClock(triangle.len_counter);
                soundChannelLengthCounterClock(noise.len_counter);
                clock_counter = 0;
            }
            break;

        default:
            break;
        }

        pulse_hz += SAMPLE_RATE;
        if (pulse_hz > 894886U)
        {
            if (pulse1.sweep.mute || pulse1.seq.reload < 8 || pulse1.len_counter.timer == 0)
            {
                pulse1.seq.output = 0;
                pulse1.env.output = 0;
            }
            if (pulse2.sweep.mute || pulse2.seq.reload < 8 || pulse2.len_counter.timer == 0)
            {
                pulse2.seq.output = 0;
                pulse2.env.output = 0;
            }

            generateSample();
            pulse_hz -= 894886U;
        }

        clock_counter++;
        clocks--;
    }
}

IRAM_ATTR inline void Apu2A03::clockChannelsMany(int clocks)
{
    if (clocks <= 0) return;
    pulseChannelClockMany(pulse1.seq, pulse1_enable, clocks);
    pulseChannelClockMany(pulse2.seq, pulse2_enable, clocks);
    noiseChannelClockMany(noise, noise_enable, clocks);
    DMCChannelClockMany(DMC, DMC_enable, clocks);
    triangleChannelClockMany(triangle, triangle_enable, clocks);
}



// COCODRILO V7.2 - mezclador no lineal NES.
// La suma lineal anterior hacía que NOISE/DMC (percusión) dominaran y
// sonaran ásperos/"radio". Estas tablas aproximan la mezcla analógica
// pulse + TND del NES sin usar float durante gameplay.
static const uint8_t COCO_PULSE_MIX[31] = {
    0, 3, 6, 9, 11, 14, 17, 19, 22, 24, 27, 29, 31, 34, 36, 38,
    40, 42, 44, 46, 48, 50, 52, 54, 56, 57, 59, 61, 62, 64, 66,
};

static const uint8_t COCO_TND_MIX[203] = {
    0, 2, 3, 5, 7, 8, 10, 12, 13, 15, 16, 18, 20, 21, 23, 24,
    26, 27, 29, 30, 32, 33, 35, 36, 37, 39, 40, 42, 43, 44, 46, 47,
    49, 50, 51, 52, 54, 55, 56, 58, 59, 60, 61, 63, 64, 65, 66, 68,
    69, 70, 71, 72, 73, 75, 76, 77, 78, 79, 80, 81, 83, 84, 85, 86,
    87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102,
    103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 115, 116, 117,
    118, 119, 120, 121, 122, 122, 123, 124, 125, 126, 127, 127, 128, 129, 130, 131,
    132, 132, 133, 134, 135, 136, 136, 137, 138, 139, 139, 140, 141, 142, 142, 143,
    144, 145, 145, 146, 147, 148, 148, 149, 150, 150, 151, 152, 152, 153, 154, 155,
    155, 156, 157, 157, 158, 159, 159, 160, 160, 161, 162, 162, 163, 164, 164, 165,
    166, 166, 167, 167, 168, 169, 169, 170, 170, 171, 172, 172, 173, 173, 174, 175,
    175, 176, 176, 177, 177, 178, 179, 179, 180, 180, 181, 181, 182, 182, 183, 184,
    184, 185, 185, 186, 186, 187, 187, 188, 188, 189, 189,
};

IRAM_ATTR inline void Apu2A03::generateSample()
{
    const uint16_t index = (buffer_index << 1);

    const uint8_t p1 =
        (pulse1.seq.output && pulse1.len_counter.timer > 0) ? pulse1.env.output : 0;
    const uint8_t p2 =
        (pulse2.seq.output && pulse2.len_counter.timer > 0) ? pulse2.env.output : 0;

    const uint8_t tri =
        (triangle.len_counter.timer > 0 && triangle.lin_counter.counter > 0)
            ? triangle.seq.output : 0;

    const uint8_t noi =
        (!(noise.shift_register & 0x01) && noise.len_counter.timer > 0)
            ? noise.env.output : 0;

    const uint8_t dmc = DMC.output_unit.output_level;

    // Índices del mezclador clásico NES:
    // pulse = p1 + p2                         -> 0..30
    // TND   = 3*triangle + 2*noise + DMC     -> 0..202
    const uint8_t pulseIndex = (uint8_t)(p1 + p2);
    uint16_t tndIndex =
        (uint16_t)tri * 3U +
        (uint16_t)noi * 2U +
        (uint16_t)dmc;
    if (tndIndex > 202U) tndIndex = 202U;

    uint16_t sample =
        (uint16_t)COCO_PULSE_MIX[pulseIndex] +
        (uint16_t)COCO_TND_MIX[tndIndex];

    // Aplicar volumen del sistema después de la mezcla no lineal.
    uint32_t temp = (uint32_t)sample * (uint32_t)volume;
    sample = (uint16_t)((temp + 50U) / 100U);
    if (sample > 255U) sample = 255U;

    // Filtro muy leve: 75% muestra nueva + 25% anterior.
    // La versión antigua 50/50 apagaba demasiado los transitorios.
    sample = (uint16_t)((sample * 3U + prev_sample) >> 2);
    prev_sample = sample;

    sample <<= 8;
    audio_buffer[index] = sample;
    audio_buffer[index + 1] = sample;

    buffer_index++;
    if (buffer_index >= AUDIO_BUFFER_SIZE)
    {
        buffer_index = 0;
        writeBuffer();
    }
}


inline void Apu2A03::writeBuffer()
{
#ifndef COMPOSITE_VIDEO
    static CocoAudioBlock block;
    static int32_t dc_q8 = 0;
    static bool dc_inicializado = false;
    static uint32_t buffer_count = 0;

    uint8_t min8 = 255;
    uint8_t max8 = 0;
    uint32_t sum8 = 0;
    uint16_t nonzero = 0;

    for (uint16_t i = 0; i < AUDIO_BUFFER_SIZE; ++i)
    {
        const uint8_t s8 = (uint8_t)(audio_buffer[i << 1] >> 8);

        if (s8 < min8) min8 = s8;
        if (s8 > max8) max8 = s8;
        sum8 += s8;
        if (s8 != 0) nonzero++;

        // Anemoia genera una señal unipolar. Para el MAX98357A eliminamos
        // lentamente el componente DC y conservamos la forma de onda.
        const int32_t x_q8 = ((int32_t)s8) << 8;
        if (!dc_inicializado)
        {
            dc_q8 = x_q8;
            dc_inicializado = true;
        }

        dc_q8 += (x_q8 - dc_q8) >> 8;

        // COCODRILO V7.1 - salida con soft-knee:
        // La versión anterior cortaba en seco a +/-14000. Eso aplastaba
        // especialmente transitorios de NOISE/DMC (percusión), mientras que
        // las melodías se oían relativamente bien.
        //
        // Bajamos un poco la ganancia y, a partir de 12000, comprimimos los
        // picos 4:1 en lugar de recortarlos de golpe.
        int32_t y = ((x_q8 - dc_q8) * 5) / 4;

        const int32_t signo = (y < 0) ? -1 : 1;
        int32_t a = (y < 0) ? -y : y;

        constexpr int32_t KNEE = 12000;
        constexpr int32_t LIMITE = 22000;

        if (a > KNEE) {
            a = KNEE + ((a - KNEE) >> 2);  // compresión suave 4:1
        }
        if (a > LIMITE) a = LIMITE;

        y = signo * a;
        const int16_t out = (int16_t)y;
        block.pcm[i << 1] = out;
        block.pcm[(i << 1) + 1] = out;
    }

    // IMPORTANTÍSIMO: aquí ya NO se llama i2s_write(). El hilo de
    // emulación nunca espera al periférico de audio. Encolamos sin bloqueo.
    if (output_enabled && coco_audio_queue != nullptr)
    {
        if (xQueueSend(coco_audio_queue, &block, 0) == pdTRUE)
        {
            coco_audio_queued++;
        }
        else
        {
            // Si por una pausa puntual la cola se llenó, descartamos el
            // bloque más viejo y conservamos el audio reciente. Esto evita
            // que el audio pueda volver a frenar el reloj emulado.
            CocoAudioBlock descartado;
            xQueueReceive(coco_audio_queue, &descartado, 0);
            coco_audio_dropped++;
            if (xQueueSend(coco_audio_queue, &block, 0) == pdTRUE)
                coco_audio_queued++;
        }

        const UBaseType_t depth = uxQueueMessagesWaiting(coco_audio_queue);
        if (depth > coco_audio_max_depth) coco_audio_max_depth = depth;
    }

    buffer_count++;
    if (buffer_count == 1 || (buffer_count % 512) == 0)
    {
        const unsigned avg = (unsigned)(sum8 / AUDIO_BUFFER_SIZE);
        const unsigned qdepth = coco_audio_queue
            ? (unsigned)uxQueueMessagesWaiting(coco_audio_queue) : 0U;
        Serial.printf(
          "[NES][APU] buf=%u min=%u max=%u avg=%u nz=%u/%u apuw=%u $4015=%02X q=%u/%u queued=%u out=%u drop=%u err=%u\n",
          (unsigned)buffer_count,
          (unsigned)min8,
          (unsigned)max8,
          avg,
          (unsigned)nonzero,
          (unsigned)AUDIO_BUFFER_SIZE,
          (unsigned)coco_apu_writes,
          (unsigned)coco_apu_4015,
          qdepth,
          (unsigned)COCO_AUDIO_QUEUE_BLOCKS,
          (unsigned)coco_audio_queued,
          (unsigned)coco_audio_written,
          (unsigned)coco_audio_dropped,
          (unsigned)coco_audio_i2s_errors
        );
    }
#else
    while (cv_audio_buffer_full(AUDIO_BUFFER_SIZE)) vTaskDelay(1);
    cv_audio_write_16((const uint16_t*)audio_buffer, AUDIO_BUFFER_SIZE, 2);
#endif
}


IRAM_ATTR inline void Apu2A03::pulseChannelClockMany(
    sequencerUnit& seq, bool enable, int clocks)
{
    if (!enable || clocks <= 0) return;

    uint32_t left = (uint32_t)clocks;
    while (left > 0)
    {
        const uint32_t toEvent = (uint32_t)seq.timer + 1U;
        if (left < toEvent)
        {
            seq.timer = (uint16_t)(seq.timer - left);
            return;
        }

        left -= toEvent;
        seq.timer = seq.reload;
        seq.output = duty_sequences[seq.duty_cycle][seq.cycle_position];
        seq.cycle_position = (seq.cycle_position + 1U) & 7U;
    }
}

IRAM_ATTR inline void Apu2A03::triangleChannelClockMany(
    triangleChannel& tri, bool enable, int clocks)
{
    if (!enable || clocks <= 0) return;

    uint32_t subLeft = (uint32_t)clocks * 2U;
    uint32_t consumed = 0;

    while (subLeft > 0)
    {
        const uint32_t toEvent =
            tri.seq.timer == 0 ? 65536U : (uint32_t)tri.seq.timer;

        if (subLeft < toEvent)
        {
            tri.seq.timer = (uint16_t)(tri.seq.timer - subLeft);
            return;
        }

        subLeft -= toEvent;
        consumed += toEvent;
        tri.seq.timer = tri.seq.reload;

        const bool countersOK =
            tri.len_counter.timer > 0 && tri.lin_counter.counter > 0;

        if (countersOK)
        {
            if (tri.seq.reload >= 2)
            {
                tri.seq.output = triangle_sequence[tri.seq.duty_cycle];
                tri.seq.duty_cycle = (tri.seq.duty_cycle + 1U) & 31U;
            }
        }
        else
        {
            // El código original hace "return" desde triangleChannelClock()
            // al vencer el timer sin counters. Si ocurrió en la primera
            // mitad del tick APU, hay que saltarse la segunda mitad.
            if ((consumed & 1U) != 0U && subLeft > 0U)
            {
                subLeft--;
                consumed++;
            }
        }
    }
}

IRAM_ATTR inline void Apu2A03::noiseChannelClockMany(
    noiseChannel& n, bool enable, int clocks)
{
    if (!enable || clocks <= 0) return;

    uint32_t left = (uint32_t)clocks;
    while (left > 0)
    {
        const uint32_t toEvent = (uint32_t)n.timer + 1U;
        if (left < toEvent)
        {
            n.timer = (uint16_t)(n.timer - left);
            return;
        }

        left -= toEvent;
        n.timer = n.reload;

        const uint8_t tap =
            n.mode ? (uint8_t)((n.shift_register >> 6) & 0x01)
                   : (uint8_t)((n.shift_register >> 1) & 0x01);

        n.output = (uint8_t)((n.shift_register & 0x01) ^ tap);
        n.shift_register >>= 1;
        n.shift_register |= (uint16_t)n.output << 14;
    }
}

IRAM_ATTR inline void Apu2A03::DMCChannelClockMany(
    DMCChannel& dmc, bool enable, int clocks)
{
    if (!enable || clocks <= 0) return;

    uint32_t left = (uint32_t)clocks;
    while (left > 0)
    {
        const uint32_t toEvent = (uint32_t)dmc.timer + 1U;
        if (left < toEvent)
        {
            dmc.timer = (uint16_t)(dmc.timer - left);
            return;
        }

        left -= toEvent;
        dmc.timer = dmc.reload + 1U;

        if (!dmc.output_unit.silence_flag)
        {
            if (dmc.output_unit.shift_register & 0x01)
            {
                if (dmc.output_unit.output_level <= 125)
                    dmc.output_unit.output_level += 2;
            }
            else
            {
                if (dmc.output_unit.output_level >= 2)
                    dmc.output_unit.output_level -= 2;
            }

            dmc.output_unit.shift_register >>= 1;
        }

        dmc.output_unit.remaining_bits--;
        if (dmc.output_unit.remaining_bits <= 0)
        {
            dmc.output_unit.remaining_bits = 8;

            if (dmc.sample_buffer_empty)
            {
                dmc.output_unit.silence_flag = true;
            }
            else
            {
                dmc.output_unit.silence_flag = false;
                dmc.output_unit.shift_register = dmc.sample_buffer;
                dmc.sample_buffer_empty = true;
                setDMCBuffer();
                cpu->cycles += 4;
            }
        }
    }
}

IRAM_ATTR inline void Apu2A03::pulseChannelClock(sequencerUnit& seq, bool enable)
{
    if (!enable) return;

    seq.timer--;
    if (seq.timer == 0xFFFF)
    {
        seq.timer = seq.reload;
        // Shift duty cycle with wrapping
        seq.output = duty_sequences[seq.duty_cycle][seq.cycle_position];
        seq.cycle_position = (seq.cycle_position + 1) & 7;
    }
}

IRAM_ATTR inline void Apu2A03::triangleChannelClock(triangleChannel& triangle, bool enable)
{
    if (!enable) return; // Temp

    for (int i = 0; i < 2; i++)
    {
        triangle.seq.timer--;
        if (triangle.seq.timer == 0)
        {
            triangle.seq.timer = triangle.seq.reload;
            if (!(triangle.len_counter.timer > 0 && triangle.lin_counter.counter > 0)) return;

            if (triangle.seq.reload >= 2)
            {
                triangle.seq.output = triangle_sequence[triangle.seq.duty_cycle];
                triangle.seq.duty_cycle = (triangle.seq.duty_cycle + 1) & 31;
            }
        }
    }
}

IRAM_ATTR inline void Apu2A03::noiseChannelClock(noiseChannel& noise, bool enable)
{
    if (!enable) return; // Temp

    noise.timer--;
    if (noise.timer == 0xFFFF)
    {
        noise.timer = noise.reload;
        uint8_t temp =
            noise.mode ? (noise.shift_register >> 6) & 0x01 : (noise.shift_register >> 1) & 0x01;
        noise.output = (noise.shift_register & 0x01) ^ (temp);
        noise.shift_register >>= 1;
        noise.shift_register |= noise.output << 14;
    }
}

IRAM_ATTR inline void Apu2A03::DMCChannelClock(DMCChannel& DMC, bool enable)
{
    if (!enable) return;

    DMC.timer--;
    if (DMC.timer == 0xFFFF)
    {
        DMC.timer = DMC.reload + 1;
        if (DMC.output_unit.silence_flag == false)
        {
            if (DMC.output_unit.shift_register & 0x01)
            {
                if (DMC.output_unit.output_level <= 125) DMC.output_unit.output_level += 2;
            }
            else
            {
                if (DMC.output_unit.output_level >= 2) DMC.output_unit.output_level -= 2;
            }

            DMC.output_unit.shift_register >>= 1;
        }

        // Update Bits remaining counter
        DMC.output_unit.remaining_bits--;
        if (DMC.output_unit.remaining_bits <= 0)
        {
            DMC.output_unit.remaining_bits = 8;

            if (DMC.sample_buffer_empty) { DMC.output_unit.silence_flag = true; }
            else
            {
                DMC.output_unit.silence_flag = false;
                DMC.output_unit.shift_register = DMC.sample_buffer;
                DMC.sample_buffer_empty = true;
                setDMCBuffer();
                cpu->cycles += 4;
            }
        }
    }
}

inline void Apu2A03::soundChannelEnvelopeClock(envelopeUnit& envelope)
{
    if (envelope.start_flag)
    {
        envelope.start_flag = false;
        envelope.decay_level_counter = 15;
        envelope.timer = envelope.volume + 1;
    }
    else
    {
        envelope.timer--;
        if (envelope.timer == 0)
        {
            envelope.timer = envelope.volume + 1;
            if (envelope.decay_level_counter > 0) envelope.decay_level_counter--;
            else if (envelope.loop) envelope.decay_level_counter = 15;
        }
    }

    if (envelope.constant_volume) envelope.output = envelope.volume;
    else envelope.output = envelope.decay_level_counter;
}

inline void Apu2A03::soundChannelSweeperClock(pulseChannel& channel)
{
    // Calculate the target period
    channel.sweep.change = (int16_t)(channel.seq.reload >> channel.sweep.shift_count);
    // Negate change if negate flag is true
    // Pulse 1 adds one's complement = -c - 1
    // Pulse 2 adds two's complement = -c
    if (channel.sweep.negate && channel.sweep.pulse_channel_number == 1)
        channel.sweep.change = (int16_t)(-channel.sweep.change - 1);
    else if (channel.sweep.negate && channel.sweep.pulse_channel_number == 2)
        channel.sweep.change = (int16_t)(-channel.sweep.change);

    channel.sweep.target_period = (int16_t)(channel.seq.reload + channel.sweep.change);
    if (channel.sweep.target_period < 0) channel.sweep.target_period = 0;

    // Check if channel should be muted
    if (channel.seq.reload < 8) channel.sweep.mute = true;
    else if (channel.sweep.target_period > 0x7FF) channel.sweep.mute = true;
    else channel.sweep.mute = false;

    channel.sweep.timer--;
    if (channel.sweep.enable && channel.sweep.timer == 0 && channel.sweep.shift_count != 0)
    {
        if (!channel.sweep.mute) channel.seq.reload = channel.sweep.target_period;
    }
    if (channel.sweep.timer == 0 || channel.sweep.reload_flag)
    {
        channel.sweep.timer = channel.sweep.reload;
        channel.sweep.reload_flag = false;
    }
}

inline void Apu2A03::soundChannelLengthCounterClock(length_counter& len_counter)
{
    if (!len_counter.halt && len_counter.timer > 0) len_counter.timer--;
}

inline void Apu2A03::linearCounterClock(linear_counter& lin_counter)
{
    if (lin_counter.reload_flag) lin_counter.counter = lin_counter.reload;
    else if (lin_counter.counter > 0) lin_counter.counter--;

    if (lin_counter.control == 0) lin_counter.reload_flag = false;
}

inline void Apu2A03::setDMCBuffer()
{
    uint8_t value = bus->cpuRead(DMC.memory_reader.address);
    if (DMC.memory_reader.remaining_bytes == 0) return;

    DMC.sample_buffer = value;
    DMC.sample_buffer_empty = false;

    DMC.memory_reader.address++;
    if (DMC.memory_reader.address == 0x0000) DMC.memory_reader.address = 0x8000;

    DMC.memory_reader.remaining_bytes--;
    if (DMC.memory_reader.remaining_bytes == 0)
    {
        if (DMC.loop_flag)
        {
            // Restart sample
            DMC.memory_reader.address = DMC.sample_address;
            DMC.memory_reader.remaining_bytes = DMC.sample_length;
        }
        else if (DMC.IRQ_flag) IRQ = true;
    }
}


