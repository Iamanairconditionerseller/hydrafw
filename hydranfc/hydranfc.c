/*
 * HydraBus/HydraNFC
 *
 * Copyright (C) 2012-2014 Benjamin VERNOUX
 * Copyright (C) 2014 Bert Vermeulen <bert@biot.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ch.h"
#include "common.h"
#include "tokenline.h"
#include "hydrabus.h"
#include "hydrabus_mode.h"
#include "hydranfc.h"
#include "trf797x.h"
#include <string.h>

static void extcb1(EXTDriver *extp, expchannel_t channel);
static int exec(t_hydra_console *con, t_tokenline_parsed *p, int token_pos);
static void show(t_hydra_console *con, t_tokenline_parsed *p);

static thread_t *key_sniff_thread;
static volatile int irq_count;
volatile int irq;
volatile int irq_end_rx;

/* Configure TRF7970A IRQ on GPIO A1 Rising Edge */
static const EXTConfig extcfg = {
	{
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_RISING_EDGE | EXT_CH_MODE_AUTOSTART | EXT_MODE_GPIOA, extcb1}, /* EXTI1 */
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL}
	}
};

static struct {
	int reg;
	char *name;
} registers[] = {
	{ CHIP_STATE_CONTROL, "Chip Status" },
	{ ISO_CONTROL, "ISO Control" },
	{ ISO_14443B_OPTIONS, "ISO 14443B Options" },
	{ ISO_14443A_OPTIONS, "ISO 14443A Options" },
	{ TX_TIMER_EPC_HIGH, "TX Timer HighByte" },
	{ TX_TIMER_EPC_LOW, "TX Timer LowByte" },
	{ TX_PULSE_LENGTH_CONTROL, "TX Pulse Length" },
	{ RX_NO_RESPONSE_WAIT_TIME, "RX No Response Wait Time" },
	{ RX_WAIT_TIME, "RX Wait Time" },
	{ MODULATOR_CONTROL, "Modulator SYS_CLK" },
	{ RX_SPECIAL_SETTINGS, "RX SpecialSettings" },
	{ REGULATOR_CONTROL, "Regulator IO" },
	{ IRQ_STATUS, "IRQ Status" },
	{ IRQ_MASK, "IRQ Mask+Collision Position1" },
	{ COLLISION_POSITION, "Collision Position2" },
	{ RSSI_LEVELS, "RSSI+Oscillator Status" },
	{ SPECIAL_FUNCTION, "Special Functions1" },
	{ RAM_START_ADDRESS, "Special Functions2" },
	{ RAM_START_ADDRESS + 1, "RAM ADDR0" },
	{ RAM_START_ADDRESS + 2, "RAM ADDR1" },
	{ RAM_START_ADDRESS + 3, "Adjust FIFO IRQ Lev" },
	{ RAM_START_ADDRESS + 4, "RAM ADDR3" },
	{ NFC_LOW_DETECTION, "NFC Low Field Level" },
	{ NFC_TARGET_LEVEL, "NFC Target Detection Level" },
	{ NFC_TARGET_PROTOCOL, "NFC Target Protocol" },
	{ TEST_SETTINGS_1, "Test Settings1" },
	{ TEST_SETTINGS_2, "Test Settings2" },
	{ FIFO_CONTROL, "FIFO Status" },
	{ TX_LENGTH_BYTE_1, "TX Length Byte1" },
	{ TX_LENGTH_BYTE_2, "TX Length Byte2" },
};

static const SPIConfig spi2cfg = {
	NULL, /* spicb, */
	/* HW dependent part.*/
	GPIOC, 1, (SPI_CR1_CPHA | SPI_CR1_BR_2)
};

enum {
	NFC_MODE_NONE,
	NFC_MODE_MIFARE,
	NFC_MODE_VICINITY,
};

/* Triggered when the Ext IRQ is pressed or released. */
static void extcb1(EXTDriver *extp, expchannel_t channel)
{
	(void)extp;
	(void)channel;

	irq_count++;
	irq = 1;
}

