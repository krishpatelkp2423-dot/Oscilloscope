# Oscilloscope
A general-purpose DIY oscilloscope built from scratch on a Seeed XIAO ESP32, comparing three signal-acquisition architectures — hardware auto-ranging (window comparators + analog mux), direct ADC sampling, and an external 16-bit SAR ADC — with a custom KiCad PCB and real-time Python visualization.
Components:
Analog Comparators Analog Comparators Quad 1.6V Push/Pull: MCP6544-I/ST
Logic Gates Logic Gates Dual 2-Input Pos A: SN74LVC2G08DCTR
Analog Comparators Analog Comparators MicroPwr CMOS: LPV7215MF/NOPB
Multiplexer Switch ICs 4-Ch. Analog: CD4052BE
Operational Amplifiers - Op Amps Operational Amplifiers - Op Amps High bandwidth (50 MHz) low offset (200 uV) rail-to-rail 5 V op amp: TSV794IYDT
Data Acquisition ADCs/DACs - Specialized Data Acquisition ADCs/DACs - Specialized 16-Bit 1-MSPS 1-Ch S AR ADC with progra A: ADS8681IRUMR
Capacitors: 0.1 µF, 10 µF, 22 µF

Resistor(s)	Value	Circuit Function
R1, R2	10 kΩ each	1.65 V reference / level shift buffer
R3, R6	10 kΩ each	Level shift input and offset
R4, R5	10 kΩ each	Level shift gain
R7	255 kΩ	(function not given)
R11, R14	2.0 kΩ each	Comparator A hysteresis isolation
R12	1 MΩ	Comparator D positive feedback
R13, R31	1 MΩ each	Comparators B & C positive feedback
R19, R21	5 kΩ each	Window 1 ladder (redone)
R20	2 kΩ	Window 1 ladder (redone)
R24, R27	10 kΩ each	Mid-rail 1.65 V for gain stages
R30	1 MΩ	Comparator A positive feedback
