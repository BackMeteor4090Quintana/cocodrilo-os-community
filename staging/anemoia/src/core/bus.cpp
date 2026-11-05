#pragma GCC optimize ("Ofast","unroll-loops")
#include "esp_timer.h"
#include "esp_cpu.h"
#include "bus.h"

Bus::Bus()
{
    memset(RAM, 0, sizeof(RAM));
    cpu.connectBus(this);
    cpu.apu.connectBus(this);
    cpu.apu.connectCPU(&cpu);
    ppu.connectBus(this);
}

Bus::~Bus()
{
}

IRAM_ATTR void Bus::cpuWrite(uint16_t addr, uint8_t data)
{
    if (cart->cpuWrite(addr, data)) {}
    else if ((addr & 0xE000) == 0x0000) { RAM[addr & 0x07FF] = data; }
    else if ((addr & 0xE000) == 0x2000) { ppu.cpuWrite(addr & 0x0007, data); }
    else if (addr == 0x4014) { cpu.OAM_DMA(data); }
    else if ((addr & 0xF000) == 0x4000 && (addr <= 0x4013 || addr == 0x4015 || addr == 0x4017))
    {
        cpu.apuWrite(addr, data);
    }
    else if (addr == 0x4016)
    {
        controller_strobe = data & 1;
        if (controller_strobe) { controller_state = controller; }
    }
}

IRAM_ATTR uint8_t Bus::cpuRead(uint16_t addr)
{
    uint8_t data = 0x00;

    if (cart->cpuRead(addr, data)) {}
    else if ((addr & 0xE000) == 0x0000) { data = RAM[addr & 0x07FF]; }
    else if ((addr & 0xE000) == 0x2000) { data = ppu.cpuRead(addr & 0x0007); }
    else if (addr == 0x4015)
    {
        data = cpu.apu.cpuRead(addr);
    }
    else if (addr == 0x4016)
    {
        uint8_t value = controller_state & 1;
        if (!controller_strobe) controller_state >>= 1;
        data = value | 0x40;
    }
    return data;
}

void Bus::reset()
{
    for (auto& i : RAM) i = 0x00;
    cart->reset();
    cpu.reset();
    ppu.reset();
}

// Diagnóstico temporal de video: tiempo realmente gastado esperando DMA
// y tiempo de encolar cada pushPixelsDMA durante el frame actual.
static uint32_t coco_tft_wait_frame_us = 0;
static uint32_t coco_tft_push_frame_us = 0;

