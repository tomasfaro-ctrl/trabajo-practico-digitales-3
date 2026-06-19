### DELAY DE GUITARRA 
> # Asignatura: Electrónica Digital 3 - Universidad Nacional de Córdoba
> # Integrantes:
>  Renata Monaldi, Lautaro Ismael Bazan, Tomas Faro
> # Profesor:Marcos Blasco
---
 ## &#x20;🚀 1. Descripción General del Proyecto .

 La idea del proyecto es realizar un delay de sonido, es un conocido pedal utilizado para generar efectos de audio en el cual el sonido se repite una cierta cantidad de veces. Esta enfocado específicamente a aquellos que deseen agregar un efecto a su guitarra o microfono.

## 🎯 Alcances del Proyecto

# Que hace el sistema:
Captura y reproduce en tiempo real una señal, mediante una frecuencia de muestreo de 22khz se captura la señal de entrada y luego se procesa matemáticamente en una interrupción. El sistema utiliza DMA para guardar los datos, y moverlos entre dos bancos de memoria distintos dado que en uno se guardan ,y en el otro se procesan y mueven al dac. El procesador solo se activa mediante una interrupción de dma al momento de procesar datos para agregarle el efecto de sonido.
Mediante UART se ajustan el tiempo de duración del efecto y otro que modifica la separación entre el tiempo donde se vuelve a inyectar una repetición.



	Digitaliza una señal de audio analógica de hasta 11khz, la procesa y luego la reconstruye sincronicamente mediante un DAC. 
	
	delay y distorsion de sonido ajustable.

	Control de los parámetros de delay y distorsion mediante Uart en conexión a pc mediante usb.

	calidad de audio media debido a los 22khz

	tiempo de delay no mayor a 0,5 Segundos debido a la limitación de memoria
	
	Ruido debido a las etapas de amplificación de baja calidad.
-
# ⏩ Posibles Etapas Siguientes (Líneas Futuras)
Planteen cómo escalaría este desarrollo en una versión 2.0 o en un ámbito profesional:
Se Utilizaria un amplificador de señal que este adaptado específicamente para audio de estas frecuencias, reduciendo el ruido, además con el mismo objetivo podemos utilizar una placa de pertinax o una pcb hecha a medida. Las capacidades utilizadas para el filtrado pueden ser ajustadas. Por ultimo es posible mejorar algunas funciones del firmware para reducir ruido y ampliar la cantidad de muestras tomadas, por lo que podría aumentarse el tiempo de delay.


### 📐 2. Arquitectura del Sistema: Hardware y Software 
🔌 Hardware & Interconexión

 Diagrama de Bloques del funcionamiento del sistema.
<img width="1408" height="768" alt="WhatsApp Image 2026-06-18 at 12 03 46 PM" src="https://github.com/user-attachments/assets/df131fbd-feed-487a-8d8e-1e1f5706eccf" />

 Esquemático del Circuito:
<img width="778" height="526" alt="WhatsApp Image 2026-06-18 at 12 41 53 PM" src="https://github.com/user-attachments/assets/b29d036a-0160-4482-8c23-3c18f5b62b13" />

Descripción del Circuito y Consideraciones de Diseño: 	

La placa LPC1769 no está diseñada para procesar directamente señales de audio alternas que presentan componentes positivas y negativas respecto de masa, ya que la entrada del ADC solo admite tensiones dentro del rango comprendido entre 0 V y 3,3 V. Por este motivo, se implementó una etapa de acondicionamiento de señal que permite adaptar la señal de audio a dicho rango de trabajo. En primer lugar, se aplicó un offset de continua mediante un divisor resistivo, centrando la señal alrededor de un valor medio adecuado para el ADC. Posteriormente, se utilizó un amplificador operacional con el objetivo de ajustar la amplitud de la señal y aprovechar de manera más eficiente el rango dinámico disponible en la conversión analógico-digital.



---
## ⚡ 3. Especificaciones Eléctricas, Alimentación y Entorno (Específico por Asignatura)

🔌 Parámetros de Alimentación y Consumo (Común a ambas materias)

Tensión de operación del sistema: 9V y 3.3V
Método de alimentación: Batería y pines de alimentación de la LPC1769
Consumo estimado o medido: 176mWh


IDE y SDK:  MCUXpresso IDE v11.8 con LPCOpen v2.10 
Microcontrolador Principal:  NXP LPC1769  
Periféricos Avanzados Utilizados:  NVIC, DMA, SysTick, DAC

## 🔄 4. Proceso de Integración y Desarrollo (Común)

Etapa 1 (Validación inicial): Implementación del adc y dac sin dma, acondicionamiento de la señal de entrada y salida.

Etapa 2 (Adquisición/Comunicación): implementación del dma.

Etapa 3 (Integración lógica): implementación del buffer de datos con el respectivo procesamiento de la señal en los dos modos con potenciometros.

Etapa 4 (Sistema Completo): adaptación del Uart.

---
## 📊 5. Ensayos, Pruebas y Resultados (Común)

Demuestren con datos empíricos que el sistema funciona correctamente. Es obligatorio incluir registro visual.




<img width="598" height="564" alt="WhatsApp Image 2026-06-18 at 8 39 28 PM" src="https://github.com/user-attachments/assets/ab5be071-38d5-4ae4-98c1-ad819272040c" />


Pruebas Funcionales Realizadas: Se inyecto señales con un generador de onda, también mediante el Jack de salida de audio de una computadora y por ultimo con una guitarra eléctrica stratocaster.

Evidencia Fotográfica y Gráficos:
https://github.com/user-attachments/assets/956acea3-147b-4a94-a1e5-bea3975f4d62



---
## 📂 6. Estructura del Repositorio (Común)
El repositorio debe mantener obligatoriamente la siguiente estructura limpia (¡Recuerden configurar correctamente el `.gitignore` para no subir carpetas temporales como `Debug/`, `Release/` o archivos `.p1` / `.d`!).
```text
├── firmware/          # Código fuente del proyecto (MPLABX / MCUXpresso / STM32Cube)
│   ├── src/           # Archivos de código (.c)
│   └── inc/           # Archivos de cabecera (.h)
├── hardware/          # Archivos de diseño (KiCad/Altium), esquemáticos en PDF/Imagen y BOM
├── docs/              # Datasheets clave, imágenes del README, notas de aplicación
└── README.md          # Este archivo de presentación
puedes acomodarlo para que se vea  bien en github?
