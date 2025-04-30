/*
 * RetroVGen168 by splash5
 * https://github.com/splash5/RetroVGen/
 *
 * Orignal RetroVGen by gorry
 * https://github.com/gorry/RetroVGen
 *
 * RetroVGen168與原始RetroVGen的不同處:
 * 1. 整體運作邏輯沒有變化, 但因為要節省記憶體(<=1KB), 所以改寫了產生文字與視窗的部份
 * 2. 輸出文字點陣的部份使用組合語言改寫, 以符合各個顯示模式要求的水平同步頻率
 * 3. 移除TimerHelpers.h, 改為自行對TIMER1控制
 * 4. 其它小修改 (BCD frame counter, 不需按RESET鈕即可切換模式, ...)
 *
 * 開發環境使用Atmel Stdio 7 + PICKit4 (但仍可使用Arduno IDE建置專案)
 * 目前program space使用4368(Atmel Studio) ~ 4646(Arduino) bytes
 *     ram space使用463(Atmel Studio) ~ 472(Arduino) bytes
 * 測試螢幕為BENQ BL702A
 *
 * Arduino build setting for ATmega168:
 * Board: Arduino Nano
 * Processor: ATmega168
 * ATmega328(Uno, Nano) should also works without any problem.
 * Fuse setting - E:0xF8, H:0xDA, L:0xF7
 */

#include <avr/pgmspace.h>
#include <avr/sleep.h>
#include <avr/interrupt.h>

typedef uint8_t byte;

#include "videoparam.h"
#include "screenFont.h"

#define INVERSED_MODE_SW
#define ACTIVE_REGION_CNT     0x57

#define INFO_WINDOW_WIDTH     18
#define INFO_WINDOW_HEIGHT    4
#define VCOUNT_WINDOW_WIDTH   4
#define VCOUNT_WINDOW_HEIGHT  3

#define WINDOW_WIDTH          50
#define WINDOW_HEIGHT         (INFO_WINDOW_HEIGHT + VCOUNT_WINDOW_HEIGHT + 1)

#define WINDOW_BORDER_CHAR    ' '

#define VSYNC_LED_BIT   PB0
#define VSYNC_BIT       PD3

static uint8_t hsync_mode;
static uint8_t sw_mode;
static uint16_t hsync_count;
static uint8_t vsync_count; // BCD

static uint8_t vblank_lines;
static uint16_t vdisp_lines;

static ScreenParam s_param;
static uint8_t window_start_line; // window start from this line
static uint16_t vcount_char_index;// vcount char index in window_buf

// for rendering
static uint16_t font;
static uint16_t text_line;

static const uint8_t num_chars[] =
{
  0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
  0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
  0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
  0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
  0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39
};

static uint8_t window_buf[WINDOW_WIDTH * (INFO_WINDOW_HEIGHT + VCOUNT_WINDOW_HEIGHT)];

static inline uint8_t get_mode_sw_value(void)
{
  uint8_t ret;

  asm volatile
  (
  "in %[ret], %[pinb]\n\t"
  "bst %[ret], 1\n\t"
  "lsl %[ret]\n\t"
  "swap %[ret]\n\t"
  "bld %[ret], 3\n\t"
#ifdef INVERSED_MODE_SW
  "com %[ret]\n\t"  // switch on is LOW, switch off is H
#endif
  "andi %[ret], 0x0f\n\t"
  : [ret] "=d" (ret)
  : [pinb] "I" (_SFR_IO_ADDR(PINB))
  );

  return ret;
}

