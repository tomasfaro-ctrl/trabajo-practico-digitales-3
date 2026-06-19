/**
 * @file    adc_dac_echo_gpdma.c
 * @brief   Eco de audio ADC -> DAC en LPC1769 usando GPDMA canales 2 y 3.
 *
 * Camino de la senal:
 *   ADC0.x -> GPDMA CH2 -> buffer ping-pong en AHB SRAM
 *   CPU mezcla senal directa + senal retardada 0..500 ms con repeat por feedback
 *   buffer ping-pong en AHB SRAM -> GPDMA CH3 -> DAC AOUT
 *
 * Pines por defecto:
 *   Entrada audio ADC : AD0.0, P0.23
 *   Time/repeat       : recibidos por UART
 *   Salida DAC        : AOUT,  P0.26
 *   Boton modo        : EINT0, P2.10
 *   Monitor UART      : UART0, TX P0.2, RX P0.3, 115200 8N1
 *
 * Los bancos AHB SRAM0/SRAM1 del LPC1769 son contiguos desde 0x2007C000 y
 * suman 32 KiB. Este proyecto guarda ahi todos los buffers de DMA, las LLI y
 * las muestras del eco. Si el linker ya usa esa zona, mover
 * AUDIO_ECHO_AHB_SRAM_BASE a otro banco AHB libre y mantener valida la
 * comprobacion de tamanio.
 */

#include <stdint.h>

#include "LPC17xx.h"
#include "lpc17xx_adc.h"
#include "lpc17xx_clkpwr.h"
#include "lpc17xx_dac.h"
#include "lpc17xx_exti.h"
#include "lpc17xx_gpdma.h"
#include "lpc17xx_timer.h"
#include "lpc17xx_uart.h"

/* Frecuencia de muestreo buscada. El DAC y el ADC se temporizan con la misma base. */
#define AUDIO_ECHO_SAMPLE_RATE_HZ 22000UL
/*
 * Reloj interno de conversion del ADC. Timer0 marca el barrido de canales; este
 * valor solo asegura que cada conversion termine antes del siguiente disparo.
 */
#define AUDIO_ECHO_ADC_CONVERSION_RATE_HZ 100000UL
/* Retardo inicial y maximo del eco. UART selecciona entre 0 y 500 ms. */
#define AUDIO_ECHO_DEFAULT_DELAY_MS 250UL
#define AUDIO_ECHO_MAX_DELAY_MS     500UL
/* Cantidad de muestras por bloque DMA. 256 da ~11.6 ms por bloque a 22 kHz. */
#define AUDIO_ECHO_BLOCK_SAMPLES  256UL

/* Canal analogico usado como entrada. AD0.0 corresponde a P0.23. */
#define AUDIO_ECHO_ADC_CHANNEL       ADC_CHANNEL_0
#define AUDIO_ECHO_ADC_CHANNEL_INDEX 0UL
#define AUDIO_ECHO_ADC_ENABLED_CHANNELS     1UL
#define AUDIO_ECHO_ADC_MIN_CONVERSION_RATE_HZ \
    (AUDIO_ECHO_SAMPLE_RATE_HZ * AUDIO_ECHO_ADC_ENABLED_CHANNELS)

#define AUDIO_ECHO_ADC_DMA_CH  GPDMA_CH_2
#define AUDIO_ECHO_DAC_DMA_CH  GPDMA_CH_3

/* Bancos AHB SRAM0/SRAM1 usados como memoria compartida entre CPU y GPDMA. */
#define AUDIO_ECHO_AHB_SRAM_BASE 0x2007C000UL
#define AUDIO_ECHO_AHB_SRAM_SIZE (32UL * 1024UL)

/* Ganancias en formato Q15: 32768 = 1.0, 16384 = 0.5. */
#define AUDIO_ECHO_DRY_GAIN_Q15  32768L
#define AUDIO_ECHO_WET_GAIN_Q15  16384L
/* Feedback maximo del repeat: 29491 ~= 0.90 en Q15. */
#define AUDIO_ECHO_DEFAULT_REPEAT_Q15 0L
#define AUDIO_ECHO_MAX_REPEAT_Q15 29491L
#define AUDIO_ECHO_MAX_REPEAT_PERCENT 90UL
/* El DAC es de 10 bits: rango 0..1023, punto medio 512 para audio con offset DC. */
#define AUDIO_ECHO_DAC_MID_SCALE 512L
#define AUDIO_ECHO_DAC_MAX       1023L
#define AUDIO_ECHO_ADC_MAX_12BIT 4095UL
#define AUDIO_ECHO_ADC_CENTER_Q8 (AUDIO_ECHO_DAC_MID_SCALE << 8)
#define AUDIO_ECHO_ADC_DC_TRACK_SHIFT 14UL
#define AUDIO_ECHO_INPUT_GATE_LSB 8L
/* Configuracion de monitoreo y seleccion de modo. */
#define AUDIO_ECHO_UART_BAUDRATE 115200UL
#define AUDIO_ECHO_UART_CMD_BUFFER_SIZE 32UL
#define AUDIO_ECHO_MODE_BUTTON_LINE EXTI_EINT0
#define AUDIO_ECHO_MODE_DEBOUNCE_BLOCKS 3UL
/* Ganancia y umbral iniciales del modo saturado: clipping duro sobre la senal directa. */
#define AUDIO_ECHO_DEFAULT_SATURATION_DRIVE_Q15 (12L * 32768L)
#define AUDIO_ECHO_MIN_SATURATION_DRIVE_Q15     (1L * 32768L)
#define AUDIO_ECHO_MAX_SATURATION_DRIVE_Q15     (32L * 32768L)
#define AUDIO_ECHO_DEFAULT_SATURATION_LIMIT_LSB 220L
#define AUDIO_ECHO_MIN_SATURATION_LIMIT_LSB     32L
#define AUDIO_ECHO_MAX_SATURATION_LIMIT_LSB     511L

/* Cantidad de muestras necesarias para lograr los retardos inicial y maximo. */
#define AUDIO_ECHO_DEFAULT_DELAY_SAMPLES \
    ((AUDIO_ECHO_SAMPLE_RATE_HZ * AUDIO_ECHO_DEFAULT_DELAY_MS + 500UL) / 1000UL)
#define AUDIO_ECHO_MAX_DELAY_SAMPLES \
    ((AUDIO_ECHO_SAMPLE_RATE_HZ * AUDIO_ECHO_MAX_DELAY_MS + 500UL) / 1000UL)
/* Banderas internas para saber que DMA termino cada bloque. TC = terminal count. */
#define AUDIO_ECHO_DMA_TC_ADC_MASK (1U << 0)
#define AUDIO_ECHO_DMA_TC_DAC_MASK (1U << 1)
#define AUDIO_ECHO_DMA_TC_BOTH     (AUDIO_ECHO_DMA_TC_ADC_MASK | AUDIO_ECHO_DMA_TC_DAC_MASK)

/* Modos de procesamiento disponibles para la senal de audio. */
typedef enum {
    AUDIO_ECHO_MODE_DELAY = 0,
    AUDIO_ECHO_MODE_SATURATED
} AudioEchoMode_T;