static bool hydranfc_test_shield(void)
{
	int init_ms;
	int err;
	static uint8_t data_buf[4];

	err = 0;

	/* Software Init TRF7970A */
	init_ms = Trf797xInitialSettings();
	if (init_ms == TRF7970A_INIT_TIMEOUT)
		return FALSE;

	Trf797xReset();

	data_buf[0] = CHIP_STATE_CONTROL;
	Trf797xReadSingle(data_buf, 1);
	if (data_buf[0] != 0x01)
		err++;

	return err == 0;
}

static bool configure_gpio(t_hydra_console *con)
{
	/* PA7 as Input connected to TRF7970A MOD Pin */
	// palSetPadMode(GPIOA, 7, PAL_MODE_INPUT);

	/* Configure NFC/TRF7970A in SPI mode with Chip Select */
	/* TRF7970A IO0 (To set to "0" for SPI) */
	palClearPad(GPIOA, 3);
	palSetPadMode(GPIOA, 3, PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_MID1);

	/* TRF7970A IO1 (To set to "1" for SPI) */
	palSetPad(GPIOA, 2);
	palSetPadMode(GPIOA, 2, PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_MID1);

	/* TRF7970A IO2 (To set to "1" for SPI) */
	palSetPad(GPIOC, 0);
	palSetPadMode(GPIOC, 0, PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_MID1);

	/*
	 * Initializes the SPI driver 1. The SPI1 signals are routed as follows:
	 * Shall be configured as SPI Slave for TRF7970A NFC data sampling on MOD pin.
	 * NSS. (Not used use Software).
	 * PA5 - SCK.(AF5)  => Connected to TRF7970A SYS_CLK pin
	 * PA6 - MISO.(AF5) (Not Used)
	 * PA7 - MOSI.(AF5) => Connected to TRF7970A MOD pin
	 */
	/* spiStart() is done in sniffer see sniffer.c */
	/* SCK.     */
	palSetPadMode(GPIOA, 5, PAL_MODE_ALTERNATE(5) | PAL_STM32_OSPEED_MID1);
	/* MISO. Not used/Not connected */
	palSetPadMode(GPIOA, 6, PAL_MODE_ALTERNATE(5) | PAL_STM32_OSPEED_MID1);
	/* MOSI. connected to TRF7970A MOD Pin */
	palSetPadMode(GPIOA, 7, PAL_MODE_ALTERNATE(5) | PAL_STM32_OSPEED_MID1);

	/*
	 * Initializes the SPI driver 2. The SPI2 signals are routed as follow:
	 * PC1 - NSS.
	 * PB10 - SCK.
	 * PC2 - MISO.
	 * PC3 - MOSI.
	 * Used for communication with TRF7970A in SPI mode with NSS.
	 */
	spiStart(&SPID2, &spi2cfg);
	/* NSS - ChipSelect. */
	palSetPad(GPIOC, 1);
	palSetPadMode(GPIOC, 1, PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_MID1);
	/* SCK.     */
	palSetPadMode(GPIOB, 10, PAL_MODE_ALTERNATE(5) | PAL_STM32_OSPEED_MID1);
	/* MISO.    */
	palSetPadMode(GPIOC, 2, PAL_MODE_ALTERNATE(5) | PAL_STM32_OSPEED_MID1);
	/* MOSI.    */
	palSetPadMode(GPIOC, 3, PAL_MODE_ALTERNATE(5) | PAL_STM32_OSPEED_MID1);

	/* Enable TRF7970A EN=1 (EN2 is already equal to GND) */
	palClearPad(GPIOB, 11);
	palSetPadMode(GPIOB, 11, PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_MID1);
	McuDelayMillisecond(2);

	palSetPad(GPIOB, 11);
	/* After setting EN=1 wait at least 21ms */
	McuDelayMillisecond(21);

	if (!hydranfc_test_shield()) {
		/* TODO: clean up */
		cprintf(con, "HydraNFC not found.\r\n");
		return FALSE;
	}

	/* Configure K1/2/3/4 Buttons as Input */
	palSetPadMode(GPIOB, 7, PAL_MODE_INPUT);
	palSetPadMode(GPIOB, 6, PAL_MODE_INPUT);
	palSetPadMode(GPIOB, 8, PAL_MODE_INPUT);
	palSetPadMode(GPIOB, 9, PAL_MODE_INPUT);

	/* Configure D2/3/4/5 LEDs as Output */
	D2_OFF;
	D3_OFF;
	D4_OFF;
	D5_OFF;
	palSetPadMode(GPIOB, 0, PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_MID1);
	palSetPadMode(GPIOB, 3, PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_MID1);
	palSetPadMode(GPIOB, 4, PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_MID1);
	palSetPadMode(GPIOB, 5, PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_MID1);

	/* Activates the EXT driver 1. */
	extStart(&EXTD1, &extcfg);

	return TRUE;
}

