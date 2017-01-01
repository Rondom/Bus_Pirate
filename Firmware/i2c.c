/*
 * This file is part of the Bus Pirate project
 * (http://code.google.com/p/the-bus-pirate/).
 *
 * Written and maintained by the Bus Pirate project.
 *
 * To the extent possible under law, the project has
 * waived all copyright and related or neighboring rights to Bus Pirate. This
 * work is published from United States.
 *
 * For details see: http://creativecommons.org/publicdomain/zero/1.0/.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 */

#include "i2c.h"

#ifdef BP_ENABLE_I2C_SUPPORT

#include "aux_pin.h"
#include "base.h"
#include "binary_io.h"
#include "bitbang.h"
#include "core.h"

#include "proc_menu.h"

#if defined(BUSPIRATEV4) && !defined(BP_I2C_USE_HW_BUS)
#error "Bus Pirate v4 must be able to use the hardware I2C interface!"
#endif /* BUSPIRATEV4 && !BP_I2C_USE_HW_BUS */

/**
 * Use a software I2C communication implementation
 */
#define I2C_TYPE_SOFTWARE 0

/**
 * Use the built-in hardware I2C communication implementation
 */
#define I2C_TYPE_HARDWARE 1

/**
 * I2C ACK bit value.
 */
#define I2C_ACK_BIT 0

/**
 * I2C NACK bit value.
 */
#define I2C_NACK_BIT 1

typedef struct {

  /**
   * Flag indicating whether a software-only I2C implementation should be
   * used instead of the built-in hardware I2C interface.
   *
   * @see I2C_TYPE_SOFTWARE
   * @see I2C_TYPE_HARDWARE
   */
  uint8_t mode : 1;

  /**
   * Flag indicating whether there is either an ACK/NACK to be received.
   */
  uint8_t acknowledgment_pending : 1;

#ifdef BUSPIRATEV4

  /**
   * Flag indicating whether it is sending commands to the I2C EEPROM or not.
   */
  uint8_t to_eeprom : 1;

#endif /* BUSPIRATEV4 */

} i2c_state_t;

/**
 * Current I2C module state.
 */
static i2c_state_t i2c_state = {0};

#define SCL BP_CLK
#define SCL_TRIS BP_CLK_DIR
#define SDA BP_MOSI
#define SDA_TRIS BP_MOSI_DIR

extern bus_pirate_configuration_t bus_pirate_configuration;
extern mode_configuration_t mode_configuration;
extern command_t last_command;
extern bool command_error;

/**
 * Performs hardware initialisation on the chosen hardware I2C interface.
 */
void hardware_i2c_setup(void);

/**
 * Sends a start condition on the chosen hardware I2C interface.
 */
void hardware_i2c_start(void);

/**
 * Sends a stop condition on the chosen hardware I2C interface.
 */
void hardware_i2c_stop(void);

/**
 * Sends either an ACK or a NACK on the chosen hardware I2C interface.
 *
 * @param[in] ack false for ACK, true for NACK.
 *
 * @see I2C_ACK_BIT
 * @see I2C_NACK_BIT
 */
void hardware_i2c_send_ack(bool ack);

unsigned char hwi2cgetack(void);
void hwi2cwrite(unsigned char c);
unsigned char hwi2cread(void);

#ifdef BP_I2C_USE_HW_BUS

static const uint8_t I2C_BRG_SPEEDS[] = {
    157, /*  100 kHz */
    37,  /*  400 kHz */
    13   /* 1000 kHz */
};

#endif /* BP_I2C_USE_HW_BUS */

void I2C_Sniffer(unsigned char termMode);

uint16_t i2c_read(void) {
  unsigned char value = 0;

  if (i2c_state.acknowledgment_pending) {
    if (i2c_state.mode == I2C_TYPE_SOFTWARE) {
      bitbang_write_bit(LOW);
    } else {
#ifdef BP_I2C_USE_HW_BUS
      hardware_i2c_send_ack(I2C_ACK_BIT);
#endif /* BP_I2C_USE_HW_BUS */
    }
    bpSP;
    MSG_ACK;
    bpSP;
    i2c_state.acknowledgment_pending = false;
  }

  if (i2c_state.mode == I2C_TYPE_SOFTWARE) {
    value = bitbang_read_value();
  } else {
#ifdef BP_I2C_USE_HW_BUS
    value = hwi2cread();
#endif /* BP_I2C_USE_HW_BUS */
  }
  i2c_state.acknowledgment_pending = true;
  return value;
}