IRAM_ATTR void Bus::clock()
{
    // DIAGNOSTICO COCODRILO:
    // Con FRAMESKIP, un Bus::clock() renderiza y el siguiente omite el render.
    // Midiendo ambos por separado sabremos si el cuello de botella está
    // principalmente en CPU/APU o en PPU/render/TFT, con solo 2 lecturas
    // de temporizador por frame.
    const int64_t perfInicioUs = esp_timer_get_time();
    ppu.resetPerfCounters();
    uint32_t perfCpuCycles = 0;

    // 1 frame == 341 dots * 261 scanlines
    // Visible scanlines 0-239

    // Rendering 3 scanlines at a time because 1 CPU clock == 3 PPU clocks
    // and there's only 341 ppu clocks (dots) in a scanline, which is not divisible by 3.
    // Using a counter/for loop with += 341 & -= 3 is too big of a performance hit.
    // 1 scanline == ~113.67 CPU clocks, so for every 3 scanlines, two scanlines will have an extra
    // CPU clock

    static bool frame_latch = false;
    for (int ppu_scanline = 0; ppu_scanline < 240; ppu_scanline += 3)
    {
        { const uint32_t c = esp_cpu_get_cycle_count(); cpu.clock(113); perfCpuCycles += (uint32_t)(esp_cpu_get_cycle_count() - c); }
        if (!frame_latch) ppu.renderScanline(ppu_scanline);
        else ppu.fakeSpriteHit(ppu_scanline);

        { const uint32_t c = esp_cpu_get_cycle_count(); cpu.clock(114); perfCpuCycles += (uint32_t)(esp_cpu_get_cycle_count() - c); }
        if (!frame_latch) ppu.renderScanline(ppu_scanline + 1);
        else ppu.fakeSpriteHit(ppu_scanline + 1);

        { const uint32_t c = esp_cpu_get_cycle_count(); cpu.clock(114); perfCpuCycles += (uint32_t)(esp_cpu_get_cycle_count() - c); }
        if (!frame_latch) ppu.renderScanline(ppu_scanline + 2);
        else ppu.fakeSpriteHit(ppu_scanline + 2);
    }

    // Setup for the next frame
    // Same reason as scanlines 0-239, 2/3 of scanlines will have an extra CPU clock.
    // Scanline 240
    { const uint32_t c = esp_cpu_get_cycle_count(); cpu.clock(113); perfCpuCycles += (uint32_t)(esp_cpu_get_cycle_count() - c); }

    // Scanline 241-261
    ppu.setVBlank();
    { const uint32_t c = esp_cpu_get_cycle_count(); cpu.clock(2501); perfCpuCycles += (uint32_t)(esp_cpu_get_cycle_count() - c); }

    ppu.clearVBlank();
    { const uint32_t c = esp_cpu_get_cycle_count(); cpu.clock(114); perfCpuCycles += (uint32_t)(esp_cpu_get_cycle_count() - c); }

#ifdef FRAMESKIP
    // frame_latch todavía describe el frame que acabamos de procesar:
    // false = render real, true = frame omitido/fakeSpriteHit.
    const bool perfFueRender = !frame_latch;
    frame_latch = !frame_latch;
#else
    const bool perfFueRender = true;
#endif

    const uint32_t perfUs = (uint32_t)(esp_timer_get_time() - perfInicioUs);

    const uint32_t perfTftWaitUs = coco_tft_wait_frame_us;
    const uint32_t perfTftPushUs = coco_tft_push_frame_us;
    const uint32_t perfTftUs = perfTftWaitUs + perfTftPushUs;
    const uint32_t perfCoreUs = (perfUs > perfTftUs) ? (perfUs - perfTftUs) : 0;

    static uint64_t perfRenderSumaUs = 0;
    static uint64_t perfSkipSumaUs = 0;
    static uint32_t perfRenderN = 0;
    static uint32_t perfSkipN = 0;
    static uint32_t perfRenderMinUs = UINT32_MAX;
    static uint32_t perfRenderMaxUs = 0;
    static uint32_t perfSkipMinUs = UINT32_MAX;
    static uint32_t perfSkipMaxUs = 0;
    static uint64_t perfCoreSumaUs = 0;
    static uint64_t perfTftWaitSumaUs = 0;
    static uint64_t perfTftPushSumaUs = 0;
    static uint64_t perfCpuCyclesSuma = 0;
    static uint64_t perfBgCyclesSuma = 0;
    static uint64_t perfSpritesCyclesSuma = 0;
    static uint64_t perfFinishCyclesSuma = 0;

    if (perfFueRender)
    {
        perfRenderSumaUs += perfUs;
        perfCoreSumaUs += perfCoreUs;
        perfTftWaitSumaUs += perfTftWaitUs;
        perfTftPushSumaUs += perfTftPushUs;
        perfCpuCyclesSuma += perfCpuCycles;
        perfBgCyclesSuma += ppu.perf_bg_cycles;
        perfSpritesCyclesSuma += ppu.perf_sprites_cycles;
        perfFinishCyclesSuma += ppu.perf_finish_cycles;
        perfRenderN++;
        if (perfUs < perfRenderMinUs) perfRenderMinUs = perfUs;
        if (perfUs > perfRenderMaxUs) perfRenderMaxUs = perfUs;
    }
    else
    {
        perfSkipSumaUs += perfUs;
        perfSkipN++;
        if (perfUs < perfSkipMinUs) perfSkipMinUs = perfUs;
        if (perfUs > perfSkipMaxUs) perfSkipMaxUs = perfUs;
    }

    // Aproximadamente cada 120 llamadas (~4 s a los ~30 FPS actuales).
    if ((perfRenderN + perfSkipN) >= 120)
    {
        const float renderAvgMs = perfRenderN
            ? (float)perfRenderSumaUs / (float)perfRenderN / 1000.0f : 0.0f;
        const float skipAvgMs = perfSkipN
            ? (float)perfSkipSumaUs / (float)perfSkipN / 1000.0f : 0.0f;

        Serial.printf(
            "[NES][PERF] render avg=%.2f ms min=%.2f max=%.2f | "
            "skip avg=%.2f ms min=%.2f max=%.2f\n",
            renderAvgMs,
            perfRenderN ? perfRenderMinUs / 1000.0f : 0.0f,
            perfRenderN ? perfRenderMaxUs / 1000.0f : 0.0f,
            skipAvgMs,
            perfSkipN ? perfSkipMinUs / 1000.0f : 0.0f,
            perfSkipN ? perfSkipMaxUs / 1000.0f : 0.0f
        );

        if (perfRenderN) {
            Serial.printf(
                "[NES][PERF2] core+ppu avg=%.2f ms | dmaWait avg=%.2f ms | dmaPush avg=%.2f ms | tft total=%.2f ms\n",
                (float)perfCoreSumaUs / (float)perfRenderN / 1000.0f,
                (float)perfTftWaitSumaUs / (float)perfRenderN / 1000.0f,
                (float)perfTftPushSumaUs / (float)perfRenderN / 1000.0f,
                (float)(perfTftWaitSumaUs + perfTftPushSumaUs) / (float)perfRenderN / 1000.0f
            );
        }

        if (perfRenderN) {
            const float ciclosPorMs = (float)ESP.getCpuFreqMHz() * 1000.0f;
            Serial.printf(
                "[NES][PERF3] cpu+apu=%.2f ms | bg=%.2f ms | sprites=%.2f ms | finish=%.2f ms\n",
                (float)perfCpuCyclesSuma / (float)perfRenderN / ciclosPorMs,
                (float)perfBgCyclesSuma / (float)perfRenderN / ciclosPorMs,
                (float)perfSpritesCyclesSuma / (float)perfRenderN / ciclosPorMs,
                (float)perfFinishCyclesSuma / (float)perfRenderN / ciclosPorMs
            );
        }

        perfRenderSumaUs = 0;
        perfSkipSumaUs = 0;
        perfCoreSumaUs = 0;
        perfTftWaitSumaUs = 0;
        perfTftPushSumaUs = 0;
        perfCpuCyclesSuma = 0;
        perfBgCyclesSuma = 0;
        perfSpritesCyclesSuma = 0;
        perfFinishCyclesSuma = 0;
        perfRenderN = 0;
        perfSkipN = 0;
        perfRenderMinUs = UINT32_MAX;
        perfRenderMaxUs = 0;
        perfSkipMinUs = UINT32_MAX;
        perfSkipMaxUs = 0;
    }
}