THD_WORKING_AREA(key_sniff_mem, 2048);
THD_FUNCTION(key_sniff, arg)
{
	int i;

	(void)arg;

	chRegSetThreadName("HydraNFC key-sniff");
	while (TRUE) {
		if (K1_BUTTON)
			D4_ON;
		else
			D4_OFF;

		if (K2_BUTTON)
			D3_ON;
		else
			D3_OFF;

		if (K3_BUTTON) {
			/* Blink Fast */
			for(i = 0; i < 4; i++) {
				D2_ON;
				chThdSleepMilliseconds(25);
				D2_OFF;
				chThdSleepMilliseconds(25);
			}
			cmd_nfc_sniff_14443A(NULL);
		}

		if (K4_BUTTON)
			D5_ON;
		else
			D5_OFF;

		if (chThdShouldTerminateX())
			return 0;

		chThdSleepMilliseconds(100);
	}

	return 0;
}

static int init(t_hydra_console *con, t_tokenline_parsed *p)
{
	mode_config_proto_t* proto = &con->mode->proto;
	int tokens_used;

	proto->dev_mode = NFC_MODE_NONE;

	/* Process cmdline arguments, skipping "nfc". */
	tokens_used = 1 + exec(con, p, 1);

	if (!configure_gpio(con))
		return 0;

	key_sniff_thread = chThdCreateStatic(key_sniff_mem,
			sizeof(key_sniff_mem), NORMALPRIO, key_sniff, NULL);

	return tokens_used;
}