typedef struct {
    /* Dos LLI para ADC: una apunta al buffer 0 y otra al buffer 1. */
    GPDMA_LLI_T adcLli[2];
    /* Dos LLI para DAC: reproducen alternadamente buffer 0 y buffer 1. */
    GPDMA_LLI_T dacLli[2];
    /* Buffers crudos del ADC. Cada palabra contiene el registro ADDR0 completo. */
    uint32_t adcDmaBuffer[2][AUDIO_ECHO_BLOCK_SAMPLES];
    /* Buffers listos para escribir en DACR. Cada palabra ya esta alineada a DAC_VALUE. */
    uint32_t dacDmaBuffer[2][AUDIO_ECHO_BLOCK_SAMPLES];
    /* Linea circular de retardo: guarda muestras centradas en cero para generar el eco. */
    int16_t delayLine[AUDIO_ECHO_MAX_DELAY_SAMPLES];
} AudioEchoAhbRam_T;

/* Falla en compilacion si la estructura completa no entra en los 32 KiB de AHB SRAM. */
typedef char AudioEchoAhbRamMustFit
    [(sizeof(AudioEchoAhbRam_T) <= AUDIO_ECHO_AHB_SRAM_SIZE) ? 1 : -1];

/* Falla en compilacion si el ADC no alcanza a convertir todos los canales por muestra. */
typedef char AudioEchoAdcRateMustFit
    [(AUDIO_ECHO_ADC_CONVERSION_RATE_HZ >= AUDIO_ECHO_ADC_MIN_CONVERSION_RATE_HZ) ? 1 : -1];

/* Puntero fijo a la zona AHB. volatile evita que el compilador optimice accesos de DMA. */
static volatile AudioEchoAhbRam_T* const s_ahb =
    (volatile AudioEchoAhbRam_T*)AUDIO_ECHO_AHB_SRAM_BASE;

static volatile uint32_t s_dmaErrorFlags;    /* Guarda errores detectados en los canales DMA. */
static volatile uint32_t s_processedBlocks;  /* Contador de bloques para monitoreo en placa. */
static uint32_t s_delayWriteIndex;           /* Posicion actual dentro de la linea de eco. */
static volatile uint32_t s_currentDelaySamples; /* Delay recibido por UART, en muestras. */
static volatile int32_t s_currentRepeatQ15;  /* Feedback recibido por UART, en Q15. */
static volatile AudioEchoMode_T s_audioMode; /* Modo actual: delay o saturado. */
static volatile uint8_t s_modeReportPending; /* La ISR pide informar el modo por UART. */
static volatile uint32_t s_lastModeButtonBlock; /* Debounce basado en bloques procesados. */
static volatile int32_t s_saturationDriveQ15; /* Ganancia del modo saturado, formato Q15. */
static volatile int32_t s_saturationLimitLsb; /* Limite simetrico de clipping del saturador. */
static volatile char s_uartCommandBuffer[AUDIO_ECHO_UART_CMD_BUFFER_SIZE]; /* Comando recibido por UART. */
static volatile uint8_t s_uartCommandIndex;    /* Proxima posicion libre del buffer UART. */
static volatile uint8_t s_uartCommandReady;    /* Indica que hay un comando completo para procesar. */
static volatile uint8_t s_uartCommandOverflow; /* Marca comandos descartados por exceder el buffer. */
static int32_t s_adcCenterQ8;                /* Centro DC estimado de la entrada de audio. */
static uint8_t s_dmaTcMask;                  /* Indica si termino ADC, DAC o ambos. */
static uint8_t s_processBlock;               /* Bloque ping-pong que la CPU debe mezclar. */

uint32_t AudioEcho_GetCurrentDelayMs(void);
uint32_t AudioEcho_GetCurrentRepeatPercent(void);

static uint32_t AudioEcho_RoundDiv(uint32_t numerator, uint32_t denominator);
static volatile const uint32_t* AudioEcho_AdcDataRegister(uint32_t channelIndex);
static LPC_GPDMACH_TypeDef* AudioEcho_DmaChannelRegisters(uint32_t channel);
static uint32_t AudioEcho_AdcResult12(uint32_t adcWord);
static uint32_t AudioEcho_DacWord(uint32_t dac10);
static int32_t AudioEcho_ClampToDac(int32_t sample);
static int16_t AudioEcho_ClampToDelayLine(int32_t sample);
static int32_t AudioEcho_Abs32(int32_t value);
static int32_t AudioEcho_AudioSampleFromAdc(uint32_t adcWord);
static int32_t AudioEcho_SaturateSample(int32_t sample, int32_t driveQ15, int32_t limitLsb);
static char AudioEcho_ToUpper(char value);
static uint32_t AudioEcho_ParseUnsigned(const char* text, uint8_t* parsed);
static uint32_t AudioEcho_DelayMsToSamples(uint32_t delayMs);
static int32_t AudioEcho_RepeatPercentToQ15(uint32_t repeatPercent);
static uint32_t AudioEcho_DelayReadIndex(uint32_t writeIndex, uint32_t delaySamples);
static void AudioEcho_ClearAhbRam(void);
static void AudioEcho_InitDacSilence(void);
static void AudioEcho_InitDmaLinkedLists(void);
static void AudioEcho_UartWriteString(const char* text);
static void AudioEcho_UartWriteUnsigned(uint32_t value);
static void AudioEcho_ReportMode(void);
static void AudioEcho_ReportModeIfPending(void);
static void AudioEcho_CaptureUartChar(char value);
static void AudioEcho_ProcessUartCommand(const char* command);
static void AudioEcho_ProcessUartCommandIfPending(void);
static void AudioEcho_ConfigUart(void);
static void AudioEcho_ConfigModeButton(void);
static void AudioEcho_ConfigAdc(void);
static void AudioEcho_ConfigDac(uint32_t sampleTicks);
static void AudioEcho_ConfigSampleTimer(uint32_t adcTriggerTicks);
static void AudioEcho_ConfigDmaChannels(void);
static void AudioEcho_ProcessBlock(uint8_t block);

/* Division entera con redondeo, usada para calcular ticks por muestra. */
static uint32_t AudioEcho_RoundDiv(uint32_t numerator, uint32_t denominator) {
    return (numerator + denominator / 2UL) / denominator;
}

/* Devuelve el registro individual ADDRn de un canal ADC. */
static volatile const uint32_t* AudioEcho_AdcDataRegister(uint32_t channelIndex) {
    return &LPC_ADC->ADDR0 + channelIndex;
}

/* Devuelve el bloque de registros del canal GPDMA usado. */
static LPC_GPDMACH_TypeDef* AudioEcho_DmaChannelRegisters(uint32_t channel) {
    switch (channel) {
        case GPDMA_CH_0: return LPC_GPDMACH0;
        case GPDMA_CH_1: return LPC_GPDMACH1;
        case GPDMA_CH_2: return LPC_GPDMACH2;
        case GPDMA_CH_3: return LPC_GPDMACH3;
        case GPDMA_CH_4: return LPC_GPDMACH4;
        case GPDMA_CH_5: return LPC_GPDMACH5;
        case GPDMA_CH_6: return LPC_GPDMACH6;
        case GPDMA_CH_7: return LPC_GPDMACH7;
        default: return LPC_GPDMACH0;
    }
}

