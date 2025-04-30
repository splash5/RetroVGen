/*
	"RetroVGen" : Retro RGB Video Signal Generator
	Hiroaki GOTO as GORRY / http://GORRY.hauN.org/
	2023/01/05 Version 20230105a

	"RetroVGen168" : A modified version of RetroVGen which fits into ATmega168 (ATmega328/88 should also work).
	Code by splash5 / https://github.com/splash5/RetroVGen
	2025/05/01 Version 20250501
*/

/*
	Connections:

	D1	: Pixel output (150 ohms in series to each one of R, G, B)	 --> Pins 1, 2, 3 on DB15 socket
	D3	: Vertical Sync (68 ohms in series) --> Pin 14 on DB15 socket
	D8	: LED Flash sync with VSYNC
	D9	: mode sw 4
	D10 : Horizontal Sync (68 ohms in series) --> Pin 13 on DB15 socket
	D11 : mode sw 1
	D12 : mode sw 2
	D13 : mode sw 3

	Gnd : --> Pins 5, 6, 7, 8, 10 on DB15 socket
*/