static void scan_mifare(t_hydra_console *con)
{
	uint8_t fifo_size;
	uint8_t data_buf[MIFARE_DATA_MAX];
	uint8_t uid_buf[MIFARE_UID_MAX];
	uint8_t bcc, i;

	/* End Test delay */
	irq_count = 0;

	/* Test ISO14443-A/Mifare read UID */
	Trf797xInitialSettings();
	Trf797xReset();

	/*
	 * Write Modulator and SYS_CLK Control Register (0x09) (13.56Mhz SYS_CLK
	 * and default Clock 13.56Mhz))
	 */
	data_buf[0] = MODULATOR_CONTROL;
	data_buf[1] = 0x31;
	Trf797xWriteSingle(data_buf, 2);

	/*
	 * Configure Mode ISO Control Register (0x01) to 0x88 (ISO14443A RX bit
	 * rate, 106 kbps) and no RX CRC (CRC is not present in the response))
	 */
	data_buf[0] = ISO_CONTROL;
	data_buf[1] = 0x88;
	Trf797xWriteSingle(data_buf, 2);

	data_buf[0] = ISO_CONTROL;
	Trf797xReadSingle(data_buf, 1);
	if (data_buf[0] != 0x88)
		cprintf(con, "Error ISO Control Register read=0x%02lX (should be 0x88)\r\n",
				(uint32_t)data_buf[0]);

	/* Configure Test Settings 1 to BIT6/0x40 => MOD Pin becomes receiver subcarrier output (Digital Output for RX/TX) */
	/*
	    data_buf[0] = TEST_SETTINGS_1;
	    data_buf[1] = BIT6;
	    Trf797xWriteSingle(data_buf, 2);

	    data_buf[0] = TEST_SETTINGS_1;
	    Trf797xReadSingle(data_buf, 1);
	    if (data_buf[0] != 0x40)
	    {
	      cprintf(con, "Error Test Settings Register(0x1A) read=0x%02lX (shall be 0x40)\r\n", (uint32_t)data_buf[0]);
	      err++;
	    }
	*/

	/* Turn RF ON (Chip Status Control Register (0x00)) */
	Trf797xTurnRfOn();

	/* Send REQA (7 bits) and receive ATQA (2 bytes) */
	data_buf[0] = 0x26; /* REQA (7bits) */
	fifo_size = Trf797x_transceive_bits(data_buf[0], 7, data_buf, MIFARE_DATA_MAX,
					    10, /* 10ms TX/RX Timeout */
					    0); /* TX CRC disabled */
	/* Re-send REQA */
	if (fifo_size == 0) {
		/* Send REQA (7 bits) and receive ATQA (2 bytes) */
		data_buf[0] = 0x26; /* REQA (7 bits) */
		fifo_size = Trf797x_transceive_bits(data_buf[0], 7, data_buf, MIFARE_DATA_MAX,
						    10, /* 10ms TX/RX Timeout */
						    0); /* TX CRC disabled */
	}
	if (fifo_size > 0) {
		cprintf(con, "ATQA:");
		for (i = 0; i < fifo_size; i++)
			cprintf(con, " %02X", (uint32_t)data_buf[i]);
		cprintf(con, "\r\n");

		/* Send AntiColl (2 bytes) and receive UID+BCC (5 bytes) */
		data_buf[0] = 0x93;
		data_buf[1] = 0x20;
		fifo_size = Trf797x_transceive_bytes(data_buf, 2, uid_buf, MIFARE_UID_MAX,
				10, /* 10ms TX/RX Timeout */
				0); /* TX CRC disabled */
		if (fifo_size > 0) {
			cprintf(con, "UID: ");
			bcc = 0;
			for (i = 0; i < fifo_size - 1; i++) {
				cprintf(con, " %02lX", (uint32_t)uid_buf[i]);
				bcc ^= uid_buf[i];
			}
			cprintf(con, " (BCC %02lX %s)\r\n", (uint32_t)uid_buf[i],
				bcc == uid_buf[i] ? "ok" : "NOT OK");

			data_buf[0] = RSSI_LEVELS;
			Trf797xReadSingle(data_buf, 1);
			if (data_buf[0] < 0x40)
				cprintf(con, "RSSI error: 0x%02lX (should be > 0x40)\r\n", (uint32_t)data_buf[0]);

			/*
			 * Select RX with CRC_A
			 * Configure Mode ISO Control Register (0x01) to 0x08
			 * (ISO14443A RX bit rate, 106 kbps) and RX CRC (CRC
			 * is present in the response)
			 */
			data_buf[0] = ISO_CONTROL;
			data_buf[1] = 0x08;
			Trf797xWriteSingle(data_buf, 2);

			/* Send SelUID (6 bytes) and receive ATQA (2 bytes) */
			data_buf[0] = 0x93;
			data_buf[1] = 0x70;
			for (i = 0; i < MIFARE_UID_MAX; i++) {
				data_buf[2 + i] = uid_buf[i];
			}
			fifo_size = Trf797x_transceive_bytes(data_buf, (2 + MIFARE_UID_MAX),  data_buf, MIFARE_DATA_MAX,
				10, /* 10ms TX/RX Timeout */
				1); /* TX CRC enabled */
			if (fifo_size > 0) {
				cprintf(con, "SAK: ");
				for (i = 0; i < fifo_size; i++)
					cprintf(con, " %02lX", (uint32_t)data_buf[i]);
				cprintf(con, "\r\n");

				/* Send Halt(2Bytes+CRC) */
				data_buf[0] = 0x50;
				data_buf[1] = 0x00;
				fifo_size = Trf797x_transceive_bytes(data_buf,
						(2 + MIFARE_UID_MAX), data_buf, MIFARE_DATA_MAX,
						5, /* 5ms TX/RX Timeout => shall not receive answer */
						1); /* TX CRC enabled */
				if (fifo_size > 0) {
					cprintf(con, "HALT:");
					for (i = 0; i < fifo_size; i++)
						cprintf(con, " %02lX", (uint32_t)data_buf[i]);
					cprintf(con, "\r\n");
				}
			}
		}
		cprint(con, "\r\n", 2);
	}

	/* Turn RF OFF (Chip Status Control Register (0x00)) */
	Trf797xTurnRfOff();

	/*
	cprintf(con, "irq_count: 0x%02ld\r\n", (uint32_t)irq_count);
	irq_count = 0;
	*/
}

