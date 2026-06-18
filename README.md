¡Aquí tienes todo tu informe estructurado y optimizado con sintaxis Markdown profesional para GitHub! He pulido la redacción técnica para que suene con un nivel de ingeniería excelente, manteniendo toda la información original y agregando los diagramas que analizamos para que tu repositorio quede impecable.

Copia y pega el siguiente bloque directamente en tu archivo `README.md`:

```markdown
# Pedal de Efectos DSP en Tiempo Real con LPC1769

> **Asignatura:** Electrónica Digital III  
> **Universidad:** Universidad Nacional de Córdoba (UNC) - Facultad de Ciencias Exactas, Físicas y Naturales  
> **Profesor:** Ing. Marcos Blasco  

---

## 👥 Integrantes
| Nombre y Apellido | Correo Electrónico / Contacto | GitHub User |
| :--- | :--- | :--- |
| Renata Monaldi | [Email] | @usuario |
| Lautaro Ismael Bazan | [Email] | @usuario |
| Tomas Faro | [Email] | @usuario |

---

## 🚀 1. Descripción General del Proyecto

Este proyecto consiste en el diseño e implementación de un sistema embebido de procesamiento digital de señales de audio (DSP) en tiempo real, desarrollado sobre la plataforma del microcontrolador NXP LPC1769. El dispositivo funciona como un pedal de efectos multiparamétrico capaz de conmutar dinámicamente entre dos modos de procesamiento: un efecto de **Delay (Eco)** con retroalimentación y un efecto de **Saturación (Distorsión por Hard-Clipping)**. El sistema está especialmente diseñado para músicos, guitarristas o técnicos de sonido que requieran un módulo modular de audio de código abierto para procesar señales analógicas provenientes de instrumentos o micrófonos.

La solución resuelve de manera eficiente el clásico problema de latencia y sobrecarga de CPU en microcontroladores mediante una arquitectura basada en hardware independiente (**GPDMA** en configuración **Ping-Pong**). Esto permite desvincular la transferencia masiva de datos entre los periféricos de conversión (ADC/DAC) de las operaciones aritméticas críticas ejecutadas por la CPU, logrando un flujo de audio continuo, libre de interrupciones o pérdidas de muestras (*glitches*), y todo bajo un esquema físico de bajo costo y alta fidelidad en comparación con plataformas comerciales cerradas.

---

## 🎯 Alcances del Proyecto

Para delimitar el desarrollo de esta entrega final, se especifican a continuación los objetivos alcanzados y las restricciones físicas del sistema:

### ✅ ¿Qué HACE el sistema? (Alcances)
* **Procesamiento Síncrono a 22 kHz:** Captura y reproduce señales analógicas mediante muestreo estricto disparado por hardware (`Timer 0`), procesando el espectro audible fundamental (hasta 11 kHz por Teorema de Nyquist).
* **Arquitectura de Doble Búfer (Ping-Pong):** Automatiza el transporte de muestras usando canales de DMA. La CPU calcula el bloque de datos `N` mientras el hardware almacena en paralelo el bloque `N+1`, eliminando colisiones en memoria.
* **Algoritmos DSP Integrados:** * *Modo Delay:* Genera ecos espaciados y controlados mediante una línea de retraso circular dinámica.
  * *Modo Saturación:* Aplica ganancia lineal (*Drive*) y recorta simétricamente la señal (*Hard-Clipping*) al superar los umbrales límite establecidos.
* **Eliminación Dinámica de DC Offset:** Filtra la componente continua introducida por el circuito de acondicionamiento mediante un seguidor digital pasabajos exponencial integrado en el firmware.
* **Compuerta de Ruido (*Noise Gate*):** Atenúa a cero absoluto las muestras de bajo nivel (ruido térmico de cables o captor) cuando el instrumento no está en ejecución.
* **Control Remoto por Terminal:** Modificación en tiempo real de los parámetros operativos (tiempo de delay, feedback, ganancia y límites) mediante comandos por consola serial (**UART0**).
* **Acondicionamiento de Señal Analógica:** Implementación de hardware para adecuar la señal alterna (AC) al rango unipolar del ADC (0 a 3.3V) usando un divisor resistivo de bias y amplificadores operacionales. Etapa de salida aislada mediante capacitor de desacople.

### ❌ ¿Qué NO HACE el sistema? (Limitaciones)
* **Audio de Alta Fidelidad (Hi-Fi):** La tasa de muestreo está limitada a 22 kHz debido a la optimización del tiempo de procesamiento por bloque, no alcanzando la calidad de un CD comercial (44.1 kHz).
* **Delay de Largo Alcance:** El tiempo de eco máximo es de **500 ms** debido a la capacidad de almacenamiento reservada dentro de la memoria rápida interna (`AHB SRAM`).
* **Supresión Total de Ruido Analógico:** El prototipo actual presenta un piso de ruido heredado de las etapas de amplificación básicas en protoboard.
* **Procesamiento Estéreo:** La arquitectura de procesamiento es estrictamente monoaural.

---

## ⏩ Posibles Etapas Siguientes (Líneas Futuras)
* **Diseño de PCB dedicada:** Migrar el circuito analógico de protoboard a un circuito impreso (PCB) en FR4 con planos de tierra independientes para aislar el ruido digital del analógico.
* **Acondicionamiento de audio profesional:** Incorporar amplificadores operacionales específicos para audio (ej. TL072 o NE5532) que mitiguen el siseo y mejoren la impedancia de entrada para instrumentos pasivos.
* **Optimización de Firmware:** Refactorizar el procesamiento matemático a nivel de ensamblador o funciones CMSIS-DSP intrínsecas para aumentar la frecuencia de muestreo a 44.1 kHz.
* **Expansión de Memoria:** Utilizar almacenamiento externo o remapear bloques de la memoria flash para extender el tiempo de delay más allá de los 0.5 segundos.

---

## 📐 2. Arquitectura del Sistema

### 🔌 Hardware & Interconexión
El sistema requiere el acoplamiento correcto entre el mundo analógico de la señal de audio y el entorno digital del microcontrolador. 

```text
  +-------------------------------------------------------------------------+
  |                           ENTRADAS Y CONTROL                            |
  |                                                                         |
  |  [ Guitarra/Mic ] -> [ Amp. Op + Bias ] -> (Pin P0.23 / AD0.0) -> [ADC] |
  |  [ Pulsador de Modo ] -------------------> (Pin P2.10 / EINT0)  |       |
  |  [ Consola Serial PC ] <-----------------> (P0.2/P0.3 / UART0)  |       |
  +-----------------------------------------------------------------|-------+
                                                                    v
