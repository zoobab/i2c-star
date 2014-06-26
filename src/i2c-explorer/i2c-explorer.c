/*
 * This file is part of the i2c-star project.
 *
 * Copyright (C) 2014 Daniel Thompson <daniel@redfelineninja.org.uk>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libopencm3/stm32/i2c.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <librfn/console.h>
#include <librfn/fibre.h>
#include <librfn/regdump.h>
#include <librfn/time.h>
#include <librfn/util.h>

static console_t cdcacm_console;

static uint32_t i2c = I2C1;

#define D(x) { #x, x }
static const regdump_desc_t i2c_sr1_desc[] = { { "I2C_SR1", 0 },
					       D(I2C_SR1_SMBALERT),
					       D(I2C_SR1_TIMEOUT),
					       D(I2C_SR1_PECERR),
					       D(I2C_SR1_OVR),
					       D(I2C_SR1_AF),
					       D(I2C_SR1_ARLO),
					       D(I2C_SR1_BERR),
					       D(I2C_SR1_TxE),
					       D(I2C_SR1_RxNE),
					       D(I2C_SR1_STOPF),
					       D(I2C_SR1_ADD10),
					       D(I2C_SR1_BTF),
					       D(I2C_SR1_ADDR),
					       D(I2C_SR1_SB),
					       { NULL, 0 } };
#undef D

typedef struct i2c_ctx {
	pt_t pt;
	pt_t leaf;

	bool verbose;
	int err;

	uint32_t i2c;
	uint32_t timeout;

	int i;
} i2c_ctx_t;

typedef struct i2c_device_map {
	uint16_t devices[8];
} i2c_device_map_t;

static void i2c_ctx_init(i2c_ctx_t *c, uint32_t pi2c)
{
	memset(c, 0, sizeof(*c));

	c->i2c = pi2c;
	c->timeout = time_now() + 100000;
}

static bool i2c_ctx_is_timed_out(i2c_ctx_t *c)
{
	if (cyclecmp32(time_now(), c->timeout) > 0) {
		if (c->verbose) {
			printf("I2C TRANSACTION TIMED OUT\n");
			regdump(I2C_SR1(c->i2c), i2c_sr1_desc);
		}

		return true;
	}

	return false;
}

static pt_state_t i2c_ctx_reset(i2c_ctx_t *c)
{
	/* TODO: Needs to reset the specified i2c cell */
	rcc_periph_reset_pulse(RST_I2C1);
	rcc_periph_reset_pulse(RST_I2C1);

	/* peripheral configuration */
	i2c_peripheral_disable(c->i2c);
	i2c_set_clock_frequency(c->i2c, I2C_CR2_FREQ_30MHZ);

	/* Standard mode (Sm), "square" duty cycle, very slow */
	i2c_set_ccr(c->i2c, 0x0fff);
	i2c_set_trise(c->i2c, 0x3f);
	i2c_set_own_7bit_slave_address(c->i2c, 0x32);
	i2c_peripheral_enable(c->i2c);

	return PT_EXITED;
}

static pt_state_t i2c_ctx_start(i2c_ctx_t *c)
{
	PT_BEGIN(&c->leaf);

	i2c_send_start(c->i2c);

	while (!i2c_ctx_is_timed_out(c) &&
	       !((I2C_SR1(c->i2c) & I2C_SR1_SB) &
		 (I2C_SR2(c->i2c) & (I2C_SR2_MSL | I2C_SR2_BUSY))))
		PT_YIELD();

	if (!I2C_SR1(c->i2c) & I2C_SR1_SB) {
		i2c_ctx_reset(c);
		c->err = EIO;
	}

	PT_END();
}

static pt_state_t i2c_ctx_sendaddr(i2c_ctx_t *c, uint16_t addr,
				   uint8_t readwrite)
{
	PT_BEGIN(&c->leaf);

	i2c_send_7bit_address(c->i2c, addr, readwrite);

	while (!i2c_ctx_is_timed_out(c) &&
	       !(I2C_SR1(c->i2c) & I2C_SR1_ADDR))
		PT_YIELD();

	if (!(I2C_SR1(c->i2c) & I2C_SR1_ADDR)) {
		i2c_ctx_reset(c);
		c->err = EIO;
	}

	/* Read sequence has side effect or clearing I2C_SR1_ADDR */
	uint32_t reg32 __attribute__((unused));
	reg32 = I2C_SR2(c->i2c);

	PT_END();
}