IRAM_ATTR void Bus::setPPUMirrorMode(Cartridge::MIRROR mirror)
{
    ppu.setMirror(mirror);
}

Cartridge::MIRROR Bus::getPPUMirrorMode()
{
    return ppu.getMirror();
}

IRAM_ATTR void Bus::OAM_Write(uint8_t addr, uint8_t data)
{
    ppu.ptr_sprite[addr] = data;
}

void Bus::insertCartridge(Cartridge* cartridge)
{
    cart = cartridge;
    cpu.connectCartridge(cartridge);
    ppu.connectCartridge(cartridge);
    cart->connectBus(this);
}

void Bus::connectScreen(TFT_eSPI* screen)
{
    ptr_screen = screen;
}

void Bus::connectFramebuffer(uint8_t* framebuffer)
{
    ppu.connectFramebuffer(framebuffer);
}

IRAM_ATTR void Bus::renderImage(uint16_t scanline)
{
#ifndef COMPOSITE_VIDEO
    if (scanline == 0)
    {
        coco_tft_wait_frame_us = 0;
        coco_tft_push_frame_us = 0;

        #ifndef DISABLE_DMA
        const int64_t t0 = esp_timer_get_time();
        ptr_screen->dmaWait();
        coco_tft_wait_frame_us += (uint32_t)(esp_timer_get_time() - t0);
        #endif

        ptr_screen->setAddrWindow(32, 0, 256, 240);
    }

    #ifndef DISABLE_DMA
    // Esperar únicamente antes de reutilizar el buffer que puede seguir en DMA.
    const int64_t tw = esp_timer_get_time();
    ptr_screen->dmaWait();
    coco_tft_wait_frame_us += (uint32_t)(esp_timer_get_time() - tw);

    const int64_t tp = esp_timer_get_time();
    ptr_screen->pushPixelsDMA(ppu.ptr_display, 256 * SCANLINES_PER_BUFFER);
    coco_tft_push_frame_us += (uint32_t)(esp_timer_get_time() - tp);
    #else
    const int64_t tp = esp_timer_get_time();
    ptr_screen->pushPixels(ppu.ptr_display, 256 * SCANLINES_PER_BUFFER);
    coco_tft_push_frame_us += (uint32_t)(esp_timer_get_time() - tp);
    #endif
#endif
}

IRAM_ATTR void Bus::IRQ()
{
    cpu.IRQ();
}

IRAM_ATTR void Bus::NMI()
{
    cpu.NMI();
}

void Bus::saveState()
{
    if (!SD_MMC.exists("/states")) SD_MMC.mkdir("/states");
    uint32_t CRC32 = cart->CRC32;

    static char CRC32_str[9];
    sprintf(CRC32_str, "%08lX", (unsigned long)CRC32);

    static char filename[32];
    sprintf(filename, "/states/%s.state", CRC32_str);

    fs::File state = SD_MMC.open(filename, FILE_WRITE);
    if (!state) return;

    // Header for verification - ANEMOIA + CRC32
    state.print("ANEMOIA");
    state.write((const uint8_t*)CRC32_str, 8);

    // Dump state
    state.write(RAM, sizeof(RAM));
    cpu.dumpState(state);
    ppu.dumpState(state);
    cart->dumpState(state);

    state.close();
}

void Bus::loadState()
{
    uint32_t CRC32 = cart->CRC32;

    static char CRC32_str[9];
    sprintf(CRC32_str, "%08lX", (unsigned long)CRC32);

    static char filename[32];
    sprintf(filename, "/states/%s.state", CRC32_str);
    if (!SD_MMC.exists(filename)) return;

    fs::File state = SD_MMC.open(filename, FILE_READ);
    if (!state) return;

    // Verify header
    static char header[8];
    static char CRC[9];
    state.read((uint8_t*)&header, 7);
    header[7] = '\0';
    state.read((uint8_t*)&CRC, 8);
    CRC[8] = '\0';

    if (strcmp(header, "ANEMOIA") != 0)
    {
        state.close();
        return;
    }
    if (strcmp(CRC, CRC32_str) != 0)
    {
        state.close();
        return;
    }

    // Load state
    state.read(RAM, sizeof(RAM));
    cpu.loadState(state);
    ppu.loadState(state);
    cart->loadState(state);

    state.close();
}