+---------------------------------------------------------------------------+
|                          MICROCONTROLADOR LPC1769                         |
|                                                                           |
|   [ TIMER 0 ] ------------> Disparo determinístico del ADC (22 kHz)       |
|                                                                           |
|   [ GPDMA CH2 ] ----------> Mueve muestras ADC -> SRAM (Fase Grabación)   |
|                                                                           |
|   [ AHB SRAM ] -----------> Búferes Ping-Pong + Línea de Retraso          |
|        |                                                                  |
|        v (Interrupción DMA despierta la CPU)                              |
|   [ CPU Cortex-M3 ] ------> Procesa algoritmos (Delay / Saturación)       |
|        |                                                                  |
|        v                                                                  |
|   [ GPDMA CH3 ] ----------> Mueve muestras SRAM -> DAC (Fase Reproducción)|
+---------------------------------------------------------------------------+
                                                                    |
                                                                    v
  +-------------------------------------------------------------------------+
  |                             ETAPA DE SALIDA                             |
  |                                                                         |
  |  [ DAC (P0.26) ] -> [ Filtro + Cap. Desacople ] -> [ Parlante / Amp ]   |
  +-------------------------------------------------------------------------+

```

* **Diagrama de Bloques Funcional:**  *(Reemplazar ruta por la imagen correspondiente)*
* **Esquemático del Circuito (KiCad/Altium):** 

#### Consideraciones de Diseño Analógico:

1. **Etapa de Entrada:** Dado que la señal de audio es una señal alterna que oscila en valores negativos y positivos, se implementó un divisor resistivo conectado a 3.3V para generar un nivel de continua (Offset de 1.65V). Un amplificador operacional eleva la amplitud para aprovechar la resolución de 12 bits del ADC.
2. **Etapa de Salida:** Para evitar dañar el parlante con tensión continua, se colocó un capacitor en serie que actúa como filtro pasa-altos de acoplamiento AC, removiendo el offset antes de la entrega final de potencia.

### 💻 Arquitectura de Software (Firmware)

El lazo principal del programa se mantiene en modo de bajo consumo (`wfi`), reaccionando estrictamente ante los eventos administrados por el controlador de interrupciones (**NVIC**).

```text
                           [ INICIO (main) ]
                                   |
                                   v
                     +---------------------------+
                     | Inicializar Periféricos   |
                     |  - ADC, DAC, Timer0       |
                     |  - GPDMA (Ping-Pong LLI)  |
                     |  - EINT0, UART0           |
                     +---------------------------+
                                   |
                                   v
                      +--------------------------+
             +------> |   CPU en Reposo (WFI)    | <-------+
             |        +--------------------------+         |
             |                     |                       |
             |         (Ocurre IRQ de GPDMA)               |
             |                     v                       |
             |        +--------------------------+         |
             |        |    DMA_IRQHandler()      |         |
             |        |  - Borra flags internos  |         |
             |        |  - Intercambia Bloque    |         |
             |        +--------------------------+         |
             |                     |                       |
             |                     v                       |
             |        +--------------------------+         |
             |        |  AudioEcho_ProcessBlock  |         |
             |        |  - Quita Offset & Gate   |         |
             |        |  - Computa Modo Activo   |         |
             |        |  - Actualiza Buffers     |         |
             |        +--------------------------+         |
             |                     |                       |
             +---------------------+-----------------------+