/* Extrae el resultado de 12 bits desde un registro individual ADDRn del ADC. */
static uint32_t AudioEcho_AdcResult12(uint32_t adcWord) {
    return ADC_DR_RESULT(adcWord);
}

/* Convierte un valor DAC de 10 bits al formato que espera el registro DACR. */
static uint32_t AudioEcho_DacWord(uint32_t dac10) {
    return DAC_VALUE(dac10);
}

/* Limita la mezcla para que nunca salga del rango fisico del DAC: 0..1023. */
static int32_t AudioEcho_ClampToDac(int32_t sample) {
    if (sample < 0) {
        return 0;
    }
    if (sample > AUDIO_ECHO_DAC_MAX) {
        return AUDIO_ECHO_DAC_MAX;
    }
    return sample;
}

/* Limita la senal interna antes de guardarla en la linea circular int16_t. */
static int16_t AudioEcho_ClampToDelayLine(int32_t sample) {
    if (sample < -32768L) {
        return (int16_t)-32768L;
    }
    if (sample > 32767L) {
        return (int16_t)32767L;
    }
    return (int16_t)sample;
}

/* Calcula el valor absoluto de una muestra de audio en entero de 32 bits. */
static int32_t AudioEcho_Abs32(int32_t value) {
    return (value < 0) ? -value : value;
}

/*
 * Convierte el ADC de audio a muestra centrada. El centro se estima lentamente
 * para corregir offsets analogicos y una compuerta chica elimina el ruido de
 * reposo antes de que llegue al DAC.
 */
static int32_t AudioEcho_AudioSampleFromAdc(uint32_t adcWord) {
    const int32_t adc10Q8 = (int32_t)(AudioEcho_AdcResult12(adcWord) >> 2) << 8;
    int32_t centeredQ8;
    int32_t sample;

    s_adcCenterQ8 += (adc10Q8 - s_adcCenterQ8) >> AUDIO_ECHO_ADC_DC_TRACK_SHIFT;
    centeredQ8 = adc10Q8 - s_adcCenterQ8;
    sample = (centeredQ8 >= 0) ? ((centeredQ8 + 128L) >> 8) : -(((-centeredQ8) + 128L) >> 8);

    if (AudioEcho_Abs32(sample) <= AUDIO_ECHO_INPUT_GATE_LSB) {
        return 0L;
    }

    return sample;
}

/* Aplica clipping duro para el modo saturado. */
static int32_t AudioEcho_SaturateSample(int32_t sample, int32_t driveQ15, int32_t limitLsb) {
    int32_t driven = (sample * driveQ15) >> 15;

    if (driven > limitLsb) {
        return limitLsb;
    }
    if (driven < -limitLsb) {
        return -limitLsb;
    }

    return driven;
}

/* Convierte letras ASCII minusculas a mayusculas para aceptar comandos flexibles. */
static char AudioEcho_ToUpper(char value) {
    if ((value >= 'a') && (value <= 'z')) {
        return (char)(value - ('a' - 'A'));
    }

    return value;
}

/* Lee un numero decimal positivo luego del identificador del comando UART. */
static uint32_t AudioEcho_ParseUnsigned(const char* text, uint8_t* parsed) {
    uint32_t value = 0UL;
    uint8_t foundDigit = 0U;

    while ((*text == ' ') || (*text == '\t') || (*text == '=') || (*text == ':')) {
        text++;
    }

    while ((*text >= '0') && (*text <= '9')) {
        foundDigit = 1U;
        value = (value * 10UL) + (uint32_t)(*text - '0');
        text++;
    }

    *parsed = foundDigit;
    return value;
}

/* Convierte milisegundos de delay a muestras, respetando el maximo admitido. */
static uint32_t AudioEcho_DelayMsToSamples(uint32_t delayMs) {
    if (delayMs > AUDIO_ECHO_MAX_DELAY_MS) {
        delayMs = AUDIO_ECHO_MAX_DELAY_MS;
    }

    return AudioEcho_RoundDiv(AUDIO_ECHO_SAMPLE_RATE_HZ * delayMs, 1000UL);
}

/* Convierte el porcentaje de repeat a Q15 para usarlo como feedback entero. */
static int32_t AudioEcho_RepeatPercentToQ15(uint32_t repeatPercent) {
    if (repeatPercent > AUDIO_ECHO_MAX_REPEAT_PERCENT) {
        repeatPercent = AUDIO_ECHO_MAX_REPEAT_PERCENT;
    }

    return (int32_t)AudioEcho_RoundDiv(repeatPercent * 32768UL, 100UL);
}

/* Calcula desde donde leer la linea circular para el delay seleccionado. */
static uint32_t AudioEcho_DelayReadIndex(uint32_t writeIndex, uint32_t delaySamples) {
    uint32_t readIndex;

    if (delaySamples >= AUDIO_ECHO_MAX_DELAY_SAMPLES) {
        delaySamples = AUDIO_ECHO_MAX_DELAY_SAMPLES;
    }

    readIndex = writeIndex + AUDIO_ECHO_MAX_DELAY_SAMPLES - delaySamples;
    if (readIndex >= AUDIO_ECHO_MAX_DELAY_SAMPLES) {
        readIndex -= AUDIO_ECHO_MAX_DELAY_SAMPLES;
    }

    return readIndex;
}

/* Limpia toda la zona AHB usada por el proyecto: LLIs, buffers y delay line. */
static void AudioEcho_ClearAhbRam(void) {
    uint32_t i;
    volatile uint32_t* ram = (volatile uint32_t*)s_ahb;
    const uint32_t words = sizeof(AudioEchoAhbRam_T) / sizeof(uint32_t);

    /* Se borra palabra por palabra porque la zona esta en una direccion absoluta. */
    for (i = 0; i < words; i++) {
        ram[i] = 0;
    }
}

/* Carga ambos buffers DAC con media escala para empezar sin golpes de audio. */
static void AudioEcho_InitDacSilence(void) {
    uint32_t block;
    uint32_t i;
    const uint32_t silence = AudioEcho_DacWord((uint32_t)AUDIO_ECHO_DAC_MID_SCALE);

    for (block = 0; block < 2UL; block++) {
        for (i = 0; i < AUDIO_ECHO_BLOCK_SAMPLES; i++) {
            s_ahb->dacDmaBuffer[block][i] = silence;
        }
    }
}