unsigned int I2Cwrite(unsigned int c) { // unsigned char c;
  if (i2c_state.acknowledgment_pending) {
    bpSP;
    MSG_ACK;
    bpSP;
    if (i2c_state.mode == I2C_TYPE_SOFTWARE) {
      bitbang_write_bit(LOW);
    } else {
#ifdef BP_I2C_USE_HW_BUS
      hardware_i2c_send_ack(I2C_ACK_BIT);
#endif /* BP_I2C_USE_HW_BUS */
    }
    i2c_state.acknowledgment_pending = false;
  }

  if (i2c_state.mode == I2C_TYPE_SOFTWARE) {
    bitbang_write_value(c);
    c = bitbang_read_bit();
  }
#ifdef BP_I2C_USE_HW_BUS
  else {
    hwi2cwrite(c);
    c = hwi2cgetack();
  }
#endif /* BP_I2C_USE_HW_BUS */
  bpSP;
  if (c == 0) {
    MSG_ACK;
    return 0x300; // bit 9=ack
  }

  MSG_NACK;
  return 0x100; // bit 9=ack
}

void i2c_start(void) {
  /* Reset the bus state if an acknowledgment is pending. */

  if (i2c_state.acknowledgment_pending) {
    MSG_NACK;
    bpBR;

    if (i2c_state.mode == I2C_TYPE_SOFTWARE) {
      bitbang_write_bit(HIGH);
    } else {
#ifdef BP_I2C_USE_HW_BUS
      hardware_i2c_send_ack(I2C_NACK_BIT);
#endif /* BP_I2C_USE_HW_BUS */
    }

    i2c_state.acknowledgment_pending = false;
  }

  /* Send a start signal on the bus. */

  if (i2c_state.mode == I2C_TYPE_SOFTWARE) {
    if (bitbang_i2c_start()) {
      /* There is a short or pull-ups are wrong. */
      BPMSG1019;
      BPMSG1020;
      bpBR;
    }
  } else {
#ifdef BP_I2C_USE_HW_BUS
    hardware_i2c_start();
#endif /* BP_I2C_USE_HW_BUS */
  }

  MSG_I2C_START_BIT;
}

void I2Cstop(void) {
  if (i2c_state.acknowledgment_pending) {
    MSG_NACK;
    bpBR;
    if (i2c_state.mode == I2C_TYPE_SOFTWARE) {
      bitbang_write_bit(HIGH);
    } else {
#ifdef BP_I2C_USE_HW_BUS
      hardware_i2c_send_ack(I2C_NACK_BIT);
#endif /* BP_I2C_USE_HW_BUS */
    }
    i2c_state.acknowledgment_pending = false;
  }

  if (i2c_state.mode == I2C_TYPE_SOFTWARE) {
    bitbang_i2c_stop();
  } else {
#ifdef BP_I2C_USE_HW_BUS
    hardware_i2c_stop();
#endif /* BP_I2C_USE_HW_BUS */
  }

  MSG_I2C_STOP_BIT;
}

void i2c_print_settings(void) {
  BPMSG1068;
#ifdef BP_I2C_USE_HW_BUS
  bp_write_dec_byte(i2c_state.mode);
#else
  bp_write_dec_byte(0);
#endif /* BP_I2C_USE_HW_BUS */
  bpSP;
  bp_write_dec_byte(mode_configuration.speed);
  bp_write_line(" )");
}

void I2Csetup(void) {
  int HW;
  int speed;

  HW = 0; // keep compiler happy if BP_USE_HW is not defined

#ifdef BP_I2C_USE_HW_BUS
  consumewhitechars();
  HW = getint();
#else
  i2c_state.mode = I2C_TYPE_SOFTWARE;
#endif /* BP_I2C_USE_HW_BUS */

  consumewhitechars();
  speed = getint();

#ifdef BP_I2C_USE_HW_BUS
  if ((HW > 0) && (HW <= 2)) {
    i2c_state.mode = HW - 1;
  } else {
    speed = 0;
  }
#endif /* BP_I2C_USE_HW_BUS */

  if ((speed > 0) && (speed <= 4)) {
    mode_configuration.speed = speed - 1;
  } else {
    speed = 0;
  }

  if (speed == 0) {
    command_error = false;

#ifdef BP_I2C_USE_HW_BUS
    BPMSG1064;
    i2c_state.mode = (getnumber(1, 1, 2, 0) - 1);
#else
    i2c_state.mode = I2C_TYPE_SOFTWARE;
#endif /* BP_I2C_USE_HW_BUS */

    if (i2c_state.mode == I2C_TYPE_SOFTWARE) {
      BPMSG1065;
      mode_configuration.speed = (getnumber(1, 1, 4, 0) - 1);
    } else {
#if defined(BUSPIRATEV3) && !defined(BPV3_IS_REV_B4_OR_LATER)
      if (bus_pirate_configuration.device_revision <= PIC_REV_A3) {
        BPMSG1066;
      }
#endif /* BUSPIRATEV3 && !BPV3_IS_REV_B4_OR_LATER */
      BPMSG1067;
      mode_configuration.speed = (getnumber(1, 1, 3, 0) - 1);
    }
  } else {
#if defined(BUSPIRATEV3) && !defined(BPV3_IS_REV_B4_OR_LATER)
    if (bus_pirate_configuration.device_revision <= PIC_REV_A3) {
      BPMSG1066;
    }
#endif /* BUSPIRATEV3 && !BPV3_IS_REV_B4_OR_LATER */
    i2c_print_settings();

    i2c_state.acknowledgment_pending = false;
  }

  mode_configuration.high_impedance = ON;
}