static void scan_vicinity(t_hydra_console *con)
{
	static uint8_t data_buf[VICINITY_UID_MAX];
	uint8_t fifo_size;
	int i;

	/* End Test delay */
	irq_count = 0;

	/* Test ISO15693 read UID */
	Trf797xInitialSettings();
	Trf797xReset();

	/* Write Modulator and SYS_CLK Control Register (0x09) (13.56Mhz SYS_CLK and default Clock 13.56Mhz)) */
	data_buf[0] = MODULATOR_CONTROL;
	data_buf[1] = 0x31;
	Trf797xWriteSingle(data_buf, 2);

	/* Configure Mode ISO Control Register (0x01) to 0x02 (ISO15693 high bit rate, one subcarrier, 1 out of 4) */
	data_buf[0] = ISO_CONTROL;
	data_buf[1] = 0x02;
	Trf797xWriteSingle(data_buf, 2);

	/* Configure Test Settings 1 to BIT6/0x40 => MOD Pin becomes receiver subcarrier output (Digital Output for RX/TX) */
	/*
	    data_buf[0] = TEST_SETTINGS_1;
	    data_buf[1] = BIT6;
	    Trf797xWriteSingle(data_buf, 2);

	    data_buf[0] = TEST_SETTINGS_1;
	    Trf797xReadSingle(data_buf, 1);
	    if (data_buf[0] != 0x40)
	    {
	      cprintf(con, "Error Test Settings Register(0x1A) read=0x%02lX (shall be 0x40)\r\n", (uint32_t)data_buf[0]);
	      err++;
	    }
	*/

	/* Turn RF ON (Chip Status Control Register (0x00)) */
	Trf797xTurnRfOn();

	McuDelayMillisecond(10);

	/* Send Inventory(3B) and receive data + UID */
	data_buf[0] = 0x26; /* Request Flags */
	data_buf[1] = 0x01; /* Inventory Command */
	data_buf[2] = 0x00; /* Mask */

	fifo_size = Trf797x_transceive_bytes(data_buf, 3, data_buf, VICINITY_UID_MAX,
			10, /* 10ms TX/RX Timeout (shall be less than 10ms (6ms) in High Speed) */
			1); /* CRC enabled */
	if (fifo_size > 0) {
		/* fifo_size should be 10. */
		cprintf(con, "UID:");
		for (i = 0; i < fifo_size; i++)
			cprintf(con, " 0x%02lX", (uint32_t)data_buf[i]);
		cprintf(con, "\r\n");

		/* Read RSSI levels and oscillator status(0x0F/0x4F) */
		data_buf[0] = RSSI_LEVELS;
		Trf797xReadSingle(data_buf, 1);
		if (data_buf[0] < 0x40) {
			cprintf(con, "RSSI error: 0x%02lX (should be > 0x40)\r\n", (uint32_t)data_buf[0]);
		}
	}

	/* Turn RF OFF (Chip Status Control Register (0x00)) */
	Trf797xTurnRfOff();

	/*
	cprintf(con, "irq_count: 0x%02ld\r\n", (uint32_t)irq_count);
	irq_count = 0;
	*/
}