/* Prepara las listas enlazadas del GPDMA para transferencias circulares ping-pong. */
static void AudioEcho_InitDmaLinkedLists(void) {
    /*
     * Control para ADC:
     * - transferSize: cantidad de muestras por bloque.
     * - burst 1: una muestra por request.
     * - word/word: ADDR0 se lee como palabra de 32 bits.
     * - DI: incrementa destino para llenar el buffer.
     * - I: genera interrupcion al terminar cada bloque.
     */
    const uint32_t adcControl =
        GPDMA_DMACCxControl_TransferSize(AUDIO_ECHO_BLOCK_SAMPLES) |
        GPDMA_DMACCxControl_SBSize(GPDMA_BSIZE_1) |
        GPDMA_DMACCxControl_DBSize(GPDMA_BSIZE_1) |
        GPDMA_DMACCxControl_SWidth(GPDMA_WORD) |
        GPDMA_DMACCxControl_DWidth(GPDMA_WORD) |
        GPDMA_DMACCxControl_DI |
        GPDMA_DMACCxControl_I;

    /*
     * Control para DAC:
     * - transferSize: cantidad de muestras por bloque.
     * - word/word: se escribe DACR completo con cada palabra preparada.
     * - SI: incrementa origen para recorrer el buffer.
     * - destino fijo: siempre se escribe LPC_DAC->DACR.
     * - I: genera interrupcion al terminar cada bloque.
     */
    const uint32_t dacControl =
        GPDMA_DMACCxControl_TransferSize(AUDIO_ECHO_BLOCK_SAMPLES) |
        GPDMA_DMACCxControl_SBSize(GPDMA_BSIZE_1) |
        GPDMA_DMACCxControl_DBSize(GPDMA_BSIZE_1) |
        GPDMA_DMACCxControl_SWidth(GPDMA_WORD) |
        GPDMA_DMACCxControl_DWidth(GPDMA_WORD) |
        GPDMA_DMACCxControl_SI |
        GPDMA_DMACCxControl_I;

    /* ADC LLI 0: lee ADDR0 y llena adcDmaBuffer[0]; luego salta a adcLli[1]. */
    s_ahb->adcLli[0].srcAddr =
        (uint32_t)(uintptr_t)AudioEcho_AdcDataRegister(AUDIO_ECHO_ADC_CHANNEL_INDEX);
    s_ahb->adcLli[0].dstAddr = (uint32_t)(uintptr_t)&s_ahb->adcDmaBuffer[0][0];
    s_ahb->adcLli[0].nextLLI = (uint32_t)(uintptr_t)&s_ahb->adcLli[1];
    s_ahb->adcLli[0].control = adcControl;

    /* ADC LLI 1: llena adcDmaBuffer[1]; luego vuelve a adcLli[0]. */
    s_ahb->adcLli[1].srcAddr =
        (uint32_t)(uintptr_t)AudioEcho_AdcDataRegister(AUDIO_ECHO_ADC_CHANNEL_INDEX);
    s_ahb->adcLli[1].dstAddr = (uint32_t)(uintptr_t)&s_ahb->adcDmaBuffer[1][0];
    s_ahb->adcLli[1].nextLLI = (uint32_t)(uintptr_t)&s_ahb->adcLli[0];
    s_ahb->adcLli[1].control = adcControl;

    /* DAC LLI 0: reproduce dacDmaBuffer[0]; luego salta a dacLli[1]. */
    s_ahb->dacLli[0].srcAddr = (uint32_t)(uintptr_t)&s_ahb->dacDmaBuffer[0][0];
    s_ahb->dacLli[0].dstAddr = (uint32_t)(uintptr_t)&LPC_DAC->DACR;
    s_ahb->dacLli[0].nextLLI = (uint32_t)(uintptr_t)&s_ahb->dacLli[1];
    s_ahb->dacLli[0].control = dacControl;

    /* DAC LLI 1: reproduce dacDmaBuffer[1]; luego vuelve a dacLli[0]. */
    s_ahb->dacLli[1].srcAddr = (uint32_t)(uintptr_t)&s_ahb->dacDmaBuffer[1][0];
    s_ahb->dacLli[1].dstAddr = (uint32_t)(uintptr_t)&LPC_DAC->DACR;
    s_ahb->dacLli[1].nextLLI = (uint32_t)(uintptr_t)&s_ahb->dacLli[0];
    s_ahb->dacLli[1].control = dacControl;
}

/* Envia una cadena por UART0 usando la rutina bloqueante de la libreria. */
static void AudioEcho_UartWriteString(const char* text) {
    uint32_t length = 0UL;

    while (text[length] != '\0') {
        length++;
    }

    if (length != 0UL) {
        (void)UART_Send(UART0, (const uint8_t*)text, length, BLOCKING);
    }
}

/* Envia un entero sin signo por UART sin depender de printf ni de la libreria stdio. */
static void AudioEcho_UartWriteUnsigned(uint32_t value) {
    char digits[10];
    uint32_t count = 0UL;

    if (value == 0UL) {
        AudioEcho_UartWriteString("0");
        return;
    }

    while ((value != 0UL) && (count < sizeof(digits))) {
        digits[count] = (char)('0' + (value % 10UL));
        value /= 10UL;
        count++;
    }

    while (count != 0UL) {
        count--;
        (void)UART_Send(UART0, (const uint8_t*)&digits[count], 1UL, BLOCKING);
    }
}

/* Informa por UART los parametros activos del modo actual. */
static void AudioEcho_ReportMode(void) {
    const AudioEchoMode_T mode = s_audioMode;

    if (mode == AUDIO_ECHO_MODE_SATURATED) {
        AudioEcho_UartWriteString("Modo: saturacion | drive=x");
        AudioEcho_UartWriteUnsigned(
            AudioEcho_RoundDiv((uint32_t)s_saturationDriveQ15, 32768UL));
        AudioEcho_UartWriteString(" | limite=");
        AudioEcho_UartWriteUnsigned((uint32_t)s_saturationLimitLsb);
        AudioEcho_UartWriteString("\r\n");
    } else {
        AudioEcho_UartWriteString("Modo: delay | time=");
        AudioEcho_UartWriteUnsigned(AudioEcho_GetCurrentDelayMs());
        AudioEcho_UartWriteString(" ms | repeat=");
        AudioEcho_UartWriteUnsigned(AudioEcho_GetCurrentRepeatPercent());
        AudioEcho_UartWriteString(" %\r\n");
    }
}

/* Ejecuta fuera de la ISR el reporte solicitado al cambiar de modo con el boton. */
static void AudioEcho_ReportModeIfPending(void) {
    if (s_modeReportPending != 0U) {
        s_modeReportPending = 0U;
        AudioEcho_ReportMode();
    }
}

/* Captura caracteres UART hasta formar una linea de comando terminada en Enter. */
static void AudioEcho_CaptureUartChar(char value) {
    if ((value == '\r') || (value == '\n')) {
        if ((s_uartCommandIndex != 0U) && (s_uartCommandReady == 0U)) {
            s_uartCommandBuffer[s_uartCommandIndex] = '\0';
            s_uartCommandReady = 1U;
        }
        s_uartCommandIndex = 0U;
        return;
    }

    if ((value == '\b') || (value == 0x7F)) {
        if (s_uartCommandIndex != 0U) {
            s_uartCommandIndex--;
        }
        return;
    }

    if ((value < ' ') || (value > '~') || (s_uartCommandReady != 0U)) {
        return;
    }

    if (s_uartCommandIndex >= (AUDIO_ECHO_UART_CMD_BUFFER_SIZE - 1UL)) {
        s_uartCommandIndex = 0U;
        s_uartCommandOverflow = 1U;
        return;
    }

    s_uartCommandBuffer[s_uartCommandIndex] = value;
    s_uartCommandIndex++;
}