```

* **Diagrama de Flujo del Firmware:** 

---

## ⚡ 3. Especificaciones Eléctricas y Entorno

* **Tensión de Operación del Sistema:** 9V (Línea analógica exterior) y 3.3V (Alimentación digital del microcontrolador).
* **Método de Alimentación:** Batería de 9V para etapas analógicas y riel de alimentación regulado mediante pines de la placa de desarrollo LPC1769.
* **Consumo Energético Estimado:** ~176 mW bajo régimen de procesamiento activo.

### 📌 Especificaciones exclusivas de Electrónica Digital III

* **IDE y Herramientas:** MCUXpresso IDE v11.8.
* **SDK Empleado:** LPCOpen v2.10.
* **Microcontrolador Central:** ARM Cortex-M3 NXP LPC1769 (Frecuencia de reloj de la CPU a 120 MHz).
* **Periféricos Avanzados Utilizados:** * `NVIC`: Priorización de eventos de DMA, UART0 e interrupciones externas.
* `GPDMA`: Modos de transferencia ráfaga M2P (SRAM a DAC) y P2M (ADC a SRAM) mediante listas enlazadas (LLI).
* `TIMER 0`: Generación de la base de tiempo estricta para la tasa de muestreo.
* `DAC`: Reconstrucción de la señal analógica con resolución de 10 bits.



---

## 🔄 4. Proceso de Integración y Desarrollo

El diseño del proyecto siguió un estricto **enfoque modular de ingeniería**, validando bloques individuales antes de proceder a la integración sistémica:

1. **Etapa 1 (Validación Analógica e I/O):** Configuración básica del ADC y el DAC mediante encuestas (polling) sencillas. Ajuste empírico en el laboratorio de las etapas analógicas de ganancia y offset mediante osciloscopio.
2. **Etapa 2 (Automatización por Hardware):** Despliegue del controlador GPDMA. Se configuraron los descriptores de Listas Enlazadas (LLI) para estructurar el intercambio automático de memoria en modo Ping-Pong sin requerir código de la CPU.
3. **Etapa 3 (Integración Lógica y DSP):** Escritura de las rutinas de procesamiento de señal. Implementación matemática del búfer circular para el Delay y las funciones de saturación por software, acoplándolas a la interrupción de fin de bloque del DMA.
4. **Etapa 4 (Sistema Completo e Interfaces):** Integración de la UART0 para la recepción asíncrona de variables externas y acoplamiento de la interrupción del pulsador físico con rutinas de eliminación de rebote (*debouncing*).

---

## 📊 5. Ensayos, Pruebas y Resultados

Para certificar el correcto desempeño del procesador de audio, se realizaron pruebas bajo tres configuraciones de entrada diferentes:

* **Inyección de Ondas de Laboratorio:** Uso de generador de señales con ondas senoidales y triangulares (100 Hz a 5 kHz) para verificar el rango de clipping en modo saturación y comprobar visualmente el retardo en el osciloscopio.
* **Línea de Audio de PC:** Reproducción de pistas musicales directas desde un Jack de 3.5 mm para evaluar la fidelidad general de la tasa de muestreo a 22 kHz.
* **Prueba de Campo Real:** Conexión directa de una guitarra eléctrica analógica tipo *Stratocaster*, validando la efectividad de la compuerta de ruido (`Noise Gate`) en el silencio y la respuesta dinâmica del efecto de eco.

### Evidencia Fotográfica e Instrumental

* **Captura de Osciloscopio (Señal limpia vs. Señal con Delay):** 
* **Fotografía del Prototipo Final en Funcionamiento:** 

---

## 📂 6. Estructura del Repositorio

```text
├── firmware/          # Código fuente del proyecto (Proyecto de MCUXpresso)
│   ├── src/           # Archivos de código fuente (.c) con lógica de inicialización y algoritmos DSP
│   └── inc/           # Archivos de cabecera (.h) con definiciones de registros y constantes
├── hardware/          # Archivos de diseño (KiCad/Altium), esquemáticos en PDF/Imagen y BOM
├── docs/              # Datasheets clave, imágenes del README, notas de aplicación
└── README.md          # Este archivo de presentación

```

```

```