static void scan(t_hydra_console *con)
{
	mode_config_proto_t* proto = &con->mode->proto;

	if (proto->dev_mode == NFC_MODE_MIFARE)
		scan_mifare(con);
	else if (proto->dev_mode == NFC_MODE_VICINITY)
		scan_vicinity(con);
}

static int exec(t_hydra_console *con, t_tokenline_parsed *p, int token_pos)
{
	mode_config_proto_t* proto = &con->mode->proto;
	int action, period, continuous, t;

	action = 0;
	period = 1000;
	continuous = FALSE;
	for (t = token_pos; p->tokens[t]; t++) {
		switch (p->tokens[t]) {
		case T_SHOW:
			show(con, p);
			break;
		case T_MIFARE:
			proto->dev_mode = NFC_MODE_MIFARE;
			break;
		case T_VICINITY:
			proto->dev_mode = NFC_MODE_VICINITY;
			break;
		case T_PERIOD:
			t += 2;
			memcpy(&period, p->buf + p->tokens[t], sizeof(int));
			break;
		case T_CONTINUOUS:
			continuous = TRUE;
			break;
		case T_SCAN:
		case T_SNIFF:
			action = p->tokens[t];
			break;
		}
	}

	if (action == T_SCAN) {
		if (proto->dev_mode == NFC_MODE_NONE) {
			cprintf(con, "Please select MIFARE or Vicinity mode first.\r\n");
			return 0;
		}

		if (continuous) {
			cprintf(con, "Scanning %s ",
					proto->dev_mode == NFC_MODE_MIFARE ? "MIFARE" : "Vicinity");
			cprintf(con, "with 1s delay. Press user button to stop.\r\n");
			t++;
			while (!USER_BUTTON) {
				scan(con);
				chThdSleepMilliseconds(period);
			}
		} else {
			scan(con);
		}
	} else if (action == T_SNIFF) {
		cmd_nfc_sniff_14443A(con);
	}

	return t + 1;
}

static void show_registers(t_hydra_console *con)
{
	unsigned int i;
	static uint8_t data_buf;

	memset(buf, 0x20, 30);
	buf[30] = 0;
	cprintf(con, "TRF7970A Registers:\r\n");
	for (i = 0; i < ARRAY_SIZE(registers); i++) {
		data_buf = registers[i].reg;
		if (data_buf == IRQ_STATUS)
			Trf797xReadIrqStatus(&data_buf);
		else
			Trf797xReadSingle(&data_buf, 1);
		cprintf(con, "0x%02x\t%s%s: 0x%02x\r\n", registers[i].reg,
				registers[i].name, buf + strlen(registers[i].name),
				data_buf);
	}
}

static void show(t_hydra_console *con, t_tokenline_parsed *p)
{
	mode_config_proto_t* proto = &con->mode->proto;

	if (p->tokens[1] == T_REGISTERS) {
		show_registers(con);
	} else {
		cprintf(con, "Protocol: %s\r\n", proto->dev_mode == NFC_MODE_MIFARE ?
				"MIFARE (ISO14443A)" : proto->dev_mode == NFC_MODE_VICINITY ?
				"Vicinity (ISO/IEC 15693)" : "None");
	}
}

static void cleanup(t_hydra_console *con)
{
	(void)con;

	chThdTerminate(key_sniff_thread);
	/* TODO deinit GPIO config */
}

static const char *get_prompt(t_hydra_console *con)
{
	(void)con;

	return "NFC" PROMPT;
}

const mode_exec_t mode_nfc_exec = {
	.init = &init,
	.exec = &exec,
	.cleanup = &cleanup,
	.get_prompt = &get_prompt,
};