/* Interpreta comandos UART para cambiar delay, repeat, saturacion o modo activo. */
static void AudioEcho_ProcessUartCommand(const char* command) {
    uint8_t parsed = 0U;
    uint32_t value = 0UL;
    const char commandId = AudioEcho_ToUpper(command[0]);

    if (commandId == '\0') {
        return;
    }

    if (commandId == '?') {
        AudioEcho_ReportMode();
        return;
    }

    if (commandId == 'D') {
        s_audioMode = AUDIO_ECHO_MODE_DELAY;
        AudioEcho_ReportMode();
        return;
    }

    if (commandId == 'S') {
        s_audioMode = AUDIO_ECHO_MODE_SATURATED;
        AudioEcho_ReportMode();
        return;
    }

    value = AudioEcho_ParseUnsigned(&command[1], &parsed);
    if (parsed == 0U) {
        AudioEcho_UartWriteString("Comando invalido. Use Tms R% Gx Llim D S ?\r\n");
        return;
    }

    if (commandId == 'T') {
        if (value > AUDIO_ECHO_MAX_DELAY_MS) {
            value = AUDIO_ECHO_MAX_DELAY_MS;
        }
        s_currentDelaySamples = AudioEcho_DelayMsToSamples(value);
        AudioEcho_ReportMode();
        return;
    }

    if (commandId == 'R') {
        if (value > AUDIO_ECHO_MAX_REPEAT_PERCENT) {
            value = AUDIO_ECHO_MAX_REPEAT_PERCENT;
        }
        s_currentRepeatQ15 = AudioEcho_RepeatPercentToQ15(value);
        AudioEcho_ReportMode();
        return;
    }

    if (commandId == 'G') {
        if (value < 1UL) {
            value = 1UL;
        }
        if (value > 32UL) {
            value = 32UL;
        }
        s_saturationDriveQ15 = (int32_t)(value * 32768UL);
        AudioEcho_ReportMode();
        return;
    }

    if (commandId == 'L') {
        if (value < (uint32_t)AUDIO_ECHO_MIN_SATURATION_LIMIT_LSB) {
            value = (uint32_t)AUDIO_ECHO_MIN_SATURATION_LIMIT_LSB;
        }
        if (value > (uint32_t)AUDIO_ECHO_MAX_SATURATION_LIMIT_LSB) {
            value = (uint32_t)AUDIO_ECHO_MAX_SATURATION_LIMIT_LSB;
        }
        s_saturationLimitLsb = (int32_t)value;
        AudioEcho_ReportMode();
        return;
    }

    AudioEcho_UartWriteString("Comando invalido. Use Tms R% Gx Llim D S ?\r\n");
}

/* Copia de forma atomica el comando recibido en ISR y lo procesa en el lazo principal. */
static void AudioEcho_ProcessUartCommandIfPending(void) {
    char command[AUDIO_ECHO_UART_CMD_BUFFER_SIZE];
    uint32_t i;

    if (s_uartCommandOverflow != 0U) {
        s_uartCommandOverflow = 0U;
        AudioEcho_UartWriteString("Error: comando UART muy largo\r\n");
    }

    if (s_uartCommandReady == 0U) {
        return;
    }

    NVIC_DisableIRQ(UART0_IRQn);
    for (i = 0UL; i < AUDIO_ECHO_UART_CMD_BUFFER_SIZE; i++) {
        command[i] = (char)s_uartCommandBuffer[i];
        if (command[i] == '\0') {
            break;
        }
    }
    command[AUDIO_ECHO_UART_CMD_BUFFER_SIZE - 1UL] = '\0';
    s_uartCommandReady = 0U;
    NVIC_EnableIRQ(UART0_IRQn);

    AudioEcho_ProcessUartCommand(command);
}

/* Configura UART0 a 115200 8N1 y habilita interrupciones de recepcion. */
static void AudioEcho_ConfigUart(void) {
    const UART_CFG_T uartCfg = {
        AUDIO_ECHO_UART_BAUDRATE,
        UART_PARITY_NONE,
        UART_DBITS_8,
        UART_STOPBIT_1
    };

    UART_PinConfig(UART_TX0_P0_2);
    UART_PinConfig(UART_RX0_P0_3);
    UART_Init(UART0, &uartCfg);
    UART_TxEnable(UART0);
    UART_IntConfig(UART0, UART_INT_RBR, ENABLE);
    UART_IntConfig(UART0, UART_INT_RLS, ENABLE);
    NVIC_ClearPendingIRQ(UART0_IRQn);
    NVIC_EnableIRQ(UART0_IRQn);
}

/* Configura EINT0 como entrada con pull-up para alternar entre delay y saturacion. */
static void AudioEcho_ConfigModeButton(void) {
    const EXTI_CFG_T buttonCfg = {
        AUDIO_ECHO_MODE_BUTTON_LINE,
        EXTI_EDGE_SENSITIVE,
        EXTI_FALLING_EDGE
    };

    EXTI_Init();
    EXTI_PinConfig(AUDIO_ECHO_MODE_BUTTON_LINE, EXTI_PULLUP);
    EXTI_ConfigEnable(&buttonCfg);
}

/* Configura el ADC para muestrear audio al ritmo marcado por MAT0.1. */
static void AudioEcho_ConfigAdc(void) {
    /* Pone los pines fisicos en funcion analogica y sin pull-up/pull-down. */
    ADC_PinConfig(AUDIO_ECHO_ADC_CHANNEL);
    /*
     * Solo queda seleccionado el canal de audio. Time y repeat se reciben por UART.
     */
    ADC_Init(AUDIO_ECHO_ADC_CONVERSION_RATE_HZ);
    /* Habilita audio. */
    ADC_ChannelEnable(AUDIO_ECHO_ADC_CHANNEL);
    /*
     * Se habilita solo DONE0 como fuente de request/interrupt del ADC. Asi el DMA
     * copia ADDR0 en cada muestra de audio y no se dispara por ADGDR.
     */
    LPC_ADC->ADINTEN = ADC_INTEN_CH(AUDIO_ECHO_ADC_CHANNEL_INDEX);
    /* El ADC se dispara con flanco ascendente del evento de match. */
    ADC_EdgeStartConfig(ADC_START_ON_RISING);
    /* MAT0.1 es la fuente de disparo; asi el ADC queda sincronizado por hardware. */
    ADC_StartCmd(ADC_START_ON_MAT01);
}