void I2Csetup_exc(void) {
  if (i2c_state.mode == I2C_TYPE_SOFTWARE) {
    SDA_TRIS = INPUT;
    SCL_TRIS = INPUT;
    SCL = LOW;
    SDA = LOW;
    bitbang_setup(2, mode_configuration.speed);
  } else {
#ifdef BP_I2C_USE_HW_BUS
    hardware_i2c_setup();
#endif /* BP_I2C_USE_HW_BUS */
  }
}

void i2c_cleanup(void) {
  /* Clear any pending ACK from previous use. */
  i2c_state.acknowledgment_pending = false;

#ifdef BP_I2C_USE_HW_BUS
  if (i2c_state.mode == I2C_TYPE_HARDWARE) {

#ifdef BUSPIRATEV4

    /* Disable external I2C module. */
    I2C3CONbits.I2CEN = OFF;

    /* Disable EEPROM I2C module. */
    I2C1CONbits.I2CEN = OFF;

#else

    /*
     * PIC24FJ64GA004 Errata - item #26:
     *
     * Bit and byte-based operations may not have the intended affect on the
     * I2CxSTAT register. It is possible for bit and byte operations, performed
     * on the lower byte of I2CxSTAT, to clear the BCL bit (I2CxSTAT<10>). Bit
     * and byte operation performed on the upper byte of I2CxSTAT, or on the
     * BCL bit directly, may not be able to clear the BCL bit.
     */

    /*
     * MSB
     * 0-xxxxxxxxxxxxxx
     * |
     * +-----------------> I2CEN: Disable I2C module.
     */
    I2C1CON &= ~(1 << 15);

#endif /* BUSPIRATEV4 */
  }
#endif /* BP_I2C_USE_HW_BUS */
}

