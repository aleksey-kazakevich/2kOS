#include <Drivers/Hpet.h>
#include <Time/Timer.h>
#include <Asm/Cpu.h>
#include <Asm/Io.h>

STATIC UINT64 TscPerMs = 0;

/* ============================================================================
 * PIT calibration
 * ============================================================================ */

#define PIT_FREQ 1193182ULL
#define PIT_CH0  0x40
#define PIT_CMD  0x43

// Однократный режим (mode 0), 16-битный доступ
STATIC VOID PitPrepareOneshot(VOID) {
    // Channel 0, lobyte/hibyte, mode 0 (interrupt on terminal count), binary
    Outb(PIT_CMD, 0x30);
}

STATIC VOID PitSetCount(UINT16 Count) {
    Outb(PIT_CH0, (UINT8)(Count & 0xFF));
    Outb(PIT_CH0, (UINT8)((Count >> 8) & 0xFF));
}

// Читает текущее значение счётчика PIT (через latch)
STATIC UINT16 PitReadCount(VOID) {
    // Latch channel 0: 0b00000000 → channel 0, latch command
    Outb(PIT_CMD, 0x00);
    UINT8 Lo = Inb(PIT_CH0);
    UINT8 Hi = Inb(PIT_CH0);
    return (UINT16)(Lo | ((UINT16)Hi << 8));
}

// Калибровка через PIT: запускаем счётчик на N мс, меряем TSC
STATIC BOOL TscCalibrateWithPit(VOID) {
    const UINT32 CalMs = 50;                    // 50 мс — достаточно точно
    const UINT32 PitTicks = (PIT_FREQ * CalMs) / 1000;

    // PIT 16-bit, макс 65535
    if (PitTicks == 0 || PitTicks > 0xFFFF) {
        return FALSE;
    }

    // Отключаем динамик / порт B (не критично, но пусть будет)
    UINT8 Port61 = Inb(0x61);
    Outb(0x61, (Port61 & 0xFC) | 0x01);    // включить таймер 2 гейт
    Outb(0x61, Port61);                     // восстановить

    // Настраиваем PIT на oneshot
    PitPrepareOneshot();
    PitSetCount((UINT16)PitTicks);

    // Ждём, пока счётчик не дойдёт до 0
    UINT64 TscStart = ReadTimeStampCounter();

    // Ждём завершения: читаем счётчик, пока не станет маленьким
    // PIT счётчик считает ВНИЗ. Мы ждём, пока он не дойдёт до 0.
    UINT16 Prev = 0xFFFF;
    UINT32 Stall = 0;
    while (1) {
        UINT16 Curr = PitReadCount();
        if (Curr == 0) break;               // счётчик достиг 0

        // Если значение вдруг выросло — значит, мы поймали перезагрузку
        if (Curr > Prev + 100) break;

        Prev = Curr;

        if (++Stall > 100000000) {
            return FALSE;                   // защита от зависания
        }
    }

    UINT64 TscEnd = ReadTimeStampCounter();
    UINT64 TscDelta = TscEnd - TscStart;

    if (TscDelta < 100000) {
        return FALSE;                       // слишком мало — что-то не так
    }

    // TscPerMs = TscDelta / CalMs
    TscPerMs = TscDelta / CalMs;

    // Проверка разумности: от 100 МГц до 10 ГГц
    if (TscPerMs < 100000 || TscPerMs > 10000000) {
        TscPerMs = 0;
        return FALSE;
    }

    return TRUE;
}

/* ============================================================================
 * Инициализация (вызывается один раз при старте таймера)
 * ============================================================================ */

VOID TimerDelayInit(VOID) {
    if (TscPerMs != 0) return;

    // 1. Попробуем HPET
    if (HpetIsAvailable()) {
        UINT64 TscFreq = HpetCalibrateTsc();    // возвращает Гц
        if (TscFreq > 0) {
            TscPerMs = TscFreq / 1000;
            return;
        }
    }

    // 2. Fallback на PIT
    if (TscCalibrateWithPit()) {
        return;
    }

    // 3. Совсем крайний случай — но без магических 2 ГГц.
    //    Оставим TscPerMs = 0 и будем использовать HpetDelayMs
    //    напрямую, если HPET доступен.
    TscPerMs = 0;
}

/* ============================================================================
 * Задержки
 * ============================================================================ */

VOID TimerUdelay(UINT32 Us) {
    if (Us == 0) return;

    // Если HPET доступен — используем его (точно и без калибровки)
    if (HpetIsAvailable()) {
        HpetDelayUs(Us);
        return;
    }

    // Иначе — TSC
    if (TscPerMs == 0) {
        TimerDelayInit();
    }

    if (TscPerMs == 0) {
        // TSC не откалиброван — грубая задержка через цикл
        // Оценим скорость: ~1000 итераций на 1 мкс на 2 ГГц.
        // Это плохо, но лучше, чем ничего.
        UINT64 Loops = (UINT64)Us * 1000;
        while (Loops--) {
            CpuPause();
        }
        return;
    }

    UINT64 Start = ReadTimeStampCounter();
    UINT64 Need = (TscPerMs * Us) / 1000;
    if (Need == 0) Need = 1;

    while (ReadTimeStampCounter() - Start < Need) {
        CpuPause();
    }
}

VOID TimerMdelay(UINT32 Ms) {
    if (Ms == 0) return;

    // Если HPET доступен — используем его
    if (HpetIsAvailable()) {
        HpetDelayMs(Ms);
        return;
    }

    // Иначе через микросекунды (TSC)
    while (Ms >= 1000) {
        TimerUdelay(1000 * 1000);
        Ms -= 1000;
    }
    if (Ms > 0) {
        TimerUdelay(Ms * 1000);
    }
}

VOID TimerSdelay(UINT32 S) {
    while (S--) {
        TimerMdelay(1000);
    }
}

VOID TimerSleep(UINT32 Ms) {
    if (Ms == 0) return;

    UINT32 Freq = TimerFreq();
    if (Freq == 0) {
        TimerMdelay(Ms);
        return;
    }

    UINT64 Start = TimerTicks();
    UINT64 Need = ((UINT64)Ms * Freq) / 1000;
    if (Need == 0) Need = 1;

    while ((TimerTicks() - Start) < Need) {
        Halt();
    }
}