/* Configura el DAC, su pin AOUT y el contador que genera requests DMA. */
static void AudioEcho_ConfigDac(uint32_t sampleTicks) {
    DAC_CONVERTER_CFG_T dacCfg;

    /* P0.26 pasa a funcion AOUT y se selecciona PCLK_DAC. */
    DAC_Init();
    /* 700 uA permite mayor velocidad de asentamiento, suficiente para audio a 22 kHz. */
    DAC_SetBias(DAC_700uA);
    /* Media escala evita un salto brusco antes de que empiece el DMA. */
    DAC_UpdateValue((uint32_t)AUDIO_ECHO_DAC_MID_SCALE);
    /* Cada sampleTicks ciclos de PCLK_DAC el DAC genera un request DMA. */
    DAC_SetDMATimeOut((uint16_t)sampleTicks);

    /*
     * Se deja el DAC preparado, pero sin requests DMA durante Init.
     * AudioEcho_Start() habilita el contador al mismo tiempo que arranca el flujo.
     */
    dacCfg.doubleBuffer = ENABLE;
    dacCfg.dmaCounter = DISABLE;
    dacCfg.dmaRequest = DISABLE;
    DAC_ConfigDAConverterControl(&dacCfg);
}

/* Configura Timer0/MAT0.1 para disparar el ADC de audio. */
static void AudioEcho_ConfigSampleTimer(uint32_t adcTriggerTicks) {
    TIM_TIMERCFG_T timerCfg;
    TIM_MATCHCFG_T matchCfg;
    /*
     * MAT0.1 alterna su estado en cada match. Como el ADC se dispara solo con
     * flanco ascendente, se usa medio periodo para obtener un flanco ascendente
     * cada adcTriggerTicks, es decir AUDIO_ECHO_SAMPLE_RATE_HZ para AD0.0.
     */
    uint32_t adcToggleTicks = adcTriggerTicks / 2UL;

    if (adcToggleTicks == 0UL) {
        adcToggleTicks = 1UL;
    }

    /* Timer0 cuenta ticks directos de PCLK_TIMER0, sin prescaler adicional. */
    timerCfg.prescaleOpt = TIM_TICK;
    timerCfg.prescaleValue = 1UL;
    TIM_InitTimer(LPC_TIM0, &timerCfg);

    /*
     * Match 1:
     * - no interrumpe a CPU,
     * - resetea el contador,
     * - conmuta MAT0.1 en hardware.
     */
    matchCfg.channel = TIM_MATCH_1;
    matchCfg.intEn = DISABLE;
    matchCfg.stopEn = DISABLE;
    matchCfg.resetEn = ENABLE;
    matchCfg.extOpt = TIM_TOGGLE;
    matchCfg.matchValue = adcToggleTicks - 1UL;
    TIM_ConfigMatch(LPC_TIM0, &matchCfg);
}

/* Configura los dos canales GPDMA pedidos: CH2 para ADC y CH3 para DAC. */
static void AudioEcho_ConfigDmaChannels(void) {
    GPDMA_Channel_CFG_T adcDma;
    GPDMA_Channel_CFG_T dacDma;

    /* Enciende y limpia el controlador GPDMA. */
    GPDMA_Init();

    /*
     * Canal 2: periferico a memoria.
     * La fuente real es LPC_ADC->ADDR0 y el destino es adcDmaBuffer[0].
     * linkedList apunta a adcLli[1] para que al terminar el primer bloque siga
     * automaticamente con el buffer 1 y despues vuelva al buffer 0.
     */
    adcDma.channelNum = AUDIO_ECHO_ADC_DMA_CH;
    adcDma.transferSize = AUDIO_ECHO_BLOCK_SAMPLES;
    adcDma.type = GPDMA_P2M;
    adcDma.srcMemAddr = 0UL;
    adcDma.dstMemAddr = (uint32_t)(uintptr_t)&s_ahb->adcDmaBuffer[0][0];
    adcDma.srcConn = GPDMA_ADC;
    adcDma.dstConn = GPDMA_ADC;
    adcDma.src.width = GPDMA_WORD;
    adcDma.src.burst = GPDMA_BSIZE_1;
    adcDma.src.increment = DISABLE;
    adcDma.dst.width = GPDMA_WORD;
    adcDma.dst.burst = GPDMA_BSIZE_1;
    adcDma.dst.increment = ENABLE;
    adcDma.intTC = ENABLE;
    adcDma.intErr = ENABLE;
    adcDma.linkedList = (uint32_t)(uintptr_t)&s_ahb->adcLli[1];

    /*
     * Canal 3: memoria a periferico.
     * La fuente inicial es dacDmaBuffer[0] y el destino real es LPC_DAC->DACR.
     * El DAC pide una palabra nueva por DMA a cada tick configurado en DACCNTVAL.
     */
    dacDma.channelNum = AUDIO_ECHO_DAC_DMA_CH;
    dacDma.transferSize = AUDIO_ECHO_BLOCK_SAMPLES;
    dacDma.type = GPDMA_M2P;
    dacDma.srcMemAddr = (uint32_t)(uintptr_t)&s_ahb->dacDmaBuffer[0][0];
    dacDma.dstMemAddr = 0UL;
    dacDma.srcConn = GPDMA_DAC;
    dacDma.dstConn = GPDMA_DAC;
    dacDma.src.width = GPDMA_WORD;
    dacDma.src.burst = GPDMA_BSIZE_1;
    dacDma.src.increment = ENABLE;
    dacDma.dst.width = GPDMA_WORD;
    dacDma.dst.burst = GPDMA_BSIZE_1;
    dacDma.dst.increment = DISABLE;
    dacDma.intTC = ENABLE;
    dacDma.intErr = ENABLE;
    dacDma.linkedList = (uint32_t)(uintptr_t)&s_ahb->dacLli[1];

    /* Si algun canal esta ocupado o falla la configuracion, se deja registrado. */
    if (GPDMA_SetupChannel(&adcDma) == ERROR) {
        s_dmaErrorFlags |= AUDIO_ECHO_DMA_TC_ADC_MASK;
    } else {
        /*
         * La libreria carga la direccion inicial del ADC desde su tabla interna,
         * que apunta a ADGDR. Se corrige el primer bloque para usar ADDR0; luego
         * las LLI mantienen esa misma fuente individual.
         */
        AudioEcho_DmaChannelRegisters(AUDIO_ECHO_ADC_DMA_CH)->DMACCSrcAddr =
            (uint32_t)(uintptr_t)AudioEcho_AdcDataRegister(AUDIO_ECHO_ADC_CHANNEL_INDEX);
    }

    if (GPDMA_SetupChannel(&dacDma) == ERROR) {
        s_dmaErrorFlags |= AUDIO_ECHO_DMA_TC_DAC_MASK;
    }

    /* Habilita la interrupcion global del GPDMA en el NVIC. */
    NVIC_EnableIRQ(DMA_IRQn);
}