static uint16_t build_window(ScreenParam *param, uint8_t window_y)
{
  uint8_t *ch, *line;
  uint8_t i, window_x;
  uint16_t vcount_ch_index;

  line = window_buf;
  i = 0;

  do
  {
    if (i == 4)
      continue;

    ch = line;

    for (window_x = 0; window_x < window_y + i; window_x++)
      *ch++ = num_chars[window_y + i];

    for (; window_x < param->mHorizontalChars; window_x++)
      *ch++ = num_chars[window_x];

    line += WINDOW_WIDTH;

  } while (++i < WINDOW_HEIGHT);

  // info window
  line = window_buf + ((param->mHorizontalChars - INFO_WINDOW_WIDTH) >> 1);
  ch = line;

  // top border
  for (i = 0; i < INFO_WINDOW_WIDTH; i++)
    *ch++ = WINDOW_BORDER_CHAR;

  // msg1
  line += WINDOW_WIDTH;
  ch = line;
  
  *ch++ = WINDOW_BORDER_CHAR;
  for (i = 0; i < sizeof(param->mMsg1); i++)
    *ch++ = param->mMsg1[i];
  *ch++ = WINDOW_BORDER_CHAR;

  // msg2
  line += WINDOW_WIDTH;
  ch = line;

  *ch++ = WINDOW_BORDER_CHAR;
  for (i = 0; i < sizeof(param->mMsg2); i++)
    *ch++ = param->mMsg2[i];
  *ch++ = WINDOW_BORDER_CHAR;

  // bottom border
  line += WINDOW_WIDTH;
  ch = line;
  for (i = 0; i < INFO_WINDOW_WIDTH; i++)
    *ch++ = WINDOW_BORDER_CHAR;

  // vcount window start
  line += (WINDOW_WIDTH + ((INFO_WINDOW_WIDTH - VCOUNT_WINDOW_WIDTH) >> 1));

  // top border
  ch = line;
  for (i = 0; i < VCOUNT_WINDOW_WIDTH; i++)
    *ch++ = WINDOW_BORDER_CHAR;

  // vcount line
  line += WINDOW_WIDTH;
  vcount_ch_index = line - window_buf;

  // bottom border
  line += WINDOW_WIDTH;
  ch = line;
  for (i = 0; i < VCOUNT_WINDOW_WIDTH; i++)
    *ch++ = WINDOW_BORDER_CHAR;

  return vcount_ch_index;
}

static inline void prepare_render_line_0(uint8_t line_doubler)
{
    asm volatile
    (
    "sub %A[display_lines], %A[hsync_count]\n\t"
    "sbc %B[display_lines], %B[hsync_count]\n\t"    // get current screen_line
    "tst %[line_doubler]\n\t"
    "breq get_font_ptr_%=\n\t"
    "lsr %B[display_lines]\n\t"
    "ror %A[display_lines]\n\t"                     // screen_line >> 1 if needs line_doubler
    "get_font_ptr_%=:\n\t"    
    "mov %B[hsync_count], %A[display_lines]\n\t"
    "andi %B[hsync_count], 0x07\n\t"
    "clr %A[hsync_count]\n\t"                       // get screen_font array index
    "subi %A[hsync_count], lo8(-(screen_font))\n\t"
    "sbci %B[hsync_count], hi8(-(screen_font))\n\t" // get pointer to screen_font[index]
    "sts %[text_line], %A[display_lines]\n\t"
    "sts %[text_line] + 1, %B[display_lines]\n\t"   // store for later pass #1
    "sts %[font], %A[hsync_count]\n\t"
    "sts %[font] + 1, %B[hsync_count]\n\t"          // store for later rendering
    : 
    : [hsync_count] "d" (hsync_count), [display_lines] "a" (vdisp_lines), [line_doubler] "a" (line_doubler),
      [font] "i" (&font), [text_line] "i" (&text_line)
    );
}