void I2Cmacro(unsigned int c) {
  int i;

  switch (c) {

  case 0:
    BPMSG1069;
    break;

  case 1:
    // setup both lines high first
    bitbang_set_pins_high(MOSI | CLK, 0);
    BPMSG1070;
#ifdef BUSPIRATEV4
    if ((!i2c_state.to_eeprom && ((BP_CLK == LOW) || (BP_MOSI == LOW))) ||
        (i2c_state.to_eeprom && ((BP_EE_SDA == LOW) && (BP_EE_SCL == LOW)))) {
#else
    if ((BP_CLK == LOW) || (BP_MOSI == LOW)) {
#endif /* BUSPIRATEV4 */
      BPMSG1019;
      BPMSG1020;
      bpBR;
      return;
    }

    for (i = 0; i < 0x100; i++) {
      if (i2c_state.mode == I2C_TYPE_SOFTWARE) {
        bitbang_i2c_start();    // send start
        bitbang_write_value(i); // send address
        c = bitbang_read_bit(); // look for ack
      } else {
#ifdef BP_I2C_USE_HW_BUS
        hardware_i2c_start();
        hwi2cwrite(i);
        c = hwi2cgetack();
#endif /* BP_I2C_USE_HW_BUS */
      }

      if (c == I2C_ACK_BIT) {
        bp_write_formatted_integer(i);
        UART1TX('(');
        bp_write_formatted_integer((i >> 1));
        /* If the first bit is set, this is a read address. */
        if ((i & 1) == 0) {
          bp_write_string(" W) ");
        } else {
          if (i2c_state.mode == I2C_TYPE_SOFTWARE) {
            bitbang_read_value();
            bitbang_write_bit(HIGH);
          } else {
#ifdef BP_I2C_USE_HW_BUS
            hwi2cread();
            hardware_i2c_send_ack(I2C_NACK_BIT);
#endif /* BP_I2C_USE_HW_BUS */
          }
          bp_write_string(" R) ");
        }
      }

      if (i2c_state.mode == I2C_TYPE_SOFTWARE) {
        bitbang_i2c_stop();
      } else {
#ifdef BP_I2C_USE_HW_BUS
        hardware_i2c_stop();
#endif /* BP_I2C_USE_HW_BUS */
      }
    }
    bpBR;

    break;

  case 2:
#ifdef BP_I2C_USE_HW_BUS
    if (i2c_state.mode == I2C_TYPE_HARDWARE) {

/* Disable I2C hardware module. */

#if defined(BUSPIRATEV3) && !defined(BPV3_IS_REV_B4_OR_LATER)

      /*
       * PIC24FJ64GA004 Errata - item #26:
       *
       * Bit and byte-based operations may not have the intended affect on the
       * I2CxSTAT register. It is possible for bit and byte operations,
       * performed
       * on the lower byte of I2CxSTAT, to clear the BCL bit (I2CxSTAT<10>). Bit
       * and byte operation performed on the upper byte of I2CxSTAT, or on the
       * BCL bit directly, may not be able to clear the BCL bit.
       */

      /*
       * MSB
       * 0-xxxxxxxxxxxxxx
       * |
       * +-----------------> I2CEN: Disable I2C module.
       */
      I2C1CON &= ~(1 << 15);

#else

      I2C1CONbits.I2CEN = OFF;

#endif /* BUSPIRATEV3 && !BPV3_IS_REV_B4_OR_LATER */
    }

#endif /* BP_I2C_USE_HW_BUS */

    BPMSG1071;
    BPMSG1250;
    I2C_Sniffer(1); // set for terminal output

#ifdef BP_I2C_USE_HW_BUS
    if (i2c_state.mode == I2C_TYPE_HARDWARE) {
      hardware_i2c_setup(); // setup hardware I2C again
    }
#endif /* BP_I2C_USE_HW_BUS */

    break;

#if defined(BUSPIRATEV4)

  case 3: {
    MSG_USING_ONBOARD_I2C_EEPROM;
    i2c_state.to_eeprom = true;

    /* 7-bits slave address. */
    I2C1CONbits.A10M = OFF;

    /* Enable clock stretching. */
    I2C1CONbits.SCLREL = OFF;

    /* General call address. */
    I2C1ADD = 0;

    /* Do not mask address bits. */
    I2C1MSK = 0;

    /* Disable SMBus. */
    I2C1CONbits.SMEN = OFF;

    /* Set the I2C baud rate generator speed. */
    I2C1BRG = I2C_BRG_SPEEDS[mode_configuration.speed];

    /* Enable the internal I2C module. */
    I2C1CONbits.I2CEN = ON;

    /* Disable the external I2C module. */
    I2C3CONbits.I2CEN = OFF;
    break;
  }

  case 4:
    if (i2c_state.to_eeprom) {
      MSG_ONBOARD_I2C_EEPROM_WRITE_PROTECT_DISABLED;
      BP_EE_WP = LOW;
    }

    break;

#endif /* BUSPIRATEV4 */

  default:
    BPMSG1016;
  }
}

void I2Cpins(void) {
#if defined(BUSPIRATEV4)
  BPMSG1261;
#else
  BPMSG1231;
#endif /* BUSPIRATEV4 */
}

#if defined(BP_I2C_USE_HW_BUS)

void hardware_i2c_start(void) {
#if defined(BUSPIRATEV4)
  if (!i2c_state.to_eeprom) {
    /* Start condition on the external v4 I2C bus. */
    I2C3CONbits.SEN = ON;
    while (I2C3CONbits.SEN == ON) {
    }

    return;
  }
#endif /* BUSPIRATEV4 */

/*
 * Start condition on the external v3 I2C bus or on the v4 EEPROM I2C bus.
 */

#if defined(BUSPIRATEV3) && !defined(BPV3_IS_REV_B4_OR_LATER)

  /*
   * PIC24FJ64GA004 Errata - item #26:
   *
   * Bit and byte-based operations may not have the intended affect on the
   * I2CxSTAT register. It is possible for bit and byte operations, performed
   * on the lower byte of I2CxSTAT, to clear the BCL bit (I2CxSTAT<10>). Bit
   * and byte operation performed on the upper byte of I2CxSTAT, or on the
   * BCL bit directly, may not be able to clear the BCL bit.
   */

  /*
   * MSB
   * x-xxxxxxxxxxxxx1
   *                |
   *                +--> SEN: Initiates Start condition on SDAx and SCLx pins.
   */

  I2C1CON |= (1 << 0);

#else

  I2C1CONbits.SEN = ON;

#endif /* BUSPIRATEV3 && !BPV3_IS_REV_B4_OR_LATER */

  while (I2C1CONbits.SEN == ON) {
  }
}

void hardware_i2c_stop(void) {

#if defined(BUSPIRATEV4)
  if (!i2c_state.to_eeprom) {
    I2C3CONbits.PEN = ON;
    while (I2C3CONbits.PEN == ON) {
    }

    return;
  }
#endif /* BUSPIRATEV4 */

#if defined(BUSPIRATEV3) && !defined(BPV3_IS_REV_B4_OR_LATER)

  /*
   * PIC24FJ64GA004 Errata - item #26:
   *
   * Bit and byte-based operations may not have the intended affect on the
   * I2CxSTAT register. It is possible for bit and byte operations, performed
   * on the lower byte of I2CxSTAT, to clear the BCL bit (I2CxSTAT<10>). Bit
   * and byte operation performed on the upper byte of I2CxSTAT, or on the
   * BCL bit directly, may not be able to clear the BCL bit.
   */

  /*
   * MSB
   * x-xxxxxxxxxxx1xx
   *              |
   *              +----> PEN: Stop condition is not in progress.
   */

  I2C1CON |= (1 << 2);

#else

  I2C1CONbits.PEN = ON;

#endif /* BUSPIRATEV3 && !BPV3_IS_REV_B4_OR_LATER */

  while (I2C1CONbits.PEN == ON) {
  }
}

unsigned char hwi2cgetack(void) {

#if defined(BUSPIRATEV4)
  if (!i2c_state.to_eeprom) {
    return I2C3STATbits.ACKSTAT;
  }
#endif /* BUSPIRATEV4 */

  return I2C1STATbits.ACKSTAT;
}

void hardware_i2c_send_ack(bool ack) {
#if defined(BUSPIRATEV4)
  if (!i2c_state.to_eeprom) {
    I2C3CONbits.ACKDT = ack;
    I2C3CONbits.ACKEN = ON;
    while (I2C3CONbits.ACKEN == ON) {
    }

    return;
  }
#endif /* BUSPIRATEV4 */

#if defined(BUSPIRATEV3) && !defined(BPV3_IS_REV_B4_OR_LATER)

  /*
   * PIC24FJ64GA004 Errata - item #26:
   *
   * Bit and byte-based operations may not have the intended affect on the
   * I2CxSTAT register. It is possible for bit and byte operations, performed
   * on the lower byte of I2CxSTAT, to clear the BCL bit (I2CxSTAT<10>). Bit
   * and byte operation performed on the upper byte of I2CxSTAT, or on the
   * BCL bit directly, may not be able to clear the BCL bit.
   */

  /*
   * MSB
   * x-xxxxxxxx?1xxxx
   *           ||
   *           |+------> ACKEN: Initiates Acknowledge sequence.
   *           +-------> ACKDT: ACK/NACK flag.
   */

  I2C1CON |= (1 << 4) | (ack ? (1 << 5) : 0);

#else

  I2C1CONbits.ACKDT = ack;
  I2C1CONbits.ACKEN = ON;

#endif /* BUSPIRATEV3 && !BPV3_IS_REV_B4_OR_LATER */

  while (I2C1CONbits.ACKEN == ON) {
  }
}

void hwi2cwrite(uint8_t value) {
#if defined(BUSPIRATEV4)
  if (!i2c_state.to_eeprom) {
    I2C3TRN = value;
    while (I2C3STATbits.TRSTAT == ON) {
    }

    return;
  }
#endif /* BUSPIRATEV4 */

  I2C1TRN = value;
  while (I2C1STATbits.TRSTAT == ON) {
  }
}

unsigned char hwi2cread(void) {
#if defined(BUSPIRATEV4)
  if (!i2c_state.to_eeprom) {
    I2C3CONbits.RCEN = ON;
    while (I2C3CONbits.RCEN == ON) {
    }

    return I2C3RCV;
  }
#endif /* BUSPIRATEV4 */

#if defined(BUSPIRATEV3) && !defined(BPV3_IS_REV_B4_OR_LATER)

  /*
   * PIC24FJ64GA004 Errata - item #26:
   *
   * Bit and byte-based operations may not have the intended affect on the
   * I2CxSTAT register. It is possible for bit and byte operations, performed
   * on the lower byte of I2CxSTAT, to clear the BCL bit (I2CxSTAT<10>). Bit
   * and byte operation performed on the upper byte of I2CxSTAT, or on the
   * BCL bit directly, may not be able to clear the BCL bit.
   */

  /*
   * MSB
   * x-xxxxxxxxxx1xxx
   *             |
   *             +-----> RCEN: Enables Receive mode for I2C.
   */

  I2C1CON |= (1 << 3);

#else

  I2C1CONbits.RCEN = ON;

#endif /* BUSPIRATEV3 && !BPV3_IS_REV_B4_OR_LATER */

  while (I2C1CONbits.RCEN == ON) {
  }

  return I2C1RCV;
}

void hardware_i2c_setup(void) {
#if defined(BUSPIRATEV4)

  /* 7-bits slave address. */
  I2C3CONbits.A10M = OFF;

  /* Enable clock stretching. */
  I2C3CONbits.SCLREL = OFF;

  /* General call address. */
  I2C3ADD = 0;

  /* Do not mask address bits. */
  I2C3MSK = 0;

  /* Disable SMBus. */
  I2C3CONbits.SMEN = OFF;

  /* Set the I2C baud rate generator speed. */
  I2C3BRG = I2C_BRG_SPEEDS[mode_configuration.speed];

  /* Enable the I2C module. */
  I2C3CONbits.I2CEN = ON;

#else

  /* General call address. */
  I2C1ADD = 0;

  /* Do not mask address bits. */
  I2C1MSK = 0;

  /* Set the I2C baud rate generator speed. */
  I2C1BRG = I2C_BRG_SPEEDS[mode_configuration.speed];

#if defined(BPV3_IS_REV_B4_OR_LATER)

  /*
   * PIC24FJ64GA004 Errata - item #26:
   *
   * Bit and byte-based operations may not have the intended affect on the
   * I2CxSTAT register. It is possible for bit and byte operations, performed
   * on the lower byte of I2CxSTAT, to clear the BCL bit (I2CxSTAT<10>). Bit
   * and byte operation performed on the upper byte of I2CxSTAT, or on the
   * BCL bit directly, may not be able to clear the BCL bit.
   */

  /*
   * MSB
   * x-x0x0x0xxxxxxxx
   *    | | |
   *    | | +----------> SMEN:   Disables the SMBus input thresholds.
   *    | +------------> A10M:   I2CxADD is a 7-bit slave address.
   *    +--------------> SCLREL: Holds SCLx clock low (clock stretch).
   */
  I2C1CON &= ~((1 << 10) | (1 << 12) | (1 << 8));

  /*
   * PIC24FJ64GA004 Errata - item #10:
   *
   * When using I2C1, the SDA1 line state may not be detected properly unless
   * it is first held low for 150ns after enabling the I2C module.
   *
   * In Master mode, this error may cause a bus collision to occur instead of
   * a Start bit transmission. Transmissions after the SDA1 pin that have
   * been held low will occur correctly.
   *
   * In Slave mode, the device may not Acknowledge the first packet sent after
   * enabling the I2C module. In this case, it will return a NACK instead of
   * an ACK. The device will correctly respond to packets after detecting a
   * low level on the line for 150ns.
   */

  /*
   * From the errata workaround section:
   *
   * If no external devices or additional I/O pins are available, it is
   * sometimes possible to perform the work around internally, using the
   * following steps:
   *
   * * With the module in Master mode, configure the RB9 pin as an output.
   * * Clear the LATB9 bit (for the default I2C1 assignment) or LATB5 (for the
   * alternate I2C1 assignment) to drive the pin low.
   * * Enable I2C1 by setting the I2CENbit (I2C1CON<15>).
   */

  BP_MOSI_DIR = OUTPUT;
  bp_delay_us(200);
  LATBbits.LATB9 = OFF;
  bp_delay_us(200);

  /*
   * MSB
   * 1-xxxxxxxxxxxxxx
   * |
   * +-----------------> I2CEN: Enable I2C module.
   */
  I2C1CON |= (1 << 15);

#else

  /* 7-bits slave address. */
  I2C1CONbits.A10M = OFF;

  /* Enable clock stretching. */
  I2C1CONbits.SCLREL = OFF;

  /* Disable SMBus. */
  I2C1CONbits.SMEN = OFF;

  /* Enable the I2C module. */
  I2C1CONbits.I2CEN = ON;

#endif /* BPV3_IS_REV_B4_OR_LATER */

#endif /* BUSPIRATEV4 */
}

#endif /* BP_I2C_USE_HW_BUS */

#define ESCAPE_CHAR '\\'

void I2C_Sniffer(unsigned char termMode) {
  unsigned char SDANew, SDAOld;
  unsigned char SCLNew, SCLOld;

  unsigned char DataState = 0;
  unsigned char DataBits = 0;
  unsigned char dat = 0;

  // unsigned char *BitBuffer=bpConfig.terminalInput;
  // unsigned short BufferPos=0;
  // unsigned short AckPos=0;
  // unsigned short DataPos=0;

  // setup ring buffer pointers
  UARTbufSetup();

  SDA_TRIS = INPUT;
  SCL_TRIS = INPUT;

  SCL = LOW;
  SDA = LOW;

  BP_MOSI_CN = ON; // enable change notice on SCL and SDA
  BP_CLK_CN = ON;

  IFS1bits.CNIF = OFF; // clear the change interrupt flag

  SDAOld = SDANew = SDA;
  SCLOld = SDANew = SCL;

  for (;;) {
    if (!IFS1bits.CNIF) { // check change notice interrupt
      // user IO service
      UARTbufService();
      if (UART1RXRdy()) {
        dat = UART1RX();
        break;
      }
      continue;
    }

    IFS1bits.CNIF = OFF; // clear interrupt flag

    SDANew = SDA; // store current state right away
    SCLNew = SCL;

    if (DataState && !SCLOld && SCLNew) // Sample When SCL Goes Low To High
    {
      if (DataBits < 8) // we're still collecting data bits
      {
        dat = dat << 1;
        if (SDANew) {
          dat |= 1;
        }

        DataBits++;
      } else {
        // put the data byte in the terminal or binary output
        if (termMode) { // output for the terminal
          bp_write_hex_byte_to_ringbuffer(dat);
        } else {                // output for binary mode
          UARTbuf(ESCAPE_CHAR); // escape character
          UARTbuf(dat);         // write byte value
        }

        if (SDANew) // SDA High Means NACK
        {
          UARTbuf('-');
        } else // SDA Low Means ACK
        {
          UARTbuf('+'); // write ACK status
        }

        DataBits = 0; // Ready For Next Data Byte
      }
    } else if (SCLOld && SCLNew) // SCL High, Must Be Data Transition
    {
      if (SDAOld && !SDANew) // Start Condition (High To Low)
      {
        DataState = 1; // Allow Data Collection
        DataBits = 0;

        UARTbuf('['); // say start, use bus pirate syntax to display data

      } else if (!SDAOld && SDANew) // Stop Condition (Low To High)
      {
        DataState = 0; // Don't Allow Data Collection
        DataBits = 0;

        UARTbuf(']'); // say stop
      }
    }

    SDAOld = SDANew; // Save Last States
    SCLOld = SCLNew;
  }

  BP_MOSI_CN = OFF;
  BP_CLK_CN = OFF;

  if (termMode) {
    bpBR;
  }
}

/*
rawI2C mode:
# 00000000//reset to BBIO
# 00000001 � mode version string (I2C1)
# 00000010 � I2C start bit
# 00000011 � I2C stop bit
# 00000100 - I2C read byte
# 00000110 - ACK bit
# 00000111 - NACK bit
# 0001xxxx � Bulk transfer, send 1-16 bytes (0=1byte!)
# (0110)000x - Set I2C speed, 3 = 400khz 2=100khz 1=50khz 0=5khz
# (0111)000x - Read speed, (planned)
# (0100)wxyz � Configure peripherals w=power, x=pullups, y=AUX, z=CS (was 0110)
# (0101)wxyz � read peripherals (planned, not implemented)
 */

void binI2C(void) {
  static unsigned char inByte, rawCommand, i;
  unsigned int j, fw, fr;

  SDA_TRIS = INPUT;
  SCL_TRIS = INPUT;
  SCL = LOW;
  SDA = LOW;

  // set CS pin direction to output on setup
  BP_CS_DIR = OUTPUT;

  mode_configuration.high_impedance = ON;
  mode_configuration.lsbEN = OFF;
  bitbang_setup(2, BITBANG_SPEED_MAXIMUM);
  MSG_I2C_MODE_IDENTIFIER;

  for (;;) {
    inByte = UART1RX();
    rawCommand = (inByte >> 4); // get command bits in seperate variable

    switch (rawCommand) {
    case 0: // reset/setup/config commands
      switch (inByte) {
      case 0:   // 0, reset exit
        return; // exit

      case 1: // 1 - id reply string
        MSG_I2C_MODE_IDENTIFIER;
        break;

      case 2: // I2C start bit
        bitbang_i2c_start();
        REPORT_IO_SUCCESS();
        break;

      case 3: // I2C stop bit
        bitbang_i2c_stop();
        REPORT_IO_SUCCESS();
        break;

      case 4: // I2C read byte
        UART1TX(bitbang_read_value());
        break;

      case 6: // I2C send ACK
        bitbang_write_bit(LOW);
        REPORT_IO_SUCCESS();
        break;

      case 7: // I2C send NACK
        bitbang_write_bit(HIGH);
        REPORT_IO_SUCCESS();
        break;

      case 8: // write-then-read
        // get the number of commands that will follow
        fw = UART1RX();
        fw = fw << 8;
        fw |= UART1RX();

        // get the number of reads to do
        fr = UART1RX();
        fr = fr << 8;
        fr |= UART1RX();

        // check length and report error
        if (fw > BP_TERMINAL_BUFFER_SIZE || fr > BP_TERMINAL_BUFFER_SIZE) {
        I2C_write_read_error: // use this for the read error too
          REPORT_IO_FAILURE();
          break;
        }

        // get bytes
        for (j = 0; j < fw; j++) {
          bus_pirate_configuration.terminal_input[j] = UART1RX();
        }

        // start
        bitbang_i2c_start();

        for (j = 0; j < fw; j++) {
          // get ACK
          // if no ack, goto error
          bitbang_write_value(
              bus_pirate_configuration.terminal_input[j]); // send byte
          if (bitbang_read_bit() == 1)
            goto I2C_write_read_error;
        }

        fw = fr - 1;
        for (j = 0; j < fr; j++) { // read bulk bytes from SPI
          // send ack
          // i flast byte, send NACK
          bus_pirate_configuration.terminal_input[j] = bitbang_read_value();

          if (j < fw) {
            bitbang_write_bit(LOW);
          } else {
            bitbang_write_bit(HIGH);
          }
        }
        // I2C stop
        bitbang_i2c_stop();

        REPORT_IO_SUCCESS();

        for (j = 0; j < fr; j++) { // send the read buffer contents over serial
          UART1TX(bus_pirate_configuration.terminal_input[j]);
        }
        break; // 00001001 xxxxxxxx

      case 9: // extended AUX command
        // confirm that the command is known
        REPORT_IO_SUCCESS();
        // inByte - used as extended commmand
        // fr - used as result
        // wait for subcommand byte
        while (U1STAbits.URXDA == OFF) {
        }

        inByte = U1RXREG; // get byte
        // 0x00 - AUX/CS low
        // 0x01 - AUX/CS high
        // 0x02 - AUX/CS HiZ
        // 0x03 - AUX read
        // 0x10 - use AUX
        // 0x20 - use CS
        fr = 1;
        switch (inByte) {

        case 0x00:
          bp_aux_pin_set_low();
          break;

        case 0x01:
          bp_aux_pin_set_high();
          break;

        case 0x02:
          bp_aux_pin_set_high_impedance();
          break;

        case 0x03:
          fr = bp_aux_pin_read();
          break;

        case 0x10:
          mode_configuration.alternate_aux = 0;
          break;

        case 0x20:
          mode_configuration.alternate_aux = 1;
          break;

        default:
          fw = 0;
          break;
        }

        UART1TX(fr); // result
        break;

      case 0b1111:
        I2C_Sniffer(0); // set for raw output
        REPORT_IO_SUCCESS();
        break;

      default:
        REPORT_IO_FAILURE();
        break;
      }
      break;

    case 0b0001:            // get x+1 bytes
      inByte &= 0b00001111; // clear command portion
      inByte++;             // increment by 1, 0=1byte
      REPORT_IO_SUCCESS();

      for (i = 0; i < inByte; i++) {
        bitbang_write_value(UART1RX()); // JTR usb port //send byte
        UART1TX(bitbang_read_bit());    // return ACK0 or NACK1
      }

      break;

    case 0b0110:                // set speed
      inByte &= 0b00000011;     // clear command portion
      bitbang_setup(2, inByte); // set I2C speed
      REPORT_IO_SUCCESS();
      break;

    case 0b0100: // configure peripherals w=power, x=pullups, y=AUX, z=CS
      bp_binary_io_peripherals_set(inByte);
      REPORT_IO_SUCCESS();
      break;

#ifdef BUSPIRATEV4
    case 0b0101:
      UART1TX(bp_binary_io_pullup_control(inByte));
      break;
#endif /* BUSPIRATEV4 */

    default:
      REPORT_IO_FAILURE();
      break;
    }
  }
}

#endif /* BP_ENABLE_I2C_SUPPORT */