/* Mezcla un bloque completo: senal directa + muestra vieja de la linea de retardo. */
static void AudioEcho_ProcessBlock(uint8_t block) {
    uint32_t i;
    uint32_t delaySamples;
    int32_t repeatQ15;
    int32_t saturationDriveQ15;
    int32_t saturationLimitLsb;
    AudioEchoMode_T mode;

    delaySamples = s_currentDelaySamples;
    repeatQ15 = s_currentRepeatQ15;
    saturationDriveQ15 = s_saturationDriveQ15;
    saturationLimitLsb = s_saturationLimitLsb;
    mode = s_audioMode;

    for (i = 0; i < AUDIO_ECHO_BLOCK_SAMPLES; i++) {
        /* El DMA guardo la palabra completa ADDR0; de ahi se extrae el resultado de 12 bits. */
        const uint32_t adcWord = s_ahb->adcDmaBuffer[block][i];
        const int32_t dry = AudioEcho_AudioSampleFromAdc(adcWord);
        int32_t dac10;
        int32_t delayInput;

        if (mode == AUDIO_ECHO_MODE_SATURATED) {
            const int32_t saturated =
                AudioEcho_SaturateSample(dry, saturationDriveQ15, saturationLimitLsb);

            dac10 = AudioEcho_ClampToDac(saturated + AUDIO_ECHO_DAC_MID_SCALE);
            delayInput = dry;
        } else {
            /*
             * delayed es la muestra vieja segun el tiempo recibido por UART. Con delay 0 se
             * anula el camino wet para evitar sumar la senal consigo misma.
             */
            const int32_t delayed = (delaySamples == 0UL)
                                        ? 0L
                                        : s_ahb->delayLine[
                                              AudioEcho_DelayReadIndex(s_delayWriteIndex,
                                                                       delaySamples)];
            /* Mezcla con aritmetica entera Q15: salida = dry*1.0 + delayed*0.5. */
            const int32_t mixed =
                ((dry * AUDIO_ECHO_DRY_GAIN_Q15) + (delayed * AUDIO_ECHO_WET_GAIN_Q15)) >> 15;
            /*
             * El repeat es feedback: parte de la muestra retardada vuelve a entrar
             * a la linea de delay para producir nuevas repeticiones.
             */
            const int32_t feedback =
                (delaySamples == 0UL) ? 0L : ((delayed * repeatQ15) >> 15);

            /* Vuelve a sumar media escala para que el DAC reciba una senal unipolar. */
            dac10 = AudioEcho_ClampToDac(mixed + AUDIO_ECHO_DAC_MID_SCALE);
            delayInput = dry + feedback;
        }

        /*
         * Se guarda la muestra actual en la linea de retardo. Cuando el indice
         * vuelva a esta posicion, esta muestra sera el eco de una muestra futura.
         */
        s_ahb->delayLine[s_delayWriteIndex] = AudioEcho_ClampToDelayLine(delayInput);
        s_delayWriteIndex++;
        if (s_delayWriteIndex >= AUDIO_ECHO_MAX_DELAY_SAMPLES) {
            s_delayWriteIndex = 0UL;
        }

        /* Se prepara la palabra exacta que CH3 escribira en LPC_DAC->DACR. */
        s_ahb->dacDmaBuffer[block][i] = AudioEcho_DacWord((uint32_t)dac10);
    }

    /* Contador visible para confirmar que el procesamiento sigue corriendo. */
    s_processedBlocks++;
}

/* Inicializa reloj, memoria AHB, DMA, DAC, timer y ADC. */
void AudioEcho_Init(void) {
    uint32_t pclk;
    uint32_t sampleTicks;
    uint32_t adcTriggerTicks;

    /* Estado inicial limpio. */
    s_dmaErrorFlags = 0UL;
    s_processedBlocks = 0UL;
    s_delayWriteIndex = 0UL;
    s_currentDelaySamples = AUDIO_ECHO_DEFAULT_DELAY_SAMPLES;
    s_currentRepeatQ15 = AUDIO_ECHO_DEFAULT_REPEAT_Q15;
    s_audioMode = AUDIO_ECHO_MODE_DELAY;
    s_modeReportPending = 0U;
    s_lastModeButtonBlock = 0UL - AUDIO_ECHO_MODE_DEBOUNCE_BLOCKS;
    s_saturationDriveQ15 = AUDIO_ECHO_DEFAULT_SATURATION_DRIVE_Q15;
    s_saturationLimitLsb = AUDIO_ECHO_DEFAULT_SATURATION_LIMIT_LSB;
    s_uartCommandIndex = 0U;
    s_uartCommandReady = 0U;
    s_uartCommandOverflow = 0U;
    s_uartCommandBuffer[0] = '\0';
    s_adcCenterQ8 = AUDIO_ECHO_ADC_CENTER_Q8;
    s_dmaTcMask = 0U;
    s_processBlock = 0U;

    /*
     * Timer0, DAC y ADC usan PCLK = CCLK/4. Al compartir base de reloj, el
     * barrido ADC queda sincronizado con la salida DAC.
     */
    CLKPWR_SetPCLKDiv(CLKPWR_PCLKSEL_TIMER0, CLKPWR_PCLKSEL_CCLK_DIV_4);
    CLKPWR_SetPCLKDiv(CLKPWR_PCLKSEL_DAC, CLKPWR_PCLKSEL_CCLK_DIV_4);
    CLKPWR_SetPCLKDiv(CLKPWR_PCLKSEL_ADC, CLKPWR_PCLKSEL_CCLK_DIV_4);
    CLKPWR_SetPCLKDiv(CLKPWR_PCLKSEL_UART0, CLKPWR_PCLKSEL_CCLK_DIV_4);

    /*
     * sampleTicks es la cantidad de ticks de PCLK por muestra.
     * Con PCLK_DAC = 25 MHz y fs = 22 kHz, resulta aproximadamente 1136 ticks.
     */
    pclk = CLKPWR_GetPCLK(CLKPWR_PCLKSEL_DAC);
    sampleTicks = AudioEcho_RoundDiv(pclk, AUDIO_ECHO_SAMPLE_RATE_HZ);
    adcTriggerTicks = AudioEcho_RoundDiv(
        pclk, AUDIO_ECHO_SAMPLE_RATE_HZ * AUDIO_ECHO_ADC_ENABLED_CHANNELS);
    if (sampleTicks < 2UL) {
        sampleTicks = 2UL;
    }
    if (sampleTicks > 0xFFFFUL) {
        sampleTicks = 0xFFFFUL;
    }
    if (adcTriggerTicks < 2UL) {
        adcTriggerTicks = 2UL;
    }

    /* Orden de arranque: primero memoria y DMA, despues perifericos sincronizados. */
    AudioEcho_ClearAhbRam();
    AudioEcho_InitDacSilence();
    AudioEcho_InitDmaLinkedLists();
    AudioEcho_ConfigUart();
    AudioEcho_ConfigModeButton();
    AudioEcho_ConfigDmaChannels();
    AudioEcho_ConfigDac(sampleTicks);
    AudioEcho_ConfigSampleTimer(adcTriggerTicks);
    AudioEcho_ConfigAdc();
}