static inline void prepare_render_line_1(uint8_t **text, uint8_t *repeat_chars)
{
    asm volatile
    (
    // rotate here so prepare_render_line_0 can finish before timer1 overflow
    "lsr %B[text_line]\n\t"
    "ror %A[text_line]\n\t"
    "lsr %B[text_line]\n\t"
    "ror %A[text_line]\n\t"
    "lsr %B[text_line]\n\t"
    "ror %A[text_line]\n\t"                         // get text_line index
    "mov __tmp_reg__, %A[text_line]\n\t"            // backup so we can use it later
    "sub %A[text_line], %[window_start_line]\n\t"
    "cpi %A[text_line], %[M_WINDOW_HEIGHT]\n\t"
    "brcc outside_window_2_%=\n\t"                  // window_line_index > WINDOW_HEIGHT
    "cpi %A[text_line], %[M_INFO_WINDOW_HEIGHT]\n\t"
    "breq outside_window_%=\n\t"                    // window_line #4 is treat as regular text line
    "brcs inside_window_%=\n\t"                     // window_line #0, #1, #2, #3 at window_buf line #0, #1, #2, #3
    "dec %A[text_line]\n\t"                         // window_line #5, #6, #7 at window_buf line #4, #5, #6
    "inside_window_%=:\n\t"
    "ldi %[window_start_line], %[M_WINDOW_WIDTH]\n\t"
    "mul %A[text_line], %[window_start_line]\n\t"   // get index in  window_buf, r1 dirty
    "movw %A[text], r0\n\t"
    "subi %A[text], lo8(-(window_buf))\n\t"
    "sbci %B[text], hi8(-(window_buf))\n\t"         // get text pointer to window_buf
    "clr %[repeat_chars]\n\t"                       // repeat_chars = 0
    "clr r1\n\t"                                    // restore r1 = __zero_reg__
    "rjmp end_prepare_render_line_1_%=\n\t"
    "outside_window_2_%=:\n\t"
    "nop$nop$"
    "outside_window_%=:\n\t"
    "movw %A[text], __tmp_reg__\n\t"                // get index to num_chars
    "subi %A[text], lo8(-(num_chars))\n\t"
    "sbci %B[text], hi8(-(num_chars))\n\t"          // get pointer to num_chars
    "mov %[repeat_chars], __tmp_reg__\n\t"          // repeat first char for text_line times
    "nop$nop$nop$nop$nop$nop$nop$"
    "end_prepare_render_line_1_%=:\n\t"
    : [text] "=e" (*text), [repeat_chars] "=d" (*repeat_chars)
    : [text_line] "d" (text_line), [window_start_line] "d" (window_start_line),
      [M_WINDOW_WIDTH] "M" (WINDOW_WIDTH), [M_WINDOW_HEIGHT] "M" (WINDOW_HEIGHT), [M_INFO_WINDOW_HEIGHT] "M" (INFO_WINDOW_HEIGHT)
    );
}

// only for waking up CPU when hsync
EMPTY_INTERRUPT(TIMER1_OVF_vect);

void setup()
{
  // PB0: VSYNC_LED, PB2: HSYNC, PB6,7: XTAL, PB1,3,4,5: SW
  // SW needs internal pull-up enabled
  DDRB = 0b11000101;
  PORTB = 0b00111010;

  // PD1: RGB, PD3: VSYNC, PD4: XCK for USART (pixel clock, not used)
  DDRD = 0b00011010;
  PORTD = 0b11101101; // vsync default is high

  // PORTC for debug
  DDRC = 0b00000000;
  PORTC = 0b11111111;
  
  // disable 8bit TIMER0 and TIMER2 interrupt
  // delay won't work!
  TIMSK0 = 0;
  OCR0A = 0;
  OCR0B = 0;
  TIMSK2 = 0;
  OCR2A = 0;
  OCR2B = 0; 

  // setup 16bit TIMER1 for HSYNC
  // set fast PWM, mode 15 in datasheet
  TCCR1A = ((1 << COM1B1) | (1 << WGM11) | (1 << WGM10));
  TCCR1B = ((1 << WGM13) | (1 << WGM12));
  TIMSK1 = (1 << TOIE1);  // enable overflow interrupt for TIMER1 (so cpu can awake up every hsync)
  TIFR1 = (1 << TOV1);    // clear TIMER1 overflow flag

  // set USART to SPI mode for RGB output
  UBRR0 = 0;  // pixel clock is fixed to 16 / (2 * (UBRR0 + 1)) = 8MHz
  UCSR0B = 0; // disable USART
  UCSR0C = ((1 << UMSEL00) | (1 << UMSEL01)   // set USART to SPI mode
          | (1 << UCPHA0)  | (1 << UCPOL0));  // use SPI mode 3 (data out on falling edge)

  // sleep to idle when waiting for hsync
  SMCR = 0x00;

  hsync_mode = 0;
  sw_mode = get_mode_sw_value();
}