static pt_state_t i2c_ctx_senddata(i2c_ctx_t *c, uint8_t data)
{
	PT_BEGIN(&c->leaf);

	i2c_send_data(c->i2c, data);

	while (!i2c_ctx_is_timed_out(c) && !(I2C_SR1(c->i2c) & I2C_SR1_BTF))
		PT_YIELD();

	if (!(I2C_SR1(c->i2c) & I2C_SR1_BTF)) {
		i2c_ctx_reset(c);
		c->err = EIO;
	}

	PT_END();
}

static pt_state_t i2c_ctx_stop(i2c_ctx_t *c)
{
	PT_BEGIN(&c->leaf);

	while (!i2c_ctx_is_timed_out(c) &&
	       !(I2C_SR1(c->i2c) & (I2C_SR1_BTF | I2C_SR1_TxE)))
		PT_YIELD();

	if (!(I2C_SR1(c->i2c) & (I2C_SR1_BTF | I2C_SR1_TxE))) {
		i2c_ctx_reset(c);
		c->err = EIO;
		PT_EXIT();
	}

	i2c_send_stop(c->i2c);

	/* TODO: is it safe to just drop out of this */

	PT_END();
}

static pt_state_t i2c_ctx_detect(i2c_ctx_t *c, i2c_device_map_t *map)
{
	PT_BEGIN(&c->pt);

	memset(map, 0, sizeof(*map));

	for (c->i = 0; c->i < 0x80; c->i++) {
		c->timeout = time_now() + 10000;
		c->err = 0;

		PT_SPAWN(&c->leaf, i2c_ctx_start(c));
		if (c->err)
			continue;

		PT_SPAWN(&c->leaf, i2c_ctx_sendaddr(c, c->i, I2C_WRITE));
		if (c->err)
			continue;

		PT_SPAWN(&c->leaf, i2c_ctx_stop(c));
		if (c->err)
			continue;

		map->devices[c->i / 16] |= 1 << (c->i % 16);
	}

	PT_END();
}


static pt_state_t do_i2cstart(console_t *c)
{
	i2c_ctx_t *ctx = (void *) &c->scratch.u8[0];

	PT_BEGIN(&c->pt);

	i2c_ctx_init(ctx, i2c);
	ctx->verbose = true;
	PT_SPAWN(&ctx->leaf, i2c_ctx_start(ctx));

	PT_END();
}

static pt_state_t do_i2csendaddr(console_t *c)
{
	i2c_ctx_t *ctx = (void *) &c->scratch.u8[0];
	uint8_t *slaveaddr = &c->scratch.u8[sizeof(*ctx)];
	uint8_t *readwrite = slaveaddr + 1;

	PT_BEGIN(&c->pt);

	*slaveaddr = strtol(c->argv[1], NULL, 0);
	*readwrite = !!strtol(c->argv[2], NULL, 0);

	i2c_ctx_init(ctx, i2c);
	ctx->verbose = true;
	PT_SPAWN(&ctx->leaf, i2c_ctx_sendaddr(ctx, *slaveaddr, *readwrite));

	PT_END();
}

static pt_state_t do_i2csendbyte(console_t *c)
{
	i2c_ctx_t *ctx = (void *) &c->scratch.u8[0];
	uint8_t *data = &c->scratch.u8[sizeof(*ctx)];

	PT_BEGIN(&c->pt);

	*data = strtol(c->argv[1], NULL, 0);

	i2c_ctx_init(ctx, i2c);
	ctx->verbose = true;
	PT_SPAWN(&ctx->leaf, i2c_ctx_senddata(ctx, *data));

	PT_END();
}



static pt_state_t do_i2cstop(console_t *c)
{
	i2c_ctx_t *ctx = (void *) &c->scratch.u8[0];

	PT_BEGIN(&c->pt);

	i2c_ctx_init(ctx, i2c);
	ctx->verbose = true;
	PT_SPAWN(&ctx->leaf, i2c_ctx_stop(ctx));

	PT_END();
}