/* Arranca el flujo continuo de audio. */
void AudioEcho_Start(void) {
    DAC_CONVERTER_CFG_T dacCfg;

    /* Primero se habilitan los canales DMA para que esten listos ante el primer request. */
    GPDMA_ChannelStart(AUDIO_ECHO_ADC_DMA_CH);
    GPDMA_ChannelStart(AUDIO_ECHO_DAC_DMA_CH);

    /* Ahora el DAC puede pedir muestras por DMA a su contador interno. */
    dacCfg.doubleBuffer = ENABLE;
    dacCfg.dmaCounter = ENABLE;
    dacCfg.dmaRequest = ENABLE;
    DAC_ConfigDAConverterControl(&dacCfg);

    /* Por ultimo arranca Timer0, que empieza a generar los disparos del ADC. */
    TIM_Enable(LPC_TIM0);
}

/* Detiene el flujo de audio dejando el DAC en media escala. */
void AudioEcho_Stop(void) {
    DAC_CONVERTER_CFG_T dacCfg;

    /* Primero se detiene el timer para que el ADC deje de pedir muestras. */
    TIM_Disable(LPC_TIM0);

    /* Despues se cortan los requests DMA del DAC. */
    dacCfg.doubleBuffer = ENABLE;
    dacCfg.dmaCounter = DISABLE;
    dacCfg.dmaRequest = DISABLE;
    DAC_ConfigDAConverterControl(&dacCfg);

    /* Se espera a que ambos canales GPDMA terminen la transaccion en curso. */
    GPDMA_ChannelGracefulStop(AUDIO_ECHO_ADC_DMA_CH);
    GPDMA_ChannelGracefulStop(AUDIO_ECHO_DAC_DMA_CH);
    /* Media escala equivale a silencio si la senal de audio esta polarizada al centro. */
    DAC_UpdateValue((uint32_t)AUDIO_ECHO_DAC_MID_SCALE);
}

/* Permite consultar desde el entorno de desarrollo si hubo error de DMA en CH2 o CH3. */
uint32_t AudioEcho_GetDmaErrorFlags(void) {
    return s_dmaErrorFlags;
}

/* Permite consultar desde el entorno de desarrollo cuantos bloques fueron procesados. */
uint32_t AudioEcho_GetProcessedBlocks(void) {
    return s_processedBlocks;
}

/* Permite consultar desde el entorno de desarrollo el delay recibido por UART. */
uint32_t AudioEcho_GetCurrentDelayMs(void) {
    return AudioEcho_RoundDiv(s_currentDelaySamples * 1000UL, AUDIO_ECHO_SAMPLE_RATE_HZ);
}

/* Permite consultar desde el entorno de desarrollo el repeat actual como porcentaje de feedback. */
uint32_t AudioEcho_GetCurrentRepeatPercent(void) {
    return AudioEcho_RoundDiv((uint32_t)s_currentRepeatQ15 * 100UL, 32768UL);
}

/* Interrupcion unica del GPDMA: atiende errores y fin de bloque de CH2/CH3. */
void DMA_IRQHandler(void) {
    /* Error en DMA ADC: se limpia la interrupcion y queda marcada la bandera. */
    if (GPDMA_IntGetStatus(GPDMA_INTERR, AUDIO_ECHO_ADC_DMA_CH) == SET) {
        GPDMA_ClearIntPending(GPDMA_CLR_INTERR, AUDIO_ECHO_ADC_DMA_CH);
        s_dmaErrorFlags |= AUDIO_ECHO_DMA_TC_ADC_MASK;
    }

    /* Error en DMA DAC: se limpia la interrupcion y queda marcada la bandera. */
    if (GPDMA_IntGetStatus(GPDMA_INTERR, AUDIO_ECHO_DAC_DMA_CH) == SET) {
        GPDMA_ClearIntPending(GPDMA_CLR_INTERR, AUDIO_ECHO_DAC_DMA_CH);
        s_dmaErrorFlags |= AUDIO_ECHO_DMA_TC_DAC_MASK;
    }

    /* CH2 termino de llenar un bloque ADC. */
    if (GPDMA_IntGetStatus(GPDMA_INTTC, AUDIO_ECHO_ADC_DMA_CH) == SET) {
        GPDMA_ClearIntPending(GPDMA_CLR_INTTC, AUDIO_ECHO_ADC_DMA_CH);
        s_dmaTcMask |= AUDIO_ECHO_DMA_TC_ADC_MASK;
    }

    /* CH3 termino de reproducir un bloque DAC. */
    if (GPDMA_IntGetStatus(GPDMA_INTTC, AUDIO_ECHO_DAC_DMA_CH) == SET) {
        GPDMA_ClearIntPending(GPDMA_CLR_INTTC, AUDIO_ECHO_DAC_DMA_CH);
        s_dmaTcMask |= AUDIO_ECHO_DMA_TC_DAC_MASK;
    }

    /*
     * Solo se procesa cuando ambos DMA terminaron su bloque. Asi la CPU modifica
     * el buffer que ya no esta siendo escrito por ADC ni leido por DAC.
     */
    if ((s_dmaTcMask & AUDIO_ECHO_DMA_TC_BOTH) == AUDIO_ECHO_DMA_TC_BOTH) {
        s_dmaTcMask &= (uint8_t)~AUDIO_ECHO_DMA_TC_BOTH;
        AudioEcho_ProcessBlock(s_processBlock);
        /* Alterna entre bloque 0 y bloque 1: esquema ping-pong. */
        s_processBlock ^= 1U;
    }
}

/* Interrupcion UART0: recibe caracteres y arma comandos sin bloquear el audio. */
void UART0_IRQHandler(void) {
    while ((UART_GetLineStatus(UART0) & UART_LINESTAT_RDR) != 0U) {
        AudioEcho_CaptureUartChar((char)UART_ReceiveByte(UART0));
    }
}

/* Interrupcion del boton: cambia el modo con debounce basado en bloques de audio. */
void EINT0_IRQHandler(void) {
    if (EXTI_GetFlag(AUDIO_ECHO_MODE_BUTTON_LINE) == SET) {
        const uint32_t currentBlock = s_processedBlocks;

        if ((uint32_t)(currentBlock - s_lastModeButtonBlock) >=
            AUDIO_ECHO_MODE_DEBOUNCE_BLOCKS) {
            s_lastModeButtonBlock = currentBlock;
            s_audioMode = (s_audioMode == AUDIO_ECHO_MODE_DELAY)
                              ? AUDIO_ECHO_MODE_SATURATED
                              : AUDIO_ECHO_MODE_DELAY;
            s_modeReportPending = 1U;
        }

        EXTI_ClearFlag(AUDIO_ECHO_MODE_BUTTON_LINE);
    }
}

int main(void) {
    /* Configura el sistema de audio completo. */
    AudioEcho_Init();
    /* Comienza a muestrear, procesar y reproducir. */
    AudioEcho_Start();
    AudioEcho_ReportMode();
    AudioEcho_UartWriteString("Comandos: Tms R% Gx Llim D S ?\r\n");

    while (1) {
        AudioEcho_ProcessUartCommandIfPending();
        AudioEcho_ReportModeIfPending();
        /* La CPU duerme entre interrupciones GPDMA para no gastar ciclos inutiles. */
        __WFI();
    }
}