void loop()
{
  while (1)
  {
    switch (hsync_mode)
    {
      case 0: // init mode
      {
        // copy screen param to ram
        memcpy_P(&s_param, &sScreenParam[sw_mode], sizeof(ScreenParam));

        uint16_t lines = s_param.mVerticalTotalLines - s_param.mVerticalSyncLines - s_param.mVerticalFrontPorch - s_param.mVerticalBackPorch;
        uint16_t disp_lines = (s_param.mVerticalChars << (s_param.mLineDoubler + 3));
        uint16_t blank_lines = (lines > disp_lines ? lines - disp_lines : 0);
    
        vdisp_lines = (lines > blank_lines ? lines - blank_lines : 0);
        vblank_lines = blank_lines;

        // build window content
        window_start_line = ((s_param.mVerticalChars - INFO_WINDOW_HEIGHT) >> 1);
        vcount_char_index = build_window(&s_param, window_start_line);

        // setup hsync and hsync pulse width
        OCR1A = s_param.mOCR1A;
        OCR1B = s_param.mOCR1B;

        hsync_count = 0;
        vsync_count = 0;
        hsync_mode = 1;

        // start timer1
        TCCR1B |= (1 << CS10);  // enable TIMER1 clock source
        sei();

        break;
      }
      case 1: // before vsync start
      {
        PORTD |= (1 << VSYNC_BIT);

        // LED turns on only when in frame 0, 1, 2, 3
        if (vsync_count < 4)
        {
          window_buf[vcount_char_index] = 0xff;
          PORTB |= (1 << VSYNC_LED_BIT);
        }
        else
        {
          window_buf[vcount_char_index] = ' ';
          PORTB &= ~(1 << VSYNC_LED_BIT);
        }

        window_buf[vcount_char_index + 1] = num_chars[(vsync_count >> 4)];
        window_buf[vcount_char_index + 2] = num_chars[(vsync_count & 0x0f)];
        window_buf[vcount_char_index + 3] = window_buf[vcount_char_index];

        hsync_mode = 2;
        hsync_count = s_param.mVerticalSyncLines;
        break;
      }
      case 2: // vsync line
      {
        if (--hsync_count == 0)
        {
          PORTD &= ~(1 << VSYNC_BIT);

          // go back porch
          hsync_mode = 3;
          hsync_count = s_param.mVerticalBackPorch;
        }

        break;
      }
      case 3: // vertical back porch
      {
        if (--hsync_count == 0)
        {
          hsync_mode = 4;
          hsync_count = vdisp_lines;
      
          // first line, first pixel row
          // no need to call prepare_render_line_0()
          font = (uint16_t)screen_font;
          text_line = 0;
        }

        break;
      }
      case 4: // display one pixel line
      {
        uint8_t *text, repeat_chars;
        prepare_render_line_1(&text, &repeat_chars);
    
        while (TCNT1L < ACTIVE_REGION_CNT);
    
        asm volatile
        (     
        // enable transmission on TX (RGB signal)
        "lds r30, %[ucsr0b]\n\t"
        "ori r30, %[txen0]\n\t"
        "sts %[ucsr0b], r30\n\t"
        // loop begins here, each loop takes 16 cycles -- matches SPI tx rate (1byte/1us)
        // load char line bitmap
        "load_next_render_font_%=:\n\t"
        "ld r30, %a[text]+\n\t"               // 2
        "add r30, %A[font]\n\t"               // 1
        "mov r31, %B[font]\n\t"               // 1
        "adc r31, __zero_reg__\n\t"           // 1
        "lpm r30, Z\n\t"                      // 3, result @ r30
        // send char line bitmap out
        "send_font_data_%=:\n\t"
        "sts %[udr0], r30\n\t"                // 2, timer1 counter should >= h back porch range
        "nop\n\t"                             // 1, 
        // all chars in line rendered?
        "dec %[i]\n\t"                        // 1
        "breq end_pixel_line_rendering_%=\n\t"// 1/2
        // check if next char is same char?
        "tst %[repeat_chars]\n\t"             // 1
        "breq load_next_render_font_%=\n\t"   // 2/1
        // skip loading next char
        "dec %[repeat_chars]\n\t"             // 1
        "nop$nop$nop$nop$nop$nop\n\t"         // 6, consume same cycles as no skip
        "rjmp send_font_data_%=\n\t"          // 2
        // loop end
        "end_pixel_line_rendering_%=:\n\t"
        "lds r30, %[ucsr0a]\n\t"              // wait for transmission complete
        "sbrs r30, 6\n\t"
        "rjmp end_pixel_line_rendering_%=\n\t"
        "lds r30, %[ucsr0b]\n\t"              // disable transmission, tx is L
        "andi r30, 0xf7\n\t"
        "sts %[ucsr0b], r30\n\t"
        :
        : [i] "l" (s_param.mHorizontalChars), [font] "a" (font), [text] "e" (text), [repeat_chars] "d" (repeat_chars),
          [ucsr0a] "M" (_SFR_MEM_ADDR(UCSR0A)),
          [ucsr0b] "M" (_SFR_MEM_ADDR(UCSR0B)),
          [udr0] "M" (_SFR_MEM_ADDR(UDR0)),
          [txen0] "M" ((1 << TXEN0))
        : "r30", "r31"
        );
    
        uint16_t hc = hsync_count - 1;

        if (hc == 0)
        {
          if (vblank_lines > 0)
          {
            hsync_mode = 5;
            hsync_count = vblank_lines;
          }
          else
          {
            hsync_mode = 6;
            hsync_count = s_param.mVerticalFrontPorch;
          }
        }
        else
        {
          hsync_count = hc;
          prepare_render_line_0(s_param.mLineDoubler);
        }

        break;
      }
      case 5: // blank lines
      {
        if (--hsync_count == 0)
        {
          hsync_mode = 6;
          hsync_count = s_param.mVerticalFrontPorch;
        }

        break;
      }
      case 6:
      {
        if (--hsync_count == 0)
        {
          // increment vsync_count (BCD)
          if ((vsync_count & 0x0f) == 0x09)
          {
            // range 00 - 59
            if (vsync_count == 0x59)
              vsync_count = 0;
            else
              vsync_count += 7;
          }
          else
          {
            vsync_count++;
          }

          // check if mode sw changed
          uint8_t sw = get_mode_sw_value();

          if (sw_mode != sw)
          {
            // mode changed
            sw_mode = sw;
            hsync_mode = 0;
            // disable interrupt and turn off timer
            cli();
            TCCR1B &= ~((1 << CS12) | (1 << CS11) | (1 << CS10));
            // don't wait hsync, back to mode 0
            continue;
          }
          else
          {
            hsync_mode = 1;
          }
        }

        break;
      }
    }
  
    SMCR = 0x01;  // enable sleep
    sleep_cpu();  // go sleep until next hsync
    SMCR = 0x00;  // disable sleep
  }  
}

#ifndef ARDUINO
int main(void)
{    
  setup();
  loop();
}
#endif