static pt_state_t do_i2creset(console_t *c)
{
	i2c_ctx_t *ctx = (void *) &c->scratch.u8[0];

	PT_BEGIN(&c->pt);

	i2c_ctx_init(ctx, i2c);
	ctx->verbose = true;
	i2c_ctx_reset(ctx);

	PT_END();
}

static pt_state_t do_i2cdetect(console_t *c)
{
	i2c_ctx_t *ctx = (void *) &c->scratch.u8[0];
	i2c_device_map_t *map = (void *) &c->scratch.u8[sizeof(*ctx)];

	PT_BEGIN(&c->pt);

	i2c_ctx_init(ctx, i2c);
	PT_SPAWN(&ctx->pt, i2c_ctx_detect(ctx, map));

	fprintf(c->out,
		"     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f");

	for (ctx->i = 0; ctx->i < 0x80; ctx->i++) {
		if ((ctx->i & 0xf) == 0) {
			printf("\n");
			printf("%02x:", ctx->i);
		}

		if (map->devices[ctx->i / 16] & 1 << (ctx->i % 16))
			printf(" %02x", ctx->i);
		else
			printf(" --");
	}
	printf("\n");

	PT_END();
}

static pt_state_t do_uptime(console_t *c)
{
	unsigned int hours, minutes, seconds, microseconds;

	uint64_t t = time64_now();

	/* get to 32-bit values as directly as possible */
	minutes = t / (60 * 1000000);
	microseconds = t % (60 * 1000000);

	hours = minutes / 60;
	minutes %= 60;
	seconds = microseconds / 1000000;
	microseconds %= 1000000;

	fprintf(c->out, "%02u:%02u:%02u.%03u\n", hours, minutes, seconds,
		microseconds / 1000);

	return PT_EXITED;
}

static const console_cmd_t cmd_list[] = {
	CONSOLE_CMD_VAR_INIT("i2cstart", do_i2cstart),
	CONSOLE_CMD_VAR_INIT("i2csendaddr", do_i2csendaddr),
	CONSOLE_CMD_VAR_INIT("i2csendbyte", do_i2csendbyte),
	CONSOLE_CMD_VAR_INIT("i2cstop", do_i2cstop),
	CONSOLE_CMD_VAR_INIT("i2creset", do_i2creset),
	CONSOLE_CMD_VAR_INIT("i2cdetect", do_i2cdetect),
	CONSOLE_CMD_VAR_INIT("uptime", do_uptime)
};

static const console_gpio_t gpio_list[] = {
	CONSOLE_GPIO_VAR_INIT("led", GPIOD, GPIO12, 0),
	CONSOLE_GPIO_VAR_INIT("dac", GPIOD, GPIO4, console_gpio_default_on)
};

static void i2c_init(void)
{
	/* clocks */
	rcc_periph_clock_enable(RCC_GPIOB);
	rcc_periph_clock_enable(RCC_I2C1);

	rcc_periph_reset_pulse(RST_I2C1);

	/* console commands */
	i2c = I2C1;

	/* GPIO for I2C1 */
	gpio_mode_setup(GPIOB, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO6 | GPIO9);
	gpio_set_output_options(GPIOB, GPIO_OTYPE_OD, GPIO_OSPEED_100MHZ,
				GPIO6 | GPIO9);
	gpio_set_af(GPIOB, GPIO_AF4, GPIO6);
	gpio_set_af(GPIOB, GPIO_AF4, GPIO9);
}

int main(void)
{
	rcc_clock_setup_hse_3v3(&hse_8mhz_3v3[CLOCK_3V3_120MHZ]);

	i2c_init();
	time_init();

	console_init(&cdcacm_console, stdout);
	for (unsigned int i=0; i<lengthof(cmd_list); i++)
		console_register(&cmd_list[i]);
	for (unsigned int i=0; i<lengthof(gpio_list); i++)
		console_gpio_register(&gpio_list[i]);

	do_i2creset(&cdcacm_console);

	fibre_scheduler_main_loop();
}
